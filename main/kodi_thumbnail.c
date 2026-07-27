#include "kodi_thumbnail.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "jpeg_decoder.h"
#include "kodi_client.h"
#include "miniz.h"

// Bounds on the *source* image accepted for decode; well above anything a
// real Kodi thumbnail/poster would be, just guards against a corrupt header.
#define THUMBNAIL_MAX_SRC_DIM 8192
#define PNG_MAX_RAW_BYTES     (2 * 1024 * 1024)

// Raw (undecoded) downloads are cached here on the SD card, keyed by a hash
// of the Kodi image path, so re-visiting a movie/show/album only re-decodes
// a local file instead of re-downloading it from Kodi. Silently does nothing
// if there is no SD card / the launcher hasn't mounted one at "/sd".
#define THUMB_CACHE_DIR "/sd/apps/kodiremote/thumbcache"

static uint32_t fnv1a_hash(char const* s) {
    uint32_t h = 2166136261u;
    for (; *s != '\0'; s++) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

static void cache_file_path(char const* thumb_path, char* out, size_t out_size) {
    snprintf(out, out_size, "%s/%08x.img", THUMB_CACHE_DIR, (unsigned int)fnv1a_hash(thumb_path));
}

static void ensure_cache_dir(void) {
    static bool tried = false;
    if (tried) return;
    tried = true;
    mkdir("/sd/apps", 0775);
    mkdir("/sd/apps/kodiremote", 0775);
    mkdir(THUMB_CACHE_DIR, 0775);
}

// Reads a whole file into a heap_caps-allocated buffer (prefers PSRAM).
static esp_err_t read_whole_file(char const* path, uint8_t** out_data, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) return ESP_FAIL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return ESP_FAIL;
    }

    uint8_t* buf = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) buf = heap_caps_malloc((size_t)size, MALLOC_CAP_8BIT);
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        heap_caps_free(buf);
        return ESP_FAIL;
    }

    *out_data = buf;
    *out_len  = (size_t)size;
    return ESP_OK;
}

static void write_whole_file(char const* path, uint8_t const* data, size_t len) {
    ensure_cache_dir();
    FILE* f = fopen(path, "wb");
    if (f == NULL) return;  // no SD card, or directory missing - caching just silently doesn't happen
    fwrite(data, 1, len, f);
    fclose(f);
}

// In-RAM cache of already-decoded thumbnails (PSRAM-backed), checked before
// the SD-card cache above. Guarantees a fast repeat-visit even when there is
// no SD card / it isn't mounted where expected - this device has 32MB of
// PSRAM, plenty for a few dozen small decoded posters for the current
// session. FIFO eviction once full; session-lifetime only.
#define THUMB_MEM_CACHE_SLOTS 60

typedef struct {
    char      path[400];
    int       w, h;
    uint16_t* pixels;  // NULL when unused
} thumb_mem_cache_slot_t;

static thumb_mem_cache_slot_t g_thumb_mem_cache[THUMB_MEM_CACHE_SLOTS];
static int                    g_thumb_mem_cache_next = 0;
static SemaphoreHandle_t      g_thumb_cache_mutex    = NULL;

esp_err_t kodi_thumbnail_init(void) {
    if (g_thumb_cache_mutex != NULL) return ESP_OK;
    g_thumb_cache_mutex = xSemaphoreCreateMutex();
    return g_thumb_cache_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static void thumb_cache_lock(void) {
    if (g_thumb_cache_mutex != NULL) xSemaphoreTake(g_thumb_cache_mutex, portMAX_DELAY);
}

static void thumb_cache_unlock(void) {
    if (g_thumb_cache_mutex != NULL) xSemaphoreGive(g_thumb_cache_mutex);
}

// Matches on path alone: this app only ever calls kodi_thumbnail_fetch() for
// a given kind of preview with one fixed max_w/max_h pair, so the actually-
// decoded (aspect-preserving) size for a given path is always the same.
static bool thumb_mem_cache_get(char const* path, pax_buf_t* out) {
    for (int i = 0; i < THUMB_MEM_CACHE_SLOTS; i++) {
        thumb_mem_cache_slot_t* slot = &g_thumb_mem_cache[i];
        if (slot->pixels == NULL || strcmp(slot->path, path) != 0) continue;

        // Hand back a fresh copy so the caller's kodi_thumbnail_free() can't
        // ever invalidate what's sitting in the cache.
        size_t    bytes = (size_t)slot->w * (size_t)slot->h * sizeof(uint16_t);
        uint16_t* copy  = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (copy == NULL) copy = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
        if (copy == NULL) return false;
        memcpy(copy, slot->pixels, bytes);
        if (!pax_buf_init(out, copy, slot->w, slot->h, PAX_BUF_16_565RGB)) {
            heap_caps_free(copy);
            return false;
        }
        return true;
    }
    return false;
}

static void thumb_mem_cache_put(char const* path, pax_buf_t const* thumb) {
    int    w     = pax_buf_get_width(thumb);
    int    h     = pax_buf_get_height(thumb);
    size_t bytes = (size_t)w * (size_t)h * sizeof(uint16_t);

    uint16_t* copy = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy == NULL) copy = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    if (copy == NULL) return;  // just skip caching this one under memory pressure
    memcpy(copy, thumb->buf, bytes);

    thumb_mem_cache_slot_t* slot = &g_thumb_mem_cache[g_thumb_mem_cache_next];
    if (slot->pixels != NULL) heap_caps_free(slot->pixels);
    snprintf(slot->path, sizeof(slot->path), "%s", path);
    slot->w      = w;
    slot->h      = h;
    slot->pixels = copy;
    g_thumb_mem_cache_next = (g_thumb_mem_cache_next + 1) % THUMB_MEM_CACHE_SLOTS;
}

static bool magic_is_jpeg(uint8_t const* data, size_t len) {
    return len >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff;
}

static bool magic_is_png(uint8_t const* data, size_t len) {
    static uint8_t const sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    return len >= sizeof(sig) && memcmp(data, sig, sizeof(sig)) == 0;
}

static uint32_t be32(uint8_t const* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static bool parse_jpeg_size(uint8_t const* data, size_t data_len, uint32_t* out_w, uint32_t* out_h) {
    if (!magic_is_jpeg(data, data_len)) return false;
    size_t pos = 2;
    while (pos + 4 <= data_len) {
        while (pos < data_len && data[pos] == 0xff) pos++;
        if (pos >= data_len) return false;
        uint8_t marker = data[pos++];
        if (marker == 0xd9 || marker == 0xda) return false;
        if (marker >= 0xd0 && marker <= 0xd7) continue;
        if (pos + 2 > data_len) return false;
        uint16_t len = ((uint16_t)data[pos] << 8) | data[pos + 1];
        if (len < 2 || pos + len > data_len) return false;
        if ((marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7) ||
            (marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf)) {
            if (len < 7) return false;
            *out_h = ((uint16_t)data[pos + 3] << 8) | data[pos + 4];
            *out_w = ((uint16_t)data[pos + 5] << 8) | data[pos + 6];
            return *out_w > 0 && *out_h > 0;
        }
        pos += len;
    }
    return false;
}

static void downsample_rgb565(uint16_t const* src, int src_w, int src_h, uint16_t* dst, int dst_w, int dst_h) {
    for (int y = 0; y < dst_h; y++) {
        int sy = (int)((int64_t)y * src_h / dst_h);
        if (sy >= src_h) sy = src_h - 1;
        for (int x = 0; x < dst_w; x++) {
            int sx = (int)((int64_t)x * src_w / dst_w);
            if (sx >= src_w) sx = src_w - 1;
            dst[y * dst_w + x] = src[sy * src_w + sx];
        }
    }
}

// Computes output dimensions that fit within max_w x max_h while preserving
// the source aspect ratio (never distorts/stretches the image).
static void fit_within_box(uint32_t src_w, uint32_t src_h, int max_w, int max_h, int* out_w, int* out_h) {
    float scale = (float)max_w / (float)src_w;
    float scale_h = (float)max_h / (float)src_h;
    if (scale_h < scale) scale = scale_h;

    *out_w = (int)(src_w * scale + 0.5f);
    *out_h = (int)(src_h * scale + 0.5f);
    if (*out_w < 1) *out_w = 1;
    if (*out_h < 1) *out_h = 1;
}

static esp_err_t decode_jpeg_thumb(uint8_t const* data, size_t data_len, int max_w, int max_h, pax_buf_t* out) {
    uint32_t width = 0, height = 0;
    if (!parse_jpeg_size(data, data_len, &width, &height) || width > THUMBNAIL_MAX_SRC_DIM ||
        height > THUMBNAIL_MAX_SRC_DIM) {
        return ESP_FAIL;
    }

    int target_w, target_h;
    fit_within_box(width, height, max_w, max_h, &target_w, &target_h);

    int      smallest_target = target_w < target_h ? target_w : target_h;
    uint32_t min_dim         = width < height ? width : height;

    uint8_t                scale_div = 1;
    esp_jpeg_image_scale_t scale     = JPEG_IMAGE_SCALE_0;
    if (min_dim / 8 >= (uint32_t)smallest_target) {
        scale     = JPEG_IMAGE_SCALE_1_8;
        scale_div = 8;
    } else if (min_dim / 4 >= (uint32_t)smallest_target) {
        scale     = JPEG_IMAGE_SCALE_1_4;
        scale_div = 4;
    } else if (min_dim / 2 >= (uint32_t)smallest_target) {
        scale     = JPEG_IMAGE_SCALE_1_2;
        scale_div = 2;
    }

    uint32_t decode_w = (width + scale_div - 1) / scale_div;
    uint32_t decode_h = (height + scale_div - 1) / scale_div;
    size_t   decode_bytes = (size_t)decode_w * decode_h * sizeof(uint16_t);

    uint16_t* decoded = heap_caps_malloc(decode_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (decoded == NULL) decoded = heap_caps_malloc(decode_bytes, MALLOC_CAP_8BIT);
    if (decoded == NULL) return ESP_ERR_NO_MEM;

    esp_jpeg_image_cfg_t cfg = {
        .indata      = (uint8_t*)data,
        .indata_size = (uint32_t)data_len,
        .outbuf      = (uint8_t*)decoded,
        .outbuf_size = (uint32_t)decode_bytes,
        .out_format  = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale   = scale,
    };
    esp_jpeg_image_output_t result = {0};
    esp_err_t                err   = esp_jpeg_decode(&cfg, &result);
    if (err != ESP_OK || result.width == 0 || result.height == 0) {
        heap_caps_free(decoded);
        return err == ESP_OK ? ESP_FAIL : err;
    }

    size_t    thumb_bytes = (size_t)target_w * target_h * sizeof(uint16_t);
    uint16_t* thumb       = heap_caps_malloc(thumb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (thumb == NULL) thumb = heap_caps_malloc(thumb_bytes, MALLOC_CAP_8BIT);
    if (thumb == NULL) {
        heap_caps_free(decoded);
        return ESP_ERR_NO_MEM;
    }
    downsample_rgb565(decoded, (int)result.width, (int)result.height, thumb, target_w, target_h);
    heap_caps_free(decoded);

    if (!pax_buf_init(out, thumb, target_w, target_h, PAX_BUF_16_565RGB)) {
        heap_caps_free(thumb);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c) {
    int p  = (int)a + (int)b - (int)c;
    int pa = abs(p - (int)a);
    int pb = abs(p - (int)b);
    int pc = abs(p - (int)c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

static int png_channels(uint8_t color_type) {
    switch (color_type) {
        case 0: return 1;
        case 2: return 3;
        case 3: return 1;
        case 4: return 2;
        case 6: return 4;
        default: return 0;
    }
}

static esp_err_t decode_png_thumb(uint8_t const* data, size_t data_len, int max_w, int max_h, pax_buf_t* out) {
    if (!magic_is_png(data, data_len)) return ESP_ERR_INVALID_ARG;

    uint32_t  width = 0, height = 0;
    uint8_t   bit_depth = 0, color_type = 0, interlace = 0;
    pax_col_t palette[256]  = {0};
    size_t    palette_len   = 0;
    uint8_t*  idat          = NULL;
    size_t    idat_len = 0, idat_cap = 0;

    size_t pos = 8;
    while (pos + 12 <= data_len) {
        uint32_t       len   = be32(data + pos);
        uint8_t const* type  = data + pos + 4;
        uint8_t const* chunk = data + pos + 8;
        if (pos + 12 + len > data_len) break;

        if (memcmp(type, "IHDR", 4) == 0 && len >= 13) {
            width      = be32(chunk);
            height     = be32(chunk + 4);
            bit_depth  = chunk[8];
            color_type = chunk[9];
            interlace  = chunk[12];
        } else if (memcmp(type, "PLTE", 4) == 0) {
            palette_len = len / 3;
            if (palette_len > 256) palette_len = 256;
            for (size_t i = 0; i < palette_len; i++) {
                palette[i] = 0xff000000 | ((pax_col_t)chunk[i * 3] << 16) | ((pax_col_t)chunk[i * 3 + 1] << 8) |
                             (pax_col_t)chunk[i * 3 + 2];
            }
        } else if (memcmp(type, "tRNS", 4) == 0 && palette_len > 0) {
            size_t n = len < palette_len ? len : palette_len;
            for (size_t i = 0; i < n; i++) palette[i] = (palette[i] & 0x00ffffff) | ((pax_col_t)chunk[i] << 24);
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (idat_len + len > PNG_MAX_RAW_BYTES) {
                heap_caps_free(idat);
                return ESP_ERR_INVALID_SIZE;
            }
            if (idat_len + len > idat_cap) {
                size_t next = idat_cap == 0 ? 16384 : idat_cap * 2;
                while (next < idat_len + len) next *= 2;
                uint8_t* new_buf = heap_caps_realloc(idat, next, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (new_buf == NULL) new_buf = heap_caps_realloc(idat, next, MALLOC_CAP_8BIT);
                if (new_buf == NULL) {
                    heap_caps_free(idat);
                    return ESP_ERR_NO_MEM;
                }
                idat     = new_buf;
                idat_cap = next;
            }
            memcpy(idat + idat_len, chunk, len);
            idat_len += len;
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + len;
    }

    int channels = png_channels(color_type);
    if (width == 0 || height == 0 || bit_depth != 8 || channels == 0 || interlace != 0 || idat_len == 0 ||
        (color_type == 3 && palette_len == 0)) {
        heap_caps_free(idat);
        return ESP_ERR_NOT_SUPPORTED;
    }

    int target_w, target_h;
    fit_within_box(width, height, max_w, max_h, &target_w, &target_h);

    size_t row_bytes = (size_t)width * (size_t)channels;
    size_t raw_len    = (row_bytes + 1) * (size_t)height;
    if (raw_len > PNG_MAX_RAW_BYTES) {
        heap_caps_free(idat);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t* raw = heap_caps_malloc(raw_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (raw == NULL) raw = heap_caps_malloc(raw_len, MALLOC_CAP_8BIT);
    if (raw == NULL) {
        heap_caps_free(idat);
        return ESP_ERR_NO_MEM;
    }

    size_t inflated = tinfl_decompress_mem_to_mem(raw, raw_len, idat, idat_len, TINFL_FLAG_PARSE_ZLIB_HEADER);
    heap_caps_free(idat);
    if (inflated != raw_len) {
        heap_caps_free(raw);
        return ESP_FAIL;
    }

    size_t    thumb_bytes = (size_t)target_w * target_h * sizeof(uint16_t);
    uint16_t* thumb        = heap_caps_malloc(thumb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (thumb == NULL) thumb = heap_caps_malloc(thumb_bytes, MALLOC_CAP_8BIT);
    if (thumb == NULL) {
        heap_caps_free(raw);
        return ESP_ERR_NO_MEM;
    }

    uint8_t* prev = heap_caps_calloc(1, row_bytes, MALLOC_CAP_8BIT);
    uint8_t* cur  = heap_caps_malloc(row_bytes, MALLOC_CAP_8BIT);
    if (prev == NULL || cur == NULL) {
        heap_caps_free(prev);
        heap_caps_free(cur);
        heap_caps_free(raw);
        heap_caps_free(thumb);
        return ESP_ERR_NO_MEM;
    }

    for (uint32_t y = 0; y < height; y++) {
        uint8_t const  filter = raw[y * (row_bytes + 1)];
        uint8_t const* src    = raw + y * (row_bytes + 1) + 1;
        for (size_t x = 0; x < row_bytes; x++) {
            uint8_t left    = x >= (size_t)channels ? cur[x - channels] : 0;
            uint8_t up      = prev[x];
            uint8_t up_left = x >= (size_t)channels ? prev[x - channels] : 0;
            switch (filter) {
                case 0: cur[x] = src[x]; break;
                case 1: cur[x] = (uint8_t)(src[x] + left); break;
                case 2: cur[x] = (uint8_t)(src[x] + up); break;
                case 3: cur[x] = (uint8_t)(src[x] + ((uint16_t)left + up) / 2); break;
                case 4: cur[x] = (uint8_t)(src[x] + paeth(left, up, up_left)); break;
                default:
                    heap_caps_free(prev);
                    heap_caps_free(cur);
                    heap_caps_free(raw);
                    heap_caps_free(thumb);
                    return ESP_FAIL;
            }
        }

        for (int oy = 0; oy < target_h; oy++) {
            uint32_t sample_y = (uint32_t)((uint64_t)oy * height / (uint32_t)target_h);
            if (sample_y != y) continue;
            for (int ox = 0; ox < target_w; ox++) {
                uint32_t       x  = (uint32_t)((uint64_t)ox * width / (uint32_t)target_w);
                uint8_t const* px = cur + (size_t)x * (size_t)channels;
                uint8_t        r = 0, g = 0, b = 0;
                if (color_type == 0 || color_type == 4) {
                    r = g = b = px[0];
                } else if (color_type == 2 || color_type == 6) {
                    r = px[0];
                    g = px[1];
                    b = px[2];
                } else if (color_type == 3) {
                    pax_col_t c = px[0] < palette_len ? palette[px[0]] : 0xffff00ff;
                    r           = (uint8_t)(c >> 16);
                    g           = (uint8_t)(c >> 8);
                    b           = (uint8_t)c;
                }
                thumb[oy * target_w + ox] = (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
            }
        }
        uint8_t* tmp = prev;
        prev         = cur;
        cur          = tmp;
    }

    heap_caps_free(prev);
    heap_caps_free(cur);
    heap_caps_free(raw);

    if (!pax_buf_init(out, thumb, target_w, target_h, PAX_BUF_16_565RGB)) {
        heap_caps_free(thumb);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t kodi_thumbnail_fetch(char const* path, int max_w, int max_h, pax_buf_t* out_thumb) {
    memset(out_thumb, 0, sizeof(*out_thumb));
    if (path == NULL || path[0] == '\0' || max_w <= 0 || max_h <= 0) return ESP_ERR_INVALID_ARG;

    thumb_cache_lock();
    bool in_memory = thumb_mem_cache_get(path, out_thumb);
    thumb_cache_unlock();
    if (in_memory) return ESP_OK;

    char cache_path[96];
    cache_file_path(path, cache_path, sizeof(cache_path));

    uint8_t*  data       = NULL;
    size_t    len        = 0;
    thumb_cache_lock();
    bool from_cache = read_whole_file(cache_path, &data, &len) == ESP_OK;
    thumb_cache_unlock();

    if (!from_cache) {
        esp_err_t err = kodi_fetch_binary(path, &data, &len);
        if (err != ESP_OK) return err;
    }

    esp_err_t err;
    if (magic_is_jpeg(data, len)) {
        err = decode_jpeg_thumb(data, len, max_w, max_h, out_thumb);
    } else if (magic_is_png(data, len)) {
        err = decode_png_thumb(data, len, max_w, max_h, out_thumb);
    } else {
        err = ESP_ERR_NOT_SUPPORTED;
    }

    // Only cache what we actually downloaded and could decode - never
    // persist a corrupt/partial response.
    if (!from_cache && err == ESP_OK) {
        thumb_cache_lock();
        write_whole_file(cache_path, data, len);
        thumb_cache_unlock();
    }
    if (err == ESP_OK) {
        thumb_cache_lock();
        thumb_mem_cache_put(path, out_thumb);
        thumb_cache_unlock();
    }

    heap_caps_free(data);
    return err;
}

esp_err_t kodi_thumbnail_precache(char const* path) {
    if (path == NULL || path[0] == '\0') return ESP_ERR_INVALID_ARG;

    char cache_path[96];
    cache_file_path(path, cache_path, sizeof(cache_path));

    struct stat st;
    thumb_cache_lock();
    bool already_cached = stat(cache_path, &st) == 0 && st.st_size > 0;
    thumb_cache_unlock();
    if (already_cached) {
        return ESP_OK;  // already cached
    }

    uint8_t*  data = NULL;
    size_t    len  = 0;
    esp_err_t err  = kodi_fetch_binary(path, &data, &len);
    if (err != ESP_OK) return err;

    thumb_cache_lock();
    write_whole_file(cache_path, data, len);
    thumb_cache_unlock();
    heap_caps_free(data);
    return ESP_OK;
}

void kodi_thumbnail_free(pax_buf_t* thumb) {
    if (thumb == NULL || thumb->buf == NULL) return;
    void* pixels = thumb->buf;
    pax_buf_destroy(thumb);
    heap_caps_free(pixels);
    memset(thumb, 0, sizeof(*thumb));
}
