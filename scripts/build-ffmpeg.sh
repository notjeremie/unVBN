#!/bin/sh
# Builds the tiny ffmpeg that unVBN.app bundles (Contents/Helpers/ffmpeg).
#
# Purpose: unVBN only needs ffmpeg to encode WebM VP9 with alpha from raw RGBA
# frames piped on stdin. So this builds a minimal, universal (arm64 + x86_64),
# macOS 14+ ffmpeg with libvpx linked statically.
#
# License: ffmpeg is built WITHOUT --enable-gpl / --enable-nonfree, so the result
# is LGPL-2.1-or-later and can be shipped as a separate, unmodified executable.
# libvpx is BSD-3-Clause. See THIRD_PARTY_NOTICES.md.
#
# Output: build/ffmpeg/ffmpeg  (build/ is gitignored). Re-running is a no-op when
# the output exists and the version stamp matches.
#
# Notes: no nasm/yasm needed. libvpx's x86_64 slice uses the "generic-gnu" target
# (plain C, slower encode on Intel Macs) and ffmpeg gets --disable-x86asm.
set -eu
cd "$(dirname "$0")/.."
ROOT=$(pwd)

FFMPEG_VERSION=7.1.5
FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz"
# SHA-256 values were pinned on first download (computed by us, not from upstream).
FFMPEG_SHA256=de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f
VPX_VERSION=1.15.2
VPX_URL="https://github.com/webmproject/libvpx/archive/refs/tags/v${VPX_VERSION}.tar.gz"
VPX_SHA256=26fcd3db88045dee380e581862a6ef106f49b74b6396ee95c2993a260b4636aa
MINVER=14.0
JOBS=10

# Bump the trailing number if the flags below change.
STAMP="ffmpeg-${FFMPEG_VERSION} libvpx-${VPX_VERSION} min${MINVER} cfg6"

OUT="$ROOT/build/ffmpeg"
# Build under a fixed neutral path: ffmpeg's configure line (shown by `ffmpeg -version`) and libvpx's
# prefix are embedded in the binary, and must not record the builder's home folder.
WORK="/tmp/unvbn-ffmpeg-build"

if [ -x "$OUT/ffmpeg" ] && [ "$(cat "$OUT/stamp" 2>/dev/null || true)" = "$STAMP" ]; then
  echo "build/ffmpeg/ffmpeg is up to date ($STAMP)"
  exit 0
fi

mkdir -p "$WORK" "$OUT"

fetch() { # url file sha256
  if [ ! -f "$WORK/$2" ]; then
    echo "Downloading $1"
    curl -fL --retry 3 -o "$WORK/$2.part" "$1"
    mv "$WORK/$2.part" "$WORK/$2"
  fi
  echo "$3  $WORK/$2" | shasum -a 256 -c - >/dev/null || {
    echo "SHA-256 mismatch for $2: aborting" >&2
    exit 1
  }
}

fetch "$FFMPEG_URL" "ffmpeg-${FFMPEG_VERSION}.tar.xz" "$FFMPEG_SHA256"
fetch "$VPX_URL" "libvpx-${VPX_VERSION}.tar.gz" "$VPX_SHA256"

# Each archive is extracted once into its own fresh directory.
extract() { # archive dir
  if [ ! -f "$2/.extracted" ]; then
    rm -rf "$2"
    mkdir -p "$2"
    tar -xf "$WORK/$1" -C "$2" --strip-components=1
    touch "$2/.extracted"
  fi
}
extract "ffmpeg-${FFMPEG_VERSION}.tar.xz" "$WORK/ffmpeg-$FFMPEG_VERSION"
extract "libvpx-${VPX_VERSION}.tar.gz" "$WORK/libvpx-$VPX_VERSION"

# Flags shared by both arches (also listed in THIRD_PARTY_NOTICES.md).
FF_FLAGS="--disable-everything --disable-autodetect --disable-doc --disable-ffplay --disable-ffprobe \
--disable-network --disable-debug --disable-avdevice --disable-postproc --enable-small \
--enable-libvpx --enable-encoder=libvpx_vp9 --enable-muxer=webm --enable-demuxer=rawvideo \
--enable-decoder=rawvideo --enable-bsf=vp9_superframe --enable-protocol=pipe --enable-protocol=fd --enable-protocol=file \
--enable-filter=buffer --enable-filter=buffersink --enable-filter=format --enable-filter=scale \
--enable-filter=null --enable-filter=nullsink --enable-filter=copy --enable-swscale"

build_arch() { # arch
  A=$1
  VB="$WORK/vpx-build-$A"
  VI="$WORK/vpx-install-$A"
  FB="$WORK/ffmpeg-build-$A"
  CCX="clang -arch $A -mmacosx-version-min=$MINVER"
  CXXX="clang++ -arch $A -mmacosx-version-min=$MINVER"

  if [ ! -f "$VI/lib/libvpx.a" ]; then
    rm -rf "$VB" "$VI"
    mkdir -p "$VB"
    case $A in
      arm64)  VT=arm64-darwin23-gcc; VRTCPU="" ;;  # runtime detect REQUIRED: without it SVE is assumed and SIGILLs on M1-M3
      x86_64) VT=generic-gnu; VRTCPU=--disable-runtime-cpu-detect ;;   # no nasm available; C-only fallback
    esac
    (cd "$VB" && CC="$CCX" CXX="$CXXX" LD="clang -arch $A" AR=ar \
      "$WORK/libvpx-$VPX_VERSION/configure" --target=$VT --prefix="$VI" \
      --disable-shared --enable-static --enable-pic --disable-examples --disable-tools \
      --disable-docs --disable-unit-tests --disable-install-bins --disable-install-srcs \
      --enable-vp9 --enable-vp9-encoder --disable-vp9-decoder --disable-vp8 \
      $VRTCPU >configure.log 2>&1 &&
      make -j$JOBS >make.log 2>&1 && make install >install.log 2>&1)
  fi

  if [ ! -x "$FB/ffmpeg" ]; then
    rm -rf "$FB"
    mkdir -p "$FB"
    XARGS=""
    if [ "$A" = x86_64 ]; then
      XARGS="--enable-cross-compile --arch=x86_64 --target-os=darwin --disable-x86asm"
    fi
    # pkg-config is not installed: --pkg-config=false makes ffmpeg fall back to check_lib.
    # shellcheck disable=SC2086
    (cd "$FB" && "$WORK/ffmpeg-$FFMPEG_VERSION/configure" \
      --cc="$CCX" --pkg-config=false $XARGS $FF_FLAGS \
      --extra-cflags="-I$VI/include -mmacosx-version-min=$MINVER" \
      --extra-ldflags="-L$VI/lib -arch $A -mmacosx-version-min=$MINVER" \
      --extra-libs="-lm -lpthread" >configure.log 2>&1 &&
      make -j$JOBS >make.log 2>&1) || {
      echo "ffmpeg build failed for $A; see $FB/{configure,make}.log" >&2
      exit 1
    }
  fi
}

for A in arm64 x86_64; do
  echo "Building $A"
  build_arch $A
done

lipo -create "$WORK/ffmpeg-build-arm64/ffmpeg" "$WORK/ffmpeg-build-x86_64/ffmpeg" -output "$OUT/ffmpeg"
strip -x "$OUT/ffmpeg"
xattr -c "$OUT/ffmpeg"   # iCloud FinderInfo xattrs break codesign
echo "$STAMP" > "$OUT/stamp"
echo "Built $OUT/ffmpeg: $(lipo -archs "$OUT/ffmpeg")"
