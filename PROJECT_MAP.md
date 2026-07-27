# Project Map

## Core App

- `main/main.c`: BSP/display/WiFi init, screen state machine (menu, remote
  control, library browser, type-to-Kodi, power, settings), input event
  handling, drawing.
- `main/kodi_client.c`: Kodi JSON-RPC over HTTP client (Input/Player/
  Application/GUI/System calls, status polling, text input) plus the generic
  `kodi_rpc_call()` used by `kodi_library.c` for calls not otherwise wrapped.
  The JSON-RPC connection (`/jsonrpc`) is a single persistent, keep-alive
  `esp_http_client` handle reused across every call instead of opening/closing
  a fresh TCP connection per request (a real source of latency on every
  keypress/status poll); a mutex (`s_rpc_mutex`) serializes access since the
  UI thread, `kodi_command_worker_task` and the download task can all call
  in. Binary image fetches (`kodi_fetch_binary`) still use a fresh client per
  call since they're much less frequent.
- `main/kodi_client.h`: Kodi client public API and `kodi_status_t`.
- `main/kodi_library.c`: VideoLibrary/AudioLibrary JSON-RPC calls (movies, TV
  shows, seasons, episodes, artists, albums), `Player.Open` playback, and
  building each item's `thumb_path` (Kodi's `/image/<percent-encoded url>`
  webserver path) from the `art` property (tries a type-appropriate priority
  of art keys, e.g. poster then thumb for movies/shows/seasons, thumb then
  poster for episodes/albums, since Kodi's flat `thumbnail` field is often
  empty for shows/seasons even when a poster is set) plus `description`
  (plot/biography/review, field name depends on item type).
  `kodi_library.h` also has `kodi_library_get_all_*` variants (paginated,
  uncapped) that fetch the whole library across every show/artist in one go
  (used by the Download Media screen; interactive browsing keeps the capped
  single-page functions above). Every fetch function also goes through an
  **in-RAM (PSRAM), session-lifetime cache** (`list_cache_read`/
  `list_cache_write`/`list_cache_filter`, backed by `g_cache_slots[]` — no SD
  card involved, cleared on app restart): the flat, unscoped calls (movies/
  tvshows/artists) read+write a single cache slot directly; the scoped ones
  (seasons/episodes/albums) only *read* from the bulk "all_seasons"/
  "all_episodes"/"all_albums" slots (written by the `get_all_*` bulk
  variants) and filter client-side by `tvshowid`/`season`/`artistid` fields
  on `kodi_library_item_t` - they never write a partial result into a
  bulk-scoped slot. A 0-match filter is treated as a cache miss and falls
  through to the network, on the assumption a real show/season/artist never
  legitimately has zero children. (An earlier version of this cache lived on
  the SD card; that turned out to not actually help, most likely because
  `/sd` isn't mounted the way that assumed — switched to RAM instead, which
  needs no such assumption and this device has 32MB of it.)
- `main/kodi_library.h`: library browsing API and `kodi_library_item_t`.
- `main/kodi_thumbnail.c`: checks an in-RAM FIFO cache first
  (`thumb_mem_cache_get`/`put`, `THUMB_MEM_CACHE_SLOTS` decoded images,
  PSRAM-backed), then an SD-card cache under `/sd/apps/kodiremote/thumbcache/`
  (works if a card happens to be mounted there, but nothing depends on it),
  then `kodi_fetch_binary()` on a full miss, decoding (JPEG via `esp_jpeg`,
  PNG via a small built-in decoder) fit within a max box with the source
  aspect ratio preserved. `kodi_thumbnail_precache()` is the download-only-
  no-decode variant used by the Download Media screen's bulk pass (SD-cache
  only, since bulk-precaching everything into the RAM cache too would just
  evict itself before you get to browse it).
- `main/kodi_thumbnail.h`: thumbnail fetch/decode API.
- `main/kodi_settings.c`: NVS-backed storage for host/port/username/password.
- `main/kodi_settings.h`: settings struct and load/save API.
- `main/CMakeLists.txt`: component sources and requirements.
- `main/idf_component.yml`: managed ESP-IDF component dependencies.

## Scripts

- `install-badgelink.ps1`: build/install/start app through BadgeLink.

## Important Generated Or External Folders

- `build/tanmatsu/`: build output (contains `application.bin`).
- `managed_components/`: ESP-IDF managed components (badge-bsp, pax-gfx,
  tanmatsu-wifi, ...), fetched automatically on first build.
- `badgelink_v020/`: BadgeLink tooling, clone with:
  `git clone https://github.com/badgeteam/esp32-component-badgelink.git badgelink_v020`
- `esp-idf/`, `esp-idf-tools/`: ESP-IDF checkout and toolchain, see
  COMMANDS.md for setup.

## App Behavior

- Menu screen: Remote control / Library / Download media / Power / Settings.
- Remote screen: cover art, media details, playback/volume state and a smooth
  elapsed/remaining timeline. Status is polled from Kodi every ~2 seconds;
  cover art is fetched and decoded on a dedicated background worker so the
  controls stay responsive. `T` opens the Type-to-Kodi screen.
- Library screen: root choice of Movies / TV Shows / Music, then drills down
  (TV Shows > Seasons > Episodes, Music > Artists > Albums) via a small
  navigation stack in `main.c`; Enter on a leaf item calls `Player.Open` and
  jumps to the Remote screen. Both the list metadata (`kodi_library.c`'s
  in-RAM cache) and the cover art (`kodi_thumbnail.c`'s in-RAM + SD cache)
  are cached, so once something has been fetched once (by browsing it, or by
  running Download Media) revisiting it in the same session is instant,
  without touching Kodi's network API again. This cache does not survive an
  app restart/reinstall - it is deliberately RAM-only for the list data, so
  there is nothing to go stale on disk when the Kodi library changes.
  Two-pane layout below the root level: item list on the left,
  a single large preview (cover art + word-wrapped plot/description) for
  whichever row is currently selected on the right. The preview image is
  fetched on a dedicated background FreeRTOS task (`lib_preview_worker_task`,
  `lib_preview_job_queue`, `lib_preview_mutex` in `main.c`) so a slow Kodi
  image download/decode never blocks navigation — only the latest
  selection's job matters (`xQueueOverwrite`), and stale results are
  discarded via a generation counter (`lib_preview_request_id`).
- Type screen: local text buffer edited with the keyboard, sent to Kodi via
  `Input.SendText` on Enter (whatever Kodi dialog currently has focus, e.g. a
  search box, receives it).
- Download Media screen: user-triggered bulk pre-cache (All / Movies / TV
  Shows / Music) — fetches the *whole* library via `kodi_library_get_all_*`
  and calls `kodi_thumbnail_precache()` per item on its own FreeRTOS task
  (`download_task_fn`), reporting progress through `download_state_mutex`-
  guarded globals in `main.c` so the render loop can show a live bar without
  touching task-local state directly. Esc/F2 sets
  `download_cancel_requested`, checked between items (not instant, but bounded
  by one item's download time). This is a separate, explicit action — there
  is still no automatic "sync everything at startup".
- Power screen: Shutdown / Reboot / Hibernate / Suspend the Kodi host, or
  quit Kodi.
- Settings screen: on-device form for host, port, username, password and
  automatic display sleep (`Off` / `30` / `60` / `90` seconds), persisted to
  NVS under the `kodiremote` namespace. While asleep, Kodi polling continues;
  the first key wakes the backlight without triggering an action.
