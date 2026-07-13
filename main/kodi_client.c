#include "kodi_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"

static char const TAG[] = "kodi_client";

#define KODI_HTTP_TIMEOUT_MS 3000
#define KODI_RESPONSE_INITIAL_CAP 8192
// Large VideoLibrary/AudioLibrary listings (hundreds of movies with thumbnail
// URLs) can easily exceed a few hundred KB of JSON; this is a sanity cap, not
// a size we expect to hit in normal use.
#define KODI_MAX_RESPONSE_BYTES (768 * 1024)
#define KODI_BINARY_TIMEOUT_MS 6000
#define KODI_MAX_BINARY_BYTES (3 * 1024 * 1024)

static char s_host[64]      = {0};
static uint16_t s_port      = 8080;
static char s_auth_header[256] = {0};
static bool s_configured    = false;

typedef struct {
    char*  buf;
    size_t len;       // bytes written, excluding the trailing '\0'
    size_t capacity;  // allocated size, including room for the trailing '\0'
    bool   overflowed;
} response_ctx_t;

// The JSON-RPC endpoint is called constantly (every remote keypress, every
// status poll) and each call used to open a brand-new TCP connection and
// tear it down again — a full handshake's worth of latency on top of the
// actual request. Kept alive and reused here instead. Several FreeRTOS tasks
// (UI thread, kodi_command_worker_task, the download task) can all call into
// this, so a mutex serializes access to the one shared connection/buffer.
static SemaphoreHandle_t         s_rpc_mutex  = NULL;
static esp_http_client_handle_t  s_rpc_client = NULL;
static response_ctx_t            s_rpc_ctx    = {0};

// Grows ctx->buf (preferring PSRAM) so it can hold at least 'need' bytes plus
// a trailing '\0'. Leaves ctx->overflowed set on allocation failure or once
// KODI_MAX_RESPONSE_BYTES would be exceeded.
static bool response_ctx_reserve(response_ctx_t* ctx, size_t need) {
    if (need + 1 <= ctx->capacity) return true;
    if (need > KODI_MAX_RESPONSE_BYTES) {
        ctx->overflowed = true;
        return false;
    }
    size_t next_cap = ctx->capacity == 0 ? KODI_RESPONSE_INITIAL_CAP : ctx->capacity * 2;
    while (next_cap < need + 1) next_cap *= 2;
    if (next_cap > KODI_MAX_RESPONSE_BYTES + 1) next_cap = KODI_MAX_RESPONSE_BYTES + 1;

    char* new_buf = heap_caps_realloc(ctx->buf, next_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (new_buf == NULL) new_buf = heap_caps_realloc(ctx->buf, next_cap, MALLOC_CAP_8BIT);
    if (new_buf == NULL) {
        ctx->overflowed = true;
        return false;
    }
    ctx->buf      = new_buf;
    ctx->capacity = next_cap;
    return true;
}

static esp_err_t http_event_handler(esp_http_client_event_t* evt) {
    response_ctx_t* ctx = (response_ctx_t*)evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || ctx == NULL || ctx->overflowed || evt->data_len <= 0) {
        return ESP_OK;
    }
    if (!response_ctx_reserve(ctx, ctx->len + (size_t)evt->data_len)) {
        return ESP_OK;
    }
    memcpy(ctx->buf + ctx->len, evt->data, (size_t)evt->data_len);
    ctx->len += (size_t)evt->data_len;
    ctx->buf[ctx->len] = '\0';
    return ESP_OK;
}

void kodi_client_configure(const char* host, uint16_t port, const char* username, const char* password) {
    strncpy(s_host, host ? host : "", sizeof(s_host) - 1);
    s_host[sizeof(s_host) - 1] = '\0';
    s_port                     = port;
    s_auth_header[0]           = '\0';

    if (username != NULL && username[0] != '\0') {
        char credentials[160];
        snprintf(credentials, sizeof(credentials), "%s:%s", username, password ? password : "");

        unsigned char encoded[220];
        size_t        encoded_len = 0;
        int           rc = mbedtls_base64_encode(encoded, sizeof(encoded), &encoded_len,
                                                   (const unsigned char*)credentials, strlen(credentials));
        if (rc == 0) {
            encoded[encoded_len] = '\0';
            snprintf(s_auth_header, sizeof(s_auth_header), "Basic %s", (char*)encoded);
        } else {
            ESP_LOGW(TAG, "Failed to base64-encode credentials (%d)", rc);
        }
    }

    s_configured = s_host[0] != '\0';
}

bool kodi_client_is_configured(void) {
    return s_configured;
}

// Performs a JSON-RPC request. 'params' ownership is taken (freed by this function).
// On success, if out_root is not NULL, the parsed response root is returned there and
// the caller must cJSON_Delete() it. When out_root is NULL the response body is discarded.
static esp_err_t kodi_rpc(const char* method, cJSON* params, cJSON** out_root) {
    if (!s_configured) {
        if (params) cJSON_Delete(params);
        return ESP_ERR_INVALID_STATE;
    }

    cJSON* request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "jsonrpc", "2.0");
    cJSON_AddStringToObject(request, "method", method);
    cJSON_AddNumberToObject(request, "id", 1);
    if (params != NULL) {
        cJSON_AddItemToObject(request, "params", params);
    }

    char* body = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (s_rpc_mutex == NULL) {
        // First caller in creates the mutex; kodi_client_configure() always
        // runs on the main task well before any other task can call kodi_rpc().
        s_rpc_mutex = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_rpc_mutex, portMAX_DELAY);

    esp_err_t result = ESP_FAIL;

    if (s_rpc_client == NULL) {
        char url[96];
        snprintf(url, sizeof(url), "http://%s:%u/jsonrpc", s_host, s_port);

        esp_http_client_config_t config = {
            .url                   = url,
            .method                = HTTP_METHOD_POST,
            .timeout_ms            = KODI_HTTP_TIMEOUT_MS,
            .event_handler         = http_event_handler,
            .user_data             = &s_rpc_ctx,
            .disable_auto_redirect = true,
            .keep_alive_enable     = true,
        };
        s_rpc_client = esp_http_client_init(&config);
    }

    if (s_rpc_client == NULL) {
        xSemaphoreGive(s_rpc_mutex);
        free(body);
        return ESP_ERR_NO_MEM;
    }

    s_rpc_ctx.len         = 0;
    s_rpc_ctx.overflowed  = false;
    if (s_rpc_ctx.buf != NULL) s_rpc_ctx.buf[0] = '\0';

    esp_http_client_set_header(s_rpc_client, "Content-Type", "application/json");
    if (s_auth_header[0] != '\0') {
        esp_http_client_set_header(s_rpc_client, "Authorization", s_auth_header);
    }
    esp_http_client_set_post_field(s_rpc_client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(s_rpc_client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(s_rpc_client);
        if (status >= 200 && status < 300) {
            result = ESP_OK;
        } else {
            ESP_LOGW(TAG, "%s -> HTTP %d", method, status);
            result = ESP_FAIL;
        }
    } else {
        ESP_LOGW(TAG, "%s request failed: %s", method, esp_err_to_name(err));
        result = err;
        // The connection may be wedged or the socket may have been closed
        // out from under us; drop it so the next call opens a fresh one
        // instead of retrying a broken handle forever.
        esp_http_client_close(s_rpc_client);
        esp_http_client_cleanup(s_rpc_client);
        s_rpc_client = NULL;
    }

    free(body);

    if (result == ESP_OK && out_root != NULL) {
        if (s_rpc_ctx.overflowed) {
            ESP_LOGW(TAG, "%s -> response exceeded %d bytes", method, KODI_MAX_RESPONSE_BYTES);
            result = ESP_ERR_INVALID_SIZE;
        } else {
            cJSON* root = cJSON_Parse(s_rpc_ctx.buf != NULL ? s_rpc_ctx.buf : "");
            if (root == NULL) {
                result = ESP_FAIL;
            } else if (cJSON_GetObjectItemCaseSensitive(root, "error") != NULL) {
                cJSON* err_obj = cJSON_GetObjectItemCaseSensitive(root, "error");
                cJSON* msg     = cJSON_GetObjectItemCaseSensitive(err_obj, "message");
                ESP_LOGW(TAG, "%s -> RPC error: %s", method, msg && cJSON_IsString(msg) ? msg->valuestring : "?");
                cJSON_Delete(root);
                result = ESP_FAIL;
            } else {
                *out_root = root;
            }
        }
    }

    xSemaphoreGive(s_rpc_mutex);
    return result;
}

static esp_err_t kodi_rpc_simple(const char* method, cJSON* params) {
    return kodi_rpc(method, params, NULL);
}

esp_err_t kodi_ping(void) {
    cJSON*    root = NULL;
    esp_err_t err  = kodi_rpc("JSONRPC.Ping", NULL, &root);
    if (root) cJSON_Delete(root);
    return err;
}

// ---- Input namespace ----

esp_err_t kodi_input_up(void) { return kodi_rpc_simple("Input.Up", NULL); }
esp_err_t kodi_input_down(void) { return kodi_rpc_simple("Input.Down", NULL); }
esp_err_t kodi_input_left(void) { return kodi_rpc_simple("Input.Left", NULL); }
esp_err_t kodi_input_right(void) { return kodi_rpc_simple("Input.Right", NULL); }
esp_err_t kodi_input_select(void) { return kodi_rpc_simple("Input.Select", NULL); }
esp_err_t kodi_input_back(void) { return kodi_rpc_simple("Input.Back", NULL); }
esp_err_t kodi_input_home(void) { return kodi_rpc_simple("Input.Home", NULL); }
esp_err_t kodi_input_context_menu(void) { return kodi_rpc_simple("Input.ContextMenu", NULL); }
esp_err_t kodi_input_info(void) { return kodi_rpc_simple("Input.Info", NULL); }
esp_err_t kodi_input_show_osd(void) { return kodi_rpc_simple("Input.ShowOSD", NULL); }

// ---- Active player helper ----

static esp_err_t get_active_player_id(int* out_id) {
    cJSON*    root = NULL;
    esp_err_t err  = kodi_rpc("Player.GetActivePlayers", NULL, &root);
    if (err != ESP_OK) return err;

    esp_err_t result = ESP_ERR_NOT_FOUND;
    cJSON*    players = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (cJSON_IsArray(players) && cJSON_GetArraySize(players) > 0) {
        cJSON* first = cJSON_GetArrayItem(players, 0);
        cJSON* id    = cJSON_GetObjectItemCaseSensitive(first, "playerid");
        if (cJSON_IsNumber(id)) {
            *out_id = id->valueint;
            result  = ESP_OK;
        }
    }

    cJSON_Delete(root);
    return result;
}

static esp_err_t player_command(const char* method, cJSON* extra_params) {
    int       playerid;
    esp_err_t err = get_active_player_id(&playerid);
    if (err != ESP_OK) {
        if (extra_params) cJSON_Delete(extra_params);
        return err;
    }

    cJSON* params = extra_params != NULL ? extra_params : cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "playerid", playerid);
    return kodi_rpc_simple(method, params);
}

esp_err_t kodi_play_pause(void) { return player_command("Player.PlayPause", NULL); }
esp_err_t kodi_stop(void) { return player_command("Player.Stop", NULL); }

esp_err_t kodi_play_next(void) {
    cJSON* params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "to", "next");
    return player_command("Player.GoTo", params);
}

esp_err_t kodi_play_previous(void) {
    cJSON* params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "to", "previous");
    return player_command("Player.GoTo", params);
}

static esp_err_t player_seek(const char* value) {
    cJSON* params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "value", value);
    return player_command("Player.Seek", params);
}

esp_err_t kodi_seek_small_forward(void) { return player_seek("smallforward"); }
esp_err_t kodi_seek_small_backward(void) { return player_seek("smallbackward"); }
esp_err_t kodi_seek_big_forward(void) { return player_seek("bigforward"); }
esp_err_t kodi_seek_big_backward(void) { return player_seek("bigbackward"); }

esp_err_t kodi_volume_step(bool increment) {
    cJSON* params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "volume", increment ? "increment" : "decrement");
    return kodi_rpc_simple("Application.SetVolume", params);
}

esp_err_t kodi_toggle_mute(void) {
    cJSON* params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "mute", "toggle");
    return kodi_rpc_simple("Application.SetMute", params);
}

esp_err_t kodi_activate_window(const char* window_name) {
    cJSON* params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "window", window_name);
    return kodi_rpc_simple("GUI.ActivateWindow", params);
}

esp_err_t kodi_system_shutdown(void) { return kodi_rpc_simple("System.Shutdown", NULL); }
esp_err_t kodi_system_reboot(void) { return kodi_rpc_simple("System.Reboot", NULL); }
esp_err_t kodi_system_hibernate(void) { return kodi_rpc_simple("System.Hibernate", NULL); }
esp_err_t kodi_system_suspend(void) { return kodi_rpc_simple("System.Suspend", NULL); }
esp_err_t kodi_quit(void) { return kodi_rpc_simple("Application.Quit", NULL); }

esp_err_t kodi_send_text(const char* text, bool done) {
    cJSON* params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "text", text);
    cJSON_AddBoolToObject(params, "done", done);
    return kodi_rpc_simple("Input.SendText", params);
}

esp_err_t kodi_rpc_call(const char* method, cJSON* params, cJSON** out_root) {
    return kodi_rpc(method, params, out_root);
}

typedef struct {
    uint8_t* buf;
    size_t   len;
    size_t   cap;
    bool     overflowed;
} binary_ctx_t;

static esp_err_t binary_event_handler(esp_http_client_event_t* evt) {
    binary_ctx_t* ctx = (binary_ctx_t*)evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || ctx == NULL || ctx->overflowed) {
        return ESP_OK;
    }

    size_t need = ctx->len + (size_t)evt->data_len;
    if (need > KODI_MAX_BINARY_BYTES) {
        ctx->overflowed = true;
        return ESP_OK;
    }

    if (need > ctx->cap) {
        size_t next_cap = ctx->cap == 0 ? 32768 : ctx->cap * 2;
        while (next_cap < need) next_cap *= 2;
        uint8_t* new_buf = heap_caps_realloc(ctx->buf, next_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (new_buf == NULL) new_buf = heap_caps_realloc(ctx->buf, next_cap, MALLOC_CAP_8BIT);
        if (new_buf == NULL) {
            ctx->overflowed = true;
            return ESP_OK;
        }
        ctx->buf = new_buf;
        ctx->cap = next_cap;
    }

    memcpy(ctx->buf + ctx->len, evt->data, (size_t)evt->data_len);
    ctx->len += (size_t)evt->data_len;
    return ESP_OK;
}

esp_err_t kodi_fetch_binary(const char* path, uint8_t** out_data, size_t* out_len) {
    *out_data = NULL;
    *out_len  = 0;
    if (!s_configured) return ESP_ERR_INVALID_STATE;

    char url[512];
    snprintf(url, sizeof(url), "http://%s:%u%s", s_host, s_port, path);

    binary_ctx_t ctx = {0};

    esp_http_client_config_t config = {
        .url                   = url,
        .method                = HTTP_METHOD_GET,
        .timeout_ms            = KODI_BINARY_TIMEOUT_MS,
        .event_handler         = binary_event_handler,
        .user_data             = &ctx,
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return ESP_ERR_NO_MEM;
    if (s_auth_header[0] != '\0') {
        esp_http_client_set_header(client, "Authorization", s_auth_header);
    }

    esp_err_t result;
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (ctx.overflowed) {
            result = ESP_ERR_INVALID_SIZE;
        } else if (status >= 200 && status < 300 && ctx.len > 0) {
            result = ESP_OK;
        } else {
            result = ESP_FAIL;
        }
    } else {
        result = err;
    }
    esp_http_client_cleanup(client);

    if (result == ESP_OK) {
        *out_data = ctx.buf;
        *out_len  = ctx.len;
    } else if (ctx.buf != NULL) {
        heap_caps_free(ctx.buf);
    }
    return result;
}

void kodi_free_binary(uint8_t* data) {
    heap_caps_free(data);
}

// ---- Status polling ----

static void extract_now_playing_subtitle(cJSON* item, char* out, size_t out_size) {
    out[0] = '\0';

    cJSON* showtitle = cJSON_GetObjectItemCaseSensitive(item, "showtitle");
    cJSON* season    = cJSON_GetObjectItemCaseSensitive(item, "season");
    cJSON* episode   = cJSON_GetObjectItemCaseSensitive(item, "episode");
    if (cJSON_IsString(showtitle) && showtitle->valuestring[0] != '\0') {
        if (cJSON_IsNumber(season) && cJSON_IsNumber(episode) && season->valueint >= 0 && episode->valueint >= 0) {
            snprintf(out, out_size, "%s S%02dE%02d", showtitle->valuestring, season->valueint, episode->valueint);
        } else {
            snprintf(out, out_size, "%s", showtitle->valuestring);
        }
        return;
    }

    cJSON* artists = cJSON_GetObjectItemCaseSensitive(item, "artist");
    if (cJSON_IsArray(artists) && cJSON_GetArraySize(artists) > 0) {
        cJSON* first = cJSON_GetArrayItem(artists, 0);
        if (cJSON_IsString(first)) {
            snprintf(out, out_size, "%s", first->valuestring);
            return;
        }
    }

    cJSON* album = cJSON_GetObjectItemCaseSensitive(item, "album");
    if (cJSON_IsString(album) && album->valuestring[0] != '\0') {
        snprintf(out, out_size, "%s", album->valuestring);
    }
}

static void extract_time(cJSON* time_obj, int* hours, int* minutes, int* seconds) {
    *hours = *minutes = *seconds = 0;
    if (!time_obj) return;
    cJSON* h = cJSON_GetObjectItemCaseSensitive(time_obj, "hours");
    cJSON* m = cJSON_GetObjectItemCaseSensitive(time_obj, "minutes");
    cJSON* s = cJSON_GetObjectItemCaseSensitive(time_obj, "seconds");
    if (cJSON_IsNumber(h)) *hours = h->valueint;
    if (cJSON_IsNumber(m)) *minutes = m->valueint;
    if (cJSON_IsNumber(s)) *seconds = s->valueint;
}

esp_err_t kodi_get_status(kodi_status_t* out) {
    memset(out, 0, sizeof(*out));
    out->player_type      = KODI_PLAYER_NONE;
    out->active_player_id = -1;

    // Volume/mute are independent of playback state.
    {
        cJSON* root   = NULL;
        cJSON* params = cJSON_CreateObject();
        cJSON* props  = cJSON_CreateArray();
        cJSON_AddItemToArray(props, cJSON_CreateString("volume"));
        cJSON_AddItemToArray(props, cJSON_CreateString("muted"));
        cJSON_AddItemToObject(params, "properties", props);

        if (kodi_rpc("Application.GetProperties", params, &root) == ESP_OK) {
            cJSON* result = cJSON_GetObjectItemCaseSensitive(root, "result");
            cJSON* volume = cJSON_GetObjectItemCaseSensitive(result, "volume");
            cJSON* muted  = cJSON_GetObjectItemCaseSensitive(result, "muted");
            if (cJSON_IsNumber(volume)) out->volume = volume->valueint;
            if (cJSON_IsBool(muted)) out->muted = cJSON_IsTrue(muted);
            cJSON_Delete(root);
        } else {
            return ESP_FAIL;
        }
    }

    int active_id = -1;
    if (get_active_player_id(&active_id) != ESP_OK) {
        out->playing = false;
        return ESP_OK;
    }

    out->playing          = true;
    out->active_player_id = active_id;

    // Item metadata
    {
        cJSON* root   = NULL;
        cJSON* params = cJSON_CreateObject();
        cJSON_AddNumberToObject(params, "playerid", active_id);
        cJSON* props = cJSON_CreateArray();
        cJSON_AddItemToArray(props, cJSON_CreateString("title"));
        cJSON_AddItemToArray(props, cJSON_CreateString("artist"));
        cJSON_AddItemToArray(props, cJSON_CreateString("showtitle"));
        cJSON_AddItemToArray(props, cJSON_CreateString("album"));
        cJSON_AddItemToArray(props, cJSON_CreateString("season"));
        cJSON_AddItemToArray(props, cJSON_CreateString("episode"));
        cJSON_AddItemToObject(params, "properties", props);

        if (kodi_rpc("Player.GetItem", params, &root) == ESP_OK) {
            cJSON* result = cJSON_GetObjectItemCaseSensitive(root, "result");
            cJSON* item   = cJSON_GetObjectItemCaseSensitive(result, "item");
            cJSON* label  = cJSON_GetObjectItemCaseSensitive(item, "label");
            cJSON* title  = cJSON_GetObjectItemCaseSensitive(item, "title");
            cJSON* type   = cJSON_GetObjectItemCaseSensitive(item, "type");

            if (cJSON_IsString(title) && title->valuestring[0] != '\0') {
                snprintf(out->title, sizeof(out->title), "%s", title->valuestring);
            } else if (cJSON_IsString(label)) {
                snprintf(out->title, sizeof(out->title), "%s", label->valuestring);
            }

            if (cJSON_IsString(type) && strcmp(type->valuestring, "song") == 0) {
                out->player_type = KODI_PLAYER_AUDIO;
            } else {
                out->player_type = KODI_PLAYER_VIDEO;
            }

            extract_now_playing_subtitle(item, out->subtitle, sizeof(out->subtitle));
            cJSON_Delete(root);
        }
    }

    // Playback properties
    {
        cJSON* root   = NULL;
        cJSON* params = cJSON_CreateObject();
        cJSON_AddNumberToObject(params, "playerid", active_id);
        cJSON* props = cJSON_CreateArray();
        cJSON_AddItemToArray(props, cJSON_CreateString("percentage"));
        cJSON_AddItemToArray(props, cJSON_CreateString("time"));
        cJSON_AddItemToArray(props, cJSON_CreateString("totaltime"));
        cJSON_AddItemToArray(props, cJSON_CreateString("speed"));
        cJSON_AddItemToObject(params, "properties", props);

        if (kodi_rpc("Player.GetProperties", params, &root) == ESP_OK) {
            cJSON* result     = cJSON_GetObjectItemCaseSensitive(root, "result");
            cJSON* percentage = cJSON_GetObjectItemCaseSensitive(result, "percentage");
            cJSON* speed      = cJSON_GetObjectItemCaseSensitive(result, "speed");
            cJSON* time_obj   = cJSON_GetObjectItemCaseSensitive(result, "time");
            cJSON* total_obj  = cJSON_GetObjectItemCaseSensitive(result, "totaltime");

            if (cJSON_IsNumber(percentage)) out->percentage = percentage->valuedouble;
            if (cJSON_IsNumber(speed)) out->paused = speed->valueint == 0;

            extract_time(time_obj, &out->time_hours, &out->time_minutes, &out->time_seconds);
            extract_time(total_obj, &out->total_hours, &out->total_minutes, &out->total_seconds);

            cJSON_Delete(root);
        }
    }

    return ESP_OK;
}
