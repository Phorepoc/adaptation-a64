// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

// a64_usbhost.cpp -- the A64's side of phe_usb_port.h: the USB1 host pair (EHCI1 0x01C1B000, OHCI1 0x01C1B400,
// SPI 74 / 75 = GIC 106 / 107; one 4 KiB register page) for the FreeBSD USB stack.
// A64_H3TEST (QEMU orangepi-pc): the H3 model has the same USB1 addresses and SPI numbers but no CCU gates to set and
// no USB PHY, so only the mapping is done. On the A64: the controllers' bus gates + resets here, then PHY1 through
// FreeBSD's aw_usbphy.c (unmodified) on the stand-ins of phe_usb_fdt.c, described by phe_usb_board_phy() below.
// CCU facts (usb-host-plan.md s5, "SETTLED" from the CCU table + DT; not yet read back on the phone):
//   BUS_CLK_GATING_REG0 0x060: EHCI1 b25, OHCI1 b29;  BUS_SOFT_RST_REG0 0x2C0: the same bits;
//   USBPHY_CFG_REG 0x0CC: PHY1 reset b1, PHY1 gate b9, OHCI1 12M gate b17.
// phy_ctrl = 0x01C19400 (shared with the OTG driver's PHY0 use), pmu0 = 0x01C1A800, pmu1 = 0x01C1B800.
#include <kernel/kern_priv.h>
#include <platform.h>

extern "C" {
#include "../phe_usb_port.h"
}

static const TPhysAddr KUsb1Phys = 0x01C1B000;		// EHCI1 +0x000, OHCI1 +0x400, HCI1 control ("pmu1") +0x800
static const TInt KIntIdEhci1 = 32 + 74;
static const TInt KIntIdOhci1 = 32 + 75;
static TLinAddr gUsb1;
#ifndef A64_H3TEST
static TLinAddr gCcu, gPhyCtl, gUsb0;
static const TUint KCcuBusGate0 = 0x060, KCcuBusRst0 = 0x2C0, KCcuUsbPhyCfg = 0x0CC;
#endif

extern "C" int phe_usb_board_init(void)
	{
	DPlatChunkHw* ch = NULL;
	TInt r = DPlatChunkHw::New(ch, KUsb1Phys, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r != KErrNone)
		return r;
	gUsb1 = ch->LinearAddress();
#ifndef A64_H3TEST
	DPlatChunkHw* c = NULL;
	if ((r = DPlatChunkHw::New(c, 0x01C20000, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking)) != KErrNone)
		return r;
	gCcu = c->LinearAddress(); c = NULL;
	if ((r = DPlatChunkHw::New(c, 0x01C19000, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking)) != KErrNone)
		return r;
	gPhyCtl = c->LinearAddress() + 0x400; c = NULL;
	if ((r = DPlatChunkHw::New(c, 0x01C1A000, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking)) != KErrNone)
		return r;
	gUsb0 = c->LinearAddress();
	// the controllers' own gates and resets (what generic_ehci_fdt/generic_ohci take from their DT nodes)
	*(volatile TUint32*)(gCcu + KCcuBusGate0) |= (1u << 25) | (1u << 29);
	*(volatile TUint32*)(gCcu + KCcuBusRst0) |= (1u << 25) | (1u << 29);
	// OHCI1's 12 MHz gate (bit 17) is a CHILD of OHCI0's (bit 16): Linux ccu-sun50i-a64.c has
	// usb_ohci1_clk = SUNXI_CCU_GATE("usb-ohci1", parent "usb-ohci0", 0x0cc, BIT(17)); setting bit 17 alone leaves
	// OHCI1 unclocked and its first register access hangs the bus (boot 2731). Both bits.
	*(volatile TUint32*)(gCcu + KCcuUsbPhyCfg) |= 1u << 16;	// OHCI0 12 MHz (the parent) first
	*(volatile TUint32*)(gCcu + KCcuUsbPhyCfg) |= 1u << 17;	// then OHCI1 12 MHz
#endif
	return r;
	}

// aw_usbphy's node: compatible, reg-names (phy_ctrl, pmu0, pmu1), the PHY1 gate + reset; PHY1 in host mode.
// PHY0 (the USB-C port) stays the OTG driver's: no PHY0 gate/reset here, and host_phy lists PHY1 only.
extern "C" int phe_usb_board_phy(struct phe_usb_phy_cfg* aCfg)
	{
#ifdef A64_H3TEST
	(void)aCfg;
	return -1;
#else
	memclr(aCfg, sizeof(*aCfg));
	aCfg->compatible = "allwinner,sun50i-a64-usb-phy";
	// phy_ctrl (0x01C19400) is PHY0's = the OTG controller's of the USB-C port (the phlink debug link): aw_usbphy's attach
	// writes PHY_CSR there to force the OTG PHY to host (ID low, VBUS valid), which would take the device-mode link away.
	// PHY1 (the modem) needs nothing in it, so aw_usbphy gets a RAM shadow of the window instead of the register page.
	static TUint32 sPhyCtlShadow[64];
	aCfg->reg_name[0] = "phy_ctrl"; aCfg->reg_base[0] = (TLinAddr)sPhyCtlShadow;
	aCfg->reg_name[1] = "pmu0";     aCfg->reg_base[1] = gUsb0 + 0x800;
	aCfg->reg_name[2] = "pmu1";     aCfg->reg_base[2] = gUsb1 + 0x800;
	aCfg->clk[0].base = gCcu; aCfg->clk[0].reg = KCcuUsbPhyCfg; aCfg->clk[0].mask = 1u << 9;
	aCfg->rst[0].base = gCcu; aCfg->rst[0].reg = KCcuUsbPhyCfg; aCfg->rst[0].mask = 1u << 1;
	aCfg->host_phy[0] = 1;
	aCfg->host_phy[1] = -1;
	return 0;
#endif
	}

extern "C" int phe_usb_board_hc(const char* aKind, int aN, struct phe_usb_hc_cfg* aCfg)
	{
	if (aN != 0 || !gUsb1)
		return -1;
	TBool ehci = aKind[0] == 'e';
	aCfg->base = gUsb1 + (ehci ? 0 : 0x400);
	aCfg->size = ehci ? 0x400 : 0x100;
	aCfg->irq = ehci ? KIntIdEhci1 : KIntIdOhci1;
	return 0;
	}
