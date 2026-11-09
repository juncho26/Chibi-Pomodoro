# Third-party notices

Chibi uses the following. Each keeps its own license. The libraries are downloaded by ESP-IDF when
you build (see `firmware/main/idf_component.yml`), and the fonts are included as converted C files.

| What | Component / location | License |
|---|---|---|
| [LVGL](https://lvgl.io) 9.2, graphics library | `lvgl/lvgl` | MIT |
| esp_lcd_axs15231b, screen driver (Espressif) | `espressif/esp_lcd_axs15231b` | Apache-2.0 |
| esp_lcd_touch (Espressif) | `espressif/esp_lcd_touch` | Apache-2.0 |
| mDNS (Espressif) | `espressif/mdns` | Apache-2.0 |
| [ESP-IDF](https://github.com/espressif/esp-idf) (Espressif), not included, needed to build | — | Apache-2.0 |
| [Noto Sans JP](https://fonts.google.com/noto/specimen/Noto+Sans+JP), converted subsets in `firmware/main/fonts/font_jp*.c`, `font_sans*.c`, `font_bold18.c`, `font_title30.c` | `firmware/main/fonts` | SIL Open Font License 1.1 |
| [JetBrains Mono](https://www.jetbrains.com/lp/mono/), converted subsets in `firmware/main/fonts/font_num*.c`, `font_mono12.c` | `firmware/main/fonts` | SIL Open Font License 1.1 |
| [Zig](https://ziglang.org), used only to build the simulator | `pip install ziglang` | MIT |

Online services the device uses (only when you've saved a home Wi-Fi network and set up the
weather):

- [Open-Meteo](https://open-meteo.com): weather forecast and place search. The data is under
  CC BY 4.0, *Weather data by Open-Meteo.com*.
- [ipinfo.io](https://ipinfo.io), [ip-api.com](https://ip-api.com) and [ipapi.co](https://ipapi.co):
  approximate location for "Near me".
- [pool.ntp.org](https://www.ntppool.org): network time, used to set the clock.
