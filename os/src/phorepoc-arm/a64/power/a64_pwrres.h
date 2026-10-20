// a64_pwrres.h -- the A64 power resource manager's ids (a64_pwrres.cpp; docs/phorepoc/power-plan.md P4).
#ifndef A64_PWRRES_H
#define A64_PWRRES_H

enum TA64Res
	{
	KA64ResDldo1,		// AXP803 DLDO1 3.3 V: DSI link supply + IMU/magnetometer (screen, sensors)
	KA64ResDldo2,		// AXP803 DLDO2 1.8 V: MIPI IO (screen)
	KA64ResLdoIo0,		// AXP803 GPIO0-LDO 3.3 V: touch, STK3311, panel VCC (never released by the screen: tap-to-wake)
	KA64ResGpuClk,		// PLL_GPU + GPU_CLK (compositor while the screen scans)
	KA64ResVeClk,		// PLL_VE + VE_CLK
	KA64ResCount
	};

extern "C" TInt A64PwrResGet(TInt aId);			// take a reference; the first one switches the resource on
extern "C" void A64PwrResPut(TInt aId);			// drop one; the last one switches it off
extern "C" TInt A64PwrResRefs(TInt aId);
extern "C" void A64PwrResDump();

#endif
