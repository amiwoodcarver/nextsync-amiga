#!/usr/bin/env bash
#
# Build the emulator's system drive from the Workbench 3.1 disk images.
#
#   tools/setup-sysdrive.sh WORKBENCH.ADF [target]   just the Workbench disk
#   tools/setup-sysdrive.sh /path/to/adfs  [target]  the whole disk set
#
# default target: emu/hd0
#
# Given a directory, every disk of the set that is there gets installed the
# way the Install disk would lay them out:
#
#   WORKBENCH  C, Devs, Libs, L, S, System, Utilities, Prefs, Classes
#   FONTS      SYS:Fonts -- without it FONTS: falls back to the drive root
#   EXTRAS     Tools, and more of Prefs, System and L
#   LOCALE     SYS:Locale, which locale.library and the help system want
#   STORAGE    SYS:Storage: spare monitors, printers, keymaps and the
#              datatype descriptors that are not installed by default
#
# The Workbench disk alone is enough to boot and to run NextSync, but it is
# not a Workbench anybody would recognise: no fonts, no Tools, and a
# datatypes setup that cannot identify a file. Install the set if you have
# it.
#
# AmiSSL's startup opens mathieeedoubbas.library and mathieeedoubtrans.library
# without checking the result, so it needs a real Libs: drawer either way.
#
# Existing files are overwritten but nothing is deleted, so a drive that is
# already set up keeps its configuration and its synced folders.
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

SRC_ARG="${1:?usage: setup-sysdrive.sh <WORKBENCH.ADF | adf-directory> [target]}"
DEST="${2:-$ROOT/emu/hd0}"

command -v xdftool >/dev/null || {
    echo "xdftool not found: pip3 install amitools" >&2
    exit 1
}

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$DEST"

# find <dir>/NAME.ADF however it happens to be capitalised
find_adf() {
    find "$1" -maxdepth 1 -iname "$2.adf" 2>/dev/null | head -1
}

# unpack an image and rsync one of its drawers into the drive
install_adf() {
    local adf="$1" want="$2" dest="$3" out
    local tmp="$TMP/$(basename "$adf" | tr -d ' ').d"

    [ -n "$adf" ] && [ -f "$adf" ] || return 1

    rm -rf "$tmp"
    mkdir -p "$tmp"
    xdftool "$adf" unpack "$tmp" >/dev/null 2>&1 || {
        echo "    could not unpack $(basename "$adf")" >&2
        return 1
    }

    # the disk's contents live in a drawer named after the volume
    out="$(find "$tmp" -maxdepth 1 -mindepth 1 -type d | head -1)"
    [ -n "$out" ] || return 1

    if [ "$want" = "." ]; then
        rsync -a "$out/" "$dest/"
    else
        local sub
        sub="$(find "$out" -maxdepth 1 -mindepth 1 -type d -iname "$want" | head -1)"
        [ -n "$sub" ] || { sub="$out"; }
        rsync -a "$sub/" "$dest/"
    fi
    return 0
}

if [ -d "$SRC_ARG" ]; then
    DIR="$SRC_ARG"
    WB="$(find_adf "$DIR" workbench)"
    [ -n "$WB" ] || { echo "no WORKBENCH.ADF in $DIR" >&2; exit 1; }
else
    DIR="$(dirname "$SRC_ARG")"
    WB="$SRC_ARG"
    [ -f "$WB" ] || { echo "no such file: $WB" >&2; exit 1; }
fi

echo "==> Workbench   $(basename "$WB")"
install_adf "$WB" . "$DEST" || { echo "could not install the Workbench disk" >&2; exit 1; }

if [ -d "$SRC_ARG" ]; then
    for spec in "fonts:Fonts:$DEST/Fonts" \
                "extras:.:$DEST" \
                "locale:Locale:$DEST/Locale" \
                "storage:Storage3.1:$DEST/Storage"; do
        name="${spec%%:*}"; rest="${spec#*:}"
        want="${rest%%:*}"; target="${rest#*:}"
        adf="$(find_adf "$DIR" "$name")"

        if [ -z "$adf" ]; then
            echo "==> $name       not found, skipping"
            continue
        fi
        echo "==> $name       $(basename "$adf")"
        install_adf "$adf" "$want" "$target" || echo "    skipped"
    done
fi

chmod +x "$DEST"/C/* "$DEST"/System/* "$DEST"/Utilities/* 2>/dev/null || true
mkdir -p "$DEST/S" "$DEST/out"

echo "==> checking"
for l in mathieeedoubbas.library mathieeedoubtrans.library; do
    [ -f "$DEST/Libs/$l" ] && echo "    $l" \
        || echo "    WARNING: $l is missing -- AmiSSL will not start" >&2
done
[ -d "$DEST/Fonts" ] && echo "    Fonts" \
    || echo "    no Fonts drawer -- install FONTS.ADF, some programs need it" >&2
[ -d "$DEST/Locale" ] && echo "    Locale" || true
[ -d "$DEST/Storage" ] && echo "    Storage" || true

echo
echo "system drive ready. Next:"
echo "  tools/fetch-deps.sh                 # download AmiSSL"
echo "  tools/install-amissl.sh $DEST"
echo "  tools/emulator.sh                   # boot it"
