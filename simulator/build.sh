#!/bin/bash
# Builds the real firmware UI (needs Python 3 and: pip install ziglang)
# Builds the real firmware UI (LVGL + ui.c + app.c + character.c + icons.c + store.c) to WebAssembly.
set -e
cd "$(dirname "$0")"
ZIG="python3 -m ziglang"
L=../firmware/components/lvgl
CF="-target wasm32-wasi -O2 -DLV_CONF_KCONFIG_EXTERNAL_INCLUDE=\"sdkconfig.h\" -DLV_LVGL_H_INCLUDE_SIMPLE -Ishim -I../firmware/main -I$L -I$L/src -Wno-unused-function -Wno-format-truncation -fno-sanitize=undefined"
mkdir -p obj
SRCS="$(find $L/src -name '*.c' | grep -v -E '/(drivers|osal/(freertos|pthread|cmsis|rtthread|windows|sdl|mqx)|libs/(ffmpeg|freetype|tiny_ttf|libjpeg|libpng|lodepng|rlottie|thorvg|tjpgd|barcode|bmp|fsdrv|gif|libwebp|rle|lz4)|others/(vg_lite|xml)|draw/(nxp|renesas|vg_lite|sdl|opengles|dma2d|nema|nxp)|debugging)' ) ../firmware/main/ui.c ../firmware/main/app.c ../firmware/main/character.c ../firmware/main/icons.c ../firmware/main/store.c ../firmware/main/textfit.c $(ls ../firmware/main/fonts/*.c) platform.c"
OBJS=""
for s in $SRCS; do
  o=obj/$(echo $s | sed 's#[/.]#_#g').o
  OBJS="$OBJS $o"
  if [ ! -f $o ] || [ $s -nt $o ] || [ platform.c -nt $o -a $s = platform.c ]; then
    $ZIG cc $CF -c $s -o $o &
    while [ $(jobs -r | wc -l) -ge 8 ]; do sleep 0.05; done
  fi
done
wait
$ZIG cc -target wasm32-wasi -O2 -mexec-model=reactor -Wl,--export-dynamic -Wl,--no-entry -Wl,--export=malloc -Wl,--export=free -Wl,--initial-memory=33554432 -Wl,--max-memory=67108864 $OBJS -o chibi.wasm
ls -la chibi.wasm
