#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/led.h"
#include "bsp/power.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "kodi_client.h"
#include "kodi_library.h"
#include "kodi_settings.h"
#include "kodi_thumbnail.h"
#include "nvs_flash.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "pax_types.h"
#include "portmacro.h"
#include "wifi_connection.h"
#include "wifi_remote.h"

static char const TAG[] = "kodiremote";

#define BLACK    0xFF000000
#define WHITE    0xFFFFFFFF
#define GREY     0xFF808080
#define DARKGREY 0xFF303030
#define GREEN    0xFF34C759
#define RED      0xFFFF3B30
#define BLUE     0xFF3478F6
#define YELLOW   0xFFFFCC00

// ---- Globals ----

static pax_buf_t     fb                = {0};
static QueueHandle_t input_event_queue = NULL;
static size_t        physical_h_res    = 0;
static size_t        physical_v_res    = 0;

typedef enum {
    SCREEN_MENU,
    SCREEN_REMOTE,
    SCREEN_LIBRARY,
    SCREEN_TYPE,
    SCREEN_DOWNLOAD,
    SCREEN_POWER,
    SCREEN_SETTINGS,
} app_screen_t;

static void enter_screen(app_screen_t screen);  // forward decl, defined below in "Input handling per screen"

static app_screen_t    current_screen = SCREEN_MENU;
static kodi_settings_t settings       = {0};

static bool           kodi_reachable  = false;
static kodi_status_t  status          = {0};
static bool           status_valid    = false;
static int64_t        last_poll_us    = 0;
#define POLL_INTERVAL_US (2000 * 1000)

// Menu screen state
#define MENU_ITEM_COUNT 5
static char const* menu_items[MENU_ITEM_COUNT] = {
    "Remote control",
    "Library",
    "Download media",
    "Power",
    "Settings",
};
static int menu_selected = 0;

// Library screen state
typedef enum {
    LIB_VIEW_MOVIES,
    LIB_VIEW_TVSHOWS,
    LIB_VIEW_SEASONS,
    LIB_VIEW_EPISODES,
    LIB_VIEW_ARTISTS,
    LIB_VIEW_ALBUMS,
} lib_view_t;

typedef struct {
    lib_view_t view;
    int        tvshowid;
    int        season;
    int        artistid;
    char       title[96];  // breadcrumb label for this level, e.g. show/artist name
} lib_frame_t;

#define LIB_STACK_MAX 4
static char const*          lib_root_items[3] = {"Movies", "TV Shows", "Music"};
static lib_frame_t          lib_stack[LIB_STACK_MAX];
static int                  lib_depth       = 0;  // 0 = at the Movies/TV Shows/Music root
static kodi_library_item_t* lib_items       = NULL;
static int                  lib_item_count  = 0;
static int                  lib_selected    = 0;
static int                  lib_scroll      = 0;
static char                 lib_message[64] = "";

// A single big preview image + description is shown for whichever row is
// currently selected (not one thumbnail per row) - much cheaper, and matches
// the "list left, big cover + plot right" layout. Fetched on a background
// task so navigating the list never blocks on the network/JPEG decode; the
// image just pops in a moment later.
#define LIB_PREVIEW_MAX_W 140
#define LIB_PREVIEW_MAX_H 190

typedef struct {
    char     path[400];
    uint32_t request_id;
} lib_preview_job_t;

static pax_buf_t         lib_preview_thumb       = {0};  // guarded by lib_preview_mutex
static int               lib_preview_thumb_index = -1;   // which lib_items[] index is currently requested/shown
static volatile uint32_t lib_preview_request_id  = 0;    // bumped every time the wanted selection changes
static SemaphoreHandle_t lib_preview_mutex       = NULL;
static QueueHandle_t     lib_preview_job_queue   = NULL;  // depth 1, xQueueOverwrite: only the latest request matters
static TaskHandle_t      lib_preview_task        = NULL;

// Type-to-Kodi screen state
static char type_buffer[128] = "";

// Download screen state: a user-triggered bulk pass that walks the whole
// library (not just what's currently browsed) and pre-caches every
// thumbnail to the SD card, so later Library browsing never has to hit the
// network for art it has already seen. Runs on its own task with a visible
// progress bar; Esc/F2 requests cancellation, checked between items.
typedef enum {
    DL_CATEGORY_ALL,
    DL_CATEGORY_MOVIES,
    DL_CATEGORY_TVSHOWS,
    DL_CATEGORY_MUSIC,
} download_category_t;

#define DOWNLOAD_ITEM_COUNT 5
static char const* download_items[DOWNLOAD_ITEM_COUNT] = {
    "Download everything", "Download movies", "Download TV shows", "Download music", "Back",
};
static int download_selected = 0;

static TaskHandle_t      download_task           = NULL;
static SemaphoreHandle_t download_state_mutex     = NULL;
static volatile bool     download_running         = false;
static volatile bool     download_finished_once   = false;  // show the result screen once a run completes
static volatile bool     download_cancel_requested = false;
static int               download_current          = 0;  // guarded by download_state_mutex
static int               download_total            = 0;  // guarded by download_state_mutex
static char              download_label[96]        = "";  // guarded by download_state_mutex
static char              download_result[64]        = "";  // guarded by download_state_mutex

// Power screen state
#define POWER_ITEM_COUNT 6
static char const* power_items[POWER_ITEM_COUNT] = {
    "Shutdown Kodi host", "Reboot Kodi host", "Hibernate Kodi host", "Suspend Kodi host", "Quit Kodi", "Cancel",
};
static int power_selected = 0;

// Settings screen state
#define SETTINGS_FIELD_COUNT 4
typedef struct {
    char*       buf;
    size_t      maxlen;
    bool        numeric_only;
    char const* label;
} settings_field_t;

static char        port_str[6]           = "8080";
static settings_field_t settings_fields[SETTINGS_FIELD_COUNT];
static int          settings_field_index = 0;
static char const*  settings_status_line = "";

// ---- Helpers ----

static void blit(void) {
    // bsp_display_blit expects the panel's native (pre-rotation) resolution,
    // not pax_buf_get_width/height (which report the rotated logical size).
    esp_err_t res = bsp_display_blit(0, 0, physical_h_res, physical_v_res, pax_buf_get_pixels(&fb));
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to blit to display: %d", res);
    }
}

static void init_settings_fields(void) {
    snprintf(port_str, sizeof(port_str), "%u", settings.port ? settings.port : 8080);
    settings_fields[0] = (settings_field_t){settings.host, sizeof(settings.host), false, "Kodi host / IP"};
    settings_fields[1] = (settings_field_t){port_str, sizeof(port_str), true, "Port"};
    settings_fields[2] = (settings_field_t){settings.username, sizeof(settings.username), false, "Username (optional)"};
    settings_fields[3] = (settings_field_t){settings.password, sizeof(settings.password), false, "Password (optional)"};
}

static void apply_kodi_config(void) {
    kodi_client_configure(settings.host, settings.port, settings.username, settings.password);
}

// ---- Fire-and-forget Kodi commands ----
//
// Every remote-control keypress (arrows, play/pause, volume, ...) used to
// call its kodi_* function directly on the UI thread. Those are blocking
// HTTP round trips (up to KODI_HTTP_TIMEOUT_MS each) - if Kodi takes even a
// few hundred ms to answer (completely normal), the *entire app* froze for
// that long on every single keypress. None of these commands' results are
// used, so they belong on a background task instead: dispatching a command
// is just a queue push and returns immediately.
typedef enum {
    KCMD_INPUT_UP,
    KCMD_INPUT_DOWN,
    KCMD_INPUT_LEFT,
    KCMD_INPUT_RIGHT,
    KCMD_INPUT_SELECT,
    KCMD_INPUT_BACK,
    KCMD_INPUT_HOME,
    KCMD_INPUT_CONTEXT_MENU,
    KCMD_INPUT_INFO,
    KCMD_INPUT_SHOW_OSD,
    KCMD_PLAY_PAUSE,
    KCMD_STOP,
    KCMD_PLAY_NEXT,
    KCMD_PLAY_PREVIOUS,
    KCMD_SEEK_SMALL_FWD,
    KCMD_SEEK_SMALL_BACK,
    KCMD_SEEK_BIG_FWD,
    KCMD_SEEK_BIG_BACK,
    KCMD_VOLUME_UP,
    KCMD_VOLUME_DOWN,
    KCMD_TOGGLE_MUTE,
    KCMD_SYSTEM_SHUTDOWN,
    KCMD_SYSTEM_REBOOT,
    KCMD_SYSTEM_HIBERNATE,
    KCMD_SYSTEM_SUSPEND,
    KCMD_QUIT,
    KCMD_SEND_TEXT,     // str_arg, bool_arg = done
    KCMD_PLAY_MOVIE,    // int_arg = movieid
    KCMD_PLAY_EPISODE,  // int_arg = episodeid
    KCMD_PLAY_ALBUM,    // int_arg = albumid
} kodi_command_kind_t;

typedef struct {
    kodi_command_kind_t kind;
    int                  int_arg;
    bool                 bool_arg;
    char                 str_arg[400];
} kodi_command_job_t;

#define KODI_COMMAND_QUEUE_DEPTH 8
static QueueHandle_t kodi_command_queue = NULL;
static TaskHandle_t  kodi_command_task  = NULL;

static void kodi_command_worker_task(void* arg) {
    (void)arg;
    kodi_command_job_t job;
    while (1) {
        if (xQueueReceive(kodi_command_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        switch (job.kind) {
            case KCMD_INPUT_UP: kodi_input_up(); break;
            case KCMD_INPUT_DOWN: kodi_input_down(); break;
            case KCMD_INPUT_LEFT: kodi_input_left(); break;
            case KCMD_INPUT_RIGHT: kodi_input_right(); break;
            case KCMD_INPUT_SELECT: kodi_input_select(); break;
            case KCMD_INPUT_BACK: kodi_input_back(); break;
            case KCMD_INPUT_HOME: kodi_input_home(); break;
            case KCMD_INPUT_CONTEXT_MENU: kodi_input_context_menu(); break;
            case KCMD_INPUT_INFO: kodi_input_info(); break;
            case KCMD_INPUT_SHOW_OSD: kodi_input_show_osd(); break;
            case KCMD_PLAY_PAUSE: kodi_play_pause(); break;
            case KCMD_STOP: kodi_stop(); break;
            case KCMD_PLAY_NEXT: kodi_play_next(); break;
            case KCMD_PLAY_PREVIOUS: kodi_play_previous(); break;
            case KCMD_SEEK_SMALL_FWD: kodi_seek_small_forward(); break;
            case KCMD_SEEK_SMALL_BACK: kodi_seek_small_backward(); break;
            case KCMD_SEEK_BIG_FWD: kodi_seek_big_forward(); break;
            case KCMD_SEEK_BIG_BACK: kodi_seek_big_backward(); break;
            case KCMD_VOLUME_UP: kodi_volume_step(true); break;
            case KCMD_VOLUME_DOWN: kodi_volume_step(false); break;
            case KCMD_TOGGLE_MUTE: kodi_toggle_mute(); break;
            case KCMD_SYSTEM_SHUTDOWN: kodi_system_shutdown(); break;
            case KCMD_SYSTEM_REBOOT: kodi_system_reboot(); break;
            case KCMD_SYSTEM_HIBERNATE: kodi_system_hibernate(); break;
            case KCMD_SYSTEM_SUSPEND: kodi_system_suspend(); break;
            case KCMD_QUIT: kodi_quit(); break;
            case KCMD_SEND_TEXT: kodi_send_text(job.str_arg, job.bool_arg); break;
            case KCMD_PLAY_MOVIE: kodi_play_movie(job.int_arg); break;
            case KCMD_PLAY_EPISODE: kodi_play_episode(job.int_arg); break;
            case KCMD_PLAY_ALBUM: kodi_play_album(job.int_arg); break;
        }
    }
}

static void kodi_cmd(kodi_command_kind_t kind) {
    kodi_command_job_t job = {.kind = kind};
    if (kodi_command_queue != NULL) xQueueSend(kodi_command_queue, &job, 0);
}

static void kodi_cmd_int(kodi_command_kind_t kind, int arg) {
    kodi_command_job_t job = {.kind = kind, .int_arg = arg};
    if (kodi_command_queue != NULL) xQueueSend(kodi_command_queue, &job, 0);
}

static void kodi_cmd_text(char const* text, bool done) {
    kodi_command_job_t job = {.kind = KCMD_SEND_TEXT, .bool_arg = done};
    snprintf(job.str_arg, sizeof(job.str_arg), "%s", text ? text : "");
    if (kodi_command_queue != NULL) xQueueSend(kodi_command_queue, &job, 0);
}

// ---- Library browsing ----

// Runs on its own task so a slow Kodi image download/JPEG decode never
// blocks list navigation. 'request_id' lets us discard a result that's no
// longer relevant (user moved the selection again before this job finished).
static void lib_preview_worker_task(void* arg) {
    (void)arg;
    lib_preview_job_t job;
    while (1) {
        if (xQueueReceive(lib_preview_job_queue, &job, portMAX_DELAY) != pdTRUE) continue;

        pax_buf_t decoded = {0};
        if (job.path[0] != '\0') {
            kodi_thumbnail_fetch(job.path, LIB_PREVIEW_MAX_W, LIB_PREVIEW_MAX_H, &decoded);
        }

        xSemaphoreTake(lib_preview_mutex, portMAX_DELAY);
        if (job.request_id == lib_preview_request_id) {
            kodi_thumbnail_free(&lib_preview_thumb);
            lib_preview_thumb = decoded;
        } else {
            kodi_thumbnail_free(&decoded);  // selection moved on before this finished
        }
        xSemaphoreGive(lib_preview_mutex);
    }
}

static void lib_reset_preview(void) {
    lib_preview_request_id++;  // invalidate any in-flight job for the old selection/list
    xSemaphoreTake(lib_preview_mutex, portMAX_DELAY);
    kodi_thumbnail_free(&lib_preview_thumb);
    xSemaphoreGive(lib_preview_mutex);
    lib_preview_thumb_index = -1;
}

static void lib_free_items(void) {
    lib_reset_preview();

    if (lib_items) {
        kodi_library_free_items(lib_items);
        lib_items = NULL;
    }
    lib_item_count = 0;
    lib_selected   = 0;
    lib_scroll     = 0;
}

// Draws a small "Loading ..." placeholder immediately, since the fetch below
// blocks on a network round trip and would otherwise leave the screen frozen
// on the previous view.
static void lib_show_loading(char const* what) {
    pax_background(&fb, BLACK);
    char msg[64];
    snprintf(msg, sizeof(msg), "Loading %s...", what);
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 12, 50, msg);
    blit();
}

static void lib_load_current(void) {
    lib_free_items();
    if (lib_depth == 0) return;  // root view uses the static lib_root_items list

    lib_frame_t* f   = &lib_stack[lib_depth - 1];
    esp_err_t    err = ESP_FAIL;

    switch (f->view) {
        case LIB_VIEW_MOVIES:
            lib_show_loading("movies");
            err = kodi_library_get_movies(&lib_items, &lib_item_count);
            break;
        case LIB_VIEW_TVSHOWS:
            lib_show_loading("TV shows");
            err = kodi_library_get_tvshows(&lib_items, &lib_item_count);
            break;
        case LIB_VIEW_SEASONS:
            lib_show_loading("seasons");
            err = kodi_library_get_seasons(f->tvshowid, &lib_items, &lib_item_count);
            break;
        case LIB_VIEW_EPISODES:
            lib_show_loading("episodes");
            err = kodi_library_get_episodes(f->tvshowid, f->season, &lib_items, &lib_item_count);
            break;
        case LIB_VIEW_ARTISTS:
            lib_show_loading("artists");
            err = kodi_library_get_artists(&lib_items, &lib_item_count);
            break;
        case LIB_VIEW_ALBUMS:
            lib_show_loading("albums");
            err = kodi_library_get_albums(f->artistid, &lib_items, &lib_item_count);
            break;
    }

    if (err != ESP_OK) {
        snprintf(lib_message, sizeof(lib_message), "Failed to load from Kodi");
    } else if (lib_item_count == 0) {
        snprintf(lib_message, sizeof(lib_message), "Nothing found");
    } else {
        lib_message[0] = '\0';
    }
}

// Dispatches a background fetch for the currently selected row's preview
// image if the selection has changed since the last dispatch. Non-blocking:
// safe to call on every render.
static void lib_ensure_preview(void) {
    if (lib_depth == 0 || lib_item_count == 0) return;
    if (lib_preview_thumb_index == lib_selected) return;  // already requested/showing this selection

    lib_preview_thumb_index = lib_selected;
    lib_preview_request_id++;

    lib_preview_job_t job = {0};
    job.request_id        = lib_preview_request_id;
    if (lib_selected >= 0 && lib_selected < lib_item_count) {
        snprintf(job.path, sizeof(job.path), "%s", lib_items[lib_selected].thumb_path);
    }
    if (lib_preview_job_queue != NULL) {
        xQueueOverwrite(lib_preview_job_queue, &job);
    }
}

static void lib_enter_root(void) {
    lib_free_items();
    lib_depth       = 0;
    lib_selected    = 0;
    lib_message[0]  = '\0';
}

static void lib_push(lib_view_t view, int tvshowid, int season, int artistid, char const* title) {
    if (lib_depth >= LIB_STACK_MAX) return;
    lib_frame_t* f = &lib_stack[lib_depth++];
    f->view        = view;
    f->tvshowid    = tvshowid;
    f->season      = season;
    f->artistid    = artistid;
    snprintf(f->title, sizeof(f->title), "%s", title ? title : "");
    lib_selected = 0;
    lib_load_current();
}

static void lib_pop(void) {
    if (lib_depth == 0) {
        enter_screen(SCREEN_MENU);
        return;
    }
    lib_depth--;
    lib_selected = 0;
    lib_load_current();
}

// ---- Download / bulk pre-cache ----

static void download_set_progress(int current, int total, char const* label) {
    xSemaphoreTake(download_state_mutex, portMAX_DELAY);
    download_current = current;
    download_total   = total;
    snprintf(download_label, sizeof(download_label), "%s", label ? label : "");
    xSemaphoreGive(download_state_mutex);
}

static void download_set_result(char const* text) {
    xSemaphoreTake(download_state_mutex, portMAX_DELAY);
    snprintf(download_result, sizeof(download_result), "%s", text);
    xSemaphoreGive(download_state_mutex);
}

// Fetches one whole category and pre-caches every item's thumbnail to the SD
// card, reporting progress after each one. Stops early, without error, if
// download_cancel_requested becomes true.
static void download_run_category(esp_err_t (*fetch_all)(kodi_library_item_t**, int*), char const* what) {
    kodi_library_item_t* items = NULL;
    int                   count = 0;
    if (fetch_all(&items, &count) != ESP_OK) return;

    for (int i = 0; i < count && !download_cancel_requested; i++) {
        char label[96];
        snprintf(label, sizeof(label), "%s: %s", what, items[i].label);
        download_set_progress(i + 1, count, label);
        if (items[i].thumb_path[0] != '\0') {
            kodi_thumbnail_precache(items[i].thumb_path);
        }
    }

    kodi_library_free_items(items);
}

static void download_task_fn(void* arg) {
    download_category_t category = (download_category_t)(intptr_t)arg;

    if (category == DL_CATEGORY_ALL || category == DL_CATEGORY_MOVIES) {
        download_run_category(kodi_library_get_all_movies, "Movie");
    }
    if (!download_cancel_requested && (category == DL_CATEGORY_ALL || category == DL_CATEGORY_TVSHOWS)) {
        download_run_category(kodi_library_get_all_tvshows, "TV Show");
        if (!download_cancel_requested) download_run_category(kodi_library_get_all_seasons, "Season");
        if (!download_cancel_requested) download_run_category(kodi_library_get_all_episodes, "Episode");
    }
    if (!download_cancel_requested && (category == DL_CATEGORY_ALL || category == DL_CATEGORY_MUSIC)) {
        download_run_category(kodi_library_get_all_artists, "Artist");
        if (!download_cancel_requested) download_run_category(kodi_library_get_all_albums, "Album");
    }

    download_set_result(download_cancel_requested ? "Cancelled" : "Done");
    download_running       = false;
    download_finished_once = true;
    vTaskDelete(NULL);
}

static void download_start(download_category_t category) {
    if (download_running) return;
    download_cancel_requested = false;
    download_running          = true;
    download_finished_once    = false;
    download_set_progress(0, 0, "Starting...");
    download_set_result("");
    xTaskCreate(download_task_fn, "kodi_dl", 8192, (void*)(intptr_t)category, tskIDLE_PRIORITY + 1, &download_task);
}

// ---- Drawing ----

static void draw_header(char const* title) {
    float w = pax_buf_get_width(&fb);
    pax_simple_rect(&fb, DARKGREY, 0, 0, w, 36);
    pax_draw_text(&fb, WHITE, pax_font_saira_regular, 22, 10, 6, title);

    pax_col_t dot_color = kodi_reachable ? GREEN : RED;
    pax_simple_rect(&fb, dot_color, w - 24, 12, 12, 12);
}

static void draw_footer_hints(char const* hints) {
    float w = pax_buf_get_width(&fb);
    float h = pax_buf_get_height(&fb);
    pax_simple_rect(&fb, DARKGREY, 0, h - 28, w, 28);
    pax_draw_text(&fb, GREY, pax_font_sky_mono, 14, 8, h - 22, hints);
}

static void draw_menu_screen(void) {
    pax_background(&fb, BLACK);
    draw_header("Kodi Remote");

    float w      = pax_buf_get_width(&fb);
    float y      = 50;
    float row_h  = 44;

    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        bool selected = (i == menu_selected);
        if (selected) {
            pax_simple_rect(&fb, BLUE, 8, y, w - 16, row_h - 6);
        }
        pax_draw_text(&fb, WHITE, pax_font_saira_regular, 20, 24, y + 10, menu_items[i]);
        y += row_h;
    }

    if (status_valid && status.playing) {
        char line[160];
        snprintf(line, sizeof(line), "Now playing: %s", status.title[0] ? status.title : "(unknown)");
        pax_draw_text(&fb, YELLOW, pax_font_sky_mono, 14, 12, y + 8, line);
    } else if (!kodi_reachable && kodi_client_is_configured()) {
        pax_draw_text(&fb, RED, pax_font_sky_mono, 14, 12, y + 8, "Kodi host unreachable - check Settings");
    } else if (!kodi_client_is_configured()) {
        pax_draw_text(&fb, YELLOW, pax_font_sky_mono, 14, 12, y + 8, "Not configured yet - open Settings");
    }

    draw_footer_hints("Up/Down select  Enter open  F1 exit app");
    blit();
}

static void format_time(char* out, size_t out_size, int h, int m, int s) {
    if (h > 0) {
        snprintf(out, out_size, "%d:%02d:%02d", h, m, s);
    } else {
        snprintf(out, out_size, "%d:%02d", m, s);
    }
}

static void draw_remote_screen(void) {
    pax_background(&fb, BLACK);
    draw_header("Remote control");

    float w  = pax_buf_get_width(&fb);
    float h  = pax_buf_get_height(&fb);
    float cx = w / 2;
    float cy = 36 + (h - 36 - 28) / 2 - 40;

    // D-pad hint graphic
    float pad = 70;
    pax_outline_rect(&fb, GREY, cx - pad, cy - pad, pad * 2, pad * 2);
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, cx - 4, cy - pad - 20, "^");
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, cx - 4, cy + pad + 4, "v");
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, cx - pad - 16, cy - 8, "<");
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, cx + pad + 4, cy - 8, ">");
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, cx - 12, cy - 8, "OK");

    float info_y = cy + pad + 40;
    if (!kodi_client_is_configured()) {
        pax_draw_text(&fb, RED, pax_font_sky_mono, 16, 12, info_y, "No Kodi host configured (see Settings)");
    } else if (!kodi_reachable) {
        pax_draw_text(&fb, RED, pax_font_sky_mono, 16, 12, info_y, "Kodi host unreachable");
    } else if (status_valid && status.playing) {
        char title_line[160];
        snprintf(title_line, sizeof(title_line), "%s", status.title[0] ? status.title : "(unknown title)");
        pax_draw_text(&fb, WHITE, pax_font_saira_regular, 18, 12, info_y, title_line);

        if (status.subtitle[0]) {
            pax_draw_text(&fb, GREY, pax_font_sky_mono, 14, 12, info_y + 24, status.subtitle);
        }

        // Progress bar
        float bar_x = 12, bar_y = info_y + 48, bar_w = w - 24, bar_h = 10;
        pax_outline_rect(&fb, GREY, bar_x, bar_y, bar_w, bar_h);
        float fill = bar_w * (float)(status.percentage / 100.0);
        if (fill > bar_w) fill = bar_w;
        if (fill < 0) fill = 0;
        pax_simple_rect(&fb, GREEN, bar_x, bar_y, fill, bar_h);

        char cur[16], tot[16], time_line[48];
        format_time(cur, sizeof(cur), status.time_hours, status.time_minutes, status.time_seconds);
        format_time(tot, sizeof(tot), status.total_hours, status.total_minutes, status.total_seconds);
        snprintf(time_line, sizeof(time_line), "%s / %s%s", cur, tot, status.paused ? "  (paused)" : "");
        pax_draw_text(&fb, GREY, pax_font_sky_mono, 14, 12, bar_y + 16, time_line);

        char vol_line[48];
        snprintf(vol_line, sizeof(vol_line), "Volume: %d%%%s", status.volume, status.muted ? " (muted)" : "");
        pax_draw_text(&fb, GREY, pax_font_sky_mono, 14, 12, bar_y + 36, vol_line);
    } else {
        char vol_line[48];
        snprintf(vol_line, sizeof(vol_line), "Nothing playing. Volume: %d%%%s", status.volume,
                 status.muted ? " (muted)" : "");
        pax_draw_text(&fb, GREY, pax_font_sky_mono, 16, 12, info_y, vol_line);
    }

    draw_footer_hints("Arrows nav  Space play  M mute  T type text  F2 menu");
    blit();
}

// Draws left-aligned, word-wrapped text within max_width, up to max_lines lines.
static void draw_wrapped_text(pax_col_t color, pax_font_t const* font, float font_size, float x, float y,
                               float max_width, float line_height, char const* text, int max_lines) {
    if (text == NULL || text[0] == '\0' || max_lines <= 0) return;

    char        line[192] = "";
    int         lines_drawn = 0;
    float       cur_y       = y;
    char const* word        = text;

    while (*word != '\0' && lines_drawn < max_lines) {
        while (*word == ' ') word++;
        char const* word_end = word;
        while (*word_end != '\0' && *word_end != ' ') word_end++;
        int word_len = (int)(word_end - word);
        if (word_len == 0) break;
        if (word_len > 96) word_len = 96;  // clamp so snprintf's static bounds check is satisfiable

        char candidate[192];
        if (line[0] == '\0') {
            snprintf(candidate, sizeof(candidate), "%.*s", word_len, word);
        } else {
            snprintf(candidate, sizeof(candidate), "%s %.*s", line, word_len, word);
        }

        pax_vec2f size = pax_text_size(font, font_size, candidate);
        if (size.x > max_width && line[0] != '\0') {
            pax_draw_text(&fb, color, font, font_size, x, cur_y, line);
            cur_y += line_height;
            lines_drawn++;
            snprintf(line, sizeof(line), "%.*s", word_len, word);
        } else {
            snprintf(line, sizeof(line), "%s", candidate);
        }

        word = word_end;
    }

    if (line[0] != '\0' && lines_drawn < max_lines) {
        pax_draw_text(&fb, color, font, font_size, x, cur_y, line);
    }
}

static void draw_library_screen(void) {
    pax_background(&fb, BLACK);

    char title[128];
    if (lib_depth == 0) {
        snprintf(title, sizeof(title), "Library");
    } else {
        snprintf(title, sizeof(title), "Library: %s", lib_stack[lib_depth - 1].title);
    }
    draw_header(title);

    float w         = pax_buf_get_width(&fb);
    float h         = pax_buf_get_height(&fb);
    float top       = 44;
    bool  has_media = lib_depth != 0;

    int count = (lib_depth == 0) ? 3 : lib_item_count;

    if (count == 0 && lib_message[0]) {
        pax_draw_text(&fb, YELLOW, pax_font_sky_mono, 16, 12, top + 8, lib_message);
        draw_footer_hints("Esc back  F2 menu");
        blit();
        return;
    }

    float list_w        = has_media ? w * 0.5f : w;
    float row_h          = 32;
    int   visible_rows = (int)((h - 28 - top) / row_h);
    if (visible_rows < 1) visible_rows = 1;

    if (lib_selected < lib_scroll) lib_scroll = lib_selected;
    if (lib_selected >= lib_scroll + visible_rows) lib_scroll = lib_selected - visible_rows + 1;
    if (lib_scroll < 0) lib_scroll = 0;

    // Clip so a long title/label can never bleed across the divider into the
    // preview pane - it gets cropped at the column edge instead.
    pax_clip(&fb, 0, (int)top, (int)list_w, (int)(h - 28 - top));
    for (int row = 0; row < visible_rows; row++) {
        int idx = lib_scroll + row;
        if (idx >= count) break;
        char const* label = (lib_depth == 0) ? lib_root_items[idx] : lib_items[idx].label;
        float       y     = top + row * row_h;
        if (idx == lib_selected) {
            pax_simple_rect(&fb, BLUE, 4, y, list_w - 8, row_h - 2);
        }
        pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 14, y + 7, label);
    }
    if (count > visible_rows) {
        char scroll_info[24];
        snprintf(scroll_info, sizeof(scroll_info), "%d/%d", lib_selected + 1, count);
        pax_draw_text(&fb, GREY, pax_font_sky_mono, 12, list_w - 56, 30, scroll_info);
    }
    pax_noclip(&fb);

    if (has_media) {
        lib_ensure_preview();

        float right_x = list_w + 8;
        float right_w = w - right_x - 8;
        if (right_w < 40) right_w = 40;

        pax_simple_line(&fb, DARKGREY, list_w, top, list_w, h - 28);

        // Clip the whole preview pane too: the decoded image is fit within a
        // fixed LIB_PREVIEW_MAX_W/H box which may be wider than the actual
        // pane on a narrow/portrait screen.
        pax_clip(&fb, (int)right_x, (int)top, (int)(w - right_x), (int)(h - 28 - top));

        xSemaphoreTake(lib_preview_mutex, portMAX_DELAY);
        bool  has_thumb = lib_preview_thumb.buf != NULL;
        float thumb_h   = has_thumb ? pax_buf_get_height(&lib_preview_thumb) : LIB_PREVIEW_MAX_H;
        if (has_thumb) {
            pax_draw_image(&fb, &lib_preview_thumb, right_x, top + 6);
        }
        xSemaphoreGive(lib_preview_mutex);

        if (!has_thumb) {
            float ph_w = right_w < LIB_PREVIEW_MAX_W ? right_w : LIB_PREVIEW_MAX_W;
            pax_outline_rect(&fb, DARKGREY, right_x, top + 6, ph_w, LIB_PREVIEW_MAX_H);
        }

        char const* description =
            (lib_selected >= 0 && lib_selected < lib_item_count) ? lib_items[lib_selected].description : "";
        float desc_y            = top + 6 + thumb_h + 10;
        float desc_line_height  = 20;  // > font size 13 with room to spare, so descenders/ascenders never touch
        if (description[0] != '\0' && desc_y < h - 28) {
            int max_lines = (int)((h - 28 - desc_y) / desc_line_height);
            draw_wrapped_text(GREY, pax_font_sky_mono, 13, right_x, desc_y, right_w, desc_line_height, description,
                               max_lines);
        }

        pax_noclip(&fb);
    }

    draw_footer_hints("Up/Down select  Enter open/play  Esc back  F2 menu");
    blit();
}

static void draw_type_screen(void) {
    pax_background(&fb, BLACK);
    draw_header("Type to Kodi");

    float w = pax_buf_get_width(&fb);

    pax_draw_text(&fb, GREY, pax_font_sky_mono, 14, 16, 50,
                  "Types into whatever text field Kodi has focused (e.g. search)");

    pax_outline_rect(&fb, BLUE, 16, 80, w - 32, 36);
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 18, 26, 90, type_buffer);

    draw_footer_hints("Type text  Enter send  F2 cancel");
    blit();
}

static void draw_download_screen(void) {
    pax_background(&fb, BLACK);
    draw_header("Download Media");

    float w = pax_buf_get_width(&fb);

    if (download_running || download_finished_once) {
        int  current, total;
        char label[96];
        char result[64];
        xSemaphoreTake(download_state_mutex, portMAX_DELAY);
        current = download_current;
        total   = download_total;
        snprintf(label, sizeof(label), "%s", download_label);
        snprintf(result, sizeof(result), "%s", download_result);
        xSemaphoreGive(download_state_mutex);

        if (download_running) {
            pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 12, 60, label);

            float bar_x = 12, bar_y = 90, bar_w = w - 24, bar_h = 16;
            pax_outline_rect(&fb, GREY, bar_x, bar_y, bar_w, bar_h);
            if (total > 0) {
                float fill = bar_w * ((float)current / (float)total);
                if (fill > bar_w) fill = bar_w;
                if (fill < 0) fill = 0;
                pax_simple_rect(&fb, GREEN, bar_x, bar_y, fill, bar_h);
            }

            char count_line[32];
            snprintf(count_line, sizeof(count_line), "%d / %d", current, total);
            pax_draw_text(&fb, GREY, pax_font_sky_mono, 14, 12, bar_y + bar_h + 8, count_line);

            draw_footer_hints("Esc / F2 cancel");
        } else {
            char done_line[96];
            snprintf(done_line, sizeof(done_line), "%s (%d items)", result, current);
            pax_draw_text(&fb, WHITE, pax_font_sky_mono, 18, 12, 60, done_line);
            draw_footer_hints("Enter/Esc continue  F2 menu");
        }
    } else {
        float y     = 50;
        float row_h = 44;
        for (int i = 0; i < DOWNLOAD_ITEM_COUNT; i++) {
            bool selected = (i == download_selected);
            if (selected) {
                pax_simple_rect(&fb, BLUE, 8, y, w - 16, row_h - 6);
            }
            pax_draw_text(&fb, WHITE, pax_font_saira_regular, 20, 24, y + 10, download_items[i]);
            y += row_h;
        }
        pax_draw_text(&fb, GREY, pax_font_sky_mono, 13, 12, y + 8,
                      "Pre-caches cover art to the SD card so Library browsing");
        pax_draw_text(&fb, GREY, pax_font_sky_mono, 13, 12, y + 24, "loads instantly later. Can take a while.");
        draw_footer_hints("Up/Down select  Enter start  F2 menu");
    }

    blit();
}

static void draw_power_screen(void) {
    pax_background(&fb, BLACK);
    draw_header("Power");

    float w     = pax_buf_get_width(&fb);
    float y     = 50;
    float row_h = 44;

    for (int i = 0; i < POWER_ITEM_COUNT; i++) {
        bool selected = (i == power_selected);
        if (selected) {
            pax_simple_rect(&fb, BLUE, 8, y, w - 16, row_h - 6);
        }
        pax_draw_text(&fb, WHITE, pax_font_saira_regular, 20, 24, y + 10, power_items[i]);
        y += row_h;
    }

    draw_footer_hints("Up/Down select  Enter confirm  F2 back");
    blit();
}

static void draw_settings_screen(void) {
    pax_background(&fb, BLACK);
    draw_header("Settings");

    float w = pax_buf_get_width(&fb);
    float y = 56;

    for (int i = 0; i < SETTINGS_FIELD_COUNT; i++) {
        bool active = (i == settings_field_index);
        pax_col_t label_color = active ? YELLOW : GREY;
        pax_draw_text(&fb, label_color, pax_font_sky_mono, 14, 16, y, settings_fields[i].label);

        pax_col_t box_color = active ? BLUE : DARKGREY;
        pax_outline_rect(&fb, box_color, 16, y + 18, w - 32, 32);

        char masked[KODI_SETTINGS_PASS_MAX];
        char const* value = settings_fields[i].buf;
        if (settings_fields[i].buf == settings.password && settings.password[0]) {
            size_t len = strlen(settings.password);
            if (len >= sizeof(masked)) len = sizeof(masked) - 1;
            memset(masked, '*', len);
            masked[len] = '\0';
            value       = masked;
        }
        pax_draw_text(&fb, WHITE, pax_font_sky_mono, 18, 26, y + 26, value);

        y += 60;
    }

    if (settings_status_line[0]) {
        pax_draw_text(&fb, YELLOW, pax_font_sky_mono, 14, 16, y + 8, settings_status_line);
    }

    draw_footer_hints("Type to edit  Up/Down field  Enter next/save  F2 cancel");
    blit();
}

static void render(void) {
    switch (current_screen) {
        case SCREEN_MENU: draw_menu_screen(); break;
        case SCREEN_REMOTE: draw_remote_screen(); break;
        case SCREEN_LIBRARY: draw_library_screen(); break;
        case SCREEN_TYPE: draw_type_screen(); break;
        case SCREEN_DOWNLOAD: draw_download_screen(); break;
        case SCREEN_POWER: draw_power_screen(); break;
        case SCREEN_SETTINGS: draw_settings_screen(); break;
    }
}

// ---- Status polling ----

static void poll_kodi_status(void) {
    if (!kodi_client_is_configured()) {
        kodi_reachable = false;
        status_valid   = false;
        return;
    }

    kodi_status_t new_status;
    esp_err_t     err = kodi_get_status(&new_status);
    if (err == ESP_OK) {
        status         = new_status;
        status_valid   = true;
        kodi_reachable = true;
    } else {
        kodi_reachable = false;
    }
}

// ---- Input handling per screen ----

static void enter_screen(app_screen_t screen) {
    current_screen = screen;
    if (screen == SCREEN_SETTINGS) {
        init_settings_fields();
        settings_field_index = 0;
        settings_status_line = "";
    }
    if (screen == SCREEN_REMOTE) {
        poll_kodi_status();
        last_poll_us = esp_timer_get_time();
    }
    if (screen == SCREEN_LIBRARY) {
        lib_enter_root();
    }
    if (screen == SCREEN_TYPE) {
        type_buffer[0] = '\0';
    }
    if (screen == SCREEN_DOWNLOAD && !download_running) {
        download_selected = 0;
    }
}

static void handle_menu_navigation(bsp_input_event_args_navigation_t const* nav) {
    if (!nav->state) return;  // only act on key press
    switch (nav->key) {
        case BSP_INPUT_NAVIGATION_KEY_UP:
            menu_selected = (menu_selected + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
            break;
        case BSP_INPUT_NAVIGATION_KEY_DOWN: menu_selected = (menu_selected + 1) % MENU_ITEM_COUNT; break;
        case BSP_INPUT_NAVIGATION_KEY_SELECT:
        case BSP_INPUT_NAVIGATION_KEY_RETURN:
            switch (menu_selected) {
                case 0: enter_screen(SCREEN_REMOTE); break;
                case 1: enter_screen(SCREEN_LIBRARY); break;
                case 2: enter_screen(SCREEN_DOWNLOAD); break;
                case 3: enter_screen(SCREEN_POWER); break;
                case 4: enter_screen(SCREEN_SETTINGS); break;
            }
            break;
        case BSP_INPUT_NAVIGATION_KEY_F1: bsp_device_restart_to_launcher(); break;
        default: break;
    }
}

static void handle_remote_navigation(bsp_input_event_args_navigation_t const* nav) {
    if (!nav->state) return;
    switch (nav->key) {
        case BSP_INPUT_NAVIGATION_KEY_UP: kodi_cmd(KCMD_INPUT_UP); break;
        case BSP_INPUT_NAVIGATION_KEY_DOWN: kodi_cmd(KCMD_INPUT_DOWN); break;
        case BSP_INPUT_NAVIGATION_KEY_LEFT: kodi_cmd(KCMD_INPUT_LEFT); break;
        case BSP_INPUT_NAVIGATION_KEY_RIGHT: kodi_cmd(KCMD_INPUT_RIGHT); break;
        case BSP_INPUT_NAVIGATION_KEY_SELECT:
        case BSP_INPUT_NAVIGATION_KEY_RETURN: kodi_cmd(KCMD_INPUT_SELECT); break;
        case BSP_INPUT_NAVIGATION_KEY_ESC:
        case BSP_INPUT_NAVIGATION_KEY_BACKSPACE: kodi_cmd(KCMD_INPUT_BACK); break;
        case BSP_INPUT_NAVIGATION_KEY_HOME: kodi_cmd(KCMD_INPUT_HOME); break;
        case BSP_INPUT_NAVIGATION_KEY_MENU: kodi_cmd(KCMD_INPUT_CONTEXT_MENU); break;
        case BSP_INPUT_NAVIGATION_KEY_F1: bsp_device_restart_to_launcher(); break;
        case BSP_INPUT_NAVIGATION_KEY_F2: enter_screen(SCREEN_MENU); break;
        case BSP_INPUT_NAVIGATION_KEY_F3: poll_kodi_status(); break;
        default: break;
    }
}

static void handle_remote_keyboard(bsp_input_event_args_keyboard_t const* kb) {
    switch (kb->ascii) {
        case ' ': kodi_cmd(KCMD_PLAY_PAUSE); break;
        case 's':
        case 'S': kodi_cmd(KCMD_STOP); break;
        case 'm':
        case 'M': kodi_cmd(KCMD_TOGGLE_MUTE); break;
        case 'n':
        case 'N': kodi_cmd(KCMD_PLAY_NEXT); break;
        case 'p':
        case 'P': kodi_cmd(KCMD_PLAY_PREVIOUS); break;
        case ',': kodi_cmd(KCMD_SEEK_SMALL_BACK); break;
        case '.': kodi_cmd(KCMD_SEEK_SMALL_FWD); break;
        case '[': kodi_cmd(KCMD_SEEK_BIG_BACK); break;
        case ']': kodi_cmd(KCMD_SEEK_BIG_FWD); break;
        case 'i':
        case 'I': kodi_cmd(KCMD_INPUT_INFO); break;
        case 'c':
        case 'C': kodi_cmd(KCMD_INPUT_CONTEXT_MENU); break;
        case 'o':
        case 'O': kodi_cmd(KCMD_INPUT_SHOW_OSD); break;
        case 't':
        case 'T': enter_screen(SCREEN_TYPE); break;
        default: break;
    }
}

static void handle_library_navigation(bsp_input_event_args_navigation_t const* nav) {
    if (!nav->state) return;

    int count = (lib_depth == 0) ? 3 : lib_item_count;

    switch (nav->key) {
        case BSP_INPUT_NAVIGATION_KEY_UP:
            if (count > 0) lib_selected = (lib_selected + count - 1) % count;
            break;
        case BSP_INPUT_NAVIGATION_KEY_DOWN:
            if (count > 0) lib_selected = (lib_selected + 1) % count;
            break;
        case BSP_INPUT_NAVIGATION_KEY_SELECT:
        case BSP_INPUT_NAVIGATION_KEY_RETURN: {
            if (lib_depth == 0) {
                switch (lib_selected) {
                    case 0: lib_push(LIB_VIEW_MOVIES, 0, 0, 0, "Movies"); break;
                    case 1: lib_push(LIB_VIEW_TVSHOWS, 0, 0, 0, "TV Shows"); break;
                    case 2: lib_push(LIB_VIEW_ARTISTS, 0, 0, 0, "Music"); break;
                }
                break;
            }
            if (lib_item_count == 0) break;

            kodi_library_item_t* item = &lib_items[lib_selected];
            switch (lib_stack[lib_depth - 1].view) {
                case LIB_VIEW_MOVIES:
                    kodi_cmd_int(KCMD_PLAY_MOVIE, item->id);
                    enter_screen(SCREEN_REMOTE);
                    break;
                case LIB_VIEW_TVSHOWS: lib_push(LIB_VIEW_SEASONS, item->id, 0, 0, item->label); break;
                case LIB_VIEW_SEASONS: {
                    lib_frame_t* season_frame = &lib_stack[lib_depth - 1];
                    char         episode_title[160];
                    if (item->id == 0) {
                        snprintf(episode_title, sizeof(episode_title), "%s - Specials", season_frame->title);
                    } else {
                        snprintf(episode_title, sizeof(episode_title), "%s - Season %d", season_frame->title,
                                 item->id);
                    }
                    lib_push(LIB_VIEW_EPISODES, season_frame->tvshowid, item->id, 0, episode_title);
                    break;
                }
                case LIB_VIEW_EPISODES:
                    kodi_cmd_int(KCMD_PLAY_EPISODE, item->id);
                    enter_screen(SCREEN_REMOTE);
                    break;
                case LIB_VIEW_ARTISTS: lib_push(LIB_VIEW_ALBUMS, 0, 0, item->id, item->label); break;
                case LIB_VIEW_ALBUMS:
                    kodi_cmd_int(KCMD_PLAY_ALBUM, item->id);
                    enter_screen(SCREEN_REMOTE);
                    break;
            }
            break;
        }
        case BSP_INPUT_NAVIGATION_KEY_ESC:
        case BSP_INPUT_NAVIGATION_KEY_BACKSPACE: lib_pop(); break;
        case BSP_INPUT_NAVIGATION_KEY_F1: bsp_device_restart_to_launcher(); break;
        case BSP_INPUT_NAVIGATION_KEY_F2: enter_screen(SCREEN_MENU); break;
        default: break;
    }
}

static void handle_type_navigation(bsp_input_event_args_navigation_t const* nav) {
    if (!nav->state) return;
    switch (nav->key) {
        case BSP_INPUT_NAVIGATION_KEY_BACKSPACE: {
            size_t len = strlen(type_buffer);
            if (len > 0) type_buffer[len - 1] = '\0';
            break;
        }
        case BSP_INPUT_NAVIGATION_KEY_RETURN:
        case BSP_INPUT_NAVIGATION_KEY_SELECT:
            kodi_cmd_text(type_buffer, true);
            enter_screen(SCREEN_REMOTE);
            break;
        case BSP_INPUT_NAVIGATION_KEY_F1: bsp_device_restart_to_launcher(); break;
        case BSP_INPUT_NAVIGATION_KEY_F2:
        case BSP_INPUT_NAVIGATION_KEY_ESC: enter_screen(SCREEN_REMOTE); break;
        default: break;
    }
}

static void handle_type_keyboard(bsp_input_event_args_keyboard_t const* kb) {
    char c = kb->ascii;
    if (c == '\b') {
        size_t len = strlen(type_buffer);
        if (len > 0) type_buffer[len - 1] = '\0';
        return;
    }
    if (c == '\r' || c == '\n') {
        kodi_cmd_text(type_buffer, true);
        enter_screen(SCREEN_REMOTE);
        return;
    }
    if (c < 32 || c > 126) return;  // ignore non-printable

    size_t len = strlen(type_buffer);
    if (len + 1 < sizeof(type_buffer)) {
        type_buffer[len]     = c;
        type_buffer[len + 1] = '\0';
    }
}

static void handle_download_navigation(bsp_input_event_args_navigation_t const* nav) {
    if (!nav->state) return;

    if (download_running) {
        switch (nav->key) {
            case BSP_INPUT_NAVIGATION_KEY_ESC:
            case BSP_INPUT_NAVIGATION_KEY_F2: download_cancel_requested = true; break;
            case BSP_INPUT_NAVIGATION_KEY_F1: bsp_device_restart_to_launcher(); break;
            default: break;
        }
        return;
    }

    if (download_finished_once) {
        download_finished_once = false;
        if (nav->key == BSP_INPUT_NAVIGATION_KEY_F1) bsp_device_restart_to_launcher();
        if (nav->key == BSP_INPUT_NAVIGATION_KEY_F2) enter_screen(SCREEN_MENU);
        return;
    }

    switch (nav->key) {
        case BSP_INPUT_NAVIGATION_KEY_UP:
            download_selected = (download_selected + DOWNLOAD_ITEM_COUNT - 1) % DOWNLOAD_ITEM_COUNT;
            break;
        case BSP_INPUT_NAVIGATION_KEY_DOWN:
            download_selected = (download_selected + 1) % DOWNLOAD_ITEM_COUNT;
            break;
        case BSP_INPUT_NAVIGATION_KEY_SELECT:
        case BSP_INPUT_NAVIGATION_KEY_RETURN:
            switch (download_selected) {
                case 0: download_start(DL_CATEGORY_ALL); break;
                case 1: download_start(DL_CATEGORY_MOVIES); break;
                case 2: download_start(DL_CATEGORY_TVSHOWS); break;
                case 3: download_start(DL_CATEGORY_MUSIC); break;
                case 4: enter_screen(SCREEN_MENU); break;
            }
            break;
        case BSP_INPUT_NAVIGATION_KEY_F1: bsp_device_restart_to_launcher(); break;
        case BSP_INPUT_NAVIGATION_KEY_F2: enter_screen(SCREEN_MENU); break;
        default: break;
    }
}

static void handle_power_navigation(bsp_input_event_args_navigation_t const* nav) {
    if (!nav->state) return;
    switch (nav->key) {
        case BSP_INPUT_NAVIGATION_KEY_UP:
            power_selected = (power_selected + POWER_ITEM_COUNT - 1) % POWER_ITEM_COUNT;
            break;
        case BSP_INPUT_NAVIGATION_KEY_DOWN: power_selected = (power_selected + 1) % POWER_ITEM_COUNT; break;
        case BSP_INPUT_NAVIGATION_KEY_SELECT:
        case BSP_INPUT_NAVIGATION_KEY_RETURN:
            switch (power_selected) {
                case 0: kodi_cmd(KCMD_SYSTEM_SHUTDOWN); break;
                case 1: kodi_cmd(KCMD_SYSTEM_REBOOT); break;
                case 2: kodi_cmd(KCMD_SYSTEM_HIBERNATE); break;
                case 3: kodi_cmd(KCMD_SYSTEM_SUSPEND); break;
                case 4: kodi_cmd(KCMD_QUIT); break;
                case 5: break;
            }
            enter_screen(SCREEN_MENU);
            break;
        case BSP_INPUT_NAVIGATION_KEY_F1: bsp_device_restart_to_launcher(); break;
        case BSP_INPUT_NAVIGATION_KEY_F2: enter_screen(SCREEN_MENU); break;
        default: break;
    }
}

static void save_settings_and_return(void) {
    settings.port = (uint16_t)strtoul(port_str, NULL, 10);
    if (settings.port == 0) settings.port = 8080;

    esp_err_t err = kodi_settings_save(&settings);
    if (err == ESP_OK) {
        apply_kodi_config();
        enter_screen(SCREEN_MENU);
    } else {
        settings_status_line = "Failed to save settings";
    }
}

static void handle_settings_navigation(bsp_input_event_args_navigation_t const* nav) {
    if (!nav->state) return;
    switch (nav->key) {
        case BSP_INPUT_NAVIGATION_KEY_UP:
            settings_field_index = (settings_field_index + SETTINGS_FIELD_COUNT - 1) % SETTINGS_FIELD_COUNT;
            break;
        case BSP_INPUT_NAVIGATION_KEY_DOWN:
            settings_field_index = (settings_field_index + 1) % SETTINGS_FIELD_COUNT;
            break;
        case BSP_INPUT_NAVIGATION_KEY_RETURN:
        case BSP_INPUT_NAVIGATION_KEY_SELECT:
            if (settings_field_index == SETTINGS_FIELD_COUNT - 1) {
                save_settings_and_return();
            } else {
                settings_field_index++;
            }
            break;
        case BSP_INPUT_NAVIGATION_KEY_BACKSPACE: {
            settings_field_t* field = &settings_fields[settings_field_index];
            size_t            len   = strlen(field->buf);
            if (len > 0) field->buf[len - 1] = '\0';
            break;
        }
        case BSP_INPUT_NAVIGATION_KEY_F1: bsp_device_restart_to_launcher(); break;
        case BSP_INPUT_NAVIGATION_KEY_F2: enter_screen(SCREEN_MENU); break;
        default: break;
    }
}

static void handle_settings_keyboard(bsp_input_event_args_keyboard_t const* kb) {
    char c = kb->ascii;
    if (c == '\b') {
        settings_field_t* field = &settings_fields[settings_field_index];
        size_t            len   = strlen(field->buf);
        if (len > 0) field->buf[len - 1] = '\0';
        return;
    }
    if (c == '\r' || c == '\n') {
        if (settings_field_index == SETTINGS_FIELD_COUNT - 1) {
            save_settings_and_return();
        } else {
            settings_field_index++;
        }
        return;
    }
    if (c < 32 || c > 126) return;  // ignore non-printable

    settings_field_t* field = &settings_fields[settings_field_index];
    if (field->numeric_only && (c < '0' || c > '9')) return;

    size_t len = strlen(field->buf);
    if (len + 1 < field->maxlen) {
        field->buf[len]     = c;
        field->buf[len + 1] = '\0';
    }
}

// ---- Main event loop ----

void app_main(void) {
    gpio_install_isr_service(0);

    lib_preview_mutex     = xSemaphoreCreateMutex();
    lib_preview_job_queue = xQueueCreate(1, sizeof(lib_preview_job_t));
    xTaskCreate(lib_preview_worker_task, "kodi_thumb", 6144, NULL, tskIDLE_PRIORITY + 1, &lib_preview_task);
    download_state_mutex = xSemaphoreCreateMutex();
    kodi_command_queue   = xQueueCreate(KODI_COMMAND_QUEUE_DEPTH, sizeof(kodi_command_job_t));
    xTaskCreate(kodi_command_worker_task, "kodi_cmd", 4096, NULL, tskIDLE_PRIORITY + 1, &kodi_command_task);

    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        res = nvs_flash_init();
    }
    ESP_ERROR_CHECK(res);

    const bsp_configuration_t bsp_configuration = {
        .display =
            {
                .requested_color_format = BSP_DISPLAY_COLOR_FORMAT_24_888RGB,
                .num_fbs                = 1,
            },
    };
    ESP_ERROR_CHECK(bsp_device_initialize(&bsp_configuration));

    size_t                     display_h_res, display_v_res;
    bsp_display_color_format_t display_color_format;
    bsp_display_endianness_t   display_data_endian;
    ESP_ERROR_CHECK(bsp_display_get_parameters(&display_h_res, &display_v_res, &display_color_format,
                                                &display_data_endian));
    physical_h_res = display_h_res;
    physical_v_res = display_v_res;

    pax_buf_init(&fb, NULL, display_h_res, display_v_res, PAX_BUF_24_888RGB);
    pax_buf_reversed(&fb, display_data_endian == BSP_DISPLAY_ENDIAN_BIG);

    bsp_display_rotation_t display_rotation = bsp_display_get_default_rotation();
    pax_orientation_t      orientation      = PAX_O_UPRIGHT;
    switch (display_rotation) {
        case BSP_DISPLAY_ROTATION_90: orientation = PAX_O_ROT_CCW; break;
        case BSP_DISPLAY_ROTATION_180: orientation = PAX_O_ROT_HALF; break;
        case BSP_DISPLAY_ROTATION_270: orientation = PAX_O_ROT_CW; break;
        case BSP_DISPLAY_ROTATION_0:
        default: orientation = PAX_O_UPRIGHT; break;
    }
    pax_buf_set_orientation(&fb, orientation);

    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    bsp_led_set_pixel(1, 0x0000FF);  // Radio LED: blue while connecting
    bsp_led_send();

    pax_background(&fb, BLACK);
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 8, 8, "Connecting to WiFi...");
    blit();

    if (wifi_remote_initialize() == ESP_OK) {
        wifi_connection_init_stack();
        if (wifi_connect_try_all() == ESP_OK) {
            // WiFi modem-sleep (the default power-save mode) puts the radio
            // to sleep between packets and makes it wait for the AP's next
            // beacon/DTIM before it can send again - for many routers that's
            // an extra 100ms-3s of latency on *every single* Kodi request,
            // regardless of how the request was dispatched. This app talks
            // to Kodi constantly and isn't battery-sensitive, so trade a bit
            // of power for the radio always being ready to send immediately.
            esp_wifi_set_ps(WIFI_PS_NONE);
            bsp_led_set_pixel(1, 0x00FF00);
        } else {
            bsp_led_set_pixel(1, 0xFF0000);
            ESP_LOGW(TAG, "Failed to connect to any known WiFi network");
        }
    } else {
        bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
        bsp_led_set_pixel(1, 0xFF0000);
        ESP_LOGE(TAG, "WiFi radio not responding");
    }
    bsp_led_send();

    if (kodi_settings_load(&settings) != ESP_OK) {
        current_screen = SCREEN_SETTINGS;
    }
    init_settings_fields();
    apply_kodi_config();

    if (current_screen != SCREEN_SETTINGS) {
        poll_kodi_status();
    }
    last_poll_us = esp_timer_get_time();

    render();

    while (1) {
        bsp_input_event_t event;
        bool               wants_periodic_refresh = current_screen == SCREEN_REMOTE ||
                                          (current_screen == SCREEN_DOWNLOAD && download_running);
        TickType_t         wait_ticks = wants_periodic_refresh ? pdMS_TO_TICKS(300) : portMAX_DELAY;
        BaseType_t         got_event  = xQueueReceive(input_event_queue, &event, wait_ticks);

        // Every physical key press generates BOTH a press and a release
        // event; none of the handlers below act on a release (they all
        // bail out on !state), so redrawing for it too is pure waste - on
        // this hardware a full-screen redraw is not free, and every extra
        // one directly adds to how laggy navigation feels. Only actually
        // render when something could have changed.
        //
        // If several events are already queued up (key repeat, bounce, or
        // just the user mashing arrows faster than we redraw), process all
        // of them before rendering instead of doing a full redraw per
        // event - a backlog of N queued events used to mean N sequential
        // full-screen redraws before the display caught up to what the
        // user already did.
        bool should_render = wants_periodic_refresh;
        int  drained       = 0;

        while (got_event == pdTRUE) {
            switch (event.type) {
                case INPUT_EVENT_TYPE_NAVIGATION:
                    if (event.args_navigation.state) should_render = true;
                    // Volume keys always control Kodi volume, regardless of screen.
                    if (event.args_navigation.state &&
                        (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_VOLUME_UP ||
                         event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN)) {
                        kodi_cmd(event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_VOLUME_UP ? KCMD_VOLUME_UP
                                                                                                  : KCMD_VOLUME_DOWN);
                        break;
                    }
                    switch (current_screen) {
                        case SCREEN_MENU: handle_menu_navigation(&event.args_navigation); break;
                        case SCREEN_REMOTE: handle_remote_navigation(&event.args_navigation); break;
                        case SCREEN_LIBRARY: handle_library_navigation(&event.args_navigation); break;
                        case SCREEN_TYPE: handle_type_navigation(&event.args_navigation); break;
                        case SCREEN_DOWNLOAD: handle_download_navigation(&event.args_navigation); break;
                        case SCREEN_POWER: handle_power_navigation(&event.args_navigation); break;
                        case SCREEN_SETTINGS: handle_settings_navigation(&event.args_navigation); break;
                    }
                    break;
                case INPUT_EVENT_TYPE_KEYBOARD:
                    should_render = true;
                    if (current_screen == SCREEN_REMOTE) {
                        handle_remote_keyboard(&event.args_keyboard);
                    } else if (current_screen == SCREEN_SETTINGS) {
                        handle_settings_keyboard(&event.args_keyboard);
                    } else if (current_screen == SCREEN_TYPE) {
                        handle_type_keyboard(&event.args_keyboard);
                    }
                    break;
                default: break;
            }

            if (++drained >= 32) break;  // safety cap, don't starve the render/poll below forever
            got_event = xQueueReceive(input_event_queue, &event, 0);
        }

        if (current_screen == SCREEN_REMOTE) {
            int64_t now = esp_timer_get_time();
            if (now - last_poll_us >= POLL_INTERVAL_US) {
                poll_kodi_status();
                last_poll_us = now;
                should_render = true;
            }
        }

        if (should_render) render();
    }
}
