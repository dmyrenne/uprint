# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

μprint is ESP-IDF firmware (C, one C++ file) that turns an ESP32-S3 into a USB print server: G-code is uploaded
via a browser UI, stored on microSD or internal flash, and streamed over USB host to a Marlin/Prusa printer. An
AxiDraw/NextDraw (EiBotBoard) mode exists in alpha (see `docs/axidraw.md`).

**Language convention:** code comments, log messages, README, commit messages and PR titles are written in
German. Keep it that way. Commit subjects follow the pattern `Bereich: kurze Beschreibung`
(e.g. `Einstellungen: …`, `CI: …`, `Web-UI: …`).

## Build & flash

Requires ESP-IDF v6.1 (CI uses `v6.1`; `idf_component.yml` allows `>=5.3`). Two board variants, each built in
its own directory with its own `sdkconfig` (generated, never committed — edit `sdkconfig.defaults*` instead):

    idf.py -DUPRINT_VARIANT=basic -B build-basic build      # any ESP32-S3 ≥4 MB flash, SD only
    idf.py -DUPRINT_VARIANT=n16r8 -B build-n16r8 build      # 16 MB flash + 8 MB PSRAM, adds internal FAT storage
    idf.py -DUPRINT_VARIANT=basic -B build-basic -p <port> flash monitor

`n16r8` is the default if `UPRINT_VARIANT` is omitted. Variant config = `sdkconfig.defaults` +
`sdkconfig.defaults.<variant>` + `partitions-<variant>.csv`. The console is on UART0 only, because the native
USB port is used as USB host for the printer.

There are no automated tests. For UI work without hardware, `tools/mock_server.py` (gitignored, local only, Python
stdlib) serves `main/www` live and emulates the full API with switchable scenarios:

    python3 tools/mock_server.py            # http://127.0.0.1:8080, --speed 1 for real-time printing

When changing the HTTP API, keep the mock in sync if it exists.

## Architecture

`app_main` (`main/main.c`) initializes in order: NVS → `settings` → `storage` → `wifi` → `usb_serial` →
`printer` → `web`. Each module is a `.c`/`.h` pair with module-level static state; the headers document the
contracts and are the best entry point.

- **printer.c** — single FreeRTOS task that owns the serial link. Web handlers never talk to the printer
  directly: they set request flags (`s_req_start`, `s_req_pause`, …) and read a `printer_status_t` snapshot, all
  under `s_lock`. Protocol: one line in flight, `N<n> cmd*<xor>` with checksum, wait for `ok`, replay from a
  32-line history on `Resend:`. Cancel runs heaters-off plus the shutdown sequence found after the
  `; -- shutdown` marker near the end of the file (written by µplot). When `device_type` is AxiDraw, the same
  task calls into `axidraw.c` instead (`axidraw_link` / `axidraw_poll`).
- **usb_serial.cpp** — USB host with CDC-ACM, CH34x, CP210x and FTDI drivers (C++ because `usb_host_vcp`
  needs exceptions). `usb_serial_readline` must only be called from the printer task; `usb_serial_generation()`
  lets consumers detect reconnects.
- **storage.c** — volumes `STORAGE_SD` and `STORAGE_FLASH` (flash only in n16r8, check `storage_present`).
  Every file access must be wrapped in `storage_acquire`/`storage_release` so a hot-removed SD card isn't
  unmounted mid-access. Only flat filenames in the root directory (`storage_name_valid`). `storage_changed()`
  bumps a revision counter the UI polls.
- **settings.c** — `settings_t` in NVS; each field is commented with when it takes effect (immediately, next USB
  connection, or after reboot → `settings_reboot_required`). Validation lives in `settings_update`.
- **web.c** — `esp_http_server` with wildcard URI matching; the route list is in `web.h`. The UI
  (`main/www/index.html`, `i18n.js`, fonts) is embedded via `EMBED_FILES` in `main/CMakeLists.txt` — new
  assets must be added there. `slicer.c` registers PrusaLink/OctoPrint-compatible endpoints on the same server
  (API key auth, routes listed in `slicer.h`). `captive.c` provides DNS + redirects when accessed via the
  device's own AP. `max_uri_handlers` is fixed in `web_start`; raise it when adding routes.
- **variant.c** — writes a `uprint_desc_t` (magic `UPRINT1` + variant name) right after `esp_app_desc_t` in the
  image; OTA upload checks it so a basic image can't be flashed onto n16r8 and vice versa.
- **Web UI** — a single hand-written `index.html` (vanilla JS/CSS, no build step). All visible strings go
  through `i18n.js` (`de` and `en`; add keys to both).

## Planning & roadmap

Features and bugs are tracked as GitHub issues (`gh issue list`, `gh issue view <n>`); check the matching
issue before starting work, because the issue bodies contain the agreed behavior (locks during printing,
confirmations, settings in the web UI instead of menuconfig, etc.). Labels: `enhancement`, `bug`, `3D-Printer`
(printer-specific), and `minor`/`major`, which also control the version bump when the PR is merged (see below).

Where the project is heading, based on the open issues and branches:
- **Pen plotter support (current focus):** AxiDraw/NextDraw. The test mode shipped in `v0.4.0` (PR #11); plotting
  (G-code → EBB, `main/plot.c`) is on branch `axidraw-plotter` → `v0.5.0`. The plan, the protocol research and the numbered task list are in `docs/axidraw.md`; keep that
  file up to date as tasks get done. Companion project [µplot](https://github.com/dmyrenne/uplot) produces the
  plotter G-code (SVG → µplot → µprint → AxiDraw), so µprint only translates G-code and does no coordinate
  transforms. Issue #12 (detect 2D/3D G-code) belongs to this.
- **Device hardware:** a custom PCB (`PCB/`, separate repo) with hardware buttons (#4), a display (#5) and
  addressable LEDs (#6). All pins and options are meant to be configurable in the web UI.
- **Printer features & UX:** preheat presets (#1), G-code thumbnails (#2), flashing printer firmware (#3),
  factory reset (#13), wiping a whole storage volume (#14), locking settings during a print (#16), a timeout and
  retry for printer connections (#17), MQTT/Home Assistant (#18).

## CI / releases

`.github/workflows/firmware.yml` builds both variants on every push. A push to a branch that changes firmware
paths (`main/`, `CMakeLists.txt`, `dependencies.lock`, `sdkconfig.defaults*`, `partitions-*.csv`) creates a
pre-release `vX.Y.Z-alpha.N`; merging the PR into `main` creates `vX.Y.Z` (patch bump, or `minor`/`major` via PR
label) and deletes the related alphas. `PROJECT_VER` comes from `UPRINT_VERSION` in CI, from `git describe`
locally. `flasher.yml` rebuilds the GitHub Pages web flasher (`flasher/index.html`) from the releases.

`PCB/` (separate repo) and `tools/` are gitignored.
