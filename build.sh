#!/bin/sh
# Builds bin/imspatch (static arm64) and the module zip.
# Needs the Android NDK; set ANDROID_NDK_HOME or have one under $ANDROID_HOME/ndk.
set -eu
cd "$(dirname "$0")"

SDK=${ANDROID_HOME:-$HOME/Library/Android/sdk}
NDK=${ANDROID_NDK_HOME:-$(ls -d "$SDK"/ndk/* 2>/dev/null | sort -V | tail -1)}
[ -d "$NDK" ] || { echo "Android NDK not found; set ANDROID_NDK_HOME" >&2; exit 1; }
HOST=$(ls "$NDK/toolchains/llvm/prebuilt" | head -1)
CC="$NDK/toolchains/llvm/prebuilt/$HOST/bin/aarch64-linux-android30-clang"

mkdir -p bin
"$CC" -static -O2 -Wall -Wextra -Werror -o bin/imspatch src/imspatch.c
"$NDK/toolchains/llvm/prebuilt/$HOST/bin/llvm-strip" bin/imspatch

VERSION=$(sed -n 's/^version=//p' module.prop)
rm -f "pixel_skt_mt_fix-$VERSION.zip"
zip -q -X "pixel_skt_mt_fix-$VERSION.zip" module.prop customize.sh common.sh service.sh action.sh uninstall.sh bin/imspatch
echo "built pixel_skt_mt_fix-$VERSION.zip"
