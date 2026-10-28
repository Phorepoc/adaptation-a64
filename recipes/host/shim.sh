#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0

# recipes/host/shim.sh <build dir or docker volume> -- the AArch64 entry shim (os/src/phorepoc-arm/a53shim/shim.S): the
# A53 cores start in AArch64, the shim drops them to AArch32 at the kernel. Runs on the host, for a board's release
# recipe: with aarch64-linux-gnu-gcc on the PATH (CI: apt) it builds natively, otherwise in a container: ARM_IMAGE if
# set (it must have the cross tools), else debian:trixie, which installs them first. Writes <build>/arm/a53/shim-ram-noprobe.bin.
set -eu
BUILD="${1:?usage: shim.sh <build dir or docker volume>}"
OS="$(cd "$(dirname "$0")/../../os" && pwd)"
build() {   # $1 = the os/ tree, $2 = the build dir
	mkdir -p "$2/arm/a53"; t="$(mktemp -d)"
	aarch64-linux-gnu-gcc -c -DTARGET=0x40300000 "$1/src/phorepoc-arm/a53shim/shim.S" -o "$t/shim.o"
	aarch64-linux-gnu-ld -Ttext=0 "$t/shim.o" -o "$t/shim.elf"
	aarch64-linux-gnu-objcopy -O binary "$t/shim.elf" "$2/arm/a53/shim-ram-noprobe.bin"
	rm -rf "$t"; ls -l "$2/arm/a53/shim-ram-noprobe.bin"
}
if command -v aarch64-linux-gnu-gcc > /dev/null && [ -d "$BUILD" ]; then
	build "$OS" "$BUILD"
else
	case "$BUILD" in /*|?:*|.*) BV="$(cd "$BUILD" && (pwd -W 2>/dev/null || pwd))" ;; *) BV="$BUILD" ;; esac
	MSYS_NO_PATHCONV=1 docker run --rm -v "$BV:/build" -v "$(cd "$OS" && (pwd -W 2>/dev/null || pwd)):/os:ro" \
		"${ARM_IMAGE:-debian:trixie}" \
		bash -c "command -v aarch64-linux-gnu-gcc >/dev/null || { apt-get update -qq >/dev/null && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu >/dev/null; }; $(declare -f build); build /os /build"
fi
