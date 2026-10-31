#!/bin/sh
# Build Release, install to ~/Applications, register with Launch Services and reset Quick Look.
set -eu
cd "$(dirname "$0")/.."
command -v xcodegen >/dev/null && xcodegen generate >/dev/null
xcodebuild -project VBN.xcodeproj -scheme VBN -configuration Release -derivedDataPath build/DerivedData build -quiet
mkdir -p "$HOME/Applications"
rm -rf "$HOME/Applications/VBN.app"
cp -R build/DerivedData/Build/Products/Release/VBN.app "$HOME/Applications/"
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$HOME/Applications/VBN.app"
open -g "$HOME/Applications/VBN.app"
qlmanage -r >/dev/null 2>&1; qlmanage -r cache >/dev/null 2>&1
pluginkit -m -v | grep -i vbn || echo "extensions not listed yet: launch VBN once from Finder"
