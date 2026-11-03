# ESP32-S3 camera: findings

Notes from bringing up the board, building the Wi-Fi capture/upload system and
measuring throughput (2026-11-03).

## Hardware

| | |
|---|---|
| Board | GOOUUU ESP32-S3-CAM v1.3 |
| Module | ESP32-S3 N16R8: 16 MB flash, 8 MB octal PSRAM (AP, 3.3 V), rev v0.2 |
| USB | **TTL** port = CH340 USB-serial (`1a86:7523`) → `/dev/ttyUSB0`. Use this one for flashing and logs. **OTG** port = native USB (would appear as `303a:xxxx`, `/dev/ttyACM*`). |
| Buttons | **BOOT** = GPIO0 (used as the capture button), **RST** = reset |
| Camera | Started with an OV3660 (3 MP); swapped to an **OV5640** (5 MP). Both answer on SCCB address `0x3C` and are auto-detected — no code change needed when swapping. |
| Factory firmware | Espressif `led_strip` demo (ESP-IDF v4.4-dev) — doesn't identify the board. |

Camera wiring is the common ESP32-S3-EYE / Freenove layout and was right on the
first try (`main/board_pins.h`):

| Signal | GPIO | Signal | GPIO |
|---|---|---|---|
| XCLK | 15 | D7–D4 | 16, 17, 18, 12 |
| SIOD / SIOC | 4 / 5 | D3–D0 | 10, 8, 9, 11 |
| VSYNC / HREF / PCLK | 6 / 7 / 13 | PWDN / RESET | not connected |

The sensor input trigger is on GPIO14 (rising edge, pulled down).

## What was built

- **Firmware** (ESP-IDF v5.5.2, `espressif/esp32-camera` 2.1.8): connects to
  Wi-Fi, captures a JPEG and POSTs it to the server on any trigger:
  - BOOT button (1 s debounce)
  - GPIO14 rising edge
  - `GET http://<board>/capture`
  - Test endpoints: `GET /sizes`, `GET /burst?size=UXGA&count=5&upload=0|1`
- **Server** (`server/`, Go, standard library only): `POST /upload` saves to
  `images/<device-mac>/<timestamp>_<trigger>.jpg`; `GET /images/` to browse.
  Validates JPEG, limits size, sanitises names. Unit tests in `main_test.go`.
- **Test tool** (`server/cmd/camtest`): runs bursts, verifies every saved file
  decodes at the right resolution, prints a throughput table.
- **Config**: `wifi.txt` (`SSID:` / `passwd:` lines, optional `server:`),
  embedded at build time, git-ignored. Server URL defaults to
  `http://192.168.1.206:8080/upload` (`main/Kconfig.projbuild`).
- **Environment**: `flake.nix` (nixpkgs-esp-dev + Go). `nix develop -c idf.py build flash`.

## Throughput

### Camera alone (no upload)

| Size | Resolution | Avg JPEG | Camera fps |
|---|---|---|---|
| 96X96 … HQVGA | ≤ 240×176 | 2–5 KB | ~20 |
| QVGA / CIF / HVGA | 320×240 – 480×320 | 8–14 KB | 22–25 |
| VGA / SVGA | 640×480 / 800×600 | 29 / 44 KB | 22 |
| XGA / HD | 1024×768 / 1280×720 | 71 / 77 KB | 19 / 26 |
| SXGA | 1280×1024 | 319 KB | 4.7 |
| **UXGA (default)** | 1600×1200 | 437 KB | 5.6 |
| FHD | 1920×1080 | 452 KB | 5.9 |
| QXGA (max offered) | 2048×1536 | 741 KB | 4.5 |

Frame rate drops sharply above 1280×720, most likely because the OV5640
switches from a subsampled readout to full-resolution readout there.
Measurements use 3 frames per size, so expect ±20 % noise.

### End to end (capture + Wi-Fi upload), good Wi-Fi conditions

| Size | Upload/img | Images/s | Upload speed | Limited by |
|---|---|---|---|---|
| QVGA 320×240 | 40 ms | 16 | 0.2 MB/s | camera |
| VGA 640×480 | 64 ms | 12 | 0.4 MB/s | upload |
| HD 1280×720 | 76 ms | 11 | 1.0 MB/s | upload |
| UXGA 1600×1200 | 212 ms | 3.7 | 2.1 MB/s | upload ≈ camera |
| QXGA 2048×1536 | 262 ms | 2.6 | 2.9 MB/s | upload ≈ camera |

### What limits throughput

1. **ESP-IDF network defaults (fixed).** Initially uploads ran at ~25 KB/s
   (a UXGA image took ~12 s). Cause: 5.7 KB TCP send buffer, small Wi-Fi
   queues, and Wi-Fi modem sleep adding 100+ ms per round trip.
   Fix: `esp_wifi_set_ps(WIFI_PS_NONE)` plus the iperf-example buffer
   settings in `sdkconfig.defaults` (64 KB TCP window, larger Wi-Fi buffers in
   PSRAM, AMPDU windows of 32) → **0.3–2.9 MB/s** depending on link
   quality, up from ~25 KB/s.
2. **2.4 GHz link quality (environmental).** The board is on channel 11 of a
   busy network (the laptop uses 6 GHz to the same router). Board ping swings
   between ~10 ms and 500+ ms; during bad spells uploads slow 10× or time out
   (10 s). The ESP32-S3 has 2.4 GHz only. This is the biggest source of
   variance in all measurements.
3. **Sequential pipeline.** Capture and upload run one after the other, and
   each upload opens a new TCP connection (~40–70 ms fixed cost). At large
   sizes camera and upload times are about equal, so overlapping them could
   nearly double throughput.
4. **Camera readout** caps at ~20–25 fps (small sizes) and ~5 fps above 1280×720.

## Problems found

| Problem | Cause | Status |
|---|---|---|
| Uploads timed out, board fine | NixOS firewall drops inbound port 8080 | Opened manually (`iptables`). For permanent: `networking.firewall.interfaces.wlo1.allowedTCPPorts = [ 8080 ];`. Mullvad may also block LAN traffic. |
| Image upside down | Module mounted rotated 180° | **Not fixed** — needs `set_vflip` + `set_hmirror` |
| Faint vertical stripes (OV5640) | Sensor column fixed-pattern noise: identical column pattern across different scenes (correlation 0.92–0.97); likely made visible by high gain indoors and downscaling | Ignored by decision |
| Washed out / purple tint (OV5640) | Auto exposure / white balance under mixed indoor light | Not tuned |
| Blurry first images (OV3660) | Lens focus out of the box | Can be adjusted by turning the lens |
| QXGA frames occasionally lost (`NO-EOI`) | Driver's JPEG buffer guess (w×h/5) too small for detailed scenes | Fixed: `jpeg_buffer_size = 1 MB` |
| 2560-wide sizes (QHD, WQXGA, QSXGA) never work (`NO-EOI`) | Data arrives corrupted, not a buffer-size problem; likely CPU copy into PSRAM can't keep up | **Dropped** — max size capped at QXGA |
| `CONFIG_CAMERA_PSRAM_DMA=y` | Breaks capture at every size | Don't use |
| A few `NO-SOI` frames at boot | Camera running while Wi-Fi starts | Harmless |

## Build / tooling gotchas

- **Nix flakes only see git-tracked files**, and copy them to the
  world-readable Nix store. `wifi.txt` was briefly staged and ended up in two
  store paths; it is now git-ignored. Clean up with `nix store delete` on
  those `-source` paths if not done yet.
- **`sdkconfig.defaults` only applies to options missing from `sdkconfig`.**
  After changing defaults, delete `sdkconfig` and rebuild.
- Console baud rate needs `CONFIG_ESP_CONSOLE_UART_CUSTOM=y`, otherwise
  `CONFIG_ESP_CONSOLE_UART_BAUDRATE` is ignored (stays 115200).
- CPU speed option is `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240`, not `ESP_CPU_FREQ_240` (default is 160 MHz).
- Components must list what they include: `main` needs `esp_psram` in `PRIV_REQUIRES`.
- The camera driver sizes frame buffers at init, so it is initialised at the
  largest size used (QXGA) and switched down to UXGA; `set_framesize` at
  runtime then works for every smaller size.
- The `esp-idf-esp32s3` Nix package is deprecated (removed in ESP-IDF 6.0);
  switch to `esp-idf-xtensa` eventually.
- Host clangd shows false errors for ESP-IDF files (unknown xtensa flags); the real build is clean.

## How to test

From `server/`, with the server running (`go run . -dir ../images`):

```bash
go run ./cmd/camtest -upload=false -sizes all   # camera speed, all 20 sizes, ~30 s
go run ./cmd/camtest                            # upload test, 5 sizes × 3 images, ~15 s
go test ./...                                   # server unit tests
```

## Possible next steps

- Flip the image (`set_vflip`, `set_hmirror`).
- Overlap capture and upload, and reuse one HTTP connection, for up to ~2× at large sizes.
- Tune OV5640 exposure / white balance; cap gain to reduce stripe visibility.
- Remove or lower the per-upload timing log line in `main/uploader.c`.
- Commit the work (nothing committed yet).
