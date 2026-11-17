/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

/* phe_usb_fdt.c -- the static "device tree" behind phe_usb_fdt.h: one node for the SoC's USB PHY provider, as the
 * board layer describes it (phe_usb_board_phy), and the PHY attach run before the host controllers attach. */
#include "phe_usb_fdt.h"

#include "phe_usb_port.h"

static struct phe_usb_phy_cfg phy_cfg;
static int phy_have;

struct phe_usb_clk {
	const struct phe_usb_gate *g;
};
static struct phe_usb_clk clks[PHE_USB_MAX_GATES], rsts[PHE_USB_MAX_GATES];
static struct phynode *phys;

static int
gate_set(const struct phe_usb_gate *g)
{
	volatile uint32_t *r = (volatile uint32_t *)(g->base + g->reg);

	*r |= g->mask;
	return (0);
}

phandle_t
ofw_bus_get_node(device_t dev)
{
	return (1);
}

const struct ofw_compat_data *
ofw_bus_search_compatible(device_t dev, const struct ofw_compat_data *compat)
{
	for (; compat->ocd_str != NULL; compat++)
		if (phy_have && strcmp(compat->ocd_str, phy_cfg.compatible) == 0)
			break;
	return (compat);
}

int
ofw_bus_find_string_index(phandle_t node, const char *prop, const char *name, int *idx)
{
	int i;

	if (strcmp(prop, "reg-names") != 0)
		return (ENOENT);
	for (i = 0; i < PHE_USB_MAX_REGS && phy_cfg.reg_name[i] != NULL; i++)
		if (strcmp(phy_cfg.reg_name[i], name) == 0) {
			*idx = i;
			return (0);
		}
	return (ENOENT);
}

int
clk_get_by_ofw_index(device_t dev, phandle_t node, int idx, clk_t *clk)
{
	if (idx < 0 || idx >= PHE_USB_MAX_GATES || phy_cfg.clk[idx].mask == 0)
		return (ENOENT);
	clks[idx].g = &phy_cfg.clk[idx];
	*clk = &clks[idx];
	return (0);
}

int clk_enable(clk_t clk) { return (gate_set(clk->g)); }
const char *clk_get_name(clk_t clk) { return ("usbphy-gate"); }

int
hwreset_get_by_ofw_idx(device_t dev, phandle_t node, int idx, hwreset_t *rst)
{
	if (idx < 0 || idx >= PHE_USB_MAX_GATES || phy_cfg.rst[idx].mask == 0)
		return (ENOENT);
	rsts[idx].g = &phy_cfg.rst[idx];
	*rst = &rsts[idx];
	return (0);
}

int hwreset_deassert(hwreset_t rst) { return (gate_set(rst->g)); }

/* no supply is switched by the PHY on these boards (the board layer powers the port), no ID/VBUS detect pins */
int regulator_get_by_ofw_property(device_t dev, phandle_t node, const char *name, regulator_t *reg) { return (ENOENT); }
int regulator_enable(regulator_t reg) { return (0); }
int regulator_disable(regulator_t reg) { return (0); }
int gpio_pin_get_by_ofw_property(device_t dev, phandle_t node, const char *name, gpio_pin_t *pin) { return (ENOENT); }
int gpio_pin_is_active(gpio_pin_t pin, bool *active) { *active = false; return (0); }

/* phynode */
struct phynode_class phynode_usb_class = { "phynode_usb", NULL, sizeof(struct phynode_usb_sc), NULL };

struct phynode *
phe_usb_phynode_create(device_t dev, struct phynode_class *cls, struct phynode_init_def *def)
{
	struct phynode *p = malloc(sizeof(*p), M_DEVBUF, M_WAITOK | M_ZERO);

	if (p == NULL)
		return (NULL);
	p->cls = cls;
	p->dev = dev;
	p->id = def->id;
	return (p);
}

struct phynode *
phynode_register(struct phynode *phy)
{
	phy->next = phys;
	phys = phy;
	return (phy);
}

device_t phynode_get_device(struct phynode *phy) { return (phy->dev); }
intptr_t phynode_get_id(struct phynode *phy) { return (phy->id); }

static void *
phy_method(struct phynode *p, const char *name)
{
	const struct phynode_class *c;
	const struct phynode_method *m;

	for (c = p->cls; c != NULL; c = c->base)
		for (m = c->methods; m != NULL && m->desc != NULL; m++)
			if (strcmp(m->desc, name) == 0)
				return (m->func);
	return (NULL);
}

int
phe_usb_phy_host(int id)
{
	struct phynode *p;
	int (*en)(struct phynode *, bool);
	int (*mode)(struct phynode *, int);
	int r;

	for (p = phys; p != NULL; p = p->next)
		if (p->id == id)
			break;
	if (p == NULL)
		return (ENXIO);
	en = phy_method(p, "phynode_enable");
	mode = phy_method(p, "phynode_usb_set_mode");
	r = en ? en(p, true) : ENXIO;
	if (r == 0 && mode != NULL)
		r = mode(p, PHY_USB_MODE_HOST);
	return (r);
}

/*------------------------------------------------------------------------*
 * The PHY provider's bus: a "simplebus" device under the USB root, with the provider as its child.
 *------------------------------------------------------------------------*/
static device_method_t sb_methods[] = { DEVMETHOD_END };
static const struct driver sb_driver = { .name = "simplebus", .methods = sb_methods, .size = 0 };
static struct module_data sb_mod = {
	.bus_name = "", .mod_name = "simplebus", .long_name = "/simplebus", .driver = &sb_driver };

/* the kshim's ofw_bus_status_okay() answers 0 while this callback is NULL, and awusbphy_probe() then returns ENXIO:
 * "awusbphy attach failed" (boot 2726). The one static node is always "okay". */
static int
phe_usb_status_ok(device_t dev)
{
	return (1);
}

int
phe_usb_phy_attach(device_t root)
{
	device_t sb, dev;
	int i, r;

	if (phe_usb_board_phy(&phy_cfg) != 0)
		return (0);		/* no PHY to set up (the QEMU bench, the H3 model) */
	phy_have = 1;
	ofw_bus_status_ok_cb = phe_usb_status_ok;
	sb = device_add_child(root, "simplebus", DEVICE_UNIT_ANY);
	if (sb == NULL)
		return (ENOMEM);
	sb->dev_module = &sb_mod;
	dev = device_add_child(sb, "awusbphy", DEVICE_UNIT_ANY);
	if (dev == NULL || (r = device_probe_and_attach(dev)) != 0) {
		printf("[usbh] awusbphy attach failed\n");
		return (ENXIO);
	}
	for (i = 0; i < PHE_USB_MAX_PHYS && phy_cfg.host_phy[i] >= 0; i++) {
		r = phe_usb_phy_host(phy_cfg.host_phy[i]);
		printf("[usbh] phy%d host mode r=%d\n", phy_cfg.host_phy[i], r);
	}
	return (0);
}

/* the provider's register windows: rid = the reg-names index */
int
phe_usb_phy_resource(struct resource *res, int rid)
{
	if (!phy_have || rid < 0 || rid >= PHE_USB_MAX_REGS || phy_cfg.reg_name[rid] == NULL)
		return (ENXIO);
	res->r_bustag = 0;
	res->r_bushandle = (bus_space_handle_t)phy_cfg.reg_base[rid];
	res->__r_i->r_start = phy_cfg.reg_base[rid];
	res->__r_i->r_end = phy_cfg.reg_base[rid] + 0xff;
	return (0);
}
