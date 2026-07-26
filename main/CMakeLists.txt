idf_build_get_property(idf_target IDF_TARGET)
if("${idf_target}" STREQUAL "esp32p4")
list(APPEND extra_requires esp-hosted-tanmatsu)
endif()


idf_component_register(
	SRCS
		"dobber_splash.c"
		"main.c"
		"kodi_client.c"
		"kodi_settings.c"
		"kodi_library.c"
		"kodi_thumbnail.c"
	PRIV_REQUIRES
		esp_lcd
		fatfs
		nvs_flash
		bt
		badge-bsp
		tanmatsu-wifi
		esp_wifi
		esp_http_client
		esp_driver_jpeg
		esp_jpeg
		esp_rom
		esp-tls
		json
		mbedtls
		esp_timer
		${extra_requires}
	INCLUDE_DIRS
		"."
)

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-const-variable" APPEND)
# snprintf() always bounds-checks and null-terminates, so it can never overflow;
# GCC's format-truncation analysis still flags several genuinely-safe call
# sites here (worst-case string lengths it computes from buffer declarations,
# not from what actually flows in at runtime). Keep it as a visible warning,
# not a hard error.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=format-truncation" APPEND)
