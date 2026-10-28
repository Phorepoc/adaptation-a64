; a64.inc -- OURS (track): the Allwinner A64 memory map,
; the sibling of src/phorepoc-arm/virt/bootstrap/virt.inc. UNVERIFIED ON HARDWARE (no serial cable yet): addresses
; from the A64 user manual's memory map as the Linux DTS (arch/arm64/boot/dts/allwinner/sun50i-a64.dtsi,
; sun50i-a64-pinephone.dtsi) and NuttX (arch/arm64/src/a64) use them -- read as hardware documentation only. layering: ok

; RAM: DRAM at 1 GiB. Every phone has >= 2 GiB; the first rung uses 1 GiB (one power-of-two bank, base a
; multiple of its size, as GetRamBanks' contract asks). Raise later.
KHwRamBaseAddr		EQU	0x40000000
KHwRamSizeMb		EQU	1024		; the DEFAULT; the user's boot config overrides it (GetRamMb, a64.s)
KHwRamCfgAddr		EQU	0x402FFFF8	; {'PHRM', MB}: written by boot.cmd from the SD boot-config sector 0x1801
					; (the last 8 bytes of the shim's 1 MB pad, just below the ROM)

; The U-Boot boot shape (as virt CFG_ROMINRAM, P3 rung 3, proven in QEMU): `load mmc ... 0x40200000 symbian.img;
; go 0x40200000`. The image = the AArch64 shim (src/phorepoc-arm/a53shim, TARGET 0x40300000) padded to 1 MB, then
; the ROM. U-Boot runs at EL2 in AArch64; PSCI (ARM Trusted Firmware, SMC) restarts the APs at the shim too.
KHwShimBase		EQU	0x40200000
KHwNorFlashBaseAddr	EQU	0x40300000	; the XIP ROM image, in RAM (bootmain excises it: KBootFlagRunFromRAM)
KHwNorFlashSize		EQU	0x04000000
KHwNorFlashCodeSize	EQU	0x02000000

; Peripherals (A64 user manual memory map)
KHwBaseCcuPio		EQU	0x01C20000	; CCU (+0x000) and PIO (+0x800) share this 4K page
KHwBaseUart0		EQU	0x01C28000	; UART0, 16550-compatible, 32-bit register stride; U-Boot's console, 115200 8N1
KHwBaseGicDist		EQU	0x01C81000	; GIC-400 distributor
KHwBaseGicCpu		EQU	0x01C82000	; GIC-400 CPU interface (8K; the registers we use are in the first page)
KHwBaseRPio		EQU	0x01F02000	; R_PIO (port L) at +0xC00 -- the power key's PMIC IRQ and more, later

; PIO: port n's registers at +0x800 + n*0x24: CFG0..3 (+0x00..+0x0C, 4 bits per pin, 1 = output), DAT (+0x10).
; phone (sun50i-a64-pinephone.dtsi): RGB LED = PD18 green, PD19 red, PD20 blue; vibrator = PD2 (gpio-vibrator, layering: ok
; its supply DCDC1 is always on).
KPioPortD		EQU	0x800 + 3*0x24	; offset of port D in the CCU/PIO page
KPioCfg0		EQU	0x00
KPioCfg2		EQU	0x08
KPioDat			EQU	0x10
KLedGreen		EQU	1 :SHL: 18
KLedRed			EQU	1 :SHL: 19
KLedBlue		EQU	1 :SHL: 20
KVibrator		EQU	1 :SHL: 2
KLedAll			EQU	KLedGreen :OR: KLedRed :OR: KLedBlue
KLedYellow		EQU	KLedGreen :OR: KLedRed
KLedMagenta		EQU	KLedRed :OR: KLedBlue
KPioPortDLo		EQU	3*0x24		; KPioPortD = 0x800 + this (no single ARM immediate)

; 16550 registers (32-bit stride): THR +0x00, LSR +0x14 (bit 5 = THR empty)
KUartThr		EQU	0x00
KUartLsr		EQU	0x14
KUartLsrThre		EQU	0x20

KHwLinSeparation	EQU	0x1000
KHwLinBaseRegisters	EQU	KPrimaryIOBase

; linear order = GetHwBanks order (src/phorepoc-arm/virt/specific/syborg.h __A64__ mirrors it)
KHwLinBaseGicDist	EQU	KHwLinBaseRegisters + 0x00*KHwLinSeparation
KHwLinBaseGicCpu	EQU	KHwLinBaseRegisters + 0x01*KHwLinSeparation	; 2 pages
KHwLinBaseUart0		EQU	KHwLinBaseRegisters + 0x03*KHwLinSeparation
KHwLinBaseCcuPio	EQU	KHwLinBaseRegisters + 0x04*KHwLinSeparation
KHwLinBaseRPio		EQU	KHwLinBaseRegisters + 0x05*KHwLinSeparation

; TEST ONLY (CFG_H3TEST, tools/phorepoc-arm/run_h3.sh): QEMU orangepi-pc's H3 CPUCFG releases the APs (no PSCI there)
KH3BaseCpuCfg		EQU	0x01F01000	; CPUCFG at +0xC00 of this page
KH3LinCpuCfg		EQU	KHwLinBaseRegisters + 0x06*KHwLinSeparation + 0xC00

; one debug UART: every port number maps to UART0
KHwBaseUart1		EQU	KHwBaseUart0
KHwBaseUart2		EQU	KHwBaseUart0
KHwBaseUart3		EQU	KHwBaseUart0
KHwLinBaseUart1		EQU	KHwLinBaseUart0
KHwLinBaseUart2		EQU	KHwLinBaseUart0
KHwLinBaseUart3		EQU	KHwLinBaseUart0

	END
