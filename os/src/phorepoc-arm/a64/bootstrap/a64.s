;
; a64.s -- OURS (track): the Allwinner A64 bootstrap PSL,
; derived from src/phorepoc-arm/virt/bootstrap/virt.s (itself from baseport/syborg/bootstrap/syborg.s). Deltas from
; virt are marked A64:. UNVERIFIED ON HARDWARE -- written from the A64 manual / Linux DTS / NuttX; no serial cable
; yet, so progress shows on the RGB LED (STAGE_LED below; the stage colours are in the port notes).
;
;/*
;* Copyright (c) 2009 Nokia Corporation and/or its subsidiary(-ies).
;* All rights reserved.
;* This component and the accompanying materials are made available
;* under the terms of the Eclipse Public License v2.0, available at
;* https://www.eclipse.org/legal/epl-2.0/ (relicensed from v1.0 under
;* section 7 of the Eclipse Public License v1.0).
;* This Source Code may also be made available under the following Secondary
;* Licenses when the conditions for such availability set forth in the Eclipse
;* Public License, v. 2.0 are satisfied: GNU General Public License, version 2
;* or later.
;*
;* Initial Contributors:
;* Nokia Corporation - initial contribution.
;*
;* Contributors:
;* The Phorepoc Authors
;* NTT DOCOMO, INC - Fix for bug 1291 "E32test t_tock.exe failed to load Logical Device"
;*
;* Description:
;*
;*/
		
		GBLL	__VARIANT_S__       ; indicates that this is platform-specific code
		GBLL	__A64_S__		; indicates which source file this is

		INCLUDE	bootcpu.inc
		INCLUDE a64.inc
;
;*******************************************************************************
; A64: STAGE_LED $bits -- show a boot stage on the phone's RGB LED (PD18/19/20; a64.inc). Inline, no stack, no
; calls; works with the MMU off (physical PIO) or on (the GetHwBanks mapping). Corrupts r0-r2.
;*******************************************************************************
	MACRO
	STAGE_LED	$bits
		GET_ADDRESS	r1, KHwBaseCcuPio, KHwLinBaseCcuPio
		; + kick WDOG0 (CTRL +0xCB0 = 0xA57 << 1 | 1, a64_glue.cpp WdogKick): boot.cmd arms 16 s for slot B before `go`
		; and the kernel's first kick is stage 3 -- a ~200 MB ROM took longer than that (2026-10-16, boots 581-616)
		ldr		r0, =0x14AF
		str		r0, [r1, #0xCB0]
		add		r1, r1, #0x800					; PIO
		add		r1, r1, #KPioPortDLo			; port D
		ldr		r0, [r1, #KPioCfg2]
		ldr		r2, =0x000FFF00					; PD18-20 function bits
		bic		r0, r0, r2
		ldr		r2, =0x00011100					; = output
		orr		r0, r0, r2
		str		r0, [r1, #KPioCfg2]
		ldr		r0, [r1, #KPioDat]
		bic		r0, r0, #KLedAll
		orr		r0, r0, #$bits
		str		r0, [r1, #KPioDat]
	MEND
;
;*******************************************************************************
;
        IMPORT	ResetEntry
	IF	SMP
        IMPORT	APResetEntry					; bootcpu.s: the AP holding pen
        IMPORT	HandshakeAP						; bootcpu.s: boot CPU side of the AP handshake
	ENDIF
;
;*******************************************************************************
;
        AREA |Boot$$Code|, CODE, READONLY, ALIGN=6
;
;*******************************************************************************
; Initialise Hardware
;	Determine the hardware configuration
;	Determine the reset reason. If it is wakeup from a low power mode, perform
;		whatever reentry sequence is required and jump back to the kernel.
;	Set up the memory controller so that at least some RAM is available
;	Set R10 to point to the super page or to a temporary version of the super page
;		with at least the following fields valid:
;		iBootTable, iCodeBase, iActiveVariant, iCpuId
;	Initialise the debug serial port
;
; Enter with:
;	R12 points to TRomHeader
;	NO STACK
;	R14 = return address (as usual)
;
; All registers may be modified by this call
;*******************************************************************************
	EXPORT	InitialiseHardware
InitialiseHardware	ROUT
	IF	SMP
		; A64: PSCI CPU_ON starts each AP at the shim, which ERETs it here, as on QEMU virt.
		; VIRT SMP: booted from flash, QEMU starts EVERY core at the reset vector. Cores other than 0 go to the
		; generic AP holding pen (bootcpu.s APResetEntry, as NaviEngine's ne1_tb.s does): it sets up this core's
		; GIC interface, receives the AP boot page address over IPIs from FinalInitialise's HandshakeAP, then
		; WFE-polls its iAPBootPtr slot for NKern::BootAP's KickCpu. R11 is the A9 SCU there (it only writes a
		; status word at +8): GICD_IIDR, read-only, so that write is ignored.
		mrc		p15, 0, r0, c0, c0, 5			; MPIDR
		ands	r0, r0, #0xff					; Aff0 = CPU number
		beq		%FT1
		ldr		r8, =KHwBaseGicDist				; physical (MMU off)
		ldr		r9, =KHwBaseGicCpu
		mov		r11, r8
		b		APResetEntry					; NO RETURN
1
	ENDIF
		mov		r13, lr							    ; save return address
		STAGE_LED	KLedRed						; A64: stage 1 = red: the AArch32 bootstrap runs

		adrl	r1, ParameterTable	        ; pass address of parameter table
		bl		InitCpu				        ; initialise CPU/MMU registers
						
;*******************************************************************************
; DoInitialise Hardware
;	Initialise CPU registers
;	Determine the hardware configuration
;	Determine the reset reason. If it is wakeup from a low power mode, perform
;		whatever reentry sequence is required and jump back to the kernel.
;	Set up the memory controller so that at least some RAM is available
;	Set R10 to point to the super page or to a temporary version of the super page
;		with at least the following fields valid:
;		iBootTable, iCodeBase, iActiveVariant, iCpuId
;	In debug builds initialise the debug serial port
;
; Enter with:
;	R12 points to TRomHeader
;	NO STACK
;	R13 = return address (as usual)
;
; All registers may be modified by this call
;*******************************************************************************
DoInitialiseHardware	ROUT
		
; Hardware memory size is 128MB - 32MB reserved for bootloader
		bl		GetRamMb					; the boot config's RAM size (R0, R1 only; LR is free here, R13 = return)
		mov		r4, r0

		bl		InitDebugPort

		ldr		r7, =CFG_HWVD					; variant number

		lsl		r10, r4, #20				    ; R10 = top of RAM
		add		r10, #KHwRamBaseAddr			; VIRT: RAM is not at 0 (A64: 1 GiB too)
		sub		r10, #0x2000				    ; put super page at end for now

; Set up the required super page values
		str		r7, [r10, #SSuperPageBase_iActiveVariant]

		mov		r1, #0
		str		r1, [r10, #SSuperPageBase_iHwStartupReason]	; reset reason (from hardware)

		add		r1, r10, #CpuPageOffset
		str		r1, [r10, #SSuperPageBase_iMachineData]
		bl		GetBootTableAddress
		str		r0, [r10, #SSuperPageBase_iBootTable]		; set the boot function table
		str		r12, [r10, #SSuperPageBase_iCodeBase]		; set the base address of bootstrap code
		mrc		p15, 0, r0, c0, c0, 0						; read CPU ID from CP15 (remove if no CP15)
		str		r0, [r10, #SSuperPageBase_iCpuId]

		mov		r0, r13
		add		sp, r10, #CpuBootStackTop		            ; set up a boot stack
		push    {r0}						                ; save return address
		bl		DoInitHw2						            ; any extra CPU-dependent stuff
				
		ldr		r7, [r10, #SSuperPageBase_iActiveVariant]
		DWORD	r7, "ActiveVariant"

		pop     {pc}						                ; return

;*******************************************************************************
DoInitHw2	ROUT
		mrc		p15, 0, r0, c0, c0, 0
		DWORD	r0, "MMUID"
		mrc		p15, 0, r0, c0, c0, 1
		DWORD	r0, "CacheType"
		mrc		p15, 0, r0, c0, c0, 2
		DWORD	r0, "TCMType"
		mrc		p15, 0, r0, c0, c0, 3
		DWORD	r0, "TLBType"
        bx      lr

;*******************************************************************************
; Get a pointer to the list of hardware banks
;
; The pointer returned should point to a list of hardware banks declared with
; the HW_MAPPING and/or HW_MAPPING_EXT macros. A zero word terminates the list.
; For the direct memory model, all hardware on the system should be mapped here
; and the mapping will set linear address = physical address.
; For the moving or multiple model, only the hardware required to boot the kernel
; and do debug tracing needs to be mapped here. The linear addresses used will
; start at KPrimaryIOBase and step up as required with the order of banks in
; the list being maintained in the linear addresses used.
;
; HW_MAPPING PB, SIZE, MULT
;	This declares a block of I/O with physical base PB and address range SIZE
;	blocks each of which has a size determined by MULT. The page size used for
;	the mapping is determined by MULT. The linear address base of the mapping
;	will be the next free linear address rounded up to the size specified by
;	MULT.
;	The permissions used for the mapping are the standard I/O permissions (BTP_Hw).
;
; HW_MAPPING_EXT PB, SIZE, MULT
;	This declares a block of I/O with physical base PB and address range SIZE
;	blocks each of which has a size determined by MULT. The page size used for
;	the mapping is determined by MULT. The linear address base of the mapping
;	will be the next free linear address rounded up to the size specified by
;	MULT.
;	The permissions used for the mapping are determined by a BTP_ENTRY macro
;	immediately following this macro in the HW bank list or by a DCD directive
;	specifying a different standard permission type.
;
; Configurations without an MMU need not implement this function.
;
; Enter with :
;		R10 points to super page
;		R12 points to ROM header
;		R13 points to valid stack
;
; Leave with :
;		R0 = pointer
;		Nothing else modified
;*******************************************************************************
	EXPORT	GetHwBanks
GetHwBanks	ROUT
		adr		r0, %FT1
		bx      lr
1
		HW_MAPPING		KHwBaseGicDist,		1,	HW_MULT_4K	; A64 (order = a64.inc KHwLinBase*)
		HW_MAPPING		KHwBaseGicCpu,		2,	HW_MULT_4K
		HW_MAPPING		KHwBaseUart0,		1,	HW_MULT_4K
		HW_MAPPING		KHwBaseCcuPio,		1,	HW_MULT_4K
		HW_MAPPING		KHwBaseRPio,		1,	HW_MULT_4K
	IF :DEF: CFG_H3TEST
		HW_MAPPING		KH3BaseCpuCfg,		1,	HW_MULT_4K	; TEST ONLY: the H3 CPUCFG (AP release, above)
	ENDIF

		DCD     0   ; terminator

;*******************************************************************************
; Notify an unrecoverable error during the boot process
;
; Enter with:
;	R14 = address at which fault detected
;
; Don't return
;*******************************************************************************
	EXPORT	Fault
Fault	ROUT
		STAGE_LED	KLedMagenta		; A64: magenta = bootstrap fault (corrupts r0-r2 of the dump)
		b		BasicFaultHandler	; generic handler dumps registers via debug
									; serial port

;*******************************************************************************
; Reboot the system
;
; Enter with:
;		R0 = reboot reason code
;
; Don't return (of course)
;*******************************************************************************
	ALIGN	32, 0
	EXPORT	RestartEntry
RestartEntry	ROUT
		ldr		r0, =0x84000009					; A64: PSCI SYSTEM_RESET (ARM Trusted Firmware); falls through
		DCD		0xE1600070						; smc #0  if it ever returns

; Save R0 parameter in HW dependent register which is preserved over reset
; Put HW specific code here to reset system
	    GETCPSR	r1
		orr		r1, #0xC0
		SETCPSR	r1										; disable interrupts

        ldr     r10, =KSuperPageLinAddr
        adr     r0, Run_Physical
        bl      RomLinearToPhysical                 ; physical address in r0

; Disable MMU
		mrc		p15, 0, r1, c1, c0, 0		        ; get MMUCR
		bic		r1, #MMUCR_M		                ; clear M bit
		mcr		p15, 0, r1, c1, c0, 0		        ; set MMUCR
        bx      r0                                  ; jump to the physical address
        
; Now running from physical address

Run_Physical
        ldr     r3, =KHwNorFlashBaseAddr            ; r3 = the in-RAM ROM image (only if PSCI SYSTEM_RESET returned)
        
; Jump to the NOR flash image
        bx      r3                              

;*******************************************************************************
; Get a pointer to the list of RAM banks
;
; The pointer returned should point to a list of {BASE; MAXSIZE;} pairs, where
; BASE is the physical base address of the bank and MAXSIZE is the maximum
; amount of RAM which may be present in that bank. MAXSIZE should be a power of
; 2 and BASE should be a multiple of MAXSIZE. The generic code will examine the
; specified range of addresses and determine the actual amount of RAM if any
; present in the bank. The list is terminated by an entry with zero size.
;
; The pointer returned will usually be to constant data, but could equally well
; point to RAM if dynamic determination of the list is required.
;
; Enter with :
;		R10 points to super page
;		R12 points to ROM header
;		R13 points to valid stack
;
; Leave with :
;		R0 = pointer
;		Nothing else modified
;*******************************************************************************
	EXPORT	GetRamBanks
GetRamBanks	ROUT
	    push    {r1-r5,lr}
		bl		GetRamMb				; R0 = Symbian's RAM in MB (boot config, validated)
		mov		r4, r0					; R4 = MB still to describe (a multiple of 256)
		mov		r5, #KHwRamBaseAddr		; R5 = next bank's physical base
		add		r0, r10, #CpuPageOffset	;
1		; greedy: the largest of 1 GB / 512 MB / 256 MB that fits -- each bank is a power of 2 and, as the sizes only
		; fall, its base stays aligned to its size (e.g. 2816 MB = 1 GB + 1 GB + 512 MB + 256 MB)
		cmp		r4, #0
		beq		%FT2
		mov		r2, #1024
		cmp		r4, r2
		movlo	r2, #512
		cmp		r4, r2
		movlo	r2, #256
		lsl		r3, r2, #20				; bank size in bytes
		orr		r1, r5, #RAM_VERBATIM	; prevent testing (overlay would break it)
		stm	    r0!, {r1, r3}
		add		r5, r5, r3
		sub		r4, r4, r2
		b		%BT1
2
		mov		r1, #0
		mov		r2, #0
		stm	    r0, {r1-r2}				; terminator
		add		r0, r10, #CpuPageOffset	; R0 = the list start
		pop     {r1-r5,pc}

;*******************************************************************************
; GetRamMb -- R0 = the RAM Symbian uses, in MB. boot.cmd writes {'PHRM', MB} at KHwRamCfgAddr from the user's boot
; config (the SD sector 0x1801); no magic or an unsupported value =
; KHwRamSizeMb. Supported: multiples of 256 from 512 to 2816 (0x40000000..0xF0000000 -- the top 256 MB of the 3 GB
; phone stays outside Symbian: crumbs, later firmware areas; user, 2026-10-12). MMU off (physical read).
; Modifies R0, R1 only; no stack needed.
;*******************************************************************************
GetRamMb	ROUT
		ldr		r0, =KHwRamCfgAddr
		ldr		r1, [r0]
		ldr		r0, =0x4D524850			; 'PHRM'
		cmp		r1, r0
		ldreq	r0, =KHwRamCfgAddr
		ldreq	r0, [r0, #4]
		movne	r0, #KHwRamSizeMb
		tst		r0, #0xFF				; a multiple of 256 MB
		movne	r0, #KHwRamSizeMb
		cmp		r0, #512
		movlo	r0, #KHwRamSizeMb
		cmp		r0, #0xB00				; 2816
		movhi	r0, #KHwRamSizeMb
		bx		lr

;*******************************************************************************
; Get a pointer to the list of ROM banks
;
; The pointer returned should point to a list of entries of SRomBank structures,
; usually declared with the ROM_BANK macro.
; The list is terminated by a zero size entry (four zero words)
;
; ROM_BANK	PB, SIZE, LB, W, T, RS, SS
; PB = physical base address of bank
; SIZE = size of bank
; LB = linear base if override required - usually set this to 0
; W = bus width (ROM_WIDTH_8, ROM_WIDTH_16, ROM_WIDTH_32)
; T = type (see TRomType enum in kernboot.h)
; RS = random speed
; SS = sequential speed
;
; Only PB, SIZE, LB are used by the rest of the bootstrap.
; The information given here can be modified by the SetupRomBank call, if
; dynamic detection and sizing of ROMs is required.
;
; Enter with :
;		R10 points to super page
;		R12 points to ROM header
;		R13 points to valid stack
;
; Leave with :
;		R0 = pointer
;		Nothing else modified
;*******************************************************************************
	EXPORT	GetRomBanks
GetRomBanks	ROUT
        adr		r0, RomBanksFlashTable      ; NOR flash
        bx      lr

RomBanksFlashTable
		ROM_BANK	KHwNorFlashBaseAddr, KHwNorFlashCodeSize, 0, ROM_WIDTH_32, ERomTypeXIPFlash, 0, 0
		DCD		0,0,0,0			; terminator

;*******************************************************************************
; Set up RAM bank
;
; Do any additional RAM controller initialisation for each RAM bank which wasn't
; done by InitialiseHardware.
; Called twice for each RAM bank :-
;	First with R3 = 0xFFFFFFFF before bank has been probed
;	Then, if RAM is present, with R3 indicating validity of each byte lane, ie
;	R3 bit 0=1 if D0-7 are valid, bit1=1 if D8-15 are valid etc.
; For each call R1 specifies the bank physical base address.
;
; Enter with :
;		R10 points to super page
;		R12 points to ROM header
;		R13 points to stack
;		R1 = physical base address of bank
;		R3 = width (bottom 4 bits indicate validity of byte lanes)
;			 0xffffffff = preliminary initialise
;
; Leave with :
;		No registers modified
;*******************************************************************************
	EXPORT	SetupRamBank
SetupRamBank	ROUT
		bx      lr

;*******************************************************************************
; Set up ROM bank
;
; Do any required autodetection and autosizing of ROMs and any additional memory
; controller initialisation for each ROM bank which wasn't done by
; InitialiseHardware.
;
; The first time this function is called R11=0 and R0 points to the list of
; ROM banks returned by the BTF_RomBanks call. This allows any preliminary setup
; before autodetection begins.
;
; This function is subsequently called once for each ROM bank with R11 pointing
; to the current information held about that ROM bank (SRomBank structure).
; The structure pointed to by R11 should be updated with the size and width
; determined. The size should be set to zero if there is no ROM present in the
; bank.
;
; Enter with :
;		R10 points to super page
;		R12 points to ROM header
;		R13 points to stack
;		R11 points to SRomBank info for this bank
;		R11 = 0 for preliminary initialise (all banks)
;
; Leave with :
;		Update SRomBank info with detected size/width
;		Set the size field to 0 if the ROM bank is absent
;		Can modify R0-R4 but not other registers
;
;*******************************************************************************
	EXPORT	SetupRomBank
SetupRomBank	ROUT						    ; only get here if running from ROM
		cmp		r11, #0
		bxeq	lr						        ; don't do anything for preliminary
		ldm	    r11, {r0,r1}				    ; r0 = base, r1 = size
        lsr     r0, pc, #20
        lsl     r0, #20                         ; r0 = image base
   		ldr		r1, [r12, #TRomHeader_iRomSize] ; r1 = size of ROM block
		stm 	r11, {r0,r1}
        bx      lr

;*******************************************************************************
; Reserve physical memory
;
; Reserve any physical RAM needed for platform-specific purposes before the
; bootstrap begins allocating RAM for page tables/kernel data etc.
;
; There are two methods for this:
;	1.	The function ExciseRamArea may be used. This will remove a contiguous
;		region of physical RAM from the RAM bank list. That region will never
;		again be identified as RAM.
;	2.	A list of excluded physical address ranges may be written at [R11].
;		This should be a list of (base,size) pairs terminated by a (0,0) entry.
;		This RAM will still be identified as RAM by the kernel but will not
;		be allocated by the bootstrap and will subsequently be marked as
;		allocated by the kernel immediately after boot.
;
; Enter with :
;		R10 points to super page
;		R11 indicates where preallocated RAM list should be written.
;		R12 points to ROM header
;		R13 points to stack
;
; Leave with :
;		R0-R3 may be modified. Other registers should be preserved.
;*******************************************************************************
	EXPORT	ReservePhysicalMemory
ReservePhysicalMemory	ROUT
		; A64 (as virt CFG_ROMINRAM): the ROM image needs nothing here -- its code base lies in the RAM bank, so
		; bootmain sets KBootFlagRunFromRAM and excises it itself (as the 2010 beagleboard port, which U-Boot also
		; loaded into RAM). Only the shim's page must survive: PSCI CPU_ON enters every AP there (method 2).
		ldr		r0, =KHwShimBase
		mov		r1, #0x1000
		mov		r2, #0
		mov		r3, #0
		stm		r11, {r0-r3}
		bx      lr

;*******************************************************************************
; Do final platform-specific initialisation before booting the kernel
;
; Typical uses for this call would be:
;	1.	Mapping cache flushing areas
;	2.	Setting up pointers to routines in the bootstrap which are used by
;		the variant or drivers (eg idle code).
;
; Enter with :
;		R10 points to super page
;		R11 points to TRomImageHeader for the kernel
;		R12 points to ROM header
;		R13 points to stack
;
; Leave with :
;		R0-R9 may be modified. Other registers should be preserved.
;
;*******************************************************************************
	EXPORT	FinalInitialise
FinalInitialise ROUT
		STAGE_LED	KLedYellow		; A64: stage 2 = yellow: MMU on, bootstrap done, the kernel is next
	IF	SMP
		; VIRT SMP: hand each AP the AP boot page (NaviEngine's FinalInitialise loop). MMU on: the GIC at its
		; GetHwBanks virtual addresses (bank 0 = distributor, bank 1 = CPU interface). The CPU count = GICD_TYPER
		; CPUNumber (bits 7:5) + 1 -- the GIC has one CPU interface per core (QEMU: -smp N). R7 is the A9 SCU for
		; HandshakeAP, which only reads its +4 for a trace: the distributor again.
		stmfd	sp!, {lr}
		; P5 (power plan, core retirement): tell the kernel where the AP boot code is. A core the kernel retires (PSCI CPU_OFF)
		; is restarted by PSCI CPU_ON at the shim, which jumps to APBootCodeEntry with the core's saved register set
		; (a64_idle.cpp; a53shim/shim.S) instead of coming through the holding pen. Its physical address goes to the
		; super page's iReserved[0] (SSuperPageBase byte offset 164, kernboot.h: after iPlatformSpecificMappings at 160).
		bl		GetAPBootCodePhysAddr			; (bootcpu.inc imports it) ADR with the MMU on = the LINEAR address ...
		bl		RomLinearToPhysical				; ... as RestartEntry's `adr` below: -> physical, in r0 (r10 = the super page)
		str		r0, [r10, #164]
		ldr		r8, =KPrimaryIOBase				; GIC distributor (virtual)
		add		r9, r8, #0x1000					; GIC CPU interface (virtual)
		mov		r7, r8
		ldr		r5, [r8, #4]					; GICD_TYPER
		mov		r5, r5, lsr #5
		and		r5, r5, #7
		add		r5, r5, #1						; r5 = number of CPUs
	IF :DEF: CFG_ONECPU
		mov		r5, #1							; BOOT_DEFS='-D CFG_ONECPU': start no APs (race experiments)
	ENDIF
		DWORD	r5, "NCPUs"
		mov		r6, #1							; first AP
		b		%FT2
1
		; A64: the APs are off until ARM Trusted Firmware's PSCI CPU_ON (SMC) starts them -- at the shim, which drops
		; them to AArch32 and into the holding pen above (target MPIDR = the CPU number: one cluster).
		ldr		r0, =0x84000003					; PSCI CPU_ON (SMC32 calling convention)
		mov		r1, r6
		ldr		r2, =KHwShimBase				; A64: entry = the shim (AArch64 EL2 -> AArch32), as U-Boot's `go`
		mov		r3, #0
	IF :DEF: CFG_H3TEST
		; TEST ONLY (run_h3.sh, QEMU orangepi-pc = Allwinner H3): no PSCI there (on real H3 boards U-Boot's secure
		; monitor provides it) -- QEMU models the H3 CPUCFG block instead: entry address, then release core r1.
		ldr		r0, =KH3LinCpuCfg
		str		r2, [r0, #0x1A4]				; CPUCFG ENTRY_ADDR
		add		r0, r0, r1, lsl #6				; CPUn reset control at +0x40 + n*0x40
		mov		r3, #3
		str		r3, [r0, #0x40]					; CORE_RESET | RESET deasserted -> the core starts at ENTRY_ADDR
		mov		r0, #0
	ELSE
		DCD		0xE1600070						; smc #0 (ARM Trusted Firmware's PSCI)
	ENDIF
		DWORD	r0, "PSCI CPU_ON"
		DWORD	r6, "Handshake AP"
		bl		HandshakeAP
		add		r6, r6, #1
2
		cmp		r6, r5
		blo		%BT1
		ldmfd	sp!, {pc}
	ELSE
		bx      lr
	ENDIF

;*******************************************************************************
; Debug port write routine associated with debug port in the super page
; Enter with :
;		R0  character to be written
;		R12 points to rom header
;		R13 points to valid stack
;
; Leave with :
;		nothing modified 
;*******************************************************************************
	EXPORT	DoWriteC
DoWriteC	ROUT
	IF	CFG_DebugBootRom
		push    {r1,lr}
		bl		GetDebugPortBase			; r1 = base address of UART registers
	
1		ldr		lr, [r1, #KUartLsr]			; A64: 16550 -- wait for THR empty (lr is saved)
		tst		lr, #KUartLsrThre
		beq		%BT1
		str		r0, [r1, #KUartThr]

		pop     {r1,pc}
	ELSE
		bx      lr
	ENDIF

;*******************************************************************************
; Initialise the debug port
;
; Enter with :
;		R12 points to ROM header
;		There is no valid stack
;
; Leave with :
;		R0-R2 modified
;		Other registers unmodified
;*******************************************************************************
InitDebugPort	ROUT
		bx		lr								; A64: U-Boot left UART0 at 115200 8N1

;*******************************************************************************
; Get the base address of the debug UART
;
; Enter with :
;		R12 points to ROM header
;		There may be no stack
;
; Leave with :
;		R1 = base address of port, 0 for JTAG
;		Z flag set for JTAG, clear for non-JTAG
;		No other registers modified
;*******************************************************************************
GetDebugPortBase	ROUT
		ldr		r1, [r12, #TRomHeader_iDebugPort]
		cmp		r1, #42							; JTAG?
		moveqs	r1, #0
		bxeq	lr							    ; yes - return 0 and set Z
		cmp		r1, #1
        blo     GetUartPort0          
        beq     GetUartPort1
		cmp		r1, #3
        blo     GetUartPort2
		beq		GetUartPort3
GetUartPort0
		GET_ADDRESS	r1, KHwBaseUart0, KHwLinBaseUart0
		movs	r1, r1							; clear Z
		bx      lr

GetUartPort1
		GET_ADDRESS	r1, KHwBaseUart1, KHwLinBaseUart1
		movs	r1, r1							; clear Z
		bx      lr

GetUartPort2
		GET_ADDRESS	r1, KHwBaseUart2, KHwLinBaseUart2
		movs	r1, r1							; clear Z
		bx      lr

GetUartPort3
		GET_ADDRESS	r1, KHwBaseUart3, KHwLinBaseUart3
		movs	r1, r1							; clear Z
		bx      lr

;*******************************************************************************
; Return parameter specified by R0 (see TBootParam enum)
;
; Enter with :
;		R0 = parameter number
;
; Leave with :
;		If parameter value is supplied, R0 = value and N flag clear
;		If parameter value is not supplied, N flag set. In this case the
;		parameter may be defaulted or the system may fault.
;		R0, R1 modified. No other registers modified.
;
;*******************************************************************************
GetParameters ROUT
		adr		r1, ParameterTable
		b		FindParameter

ParameterTable
		DCD		-1								; terminator

;*******************************************************************************
; BOOT FUNCTION TABLE
;*******************************************************************************
GetBootTableAddress	ROUT
		adr		r0, A64BootTable
        bx      lr

A64BootTable
		DCD	DoWriteC					; output a debug character
		DCD	GetRamBanks					; get list of RAM banks
		DCD	SetupRamBank				; set up a RAM bank
		DCD	GetRomBanks					; get list of ROM banks
		DCD	SetupRomBank				; set up a ROM bank
		DCD	GetHwBanks					; get list of HW banks
		DCD	ReservePhysicalMemory		; reserve physical RAM if required
		DCD	GetParameters				; get addresses for direct memory model
		DCD	FinalInitialise				; Final initialisation before booting the kernel
		DCD	HandleAllocRequest			; allocate memory (usually in generic code)
		DCD	GetPdeValue					; usually in generic code
		DCD	GetPteValue					; usually in generic code
		DCD	PageTableUpdate				; usually in generic code
		DCD	EnableMmu					; Enable the MMU (usually in generic code)

; These entries specify the standard MMU permissions for various areas
		
; VIRT: the table serves the flexible model too (syborg.s had it under CFG_MMMultiple only, the CFG_MMFlexible
; line commented out -- a flexible build then read garbage PDE/PTE permissions: domain 14 -> fault at EnableMmu)
	IF  CFG_MMMultiple :LOR: CFG_MMFlexible
	    IF CFG_ARMV7
		BTP_ENTRY   CLIENT_DOMAIN, PERM_RORO, MEMORY_FULLY_CACHED,       	1,  1,  0,  0   ; ROM
		BTP_ENTRY   CLIENT_DOMAIN, PERM_RWNO, MEMORY_FULLY_CACHED,       	0,  1,  0,  0   ; kernel data/stack/heap
		BTP_ENTRY   CLIENT_DOMAIN, PERM_RWNO, MEMORY_FULLY_CACHED,       	0,  1,  0,  0   ; super page/CPU page
		BTP_ENTRY   CLIENT_DOMAIN, PERM_RWNO, MEMORY_FULLY_CACHED,  	0,  1,  0,  0   ; page directory/tables
		BTP_ENTRY   CLIENT_DOMAIN, PERM_RONO, MEMORY_FULLY_CACHED,       	1,  1,  0,  0   ; exception vectors
		BTP_ENTRY   CLIENT_DOMAIN, PERM_RWNO, MEMORY_STRONGLY_ORDERED,      0,  1,  0,  0   ; hardware registers
		DCD         0                                                           ; unused (minicache flush)
		DCD         0                                                           ; unused (maincache flush)
		BTP_ENTRY   CLIENT_DOMAIN, PERM_RWNO, MEMORY_FULLY_CACHED,       	0,  1,  0,  0   ; page table info
		BTP_ENTRY   CLIENT_DOMAIN, PERM_RWRW, MEMORY_FULLY_CACHED,       	1,  1,  0,  0   ; user RAM
		BTP_ENTRY   CLIENT_DOMAIN, PERM_RONO, MEMORY_STRONGLY_ORDERED,      1,  1,  0,  0   ; temporary identity mapping
		BTP_ENTRY   CLIENT_DOMAIN, UNC_PERM,  MEMORY_STRONGLY_ORDERED,      0,  1,  0,  0   ; uncached
	    ENDIF	
	    IF CFG_ARMV6
		BTP_ENTRY   CLIENT_DOMAIN, PERM_RORO, CACHE_WBWA,       1,  1,  0,  0       ; ROM
	        BTP_ENTRY   CLIENT_DOMAIN, PERM_RWNO, CACHE_WBWA,       0,  1,  0,  0       ; kernel data/stack/heap
	        BTP_ENTRY   CLIENT_DOMAIN, PERM_RWNO, CACHE_WBWA,       0,  1,  0,  0       ; super page/CPU page
		BTP_ENTRY   CLIENT_DOMAIN, PERM_RWNO, CACHE_WBWA,		0,  1,  0,  0       ; page directory/tables
	        BTP_ENTRY   CLIENT_DOMAIN, PERM_RONO, CACHE_WTRA,       1,  1,  0,  0       ; exception vectors
		BTP_ENTRY   CLIENT_DOMAIN, PERM_RWNO, CACHE_SO,         0,  1,  0,  0       ; hardware registers
		DCD			0										; unused (minicache flush)
		DCD			0										; unused (maincache flush)
	        BTP_ENTRY   CLIENT_DOMAIN, PERM_RWNO, CACHE_WBWA,       0,  1,  0,  0       ; page table info
	        BTP_ENTRY   CLIENT_DOMAIN, PERM_RWRW, CACHE_WBWA,       1,  1,  0,  0       ; user RAM
		BTP_ENTRY   CLIENT_DOMAIN, PERM_RONO, CACHE_SO,         1,  1,  0,  0       ; temporary identity mapping
	        BTP_ENTRY   CLIENT_DOMAIN, UNC_PERM,  CACHE_SO,         0,  1,  0,  0       ; uncached
	    ENDIF
	ENDIF
	IF CFG_MMMoving
		BTP_ENTRY	CLIENT_DOMAIN, PERM_RORO, CACHE_WT		; ROM
		BTP_ENTRY	CLIENT_DOMAIN, PERM_RWNO, CACHE_WB		; kernel data/stack/heap
		BTP_ENTRY	CLIENT_DOMAIN, PERM_RWNO, CACHE_WB		; super page/CPU page
		BTP_ENTRY	CLIENT_DOMAIN, PERM_RWNO, CACHE_WT		; page directory/tables
		BTP_ENTRY	CLIENT_DOMAIN, PERM_RORO, CACHE_WT		; exception vectors
		BTP_ENTRY	CLIENT_DOMAIN, PERM_RWRO, CACHE_NCNB	; hardware registers
		DCD			0										; unused (minicache flush)
		DCD			0										; unused (maincache flush)
		BTP_ENTRY	CLIENT_DOMAIN, PERM_RWNO, CACHE_WB		; page table info
		BTP_ENTRY	CLIENT_DOMAIN, PERM_RWRW, CACHE_WB		; user RAM
		BTP_ENTRY	CLIENT_DOMAIN, PERM_RORO, CACHE_NCNB	; temporary identity mapping
		BTP_ENTRY	CLIENT_DOMAIN, UNC_PERM,  CACHE_NCNB	; uncached
	ENDIF

	END
