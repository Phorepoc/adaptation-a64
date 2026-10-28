# adaptation-a64

These are the Symbian drivers for the Allwinner A64 system-on-chip. They cover the display, the Mali-400
GPU, the video decoder, the camera interface, the sensors, USB and Wi-Fi.

Nothing here is specific to one device, so any A64 board can use it. A board repository names this one in its
`board.conf`.

## What's in here

```
os/src/phorepoc-arm/a64/    the chip's drivers
os/src/phorepoc-arm/mali/   the Mali-400 GPU kernel driver
os/src/phorepoc-arm/vde/    the video decoder driver
os/src/<component>/         programs and tests that use these drivers (phevdec, phecam, phfs, ssya64 and others)
registry/components_a64.py  the components this repository adds
```

At build time `os/` is laid over [core](https://github.com/Phorepoc/core)'s `os/`, so the paths line up with core's.

## Building

You don't build this repository by itself. Check it out next to core, the [pdk](https://github.com/Phorepoc/pdk),
a board repository that names it and mesa-symbian, and build the image as the pdk README describes. The board's
`board.conf` brings this repository in.

## What the board provides

The wiring of a board is the board's. The panel, its backlight and the PMIC rails are functions the board implements
(`a64_board.h`, in the board's `board/kernel/`). Pin numbers, the touch controller's bus and address, the USB product
string and the sensor location are macros in the board's `board/kernel/a64_board_config.h`; the code here stops the
build with an `#error` if one is missing. `a64_glue.cpp` takes the LED and vibrator pins, `a64uart.cpp` the modem's
control pins, `a64touch.cpp` the touch wiring, `a64_usbcc.cpp` the product string and `ssya64` the sensor location. The
Bluetooth UART's pins in `a64uart.cpp` are still fixed numbers here.

## Code from Linux

Two drivers started from Linux code that's offered under a choice of licenses, and we use the permissive one:

- The Wi-Fi driver in `os/src/phorepoc-arm/a64/wlan/rtw88/` is Linux's rtw88, used under BSD-3-Clause.
- The GPU scheduler in `os/src/phorepoc-arm/mali/mali_ldd.cpp` comes from Linux's lima driver, used under MIT.

One thing we can't ship is the table of coefficients the display scaler uses, because the only source we know of is
GPL-only. Without it, video at its own size looks right, and scaled video is a bit soft.

## License

Code we wrote is Apache-2.0. Symbian files we've changed are EPL-2.0, with GPL-2.0-or-later as a Secondary License, and
Symbian files we use unchanged stay EPL-1.0. The code from Linux keeps the license named above. The license texts are in
`LICENSES/`.

Contributions are welcome under the same terms. Add your name to `AUTHORS` in your first pull request.
