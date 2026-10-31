#!/bin/sh
# Builds a Developer ID–signed, notarized unVBN-<version>.dmg into build/release/.
#
#   TEAM_ID=XXXXXXXXXX NOTARY_PROFILE=<profile> scripts/release.sh
#
# TEAM_ID        Apple team whose "Developer ID Application" certificate is in the keychain.
# NOTARY_PROFILE notarytool keychain profile, created once with
#                `xcrun notarytool store-credentials <profile> --apple-id <id> --team-id <TEAM_ID>`.
#                Without it the dmg is signed but not notarized (Gatekeeper will refuse it on other Macs).
#
# project.yml stays ad-hoc so anyone can build; the Developer ID settings are passed only here.
set -eu
cd "$(dirname "$0")/.."
: "${TEAM_ID:?set TEAM_ID}"
# Build and package outside ~/Documents: iCloud Drive (File Provider) tags files there with
# com.apple.FinderInfo, which `codesign --strict` and notarization reject. Only the final dmg is copied back.
WORK="${TMPDIR:-/tmp}/unvbn-release"
OUT=build/release
APP=unVBN.app
VERSION=$(sed -n 's/^ *MARKETING_VERSION: *"\(.*\)"/\1/p' project.yml)
DMG="$WORK/unVBN-$VERSION.dmg"
IDENTITY="Developer ID Application"

scripts/prebuild.sh
rm -rf "$WORK" "$OUT"; mkdir -p "$WORK" "$OUT"

echo "==> build $VERSION (Release, $IDENTITY, hardened runtime)"
xcodebuild -project unVBN.xcodeproj -scheme unVBN -configuration Release \
  -derivedDataPath "$WORK/DerivedData" build -quiet \
  CODE_SIGN_STYLE=Manual CODE_SIGN_IDENTITY="$IDENTITY" DEVELOPMENT_TEAM="$TEAM_ID" \
  ENABLE_HARDENED_RUNTIME=YES CODE_SIGN_INJECT_BASE_ENTITLEMENTS=NO OTHER_CODE_SIGN_FLAGS=--timestamp
ditto "$WORK/DerivedData/Build/Products/Release/$APP" "$WORK/$APP"

echo "==> verify signature"
codesign --verify --deep --strict --verbose=2 "$WORK/$APP"
if codesign -d --entitlements - "$WORK/$APP" 2>/dev/null | grep -q get-task-allow; then
  echo "get-task-allow present: refusing to ship a debuggable build" >&2; exit 1
fi
for helper in ffmpeg unvbn; do
  bin="$WORK/$APP/Contents/Helpers/$helper"
  [ -x "$bin" ] || { echo "missing Contents/Helpers/$helper" >&2; exit 1; }
  codesign -dv "$bin" 2>&1 | grep -q "TeamIdentifier=$TEAM_ID" || { echo "$helper not signed by $TEAM_ID" >&2; exit 1; }
  codesign -dv "$bin" 2>&1 | grep -q "flags=.*runtime" || { echo "$helper lacks the hardened runtime" >&2; exit 1; }
done

echo "==> dmg"
STAGE="$WORK/dmg"; mkdir -p "$STAGE"
ditto "$WORK/$APP" "$STAGE/$APP"; ln -s /Applications "$STAGE/Applications"
hdiutil create -quiet -volname "unVBN $VERSION" -srcfolder "$STAGE" -fs HFS+ -format UDZO -ov "$DMG"
rm -rf "$STAGE"
codesign --sign "$IDENTITY" --timestamp "$DMG"

if [ -n "${NOTARY_PROFILE:-}" ]; then
  echo "==> notarize (a few minutes)"
  xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait
  xcrun stapler staple "$DMG"
  spctl -a -t open --context context:primary-signature -vv "$DMG"
else
  echo "NOTARY_PROFILE not set: $DMG is signed but NOT notarized"
fi
cp "$DMG" "$OUT/"
(cd "$OUT" && shasum -a 256 "$(basename "$DMG")")
