// Helpers shared by the export tests (export_test.cpp, portable; export_test_win.cpp, Windows encoders).
#pragma once

#include <array>
#include <cstdio>
#include <fstream>
#include <functional>
#include <random>
#include <string>

#include "../src/export/exporter.h"
#include "fixtures.h"

namespace et {

namespace fs = std::filesystem;
using fx::Bytes;

inline int failures = 0;

#define CHECK(cond, msg)                                                                          \
    do {                                                                                          \
        if (!(cond)) { ++et::failures; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } \
    } while (0)

/// Runs `body`, which must throw a vbnx::Error of `kind`; returns its message ("" if it did not).
inline std::string expectError(vbnx::Error::Kind kind, const std::function<void()>& body, const char* what) {
    try {
        body();
    } catch (const vbnx::Error& e) {
        CHECK(e.kind == kind, what);
        return e.what();
    } catch (const std::exception& e) {
        std::printf("  unexpected exception: %s\n", e.what());
        CHECK(false, what);
        return e.what();
    }
    CHECK(false, what);
    return "";
}

using RGBA = std::array<uint8_t, 4>;

/// Uncompressed .vbn (bytes R, G, B, A) filled with one straight-alpha colour.
inline Bytes solidVBN(uint32_t w, uint32_t h, RGBA c) {
    Bytes payload;
    payload.reserve(size_t(w) * h * 4);
    for (size_t i = 0; i < size_t(w) * h; ++i) payload.insert(payload.end(), c.begin(), c.end());
    return fx::makeVBN(w, h, 0, payload);
}

inline void writeFile(const fs::path& p, const Bytes& data) {
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}

/// A fresh temporary folder, removed by the destructor.
struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("unvbn-export-test-" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

/// Writes `count` frames NAME_00000.vbn... into `dir/seq-NAME/` and returns that sequence.
inline vbnx::Sequence makeSequence(const fs::path& dir, const std::string& name, int count, uint32_t w = 7, uint32_t h = 5,
                                   std::function<RGBA(int)> color = nullptr) {
    if (!color) color = [](int i) { return RGBA{uint8_t(i * 20 % 256), 100, 50, 128}; };
    const fs::path seqDir = dir / vbnx::fromUtf8("seq-" + name);
    fs::create_directories(seqDir);
    for (int i = 0; i < count; ++i) {
        char file[64];
        std::snprintf(file, sizeof file, "_%05d.vbn", i);
        writeFile(seqDir / vbnx::fromUtf8(name + file), solidVBN(w, h, color(i)));
    }
    return vbnx::findSequences(seqDir).at(0);
}

}  // namespace et
