/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later */

@ minisec.s -- a minimal from-source USER-mode "secondary" EXE for Track B.
@ The kernel launches the ROM's iSecondaryFile (normally EFILE.EXE) as the first user process
@ (exstart.cpp: StartSecondary -> Kern::ProcessCreate + Resume). This stands in for it: a tiny
@ position-independent user program that proves the user->kernel SWI/exec path in the
@ from-source kernel.
@
@ It does two things, in order:
@   1) A ONE-SHOT RDisplayChannel::Open smoke. RDisplayChannel::Open(aScreen) is exactly
@        RBusLogicalChannel::DoCreate("displaychannel", TVersion(1,0,1), aScreen, NULL, NULL)
@      which lands on Exec::ChannelCreate (slow exec 132 = swi 0x84):
@        r0 = const TDesC8& aLogicalDevice   (the LDD name)
@        r1 = TChannelCreateInfo8&           (iVersion, iUnit, iPhysicalDevice, iInfo)
@        r2 = TInt aType                     (EOwnerProcess = 1)
@        -> r0 = handle (>=0) or a system error (<0).
@      This drives the M6a-registered LDD/PDD `Create`/CreateChannelSetup path AND is the first
@      thing to queue work to the priority-29 display DFC thread, so a success return confirms
@      that thread is live/scheduled (resolves the M6a ThreadResume open thread). We print the
@      raw result as hex so the exact handle/error is visible on the UART.
@   2) The original "[USERLAND] hello" loop (unchanged), so boot proceeds identically after the
@      smoke -- non-regression of the M4/M5.5 observable behavior.
@
@ Fully PIC: all string/struct addresses are PC-relative (adr) and descriptors are built on the
@ user stack, so the image can be linked/placed anywhere and executes in place from its ROM
@ address. Slow execs preserve the user callee-saved registers (r4-r11), so r5 (the create
@ result) survives the print SWIs.
@
@ Exec::DebugPrint(TAny* aDes, TInt aMode): r0=&descriptor (a TDesC8), r1=aMode(1 => 8-bit desc).
@ Descriptor = TPtrC8 { word0 = length | (EPtrC=1 << 28), word1 = data ptr }.

	.text
	.arm
	.align	2
	.global	_start
_start:
	@ ---- 1) announce the smoke ----
	adr	r0, msg_open
	mov	r1, #(msg_open_end - msg_open)
	bl	print_des

	@ ---- build TChannelCreateInfo (16 bytes) on the stack ----
	@ iVersion(+0)=TVersion(1,0,1) packed = 0x00010001 (iMajor=1,iMinor=0,iBuild=1)
	@ iUnit(+4)=0 (screen 0)   iPhysicalDevice(+8)=NULL   iInfo(+12)=NULL
	sub	sp, sp, #16
	mov	r3, #0x10000
	orr	r3, r3, #1			@ 0x00010001
	str	r3, [sp, #0]
	mov	r3, #0
	str	r3, [sp, #4]
	str	r3, [sp, #8]
	str	r3, [sp, #12]

	@ ---- build the LDD-name descriptor "displaychannel" (TPtrC8) on the stack ----
	sub	sp, sp, #8
	adr	r2, name_disp
	mov	r3, #(name_disp_end - name_disp)	@ length (= 14)
	orr	r3, r3, #0x10000000			@ | (EPtrC << 28)
	str	r3, [sp, #0]
	str	r2, [sp, #4]

	mov	r0, sp				@ r0 = &name descriptor (TDesC8)
	add	r1, sp, #8			@ r1 = &TChannelCreateInfo (8 bytes above name desc)
	mov	r2, #1				@ r2 = aType = EOwnerProcess
	swi	0x84				@ Exec::ChannelCreate (slow exec 132)
	mov	r5, r0				@ save result (handle >=0, or error <0)
	add	sp, sp, #24			@ pop name desc (8) + info (16)

	@ ---- print "  ChannelCreate r=0x" + hex(r5) + "\n" ----
	adr	r0, msg_res
	mov	r1, #(msg_res_end - msg_res)
	bl	print_des

	sub	sp, sp, #16			@ scratch buffer for hex + newline
	mov	r0, r5
	mov	r1, sp
	bl	hex8				@ writes 8 ascii hex chars at [sp]
	mov	r3, #0x0a
	strb	r3, [sp, #8]			@ trailing newline
	mov	r0, sp
	mov	r1, #9				@ 8 hex + '\n'
	bl	print_des
	add	sp, sp, #16

	@ ---- 2) fall into the original hello loop (unchanged behavior) ----
hello:
	sub	sp, sp, #16
	adr	r2, msg				@ r2 = runtime (ROM) address of the string (PC-relative)
	mov	r3, #(msgend - msg)		@ length
	orr	r3, r3, #0x10000000		@ | (EPtrC << 28)  -> descriptor word0
	str	r3, [sp, #0]
	str	r2, [sp, #4]			@ descriptor word1 = data ptr
	mov	r0, sp				@ r0 = &descriptor
	mov	r1, #1				@ mode = 1 (8-bit descriptor)
	swi	0x56				@ Exec::DebugPrint (slow exec 86)
	add	sp, sp, #16
	@ crude delay so we don't flood the UART, then repeat
	ldr	r4, =0x00800000
1:	subs	r4, r4, #1
	bne	1b
	b	hello

@ ---- print_des: print a TDesC8. r0 = data ptr, r1 = length. Clobbers r0-r3; preserves r4-r11. ----
print_des:
	push	{r4, lr}
	sub	sp, sp, #8
	orr	r3, r1, #0x10000000		@ word0 = len | (EPtrC << 28)
	str	r3, [sp, #0]
	str	r0, [sp, #4]			@ word1 = data ptr
	mov	r0, sp
	mov	r1, #1
	swi	0x56				@ Exec::DebugPrint
	add	sp, sp, #8
	pop	{r4, lr}
	bx	lr

@ ---- hex8: r0 = value, r1 = dest buffer. Writes 8 ascii hex chars (MS nibble first). ----
hex8:
	push	{r4, lr}
	mov	r2, #8
2:	mov	r3, r0, lsr #28
	and	r3, r3, #0xf
	cmp	r3, #9
	addhi	r3, r3, #('a' - 10)
	addls	r3, r3, #'0'
	strb	r3, [r1], #1
	mov	r0, r0, lsl #4
	subs	r2, r2, #1
	bne	2b
	pop	{r4, lr}
	bx	lr

	.ltorg
	.align	2
name_disp:
	.ascii	"displaychannel"
name_disp_end:
	.align	2
msg_open:
	.ascii	"[USERLAND] RDisplayChannel::Open smoke -> Exec::ChannelCreate(displaychannel)\n"
msg_open_end:
	.align	2
msg_res:
	.ascii	"[USERLAND]   ChannelCreate r=0x"
msg_res_end:
	.align	2
msg:
	.ascii	"[USERLAND] hello from the from-source secondary!\n"
msgend:
	.align	2
