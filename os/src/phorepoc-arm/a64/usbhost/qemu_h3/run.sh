#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: EPL-1.0
#
# run.sh -- boot bench.elf on QEMU orangepi-pc (headless), with USB devices on the H3's EHCI1 bus (usb-bus.1 =
# 0x01C1B000) and OHCI1 bus (usb-bus.5 = 0x01C1B400; QEMU usb-serial is full speed only). Serial -> $O/bench.log (first LOGMAX bytes). Judge the run by the [usbh] markers.
# Env: O (build output), SECS (run time, default 20), DEVS (extra -device args; default = a usb-serial whose
# chardev is an echo server, i.e. a loopback that answers like a modem's AT port in echo mode).
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
O="${O:-$HERE/out}"
SECS="${SECS:-20}"
rm -f "$O/bench.full.log" "$O/bench.log"
# the far end of the USB serial port: a TCP echo server (the stand-in for the modem's AT interpreter)
python3 - "$O/echo.port" <<'PY' &
import socket, sys, threading
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(('127.0.0.1', 0)); s.listen(1)
open(sys.argv[1], 'w').write(str(s.getsockname()[1]))
c, _ = s.accept()
while True:
    d = c.recv(256)
    if not d: break
    c.sendall(d)
    if b'AT' in d:
        c.sendall(b'\r\nOK\r\n')
PY
ECHO=$!
for i in $(seq 50); do [ -s "$O/echo.port" ] && break; sleep 0.1; done
PORT=$(cat "$O/echo.port")
DEVS="${DEVS:--chardev socket,id=s0,host=127.0.0.1,port=$PORT -device usb-serial,bus=usb-bus.5,chardev=s0}"
timeout "$SECS" qemu-system-arm -M orangepi-pc -m 1024 -display none -monitor none \
  -serial file:"$O/bench.full.log" -kernel "$O/bench.elf" $DEVS || true
kill $ECHO 2>/dev/null || true
rm -f "$O/echo.port"
head -c "${LOGMAX:-60000}" "$O/bench.full.log" > "$O/bench.log"
echo "run.sh: $(grep -c '^\[usbh\]' "$O/bench.log" || true) [usbh] lines -> $O/bench.log"
