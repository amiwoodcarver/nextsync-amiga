#!/usr/bin/env bash
#
# Generate an Amiberry configuration for the test rig. Paths are resolved
# at generation time, so nothing with a machine specific path has to be
# committed.
#
#   tools/emu-config.sh <profile> [sysdrive] [kickstart] > machine.uae
#
# Profiles:
#   a4000   68040 with JIT and 64 MB Z3 fast  -- fast, for iterating
#   a1200   68020, no FPU, 8 MB fast          -- honest stock A1200 timing
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

PROFILE="${1:?usage: emu-config.sh <a4000|a1200> [sysdrive] [kickstart]}"
SYS="${2:-$ROOT/emu/hd0}"
KICK="${3:-}"

case "$PROFILE" in
    a4000) CPU=68040; FPU=68040; JIT=8192;  Z3=64; FAST=0; DEFROM="A4000" ;;
    a1200) CPU=68020; FPU=0;     JIT=0;     Z3=0;  FAST=8; DEFROM="A1200" ;;
    *) echo "unknown profile: $PROFILE" >&2; exit 1 ;;
esac

if [ -z "$KICK" ]; then
    KICK=$(ls "$ROOT"/kickstarts/*"$DEFROM"*.rom 2>/dev/null | head -1 || true)
fi
[ -n "$KICK" ] && [ -f "$KICK" ] || {
    echo "no Kickstart ROM found; put one in $ROOT/kickstarts or pass a path" >&2
    echo "(3.1 for a $DEFROM: these ROMs are 68020 only, which AmiSSL needs anyway)" >&2
    exit 1
}
[ -d "$SYS" ] || { echo "no system drive at $SYS -- see README" >&2; exit 1; }

cat <<EOF
config_description=NextSync test rig ($PROFILE)
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
chipset_compatible=$DEFROM
bsdsocket_emu=true
sound_output=none
show_leds=false
nr_floppies=0
floppy0type=-1
filesystem2=rw,DH0:DH0:$SYS,1
uaehf0=dir,rw,DH0:DH0:$SYS,1
EOF
