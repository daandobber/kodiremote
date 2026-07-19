#pragma once

#include "esp_err.h"
#include "pax_gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initializes the shared RAM/SD thumbnail-cache guard. Call once before any
// thumbnail worker tasks are started.
esp_err_t kodi_thumbnail_init(void);

// Downloads the image at 'path' (a ready-to-GET path on the configured Kodi
// host, as produced in kodi_library_item_t.thumb_path) and decodes it
// (JPEG or PNG) to RGB565, scaled to fit within max_w x max_h while
// preserving the source aspect ratio (never distorts/stretches). The actual
// output size (<= max_w x max_h) is whatever pax_buf_get_width/height report
// on out_thumb afterwards. On success 'out_thumb' owns its pixel buffer; free
// with kodi_thumbnail_free().
//
// The raw downloaded bytes are cached under /sd/apps/kodiremote/thumbcache/
// (keyed by a hash of 'path'), so a second fetch of the same image reads the
// SD card instead of hitting the network. Silently skipped if there is no SD
// card mounted at /sd.
esp_err_t kodi_thumbnail_fetch(const char* path, int max_w, int max_h, pax_buf_t* out_thumb);
void      kodi_thumbnail_free(pax_buf_t* thumb);

// Makes sure the image at 'path' is present in the on-disk cache, downloading
// it from Kodi if it is not already there. Does not decode it - for bulk
// pre-caching (e.g. a "download everything" pass) where nothing needs to be
// displayed yet. Returns ESP_OK once the file is cached (already was, or was
// just written).
esp_err_t kodi_thumbnail_precache(const char* path);

#ifdef __cplusplus
}
#endif
