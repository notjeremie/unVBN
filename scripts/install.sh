#!/bin/sh
# Build Release, install to ~/Applications, register with Launch Services and reset Quick Look.
set -eu
cd "$(dirname "$0")/.."
command -v xcodegen >/dev/null && xcodegen generate >/dev/null
xcodebuild -project unVBN.xcodeproj -scheme unVBN -configuration Release -derivedDataPath build/DerivedData build -quiet
mkdir -p "$HOME/Applications"
rm -rf "$HOME/Applications/unVBN.app" "$HOME/Applications/iVBN.app" "$HOME/Applications/VBN.app"   # older names
cp -R build/DerivedData/Build/Products/Release/unVBN.app "$HOME/Applications/"
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$HOME/Applications/unVBN.app"
open -g "$HOME/Applications/unVBN.app"
qlmanage -r >/dev/null 2>&1; qlmanage -r cache >/dev/null 2>&1
pluginkit -m -v | grep -i vbn || echo "extensions not listed yet: launch unVBN once from Finder"
