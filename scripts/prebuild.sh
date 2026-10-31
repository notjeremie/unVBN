#!/bin/sh
# Shared first step of install.sh and release.sh: refuse iCloud conflict copies, regenerate the
# Xcode project only when project.yml changed, and make sure the bundled ffmpeg is built (cached).
set -eu
cd "$(dirname "$0")/.."
# iCloud Drive can leave conflict copies ("ExportView 2.swift") that XcodeGen would compile.
dupes=$(find . \( -path ./build -o -path ./build.nosync -o -path ./.git -o -path ./.git.nosync -o -path ./samples \) -prune -o -name "* [0-9]*" -print)
if [ -n "$dupes" ]; then echo "iCloud conflict copies present, remove them first:" >&2; echo "$dupes" >&2; exit 1; fi
# --use-cache: no rewrite of unVBN.xcodeproj (and no new iCloud upload) when project.yml is unchanged.
command -v xcodegen >/dev/null && xcodegen generate --use-cache >/dev/null
scripts/build-ffmpeg.sh >/dev/null   # the app embeds build/ffmpeg/ffmpeg for WebM export
