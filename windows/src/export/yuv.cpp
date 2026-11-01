#include "yuv.h"

#include <algorithm>
#include <thread>

namespace vbnx {

// BT.709 (Kr 0.2126, Kb 0.0722), limited range (Y' 16-235, Cb/Cr 16-240), in 16.16 fixed point.
// Each chroma row sums to zero, so greys map to exactly 128.
void rgbToYCbCr709(int r, int g, int b, uint8_t& y, uint8_t& cb, uint8_t& cr) {
    constexpr int round = 1 << 15;
    y = uint8_t((11966 * r + 40254 * g + 4064 * b + (16 << 16) + round) >> 16);
    cb = uint8_t((-6596 * r - 22189 * g + 28785 * b + (128 << 16) + round) >> 16);
    cr = uint8_t((28785 * r - 26145 * g - 2640 * b + (128 << 16) + round) >> 16);
}

namespace {

/// Runs `fn(first, end)` over rows [0, rows) in bands, one per core (at least 64 rows each). Bands start on
/// even rows, so each 4:2:0 chroma row belongs to one band. Frames are converted on the encoder's
/// critical path, and a 16384x2160 frame is 141 MB: one core alone is the bottleneck.
template <typename Fn>
void parallelRows(uint32_t rows, const Fn& fn) {
    const uint32_t cores = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    const uint32_t band = std::max(64u, ((rows + cores - 1) / cores + 1) & ~1u);
    std::vector<std::thread> threads;
    for (uint32_t first = band; first < rows; first += band) threads.emplace_back(fn, first, std::min(rows, first + band));
    fn(0u, std::min(rows, band));
    for (auto& t : threads) t.join();
}

/// Writes luma for every pixel and chroma for every 2x2 block (clamped at odd edges) through `put`.
template <typename PutChroma>
void convert420(const uint8_t* bgra, uint32_t width, uint32_t height, PlaneOut y, const PutChroma& putChroma) {
    parallelRows(height, [&](uint32_t first, uint32_t end) {
    uint8_t cb, cr;
    for (uint32_t row = first; row < end; ++row) {
        const uint8_t* s = bgra + size_t(row) * width * 4;
        uint8_t* d = y.data + ptrdiff_t(row) * y.stride;
        for (uint32_t x = 0; x < width; ++x, s += 4) rgbToYCbCr709(s[2], s[1], s[0], d[x], cb, cr);
    }
    for (uint32_t cy = first / 2; cy < (end + 1) / 2; ++cy) {
        const uint32_t r0 = cy * 2, r1 = std::min(r0 + 1, height - 1);
        for (uint32_t cx = 0; cx < (width + 1) / 2; ++cx) {
            const uint32_t c0 = cx * 2, c1 = std::min(c0 + 1, width - 1);
            const uint8_t* p[4] = {bgra + (size_t(r0) * width + c0) * 4, bgra + (size_t(r0) * width + c1) * 4,
                                   bgra + (size_t(r1) * width + c0) * 4, bgra + (size_t(r1) * width + c1) * 4};
            const int b = (p[0][0] + p[1][0] + p[2][0] + p[3][0] + 2) / 4;
            const int g = (p[0][1] + p[1][1] + p[2][1] + p[3][1] + 2) / 4;
            const int r = (p[0][2] + p[1][2] + p[2][2] + p[3][2] + 2) / 4;
            uint8_t luma;
            rgbToYCbCr709(r, g, b, luma, cb, cr);
            putChroma(cx, cy, cb, cr);
        }
    }
    });
}

}  // namespace

void bgraToI420(const uint8_t* bgra, uint32_t width, uint32_t height, PlaneOut y, PlaneOut u, PlaneOut v) {
    convert420(bgra, width, height, y, [&](uint32_t cx, uint32_t cy, uint8_t cb, uint8_t cr) {
        u.data[ptrdiff_t(cy) * u.stride + cx] = cb;
        v.data[ptrdiff_t(cy) * v.stride + cx] = cr;
    });
}

void bgraToNV12(const uint8_t* bgra, uint32_t width, uint32_t height, PlaneOut y, PlaneOut uv) {
    convert420(bgra, width, height, y, [&](uint32_t cx, uint32_t cy, uint8_t cb, uint8_t cr) {
        uint8_t* d = uv.data + ptrdiff_t(cy) * uv.stride + cx * 2;
        d[0] = cb;
        d[1] = cr;
    });
}

void bgraAlphaPlane(const uint8_t* bgra, uint32_t width, uint32_t height, PlaneOut a) {
    parallelRows(height, [&](uint32_t first, uint32_t end) {
        for (uint32_t row = first; row < end; ++row) {
            const uint8_t* s = bgra + size_t(row) * width * 4 + 3;
            uint8_t* d = a.data + ptrdiff_t(row) * a.stride;
            for (uint32_t x = 0; x < width; ++x, s += 4) d[x] = *s;
        }
    });
}

void flattenOnto(const uint8_t* bgra, uint32_t width, uint32_t height, uint8_t bgR, uint8_t bgG, uint8_t bgB,
                 uint32_t outWidth, uint32_t outHeight, std::vector<uint8_t>& out) {
    out.resize(size_t(outWidth) * outHeight * 4);
    const uint8_t bg[3] = {bgB, bgG, bgR};
    parallelRows(outHeight, [&](uint32_t first, uint32_t end) {
    for (uint32_t row = first; row < end; ++row) {
        uint8_t* d = out.data() + size_t(row) * outWidth * 4;
        for (uint32_t x = 0; x < outWidth; ++x, d += 4) {
            if (row < height && x < width) {
                const uint8_t* s = bgra + (size_t(row) * width + x) * 4;
                const int a = s[3], ia = 255 - a;
                for (int c = 0; c < 3; ++c) d[c] = uint8_t((s[c] * a + bg[c] * ia + 127) / 255);
            } else {
                d[0] = bg[0]; d[1] = bg[1]; d[2] = bg[2];
            }
            d[3] = 255;
        }
    }
    });
}

}  // namespace vbnx
