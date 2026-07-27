#!/usr/bin/env bash
#
# Fetch the third party pieces that are not kept in the repository:
# the AmiSSL SDK (headers + link libraries, needed to compile NextSync)
# and the AmiSSL OS3 runtime (libraries + CA certificates, needed to run
# it). Both are Apache-2.0 licensed, from jens-maus/amissl.
#
#   tools/fetch-deps.sh [version]        default: 5.27
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VER="${1:-5.27}"
BASE="https://github.com/jens-maus/amissl/releases/download/$VER"

command -v lha >/dev/null || {
    echo "lha not found: brew install lhasa" >&2
    exit 1
}

mkdir -p "$ROOT/vendor"
cd "$ROOT/vendor"

for kind in SDK OS3; do
    f="AmiSSL-$VER-$kind.lha"
    if [ ! -f "$f" ]; then
        echo "==> downloading $f"
        curl -fL --progress-bar -o "$f" "$BASE/$f"
    fi
done

echo "==> extracting"
rm -rf sdk os3
mkdir -p sdk os3
(cd sdk && lha xq "../AmiSSL-$VER-SDK.lha")
(cd os3 && lha xq "../AmiSSL-$VER-OS3.lha")

echo
echo "AmiSSL $VER ready:"
echo "  vendor/sdk/AmiSSL/Developer   headers and link libraries"
echo "  vendor/os3/AmiSSL             runtime libraries and CA certificates"
echo
echo "Install the runtime into your Amiga system drive with:"
echo "  tools/install-amissl.sh <path to system drive>"
