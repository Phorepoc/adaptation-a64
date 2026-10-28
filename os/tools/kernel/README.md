# The ARM kernel + ROM image recipe

How an ARM Phorepoc image is made, from the kernel link to the ROM the board's boot loader starts:

- `build_ekern_rom.sh` -- the whole recipe: links the kernel (`link_ekern.sh`: the unmodified 2010 EKA2 kernel, our
  board seam `ekern_seam.cpp` + glue, and the board's drivers, linked statically -- `BOARD=a64` takes them from
  `src/phorepoc-arm/a64/`), builds the bootstrap (`../build_minimal_rom.sh`), lays the ROM out (pdk's
  `rombuild.py` + the Phorepoc ROM recipe) and hands it to the board's packager (a board repository's
  image script).
- `ekern.lds` -- the kernel's link layout; `minisec.s` -- the minimal secondary process of the early boot rungs.
- `*_compile_check.sh` -- compile the unmodified nanokernel / kernel / WLAN LDD with our toolchain (called by the link).
- `cenrep_files.txt` + `cenrep/` -- the CenRep initialisation files the ROM ships.
- `../phorepoc-arm/build_shim.sh` + `src/phorepoc-arm/a53shim/shim.S` -- the AArch64 entry shim that starts the
  32-bit kernel on the A64's Cortex-A53 cores.

**Status:** these scripts still expect the single-tree layout they were written in (the OS sources, the pdk's
`emitter/` tools and the Symbian packages under one root, run from that root). The pdk will assemble that view
and run them (`ph image` for ARM boards); until then they document the recipe and are not run by CI.

Typical phone build (once wired): `BOARD=a64 SMP=1 MMODEL=flexible SECONDARY=efile PHOREPOC=1 BOOT=ssm
build_ekern_rom.sh`, then the board's image script (boot.scr + symbian.img) and its SD script (a whole
SD-card image to write with balenaEtcher).
