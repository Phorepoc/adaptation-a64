# symbian2010-camerasc -- the 2010 shared-chunk camera driver, restored from git history

The generic camera LDD of the Symbian 2010 open-source release (`DCameraScLdd`, device name `CameraSc`, user API `RDevCameraSc` in `d32camerasc.h`) and the template
PDD with its fake sensor (`bsptemplate/asspandvariant/template_variant/camerasc/`: a timer-driven sensor that alternates a YUV logo). Both were deleted from
`oss.FCL.sf.os.kernelhwsrv` master (commit 1d351557) but live on branch `origin/BRANCH_RCL_3`; the files here are byte-for-byte
`git show origin/BRANCH_RCL_3:<path>` (RCL_3 tip 75a20cd9, 2010-12-16). Licence: originally Eclipse Public License v1.0 (Nokia); the headers here now say EPL-2.0 with GPL-2.0-or-later as a Secondary License (relicensed under EPL-1.0 section 7), like the rest of the
Symbian Foundation release.

They are compiled **unmodified** into the x86 kernel (`tools/phorepoc-x86/ekern/make_ekern.sh`, entry points renamed on the command line only) and installed as built-in
devices `Sys\Bin\ecamerasc.ldd` / `camerasc.pdd` (`xk_camerasc.cpp`).

| path | what |
|---|---|
| `kernel/eka/drivers/camerasc/cameraldd.cpp`, `ecamerasc.mmp` | the LDD (2223 lines) |
| `kernel/eka/include/d32camerasc.h/.inl` | `RDevCameraSc` + the config/caps structs |
| `kernel/eka/include/drivers/camerasc.h/.inl` | the PDD contract (`DCameraScPdd`) |
| `bsptemplate/.../camerasc/camerasc.cpp`, `camerasc_sensor.cpp`, `camerasc_plat.h`, `logoyuv2.cpp`, `camerasctemplate.mmp` | the template PDD + fake sensor |
