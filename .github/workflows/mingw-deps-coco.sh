#!/usr/bin/env bash
# The extra static library the CoCo desktop's Windows build needs, on top of mingw-deps.sh: TRE, the
# POSIX regex library XRoar's portalib/sdsx.c uses (MinGW has no regex.h). Run after mingw-deps.sh
# with the same <prefix>; needs autoconf, automake, libtool, gettext and autopoint.
set -euo pipefail

PREFIX=${1:?usage: mingw-deps-coco.sh <prefix>}
PREFIX=$(cd "$PREFIX" && pwd)
WORK=$PREFIX/src
JOBS=$(nproc)

TRIPLE=x86_64-w64-mingw32
TRE_COMMIT=d0e0c997336b3210f05b3e1daa7bb5cb9900d274 # v0.9.0

mkdir -p "$WORK"
cd "$WORK"
git init -q tre-src
git -C tre-src fetch -q --depth 1 https://github.com/laurikari/tre.git "$TRE_COMMIT"
git -C tre-src checkout -q FETCH_HEAD
test "$(git -C tre-src rev-parse HEAD)" = "$TRE_COMMIT"
(
    cd tre-src
    ./utils/autogen.sh
    ./configure --host=$TRIPLE --prefix="$PREFIX" --disable-shared --enable-static \
        --disable-agrep --disable-approx --disable-nls CC=$TRIPLE-gcc-posix
    make -j"$JOBS"
    make install
)
# The MSYS2 build links `-lsystre -ltre`; libsystre only fronts TRE with the POSIX names, which TRE's
# own tre/regex.h already maps, so an empty archive satisfies the link line.
rm -f "$PREFIX/lib/libsystre.a"
$TRIPLE-ar rcs "$PREFIX/lib/libsystre.a"

echo "TRE installed in $PREFIX"
