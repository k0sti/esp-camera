# esp-camera

ESP32-S3 camera that captures a JPEG and uploads it to a server whenever it is
triggered: by a button press, a sensor signal on a GPIO, or an HTTP request.
Includes a small Go server that receives and saves the images, and a test tool
that measures capture and upload throughput at every resolution.

```
 button (GPIO0) ─┐
 sensor (GPIO14) ┼─► ESP32-S3 + OV5640 ──HTTP POST image/jpeg──► Go server ──► images/<device>/<time>_<trigger>.jpg
 GET /capture ───┘
```

## Hardware

- **Board:** GOOUUU ESP32-S3-CAM v1.3 (ESP32-S3 N16R8: 16 MB flash, 8 MB octal PSRAM).
  Other ESP32-S3 camera boards with the ESP32-S3-EYE / Freenove pinout should
  work as is; otherwise edit [`main/board_pins.h`](main/board_pins.h).
- **Camera:** OV5640 or OV3660 (tested); other sensors supported by
  [esp32-camera](https://github.com/espressif/esp32-camera) are auto-detected.
- Connect USB to the **TTL** port (CH340, shows up as `/dev/ttyUSB0`).

## Quick start

Requires [Nix](https://nixos.org) with flakes. The dev shell provides ESP-IDF
v5.5.2 (via [nixpkgs-esp-dev](https://github.com/mirrexagon/nixpkgs-esp-dev)) and Go.

1. **Wi-Fi settings.** Create `wifi.txt` in the project root (it is git-ignored
   and embedded into the firmware at build time):

   ```
   SSID:your-network
   passwd:your-password
   server:http://192.168.1.10:8080/upload
   ```

   The `server:` line is optional. Without it the URL from `main/Kconfig.projbuild`
   is used (change it with `idf.py menuconfig` → *Camera uploader*).

2. **Build and flash:**

   ```bash
   nix develop -c idf.py build flash monitor
   ```

   The port defaults to `/dev/ttyUSB0`; override with `ESPPORT=/dev/ttyUSB1`.
   The log shows the board's IP address once Wi-Fi is connected.

3. **Run the server** (on the machine at the upload URL):

   ```bash
   nix develop -c sh -c 'cd server && go run . -dir ../images'
   ```

   Port 8080 must be reachable from the board. On NixOS, for example:
   `networking.firewall.allowedTCPPorts = [ 8080 ];`

4. **Trigger a capture:** press **BOOT** on the board, pull GPIO14 high, or run
   `curl http://<board-ip>/capture`. Browse saved images at
   `http://localhost:8080/images/`.

## Board HTTP API

| Endpoint | Description |
|---|---|
| `GET /capture` | Capture at the current resolution and upload. Returns `{"ok":true,"bytes":…,"server_status":201}`. |
| `GET /sizes` | Resolutions the sensor supports, and the current one. |
| `GET /burst?size=UXGA&count=5&upload=1` | Capture `count` images back to back at `size` and report timings. `upload=0` measures the camera alone. Restores the previous resolution afterwards. |

Uploads are `POST <server_url>` with body `image/jpeg` and headers
`X-Device-Id` (Wi-Fi MAC) and `X-Trigger` (`button`, `sensor`, `http`, `test-<SIZE>`).

## Server

`server/main.go` — Go standard library only.

| Flag | Default | |
|---|---|---|
| `-addr` | `:8080` | listen address |
| `-dir` | `images` | where images are saved |
| `-max-size` | 10 MiB | largest accepted upload |

Uploads are checked to be JPEGs, device and trigger names are sanitised before
use in paths, and files are written atomically.

## Testing

From `server/`, with the server running:

```bash
go test ./...                                  # server unit tests
go run ./cmd/camtest -upload=false -sizes all  # camera speed at every resolution (~30 s)
go run ./cmd/camtest                           # capture + upload at 5 sizes, verifies saved files (~15 s)
```

`camtest` flags: `-board http://<board-ip>`, `-images ../images`, `-count 3`,
`-sizes QVGA,VGA,HD,UXGA,QXGA` (or `all`), `-upload=false`.

Example results (OV5640, good Wi-Fi):

| Resolution | JPEG | End-to-end images/s | Upload speed |
|---|---|---|---|
| 320×240 | 8 KB | 16 | 0.2 MB/s |
| 640×480 | 28 KB | 12 | 0.4 MB/s |
| 1280×720 | 79 KB | 11 | 1.0 MB/s |
| 1600×1200 (default) | 434 KB | 3.7 | 2.1 MB/s |
| 2048×1536 (max) | 755 KB | 2.6 | 2.9 MB/s |

See [docs/findings.md](docs/findings.md) for full measurements, what limits
throughput, and known issues.

## Project layout

```
main/            firmware
  main.c         startup: config → camera → Wi-Fi → triggers
  camera.c       sensor init, resolution switching
  wifi.c         wifi.txt parsing, station connect/reconnect
  uploader.c     capture + HTTP POST, burst measurements
  triggers.c     button / sensor GPIO interrupts, HTTP endpoints
  board_pins.h   camera and trigger pin mapping
server/          Go upload server
  cmd/camtest/   hardware throughput test
docs/            findings and measurements
sdkconfig.defaults  PSRAM, console, and network throughput settings
flake.nix        development environment
```

## Known issues

- The image is upside down on the tested board (sensor mounted rotated 180°).
- 2560-pixel-wide OV5640 modes produce corrupt frames, so the largest offered
  size is 2048×1536.
- Throughput depends heavily on 2.4 GHz Wi-Fi conditions.
