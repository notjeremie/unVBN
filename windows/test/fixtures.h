// Synthetic .vbn fixtures, ported from Tests/VBNCoreTests/Fixtures.swift (same bytes, same expectations).
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace fx {

using Bytes = std::vector<uint8_t>;

inline Bytes le32(uint32_t v) { return {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; }

inline Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

/// 192-byte header + payload.
inline Bytes makeVBN(uint32_t w, uint32_t h, uint32_t format, const Bytes& payload, uint32_t magic = 0x900DF11E) {
    Bytes hd(0xC0, 0);
    auto put = [&](uint32_t v, size_t at) { for (int i = 0; i < 4; ++i) hd[at + i] = uint8_t(v >> (8 * i)); };
    put(magic, 0x00); put(3, 0x04); put(4, 0x08); put(w, 0x0C); put(h, 0x10); put(4, 0x14);
    put(format, 0x18); put(5, 0x1C); put(uint32_t(payload.size()), 0x24); put(uint32_t(payload.size()), 0x40);
    return cat({hd, payload});
}

/// QuickLZ L3 block with the 9-byte header (flags 0x4F).
inline Bytes qlzBlock(const Bytes& body, uint32_t dsize) {
    return cat({{0x4F}, le32(uint32_t(9 + body.size())), le32(dsize), body});
}

/// All-literal QuickLZ L3 stream: control word 0x80000000 then 31 bytes, repeated.
inline Bytes qlzAllLiterals(const Bytes& data) {
    Bytes body;
    for (size_t i = 0; i < data.size(); i += 31) {
        auto cw = le32(0x80000000u);
        body.insert(body.end(), cw.begin(), cw.end());
        body.insert(body.end(), data.begin() + i, data.begin() + std::min(i + 31, data.size()));
    }
    return qlzBlock(body, uint32_t(data.size()));
}

/// 4x4 pixels in STORED (bottom-up) order, byte order R, G, B, A. Stored row r, column x:
/// R = 0x10*(x+1)+r, G = r, B = x, A = 0x80+4r+x.
inline Bytes rgba4x4Stored() {
    Bytes out;
    for (int r = 0; r < 4; ++r)
        for (int x = 0; x < 4; ++x)
            out.insert(out.end(), {uint8_t(0x10 * (x + 1) + r), uint8_t(r), uint8_t(x), uint8_t(0x80 + 4 * r + x)});
    return out;
}

/// Expected upright BGRA at (x, y) for rgba4x4Stored(): upright row y = stored row 3 - y.
inline Bytes expectedBGRA4x4(int x, int y) {
    const int r = 3 - y;
    return {uint8_t(x), uint8_t(r), uint8_t(0x10 * (x + 1) + r), uint8_t(0x80 + 4 * r + x)};
}

/// 8x4 = two BC3 blocks. Block 0: a0=255 > a1=0, alpha index j%8; c0 red 0xF800, c1 blue 0x001F, colour index j%4.
/// Block 1: a0=0 <= a1=255 (6-value + 0/255), alpha index j%8; colour index 0, c0 green 0x07E0.
inline Bytes dxt8x4() {
    uint64_t abits = 0;
    for (int j = 0; j < 16; ++j) abits |= uint64_t(j % 8) << (3 * j);
    Bytes alpha;
    for (int i = 0; i < 6; ++i) alpha.push_back(uint8_t(abits >> (8 * i)));
    uint32_t cbits = 0;
    for (int j = 0; j < 16; ++j) cbits |= uint32_t(j % 4) << (2 * j);
    Bytes b0 = cat({{255, 0}, alpha, {0x00, 0xF8, 0x1F, 0x00}, le32(cbits)});
    Bytes b1 = cat({{0, 255}, alpha, {0xE0, 0x07, 0xFF, 0xFF}, le32(0)});
    return cat({b0, b1});
}

}  // namespace fx
