# Chibi simulator

The **real firmware screens running in your browser**. `ui.c`, `app.c`, `character.c` and the
rest are compiled to WebAssembly together with LVGL, so the animations, timers and rules are
exactly the ones on the device.

You can turn the device (buttons or keys **1**–**4**), press **BOOT** / **PWR**, knock ●● ●,
tap and swipe the screen, add tasks, change the weather and battery, and fast-forward the
study timer.

## Run it

Browsers don't load WebAssembly from a file opened by double-clicking, so serve the folder:

```bash
cd simulator
python3 -m http.server 8000
```

Then open <http://localhost:8000>.

**Tip:** if you turn on GitHub Pages for this repository, the simulator also works online at
[juncho26.github.io/Chibi-Pomodoro/simulator](https://juncho26.github.io/Chibi-Pomodoro/simulator/).

## Rebuild it after changing the firmware

You need Python 3, git, and the Zig compiler, which is installed through pip. The script uses
the LVGL that ESP-IDF downloaded for the firmware, or clones LVGL 9.2.2 into `simulator/lvgl`
the first time.

```bash
pip install ziglang
cd simulator
./build.sh
```

This writes a new `chibi.wasm`. Reload the page to see your change.

## Files

| File | What it is |
|---|---|
| `index.html` | The simulator page: the device, buttons, phone page stand-in and controls. |
| `chibi.wasm` | The compiled firmware screens (rebuilt by `build.sh`). |
| `platform.c` | Stands in for the hardware: clock, motion sensor, buttons, battery, flash and Wi-Fi. |
| `runtime.js` | Loads `chibi.wasm` in a browser or in Node.js (handy for automated tests). |
| `shim/` | Minimal stand-ins for the ESP-IDF headers the firmware includes. |
