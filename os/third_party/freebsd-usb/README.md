# freebsd-usb (unmodified import)

FreeBSD USB host sources for the A64 USB1 host port: the USB core, the EHCI and OHCI controller drivers, the Allwinner
USB PHY, and FreeBSD's own kernel shim for running the USB stack outside the FreeBSD kernel (`stand/kshim`, used by
FreeBSD's `stand/usb`), imported unmodified. Each file keeps its own licence header (BSD-2-Clause; `usb_pf.h` and the `sys/sys`
headers BSD-3-Clause).

Pin: https://github.com/freebsd/freebsd-src commit 043d6753ca7c1c1e3ffa09a9f953f0df50d3a22e (main, 2026-11-13).

| Files | Upstream path (same) | What |
|---|---|---|
| sys/dev/usb/controller/ehci.c, ehci.h, ehcireg.h, generic_ehci.c, generic_ehci.h, generic_ehci_fdt.c | same | EHCI |
| sys/dev/usb/controller/ohci.c, ohci.h, ohcireg.h, usb_controller.c | same | OHCI, the usbus driver |
| sys/dev/usb/usb_*.c / *.h, usb.h, usbdi*.h, usbhid.h, quirk/usb_quirk.h, template/usb_template.h | same | USB core |
| sys/dev/hid/hid.h, sys/sys/{cdefs,_decls,_visible,errno,gpio,ioccom,queue}.h | same | headers the core includes |
| stand/kshim/bsd_kernel.c, bsd_kernel.h, bsd_busspace.c, bsd_global.h, sysinit.h | same | the kernel shim |
| stand/usb/usb_busdma_loader.c, stand/usb/tools/sysinit.c | same | busdma on flat memory; the SYSINIT table tool |
| sys/arm/allwinner/aw_usbphy.c | same | A64 USB PHY (not built yet) |

The file set is what the build really includes (compiler dependency lists). Our glue is separate:
`os/src/phorepoc-arm/a64/usbhost` (README there). These files stay byte-identical to upstream (check: `sh check.sh`,
every file listed in SHA256SUMS).
