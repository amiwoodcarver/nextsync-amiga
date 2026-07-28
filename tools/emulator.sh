#!/usr/bin/env bash
#
# Turnkey Amiberry: build NextSync, install it on a bootable Workbench 3.1
# drive with an icon, and hand you the machine.
#
#   tools/emulator.sh                 boot to Workbench, A4000/040
#   tools/emulator.sh a1200           stock A1200 speed instead
#   tools/emulator.sh --fresh         forget the configuration first, so
#                                     Preferences opens like a first run
#   tools/emulator.sh --shell         boot to a Shell rather than Workbench
#
# Once it is up: open the DH0 icon, double click NextSync. Networking is
# the host's, through Amiberry's bsdsocket emulation, so nothing has to be
# configured inside the guest.
#
# Everything the guest writes to DH0: appears under emu/hd0 on the Mac
# immediately, and the other way round.
#
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

PROFILE=a4000
FRESH=0
BOOT=workbench

for a in "$@"; do
    case "$a" in
        a1200|a4000) PROFILE="$a" ;;
        --fresh)     FRESH=1 ;;
        --shell)     BOOT=shell ;;
        -h|--help)   sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $a  (try --help)" >&2; exit 1 ;;
    esac
done

SYS="${SYSDRIVE:-$ROOT/emu/hd0}"
AMIBERRY="${AMIBERRY:-/Applications/Amiberry.app/Contents/MacOS/Amiberry}"

say()  { printf '  %s\n' "$*"; }
die()  { printf '\n%s\n' "$*" >&2; exit 1; }

# ------------------------------------------------------------------ #
# prerequisites, each with the command that fixes it                   #
# ------------------------------------------------------------------ #

[ -x "$AMIBERRY" ] || die "Amiberry not found at $AMIBERRY
  brew install --cask amiberry
  (or point AMIBERRY= at it)"

ls kickstarts/*.rom >/dev/null 2>&1 || die "no Kickstart ROM in kickstarts/
  A 3.1 ROM for the A1200 or A4000. These are Commodore's, so they are
  not in this repository."

[ -d "$SYS/C" ] || die "no Workbench install at $SYS
  tools/setup-sysdrive.sh /path/to/WORKBENCH.ADF"

[ -f "$SYS/Libs/amisslmaster.library" ] || die "AmiSSL is not installed at $SYS
  tools/fetch-deps.sh && tools/install-amissl.sh $SYS"

for l in mathieeedoubbas.library mathieeedoubtrans.library; do
    [ -f "$SYS/Libs/$l" ] || die "$SYS/Libs/$l is missing.
  AmiSSL's startup calls into it without checking and the guest will hang.
  Rebuild the drive with tools/setup-sysdrive.sh."
done

# ------------------------------------------------------------------ #
# build and install                                                    #
# ------------------------------------------------------------------ #

echo "==> building"
make -s || exit 1

echo "==> installing on $SYS"
cp NextSync "$SYS/NextSync" && chmod 755 "$SYS/NextSync"
python3 tools/mkicon.py --stack 100000 "$SYS/NextSync.info"
say "NextSync + icon in DH0:"

mkdir -p "$SYS/out"

if [ "$FRESH" = 1 ]; then
    # moved aside rather than deleted: it holds a password
    [ -f "$SYS/NextSync.conf" ] && mv "$SYS/NextSync.conf" "$SYS/NextSync.conf.old"
    say "configuration set aside as NextSync.conf.old"
    say "NextSync will open Preferences on startup"
elif [ -f "$SYS/NextSync.conf" ]; then
    say "keeping the existing NextSync.conf (--fresh clears it)"
else
    say "no NextSync.conf yet: NextSync will open Preferences on startup"
fi

# ------------------------------------------------------------------ #
# boot script                                                          #
# ------------------------------------------------------------------ #
#
# Note this is rewritten on every launch, and tools/run-emu.sh replaces it
# with a scripted one -- so whichever you ran last decides how DH0: boots.

{
    cat <<'EOF'
FailAt 21
C:SetPatch QUIET
C:Version >NIL:
C:AddBuffers >NIL: DH0: 30
C:MakeDir >NIL: RAM:T RAM:Clipboards RAM:Env RAM:Env/Sys
C:Assign >NIL: ENV: RAM:Env
C:Assign >NIL: T: RAM:T
C:Assign >NIL: CLIPS: RAM:Clipboards
C:Assign >NIL: ENVARC: SYS:Prefs/Env-Archive
C:Assign >NIL: REXX: SYS:Rexxc
C:Assign >NIL: FONTS: SYS:Fonts
C:Copy >NIL: ENVARC: ENV: ALL QUIET
C:Assign >NIL: AmiSSL: SYS:AmiSSL
IF EXISTS SYS:Locale
  C:Assign >NIL: LOCALE: SYS:Locale
  C:Assign >NIL: HELP: LOCALE:Help DEFER
ENDIF
C:ConClip
; datatypes has to be registered or MultiView cannot identify anything,
; not even plain text -- this is what lets a .guide be opened
C:AddDataTypes REFRESH QUIET
EOF
    if [ "$BOOT" = workbench ]; then
        cat <<'EOF'
C:IPrefs
Echo "Starting Workbench. NextSync is in the DH0: window."
C:LoadWB
EndCLI >NIL:
EOF
    else
        cat <<'EOF'
Stack 100000
Echo "NextSync SYNC to sync, NextSync for the GUI, NextSync PREFS to set up."
EOF
    fi
} >"$SYS/S/Startup-Sequence"

CONF="$ROOT/emu/configs/$PROFILE-interactive.uae"
mkdir -p "$(dirname "$CONF")"
tools/emu-config.sh "$PROFILE" "$SYS" "" interactive >"$CONF" || exit 1

# ------------------------------------------------------------------ #

cat <<EOF

  profile   $PROFILE   (kickstart $(basename "$(ls kickstarts/*"$([ "$PROFILE" = a1200 ] && echo A1200 || echo A4000)"*.rom 2>/dev/null | head -1)" 2>/dev/null))
  boots to  $BOOT
  drive     $SYS  <->  DH0:

  F12 opens Amiberry's own menu, and quits from there.

EOF

# -G boots the machine straight away instead of stopping at Amiberry's own
# configuration window; F12 brings that up later if it is wanted.
exec "$AMIBERRY" -f "$CONF" -G
