#!/bin/sh
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later
# Re-download every file at the pin and compare.
cd "$(dirname "$0")"; rc=0
while read f s; do t=$(curl -sSfL https://raw.githubusercontent.com/freebsd/freebsd-src/043d6753ca7c1c1e3ffa09a9f953f0df50d3a22e/$f | sha256sum | cut -c1-64); [ "$t" = "$s" ] || { echo "DIFF $f"; rc=1; }; done < SHA256SUMS; exit $rc
