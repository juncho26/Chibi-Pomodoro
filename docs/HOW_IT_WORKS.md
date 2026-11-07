# How Chibi works

This page is for anyone who wants to read or change the code. All the firmware is in
[`firmware/main/`](../firmware/main/), written in C on **ESP-IDF 5.5.3** with **LVGL 9.2.2**
for graphics.

## The files

| File | What it does |
|---|---|
| `main.c` | Starts everything and runs the screen loop (see below). |
| `app.c` | **The rules**: which screen to show for each position, study and break timers, carry mode, buttons, settings, dimming, sleep and power-off. |
| `ui.c` | **The screens**: building each one, the falling clock digits, the task list, slashes, combos and the ultimate, rain and snow, and deciding exactly which parts of the screen need redrawing. |
| `character.c` | **The character**: its movement, expressions (happy, sleepy, determined…), blinking, umbrella and stars. It's all drawn from simple shapes, with no images. |
| `display.c` | The screen and touch hardware: LVGL setup, rotation, and sending pixels to the panel. |
| `imu.c` | The motion sensor (QMI8658): which way the device is facing, knock detection, wake on motion. |
| `power.c` | Buttons, battery level and charging, and keeping the board powered on battery (see "Power latch" below). |
| `rtc.c` | The real-time clock chip (PCF85063), so the time survives sleep and power-off. |
| `store.c` | Saves tasks, settings, the current session, Wi-Fi networks and calibration to flash (NVS). |
| `net.c` | Wi-Fi (own hotspot or your home network), the sign-in page trick, the phone page and its API. |
| `weather.c` | Fetches today's rain forecast from Open-Meteo about once an hour. |
| `textfit.c` | Works out where a task name gets "..." on the device, so the phone's preview matches exactly. |
| `icons.c` | The small icons (check, pause, battery, Wi-Fi…), drawn from shapes. |
| `fonts/` | The fonts, converted for LVGL (JetBrains Mono for numbers, Noto Sans JP for text). |
| `web/index.html` | The phone page: one file with HTML, CSS and JavaScript. |
| `board.h` | Which pin is wired to what, for V1 and V2 boards. |

## The screen loop

`main.c` runs one loop on its own task:

1. `ui_frame()`: update the rules (`app.c`), the character and the animations by the time
   that has passed.
2. `lv_timer_handler()`: LVGL's own timers (touch input, animations). LVGL's automatic redraw
   timer is kept paused here, so a frame is never drawn twice.
3. `ui_render_prep()`: work out the **smallest areas** of the screen that changed. The
   character, every raindrop, the battery icon and the digits each report their exact box, and
   nothing else is redrawn.
4. `lv_refr_now()`: LVGL draws just those areas.

The loop runs at up to **60 frames per second** while something moves, **20** when nothing has
changed for 0.4 s, and **30** when the screen is dimmed. A frame where nothing changed costs
almost nothing.

## Getting pixels to the panel

The panel (an AXS15231B controller, 172×640, on a 40 MHz QSPI bus) is mounted portrait, but
most screens are landscape. `display.c` does this:

1. LVGL draws into a full frame buffer in PSRAM, in the orientation of the current screen.
2. Only the changed areas are **rotated and copied** into one of two transfer buffers.
3. A separate task on the other CPU core sends the buffer to the panel in 64-row chunks, while
   the next frame is being drawn.
4. This panel can't start a transfer in the middle of the screen, so each frame sends rows
   from the top down to the **lowest changed row**. Changes near the top are cheap to send.

There are timeouts and recovery on every transfer, so a glitch on the bus can't freeze the UI.
A watchdog restarts the device if the UI ever stops responding for 8 seconds.

## Sensors and timing

- **Orientation:** the accelerometer is read 100 times a second. A new position counts after
  it has been steady for 0.45 s. **Settings › Direction** records the real gravity direction
  of each position, so any mounting works.
- **Knocks:** a knock is a short, sharp spike on top of gravity. It has to start suddenly, be
  over within 0.12 s, and not be too hard (dropping it doesn't count). The device must also
  not tilt or turn during the pattern. The ●● ● rhythm is checked from the spike times.
- **Clock:** the PCF85063 keeps time. It's set from your phone when you open the page, or from
  the internet when the device joins your home Wi-Fi.

## Network

- **No home Wi-Fi saved, or none in range:** Chibi makes its own network `Chibi-XXXX` with a
  random password (shown inside the QR code). Its tiny DNS server answers *every* name with
  `192.168.4.1`. A phone's "do I have internet?" check therefore lands on Chibi's web server,
  which redirects it to the page, and the phone shows it as a sign-in page.
- **Home Wi-Fi:** Chibi joins it, announces itself as `chibi.local`, and the QR code contains
  `http://<address>/?t=<key>`. The key is random and saved on the device, and every API call
  needs it.
- **API** (JSON over HTTP, used by the phone page):

  | Endpoint | Does |
  |---|---|
  | `GET /api/state` | Tasks, settings, Wi-Fi and weather status |
  | `POST /api/tasks` · `PATCH /api/tasks/<id>` · `DELETE /api/tasks/<id>` | Add, change (name or done) and delete tasks |
  | `POST /api/order` | Reorder tasks |
  | `POST /api/settings` | Settings such as the task text size (`preview` shows it without saving) |
  | `POST /api/time` | Set the clock from the phone |
  | `POST /api/wifi` | Add a network, change its password, or remove it |
  | `GET /api/scan` | Nearby Wi-Fi networks (only scans when asked) |
  | `POST /api/weather` | The forecast place, or turn the forecast off |
  | `GET /api/fit` | Where each task name is cut with "..." at each text size |
  | `GET /api/diag` | Diagnostics: boot reasons, battery, display counters |

- **Battery:** Wi-Fi is the biggest power cost after the screen, so it turns off when nobody
  is using it. See [USING.md](USING.md#the-phone-page).

## Power latch (why `power.c` is careful)

On this board, the battery stays connected only while pin EXIO6 (SYS_EN) of the TCA9554 I/O
expander is an output driven high. Espressif's stock `esp_io_expander_tca9554` driver resets
the expander's direction register when it starts. That releases SYS_EN and **switches the
board off on every reset or wake-up when running on battery**.

So this firmware never resets the expander. It reads the real registers, sets the SYS_EN
output latch high first, and only then makes the pin an output. A guard re-checks it every
2 seconds. The last 8 boot reasons are kept in flash and shown at `/api/diag`.

## Saving

Everything is stored in NVS (the ESP32's key-value flash storage). Saving is grouped, so the
flash isn't worn out: settings and tasks are saved shortly after they change, and the running
session every 2 minutes. Newer firmware versions read older saved data, and new fields get
their defaults.

## Testing without hardware

The [simulator](../simulator/) compiles `ui.c`, `app.c`, `character.c`, `icons.c`, `store.c`,
`textfit.c` and the fonts, together with LVGL, to WebAssembly. A small layer
(`simulator/platform.c`) stands in for the clock, sensors, buttons and flash. Because it's the
same code, what you see there is what the device does. It's how most of the animation and
battery tuning was checked.
