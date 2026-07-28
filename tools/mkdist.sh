#!/usr/bin/env bash
#
# Assemble the Aminet distribution.
#
#   tools/mkdist.sh              build dist/ and, if lha is around, the archive
#   tools/mkdist.sh --no-archive stage the files only
#
# Produces:
#
#   dist/NextSync.readme     the Aminet readme -- upload this next to the
#                            archive, not inside it
#   dist/NextSync.lha        the archive
#   dist/stage/              exactly what is in the archive, to look through
#                            or edit before it is rolled up
#
# The archive holds a NextSync drawer plus its icon, so it unpacks into one
# tidy drawer wherever the user happens to extract it. Aminet asks for LhA,
# filenames of 30 characters or less, and a readme alongside the upload;
# see aminet/NextSync.readme for the fields.
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

ARCHIVE=1
[ "${1:-}" = "--no-archive" ] && ARCHIVE=0

DIST="$ROOT/dist"
STAGE="$DIST/stage"
DRAWER="$STAGE/NextSync"

echo "==> building"
make -s

VERSION=$(sed -n 's/^Version: *//p' aminet/NextSync.readme | head -1)
[ -n "$VERSION" ] || { echo "no Version: in aminet/NextSync.readme" >&2; exit 1; }

echo "==> staging $DIST (version $VERSION)"
rm -rf "$DIST"
mkdir -p "$DRAWER"

# the program, its icon, and the drawer's own icon one level up
cp NextSync "$DRAWER/NextSync"
chmod 755 "$DRAWER/NextSync"
python3 tools/mkicon.py --stack 100000 "$DRAWER/NextSync.info"
python3 tools/mkicon.py --drawer "$STAGE/NextSync.info"

# documentation: the guide is the real manual, the readme is what Aminet
# shows on the web page and is included here as well out of habit
cp aminet/NextSync.guide "$DRAWER/NextSync.guide"
python3 tools/mkicon.py --project --tool SYS:Utilities/MultiView \
        "$DRAWER/NextSync.guide.info"
cp aminet/NextSync.readme "$DRAWER/NextSync.readme"
cp aminet/NextSync.readme "$DIST/NextSync.readme"

cp NextSync.conf.example "$DRAWER/NextSync.conf.example"

# Amiga text files are LF, like the readme Aminet wants
for f in "$DRAWER/NextSync.guide" "$DRAWER/NextSync.readme" \
         "$DRAWER/NextSync.conf.example" "$DIST/NextSync.readme"; do
    perl -pi -e 's/\r\n/\n/g' "$f"
done

# ------------------------------------------------------------------ #
# checks Aminet would otherwise do for us
# ------------------------------------------------------------------ #

fail=0
note() { printf '    %s\n' "$*"; }

echo "==> checking"

while IFS= read -r line; do
    [ ${#line} -le 78 ] || { note "readme line over 78 chars: ${line:0:40}..."; fail=1; }
done < "$DIST/NextSync.readme"

for field in Short Author Uploader Type Architecture Version; do
    grep -q "^$field:" "$DIST/NextSync.readme" || { note "readme has no $field:"; fail=1; }
done

short=$(sed -n 's/^Short: *//p' "$DIST/NextSync.readme" | head -1)
[ ${#short} -le 40 ] || { note "Short: is ${#short} chars, limit is 40"; fail=1; }

python3 tools/checkguide.py "$DRAWER/NextSync.guide" || fail=1

# the $VER: string in the binary is what the Version command reports, so
# it must not drift from the version being uploaded
# not anchored: strings happily runs the tag together with whatever
# preceded it in the data segment
binver=$(strings "$DRAWER/NextSync" |
         sed -n 's/.*\$VER: NextSync \([0-9.]*\).*/\1/p' | head -1)
if [ -z "$binver" ]; then
    note "the binary carries no \$VER: string"
    fail=1
elif [ "$binver" != "$VERSION" ]; then
    note "binary says version $binver, readme says $VERSION"
    fail=1
else
    note "binary and readme both say version $VERSION"
fi

for f in NextSync.lha NextSync.readme; do
    [ ${#f} -le 30 ] || { note "$f is longer than 30 characters"; fail=1; }
done

if grep -rqiE 'moln\.hinken|hinken\.nu' "$STAGE" "$DIST/NextSync.readme"; then
    note "a real server name is in the distribution"
    fail=1
fi

[ $fail -eq 0 ] && note "readme fields, line lengths and file names look right"

# ------------------------------------------------------------------ #

if [ "$ARCHIVE" = 1 ]; then
    echo "==> packing"

    # Homebrew's "lha" is Lhasa, which only unpacks. Anything that can
    # actually write an archive is used if it is there. Note the banner is
    # captured rather than piped straight into grep: lha exits non-zero
    # when it prints usage, and under pipefail that is what the test would
    # end up looking at.
    banner=""
    command -v lha >/dev/null && banner="$( { lha 2>&1 || true; } | head -1 )"

    if [ -n "$banner" ] && ! printf '%s' "$banner" | grep -qi lhasa; then
        ( cd "$STAGE" && lha -aq2 "$DIST/NextSync.lha" NextSync.info NextSync )
        note "NextSync.lha  $(stat -f%z "$DIST/NextSync.lha" 2>/dev/null ||
                              stat -c%s "$DIST/NextSync.lha") bytes"
    else
        note "no LhA that can create archives on this machine"
        note "(the lha in PATH is Lhasa, which only extracts)"
        note "pack it with LhA on the Amiga, from the parent of the drawer:"
        note "    lha -r a NextSync.lha NextSync.info NextSync"
    fi

    # Aminet takes zip as well, so there is always something uploadable
    ( cd "$STAGE" && zip -qr "$DIST/NextSync.zip" NextSync.info NextSync )
    note "NextSync.zip  $(stat -f%z "$DIST/NextSync.zip" 2>/dev/null ||
                          stat -c%s "$DIST/NextSync.zip") bytes"
fi

echo
echo "ready in dist/"
echo "  stage/          what goes in the archive -- look it over, edit it"
echo "  NextSync.readme upload this beside the archive, not inside it"
echo "  upload to ftp://main.aminet.net/new, anonymous, email as password"
exit $fail
