// Port of Sources/VBNCore (Swift) and reference/ (Python). Same integer rounding, so output is
// pixel-identical; the only difference is BGRA output order (what WIC wants) instead of RGBA.
#include "vbn_decode.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <stdexcept>

namespace vbn {
namespace {

uint32_t readU32(const uint8_t* b) {
    return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
}

[[noreturn]] void fail(const std::string& why) { throw std::runtime_error(why); }

struct Rgb { int r, g, b; };

Rgb c565(int c) { return {((c >> 11) & 31) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31}; }

/// BC3 blocks -> BGRA, rows in stored (bottom-up) order.
void decodeBC3(const uint8_t* data, uint32_t w, uint32_t h, uint8_t* out) {
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    int al[8];
    Rgb cl[4];
    const uint8_t* p = data;
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx, p += 16) {
            const int a0 = p[0], a1 = p[1];
            uint64_t abits = 0;
            for (int i = 0; i < 6; ++i) abits |= uint64_t(p[2 + i]) << (8 * i);
            al[0] = a0; al[1] = a1;
            if (a0 > a1) {
                for (int i = 0; i < 6; ++i) al[2 + i] = ((6 - i) * a0 + (i + 1) * a1) / 7;
            } else {
                for (int i = 0; i < 4; ++i) al[2 + i] = ((4 - i) * a0 + (i + 1) * a1) / 5;
                al[6] = 0; al[7] = 255;
            }
            const Rgb r0 = c565(p[8] | p[9] << 8), r1 = c565(p[10] | p[11] << 8);
            const uint32_t bits = readU32(p + 12);
            cl[0] = r0; cl[1] = r1;
            cl[2] = {(2 * r0.r + r1.r) / 3, (2 * r0.g + r1.g) / 3, (2 * r0.b + r1.b) / 3};
            cl[3] = {(r0.r + 2 * r1.r) / 3, (r0.g + 2 * r1.g) / 3, (r0.b + 2 * r1.b) / 3};
            for (int j = 0; j < 16; ++j) {
                const uint32_t x = bx * 4 + (j & 3), y = by * 4 + (j >> 2);
                if (x >= w || y >= h) continue;
                uint8_t* o = out + (size_t(y) * w + x) * 4;
                const Rgb& c = cl[(bits >> (2 * j)) & 3];
                o[0] = uint8_t(c.b); o[1] = uint8_t(c.g); o[2] = uint8_t(c.r);
                o[3] = uint8_t(al[(abits >> (3 * j)) & 7]);
            }
        }
    }
}

}  // namespace

size_t Header::pixelDataSize() const {
    if (dxt5) return size_t((width + 3) / 4) * ((height + 3) / 4) * 16;
    return size_t(width) * height * 4;
}

std::vector<uint8_t> quicklzDecompress(const uint8_t* in, size_t n, size_t maxOutput) {
    if (n < 3) fail("truncated QuickLZ header");
    const uint8_t flags = in[0];
    size_t csize, dsize, p;
    if (flags & 2) {
        if (n < 9) fail("truncated QuickLZ header");
        csize = readU32(in + 1); dsize = readU32(in + 5); p = 9;
    } else {
        csize = in[1]; dsize = in[2]; p = 3;
    }
    if (dsize > maxOutput) fail("QuickLZ block larger than the image it should hold");
    if (!(flags & 1)) {  // stored
        if (n < p + dsize) fail("truncated QuickLZ stored block");
        return std::vector<uint8_t>(in + p, in + p + dsize);
    }
    if (csize < p || n < csize) fail("truncated QuickLZ block");
    if (dsize == 0) return {};

    // The reference pads 8 zero bytes so 4-byte fetches near the end stay in range.
    std::vector<uint8_t> src(in, in + csize);
    src.resize(csize + 8, 0);
    std::vector<uint8_t> dst;
    dst.reserve(dsize);
    const size_t last = dsize - 1;
    uint32_t cword = 1;
    auto fetch32 = [&](size_t at) {
        if (at + 4 > src.size()) fail("QuickLZ stream ends early");
        return readU32(src.data() + at);
    };

    for (;;) {
        if (cword == 1) { cword = fetch32(p); p += 4; }
        const uint32_t fetch = fetch32(p);
        if (cword & 1) {
            cword >>= 1;
            size_t off, ml;
            if ((fetch & 3) == 0)        { off = (fetch & 0xff) >> 2; ml = 3; p += 1; }
            else if ((fetch & 2) == 0)   { off = (fetch & 0xffff) >> 2; ml = 3; p += 2; }
            else if ((fetch & 1) == 0)   { off = (fetch & 0xffff) >> 6; ml = ((fetch >> 2) & 15) + 3; p += 2; }
            else if ((fetch & 127) != 3) { off = (fetch >> 7) & 0x1ffff; ml = ((fetch >> 2) & 0x1f) + 2; p += 3; }
            else                         { off = fetch >> 15; ml = ((fetch >> 7) & 255) + 3; p += 4; }
            if (off < 1 || off > dst.size()) fail("QuickLZ back-reference before start of output");
            if (dst.size() + ml > dsize) fail("QuickLZ match runs past declared size");
            const size_t s = dst.size() - off;
            for (size_t i = 0; i < ml; ++i) dst.push_back(dst[s + i]);  // byte by byte: overlap is intended
        } else if (dst.size() + 10 < last) {
            if (p >= src.size()) fail("QuickLZ stream ends early");
            dst.push_back(src[p++]); cword >>= 1;
        } else {
            // Tail rule: the last bytes are always literals; control words are skipped.
            while (dst.size() <= last) {
                if (cword == 1) { p += 4; cword = 1u << 31; }
                if (p >= src.size()) fail("QuickLZ stream ends early");
                dst.push_back(src[p++]); cword >>= 1;
            }
            return dst;
        }
    }
}

bool parseHeader(const uint8_t* b, size_t size, Header& h, std::string& error) {
    if (size < 4) { error = "file too short"; return false; }
    if (readU32(b) != kMagic) { error = "not a Vizrt .vbn file"; return false; }
    if (size < kHeaderSize) { error = "truncated header"; return false; }
    if (readU32(b + 0x04) != 3) { error = "unsupported .vbn version"; return false; }
    const uint32_t w = readU32(b + 0x0C), hh = readU32(b + 0x10), channels = readU32(b + 0x14);
    if (w == 0 || hh == 0 || w > kMaxDimension || hh > kMaxDimension || channels != 4) {
        error = "invalid dimensions"; return false;
    }
    const uint32_t format = readU32(b + 0x18);
    if ((format & ~0x1FFu) != 0 || ((format & 0xFF) != 0 && (format & 0xFF) != 3)) {
        error = "unknown pixel format"; return false;
    }
    h.width = w; h.height = hh;
    h.dxt5 = (format & 0xFF) == 3;
    h.compressed = (format & 0x100) != 0;
    h.payloadSize = readU32(b + 0x24);
    return true;
}

bool decode(const uint8_t* data, size_t size, Image& image, std::string& error) {
    try {
        Header h;
        if (!parseHeader(data, size, h, error)) return false;
        if (size - kHeaderSize < h.payloadSize) { error = "truncated payload"; return false; }
        const uint8_t* payload = data + kHeaderSize;
        const size_t need = h.pixelDataSize();

        std::vector<uint8_t> unpacked;
        const uint8_t* pixels = payload;
        size_t available = h.payloadSize;
        if (h.compressed) {
            unpacked = quicklzDecompress(payload, h.payloadSize, need);
            pixels = unpacked.data();
            available = unpacked.size();
        }
        if (available < need) { error = "truncated pixel data"; return false; }

        const uint32_t w = h.width, hh = h.height;
        const size_t row = size_t(w) * 4;
        std::vector<uint8_t> stored(row * hh);
        if (h.dxt5) {
            decodeBC3(pixels, w, hh, stored.data());
        } else {  // stored R, G, B, A -> B, G, R, A
            for (size_t i = 0; i < stored.size(); i += 4) {
                stored[i] = pixels[i + 2]; stored[i + 1] = pixels[i + 1];
                stored[i + 2] = pixels[i]; stored[i + 3] = pixels[i + 3];
            }
        }

        // Rows are stored bottom-up (OpenGL): flip.
        image.width = w; image.height = hh;
        image.bgra.resize(stored.size());
        for (uint32_t y = 0; y < hh; ++y)
            std::memcpy(image.bgra.data() + y * row, stored.data() + (hh - 1 - y) * row, row);
        return true;
    } catch (const std::bad_alloc&) {
        error = "out of memory"; return false;
    } catch (const std::exception& e) {
        error = e.what(); return false;
    }
}

}  // namespace vbn
