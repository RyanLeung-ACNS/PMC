#!/bin/sh
# Builds the host screen render and writes one PNG per scenario to out/.
# Needs gcc and python3 with Pillow. LVGL is compiled once into out/liblvgl.a.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
PROJ="$HERE/../../InitialTesting"
OUT="$HERE/out"
mkdir -p "$OUT/lvgl"
CFLAGS="-O1 -w -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE \
  -I$PROJ/lvgl_940 -I$PROJ/lvgl_940/lv_conf/heavy \
  -I$PROJ -I$PROJ/build-MM4/generated"

if [ ! -f "$OUT/liblvgl.a" ]; then
  echo "Compiling LVGL (once)..."
  find "$PROJ/lvgl_940/src" -name '*.c' | while read -r f; do
    o="$OUT/lvgl/$(echo "$f" | sed "s|$PROJ/lvgl_940/src/||; s|/|_|g").o"
    gcc $CFLAGS -c "$f" -o "$o"
  done
  ar rcs "$OUT/liblvgl.a" "$OUT"/lvgl/*.o
fi

gcc $CFLAGS -DPMC_UI_NEW=0 -o "$OUT/ui_preview" \
  "$HERE/sim_main.c" "$HERE/fake_hw.c" \
  "$PROJ/main_screen.c" "$PROJ/motor_screen.c" "$PROJ/fixed.c" \
  "$PROJ/build-MM4/generated/scr_main_screen.c" \
  "$PROJ/build-MM4/generated/screens.c" \
  "$OUT/liblvgl.a" -lm

for s in ok nopm lowexc trip cal unsaved nodruck neg; do
  "$OUT/ui_preview" "$s" "$OUT/$s.raw"
  python3 "$HERE/render.py" "$OUT/$s.raw" "$OUT/$s.png"
  rm "$OUT/$s.raw"
  "$OUT/ui_preview" "$s" "$OUT/$s-chip.raw" raw
  python3 "$HERE/render.py" "$OUT/$s-chip.raw" "$OUT/$s-chip.png"
  rm "$OUT/$s-chip.raw"
done
"$OUT/ui_preview" ok "$OUT/hist.raw" hist
python3 "$HERE/render.py" "$OUT/hist.raw" "$OUT/hist.png"
rm "$OUT/hist.raw"
"$OUT/ui_preview" ok "$OUT/rise.raw" rise
python3 "$HERE/render.py" "$OUT/rise.raw" "$OUT/rise.png"
rm "$OUT/rise.raw"
"$OUT/ui_preview" ok "$OUT/motor.raw" motor
"$OUT/ui_preview" trip "$OUT/motor-trip.raw" motor
python3 "$HERE/render.py" "$OUT/motor-trip.raw" "$OUT/motor-trip.png"
rm "$OUT/motor-trip.raw"
python3 "$HERE/render.py" "$OUT/motor.raw" "$OUT/motor.png"
rm "$OUT/motor.raw"
# Stage 4 screens (ui.c): Home states, Menu, keypad, confirm.
gcc $CFLAGS -o "$OUT/ui_new" "$HERE/ui_new_main.c" "$HERE/fake_hw.c" \
  "$PROJ/ui.c" "$PROJ/ui_home.c" "$PROJ/ui_font_12.c" "$PROJ/ui_font_16.c" "$PROJ/ui_font_26.c" \
  "$PROJ/fixed.c" "$OUT/liblvgl.a" -lm
for s in notcal manual calib moving nodruck stop neg menu keypad confirm; do
  "$OUT/ui_new" "$s" "$OUT/new-$s.raw"
  python3 "$HERE/render.py" "$OUT/new-$s.raw" "$OUT/new-$s.png"
  rm "$OUT/new-$s.raw"
done
echo "Wrote calibration and chip-reading previews for all scenarios to $OUT"
