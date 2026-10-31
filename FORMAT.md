# Vizrt `.vbn` image format (reverse-engineered 2026-10-31)

Vizrt Viz Artist / Viz Engine image files (one file per image; animations = one file per frame,
e.g. `NAME_00000.vbn` … `NAME_00749.vbn`). Typical uses: station branding, on-air graphics, logo bugs. All values little-endian.

## Header (192 bytes, 0x00–0xBF)

| Offset | Type | Value seen | Meaning |
|---|---|---|---|
| 0x00 | u32 | `0x900DF11E` | magic (bytes `1E F1 0D 90`) |
| 0x04 | u32 | 3 | version |
| 0x08 | u32 | 4 | ? (always 4 in the sample) |
| 0x0C | u32 | e.g. 436 | width |
| 0x10 | u32 | e.g. 180 | height |
| 0x14 | u32 | 4 | channels (BGRA / RGBA after decode) |
| 0x18 | u32 | 0x000 / 0x003 / 0x100 / 0x103 | **pixel format**: low byte 0 = BGRA8888, 3 = DXT5 (BC3); bit 0x100 = payload QuickLZ-compressed |
| 0x1C | u32 | 5 | ? |
| 0x20 | u32 | 0 | ? |
| 0x24 | u32 | n | payload size in bytes (also repeated at 0x40) |
| … | | 0 | padding up to 0xC0 |

## Payload (from 0xC0)

- **Compressed (bit 0x100)**: one **QuickLZ 1.5 level 3** block with the long (9-byte) header:
  `flags u8 (0x4F = compressed | 9-byte header | level 3 | always-set bit 6)`, `compressed size u32`,
  `decompressed size u32`, then 32-bit control words. Flags bit0 = 0 → stored (copy as is).
- **Uncompressed**: the pixel data directly.

## Pixels

- **Format 0 (BGRA8888)**: width × height × 4, byte order B, G, R, A.
- **Format 3 (DXT5 / BC3)**: ceil(w/4) × ceil(h/4) blocks of 16 bytes (8 bytes alpha + 8 bytes 565 colour).
- **Rows are stored bottom-up** (OpenGL convention): flip vertically for a normal image.

## No audio

A `.vbn` holds one image and nothing else: in every file checked, the size is exactly the 192-byte header
plus the payload, and the header has no audio fields. A sequence is only a folder of numbered frames,
with no container or index that could carry sound. A survey of a production image library
(~100,000 `.vbn` files) found no audio files at all, so sound for an animation, if any, lives elsewhere
in the playout system (the scene, a clip or the audio chain), not with the frames.

## Sample survey (1,500 random files on the share)

60 % `0x003` raw DXT5 · 27 % `0x103` QuickLZ+DXT5 · 9 % `0x100` QuickLZ+BGRA · 2 % `0x000` raw BGRA ·
2 % empty/truncated files. No other variant seen.

## Verified

a 436×180 stripe logo (QLZ+BGRA), a 132×132 logo bug (DXT5), another 132×132 logo bug (QLZ+DXT5)
and an 884×144 background (QLZ+DXT5) — all
decode to correct, upright images with alpha.

Reference decoder: `reference/vbn.py` (QuickLZ L3 + header) and `reference/dxt.py` (BC3).
