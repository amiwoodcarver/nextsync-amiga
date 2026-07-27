#!/usr/bin/env bash
#
# Install the AmiSSL runtime into an Amiga system drive (a directory that
# the emulator mounts, or a real volume over a network share).
#
#   tools/install-amissl.sh emu/hd0 [68020-40|68060]
#
# Afterwards the boot script needs:  Assign AmiSSL: SYS:AmiSSL
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HD="${1:?usage: install-amissl.sh <system drive dir> [cpu]}"
CPU="${2:-68020-40}"
SRC="$ROOT/vendor/os3/AmiSSL"

[ -d "$SRC" ] || { echo "run tools/fetch-deps.sh first" >&2; exit 1; }
[ -d "$HD" ]  || { echo "no such directory: $HD" >&2; exit 1; }

LIB=$(ls "$SRC/Libs/AmigaOS3/AmiSSL/$CPU/"amissl_v*.library 2>/dev/null | head -1)
[ -n "$LIB" ] || { echo "no library for CPU '$CPU'" >&2; exit 1; }

mkdir -p "$HD/Libs/AmiSSL" "$HD/AmiSSL"
cp "$SRC/Libs/AmigaOS3/amisslmaster.library" "$HD/Libs/"
rm -f "$HD"/Libs/AmiSSL/amissl_v*.library
cp "$LIB" "$HD/Libs/AmiSSL/"
rm -rf "$HD/AmiSSL/Certs"
cp -r "$SRC/Certs" "$HD/AmiSSL/"

echo "installed $(basename "$LIB") ($CPU) into $HD"
echo "  $(ls "$HD/AmiSSL/Certs" | wc -l | tr -d ' ') CA certificates"
echo
echo "Remember, the AmiSSL init needs these in LIBS: (they ship with"
echo "Workbench): mathieeedoubbas.library, mathieeedoubtrans.library"
