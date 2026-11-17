/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

/* phe_usb_fdt.h -- stand-ins for the FreeBSD kernel frameworks aw_usbphy.c (unmodified, third_party/freebsd-usb)
 * uses beyond the kshim: OFW/FDT node queries, clk, hwreset, regulator, GPIO pins and the PHY (phynode) framework.
 * There is no device tree: one static node per device (phe_usb_fdt.c), filled by the board layer with the
 * register windows, clock gates and resets. Only the calls aw_usbphy.c makes are provided, with FreeBSD's names and
 * argument shapes (the interface, not FreeBSD's implementation). kinc/ maps the FreeBSD header names onto this file. */
#ifndef PHE_USB_FDT_H
#define PHE_USB_FDT_H

#include <bsd_global.h>

/* types and small kernel helpers the kshim does not define */
typedef long intptr_t;
typedef long intmax_t;
typedef unsigned int phandle_t;
#define bootverbose 0
#define bzero(p, n) memset((p), 0, (n))
#define MODULE_VERSION(...)
#define BUS_PASS_SUPPORTDEV 0
#define BUS_PASS_ORDER_MIDDLE 0
#define bus_read_4(r, o) bus_space_read_4((r)->r_bustag, (r)->r_bushandle, (o))
#define bus_write_4(r, o, v) bus_space_write_4((r)->r_bustag, (r)->r_bushandle, (o), (v))

/* OFW */
struct ofw_compat_data {
	const char *ocd_str;
	uintptr_t ocd_data;
};
phandle_t ofw_bus_get_node(device_t dev);
const struct ofw_compat_data *ofw_bus_search_compatible(device_t dev, const struct ofw_compat_data *compat);
int ofw_bus_find_string_index(phandle_t node, const char *prop, const char *name, int *idx);

/* clk / hwreset / regulator / gpio: opaque handles into the node's tables */
typedef struct phe_usb_clk *clk_t;
typedef struct phe_usb_clk *hwreset_t;
typedef struct phe_usb_regulator *regulator_t;
typedef struct phe_usb_gpio *gpio_pin_t;
int clk_get_by_ofw_index(device_t dev, phandle_t node, int idx, clk_t *clk);
int clk_enable(clk_t clk);
const char *clk_get_name(clk_t clk);
int hwreset_get_by_ofw_idx(device_t dev, phandle_t node, int idx, hwreset_t *rst);
int hwreset_deassert(hwreset_t rst);
int regulator_get_by_ofw_property(device_t dev, phandle_t node, const char *name, regulator_t *reg);
int regulator_enable(regulator_t reg);
int regulator_disable(regulator_t reg);
int gpio_pin_get_by_ofw_property(device_t dev, phandle_t node, const char *name, gpio_pin_t *pin);
int gpio_pin_is_active(gpio_pin_t pin, bool *active);

/* the PHY framework (phynode) */
#define PHY_USB_MODE_OTG	0
#define PHY_USB_MODE_HOST	1
#define PHY_USB_MODE_DEVICE	2
struct phynode_method {
	const char *desc;
	void *func;
};
typedef struct phynode_method phynode_usb_method_t;
typedef struct phynode_method phynode_method_t;
#define PHYNODEMETHOD(what, func) { #what, (void *)&func }
#define PHYNODEMETHOD_END { 0, 0 }
struct phynode_class {
	const char *name;
	const struct phynode_method *methods;
	unsigned int size;
	const struct phynode_class *base;
};
#define DEFINE_CLASS_1(name, classvar, methods, size, base1) \
	struct phynode_class classvar = { #name, methods, size, &base1 }
extern struct phynode_class phynode_usb_class;
struct phynode_usb_sc {
	int mode;
};
struct phynode_init_def {
	intptr_t id;
	phandle_t ofw_node;
};
struct phynode {
	const struct phynode_class *cls;
	device_t dev;
	intptr_t id;
	struct phynode *next;
	unsigned char sc[32];
};
#define phynode_create(dev, cls, def) phe_usb_phynode_create((dev), (cls), (def))
struct phynode *phe_usb_phynode_create(device_t dev, struct phynode_class *cls, struct phynode_init_def *def);
struct phynode *phynode_register(struct phynode *phy);
device_t phynode_get_device(struct phynode *phy);
intptr_t phynode_get_id(struct phynode *phy);

/* ours: enable PHY `id` of the registered provider and put it in host mode (what generic_ehci_fdt.c asks of it) */
int phe_usb_phy_host(int id);

#endif
