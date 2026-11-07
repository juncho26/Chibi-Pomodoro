# Ready-made firmware

| File | For |
|---|---|
| `chibi-board-v2.bin` | **V2 / Rev1.1** boards (look for "Rev1.1" or "V2" on the back of the board) |
| `chibi-board-v1.bin` | **V1** (original) boards |

Each file is a complete image. Write it at address **`0x0`**. The step-by-step guide, which
works from the browser with nothing to install, is in
[docs/BUILDING.md](../docs/BUILDING.md#2-put-the-firmware-on-the-board).

Flashing one of these files clears saved tasks and settings, so the device starts fresh.

These files were built from the code in [`firmware/`](../firmware/) with ESP-IDF 5.5.3.
The V2 build is the one used and tested on real hardware. The V1 build uses the same code with
the V1 pin and power settings, but hasn't been tested on a V1 board yet. Reports are welcome.
