#!/usr/bin/env bash
#
# Build the m68k-amigaos cross toolchain into toolchain/. Takes roughly
# 20 minutes on an M-series Mac. Only needs to be done once.
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/build/m68k-amigaos-gcc"

echo "==> checking Homebrew prerequisites"
brew install bash wget make lhasa gmp mpfr libmpc flex gettext gnu-sed \
             texinfo gcc@12 autoconf bison

mkdir -p "$ROOT/build"
if [ ! -d "$SRC" ]; then
    echo "==> cloning AmigaPorts/m68k-amigaos-gcc"
    git clone --depth 1 https://github.com/AmigaPorts/m68k-amigaos-gcc.git "$SRC"
fi

cd "$SRC"
echo "==> building (this takes a while)"
PATH="$(brew --prefix bison)/bin:$PATH" CC=gcc-12 CXX=g++-12 \
    gmake -j"$(sysctl -n hw.ncpu)" \
        PREFIX="$ROOT/toolchain" \
        SHELL="$(brew --prefix)/bin/bash" \
        binutils gcc libnix libgcc libnix4.library ndk vasm vlink \
        sfdc fd2pragma fd2sfd

echo
"$ROOT/toolchain/bin/m68k-amigaos-gcc" --version | head -1
echo "installed into $ROOT/toolchain"
echo "the source tree in build/ can be deleted once you are happy"
