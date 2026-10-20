; config.inc -- OURS: bootstrap config for the Allwinner A64 / PinePhone (4 x Cortex-A53 in AArch32), from
; src/phorepoc-arm/virt/config.inc. CPU config stays the generic ARMv7 one (A8N), as on virt's A53 rung; the
; ARMv8 deltas (no ThumbEE) come from -D CFG_A53, which BOARD=a64 always passes (tools/armasm2gas/build_bootstrap.sh).
	GBLL    CFG_CPU_CORTEX_A8N
	INIT_NUMERIC_CONSTANT	CFG_HWVD, 0x0B0A0001
; SCTLR = bootcpu.inc's ARMv7 (A8) value WITHOUT MMUCR_A (alignment checking), as Linux runs ARMv7: S60 reads the
; bitmap font's 16-bit tables in place from the ROM at odd addresses (fntstore's XIP path; real phones loaded fonts
; from NAND into RAM) -- an unaligned LDRH the A53 does in hardware, but which faults with A set (alignment fault in
; akncapserver, then Kern::Fault -- magenta LED, 2026-10-06). LDM/LDRD/exclusives still require alignment.
	INIT_NUMERIC_CONSTANT	InitialMMUCR,	MMUCR_W+MMUCR_SBO+MMUCR_I+MMUCR_DT+MMUCR_IT+MMUCR_U+MMUCR_XP+MMUCR_TRE+MMUCR_FA
	END
