// Portable tests for vbn_decode (ported from Tests/VBNCoreTests). Builds on macOS, Linux and Windows.
//   test_decode                       run the synthetic tests
//   test_decode dump in.vbn out.raw   decode a file to raw BGRA (used to compare against real samples)
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "../src/vbn_decode.h"
#include "fixtures.h"

using fx::Bytes;

static int failures = 0;
#define CHECK(cond, msg)                                                         \
    do {                                                                         \
        if (!(cond)) { ++failures; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } \
    } while (0)

static bool decodeOK(const Bytes& file, vbn::Image& img) {
    std::string err;
    return vbn::decode(file.data(), file.size(), img, err);
}

static bool decodeFails(const Bytes& file) {
    vbn::Image img;
    std::string err;
    return !vbn::decode(file.data(), file.size(), img, err) && !err.empty();
}

static Bytes pixel(const vbn::Image& img, int x, int y) {
    auto p = img.bgra.begin() + (size_t(y) * img.width + x) * 4;
    return Bytes(p, p + 4);
}

static void check4x4(const vbn::Image& img) {
    CHECK(img.width == 4 && img.height == 4, "4x4 size");
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) CHECK(pixel(img, x, y) == fx::expectedBGRA4x4(x, y), "4x4 pixel");
}

static void testRawBGRA() {
    vbn::Image img;
    CHECK(decodeOK(fx::makeVBN(4, 4, 0x000, fx::bgra4x4Stored()), img), "raw decode");
    check4x4(img);
}

static void testDXT5() {
    vbn::Image img;
    CHECK(decodeOK(fx::makeVBN(8, 4, 0x003, fx::dxt8x4()), img), "dxt5 decode");
    const uint8_t alpha8[] = {255, 0, 218, 182, 145, 109, 72, 36};
    const uint8_t alpha6[] = {0, 255, 51, 102, 153, 204, 0, 255};
    const Bytes colours[] = {{0, 0, 255}, {255, 0, 0}, {85, 0, 170}, {170, 0, 85}};  // BGR of red, blue, mixes
    for (int j = 0; j < 16; ++j) {
        const int sx = j & 3, y = 3 - (j >> 2);
        Bytes c0 = colours[j % 4];
        c0.push_back(alpha8[j % 8]);
        CHECK(pixel(img, sx, y) == c0, "dxt5 block 0");
        CHECK(pixel(img, 4 + sx, y) == (Bytes{0, 255, 0, alpha6[j % 8]}), "dxt5 block 1");
    }
    // 5x3 needs 2x1 blocks; pixels outside the image are dropped.
    vbn::Image crop;
    CHECK(decodeOK(fx::makeVBN(5, 3, 0x003, fx::dxt8x4()), crop), "dxt5 partial decode");
    CHECK(crop.bgra.size() == 5 * 3 * 4, "dxt5 partial size");
    CHECK(pixel(crop, 4, 2) == (Bytes{0, 255, 0, 0}), "dxt5 partial pixel");
}

static void testQuickLZ() {
    auto stored = fx::bgra4x4Stored();
    auto qlz = fx::qlzAllLiterals(stored);
    CHECK(vbn::quicklzDecompress(qlz.data(), qlz.size(), 1 << 20) == stored, "qlz all literals");
    vbn::Image img;
    CHECK(decodeOK(fx::makeVBN(4, 4, 0x100, qlz), img), "qlz bgra decode");
    check4x4(img);

    Bytes long1000;
    for (int i = 0; i < 1000; ++i) long1000.push_back(uint8_t(i * 7 + 3));
    auto q1000 = fx::qlzAllLiterals(long1000);
    CHECK(vbn::quicklzDecompress(q1000.data(), q1000.size(), 1 << 20) == long1000, "qlz many control words");

    // "ABCD", 2-byte match (offset 4, length 18, overlapping), then 26 + 16 literals: 64 bytes.
    Bytes head = {0x41, 0x42, 0x43, 0x44}, lit1, lit2;
    for (int i = 0; i < 26; ++i) lit1.push_back(uint8_t(100 + i));
    for (int i = 0; i < 16; ++i) lit2.push_back(uint8_t(200 + i));
    const uint32_t token = (4u << 6) | ((18u - 3) << 2) | 2;
    Bytes body = fx::cat({fx::le32((1u << 4) | (1u << 31)), head, {uint8_t(token), uint8_t(token >> 8)}, lit1,
                          fx::le32(0x80000000u), lit2});
    Bytes expected = head;
    for (int i = 0; i < 18; ++i) expected.push_back(head[i % 4]);
    expected = fx::cat({expected, lit1, lit2});
    auto block = fx::qlzBlock(body, 64);
    CHECK(vbn::quicklzDecompress(block.data(), block.size(), 1 << 20) == expected, "qlz back-reference");

    // 1-byte token: "xyz", match offset 3 length 3, then 14 literals.
    Bytes lits;
    for (int i = 0; i < 14; ++i) lits.push_back(uint8_t(0x30 + i));
    auto b2 = fx::qlzBlock(fx::cat({fx::le32((1u << 3) | (1u << 31)), {0x78, 0x79, 0x7A}, {uint8_t(3 << 2)}, lits}), 20);
    CHECK(vbn::quicklzDecompress(b2.data(), b2.size(), 1 << 20) == fx::cat({{0x78, 0x79, 0x7A, 0x78, 0x79, 0x7A}, lits}),
          "qlz 1-byte token");

    vbn::Image a, b;
    CHECK(decodeOK(fx::makeVBN(8, 4, 0x103, fx::qlzAllLiterals(fx::dxt8x4())), a), "qlz dxt5 decode");
    CHECK(decodeOK(fx::makeVBN(8, 4, 0x003, fx::dxt8x4()), b), "dxt5 decode");
    CHECK(a.bgra == b.bgra, "qlz dxt5 == raw dxt5");

    // Stored block.
    vbn::Image s;
    CHECK(decodeOK(fx::makeVBN(4, 4, 0x100, fx::cat({{0x4E}, fx::le32(9 + 64), fx::le32(64), stored})), s), "stored");
    check4x4(s);
}

static void testErrors() {
    auto raw = fx::bgra4x4Stored();
    CHECK(decodeFails(fx::makeVBN(4, 4, 0, raw, 0x12345678)), "bad magic");
    CHECK(decodeFails({0x89, 0x50, 0x4E, 0x47}), "png bytes");
    auto full = fx::makeVBN(4, 4, 0, raw);
    for (size_t n : {size_t(0), size_t(3), size_t(100), size_t(0xC0), size_t(0xC0 + 10)})
        CHECK(decodeFails(Bytes(full.begin(), full.begin() + n)), "truncated file");
    CHECK(decodeFails(fx::makeVBN(4, 4, 0, Bytes(raw.begin(), raw.begin() + 60))), "payload too small");
    auto q = fx::qlzAllLiterals(raw);
    CHECK(decodeFails(fx::makeVBN(4, 4, 0x100, Bytes(q.begin(), q.begin() + 40))), "qlz cut short");
    for (uint32_t f : {0x001u, 0x005u, 0x203u, 0x10003u}) CHECK(decodeFails(fx::makeVBN(4, 4, f, raw)), "unknown format");
    auto corrupt = fx::qlzBlock(fx::cat({fx::le32(1u | (1u << 31)), {uint8_t(8 << 2)}, Bytes(40, 0)}), 30);
    CHECK(decodeFails(fx::makeVBN(4, 4, 0x100, corrupt)), "corrupt back-reference");
    // Hostile: a tiny file declaring a 1 GiB QuickLZ output must be refused before allocating it.
    auto bomb = fx::qlzBlock(fx::le32(0x80000000u), 0x40000000u);
    CHECK(decodeFails(fx::makeVBN(4, 4, 0x100, bomb)), "qlz size bomb");
    CHECK(decodeFails(fx::makeVBN(20000, 4, 0, raw)), "oversized dimensions");
}

static int dump(const char* in, const char* out) {
    std::ifstream f(in, std::ios::binary);
    Bytes data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    vbn::Image img;
    std::string err;
    if (!vbn::decode(data.data(), data.size(), img, err)) { std::fprintf(stderr, "%s: %s\n", in, err.c_str()); return 1; }
    std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(img.bgra.data()), std::streamsize(img.bgra.size()));
    std::printf("%u %u\n", img.width, img.height);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "dump") return dump(argv[2], argv[3]);
    testRawBGRA();
    testDXT5();
    testQuickLZ();
    testErrors();
    std::printf(failures ? "%d failure(s)\n" : "all decoder tests passed\n", failures);
    return failures ? 1 : 0;
}
