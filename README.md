<p align="center"><img src="docs/icon.png" width="160" alt="unVBN icon"></p>

# unVBN: Vizrt `.vbn` images on macOS and Windows

Vizrt Viz Artist / Viz Engine store their images and animations as `.vbn` files, which no regular image
app can open. unVBN makes them viewable on a Mac and on Windows, and converts animations to video on both.

| | macOS | Windows |
|---|---|---|
| Thumbnails | Finder | Explorer |
| Preview | Quick Look (space bar) | Windows Photo Viewer |
| Double-click | Opens as a PNG in Preview | Opens in Windows Photo Viewer |
| Other apps | – | Any app that reads images through Windows' WIC |
| Animations → PNG sequence, MP4, WebM | ✅ app + `unvbn` command line | ✅ export window (right-click a folder) + `unvbn` command line |

Download: **[latest release](https://github.com/notjeremie/unVBN/releases/latest)**. The format is documented in
[`FORMAT.md`](FORMAT.md).

## Install

### macOS (14 or later, Apple silicon and Intel)

1. Download `unVBN-<version>.dmg` and drag **unVBN** to Applications.
2. Launch it once (this registers the `.vbn` type and the Quick Look extensions), then quit it.

From then on, the space bar and thumbnails work in Finder. The app is signed with a Developer ID and
notarized by Apple.

### Windows (10 or 11)

1. Download `unVBN-windows-<version>-x64.msi`. Use the `-arm64.msi` only on an ARM PC (Snapdragon laptop,
   Surface Pro X); *Settings > System > About* shows which one you have.
2. Run it. The installer is not code-signed, so Windows SmartScreen warns first: *More info > Run anyway*.
   It needs administrator rights and installs to `C:\Program Files\unVBN\`.
3. If thumbnails don't appear right away, restart Explorer (Task Manager > *Windows Explorer* > *Restart*).

Uninstall from *Settings > Apps*. If another program (such as a Vizrt install) already owns the `.vbn` file
type, unVBN keeps its default app and thumbnails and only adds itself to *Open with*.

## Using it

### macOS: viewing

- **Space bar** in Finder: Quick Look preview. **Thumbnails** appear in Finder.
- **Double-click** a `.vbn`: it opens as a PNG in Preview (the app converts it to a temporary PNG, because
  Apple has no way for third parties to teach Preview a new format). *File > Export PNG…* saves that PNG.

### macOS: converting animations

An animation is a folder of numbered frames, `NAME_00000.vbn` … `NAME_00749.vbn`. unVBN turns it into:

| Format | Transparency | Notes |
|---|---|---|
| PNG sequence | yes | One PNG per frame, same names as the source frames. |
| MP4 (H.264) | no | Transparent areas are flattened onto a background colour (black by default). BT.709. |
| WebM (VP9) | yes | Plays with transparency in Chrome and Firefox. |

**In the app**: drop the folder (or any one frame) on the unVBN window or Dock icon, or use
*File > Export Sequence…* (⇧⌘E). Pick the format and frame rate (50 fps by default), then *Export…*.
A progress bar shows the frame, speed and time left, also on the Dock icon. A folder holding several
animations offers a picker.

**Several folders at once**: drop (or choose) several folders together. They are exported one after the
other with the same settings, into one folder you pick (*Export All…*). Each gets its own file; two animations
with the same name become `NAME (FOLDER).webm`. If some exports already exist there, unVBN asks before
replacing them. A folder that fails is reported and the others still export.

**On the command line**: the `unvbn` tool is inside the app. Link it once:

```sh
ln -s ~/Applications/unVBN.app/Contents/Helpers/unvbn /usr/local/bin/unvbn   # or any folder on your PATH
unvbn info LOGO_FOLDER/                       # name, frame count, size, missing frames
unvbn LOGO_FOLDER/ -f webm                    # -> LOGO.webm next to the folder
unvbn LOGO_00000.vbn -f mp4 --fps 25 -o logo.mp4
unvbn ALL_LOGOS/ -f png                       # every animation in the folder -> NAME_png/ folders
unvbn LOGO_A/ LOGO_B/ LOGO_C/ -f webm         # several folders, one after the other
unvbn LOGO_FOLDER/ -f mp4 --background '#FFFFFF'
unvbn LOGO_FOLDER/ -f webm --crf 20           # WebM quality: 0 (best) … 63, default 30
```

Frames are ordered by the number at the end of their names. Gaps in the numbering are reported and
skipped; every frame must have the same size.

### Windows: viewing

Once installed, Windows reads `.vbn` like one of its own image formats: thumbnails in Explorer,
double-click opens Windows Photo Viewer, and apps that load images through WIC (the Windows Imaging
Component) can open them.

### Windows: converting animations

Same formats and options as on the Mac (table above).

**In Explorer**: right-click an animation's folder > *Export with unVBN…* (on Windows 11 it is under
*Show more options*). Select several folders first to export them together. The export window lists every
animation it found, all ticked; untick the ones to skip, pick the format and frame rate, then *Export…* (one
animation) or *Export All…* (several, into one folder you pick, `NAME (FOLDER)` when two share a name).
More folders can be dropped on the window. Progress also shows on the taskbar button.

**On the command line**: the installer puts `unvbn` on the PATH (open a new terminal after installing).
Same commands as on the Mac:

```bat
unvbn info LOGO_FOLDER
unvbn LOGO_FOLDER -f webm
unvbn LOGO_A LOGO_B LOGO_C -f mp4 --fps 25 --background #FFFFFF
```

Ctrl+C stops an export and deletes the unfinished file.

## Good to know

- **No sound.** A `.vbn` holds one image; an animation is just a folder of frames. Sound on air comes
  from elsewhere in the playout system, so exported videos are silent.
- **Windows Photos app**: it does not use installed codecs, so it can't open `.vbn`. Double-click
  (Windows Photo Viewer) works instead.
- **Colours before 0.3.0 were wrong** for uncompressed `.vbn` files: red and blue were swapped (a red logo
  showed blue). Compressed (DXT5) files were fine. Re-export anything made from uncompressed files with an
  older version.
- **WebM on Intel Macs** is slower than on Apple silicon (the bundled encoder uses plain C there).
- **MP4 on Windows** uses Windows' own H.264 encoder, which refuses frames with a side of 32 pixels or less
  and very large ones (8K): tiny animations are padded to 34 pixels with the background colour, and
  oversized ones fail with a message. WebM and PNG have no such limit. On Windows N editions, MP4 needs the
  Media Feature Pack.
- **Transparency**: every real `.vbn` checked so far was opaque; transparency handling is verified on test
  images.

## Development

Real Viz files belong to whoever made them: **never commit them**. `*.vbn` and `samples/` are git-ignored;
the real-sample tests read `samples/` when it exists and are skipped otherwise.

### macOS app

`project.yml` is the source of truth; `unVBN.xcodeproj` is generated with
[XcodeGen](https://github.com/yonaskolb/XcodeGen) and committed. After editing `project.yml`, run
`xcodegen generate` and commit both.

```sh
scripts/build-ffmpeg.sh     # once: the minimal LGPL ffmpeg used for WebM (pinned, checksummed sources, ~1 min)
xcodebuild -project unVBN.xcodeproj -scheme unVBN -destination 'platform=macOS' -derivedDataPath build/DerivedData test
scripts/install.sh          # Release build, installed to ~/Applications and registered
```

Without the bundled ffmpeg the app still builds; WebM then falls back to a Homebrew ffmpeg in debug builds.
`UNVBN_FFMPEG=/path/to/ffmpeg` overrides the choice.

| Target | What |
|---|---|
| `VBNCore` | The decoder: header, QuickLZ 1.5 level 3, BC3 (DXT5), upright RGBA with straight alpha. |
| `VBNExport` | `FrameSequence` (finds and orders frames) and `SequenceExporter` (PNG, AVAssetWriter H.264, ffmpeg pipe for WebM), with progress reporting. |
| `unVBN` | The app (AppKit + SwiftUI). Embeds `unvbn` and `ffmpeg` in `Contents/Helpers/`. |
| `unvbn-cli` | The `unvbn` tool (target name differs from the app's by more than case: on a case-insensitive disk they would share build folders). |
| `unVBNPreview`, `unVBNThumbnail` | Sandboxed Quick Look extensions (`QLPreviewProvider`, `QLThumbnailProvider`). |
| `VBNCoreTests` | Synthetic fixtures for every variant and error case, export tests, and the real-sample comparison. |

`com.vizrt.vbn` is declared as an **imported** type: the format belongs to Vizrt, and an imported
declaration gives way to Vizrt's own if one is ever installed. Checking by hand:

```sh
qlmanage -t -x -s 256 -o /tmp/thumbs file.vbn   # thumbnail through quicklookd (-x), as Finder does
qlmanage -p file.vbn                            # preview window
```

### Windows

`windows/` is C++17, built with CMake and Visual Studio 2022: the WIC codec and the export tools.
[vcpkg](https://vcpkg.io) supplies libvpx and libwebm (manifest `windows/vcpkg.json`, linked statically).

| Part | What |
|---|---|
| `windows/src/vbn_decode.*` | Portable port of `VBNCore`, pixel-identical to it. QuickLZ output is capped at the size the header requires, so a hostile file can't make Explorer allocate gigabytes. |
| `windows/src/wic_codec.cpp` | The COM DLL: WIC decoder (32bpp BGRA, straight alpha, magic-number pattern `1E F1 0D 90`), Explorer thumbnail handler (`IThumbnailProvider`), and `DllRegisterServer` / `DllUnregisterServer`, the single source of truth for the registry entries. |
| `windows/src/export/` | Port of `VBNExport`: sequence scanning and naming (portable), parallel exporter and batch runner (portable), BT.709 conversion (portable), and the Windows encoders in `sinks_win.cpp`: PNG through WIC, MP4 through Media Foundation, WebM VP9 + alpha through libvpx and libwebm (colour and alpha as two VP9 streams, alpha in `BlockAdditional`, `AlphaMode` 1, as browsers expect). |
| `windows/src/cli/` | `unvbn.exe`, same usage as the macOS tool. |
| `windows/src/app/` | `unvbn-export.exe`, the Win32 export window. A second launch (Explorer starts one per selected folder) hands its folders to the open window. |
| `windows/installer/unVBN.wxs` | WiX v4 per-machine MSI with a short wizard: the codec (self-registration), both programs, the PATH entry, the folder verb and `THIRD_PARTY_NOTICES.txt` (generated by CMake from vcpkg's licence files). |
| `windows/test/` | `test_decode` and `export_test` (portable parts also run on macOS), `wic_test` (decodes through WIC itself, short-read streams, Explorer thumbnails). `export_test` reads every encoder's output back independently (WIC, Media Foundation, an EBML walker plus libvpx's decoder) and compares it with the source pixels and alpha. |

CI (`.github/workflows/windows.yml`) builds x64 and ARM64, runs the test programs and a CLI smoke test,
registers the codec with `regsvr32`, then installs and uninstalls the MSI and checks everything before and
after (files, PATH, folder verb). The MSIs are the run's artifacts. By hand, on Windows (`VCPKG_ROOT` set;
`arm64-windows-static` and `-A ARM64` on ARM):

```bat
cmake -S windows -B build -A x64 -DUNVBN_VERSION=0.3.1 ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
regsvr32 build\Release\unvbn_wic.dll        &:: as administrator; regsvr32 /u to remove
```

On a Mac (portable tests only):

```sh
cd windows
clang++ -std=c++17 -Isrc src/vbn_decode.cpp test/test_decode.cpp -o /tmp/test_decode && /tmp/test_decode
clang++ -std=c++17 -Isrc src/vbn_decode.cpp src/export/{sequence,exporter,yuv,sinks_none}.cpp test/export_test.cpp \
  -o /tmp/export_test && /tmp/export_test
```

### Releasing

1. Bump `MARKETING_VERSION` in `project.yml` (the Windows build reads it too) and push; CI builds the MSIs.
2. Mac: `TEAM_ID=<TEAM_ID> NOTARY_PROFILE=<profile> scripts/release.sh` builds the Developer ID–signed,
   notarized dmg in `build/release/`. It builds in `$TMPDIR`: iCloud Drive tags files under `~/Documents`
   with attributes that `codesign --strict` and notarization reject. The notary profile is created once
   with `xcrun notarytool store-credentials <profile> --apple-id <apple-id> --team-id <TEAM_ID>`.
3. Download the MSIs from the CI run (`gh run download`), tag, and attach the dmg and both MSIs to the
   GitHub release.

Before publishing, check the binaries with `strings` for local paths (`/Users/`) and anything internal.

The app icon is drawn in code: `scripts/make-icon.sh` rebuilds `App/Assets.xcassets/AppIcon.appiconset`
and `docs/icon.png`.

## License

[MIT](LICENSE). Vizrt and Viz Artist / Viz Engine are trademarks of Vizrt; this project is not affiliated
with Vizrt. The format was documented by reading files, for interoperability.

The ffmpeg bundled in the Mac app is LGPL-2.1-or-later and libvpx is BSD-3-Clause: see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) (also inside the app, in `Contents/Resources/`).
