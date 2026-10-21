# adaptation-a64

The Allwinner A64 platform adaptation of Phorepoc: the SoC's drivers (display engine, Mali-400 GPU, video engine,
camera, sensors, USB, WLAN), reusable by any A64 board. Device adaptations build on it:
[adaptation-pinephone](https://github.com/Phorepoc/adaptation-pinephone).

## Layout

```
os/src/phorepoc-arm/a64/    the SoC drivers
os/src/phorepoc-arm/mali/   the Mali-400 (Utgard) kernel driver
os/src/phorepoc-arm/vde/    the video engine driver
os/src/<component>/         user-side components and tests on these drivers (phevdec, phecam, phfs, ssya64 ...)
registry/components_a64.py  this adaptation's components (merged into core's registry)
```

`os/` is laid over [core](https://github.com/Phorepoc/core)'s `os/` at build time, so paths match core's.

**Status:** the components here build one by one with the pdk (`ph component build -b pinephone -arch arm <name>`). The
PinePhone **image** (the ARM kernel and ROM link, the AArch64 boot shim, the U-Boot packaging) is not in the public
pdk yet; it is being ported. Until then, the PC board is the one that builds a complete image.

## Third-party code

- `os/src/phorepoc-arm/a64/wlan/rtw88/`: Linux's rtw88 driver, dual-licensed GPL-2.0 OR BSD-3-Clause, used here under
  BSD-3-Clause.
- `os/src/phorepoc-arm/mali/mali_ldd.cpp`: derived from Linux's lima scheduler, dual-licensed GPL-2.0 OR MIT, used here
  under MIT.

Not included (GPL-only): the display scaler coefficient table (`a64_vi_coeff.h`) has to be replaced with our own before
the display driver builds.

## Licence

Eclipse Public License 2.0 with GPL-2.0-or-later as a Secondary License (see `LICENSES/EPL-2.0.txt`; the Nokia-derived files are our updated versions relicensed from EPL-1.0 under its section 7, unmodified Symbian sources are fetched from upstream and remain EPL-1.0), copyright The Phorepoc Authors (`AUTHORS`, `NOTICE`), except the third-party files above.

Contributions are welcome under the same terms: add yourself to `AUTHORS`.
