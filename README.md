<p align="center"><img src="docs/icon.png" width="160" alt="unVBN icon"></p>

# unVBN

Opens Vizrt Viz Artist / Viz Engine `.vbn` images on macOS and Windows, and converts `.vbn` animations
to PNG, MP4 or WebM.

**[Download the latest release](https://github.com/notjeremie/unVBN/releases/latest)**

| | macOS | Windows |
|---|---|---|
| Thumbnails | Finder | Explorer |
| Preview | Space bar (Quick Look) | Double-click (Windows Photo Viewer) |
| Convert animations | Drop the folder on the app | Right-click the folder > *Export with unVBN…* |

## Install

**macOS 14 or later**: open the `.dmg`, drag **unVBN** to Applications, launch it once, then quit it.

**Windows 10 or 11**: run `unVBN-windows-<version>-x64.msi` (`-arm64.msi` on ARM PCs). The installer is
not code-signed, so SmartScreen asks first: *More info > Run anyway*. If thumbnails don't show up, restart
Explorer from Task Manager.

## Converting animations

An animation is a folder of numbered frames (`NAME_00000.vbn`, `NAME_00001.vbn`, …). Pick the format and
frame rate (50 fps by default), then export:

- **WebM**: keeps transparency, plays in Chrome and Firefox.
- **MP4**: no transparency; transparent areas go on a background colour (black by default).
- **PNG**: one image per frame, with transparency.

Several folders can be exported at once, with the same settings. On Windows 11, *Export with unVBN…* is
under *Show more options*.

There is also a command line, `unvbn` (on Windows it is installed on the PATH; on the Mac it is in
`unVBN.app/Contents/Helpers/`):

```sh
unvbn LOGO_FOLDER -f webm               # -> LOGO.webm next to the folder
unvbn LOGO_FOLDER -f mp4 --fps 25
unvbn --help
```

## Good to know

- Exported videos have no sound: `.vbn` animations don't contain any.
- The Windows *Photos* app can't open `.vbn`; double-click uses Windows Photo Viewer instead.
- MP4 on Windows is limited to 8192 pixels per side, and to 4096×2304 in area on Windows 10 (8192×4320 on
  Windows 11). Export bigger animations, such as a video wall, as WebM or PNG.

## License

[MIT](LICENSE). Vizrt and Viz Artist / Viz Engine are trademarks of Vizrt; this project is not affiliated
with Vizrt. The file format is described in [`FORMAT.md`](FORMAT.md); third-party licences are in
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). Building and releasing: [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md).
