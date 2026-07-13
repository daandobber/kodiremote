#include "kodi_library.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "kodi_client.h"

#define LIBRARY_FETCH_LIMIT 300

// In-RAM cache of already-built kodi_library_item_t arrays (PSRAM-backed;
// this device has 32MB of it). Session-lifetime only - cleared on restart,
// but that's fine: the point is to avoid re-hitting Kodi's network API on
// every single menu step, not to survive a reboot. This does not depend on
// an SD card being present/mounted at all, unlike a disk-based cache would.
typedef struct {
    char const*           key;
    kodi_library_item_t*  items;
    int                   count;  // -1 = not cached yet
} mem_cache_slot_t;

static mem_cache_slot_t g_cache_slots[] = {
    {"movies", NULL, -1}, {"tvshows", NULL, -1},      {"artists", NULL, -1},
    {"all_seasons", NULL, -1}, {"all_episodes", NULL, -1}, {"all_albums", NULL, -1},
};

static mem_cache_slot_t* find_cache_slot(char const* key) {
    for (size_t i = 0; i < sizeof(g_cache_slots) / sizeof(g_cache_slots[0]); i++) {
        if (strcmp(g_cache_slots[i].key, key) == 0) return &g_cache_slots[i];
    }
    return NULL;
}

static kodi_library_item_t* dup_items(kodi_library_item_t const* items, int count) {
    if (count <= 0) return NULL;
    size_t                bytes = (size_t)count * sizeof(kodi_library_item_t);
    kodi_library_item_t* copy  = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy == NULL) copy = malloc(bytes);
    if (copy != NULL) memcpy(copy, items, bytes);
    return copy;
}

// Returns a fresh copy of whatever is cached under 'key' so the caller's
// normal kodi_library_free_items() ownership/lifetime is unaffected; the
// cache keeps its own copy. ESP_FAIL means "not cached yet".
static esp_err_t list_cache_read(char const* key, kodi_library_item_t** out_items, int* out_count) {
    mem_cache_slot_t* slot = find_cache_slot(key);
    if (slot == NULL || slot->count < 0) return ESP_FAIL;
    if (slot->count == 0) {
        *out_items = NULL;
        *out_count = 0;
        return ESP_OK;
    }
    kodi_library_item_t* copy = dup_items(slot->items, slot->count);
    if (copy == NULL) return ESP_ERR_NO_MEM;
    *out_items = copy;
    *out_count = slot->count;
    return ESP_OK;
}

static void list_cache_write(char const* key, kodi_library_item_t const* items, int count) {
    mem_cache_slot_t* slot = find_cache_slot(key);
    if (slot == NULL) return;
    kodi_library_item_t* copy = dup_items(items, count);
    if (count > 0 && copy == NULL) return;  // keep whatever was cached before rather than wipe it on OOM
    free(slot->items);
    slot->items = copy;
    slot->count = count;
}

typedef bool (*list_cache_match_fn)(kodi_library_item_t const* item, int a, int b);

// Serves a scoped request (e.g. "seasons of show X") from a bulk "all
// seasons" in-RAM cache written by the Download Media screen, filtering
// client-side instead of hitting the network. *out_count == 0 on ESP_OK
// means the cache exists but nothing matched (treated as a miss by callers,
// since a real show/season/artist should never legitimately have zero
// entries - more likely the cache just predates this item).
static esp_err_t list_cache_filter(char const* key, list_cache_match_fn matches, int a, int b,
                                    kodi_library_item_t** out_items, int* out_count) {
    mem_cache_slot_t* slot = find_cache_slot(key);
    if (slot == NULL || slot->count < 0) return ESP_FAIL;

    int matched = 0;
    for (int i = 0; i < slot->count; i++) {
        if (matches(&slot->items[i], a, b)) matched++;
    }

    kodi_library_item_t* filtered = matched > 0 ? malloc((size_t)matched * sizeof(kodi_library_item_t)) : NULL;
    if (matched > 0 && filtered == NULL) return ESP_ERR_NO_MEM;

    int j = 0;
    for (int i = 0; i < slot->count; i++) {
        if (matches(&slot->items[i], a, b)) filtered[j++] = slot->items[i];
    }

    *out_items = filtered;
    *out_count = matched;
    return ESP_OK;
}

static bool match_tvshowid(kodi_library_item_t const* item, int tvshowid, int unused) {
    (void)unused;
    return item->tvshowid == tvshowid;
}

static bool match_tvshowid_season(kodi_library_item_t const* item, int tvshowid, int season) {
    return item->tvshowid == tvshowid && item->season == season;
}

static bool match_artistid(kodi_library_item_t const* item, int artistid, int unused) {
    (void)unused;
    return item->artistid == artistid;
}

// Kodi's "thumbnail" property is itself already an "image://<percent-encoded
// inner url>/" value. To fetch it we GET Kodi's own /image/<percent-encoded
// whole-thing> endpoint, so the "image://" wrapper needs encoding again here.
static void percent_encode_into(char const* in, char* out, size_t out_size) {
    static char const hex[] = "0123456789ABCDEF";
    size_t            o     = 0;
    for (size_t i = 0; in[i] != '\0' && o + 4 < out_size; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0xF];
        }
    }
    out[o] = '\0';
}

// Kodi's flat "thumbnail" convenience field is often empty for TV shows and
// seasons even when a poster is set (it only ever reflects one specific art
// type). The "art" property is a {"poster": ..., "thumb": ..., ...} object
// with everything Kodi actually has; try a type-appropriate priority list of
// keys against it instead so shows/seasons get their poster too.
static void build_thumb_path_from_art(cJSON* entry, char const* const* priority_keys, int priority_count, char* out,
                                      size_t out_size) {
    out[0] = '\0';
    cJSON* art = cJSON_GetObjectItemCaseSensitive(entry, "art");
    if (!cJSON_IsObject(art)) return;

    for (int i = 0; i < priority_count; i++) {
        cJSON* value = cJSON_GetObjectItemCaseSensitive(art, priority_keys[i]);
        if (cJSON_IsString(value) && value->valuestring[0] != '\0') {
            char encoded[800];
            percent_encode_into(value->valuestring, encoded, sizeof(encoded));
            snprintf(out, out_size, "/image/%s", encoded);
            return;
        }
    }
}

static void build_description(cJSON* entry, char const* field_name, char* out, size_t out_size) {
    out[0] = '\0';
    cJSON* field = cJSON_GetObjectItemCaseSensitive(entry, field_name);
    if (cJSON_IsString(field)) {
        snprintf(out, out_size, "%s", field->valuestring);
    }
}

typedef void (*item_builder_fn)(cJSON* entry, kodi_library_item_t* out);

// Runs a JSON-RPC call, walks result[array_key] (an array of objects) and
// builds one kodi_library_item_t per entry via 'builder'. Takes ownership of
// 'params'. On success (including an empty/missing array) *out_items/out_count
// are set; on failure both are left at NULL/0.
static esp_err_t fetch_array(const char* method, cJSON* params, const char* array_key, item_builder_fn builder,
                              kodi_library_item_t** out_items, int* out_count) {
    *out_items = NULL;
    *out_count = 0;

    cJSON*    root = NULL;
    esp_err_t err  = kodi_rpc_call(method, params, &root);
    if (err != ESP_OK) return err;

    cJSON* result = cJSON_GetObjectItemCaseSensitive(root, "result");
    cJSON* array  = cJSON_GetObjectItemCaseSensitive(result, array_key);
    if (!cJSON_IsArray(array)) {
        cJSON_Delete(root);
        return ESP_OK;
    }

    int                   count = cJSON_GetArraySize(array);
    kodi_library_item_t*  items = count > 0 ? calloc((size_t)count, sizeof(kodi_library_item_t)) : NULL;
    if (count > 0 && items == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < count; i++) {
        builder(cJSON_GetArrayItem(array, i), &items[i]);
    }

    cJSON_Delete(root);
    *out_items = items;
    *out_count = count;
    return ESP_OK;
}

static cJSON* make_string_array(char const* const* values, int count) {
    cJSON* array = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON_AddItemToArray(array, cJSON_CreateString(values[i]));
    }
    return array;
}

static cJSON* make_sort(char const* method) {
    cJSON* sort = cJSON_CreateObject();
    cJSON_AddStringToObject(sort, "method", method);
    cJSON_AddStringToObject(sort, "order", "ascending");
    return sort;
}

static cJSON* make_limits(void) {
    cJSON* limits = cJSON_CreateObject();
    cJSON_AddNumberToObject(limits, "start", 0);
    cJSON_AddNumberToObject(limits, "end", LIBRARY_FETCH_LIMIT);
    return limits;
}

static cJSON* make_limits_range(int start, int end) {
    cJSON* limits = cJSON_CreateObject();
    cJSON_AddNumberToObject(limits, "start", start);
    cJSON_AddNumberToObject(limits, "end", end);
    return limits;
}

// Bulk pagination for a "download everything" pass: unlike fetch_array()
// above (one call, capped at LIBRARY_FETCH_LIMIT, for interactive browsing)
// this keeps requesting pages until Kodi returns fewer than a full page, so
// it picks up the whole library regardless of size.
#define BULK_PAGE_SIZE 200

typedef cJSON* (*page_params_builder_fn)(int start, int end);

static esp_err_t fetch_array_paginated(char const* method, char const* array_key, item_builder_fn builder,
                                        page_params_builder_fn make_params, kodi_library_item_t** out_items,
                                        int* out_count) {
    *out_items = NULL;
    *out_count = 0;

    kodi_library_item_t* items    = NULL;
    int                   count    = 0;
    int                   capacity = 0;
    int                   start    = 0;

    while (1) {
        cJSON*    params = make_params(start, start + BULK_PAGE_SIZE);
        cJSON*    root   = NULL;
        esp_err_t err    = kodi_rpc_call(method, params, &root);
        if (err != ESP_OK) {
            free(items);
            return err;
        }

        cJSON* result     = cJSON_GetObjectItemCaseSensitive(root, "result");
        cJSON* array       = cJSON_GetObjectItemCaseSensitive(result, array_key);
        int    page_count = cJSON_IsArray(array) ? cJSON_GetArraySize(array) : 0;

        if (page_count > 0) {
            if (count + page_count > capacity) {
                capacity                        = count + page_count + BULK_PAGE_SIZE;
                kodi_library_item_t* new_items = realloc(items, (size_t)capacity * sizeof(kodi_library_item_t));
                if (new_items == NULL) {
                    cJSON_Delete(root);
                    free(items);
                    return ESP_ERR_NO_MEM;
                }
                items = new_items;
            }
            for (int i = 0; i < page_count; i++) {
                memset(&items[count], 0, sizeof(kodi_library_item_t));
                builder(cJSON_GetArrayItem(array, i), &items[count]);
                count++;
            }
        }

        cJSON_Delete(root);
        if (page_count < BULK_PAGE_SIZE) break;  // last page
        start += BULK_PAGE_SIZE;
    }

    *out_items = items;
    *out_count = count;
    return ESP_OK;
}

static void build_movie(cJSON* entry, kodi_library_item_t* out) {
    out->tvshowid = -1;
    out->season   = -1;
    out->artistid = -1;
    cJSON*      id    = cJSON_GetObjectItemCaseSensitive(entry, "movieid");
    cJSON*      title = cJSON_GetObjectItemCaseSensitive(entry, "title");
    cJSON*      year  = cJSON_GetObjectItemCaseSensitive(entry, "year");
    char const* title_str = cJSON_IsString(title) ? title->valuestring : "?";

    out->id = cJSON_IsNumber(id) ? id->valueint : -1;
    if (cJSON_IsNumber(year) && year->valueint > 0) {
        snprintf(out->label, sizeof(out->label), "%s (%d)", title_str, year->valueint);
    } else {
        snprintf(out->label, sizeof(out->label), "%s", title_str);
    }
    char const* art_priority[] = {"poster", "thumb"};
    build_thumb_path_from_art(entry, art_priority, 2, out->thumb_path, sizeof(out->thumb_path));
    build_description(entry, "plot", out->description, sizeof(out->description));
}

esp_err_t kodi_library_get_movies(kodi_library_item_t** out_items, int* out_count) {
    if (list_cache_read("movies", out_items, out_count) == ESP_OK && *out_count > 0) return ESP_OK;

    cJSON*      params = cJSON_CreateObject();
    char const* props[] = {"title", "year", "art", "plot"};
    cJSON_AddItemToObject(params, "properties", make_string_array(props, 4));
    cJSON_AddItemToObject(params, "sort", make_sort("label"));
    cJSON_AddItemToObject(params, "limits", make_limits());
    esp_err_t err = fetch_array("VideoLibrary.GetMovies", params, "movies", build_movie, out_items, out_count);
    if (err == ESP_OK) list_cache_write("movies", *out_items, *out_count);
    return err;
}

static void build_tvshow(cJSON* entry, kodi_library_item_t* out) {
    out->tvshowid = -1;
    out->season   = -1;
    out->artistid = -1;
    cJSON* id    = cJSON_GetObjectItemCaseSensitive(entry, "tvshowid");
    cJSON* title = cJSON_GetObjectItemCaseSensitive(entry, "title");
    out->id      = cJSON_IsNumber(id) ? id->valueint : -1;
    snprintf(out->label, sizeof(out->label), "%s", cJSON_IsString(title) ? title->valuestring : "?");
    char const* art_priority[] = {"poster", "thumb"};
    build_thumb_path_from_art(entry, art_priority, 2, out->thumb_path, sizeof(out->thumb_path));
    build_description(entry, "plot", out->description, sizeof(out->description));
}

esp_err_t kodi_library_get_tvshows(kodi_library_item_t** out_items, int* out_count) {
    if (list_cache_read("tvshows", out_items, out_count) == ESP_OK && *out_count > 0) return ESP_OK;

    cJSON*      params = cJSON_CreateObject();
    char const* props[] = {"title", "art", "plot"};
    cJSON_AddItemToObject(params, "properties", make_string_array(props, 3));
    cJSON_AddItemToObject(params, "sort", make_sort("label"));
    cJSON_AddItemToObject(params, "limits", make_limits());
    esp_err_t err = fetch_array("VideoLibrary.GetTVShows", params, "tvshows", build_tvshow, out_items, out_count);
    if (err == ESP_OK) list_cache_write("tvshows", *out_items, *out_count);
    return err;
}

static void build_season(cJSON* entry, kodi_library_item_t* out) {
    cJSON* season    = cJSON_GetObjectItemCaseSensitive(entry, "season");
    int    seasonnum = cJSON_IsNumber(season) ? season->valueint : -1;
    out->id          = seasonnum;
    out->season      = -1;  // unused for season-type items themselves; 'id' already holds the season number
    out->artistid    = -1;
    cJSON* tvshowid  = cJSON_GetObjectItemCaseSensitive(entry, "tvshowid");
    out->tvshowid    = cJSON_IsNumber(tvshowid) ? tvshowid->valueint : -1;
    if (seasonnum == 0) {
        snprintf(out->label, sizeof(out->label), "Specials");
    } else {
        snprintf(out->label, sizeof(out->label), "Season %d", seasonnum);
    }
    char const* art_priority[] = {"poster", "thumb"};
    build_thumb_path_from_art(entry, art_priority, 2, out->thumb_path, sizeof(out->thumb_path));
}

esp_err_t kodi_library_get_seasons(int tvshowid, kodi_library_item_t** out_items, int* out_count) {
    if (list_cache_filter("all_seasons", match_tvshowid, tvshowid, 0, out_items, out_count) == ESP_OK &&
        *out_count > 0) {
        return ESP_OK;
    }

    cJSON* params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "tvshowid", tvshowid);
    char const* props[] = {"season", "art"};
    cJSON_AddItemToObject(params, "properties", make_string_array(props, 2));
    cJSON_AddItemToObject(params, "sort", make_sort("season"));
    return fetch_array("VideoLibrary.GetSeasons", params, "seasons", build_season, out_items, out_count);
}

static void build_episode(cJSON* entry, kodi_library_item_t* out) {
    cJSON* id       = cJSON_GetObjectItemCaseSensitive(entry, "episodeid");
    cJSON* title    = cJSON_GetObjectItemCaseSensitive(entry, "title");
    cJSON* epnum    = cJSON_GetObjectItemCaseSensitive(entry, "episode");
    cJSON* tvshowid = cJSON_GetObjectItemCaseSensitive(entry, "tvshowid");
    cJSON* seasonn  = cJSON_GetObjectItemCaseSensitive(entry, "season");
    out->id         = cJSON_IsNumber(id) ? id->valueint : -1;
    out->tvshowid   = cJSON_IsNumber(tvshowid) ? tvshowid->valueint : -1;
    out->season     = cJSON_IsNumber(seasonn) ? seasonn->valueint : -1;
    out->artistid   = -1;
    snprintf(out->label, sizeof(out->label), "%d. %s", cJSON_IsNumber(epnum) ? epnum->valueint : 0,
             cJSON_IsString(title) ? title->valuestring : "?");
    char const* art_priority[] = {"thumb", "poster"};
    build_thumb_path_from_art(entry, art_priority, 2, out->thumb_path, sizeof(out->thumb_path));
    build_description(entry, "plot", out->description, sizeof(out->description));
}

esp_err_t kodi_library_get_episodes(int tvshowid, int season, kodi_library_item_t** out_items, int* out_count) {
    if (list_cache_filter("all_episodes", match_tvshowid_season, tvshowid, season, out_items, out_count) ==
            ESP_OK &&
        *out_count > 0) {
        return ESP_OK;
    }

    cJSON* params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "tvshowid", tvshowid);
    cJSON_AddNumberToObject(params, "season", season);
    char const* props[] = {"title", "episode", "season", "art", "plot"};
    cJSON_AddItemToObject(params, "properties", make_string_array(props, 5));
    cJSON_AddItemToObject(params, "sort", make_sort("episode"));
    return fetch_array("VideoLibrary.GetEpisodes", params, "episodes", build_episode, out_items, out_count);
}

static void build_artist(cJSON* entry, kodi_library_item_t* out) {
    out->tvshowid = -1;
    out->season   = -1;
    out->artistid = -1;
    cJSON* id    = cJSON_GetObjectItemCaseSensitive(entry, "artistid");
    cJSON* label = cJSON_GetObjectItemCaseSensitive(entry, "label");
    out->id      = cJSON_IsNumber(id) ? id->valueint : -1;
    snprintf(out->label, sizeof(out->label), "%s", cJSON_IsString(label) ? label->valuestring : "?");
    char const* art_priority[] = {"thumb", "fanart"};
    build_thumb_path_from_art(entry, art_priority, 2, out->thumb_path, sizeof(out->thumb_path));
    build_description(entry, "description", out->description, sizeof(out->description));
}

esp_err_t kodi_library_get_artists(kodi_library_item_t** out_items, int* out_count) {
    if (list_cache_read("artists", out_items, out_count) == ESP_OK && *out_count > 0) return ESP_OK;

    cJSON*      params = cJSON_CreateObject();
    char const* props[] = {"art", "description"};
    cJSON_AddItemToObject(params, "properties", make_string_array(props, 2));
    cJSON_AddItemToObject(params, "sort", make_sort("label"));
    cJSON_AddItemToObject(params, "limits", make_limits());
    esp_err_t err = fetch_array("AudioLibrary.GetArtists", params, "artists", build_artist, out_items, out_count);
    if (err == ESP_OK) list_cache_write("artists", *out_items, *out_count);
    return err;
}

static void build_album(cJSON* entry, kodi_library_item_t* out) {
    cJSON*      id    = cJSON_GetObjectItemCaseSensitive(entry, "albumid");
    cJSON*      title = cJSON_GetObjectItemCaseSensitive(entry, "title");
    cJSON*      year  = cJSON_GetObjectItemCaseSensitive(entry, "year");
    char const* title_str = cJSON_IsString(title) ? title->valuestring : "?";

    out->id       = cJSON_IsNumber(id) ? id->valueint : -1;
    out->tvshowid = -1;
    out->season   = -1;
    // Kodi returns "artistid" as an array (an album can have several artists);
    // use the first one for client-side filtering purposes.
    cJSON* artistid_field = cJSON_GetObjectItemCaseSensitive(entry, "artistid");
    if (cJSON_IsNumber(artistid_field)) {
        out->artistid = artistid_field->valueint;
    } else if (cJSON_IsArray(artistid_field) && cJSON_GetArraySize(artistid_field) > 0) {
        cJSON* first  = cJSON_GetArrayItem(artistid_field, 0);
        out->artistid = cJSON_IsNumber(first) ? first->valueint : -1;
    } else {
        out->artistid = -1;
    }
    if (cJSON_IsNumber(year) && year->valueint > 0) {
        snprintf(out->label, sizeof(out->label), "%s (%d)", title_str, year->valueint);
    } else {
        snprintf(out->label, sizeof(out->label), "%s", title_str);
    }
    char const* art_priority[] = {"thumb", "poster"};
    build_thumb_path_from_art(entry, art_priority, 2, out->thumb_path, sizeof(out->thumb_path));
    build_description(entry, "description", out->description, sizeof(out->description));
}

esp_err_t kodi_library_get_albums(int artistid, kodi_library_item_t** out_items, int* out_count) {
    if (list_cache_filter("all_albums", match_artistid, artistid, 0, out_items, out_count) == ESP_OK &&
        *out_count > 0) {
        return ESP_OK;
    }

    cJSON* params = cJSON_CreateObject();
    cJSON* filter = cJSON_CreateObject();
    cJSON_AddNumberToObject(filter, "artistid", artistid);
    cJSON_AddItemToObject(params, "filter", filter);
    char const* props[] = {"title", "year", "art", "description", "artistid"};
    cJSON_AddItemToObject(params, "properties", make_string_array(props, 5));
    cJSON_AddItemToObject(params, "sort", make_sort("year"));
    return fetch_array("AudioLibrary.GetAlbums", params, "albums", build_album, out_items, out_count);
}

// ---- Bulk "download everything" variants ----

static cJSON* all_movies_page_params(int start, int end) {
    cJSON*      params = cJSON_CreateObject();
    char const* props[] = {"title", "year", "art", "plot"};
    cJSON_AddItemToObject(params, "properties", make_string_array(props, 4));
    cJSON_AddItemToObject(params, "sort", make_sort("label"));
    cJSON_AddItemToObject(params, "limits", make_limits_range(start, end));
    return params;
}

esp_err_t kodi_library_get_all_movies(kodi_library_item_t** out_items, int* out_count) {
    esp_err_t err = fetch_array_paginated("VideoLibrary.GetMovies", "movies", build_movie, all_movies_page_params,
                                           out_items, out_count);
    if (err == ESP_OK) list_cache_write("movies", *out_items, *out_count);
    return err;
}

static cJSON* all_tvshows_page_params(int start, int end) {
    cJSON*      params = cJSON_CreateObject();
    char const* props[] = {"title", "art", "plot"};
    cJSON_AddItemToObject(params, "properties", make_string_array(props, 3));
    cJSON_AddItemToObject(params, "sort", make_sort("label"));
    cJSON_AddItemToObject(params, "limits", make_limits_range(start, end));
    return params;
}

esp_err_t kodi_library_get_all_tvshows(kodi_library_item_t** out_items, int* out_count) {
    esp_err_t err = fetch_array_paginated("VideoLibrary.GetTVShows", "tvshows", build_tvshow, all_tvshows_page_params,
                                           out_items, out_count);
    if (err == ESP_OK) list_cache_write("tvshows", *out_items, *out_count);
    return err;
}

// No "tvshowid" filter: Kodi returns seasons for every show in the library.
static cJSON* all_seasons_page_params(int start, int end) {
    cJSON*      params = cJSON_CreateObject();
    char const* props[] = {"season", "art"};
    cJSON_AddItemToObject(params, "properties", make_string_array(props, 2));
    cJSON_AddItemToObject(params, "sort", make_sort("season"));
    cJSON_AddItemToObject(params, "limits", make_limits_range(start, end));
    return params;
}

esp_err_t kodi_library_get_all_seasons(kodi_library_item_t** out_items, int* out_count) {
    esp_err_t err = fetch_array_paginated("VideoLibrary.GetSeasons", "seasons", build_season,
                                           all_seasons_page_params, out_items, out_count);
    if (err == ESP_OK) list_cache_write("all_seasons", *out_items, *out_count);
    return err;
}

// No "tvshowid"/"season" filter: Kodi returns every episode in the library.
static cJSON* all_episodes_page_params(int start, int end) {
    cJSON*      params = cJSON_CreateObject();
    char const* props[] = {"title", "episode", "season", "art", "plot"};
    cJSON_AddItemToObject(params, "properties", make_string_array(props, 5));
    cJSON_AddItemToObject(params, "sort", make_sort("title"));
    cJSON_AddItemToObject(params, "limits", make_limits_range(start, end));
    return params;
}

esp_err_t kodi_library_get_all_episodes(kodi_library_item_t** out_items, int* out_count) {
    esp_err_t err = fetch_array_paginated("VideoLibrary.GetEpisodes", "episodes", build_episode,
                                           all_episodes_page_params, out_items, out_count);
    if (err == ESP_OK) list_cache_write("all_episodes", *out_items, *out_count);
    return err;
}

static cJSON* all_artists_page_params(int start, int end) {
    cJSON*      params = cJSON_CreateObject();
    char const* props[] = {"art", "description"};
    cJSON_AddItemToObject(params, "properties", make_string_array(props, 2));
    cJSON_AddItemToObject(params, "sort", make_sort("label"));
    cJSON_AddItemToObject(params, "limits", make_limits_range(start, end));
    return params;
}

esp_err_t kodi_library_get_all_artists(kodi_library_item_t** out_items, int* out_count) {
    esp_err_t err = fetch_array_paginated("AudioLibrary.GetArtists", "artists", build_artist,
                                           all_artists_page_params, out_items, out_count);
    if (err == ESP_OK) list_cache_write("artists", *out_items, *out_count);
    return err;
}

// No "filter" by artistid: Kodi returns every album in the library.
static cJSON* all_albums_page_params(int start, int end) {
    cJSON*      params = cJSON_CreateObject();
    char const* props[] = {"title", "year", "art", "description", "artistid"};
    cJSON_AddItemToObject(params, "properties", make_string_array(props, 5));
    cJSON_AddItemToObject(params, "sort", make_sort("label"));
    cJSON_AddItemToObject(params, "limits", make_limits_range(start, end));
    return params;
}

esp_err_t kodi_library_get_all_albums(kodi_library_item_t** out_items, int* out_count) {
    esp_err_t err = fetch_array_paginated("AudioLibrary.GetAlbums", "albums", build_album, all_albums_page_params,
                                           out_items, out_count);
    if (err == ESP_OK) list_cache_write("all_albums", *out_items, *out_count);
    return err;
}

void kodi_library_free_items(kodi_library_item_t* items) {
    free(items);
}

static esp_err_t play_item_by_field(char const* field, int id) {
    cJSON* item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, field, id);
    cJSON* params = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "item", item);
    return kodi_rpc_call("Player.Open", params, NULL);
}

esp_err_t kodi_play_movie(int movieid) { return play_item_by_field("movieid", movieid); }
esp_err_t kodi_play_episode(int episodeid) { return play_item_by_field("episodeid", episodeid); }
esp_err_t kodi_play_album(int albumid) { return play_item_by_field("albumid", albumid); }
