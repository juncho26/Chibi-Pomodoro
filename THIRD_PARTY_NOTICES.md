# Third-party notices

Chibi includes or uses the following. Each keeps its own license.

| What | Where | License |
|---|---|---|
| [LVGL](https://lvgl.io) 9.2.2, graphics library | `firmware/components/lvgl` | MIT (see `LICENCE.txt` there) |
| esp_lcd_axs15231b, screen driver (Espressif) | `firmware/components/esp_lcd_axs15231b` | Apache-2.0 (see `license.txt` there) |
| esp_lcd_touch (Espressif) | `firmware/components/esp_lcd_touch` | Apache-2.0 (see `license.txt` there) |
| mDNS (Espressif) | `firmware/components/mdns` | Apache-2.0 (see `LICENSE` there) |
| [ESP-IDF](https://github.com/espressif/esp-idf) (Espressif), not included, needed to build | — | Apache-2.0 |
| [Noto Sans JP](https://fonts.google.com/noto/specimen/Noto+Sans+JP), converted subsets in `firmware/main/fonts/font_jp*.c`, `font_sans*.c`, `font_bold18.c`, `font_title30.c` | `firmware/main/fonts` | SIL Open Font License 1.1 |
| [JetBrains Mono](https://www.jetbrains.com/lp/mono/), converted subsets in `firmware/main/fonts/font_num*.c`, `font_mono12.c` | `firmware/main/fonts` | SIL Open Font License 1.1 |
| [Zig](https://ziglang.org), not included, used only to build the simulator | — | MIT |

Online services the device uses (only when you've saved a home Wi-Fi network and set up the
weather):

- [Open-Meteo](https://open-meteo.com): weather forecast and place search. The data is under
  CC BY 4.0, *Weather data by Open-Meteo.com*.
- [ipinfo.io](https://ipinfo.io) and [ipapi.co](https://ipapi.co): approximate location for
  "Near me".
- [pool.ntp.org](https://www.ntppool.org): network time, used to set the clock.
