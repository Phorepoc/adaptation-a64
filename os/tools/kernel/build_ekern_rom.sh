#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0

# build_ekern_rom.sh -- wrap the from-source ekern.elf as an XIP image and boot it.
#
# The kernel is NOT position-independent, so it is linked at its exact ROM address:
# the TRomImageHeader sits at PLACE_AT and the code at PLACE_AT+120 (= code base). The
# linker script splits .data/.bss to run at the kernel data RAM base (KERN_DATA_BASE);
# the bootstrap copies .data (ROM LMA) -> that RAM and zeroes .bss using the header fields.
set -eu
cd "${KVIEW:-$(dirname "$0")/../..}"   # KVIEW: the tree root (the pdk assembles it for the public repos)

BIN_DEFAULT="/c/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/14.2 rel1/bin"
BIN="${ARM_BIN:-$BIN_DEFAULT}"
NM="$BIN/arm-none-eabi-nm"; OC="$BIN/arm-none-eabi-objcopy"
[ -x "$NM" ] || NM="$(command -v arm-none-eabi-nm)"
[ -x "$OC" ] || OC="$(command -v arm-none-eabi-objcopy)"

# The kernel .text is 256-byte aligned, so the code base must be 256-aligned or the linker
# inserts a gap that breaks the ROM offset math. Place the 120-byte TRomImageHeader exactly
# before it (PLACE_AT = CODE_BASE - 120).
# CODE_BASE MUST ALSO BE 4KB(page)-ALIGNED: the exception vector TABLE is the first thing in
# .text (nk_entry.cia), so it starts at iCodeAddress = CODE_BASE. The bootstrap ("Map vectors",
# bootmain.s) maps the 4KB PAGE containing iCodeAddress to the HIVECS base 0xFFFF0000; if CODE_BASE
# is not page-aligned the vectors land at 0xFFFF0000+(CODE_BASE&0xFFF) and 0xFFFF0000 reads the zero
# padding before them -- so an swi/IRQ/abort jumps to a zero vector, runs off, and FAULTs. With a
# 256-but-not-4K base (0x80020100) the swi vector was at 0xFFFF0108 and user-mode swi crashed.
# (2026-09-18: the vector-alignment fix)
CODE_BASE="${CODE_BASE:-0x80021000}"               # 4KB-aligned (also 256-aligned) kernel code base
PLACE_AT=$(printf '0x%x' $(( CODE_BASE - 120 )))   # header sits 120 bytes before the code
KERN_DATA_BASE="${KERN_DATA_BASE:-0xC8000000}"     # multiple model KKernelDataBase
O=.ekern-build; [ "${MMODEL:-multiple}" = flexible ] && O=.ekern-flex-build   # MMODEL=flexible: see link_ekern.sh
[ "${SMP:-0}" = 1 ] && O=.ekernsmp-flex-build   # SMP=1: the SMP kernel (link_ekern.sh SMP=1; needs MMODEL=flexible)
MMODEL_XINFO=; [ "${MMODEL:-multiple}" = flexible ] && MMODEL_XINFO=,flexible=1   # rombuild: ROM DLL data under KUserLocalDataEnd
# BOARD=a64: the user content at a FIXED ROM address (rombuild user_at; ROM_USER_AT=0 = right after the kernel, as before)
# -- the PinePhone delta upload then re-sends only what changed instead of everything behind a kernel that changed size
if [ "${BOARD:-syborg}" = a64 ] && [ "${ROM_USER_AT:-0x80400000}" != 0 ]; then
  MMODEL_XINFO="$MMODEL_XINFO,user_at=${ROM_USER_AT:-0x80400000}"
fi

T0=$SECONDS; step_time() { echo "      ($1: $((SECONDS-T0)) s)"; T0=$SECONDS; }
echo "[1/5] link ekern @ code_base=$CODE_BASE (data/bss @ $KERN_DATA_BASE)"
if [ "${SMP:-0}" = 1 ] && [ "${MMODEL:-multiple}" = flexible ]; then
  # a clean tree has no kernel objects yet: the SMP nanokernel + the flexible-model kernel compile checks make them
  # (unchanged objects are cached)
  bash tools/kernel/nkernsmp_compile_check.sh >"$O.nk.log" 2>&1 || { tail -20 "$O.nk.log"; echo "nkernsmp compile failed" >&2; exit 1; }
  SMP=1 MMODEL=flexible bash tools/kernel/ekern_compile_check.sh >"$O.ek.log" 2>&1 || { tail -20 "$O.ek.log"; echo "ekern compile failed" >&2; exit 1; }
fi
CODE_BASE="$CODE_BASE" KERN_DATA_BASE="$KERN_DATA_BASE" bash tools/kernel/link_ekern.sh >/dev/null
sym() { "$NM" "$O/ekern.elf" | awk -v s="$1" '$3==s{print "0x"$1}'; }
ENTRY=$(sym "${ENTRY_SYM:-_ekern_entry}"); CODE_SIZE=$(sym __code_size__); DATA_LMA=$(sym __data_lma__)
DATA_SIZE=$(sym __data_size__); BSS_SIZE=$(sym __bss_size__)
echo "      entry=$ENTRY code_size=$CODE_SIZE data_lma=$DATA_LMA data_size=$DATA_SIZE bss_size=$BSS_SIZE"

step_time "link ekern"
echo "[2/5] objcopy ekern.elf -> raw (text+rodata+ctor+data, .bss excluded)"
"$OC" -O binary "$O/ekern.elf" "$O/ekern.bin"
echo "      ekern.bin = $(wc -c < "$O/ekern.bin") bytes"

# CFG_DebugBootRom makes the bootstrap dump ~2000+ verbose lines (SPageInfo/GetPDE/GetPTE/
# MapSglE) before the kernel starts, which buries the [EKERN] serial log past any sane cap.
# Default to a QUIET bootstrap so kernel output appears promptly; set BOOT_DEFS='-D CFG_DebugBootRom'
# to restore the verbose page-table trace when debugging the bootstrap itself.
echo "[3/5] build the ${MMODEL:-multiple}-model bootstrap (BOOT_DEFS='${BOOT_DEFS:-}')"
MM_MODEL="${MMODEL:-multiple}" EXTRA_DEFS="${BOOT_DEFS:-}$([ "${SMP:-0}" = 1 ] && echo " -D SMP")" bash tools/build_minimal_rom.sh >/dev/null

echo "[3b/5] assemble the minimal user 'secondary' EXE (minisec, fully PIC)"
GCC="$BIN/arm-none-eabi-gcc"; LD="$BIN/arm-none-eabi-ld"
[ -x "$GCC" ] || GCC="$(command -v arm-none-eabi-gcc)"
[ -x "$LD" ] || LD="$(command -v arm-none-eabi-ld)"
"$GCC" -c -marm -march=armv7-a tools/kernel/minisec.s -o "$O/minisec.o"
"$LD" -Ttext=0 -e _start "$O/minisec.o" -o "$O/minisec.elf"
"$OC" -O binary "$O/minisec.elf" "$O/minisec.bin"
echo "      minisec.bin = $(wc -c < "$O/minisec.bin") bytes (entry @ code_base+0)"

step_time "objcopy + bootstrap + minisec"
echo "[4/5] rombuild: place ekern @ $PLACE_AT + variant extension DLL + secondary user EXE"
# The variant's code is statically in ekern; the variant "file" is a TRomImageHeader whose
# export table (ordinals 1..9, kasyborg.def) points at these ekern addresses.
V_ENTRY=$(sym _variant_e32dll)
EXPORTS=""
VINIT=_Z17VariantInitialisev; [ "${SMP:-0}" = 1 ] && VINIT=_Z17VariantInitialisei   # SMP: VariantInitialise(TInt)
for s in $VINIT _Z14AsicInitialisev _ZN6Syborg11DebugOutputEj \
         _ZN6Syborg12MsTickPeriodEv _ZN6Syborg13StartupReasonEv \
         _ZN6Syborg27SystemTimeInSecondsFrom2000ERi _ZN6Syborg30SetSystemTimeInSecondsFrom2000Ei \
         _ZN6Syborg5Init1Ev _ZN6Syborg5Init3Ev; do
  EXPORTS="$EXPORTS:$(sym $s)"
done
EXPORTS="${EXPORTS#:}"
# SECONDARY=minisec (default) keeps the proven asm hello-loop secondary.
# SECONDARY=efile places the from-source EFILE.EXE + euser/efsrv/hal as XIP-prelinked
# E32Images (tools/rombuild.py --xip: relocs applied + ordinal imports resolved at
# ROM build; EFILE becomes iSecondaryFile). Artifacts must exist in emitter/
# (py emitter/build_euser_device.py / build_efsrv_device.py / build_hal_device.py /
#  build_efile_exe.py). All-caps for both: trusted ROM components (see M6b note).
BS='\'
# ESTART's drive map: syborg's (D: = IRAM, C: = NVMemory), or on BOARD=virt C: = the internal RAM drive
ESTART_TXT=baseline/symbian-2010/oss.FCL.sf.adapt.Qemu/baseport/syborg/estart.txt
[ "${BOARD:-syborg}" = virt ] && ESTART_TXT=src/phorepoc-arm/virt/estart.txt
[ "${BOARD:-syborg}" = a64 ] && ESTART_TXT=src/phorepoc-arm/a64/estart.txt   # C: = the RAM drive (no storage driver yet)
if [ "${SECONDARY:-minisec}" = "efile" ]; then
  SEC_ARGS=(--xip 'Sys\Bin\euser.dll=emitter/euser_dev.dll'
            --xip 'Sys\Bin\efsrv.dll=emitter/efsrv_dev.dll'
            --xip 'Sys\Bin\hal.dll=emitter/hal_dev.dll'
            --xip 'Sys\Bin\efile.exe=emitter/efile.exe'
            # The REAL estart (py emitter/build_estart_exe.py): unmodified
            # TFSStartup + syborg TSyborgFSStartup; reads Z:\SYS\DATA\ESTART.TXT
            # (the real syborg mapping file, added below as a plain data file).
            --xip 'Sys\Bin\ESTART.EXE=emitter/estart.exe'
            # ELOCAL.FSY (py emitter/build_elocal_fsy.py): from-source FAT16
            # file system plugin -- estart.txt maps C:/D: as `ELOCAL FAT`.
            # Imports resolve against efile.exe's export table at prelink.
            --xip 'Sys\Bin\ELOCAL.FSY=emitter/elocal.fsy'
            # The REAL EWSRV.EXE (py emitter/build_ewsrv_exe.py): text window
            # server + econs/ektran/domaincli fillin TUs. estart launches it
            # after StartupInitComplete (StartSystem).
            --xip 'Sys\Bin\EWSRV.EXE=emitter/ewsrv.exe'
            # EDISP.DLL (py emitter/build_edisp_dll.py): text screen driver,
            # RLibrary-loaded by EWSRV (Lookup(1) -> CScreenDriver::New).
            --xip 'Sys\Bin\EDISP.DLL=emitter/edisp.dll'
            # EKDATA.DLL (py emitter/build_ekdata_dll.py): syborg keymap data,
            # RLibrary-loaded by EWSRV's CKeyTranslator (3 data exports).
            --xip 'Sys\Bin\EKDATA.DLL=emitter/ekdata.dll'
            # ECONS.DLL (py emitter/build_econs_dll.py): console client;
            # ESHELL's Console::NewL loads it (NewConsole @1 -> RConsole).
            --xip 'Sys\Bin\ECONS.DLL=emitter/econs.dll'
            # ESHELL.EXE (py emitter/build_eshell_exe.py): the text shell
            # EWSRV spawns (Fault ECreateShell if absent).
            --xip 'Sys\Bin\ESHELL.EXE=emitter/eshell.exe'
            # The 3-DLL locale (2026-10-07): euser + EFILE are built with SYMBIAN_DISTINCT_LOCALE_MODEL
            # since the SSM ROM (dba9932f); without these the old single-DLL path jumps to 0 (EWSRV
            # died at start, blue screen with no ESHELL).
            --xip 'Sys\Bin\elocl_lan.001=emitter/elocl_lan.001'
            --xip 'Sys\Bin\elocl_reg.826=emitter/elocl_reg.826'
            --xip 'Sys\Bin\elocl_col.001=emitter/elocl_col.001'
            --data "Sys${BS}Data${BS}estart.txt=$ESTART_TXT"
            --xinfo 'databss=0x00400000,cap1=0xFFFFFFFF,cap2=0xFFFFFFFF'"${MMODEL_XINFO:-}")
  # The PinePhone GOLDEN image (BOARD=a64, no PHOREPOC=1) at par with slot B's PhoneLab file service: phfs.exe (the user half of
  # phlink's 'F' command, src/phfs) started by Z:\AUTOEXEC.BAT -- ESHELL runs the first AUTOEXEC.BAT it finds (Y:..A:, then Z:,
  # ts_edshl.cpp), 8-bit text; `start` = run without waiting. Slot B starts it from the SSM list instead (r_cmd_phfs).
  if [ "${BOARD:-syborg}" = a64 ] && [ "${PHOREPOC:-0}" != 1 ] && [ -f emitter/phfs.exe ]; then
    mkdir -p .fb-out; printf 'start phfs\r\n' > .fb-out/autoexec.bat
    SEC_ARGS+=(--xip 'Sys\Bin\phfs.exe=emitter/phfs.exe' --data "AUTOEXEC.BAT=.fb-out/autoexec.bat")
  fi
  # GUI=1 -- the Symbian "minigui" configuration (windowserver/minigui/*.oby), from
  # source: the GRAPHICAL window server takes the EWSRV.EXE slot (wserv.iby:
  # wserv_nonnga.exe -> EwSrv.exe), econswserv.dll takes the ECONS.DLL slot
  # (econs_wserv.iby) so ESHELL's console is a real WSERV window, wsini.ini says
  # `startup eshell`, fbserv + the gdi/bitgdi/fntstore stack ride along, and the
  # bitmap font is provisioned on C: (tools/mkfat.py; fntstore's
  # XIP-ROM path has an alignment bug real NAND phones never hit -- they use the RAM path). All built by `py emitter/build_mmp.py --all`.
  # PHOREPOC=1 BOOT=ssm|@... (2026-10-06): the PHOREPOC ROM on ARM -- the same contents as the amd64 ROM, from the
  # same recipe (tools/make_phorepoc_rom.sh ARGS_ONLY=1 with ROM_ARCH=arm: x32_rom_args.py over the ARM images in
  # emitter/ + the shared data: fonts, cenrep, CommsDat, wsini, features...). Only the core four stay ours here;
  # BOOT=ssm = the real ESTART (e32strt, its own mmp) -> SSM, as on amd64. Build: ARM_VFP=1 py emitter/build_mmp.py --all
  if [ "${PHOREPOC:-0}" = 1 ]; then
    [ -n "${BOOT:-}" ] || { echo "PHOREPOC=1 needs BOOT=ssm|@..."; exit 1; }
    SEC_ARGS=(--xip 'Sys\Bin\euser.dll=emitter/euser_dev.dll' --xip 'Sys\Bin\efsrv.dll=emitter/efsrv_dev.dll'
              --xip 'Sys\Bin\hal.dll=emitter/hal_dev.dll' --xip 'Sys\Bin\efile.exe=emitter/efile.exe')
    if [ "$BOOT" = ssm ]; then SEC_ARGS+=(--xip 'Sys\Bin\estart.exe=emitter/e32strt.exe')
    else SEC_ARGS+=(--xip 'Sys\Bin\ESTART.EXE=emitter/estart.exe'); fi
    SEC_ARGS+=(--data "Sys${BS}Data${BS}estart.txt=$ESTART_TXT"
               --xinfo 'databss=0x00400000,cap1=0xFFFFFFFF,cap2=0xFFFFFFFF'"${MMODEL_XINFO:-}")
    PARGS=$(ARGS_ONLY=1 ROM_ARCH=arm PHOREPOC_B=.phorepoc-arm-build BOOT="$BOOT" bash tools/make_phorepoc_rom.sh | tail -n 1)
    [ -f "$PARGS" ] || { echo "Error: the Phorepoc ROM recipe gave no args file ($PARGS)"; exit 1; }
    while IFS= read -r a; do SEC_ARGS+=("$a"); done < <(tr -d '\r' < "$PARGS")
    echo "PHOREPOC ROM (ARM): $(grep -c '^--xip' "$PARGS") images + $(grep -c '^--data' "$PARGS") data files from the shared recipe"
  elif [ "${GUI:-0}" = "1" ]; then
    G=baseline/symbian-2010/oss.FCL.sf.os.graphics
    FILTERED=()
    for a in "${SEC_ARGS[@]}"; do
      case "$a" in *EWSRV.EXE=*|*ECONS.DLL=*|*EDISP.DLL=*) ;; *) FILTERED+=("$a") ;; esac
    done
    # drop the now-dangling --xip flags that preceded the removed specs
    SEC_ARGS=()
    i=0
    while [ $i -lt ${#FILTERED[@]} ]; do
      if [ "${FILTERED[$i]}" = "--xip" ] && { [ $((i+1)) -ge ${#FILTERED[@]} ] || [ "${FILTERED[$((i+1))]:0:2}" = "--" ]; }; then
        i=$((i+1)); continue
      fi
      SEC_ARGS+=("${FILTERED[$i]}"); i=$((i+1))
    done
    # Every built registry component (emitter/components.py) + the files its build
    # staged for the ROM (compiled resources, ECOM registrations) -- one source of
    # truth instead of a hand-kept list (tools/kernel/gui_rom_args.py).
    while IFS= read -r a; do SEC_ARGS+=("$a"); done < <(py tools/kernel/gui_rom_args.py | tr -d '\r')
    # central repository initialisation files (Z:\private\10202be9\<uid>.txt)
    # = everything tools/crml2cenrep.py derives from the tree's crml/confml (+ shipped
    # cenrep/*.txt), overridden by the explicit list in tools/kernel/cenrep_files.txt
    [ -d .cenrep-gen ] || py tools/crml2cenrep.py .cenrep-gen >/dev/null
    declare -A CENREP=()
    for f in .cenrep-gen/*.txt; do
      u=$(basename "$f" .txt); CENREP[${u,,}]="$f"
    done
    while IFS= read -r f; do
      case "$f" in ''|'#'*) continue ;; esac
      u=$(basename "$f" .txt); CENREP[${u,,}]="baseline/symbian-2010/$f"
    done < <(tr -d '\r' < tools/kernel/cenrep_files.txt)
    for u in "${!CENREP[@]}"; do
      SEC_ARGS+=(--data "private${BS}10202be9${BS}$u.txt=${CENREP[$u]}")
    done
    # BOARD=a64: the phone has no C: disk with the bitmap font (QEMU's run_virt.sh attaches one; the phone's C: is
    # an EMPTY RAM drive) -- without a font, ESHELL's console dies in a loop and Avkon panics WSERV 13 (invalid font
    # handle), so nothing ever draws (2026-10-06). The font goes into the ROM instead: the A53 does unaligned 16-bit
    # loads in hardware, so fntstore's XIP-ROM path (the ARM9 alignment bug above) should hold.
    if [ "${BOARD:-}" = a64 ]; then
      SEC_ARGS+=(--data "resource${BS}fonts${BS}ceurope.gdr=baseline/symbian-2010/oss.FCL.sf.os.graphics/graphicstest/uibench/data/CEUROPE.GDR")
    fi
    # Feature Manager ROM database (tools/features_dat.py: derived from the S60 variant)
    py tools/features_dat.py .fb-out/features.dat >/dev/null
    SEC_ARGS+=(--data "private${BS}10205054${BS}features.dat=.fb-out/features.dat")
    # Installed-languages list (syslangutil; akncapserver publishes it and dies without it).
    # A real image creation writes it from the language-pack config: TFileText (UTF-16LE+BOM),
    # one TLanguage code per line, ",d" = default. Ours: English only.
    py -c "open('.fb-out/languages.txt','wb').write('﻿01,d\r\n'.encode('utf-16-le'))"
    SEC_ARGS+=(--data "resource${BS}bootdata${BS}languages.txt=.fb-out/languages.txt")
    # BOOT="srv1 srv2 !app": OUR minimal starter (src/bootui) runs the list in order
    # (waits for each server's rendezvous; `!` = don't wait). Implies APP=bootui.
    # BOOT=@ = the standard S60 boot list ending in the default shell, the real S^3 launcher
    # (matrixmenu); BOOT=@<app> = the same list with another foreground app (@aknmenu, @aknapp)
    case "${BOOT:-}" in @*)
      BOOT_APP="${BOOT#@}"
      BOOT="=101f75b6:102047b7:0 !domainsrv ~2:10000 baksrvs featmgrserver cdlserver ssmutilsrv +capserver !${BOOT_APP:-matrixmenu}";;
    esac
    if [ -n "${BOOT:-}" ]; then
      printf '%s\n' $BOOT > .fb-out/bootui.txt
      SEC_ARGS+=(--data "Sys${BS}Data${BS}bootui.txt=.fb-out/bootui.txt")
      APP=bootui
    fi
    # APP=<exe stem>: wsini `startup <app>` instead of the minigui default (eshell)
    if [ "${NGA:-0}" = 1 ]; then
      # GUI=1 NGA=1 (GPU plan G1b, the minimal NGA image): the NGA render stages (flickerbuffer + display = libWFC)
      py tools/mkwsini.py .fb-out/wsini_app.ini "${APP:-eshell}" "PLUGINS FLICKERBUFFER DISPLAY" "RENDERSTAGES flickerbuffer display"
      SEC_ARGS+=(--data "System${BS}Data${BS}wsini.ini=.fb-out/wsini_app.ini")
    elif [ "${APP:-eshell}" != "eshell" ]; then
      py tools/mkwsini.py .fb-out/wsini_app.ini "$APP"
      SEC_ARGS+=(--data "System${BS}Data${BS}wsini.ini=.fb-out/wsini_app.ini")
    else
      SEC_ARGS+=(--data "System${BS}Data${BS}wsini.ini=$G/windowing/windowserver/minigui/group/wsini_minigui-stripped.ini")
    fi
  fi
else
  SEC_ARGS=(--secondary "$O/minisec.bin" --sinfo "entry_off=0,cap1=0xFFFFFFFF,cap2=0xFFFFFFFF")
fi
# the file list goes through an @response file (one argument per line): the GUI bundle's
# ~200 components overflow the Windows command line ("Argument list too long")
printf '%s\n' "${SEC_ARGS[@]}" > "$O/rombuild_sec.args"
# STABLE FILE PLACEMENT (2026-10-23; BOARD=a64): each ROM file keeps its address while it still fits its slot (the map is
# $O/rom.placement.json), so two builds differ only where a file changed and a PhoneLab delta push sends just that.
# FULL=1 repacks compactly (a new map); ROM_STABLE=0 = the old sequential layout.
PLACE_ARGS=()
[ "${BOARD:-syborg}" = a64 ] && [ "${ROM_STABLE:-1}" != 0 ] && PLACE_ARGS=(--placement "$O/rom.placement.json")
py tools/rombuild.py .bootstrap-build/boot.bin .bootstrap-build/ekern.rom --primary "$O/ekern.bin" \
  --kinfo "place_at=$PLACE_AT,entry=$ENTRY,code_size=$CODE_SIZE,data_lma=$DATA_LMA,data_size=$DATA_SIZE,bss_size=$BSS_SIZE,kern_data_base=$KERN_DATA_BASE,trace_mask=${TRACE_MASK:-0},uid1=0x1000007A" \
  --variant "entry=$V_ENTRY,exports=$EXPORTS" \
  "@$O/rombuild_sec.args" "${PLACE_ARGS[@]}" || { echo "Error: rombuild failed"; exit 1; }

step_time "rombuild"
echo "[5/5] boot"
if [ "${BOARD:-syborg}" = a64 ]; then
  bash tools/phorepoc-arm/make_pinephone_image.sh .bootstrap-build/ekern.rom   # no emulator: package for U-Boot
elif [ "${BOARD:-syborg}" = virt ]; then
  bash tools/phorepoc-arm/run_virt.sh .bootstrap-build/ekern.rom   # QEMU virt (PinePhone track)
else
  QEMU_RUN_SECS="${QEMU_RUN_SECS:-15}" bash tools/qemu-syborg/run_syborg.sh .bootstrap-build/ekern.rom
fi
