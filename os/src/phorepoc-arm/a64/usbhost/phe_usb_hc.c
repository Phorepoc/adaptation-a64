/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

/* phe_usb_hc.c -- attaches FreeBSD's host controller drivers (unmodified, third_party/freebsd-usb: ehci.c through
 * generic_ehci.c, ohci.c) to the controllers the board layer lists, on a "pci" root bus of FreeBSD's stand/kshim
 * (also unmodified). It does what generic_ehci_fdt.c / generic_ohci.c do on FreeBSD, without FDT, clocks or PHYs:
 * probe, hand out the MMIO window + IRQ as bus resources, and (OHCI) the attach sequence of the generic driver.
 * The PHY / clock / reset bring-up (aw_usbphy on the A64) is the board layer's job before usb_init(). */
#include <bsd_global.h>

#include <dev/usb/controller/generic_ehci.h>
#include <dev/usb/controller/ohci.h>
#include <dev/usb/controller/ohcireg.h>

#include "phe_usb_port.h"

/* bring-up breadcrumbs (the step before a hang names it in slot A's SD-log replay): only in a PHE_USBHOST_TRACE build */
#ifdef PHE_USBHOST_TRACE
#define HCTRC(...) printf(__VA_ARGS__)
#else
#define HCTRC(...) ((void)0)
#endif

/* The kshim's busdma (stand/usb/usb_busdma_loader.c) takes bus address = CPU address. Every controller driver gets its
 * descriptor and buffer addresses from usbd_get_page(), so the link wraps it (--wrap=usbd_get_page) and the host
 * translates here: phe_usb_v2p() (the kernel: the uncached pool's offset; the bench: identity). */
void __real_usbd_get_page(struct usb_page_cache *pc, usb_frlength_t offset, struct usb_page_search *res);
void
__wrap_usbd_get_page(struct usb_page_cache *pc, usb_frlength_t offset, struct usb_page_search *res)
{
	__real_usbd_get_page(pc, offset, res);
	if (res->physaddr != 0)
		res->physaddr = phe_usb_v2p((unsigned long)res->physaddr);
}

/* declared by the kshim (bsd_kernel.h) but not implemented there; ohci.c uses it */
void
bus_space_barrier(bus_space_tag_t space, bus_space_handle_t handle, bus_size_t offset, bus_size_t length, int flags)
{
	__asm__ volatile("dsb" ::: "memory");
}

/* phe_usb_fdt.c (the aw_usbphy stand-ins); weak: the bench and boards without a PHY to set up leave it out */
int phe_usb_phy_attach(device_t root) __attribute__((weak));
int phe_usb_phy_resource(struct resource *res, int rid) __attribute__((weak));
int phe_usb_phy_resource(struct resource *res, int rid) { return (ENXIO); }

static int
phe_usb_resource_cb(struct resource *res, device_t dev, int type, int *rid, unsigned int flags)
{
	struct phe_usb_hc_cfg cfg;

	if (strcmp(device_get_name(dev), "awusbphy") == 0)
		return (phe_usb_phy_resource(res, *rid));
	if (phe_usb_board_hc(device_get_name(dev), device_get_unit(dev), &cfg) != 0)
		return (ENXIO);
	switch (type) {
	case SYS_RES_MEMORY:
		res->r_bustag = 0;
		res->r_bushandle = (bus_space_handle_t)cfg.base;
		res->__r_i->r_start = cfg.base;
		res->__r_i->r_end = cfg.base + cfg.size - 1;
		return (0);
	case SYS_RES_IRQ:
		/* the kshim polls: device_run_interrupts() calls the handler on every usb_idle() */
		res->__r_i->r_start = res->__r_i->r_end = (cfg.irq < 0) ? 0 : cfg.irq;
		return (0);
	default:
		return (ENXIO);
	}
}

static int
phe_ehci_probe(device_t self)
{
	struct phe_usb_hc_cfg cfg;

	if (phe_usb_board_hc("ehci", device_get_unit(self), &cfg) != 0)
		return (ENXIO);
	bus_alloc_resource_any_cb = phe_usb_resource_cb;
	device_set_desc(self, "EHCI (generic)");
	return (0);
}

static device_method_t phe_ehci_methods[] = {
	DEVMETHOD(device_probe, phe_ehci_probe),
	DEVMETHOD(device_attach, generic_ehci_attach),
	DEVMETHOD(device_detach, generic_ehci_detach),
	DEVMETHOD_END
};

static driver_t phe_ehci_driver = {
	.name = "ehci",
	.methods = phe_ehci_methods,
	.size = sizeof(ehci_softc_t),
};

DRIVER_MODULE(ehci, pci, phe_ehci_driver, 0, 0);

/* OHCI: the same steps as FreeBSD's generic_ohci attach, minus its FDT clock/PHY/reset lists. */
static int
phe_ohci_probe(device_t self)
{
	struct phe_usb_hc_cfg cfg;

	if (phe_usb_board_hc("ohci", device_get_unit(self), &cfg) != 0)
		return (ENXIO);
	bus_alloc_resource_any_cb = phe_usb_resource_cb;
	device_set_desc(self, "OHCI (generic)");
	return (0);
}

static int
phe_ohci_attach(device_t self)
{
	ohci_softc_t *sc = device_get_softc(self);
	int rid = 0;

	sc->sc_bus.parent = self;
	sc->sc_bus.devices = sc->sc_devices;
	sc->sc_bus.devices_max = OHCI_MAX_DEVICES;
	sc->sc_bus.dma_bits = 32;
	HCTRC("[usbh] ohci: bus mem alloc\n");
	if (usb_bus_mem_alloc_all(&sc->sc_bus, USB_GET_DMA_TAG(self), &ohci_iterate_hw_softc))
		return (ENOMEM);
	sc->sc_io_res = bus_alloc_resource_any(self, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	sc->sc_irq_res = bus_alloc_resource_any(self, SYS_RES_IRQ, &rid, RF_ACTIVE);
	if (sc->sc_io_res == NULL || sc->sc_irq_res == NULL)
		return (ENXIO);
	sc->sc_io_tag = rman_get_bustag(sc->sc_io_res);
	sc->sc_io_hdl = rman_get_bushandle(sc->sc_io_res);
	sc->sc_io_size = rman_get_size(sc->sc_io_res);
	sc->sc_bus.bdev = device_add_child(self, "usbus", DEVICE_UNIT_ANY);
	if (sc->sc_bus.bdev == NULL)
		return (ENXIO);
	device_set_ivars(sc->sc_bus.bdev, &sc->sc_bus);
	strlcpy(sc->sc_vendor, "Generic", sizeof(sc->sc_vendor));
	if (bus_setup_intr(self, sc->sc_irq_res, INTR_TYPE_BIO | INTR_MPSAFE, NULL,
	    (driver_intr_t *)ohci_interrupt, sc, &sc->sc_intr_hdl) != 0)
		return (ENXIO);
	HCTRC("[usbh] ohci: ohci_init\n");
	if (ohci_init(sc) != 0)
		return (ENXIO);
	HCTRC("[usbh] ohci: usbus attach\n");
	return (device_probe_and_attach(sc->sc_bus.bdev));
}

static device_method_t phe_ohci_methods[] = {
	DEVMETHOD(device_probe, phe_ohci_probe),
	DEVMETHOD(device_attach, phe_ohci_attach),
	DEVMETHOD_END
};

static driver_t phe_ohci_driver = {
	.name = "ohci",
	.methods = phe_ohci_methods,
	.size = sizeof(ohci_softc_t),
};

DRIVER_MODULE(ohci, pci, phe_ohci_driver, 0, 0);

/*------------------------------------------------------------------------*
 * Root bus. The kshim's own "pci" root (bsd_kernel.c, USB_PCI_PROBE_LIST) does not build at the pin (its
 * DRIVER_MODULE call still passes the devclass argument newbus dropped), so the root is made here: a "pci" device
 * with one "ehci" / "ohci" child per board controller. Interrupts are polled: from phe_usb_service() and from a 1-tick
 * callout, which also runs while the stack waits inside the kshim's cv_timedwait().
 *------------------------------------------------------------------------*/

static device_t phe_usb_root;
static device_method_t phe_usb_root_methods[] = { DEVMETHOD_END };
static const struct driver phe_usb_root_driver = { .name = "pci", .methods = phe_usb_root_methods, .size = 0 };
static struct module_data phe_usb_root_mod = {
	.bus_name = "", .mod_name = "pci", .long_name = "/pci", .driver = &phe_usb_root_driver };
static struct callout phe_usb_irq_callout;

static void
phe_usb_irq_poll(void *arg)
{
	device_run_interrupts(phe_usb_root);
	callout_reset(&phe_usb_irq_callout, 1, phe_usb_irq_poll, NULL);
}

static void
phe_usb_root_load(void *arg)
{
	static const char *const kinds[] = { "ehci", "ohci" };
	struct phe_usb_hc_cfg cfg;
	int k, n;

	phe_usb_root = device_add_child(NULL, "pci", DEVICE_UNIT_ANY);
	if (phe_usb_root == NULL)
		return;
	/* children find their driver by the parent's driver name (device_get_name), so the root needs one */
	phe_usb_root->dev_module = &phe_usb_root_mod;
	bus_alloc_resource_any_cb = phe_usb_resource_cb;
	if (phe_usb_phy_attach != NULL) {
		HCTRC("[usbh] hc: PHY attach\n");
		phe_usb_phy_attach(phe_usb_root);	/* the PHY provider first, as FreeBSD's bus passes order it */
		HCTRC("[usbh] hc: PHY attach done\n");
	}
	callout_init_mtx(&phe_usb_irq_callout, NULL, 0);
	callout_reset(&phe_usb_irq_callout, 1, phe_usb_irq_poll, NULL);
	for (k = 0; k < 2; k++) {
		for (n = 0; phe_usb_board_hc(kinds[k], n, &cfg) == 0; n++) {
			device_t dev = device_add_child(phe_usb_root, kinds[k], DEVICE_UNIT_ANY);
			HCTRC("[usbh] hc: %s%d attach\n", kinds[k], n);
			if (dev == NULL || device_probe_and_attach(dev) != 0)
				printf("[usbh] %s%d at 0x%08lx: attach failed\n", kinds[k], n, (unsigned long)cfg.base);
			else {
				printf("[usbh] %s%d at 0x%08lx attached\n", kinds[k], n, (unsigned long)cfg.base);
				/* the GIC line only once the controller is initialised: bound before, an interrupt the
				 * stack has no handler for yet re-fires at once (ISR -> DFC -> unmask) and storms CPU 0 */
				phe_usb_irq_attach(cfg.irq);
			}
		}
	}
}
SYSINIT(phe_usb_root_load, SI_SUB_RUN_SCHEDULER, SI_ORDER_MIDDLE, phe_usb_root_load, 0);

/*------------------------------------------------------------------------*
 * Service pass + descriptor dump
 *------------------------------------------------------------------------*/

/* phe_usb_tty.c (the serial ports); weak: builds without the serial drivers leave it out */
void phe_usb_tty_service(void) __attribute__((weak));

void
phe_usb_service(void)
{
	static uint32_t last;
	uint32_t now = phe_usb_time_us();

	device_run_interrupts(phe_usb_root);
	if (phe_usb_tty_service != NULL)
		phe_usb_tty_service();
	usb_idle();
	while ((uint32_t)(now - last) >= 1000) {
		last += 1000;
		callout_process(1);
	}
}

static void
phe_usb_dump_one(struct usb_device *udev)
{
	struct usb_device_descriptor *dd = &udev->ddesc;
	struct usb_config_descriptor *cd = udev->cdesc;
	struct usb_descriptor *desc = NULL;

	printf("[usbh] dev addr=%d VID:PID=%04x:%04x class=%02x sub=%02x proto=%02x bcdUSB=%04x mps0=%d cfgs=%d speed=%d"
	    " '%s' '%s'\n",
	    udev->address, UGETW(dd->idVendor), UGETW(dd->idProduct), dd->bDeviceClass, dd->bDeviceSubClass,
	    dd->bDeviceProtocol, UGETW(dd->bcdUSB), dd->bMaxPacketSize, dd->bNumConfigurations, udev->speed,
	    usb_get_manufacturer(udev), usb_get_product(udev));
	if (cd == NULL)
		return;
	printf("[usbh] cfg%d total=%d ifaces=%d attr=%02x power=%dmA\n", cd->bConfigurationValue,
	    UGETW(cd->wTotalLength), cd->bNumInterface, cd->bmAttributes, cd->bMaxPower * 2);
	while ((desc = usb_desc_foreach(cd, desc)) != NULL) {
		if (desc->bDescriptorType == UDESC_INTERFACE) {
			struct usb_interface_descriptor *id = (void *)desc;
			printf("[usbh]  iface %d alt %d class=%02x sub=%02x proto=%02x eps=%d\n", id->bInterfaceNumber,
			    id->bAlternateSetting, id->bInterfaceClass, id->bInterfaceSubClass,
			    id->bInterfaceProtocol, id->bNumEndpoints);
		} else if (desc->bDescriptorType == UDESC_ENDPOINT) {
			struct usb_endpoint_descriptor *ed = (void *)desc;
			printf("[usbh]   ep %02x %s mps=%d interval=%d\n", ed->bEndpointAddress,
			    (const char *[]){"control", "isoc", "bulk", "intr"}[ed->bmAttributes & 3],
			    UGETW(ed->wMaxPacketSize), ed->bInterval);
		}
	}
}

void
phe_usb_dump_devices(void)
{
	devclass_t dc = devclass_find("usbus");
	int unit, i;

	if (dc == NULL)
		return;
	for (unit = 0; unit < DEVCLASS_MAXUNIT; unit++) {
		device_t bdev = devclass_get_device(dc, unit);
		struct usb_bus *bus = bdev ? device_get_ivars(bdev) : NULL;
		if (bus == NULL)
			continue;
		for (i = 0; i < (int)bus->devices_max; i++) {
			struct usb_device *udev = bus->devices[i];
			if (udev != NULL && udev->parent_hub != NULL)	/* skip the root hub */
				phe_usb_dump_one(udev);
		}
	}
}
