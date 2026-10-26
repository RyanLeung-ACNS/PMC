#!/bin/sh
# Builds and runs the host test of the Modbus TCP server (modbus.c, mbmap.c)
# twice: remote writes off (the default) and on. Needs gcc only.
# mb.sh (bash only) then talks to it over TCP, and client.py too when
# python3 and pymodbus are installed.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
PROJ="$HERE/../../InitialTesting"
mkdir -p "$HERE/out"
for W in 0 1; do
  gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -DPMC_MODBUS_WRITE=$W \
    -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE \
    -I"$PROJ/lvgl_940" -I"$PROJ/lvgl_940/lv_conf/heavy" -I"$PROJ" \
    -o "$HERE/out/test_modbus_w$W" "$HERE/test_modbus.c" \
    "$PROJ/modbus.c" "$PROJ/mbmap.c" "$PROJ/fixed.c" -lm
  "$HERE/out/test_modbus_w$W"
done
# mb.sh (the Git Bash client) against the host server, writes off then on.
for W in 0 1; do
  "$HERE/out/test_modbus_w$W" serve 15030 > "$HERE/out/serve.log" &
  PID=$!
  sleep 0.3
  OUT=$("$HERE/mb.sh" 127.0.0.1 15030 writetest; "$HERE/mb.sh" 127.0.0.1 15030)
  kill $PID
  echo "$OUT" | grep -q "pressure      4.625 mbar" || { echo "mb.sh FAIL: map"; echo "$OUT"; exit 1; }
  if [ $W = 0 ]; then WANT="write target 4.8:  90 01"; else WANT="write target 4.8:  10 00 0c 00 02"; fi
  echo "$OUT" | grep -q "$WANT" || { echo "mb.sh FAIL: writes $W"; echo "$OUT"; exit 1; }
  echo "mb.sh, writes $W: ok"
done
if python3 -c "import pymodbus" 2>/dev/null; then
  for W in 0 1; do
    "$HERE/out/test_modbus_w$W" serve 15020 > "$HERE/out/serve.log" &
    PID=$!
    sleep 0.3
    python3 "$HERE/client.py" 127.0.0.1 15020 $W || { kill $PID; exit 1; }
    kill $PID
  done
else
  echo "pymodbus not installed: TCP client check skipped"
fi
