#!/usr/bin/env bash
#
# Boot the real Workbench 3.1 hard disk image, with NextSync on it.
#
#   tools/wb31.sh              A4000/040, boots to Workbench
#   tools/wb31.sh a1200        stock A1200 instead
#
# This is a proper Workbench install in a hardfile -- FFS, a real
# Startup-Sequence, DOpus, datatypes that work -- as opposed to the
# directory drive tools/emulator.sh boots, which is assembled from the
# install disks and is only as complete as that makes it.
#
# What is on it:
#
#   DH0:Aminet/NextSync/     the release, as it unpacks from the archive
#   DH0:Libs/AmiSSL/         AmiSSL 5.x runtime
#   DH0:AmiSSL/Certs/        290 CA certificates
#   S:User-Startup           assigns AmiSSL:
#
# Networking is the host's, through Amiberry's bsdsocket emulation, so
# there is no TCP/IP stack to set up in the guest.
#
# HDF= points it at a different image. The original, before any of this
# was added, is kept next to it as wb31-original.hdf.
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

PROFILE="${1:-a4000}"
HDF="${HDF:-/Users/axel/dev/amiga/wb31.hdf}"
AMIBERRY="${AMIBERRY:-/Applications/Amiberry.app/Contents/MacOS/Amiberry}"

case "$PROFILE" in
    a4000) CPU=68040; FPU=68040; JIT=8192; Z3=64; FAST=0; ROM=A4000 ;;
    a1200) CPU=68020; FPU=0;     JIT=0;    Z3=0;  FAST=8; ROM=A1200 ;;
    *) echo "unknown profile: $PROFILE (a4000 or a1200)" >&2; exit 1 ;;
esac

[ -x "$AMIBERRY" ] || { echo "Amiberry not found at $AMIBERRY" >&2; exit 1; }
[ -f "$HDF" ] || { echo "no hard disk image at $HDF" >&2; exit 1; }

KICK=$(ls "$ROOT"/kickstarts/*"$ROM"*.rom 2>/dev/null | head -1 || true)
[ -n "$KICK" ] || { echo "no Kickstart 3.1 ROM for an $ROM in kickstarts/" >&2; exit 1; }

CONF="$ROOT/emu/configs/wb31-$PROFILE.uae"
mkdir -p "$(dirname "$CONF")"

cat >"$CONF" <<EOF
config_description=NextSync on Workbench 3.1 ($PROFILE)
kickstart_rom_file=$KICK
cpu_model=$CPU
fpu_model=$FPU
cpu_compatible=false
cpu_24bit_addressing=false
cachesize=$JIT
z3mem_size=$Z3
fastmem_size=$FAST
chipmem_size=4
chipset=aga
chipset_compatible=$ROM
bsdsocket_emu=true
sound_output=none
nr_floppies=0
floppy0type=-1
hardfile2=rw,DH0:$HDF,0,0,0,512,0,,uae
gfx_width=640
gfx_height=512
gfx_width_windowed=640
gfx_height_windowed=512
gfx_fullscreen_amiga=false
gfx_linemode=double
gfx_correct_aspect=true
show_leds=true
EOF

cat <<EOF

  Workbench 3.1 on $PROFILE, from $(basename "$HDF")

  Open DH0, then the Aminet drawer.
    NextSync/NextSync.guide   double click to read the manual
    NextSync/NextSync         the program; with no configuration it
                              opens Preferences by itself

  F12 for Amiberry's menu, and to quit.

EOF

exec "$AMIBERRY" -f "$CONF" -G
