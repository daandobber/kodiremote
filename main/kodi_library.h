#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// A single browsable library entry. 'id' meaning depends on which fetch
// function produced it: movieid / tvshowid / season number / episodeid /
// artistid / albumid.
typedef struct {
    char label[96];
    int  id;
    // Ready-to-GET path on the Kodi webserver for this item's thumbnail/cover
    // art (e.g. "/image/image%3a%2f%2f..."), or empty if Kodi has none set.
    // Pass to kodi_fetch_binary() / kodi_thumbnail_fetch().
    char thumb_path[400];
    // Plot (movies/tvshows/episodes) or biography/review (artists/albums),
    // empty if Kodi has none set or this item type has no such field
    // (seasons).
    char description[512];
    // Grouping keys, only meaningful for seasons/episodes (tvshowid),
    // episodes (season) and albums (artistid) - used to filter a bulk-
    // fetched "all seasons"/"all episodes"/"all albums" cache down to one
    // show/season/artist client-side. -1 elsewhere.
    int tvshowid;
    int season;
    int artistid;
} kodi_library_item_t;

// Each of these allocates *out_items (caller must free with
// kodi_library_free_items) and sets *out_count. On failure both are left at
// NULL/0 and the array is not allocated.
esp_err_t kodi_library_get_movies(kodi_library_item_t** out_items, int* out_count);
esp_err_t kodi_library_get_tvshows(kodi_library_item_t** out_items, int* out_count);
esp_err_t kodi_library_get_seasons(int tvshowid, kodi_library_item_t** out_items, int* out_count);
esp_err_t kodi_library_get_episodes(int tvshowid, int season, kodi_library_item_t** out_items, int* out_count);
esp_err_t kodi_library_get_artists(kodi_library_item_t** out_items, int* out_count);
esp_err_t kodi_library_get_albums(int artistid, kodi_library_item_t** out_items, int* out_count);

// Bulk variants for a "download everything" pass: fetch the WHOLE library
// (paginated internally, not capped like the interactive functions above),
// across every show/artist rather than one at a time. Same allocation
// contract as above.
esp_err_t kodi_library_get_all_movies(kodi_library_item_t** out_items, int* out_count);
esp_err_t kodi_library_get_all_tvshows(kodi_library_item_t** out_items, int* out_count);
esp_err_t kodi_library_get_all_seasons(kodi_library_item_t** out_items, int* out_count);
esp_err_t kodi_library_get_all_episodes(kodi_library_item_t** out_items, int* out_count);
esp_err_t kodi_library_get_all_artists(kodi_library_item_t** out_items, int* out_count);
esp_err_t kodi_library_get_all_albums(kodi_library_item_t** out_items, int* out_count);

void kodi_library_free_items(kodi_library_item_t* items);

// Starts playback of a single item on the Kodi host.
esp_err_t kodi_play_movie(int movieid);
esp_err_t kodi_play_episode(int episodeid);
esp_err_t kodi_play_album(int albumid);

#ifdef __cplusplus
}
#endif
