# Building Chibi

This guide assumes no prior experience. You can make a working Chibi **without installing any
programming tools**: print the case, then put the ready-made firmware on the board from your
browser (sections 1–2). Section 3 is only for people who want to change the code.

- [1. Parts and the case](#1-parts-and-the-case)
- [2. Put the firmware on the board](#2-put-the-firmware-on-the-board)
- [3. Build it yourself (optional)](#3-build-it-yourself-optional)
- [4. Change the phone page](#4-change-the-phone-page)
- [5. Try changes in the simulator](#5-try-changes-in-the-simulator)

---

## 1. Parts and the case

| Part | Notes |
|---|---|
| **Waveshare ESP32-S3-Touch-LCD-3.49** | [Product page](https://www.waveshare.com/esp32-s3-touch-lcd-3.49.htm). A 640×172 touch screen with an ESP32-S3, motion sensor (QMI8658), real-time clock (PCF85063), battery charging and two buttons, all on one board. No soldering needed. |
| **The back case** | 3D-printed from [`hardware/case/`](../hardware/case/). See [hardware/README.md](../hardware/README.md). |
| **USB-C cable** | For power and for putting the firmware on the board. It has to be a data cable, not a charge-only one. |
| **Battery** (optional) | A single-cell 3.7 V LiPo with the plug that fits the board's battery socket. Without one, Chibi runs on USB power. |

**Which board revision do you have?** Waveshare changed the board in June 2026. Look at the
back of the board: **"Rev1.1" or "V2"** means **V2**, otherwise it's **V1**. You need this in
the next step.

---

## 2. Put the firmware on the board

You only need a computer with **Chrome or Edge**. They can talk to the board over USB from a
web page. Safari and Firefox can't.

1. Download the file for your board from [`prebuilt/`](../prebuilt/):
   - `chibi-board-v2.bin` for V2 / Rev1.1 boards
   - `chibi-board-v1.bin` for V1 boards
2. Plug the board into your computer with the USB-C cable.
3. Open **[espressif.github.io/esptool-js](https://espressif.github.io/esptool-js/)**. It's
   Espressif's official flashing tool, and it runs in the browser.
4. Leave the baud rate as it is and click **Connect**. Pick the port that appears (usually
   called *USB JTAG/serial debug unit*) and click **Connect** again.
   - **Nothing appears, or it fails to connect?** Unplug the board. Hold the **BOOT** button,
     plug the board back in, then let go, and try **Connect** again.
5. Under **Flash Address**, enter **`0x0`** and choose the `.bin` file you downloaded.
6. Click **Program** and wait for it to reach 100 % (about a minute).
7. Unplug the board and plug it back in (or press its reset button). Chibi starts up.

That's it. Stand it up, scan the QR code with your phone, and start adding tasks.
The [user guide](USING.md) explains everything else.

> **Updating later:** do the same steps with the new file. The file covers the whole memory, so
> your saved tasks, settings and Wi-Fi networks are cleared, and Chibi starts fresh with a new
> hotspot password. If you build the firmware yourself (section 3), `idf.py flash` keeps them.

**Prefer the command line?** With Python installed:

```bash
pip install esptool
python -m esptool --chip esp32s3 write_flash 0x0 chibi-board-v2.bin
```

---

## 3. Build it yourself (optional)

You need this only to change the firmware.

### Install ESP-IDF 5.5.3

ESP-IDF is Espressif's toolkit for the ESP32. Chibi is built and tested with **version 5.5.3**.

- **Easiest (Windows, macOS, Linux):** install [VS Code](https://code.visualstudio.com/), add
  the **ESP-IDF** extension, run **ESP-IDF: Configure ESP-IDF Extension**, and choose version
  **v5.5.3**.
- **Windows without VS Code:** the
  [ESP-IDF Windows installer](https://dl.espressif.com/dl/esp-idf/). Pick v5.5.3, then use the
  *ESP-IDF 5.5 CMD* shortcut it creates.
- **macOS / Linux in a terminal:** follow
  [Espressif's guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/get-started/linux-macos-setup.html),
  using `git clone -b v5.5.3 --recursive https://github.com/espressif/esp-idf.git`. Then run
  `./install.sh esp32s3` once and `. ./export.sh` in every new terminal.

### Build and flash

```bash
git clone https://github.com/YOUR-NAME/chibi.git
cd chibi/firmware
idf.py set-target esp32s3     # only the first time
idf.py build
idf.py -p PORT flash monitor  # PORT is e.g. COM5 on Windows or /dev/ttyACM0 on Linux
```

- **V1 board?** Run `idf.py menuconfig`, go to **Chibi › Board revision**, choose **V1**, save
  and build again.
- Everything the build needs is in the repository (LVGL, the screen driver, mDNS), so it
  doesn't download anything.
- To leave the monitor, press **Ctrl + ]**.

### Make a single file to share (like the ones in `prebuilt/`)

```bash
cd build
python -m esptool --chip esp32s3 merge_bin -o chibi-board-v2.bin @flash_args
```

---

## 4. Change the phone page

The page you open from the QR code is one file:
[`firmware/main/web/index.html`](../firmware/main/web/index.html). It's plain HTML, CSS and
JavaScript with no frameworks.

- **To see it without a device**, open the file in your browser. It runs in a preview mode
  with sample tasks.
- **To put it on the device**, rebuild and flash. The build compresses the page and stores it
  inside the firmware automatically.

---

## 5. Try changes in the simulator

The [`simulator/`](../simulator/) folder runs the **real firmware screens** in a web page. That
includes the same C code, animations, timers and rules, with buttons to turn the device, knock,
change the weather and fast-forward timers. It's the quickest way to see a change to
`ui.c`, `app.c` or `character.c` without flashing. See [simulator/README.md](../simulator/README.md).
