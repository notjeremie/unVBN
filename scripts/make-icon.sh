#!/bin/sh
# Regenerates App/Assets.xcassets/AppIcon.appiconset and docs/icon.png from scripts/make-icon.swift.
set -eu
cd "$(dirname "$0")/.."
SET=App/Assets.xcassets/AppIcon.appiconset
mkdir -p "$SET" docs
swift scripts/make-icon.swift docs/icon.png
for s in 16 32 128 256 512; do
  sips -z $s $s docs/icon.png --out "$SET/icon_${s}x${s}.png" >/dev/null
  d=$((s * 2)); sips -z $d $d docs/icon.png --out "$SET/icon_${s}x${s}@2x.png" >/dev/null
done
cat > "$SET/Contents.json" <<JSON
{
  "images" : [
$(for s in 16 32 128 256 512; do
  printf '    { "idiom" : "mac", "scale" : "1x", "size" : "%sx%s", "filename" : "icon_%sx%s.png" },\n' $s $s $s $s
  printf '    { "idiom" : "mac", "scale" : "2x", "size" : "%sx%s", "filename" : "icon_%sx%s@2x.png" },\n' $s $s $s $s
done | sed '$ s/,$//')
  ],
  "info" : { "author" : "xcode", "version" : 1 }
}
JSON
printf '{ "info" : { "author" : "xcode", "version" : 1 } }\n' > App/Assets.xcassets/Contents.json
echo "icon set written to $SET"
