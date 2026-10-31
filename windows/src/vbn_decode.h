// Portable Vizrt .vbn decoder (see FORMAT.md). Plain C++17, no platform headers: it is compiled into
// the Windows WIC codec and, for testing, natively on macOS/Linux against the same fixtures.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vbn {

constexpr uint32_t kMagic = 0x900DF11E;
constexpr size_t kHeaderSize = 0xC0;
/// Far above any Viz texture; keeps a corrupt or hostile header from asking for gigabytes.
constexpr uint32_t kMaxDimension = 16384;

struct Header {
    uint32_t width = 0, height = 0;
    bool dxt5 = false;        // else RGBA8888 (byte order R, G, B, A)
    bool compressed = false;  // QuickLZ 1.5 level 3
    uint32_t payloadSize = 0;

    /// Bytes of pixel data once decompressed.
    size_t pixelDataSize() const;
};

/// A decoded image: BGRA8, straight (non-premultiplied) alpha, rows top-down.
struct Image {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> bgra;
};

/// Parses and validates the 192-byte header. On failure returns false and sets `error`.
bool parseHeader(const uint8_t* data, size_t size, Header& header, std::string& error);

/// Decodes a whole .vbn file. On failure returns false and sets `error`. Never throws.
bool decode(const uint8_t* data, size_t size, Image& image, std::string& error);

/// QuickLZ 1.5 level-3 block. Fails if the block declares more than `maxOutput` bytes. Throws std::runtime_error.
std::vector<uint8_t> quicklzDecompress(const uint8_t* in, size_t size, size_t maxOutput);

}  // namespace vbn
