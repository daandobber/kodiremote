#include "kodi_settings.h"

#include <string.h>

#include "nvs.h"

static char const NVS_NAMESPACE[] = "kodiremote";

esp_err_t kodi_settings_load(kodi_settings_t* out) {
    memset(out, 0, sizeof(*out));
    out->port = 8080;

    nvs_handle_t handle;
    esp_err_t    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return ESP_ERR_NVS_NOT_FOUND;
    }

    size_t host_len = sizeof(out->host);
    nvs_get_str(handle, "host", out->host, &host_len);

    uint16_t port = 8080;
    if (nvs_get_u16(handle, "port", &port) == ESP_OK) {
        out->port = port;
    }

    size_t user_len = sizeof(out->username);
    nvs_get_str(handle, "username", out->username, &user_len);

    size_t pass_len = sizeof(out->password);
    nvs_get_str(handle, "password", out->password, &pass_len);

    nvs_close(handle);

    return out->host[0] != '\0' ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t kodi_settings_save(const kodi_settings_t* settings) {
    nvs_handle_t handle;
    esp_err_t    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    nvs_set_str(handle, "host", settings->host);
    nvs_set_u16(handle, "port", settings->port);
    nvs_set_str(handle, "username", settings->username);
    nvs_set_str(handle, "password", settings->password);

    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
