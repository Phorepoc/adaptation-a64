# CHARGE-ONLY boot script (OURS, 2026-10-06): a flat battery boot-looped the phone even under Mobian (the boot load
# browns it out). This sits in U-Boot instead -- one core, no display -- and makes sure the AXP803 charges:
# RSB bring-up (as a64_display.cpp RsbInit), REG 33H bit 7 = charge enable, REG 30H bits 1:0 = VBUS input limit 2 A.
# Every ~10 s it snapshots the PMIC to pmic.bin on this FAT partition (read it on the laptop):
#   byte 0 REG00 power status (bit5 VBUS present, bit2 battery current direction 1 = charging)
#   byte 1 REG01 (bit6 charging, bit5 battery present, bit4 battery-present valid)
#   byte 2 REG30, 3 REG33, 4 REG78 + 5 REG79 = battery voltage (12 bit, 1.1 mV/LSB), 6 REGB9 fuel gauge % (bit7 valid)
#   pmic0.bin = the same, before we wrote anything.
# LEDs: green blink = charging; red blink = not charging. Restore: copy boot-symbian.scr back to boot.scr.
gpio clear PD18
gpio clear PD19
gpio set PD20
# --- RSB bring-up: R_PRCM APB0 gate + reset (bit 3), PL0/PL1 = function 2, soft reset, 3 MHz, I2C -> RSB, SRTA 0x2d
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
sleep 1
mw.l 0x01F03404 0x203
mw.l 0x01F03428 0x807c3e00
sleep 1
setexpr.l t *0x01F0340C
mw.l 0x01F0340C ${t}
mw.l 0x01F0342C 0xe8
mw.l 0x01F03430 0x2d03a3
mw.l 0x01F03400 0x80
sleep 1
setexpr.l t *0x01F0340C
mw.l 0x01F0340C ${t}
# rsbrd: reg -> val (byte);  rsbwr: reg, val
setenv rsbrd 'setexpr.l s *0x01F0340C; mw.l 0x01F0340C ${s}; mw.l 0x01F0342C 0x8b; mw.l 0x01F03430 0x2d0000; mw.l 0x01F03410 ${reg}; mw.l 0x01F03400 0x80; sleep 0.1; setexpr.b val *0x01F0341C'
setenv rsbwr 'setexpr.l s *0x01F0340C; mw.l 0x01F0340C ${s}; mw.l 0x01F0342C 0x4e; mw.l 0x01F03430 0x2d0000; mw.l 0x01F03410 ${reg}; mw.l 0x01F0341C ${val}; mw.l 0x01F03400 0x80; sleep 0.1'
setenv snap 'setenv reg 0; run rsbrd; mw.b 0x4F200000 ${val}; setenv reg 1; run rsbrd; mw.b 0x4F200001 ${val}; setenv reg 30; run rsbrd; mw.b 0x4F200002 ${val}; setenv reg 33; run rsbrd; mw.b 0x4F200003 ${val}; setenv reg 78; run rsbrd; mw.b 0x4F200004 ${val}; setenv reg 79; run rsbrd; mw.b 0x4F200005 ${val}; setenv reg b9; run rsbrd; mw.b 0x4F200006 ${val}; mw.b 0x4F200007 ${cnt}'
setenv cnt 0
mw.l 0x4F200000 0 2
run snap
fatwrite mmc 0:1 0x4F200000 pmic0.bin 8
# --- charge enable + 2 A input limit
setenv reg 33; run rsbrd
setexpr val ${val} "|" 80
run rsbwr
setenv reg 30; run rsbrd
setexpr val ${val} "&" fc
setexpr val ${val} "|" 2
run rsbwr
# --- idle loop: snapshot + LED every ~10 s
while true; do
	setexpr cnt ${cnt} + 1
	setexpr cnt ${cnt} "&" ff
	run snap
	fatwrite mmc 0:1 0x4F200000 pmic.bin 8
	setexpr.b st *0x4F200001
	setexpr st ${st} "&" 40
	gpio clear PD20
	if test "${st}" = 40; then gpio set PD18; else gpio set PD19; fi
	sleep 1
	gpio clear PD18
	gpio clear PD19
	sleep 9
done
