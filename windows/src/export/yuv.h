// BGRA -> Y'CbCr 4:2:0, BT.709, limited (studio) range, for the video encoders. Portable and tested on
// macOS. Chroma is computed from the 2x2 average of R'G'B' (edges clamped for odd sizes).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vbnx {

struct PlaneOut {
    uint8_t* data;
    ptrdiff_t stride;
};

/// One pixel: R'G'B' -> limited-range BT.709 Y'CbCr.
void rgbToYCbCr709(int r, int g, int b, uint8_t& y, uint8_t& cb, uint8_t& cr);

/// Straight-alpha BGRA (tightly packed, `width` x `height`) -> planar 4:2:0. The chroma planes are
/// ((width + 1) / 2) x ((height + 1) / 2). Alpha is ignored (the colour is kept as stored).
void bgraToI420(const uint8_t* bgra, uint32_t width, uint32_t height, PlaneOut y, PlaneOut u, PlaneOut v);

/// The alpha channel as a full-resolution plane (VP9 alpha streams code it as luma, unscaled).
void bgraAlphaPlane(const uint8_t* bgra, uint32_t width, uint32_t height, PlaneOut a);

/// Composites straight-alpha BGRA over `bg` into an opaque `outWidth` x `outHeight` BGRA image; the area
/// beyond the source (padding) is plain background.
std::vector<uint8_t> flattenOnto(const uint8_t* bgra, uint32_t width, uint32_t height, uint8_t bgR, uint8_t bgG, uint8_t bgB,
                                 uint32_t outWidth, uint32_t outHeight);

/// BGRA (even `width` and `height`) -> NV12: a Y plane then interleaved CbCr at half resolution.
void bgraToNV12(const uint8_t* bgra, uint32_t width, uint32_t height, PlaneOut y, PlaneOut uv);

}  // namespace vbnx
