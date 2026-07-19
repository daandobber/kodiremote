# Kodi Remote for Tanmatsu

A native Tanmatsu app that turns the badge into a remote control for
[Kodi](https://kodi.tv/), talking directly to Kodi's built-in JSON-RPC API
over your local WiFi network. No cloud, no pairing, no companion app on the
Kodi side beyond what Kodi already ships with.

## Features

- Directional remote control (up/down/left/right/select/back/home/context menu)
  mapped onto the Tanmatsu keyboard's navigation keys.
- Playback control: play/pause, stop, next/previous, seek forward/backward
  (small and big steps).
- Volume up/down on the side buttons, plus mute toggle, control Kodi's volume
  directly.
- Rich now-playing screen with cover art, title, artist/show, album or
  year/genre, a smoothly advancing timeline, elapsed/total/remaining time and
  playback/volume state. Artwork loads in the background, so the remote stays
  responsive while a cover is being downloaded or decoded.
- On-device library browser: Movies, TV Shows (season/episode drill-down) and
  Music (artist/album) fetched straight from Kodi's library, with a press of
  Enter starting playback. Two-pane layout: scrollable list on the left, a
  large cover/poster (decoded on-device, JPEG and PNG, aspect ratio
  preserved) plus the plot/description of the highlighted item on the right,
  updated as you move the selection. Loaded on a background task so
  navigating the list never blocks. Both the list itself (titles, seasons,
  episodes, plots) and the cover art are cached in the badge's own RAM (this
  device has 32MB of PSRAM), so revisiting something already seen — or
  anything covered by a Download Media pass — is instant for the rest of the
  session, no network round trip. This cache is cleared on app
  restart/reinstall (by design — it's meant to avoid re-hitting Kodi while
  you're actively browsing, not to survive a reboot).
- Type-to-Kodi screen: type on the Tanmatsu's keyboard and send the text into
  whatever field Kodi currently has focused (e.g. a search box).
- Download Media screen: walk the whole library (or just movies / just TV
  shows / just music) ahead of time — this both warms the in-RAM list cache
  above for that whole category (so Library browsing needs no further
  network calls for it this session) and pre-caches cover art to the SD card
  (`/sd/apps/kodiremote/thumbcache/`, since there can be far more images than
  comfortably fit in RAM at once). Live progress bar and title-by-title
  label, cancellable at any point. Runs on a background task; browsing the
  Library screen at the same time works fine.
- Power menu: shutdown, reboot, hibernate or suspend the machine running
  Kodi, or just quit Kodi.
- Host, port and optional HTTP basic-auth credentials are entered on-device
  and stored in NVS, so they survive reboots.
- Configurable automatic display sleep (`Off`, `30`, `60` or `90` seconds).
  Kodi polling continues with the backlight off; the first key only wakes the
  screen and is not sent to Kodi.

## Requirements

- A Tanmatsu (or Konsool) badge connected to the same WiFi network as your
  Kodi instance. WiFi credentials are configured once system-wide (the
  launcher's settings app), this app just uses whatever network the badge is
  already connected to.
- Kodi with its webserver enabled: **Settings > Services > Control** and turn
  on "Allow remote control via HTTP". Note the port (default `8080`) and, if
  you set a username/password there, enter the same in this app's Settings
  screen.

## Controls

| Screen | Key | Action |
| --- | --- | --- |
| Any | `F1` | Exit app, return to launcher |
| Menu | Up/Down, Enter | Navigate / open menu item |
| Remote | Arrow keys | Kodi directional navigation |
| Remote | Enter | Select |
| Remote | Esc / Backspace | Back |
| Remote | Home | Kodi home |
| Remote | Menu | Context menu |
| Remote | Space | Play/Pause |
| Remote | `S` | Stop |
| Remote | `M` | Mute toggle |
| Remote | `N` / `P` | Next / previous item |
| Remote | `,` / `.` | Seek small step back / forward |
| Remote | `[` / `]` | Seek big step back / forward |
| Remote | `I` | Show info |
| Remote | `O` | Show OSD |
| Remote | `T` | Open Type-to-Kodi screen |
| Remote | `F2` | Back to menu |
| Remote | `F3` | Force-refresh status |
| Any | Side `+` / `-` | Volume up / down (always active) |
| Library | Up/Down | Select item |
| Library | Enter | Open (drill down) or play |
| Library | Esc / Backspace | Back one level (or to menu at the root) |
| Library | `F2` | Jump straight back to menu |
| Type | type | Edit text |
| Type | Enter | Send text to Kodi and return to Remote |
| Type | `F2` / Esc | Cancel, back to Remote |
| Download | Up/Down | Select what to download |
| Download | Enter | Start (or, on the "Back" row, return to menu) |
| Download | Esc / `F2` (while running) | Cancel the current download |
| Download | any key (when finished) | Dismiss the result, back to the picker |
| Settings | type | Edit selected field (host/port/user/password) |
| Settings | Up/Down | Switch field |
| Settings | Left/Right | Change display sleep when that field is selected |
| Settings | Enter | Next field, or save on the last field |
| Settings | `F2` | Cancel, back to menu |

## Building and installing

See [COMMANDS.md](COMMANDS.md) for the exact ESP-IDF build commands and
[install-badgelink.ps1](install-badgelink.ps1) for pushing the built firmware
to a Tanmatsu over USB via BadgeLink. [PROJECT_MAP.md](PROJECT_MAP.md) has an
overview of where things live in this repository, and
[HARDWARE.md](HARDWARE.md) is a Tanmatsu hardware reference.

## How it talks to Kodi

All communication is plain HTTP POST requests to `http://<host>:<port>/jsonrpc`
using [Kodi's JSON-RPC API](https://kodi.wiki/view/JSON-RPC_API), the same
protocol used by the official Kodi mobile remotes (`Input.*`, `Player.*`,
`Application.*`, `GUI.ActivateWindow`, `System.*`, `VideoLibrary.*`,
`AudioLibrary.*`). See [main/kodi_client.h](main/kodi_client.h) and
[main/kodi_library.h](main/kodi_library.h) for the exact set of calls
implemented.

Thumbnails are fetched as plain `GET http://<host>:<port>/image/<percent
encoded thumbnail url>` requests (the same endpoint Kodi's own web interface
uses) and decoded on-device with the ESP32-P4's JPEG decoder (`esp_jpeg`) or a
small built-in PNG decoder, straight down to a small fixed size — see
[main/kodi_thumbnail.h](main/kodi_thumbnail.h).

## License

The contents of this repository may be considered in the public domain or
[CC0-1.0](https://creativecommons.org/publicdomain/zero/1.0) licensed at your
disposal, consistent with the
[Nicolai-Electronics/tanmatsu-template](https://github.com/Nicolai-Electronics/tanmatsu-template)
this project is built on.
