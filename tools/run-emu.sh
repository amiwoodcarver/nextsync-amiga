#!/usr/bin/env bash
#
# Build NextSync, drop it on the system drive, boot Amiberry running the
# given command, wait for it to finish (or crash), report.
#
#   tools/run-emu.sh ["command"] [profile]
#
#     command   what the startup sequence runs. Default "NextSync SYNC".
#     profile   a4000 (default, fast) or a1200 (stock timing)
#
# Environment:
#   SYSDRIVE  system drive directory        (default emu/hd0)
#   TIMEOUT   seconds before giving up      (default 180)
#
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CMD="${1:-NextSync SYNC}"
PROFILE="${2:-a4000}"
SYS="${SYSDRIVE:-$ROOT/emu/hd0}"
TIMEOUT="${TIMEOUT:-180}"

AMIBERRY="${AMIBERRY:-/Applications/Amiberry.app/Contents/MacOS/Amiberry}"
[ -x "$AMIBERRY" ] || { echo "Amiberry not found (brew install --cask amiberry)" >&2; exit 1; }

make -s || exit 1

[ -d "$SYS" ] || { echo "no system drive at $SYS -- see README section 3" >&2; exit 1; }
cp NextSync "$SYS/NextSync" && chmod 755 "$SYS/NextSync"

rm -rf "$SYS/out"; mkdir -p "$SYS/out" "$SYS/S"
cat >"$SYS/S/Startup-Sequence" <<EOF
FailAt 21
C:SetPatch QUIET
C:MakeDir >NIL: RAM:T RAM:Clipboards RAM:Env
C:Assign >NIL: ENV: RAM:Env
C:Assign >NIL: T: RAM:T
C:Assign >NIL: CLIPS: RAM:Clipboards
C:Assign >NIL: AmiSSL: SYS:AmiSSL
Stack 100000
$CMD
Echo >out/done.txt "d"
EndCLI
EOF

CONF="$ROOT/emu/configs/$PROFILE.uae"
mkdir -p "$(dirname "$CONF")"
tools/emu-config.sh "$PROFILE" "$SYS" >"$CONF" || exit 1

ALOG="$HOME/Documents/Amiberry/Amiberry.log"
rm -f "$ALOG"
"$AMIBERRY" --log -f "$CONF" -G >/tmp/nextsync-emu.log 2>&1 &
PID=$!

rc=1
for i in $(seq 1 "$TIMEOUT"); do
    sleep 1
    if [ -f "$SYS/out/done.txt" ]; then
        echo "[emu] finished after ${i}s"
        rc=0
        break
    fi
    if grep -q -i -E "cpu halted|double fault" "$ALOG" /tmp/nextsync-emu.log 2>/dev/null; then
        echo "[emu] guest crashed after ${i}s"
        break
    fi
    kill -0 $PID 2>/dev/null || { echo "[emu] emulator exited after ${i}s"; break; }
done
[ $rc -ne 0 ] && [ "$i" -ge "$TIMEOUT" ] && echo "[emu] timed out after ${TIMEOUT}s"
kill $PID 2>/dev/null; wait $PID 2>/dev/null

for f in "$SYS"/out/*.log "$SYS"/out/*.txt; do
    [ -f "$f" ] && [ -s "$f" ] || continue
    case "$f" in *done.txt) continue ;; esac
    echo "--- $(basename "$f") ---"
    cat "$f"
done

# any screen snapshot the guest saved becomes a PNG on the host
for a in "$SYS"/out/*.ags; do
    [ -f "$a" ] || continue
    python3 "$ROOT/tools/ags2png.py" "$a" "$ROOT/docs/$(basename "${a%.ags}").png"
done

exit $rc
