# usbhost: FreeBSD USB host stack on a small compatibility layer

The USB host stack is FreeBSD's, imported unmodified in `os/third_party/freebsd-usb` (BSD-2-Clause; pin and file list
there): the USB core (`sys/dev/usb`), the EHCI and OHCI controller drivers, and FreeBSD's own kernel shim for running
that stack outside the FreeBSD kernel (`stand/kshim`, with `stand/usb/usb_busdma_loader.c`). The kshim supplies
mutexes, condition variables, callouts, the device/devclass glue, bus_space and bus_dma. The files here are ours
(EPL-1.0) and only fill the gaps:

| File | What |
|---|---|
| `phe_usb_port.h` | the seam to the host OS: the board's controller list, a microsecond clock, the service pass |
| `phe_usb_hc.c` | the root bus and the EHCI / OHCI attachment (generic_ehci.c is used as-is; OHCI repeats the generic attach steps without FDT), polled interrupts, `bus_space_barrier`, a descriptor dump |
| `kinc/` | one-line kernel-header stand-ins for FreeBSD files that do not support `USB_GLOBAL_INCLUDE_FILE` (generic_ehci.c) |
| `qemu_h3/` | a bare-metal bench on QEMU `orangepi-pc` (MMU off, single CPU, polled): `build.sh`, `run.sh` |

Two gaps in the kshim at the pin, handled here rather than by patching FreeBSD files: its own `pci` root bus
(`USB_PCI_PROBE_LIST`) still passes a devclass argument that newbus dropped, and it declares `bus_space_barrier`
without defining it.

## Bench

```
FB=<os/third_party/freebsd-usb> O=<out> bash qemu_h3/build.sh
O=<out> SECS=14 bash qemu_h3/run.sh          # default device: a usb-serial on OHCI1 (usb-bus.5)
O=<out> DEVS="-drive if=none,id=d0,file=disk.img,format=raw -device usb-storage,bus=usb-bus.1,drive=d0" bash qemu_h3/run.sh
```
Needs `arm-none-eabi-gcc` and `qemu-system-arm`. The H3's USB1 pair (EHCI1 at 0x01C1B000, OHCI1 at 0x01C1B400) has the
same addresses as the A64's USB1, the modem port. QEMU wires no EHCI/OHCI companion routing: put high-speed devices on
usb-bus.1 and full/low-speed ones on usb-bus.5.
