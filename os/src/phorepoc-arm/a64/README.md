# a64 — the A64 board layer (Allwinner A64, 4 × Cortex-A53 in AArch32)

OURS.
**Unverified on the real phone** — written from the A64 manual's memory map, the Linux DTS (as documentation) and
NuttX's A64 port; tested in QEMU `orangepi-pc` (Allwinner H3), which shares the A64's UART0, GIC-400, CCU/PIO and
DRAM addresses (`tools/phorepoc-arm/run_h3.sh`).

| File | What |
|---|---|
| `bootstrap/a64.inc`, `a64.s` | the bootstrap PSL (from `../virt/bootstrap`): memory map, 16550 debug UART, LED stage codes, PSCI over SMC, run-from-RAM |
| `config.inc` | bootstrap config (A8N CPU config + `CFG_A53` from the build) |
| `specific/a64_glue.cpp` | the seam's driver hooks (RAM drive only) + LED/vibrator boot stages |
| `estart.txt` | drive map: C: = the internal RAM drive (no storage driver yet) |
| `boot.cmd` | the U-Boot script (→ `boot.scr`, `tools/phorepoc-arm/mkbootscr.py`) |

The kernel variant itself is `../virt/specific` compiled with `-D__A64__` (the GIC/generic-timer code is shared).

Build: `BOARD=a64 SMP=1 MMODEL=flexible bash tools/kernel/build_ekern_rom.sh` → the board build dir's `symbian.img` +
`boot.scr` (copy both to the boot partition U-Boot reads). H3 test build: add `BOOT_DEFS="-D CFG_H3TEST"`, then
`bash tools/phorepoc-arm/run_h3.sh`.

LED stages (no serial console): red = bootstrap running · yellow = bootstrap done, kernel next · green + short buzz =
kernel up · blue = first user-mode thread · magenta = fault. (Pins from the DTS: PD18 green, PD19 red, PD20 blue,
PD2 vibrator — if two colours look swapped on the phone, the DTS reading is wrong, not the stage.)
