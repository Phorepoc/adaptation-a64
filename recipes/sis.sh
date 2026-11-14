#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later
#
# sis.sh -- the pdk's package.yml runs it after the components (package.conf): the screen recorder package,
# $PHOREPOC_BUILD/sis/screenrec-a64-arm.sis = sys/bin/pherecenc.dll (the H.264 encoder plug-in) with the Apache-2.0 text
# and NOTICE. Without it the image's recorder refuses to record; nothing of it is in an image.
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
LIC=
for f in "$HERE/LICENSES/Apache-2.0.txt" "$HERE/NOTICE"; do
  b=$(basename "$f"); case $b in *.txt) ;; *) b=$b.txt;; esac
  LIC="$LIC --file $f=!:/resource/screenrec/$b"
done
python3 "$PHOREPOC_PDK/tools/make_sis.py" pherecenc --arm --unsigned --version "${PHOREPOC_PACKAGE_VERSION:-1.0.0}"   --name "Phorepoc screen recorder (A64)" --vendor Phorepoc $LIC   --out "$PHOREPOC_BUILD/sis/screenrec-a64-arm.sis"
