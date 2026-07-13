#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Player IDs as returned by Kodi (0 = video/picture, 1 = audio in most builds).
// The client asks Kodi for the active player id before sending player commands,
// so callers do not need to guess it.
typedef enum {
    KODI_PLAYER_NONE = -1,
    KODI_PLAYER_VIDEO,
    KODI_PLAYER_AUDIO,
} kodi_player_type_t;

typedef struct {
    bool     playing;             // any player active
    bool     paused;               // active player is paused (speed == 0)
    kodi_player_type_t player_type;
    int      active_player_id;
    char     title[128];
    char     subtitle[128];        // artist / show name / album, whichever applies
    double   percentage;           // 0-100
    int      time_hours;
    int      time_minutes;
    int      time_seconds;
    int      total_hours;
    int      total_minutes;
    int      total_seconds;
    int      volume;               // 0-100
    bool     muted;
} kodi_status_t;

// Configure the client for a given base URL host/port and optional HTTP basic auth.
// Must be called (directly or via kodi_settings) before any request functions are used.
void kodi_client_configure(const char* host, uint16_t port, const char* username, const char* password);

// Returns true if a host has been configured (does not imply reachability).
bool kodi_client_is_configured(void);

// Fetches current player status. Fills 'out' and returns ESP_OK on success.
// When nothing is playing, 'out->playing' is false and the rest of the struct is zeroed.
esp_err_t kodi_get_status(kodi_status_t* out);

// Simple connectivity check: JSONRPC.Ping
esp_err_t kodi_ping(void);

// Input control (directional navigation, selection, back, context menu, info, home)
esp_err_t kodi_input_up(void);
esp_err_t kodi_input_down(void);
esp_err_t kodi_input_left(void);
esp_err_t kodi_input_right(void);
esp_err_t kodi_input_select(void);
esp_err_t kodi_input_back(void);
esp_err_t kodi_input_home(void);
esp_err_t kodi_input_context_menu(void);
esp_err_t kodi_input_info(void);
esp_err_t kodi_input_show_osd(void);

// Playback control, operate on the currently active player
esp_err_t kodi_play_pause(void);
esp_err_t kodi_stop(void);
esp_err_t kodi_play_next(void);
esp_err_t kodi_play_previous(void);
esp_err_t kodi_seek_small_forward(void);
esp_err_t kodi_seek_small_backward(void);
esp_err_t kodi_seek_big_forward(void);
esp_err_t kodi_seek_big_backward(void);

// Volume control. 'increment' true = up, false = down.
esp_err_t kodi_volume_step(bool increment);
esp_err_t kodi_toggle_mute(void);

// Navigate to a built in Kodi window, e.g. "videos", "music", "pictures", "settings", "home"
esp_err_t kodi_activate_window(const char* window_name);

// Power management of the host running Kodi
esp_err_t kodi_system_shutdown(void);
esp_err_t kodi_system_reboot(void);
esp_err_t kodi_system_hibernate(void);
esp_err_t kodi_system_suspend(void);
esp_err_t kodi_quit(void);

// Sends free-form text into whatever Kodi text field currently has focus
// (e.g. a search dialog). 'done' closes the on-screen keyboard if one is open.
esp_err_t kodi_send_text(const char* text, bool done);

// Generic JSON-RPC call for methods not wrapped above (VideoLibrary.*,
// AudioLibrary.*, Player.Open, ...). Takes ownership of 'params' (may be
// NULL). On ESP_OK, if out_root is non-NULL the parsed response is returned
// there and the caller must cJSON_Delete() it.
esp_err_t kodi_rpc_call(const char* method, cJSON* params, cJSON** out_root);

// Downloads whatever is at 'path' (a ready-to-GET path on the Kodi webserver,
// e.g. "/image/image%3a%2f%2f...") using the same host/port/auth as the
// JSON-RPC calls above. On success *out_data (allocated, prefer PSRAM) and
// *out_len are set; caller must free it with kodi_free_binary().
esp_err_t kodi_fetch_binary(const char* path, uint8_t** out_data, size_t* out_len);
void      kodi_free_binary(uint8_t* data);

#ifdef __cplusplus
}
#endif
