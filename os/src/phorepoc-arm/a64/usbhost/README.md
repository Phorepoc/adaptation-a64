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

## USB Ethernet (mobile data): `USBHOST_NET=1`

The modem's data interface in `AT+QCFG="usbnet",1` (ECM) mode is a CDC Ethernet function. `build_bsd.sh` with
`USBHOST_NET=1` adds FreeBSD's `if_cdce.c` (CDC ECM and NCM, imported unmodified into `modem-eg25/third_party`,
`MODEM_EG25` = that checkout) and `phe_usb_net.c`, which replaces the pieces of FreeBSD's networking that `if_cdce.c`
calls (a flat `struct mbuf`, `struct ifnet`, `ifmedia`, `uether_*`; headers in `kinc/net`, `kinc/sys/mbuf.h`,
`kinc/dev/usb/net`). The port seam is `phe_usb_net.h` (`phe_usb_net_up/_tx/_eaddr/_link`, `phe_usb_net_rx_cb`);
`ekern/phe_usb_ether.cpp` is the "Ethernet.USB" PDD under the real Ethernet LDD (same contract as the e1000 PDD on x86).
`link_block.sh` takes `USBHOST_NET=1` too. Compile-checked: `usbhost_bsd.o` links with and without it.
