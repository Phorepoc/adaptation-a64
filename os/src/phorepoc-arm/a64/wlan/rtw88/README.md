# Vendored rtw88 (Linux) -- the RTL8703B subset, unmodified

These files are the Realtek rtw88 driver from the Linux kernel, `drivers/net/wireless/realtek/rtw88/`, at torvalds/linux
master commit `9f2948010764d708bda27369d09ce6f194abe8e3` (2026-07-30, "wifi: rtw88: Fix potential memory leak in
rtw_txq_push_skb()"), fetched 2026-10-10. Licence: **GPL-2.0 OR BSD-3-Clause** (every file's SPDX line) -- the BSD
alternative is what lets this sit next to the Symbian (EPL) code; the copyright is Realtek's and the named authors'.

**Nothing in this directory is edited.** The driver compiles as is against our freestanding "Linux"
(`../shim/include`: `shim_core.h` + thin `linux/*.h`, `net/{cfg80211,mac80211}.h`, `linux/ieee80211.h`), written from
the driver's usage of the Linux interface, not copied from Linux headers. The parts of rtw88 we do not vendor are
no-op stubs with the real signatures in `../shim/rtw88_stubs.c` (regd, bf, sar, led, debugfs; `coex.c` was stubbed
until W4 -- without it the shared-antenna arbiter stayed at its power-on defaults and nothing WiFi sent reached
the air, 2026-10-10); `rtw8723cs.c`
(the module/probe table) is replaced by `../a64wlan_glue.c`. `mac80211.c` (rtw88's `rtw_ops`, the driver's side of
mac80211) is vendored too since 2026-10-10, same commit, unmodified: the glue calls those ops in the order mac80211
does (`docs/phorepoc/wifi-station-join-transcript.md`).

Built into the kernel by `tools/kernel/link_ekern.sh` (the `wl_*.o` loop). Design + status:
`docs/phorepoc/pinephone-port.md` "WiFi: the chip core".

**2026-10-17: build-selectable again, with marked edits.** `WIFI_DRV=rtw88` (link_ekern.sh / `./ph pp-image`) builds this
driver instead of the default `rtl8703b/`; it joins WPA2, DHCP and NTP since boot 954. It compiles with
`-DPHE_RTW88_VENDOR_DIFF`, which turns on the only edits in this directory, each a recorded vendor difference
(docs/phorepoc/rtw88-vs-8723cs.md): (1) no external-XTAL/CLK_REQ bits in the pre-enable sequence (rtw8703b.c); (2)
management frames through the HIGH FIFO/queue, not EXTRA (rtw8703b.c rqpn, sdio.c); (3) HWSEQ_CTRL 0xFF (mac.c); (6)
the WiFi-only coex table 0xAAAAAAAA with BT disabled (coex.c). Without the define the code is upstream as fetched.
The bug that kept rtw88 deaf from 2026-10-10 to boot 952 was in OUR shim, not here: `__LITTLE_ENDIAN` was undefined, so
every little-endian bitfield in these files took its big-endian layout (the PHY table condition word skipped RF/MAC table
entries; efuse TX-power nibbles and the PHY status were swapped). `../shim/include/rtw88_config.h` defines it now.
