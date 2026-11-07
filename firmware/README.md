# Firmware

The code that runs on the board: ESP-IDF 5.5.3 and LVGL 9.2.2.

```bash
cd firmware
idf.py set-target esp32s3   # first time only
idf.py build flash monitor
```

- How to install ESP-IDF, flash and build: [docs/BUILDING.md](../docs/BUILDING.md)
- What each file does and how it fits together: [docs/HOW_IT_WORKS.md](../docs/HOW_IT_WORKS.md)

The libraries are listed in [`main/idf_component.yml`](main/idf_component.yml). ESP-IDF
downloads them into `managed_components/` on the first build, so that first build needs an
internet connection:

| Library | What | License |
|---|---|---|
| `lvgl/lvgl` 9.2 | Graphics | MIT |
| `espressif/esp_lcd_axs15231b` | Driver for the screen's AXS15231B controller | Apache-2.0 |
| `espressif/esp_lcd_touch` | Touch driver interface | Apache-2.0 |
| `espressif/mdns` | Announces the device as `chibi.local` | Apache-2.0 |
