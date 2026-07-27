#!/usr/bin/env bash
#
# Build the emulator's system drive from a Workbench 3.1 disk image.
#
#   tools/setup-sysdrive.sh WORKBENCH.ADF [target]     default target: emu/hd0
#
# AmiSSL's startup opens mathieeedoubbas.library and mathieeedoubtrans.library
# without checking the result, so it needs a real Workbench Libs: drawer --
# a bare boot directory is not enough. That is what this sets up.
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

ADF="${1:?usage: setup-sysdrive.sh WORKBENCH.ADF [target dir]}"
DEST="${2:-$ROOT/emu/hd0}"

command -v xdftool >/dev/null || {
    echo "xdftool not found: pip3 install amitools" >&2
    exit 1
}
[ -f "$ADF" ] || { echo "no such file: $ADF" >&2; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "==> unpacking $(basename "$ADF")"
xdftool "$ADF" unpack "$TMP" >/dev/null

SRC="$(find "$TMP" -maxdepth 2 -type d -name "Libs" | head -1 | xargs dirname)"
[ -n "$SRC" ] || { echo "no Libs drawer in that image -- is it the Workbench disk?" >&2; exit 1; }

echo "==> installing into $DEST"
mkdir -p "$DEST"
rsync -a "$SRC/" "$DEST/"
chmod +x "$DEST"/C/* "$DEST"/System/* 2>/dev/null || true
mkdir -p "$DEST/S" "$DEST/out"

for l in mathieeedoubbas.library mathieeedoubtrans.library; do
    if [ -f "$DEST/Libs/$l" ]; then
        echo "    $l present"
    else
        echo "    WARNING: $l missing -- AmiSSL will crash without it" >&2
    fi
done

echo
echo "system drive ready. Next:"
echo "  tools/fetch-deps.sh                 # download AmiSSL"
echo "  tools/install-amissl.sh $DEST"
echo "  cp NextSync.conf.example $DEST/NextSync.conf   # then edit it"
