# freebsd-usb (unmodified import)

FreeBSD USB host controller sources for the A64 USB1 host port (EHCI + the Allwinner USB PHY),
imported unmodified. Each file keeps its own licence header (BSD-2-Clause).

Pin: https://github.com/freebsd/freebsd-src commit 043d6753ca7c1c1e3ffa09a9f953f0df50d3a22e (main, 2026-11-13).

| File | Upstream path |
|---|---|
| sys/dev/usb/controller/ehci.c, ehci.h, ehcireg.h | same |
| sys/dev/usb/controller/generic_ehci.c, generic_ehci.h, generic_ehci_fdt.c | same |
| sys/arm/allwinner/aw_usbphy.c | same |

The Symbian glue (kernel extension, the USBDI channel interface) is written separately; these files stay byte-identical
to upstream (check: `sh check.sh`).
