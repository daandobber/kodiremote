#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KODI_SETTINGS_HOST_MAX 64
#define KODI_SETTINGS_USER_MAX 32
#define KODI_SETTINGS_PASS_MAX 32

typedef struct {
    char     host[KODI_SETTINGS_HOST_MAX];
    uint16_t port;
    char     username[KODI_SETTINGS_USER_MAX];
    char     password[KODI_SETTINGS_PASS_MAX];
    uint16_t display_sleep_seconds;  // 0 disables automatic display sleep
} kodi_settings_t;

// Loads settings from NVS into 'out'. If nothing is stored yet, 'out' is zeroed
// and port defaults to 8080; returns ESP_ERR_NVS_NOT_FOUND in that case.
esp_err_t kodi_settings_load(kodi_settings_t* out);

// Persists settings to NVS.
esp_err_t kodi_settings_save(const kodi_settings_t* settings);

#ifdef __cplusplus
}
#endif
