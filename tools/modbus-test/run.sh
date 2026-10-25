#!/bin/sh
# Builds and runs the host test of the Modbus TCP server (modbus.c, mbmap.c)
# twice: remote writes off (the default) and on. Needs gcc only.
# With python3 and pymodbus installed, client.py also talks to it over TCP.
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
