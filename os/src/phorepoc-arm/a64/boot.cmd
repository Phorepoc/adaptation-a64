# Phorepoc (Symbian) on the PinePhone -- the U-Boot boot script (OURS).
# tools/phorepoc-arm/make_pinephone_image.sh turns this into boot.scr. U-Boot's distro boot finds boot.scr on the
# boot partition and sets devtype/devnum/distro_bootpart; run by hand they default to the SD card's first partition.
# symbian.img = the AArch64 shim (padded to 1 MB) + the ROM: loaded at 0x40200000, the ROM lands at 0x40300000 --
# the addresses the A64 bootstrap expects (src/phorepoc-arm/a64/bootstrap/a64.inc). U-Boot's `go` does not clean
# its caches, and the shim + bootstrap run with the MMU (and so the caches) off: flush and disable them first.
#
# A/B SLOTS (a64_glue.cpp "A/B SLOTS"): slot A = symbian.img on this FAT partition (golden); slot B
# = partition 2, raw sectors from 0x220000 (1088 MiB; tools/phorepoc-arm/sd_layout.py), written over USB by phlink. The control sector 0x1800 = {'PHSL', state, len, sum}:
#   PENDING (1) -> mark TRYING (2), boot B once, watchdog armed (a hang resets -> A)
#   TRYING -> mark DONE (4), boot A (whose kernel replays B's SD log);   anything else -> A. Slot B never boots again until re-armed (phlink upload, or phlink 'B' = re-arm + reboot).
# LED NARRATION (no console): red = the script started; yellow = slot A loaded, green = slot B loaded; blue = go.
gpio clear PD18
gpio set PD19
gpio clear PD20
# CHARGER (2026-10-07; as charge.cmd, tested on the phone): before ANY OS -- whatever boots next, the AXP803 charges.
# RSB bring-up (a64_display.cpp RsbInit), REG 33H bit 7 = charge enable, REG 30H bits 1:0 = 2 (2 A VBUS input limit).
# LOW-BATTERY GUARD: battery voltage high byte (REG 78H; V = raw12 * 1.1 mV) below 0xC4 (~3.45 V) = too low to boot
# the system (it browns out -> reset -> boot loop): stay here -- one core, no display -- with a red blink every 10 s
# until it reaches 0xCC (~3.6 V). 60 checks (~10 min) with REG 01H bit 6 (charging) clear = power off (REG 32H bit 7).
setexpr.l t *0x01F01428
setexpr t ${t} "|" 8
mw.l 0x01F01428 ${t}
setexpr.l t *0x01F014B0
setexpr t ${t} "|" 8
mw.l 0x01F014B0 ${t}
setexpr.l t *0x01F02C00
setexpr t ${t} "&" ffffff00
setexpr t ${t} "|" 22
mw.l 0x01F02C00 ${t}
mw.l 0x01F03400 1
sleep 0.1
mw.l 0x01F03404 0x203
mw.l 0x01F03428 0x807c3e00
sleep 0.1
setexpr.l t *0x01F0340C
mw.l 0x01F0340C ${t}
mw.l 0x01F0342C 0xe8
mw.l 0x01F03430 0x2d03a3
mw.l 0x01F03400 0x80
sleep 0.1
setexpr.l t *0x01F0340C
mw.l 0x01F0340C ${t}
setenv rsbrd 'setexpr.l s *0x01F0340C; mw.l 0x01F0340C ${s}; mw.l 0x01F0342C 0x8b; mw.l 0x01F03430 0x2d0000; mw.l 0x01F03410 ${reg}; mw.l 0x01F03400 0x80; sleep 0.1; setexpr.b val *0x01F0341C'
setenv rsbwr 'setexpr.l s *0x01F0340C; mw.l 0x01F0340C ${s}; mw.l 0x01F0342C 0x4e; mw.l 0x01F03430 0x2d0000; mw.l 0x01F03410 ${reg}; mw.l 0x01F0341C ${val}; mw.l 0x01F03400 0x80; sleep 0.1'
setenv reg 33; run rsbrd
setexpr val ${val} "|" 80
run rsbwr
setenv reg 30; run rsbrd
setexpr val ${val} "&" fc
setexpr val ${val} "|" 2
run rsbwr
# THE USB input limit: REG 35H bits 7:4 (Linux axp20x_usb_power, AXP813) -- 5 = 2.5 A (default 1 = 500 mA)
setenv reg 35; run rsbrd
setexpr val ${val} "&" 0f
setexpr val ${val} "|" 50
run rsbwr
# TS pin = ADC, battery-temperature monitoring off (REG 84H bits 2:0 = 100): at the reset default it blocks charging
setenv reg 84; run rsbrd
setexpr val ${val} "&" f8
setexpr val ${val} "|" 4
run rsbwr
# battery voltage high byte -> lowbat = 1 when below the threshold (unsigned: a negative difference has bit 31 set)
setenv battchk 'setenv reg 78; run rsbrd; setexpr d ${val} - ${thr}; setexpr lowbat ${d} / 80000000'
setenv thr c4
run battchk
if test "${lowbat}" = 1; then
	echo "Phorepoc: battery low -- charging before boot"
	setenv thr cc
	setenv nochg 0
	while test "${lowbat}" = 1; do
		gpio clear PD19
		sleep 9
		gpio set PD19
		sleep 1
		setenv reg 1; run rsbrd
		setexpr chg ${val} "&" 40
		if test "${chg}" = 40; then setenv nochg 0; else setexpr nochg ${nochg} + 1; fi
		if test "${nochg}" = 3c; then
			echo "Phorepoc: not charging -- power off"
			setenv reg 32; run rsbrd
			setexpr val ${val} "|" 80
			run rsbwr
		fi
		run battchk
	done
fi
sleep 1
if test -z "${devtype}"; then setenv devtype mmc; setenv devnum 0; setenv distro_bootpart 1; fi
setenv phslot a
mmc dev ${devnum}
mmc read 0x7F000000 0x1800 1
# slot B's length NOW: the boot-config read below reuses this buffer (2026-10-15: read after it, the length was the
# config sector's word, the slot B read failed and the copy booted the stale golden image left at 0x80000000)
setexpr.l phsize *0x7F000008
mw.l 0x7F100000 0x4C534850
# INSTALLED (GOOD, 3): a release card (make_pinephone_sd.sh RELEASE=1) keeps its image in slot B and boots it every
# time; nothing is written back. A development upload (PENDING) replaces the state, as below.
mw.l 0x7F100004 3
if cmp.l 0x7F000000 0x7F100000 2; then
	echo "Phorepoc: booting the installed image (slot B)"
	setenv phslot b
fi
mw.l 0x7F100004 1
if test "${phslot}" = b; then
	setenv phslot b
elif cmp.l 0x7F000000 0x7F100000 2; then
	# ONE-SHOT: an armed slot B (phlink upload or 'B' re-arm) boots exactly once; marked TRYING now, so the next
	# reset -- whatever caused it -- is slot A again. No health timing (2026-10-06: S60 trials looped through it).
	echo "Phorepoc: slot B armed -- one-shot boot"
	mw.l 0x7F000004 2
	mmc write 0x7F000000 0x1800 1
	setenv phslot b
else
	# back from slot B's one shot: TRYING -> DONE (4), so this slot A kernel knows it is A and replays B's SD log
	mw.l 0x7F100004 2
	if cmp.l 0x7F000000 0x7F100000 2; then
		mw.l 0x7F000004 4
		mmc write 0x7F000000 0x1800 1
	fi
fi
# BOOT CONFIG (2026-10-12): SD sector 0x1801 = {'PHCF', RAM MB, ...}, the user's settings (PhoneLab / a Symbian
# Settings page write it). RAM MB -> {'PHRM', MB} at 0x402FFFF8 just before `go`; the bootstrap (a64.s GetRamMb)
# accepts multiples of 256 MB from 512 to 2816 and falls back to 1024. No config sector = the image's baked value
# (make_pinephone_image.sh RAM_MB, written into the same word).
setenv phram
mmc read 0x7F000000 0x1801 1
mw.l 0x7F100000 0x46434850
if cmp.l 0x7F000000 0x7F100000 1; then
	setexpr.l phram *0x7F000004
fi
if test -n "${phram}"; then echo "Phorepoc: boot config: RAM for Symbian 0x${phram} MB"; fi
# LOAD HIGH, COPY DOWN (2026-10-15): the image is read to 0x80000000 (free while U-Boot runs; a 1 GiB image ends at
# 0xC0000000, below U-Boot's relocated copy), then copied to its run address 0x40200000 and compared in ONE if-block --
# hush parses the whole block before running it, so the copy may overwrite this script (scriptaddr 0x4FC00000) safely.
# The only size bound is slot B's 1 GiB partition (was 112 MiB: two loads 126 MiB apart). Slot B's upload is already
# read back and checked by the kernel; the copy compare catches a bad copy/cache shutdown.
if test "${phslot}" = b; then
	echo "Phorepoc: loading slot B (P2, raw sectors 0x220000..)"
	# the image length from the control sector (+8) -> whole sectors (U-Boot setexpr works in hex)
	setexpr phsect ${phsize} + 1ff
	setexpr phsect ${phsect} / 200
	mw.l 0x80000000 0 0x400
	if mmc read 0x80000000 0x220000 ${phsect}; then echo "Phorepoc: slot B read 0x${phsize} bytes"; else echo "Phorepoc: slot B read FAILED -> slot A"; reset; fi
	gpio set PD18
	gpio clear PD19
	gpio clear PD20
	sleep 2
else
	echo "Phorepoc: loading slot A (symbian.img from ${devtype} ${devnum}:${distro_bootpart})"
	load ${devtype} ${devnum}:${distro_bootpart} 0x80000000 symbian.img
	setenv phsize ${filesize}
	gpio set PD18
	gpio set PD19
	gpio clear PD20
	sleep 2
fi
gpio clear PD18
gpio clear PD19
gpio set PD20
sleep 1
dcache flush
dcache off
icache off
if true; then
	cp.b 0x80000000 0x40200000 ${phsize}
	if cmp.b 0x40200000 0x80000000 ${phsize}; then
		echo "Phorepoc: starting Symbian (the LED shows the boot stage from here)"
		if test "${phslot}" = b; then
			# slot B: arm the A64 watchdog -- whole-system reset (CFG = 1), 16 s (MODE = 0xB << 4 | 1); the kernel feeds it
			mw.l 0x01C20CB4 1
			mw.l 0x01C20CB8 0xB1
		fi
		# this word is inside the shim's pad (caches are off, a plain store reaches DRAM)
		if test -n "${phram}"; then
			mw.l 0x402FFFF8 0x4D524850
			mw.l 0x402FFFFC ${phram}
		fi
		go 0x40200000
	fi
	# cyan = the copy differs: slot B (already marked TRYING) -> reset = slot A, never on to U-Boot's next boot
	# target (2026-10-14: a failed slot B boot fell through to Mobian on the eMMC)
	gpio set PD18
	gpio clear PD19
	gpio set PD20
	sleep 10
	if test "${phslot}" = b; then reset; fi
	sleep 600
fi
