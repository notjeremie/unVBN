// Windows encoder tests: every output is read back independently of the code that wrote it (PNG through
// WIC, MP4 through Media Foundation's decoder, WebM through a small EBML walker written from the Matroska
// spec plus libvpx's decoder) and checked against the source pixels.
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdlib>
#include <map>

#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

#include "../src/export/yuv.h"
#include "export_test_util.h"

using Microsoft::WRL::ComPtr;
using namespace et;
using vbnx::Error;
using vbnx::Format;

namespace {

bool within(int a, int b, int tolerance) { return std::abs(a - b) <= tolerance; }

vbnx::Options optionsFor(Format f) {
    vbnx::Options o;
    o.format = f;
    return o;
}

/// Folder names with accents and Hebrew: every encoder must handle non-ASCII paths.
const char* kUnicodeName = "s\xC3\xA9quence \xD7\xA2\xD7\x91\xD7\xA8\xD7\x99\xD7\xAA";

// MARK: PNG

struct Decoded {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> bgra;
};

Decoded readPNG(const fs::path& p) {
    Decoded d;
    ComPtr<IWICImagingFactory> f;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) ||
        FAILED(f->CreateDecoderFromFilename(p.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(f->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)) ||
        FAILED(conv->GetSize(&d.width, &d.height)))
        return {};
    d.bgra.resize(size_t(d.width) * d.height * 4);
    if (FAILED(conv->CopyPixels(nullptr, d.width * 4, UINT(d.bgra.size()), d.bgra.data()))) return {};
    return d;
}

void testPNG(const fs::path& root) {
    const auto seq = makeSequence(root / vbnx::fromUtf8(kUnicodeName), "LOGO", 12);
    const fs::path out = root / vbnx::fromUtf8(std::string(kUnicodeName) + " png");
    std::vector<int> progress;
    vbnx::exportSequence(seq, optionsFor(Format::png), out, [&](const vbnx::Progress& p) { progress.push_back(p.done); });
    CHECK(!progress.empty() && progress.back() == 12, "png progress");
    for (int i = 0; i < 12; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "LOGO_%05d.png", i);
        const Decoded d = readPNG(out / name);
        CHECK(d.width == 7 && d.height == 5, "png size");
        if (d.bgra.size() < 4) continue;
        // Straight alpha kept as is: B, G, R, A.
        CHECK(d.bgra[0] == 50 && d.bgra[1] == 100 && d.bgra[2] == uint8_t(i * 20 % 256) && d.bgra[3] == 128, "png pixel");
    }
}

// MARK: MP4

struct VideoInfo {
    int frames = 0;
    uint32_t width = 0, height = 0;
    double seconds = 0;
    /// Y, Cb, Cr of pixel (0, 0) of the first and the last frame.
    std::array<int, 3> first{}, last{};
};

VideoInfo readMP4(const fs::path& p) {
    VideoInfo info;
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(p.c_str(), nullptr, &reader))) return info;
    ComPtr<IMFMediaType> type;
    MFCreateMediaType(&type);
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (FAILED(reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type.Get()))) return info;
    ComPtr<IMFMediaType> current;
    reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &current);
    MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &info.width, &info.height);
    PROPVARIANT duration;
    PropVariantInit(&duration);
    if (SUCCEEDED(reader->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration)))
        info.seconds = double(duration.uhVal.QuadPart) / 1e7;
    PropVariantClear(&duration);
    for (;;) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, nullptr, &sample))) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!sample) continue;
        ComPtr<IMFMediaBuffer> buf;
        sample->ConvertToContiguousBuffer(&buf);
        BYTE* data = nullptr;
        DWORD len = 0;
        if (SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
            // Decoders may pad rows; the chroma plane starts after `len * 2 / 3` bytes either way.
            std::array<int, 3> yuv{data[0], data[len * 2 / 3], data[len * 2 / 3 + 1]};
            if (info.frames == 0) info.first = yuv;
            info.last = yuv;
            buf->Unlock();
        }
        ++info.frames;
    }
    return info;
}

std::array<int, 3> expectedYuv(int r, int g, int b) {
    uint8_t y, cb, cr;
    vbnx::rgbToYCbCr709(r, g, b, y, cb, cr);
    return {y, cb, cr};
}

bool nearYuv(const std::array<int, 3>& a, const std::array<int, 3>& b) {
    return within(a[0], b[0], 3) && within(a[1], b[1], 3) && within(a[2], b[2], 3);
}

void testMP4(const fs::path& root) {
    // Half-transparent red over white -> (255, 128, 128); the last frame is opaque blue.
    const auto seq = makeSequence(root / vbnx::fromUtf8(kUnicodeName), "MP4", 25, 64, 36,
                                  [](int i) { return i == 24 ? RGBA{0, 0, 255, 255} : RGBA{255, 0, 0, 128}; });
    auto options = optionsFor(Format::mp4);
    options.background = {255, 255, 255};
    const fs::path out = root / vbnx::fromUtf8(std::string(kUnicodeName) + ".mp4");
    vbnx::exportSequence(seq, options, out);
    const VideoInfo v = readMP4(out);
    CHECK(v.frames == 25 && v.width == 64 && v.height == 36, "mp4 frames and size");
    CHECK(within(int(v.seconds * 1000), 500, 25), "mp4 duration at 50 fps");
    CHECK(nearYuv(v.first, expectedYuv(255, 128, 128)), "mp4 flattened onto the background (BT.709)");
    CHECK(nearYuv(v.last, expectedYuv(0, 0, 255)), "mp4 last frame");
    if (!nearYuv(v.first, expectedYuv(255, 128, 128)))
        std::printf("  got Y %d Cb %d Cr %d\n", v.first[0], v.first[1], v.first[2]);

    // Odd sizes are padded to even; tiny ones to the encoder's minimum of 34.
    const auto odd = makeSequence(root, "ODD", 3, 65, 37);
    vbnx::exportSequence(odd, optionsFor(Format::mp4), root / "odd.mp4");
    const VideoInfo o = readMP4(root / "odd.mp4");
    CHECK(o.frames == 3 && o.width == 66 && o.height == 38, "mp4 odd size padded");
    const auto tiny = makeSequence(root, "TINY", 3, 7, 5);
    vbnx::exportSequence(tiny, optionsFor(Format::mp4), root / "tiny.mp4");
    const VideoInfo t = readMP4(root / "tiny.mp4");
    CHECK(t.frames == 3 && t.width == 34 && t.height == 34, "mp4 tiny size padded");

    // 29.97 fps keeps its exact duration.
    auto ntsc = optionsFor(Format::mp4);
    ntsc.fps = 29.97;
    vbnx::exportSequence(seq, ntsc, root / "ntsc.mp4");
    const VideoInfo n = readMP4(root / "ntsc.mp4");
    CHECK(n.frames == 25 && within(int(n.seconds * 1000), 834, 25), "mp4 at 29.97 fps");
}

void testCancelRemovesPartialOutput(const fs::path& root) {
    const auto seq = makeSequence(root, "CANCEL", 30, 16, 16);
    for (Format f : {Format::mp4, Format::webm}) {
        const fs::path out = root / (std::string("cancel.") + vbnx::formatExtension(f));
        int done = 0;
        expectError(Error::Kind::cancelled, [&] {
            vbnx::exportSequence(seq, optionsFor(f), out, [&](const vbnx::Progress& p) { done = p.done; }, [&] { return done >= 3; });
        }, "cancel");
        CHECK(!fs::exists(out), "partial video removed");
    }
}

// MARK: WebM

/// The parts of a WebM file the tests look at, read straight from the EBML structure.
struct WebmFile {
    bool ok = false;
    std::string codec;
    uint64_t width = 0, height = 0, alphaMode = 0, matrix = 0;
    struct Block {
        bool key = false;
        std::vector<uint8_t> color, alpha;
    };
    std::vector<Block> blocks;
};

class Ebml {
public:
    explicit Ebml(std::vector<uint8_t> data) : d_(std::move(data)) {}

    WebmFile parse() {
        walk(0, d_.size());
        file_.ok = !failed_;
        return file_;
    }

private:
    bool vint(size_t& pos, size_t end, uint64_t& value, bool keepMarker) {
        if (pos >= end) return false;
        const uint8_t first = d_[pos];
        int len = 1;
        while (len <= 8 && !(first & (0x80 >> (len - 1)))) ++len;
        if (len > 8 || pos + len > end) return false;
        value = keepMarker ? first : first & (0xFF >> len);
        for (int i = 1; i < len; ++i) value = value << 8 | d_[pos + i];
        pos += len;
        return true;
    }

    uint64_t uint(size_t pos, size_t size) const {
        uint64_t v = 0;
        for (size_t i = 0; i < size; ++i) v = v << 8 | d_[pos + i];
        return v;
    }

    void walk(size_t pos, size_t end) {
        while (pos < end && !failed_) {
            uint64_t id = 0, size = 0;
            if (!vint(pos, end, id, true) || !vint(pos, end, size, false) || pos + size > end) { failed_ = true; return; }
            switch (id) {
            case 0x18538067: case 0x1F43B675: case 0x1654AE6B: case 0xAE: case 0xE0: case 0x55B0:  // masters
                walk(pos, pos + size);
                break;
            case 0xA0:  // BlockGroup
                file_.blocks.emplace_back();
                walk(pos, pos + size);
                break;
            case 0x75A1: case 0xA6:  // BlockAdditions, BlockMore
                walk(pos, pos + size);
                break;
            case 0xA1: {  // Block: track vint, int16 timecode, flags, frame
                size_t p = pos;
                uint64_t track = 0;
                if (file_.blocks.empty() || !vint(p, pos + size, track, false) || p + 3 > pos + size) { failed_ = true; return; }
                file_.blocks.back().color.assign(d_.begin() + ptrdiff_t(p + 3), d_.begin() + ptrdiff_t(pos + size));
                break;
            }
            case 0xA3: failed_ = true; return;  // SimpleBlock: cannot carry alpha
            case 0xA5:  // BlockAdditional
                if (!file_.blocks.empty())
                    file_.blocks.back().alpha.assign(d_.begin() + ptrdiff_t(pos), d_.begin() + ptrdiff_t(pos + size));
                break;
            case 0x86: file_.codec.assign(d_.begin() + ptrdiff_t(pos), d_.begin() + ptrdiff_t(pos + size)); break;
            case 0xB0: file_.width = uint(pos, size_t(size)); break;
            case 0xBA: file_.height = uint(pos, size_t(size)); break;
            case 0x53C0: file_.alphaMode = uint(pos, size_t(size)); break;
            case 0x55B1: file_.matrix = uint(pos, size_t(size)); break;
            default: break;
            }
            pos += size_t(size);
        }
    }

    std::vector<uint8_t> d_;
    WebmFile file_;
    bool failed_ = false;
};

/// Decodes one VP9 stream; returns plane 0 value at (0, 0) per frame, plus Cb/Cr for the colour stream.
struct Vp9Decoder {
    vpx_codec_ctx_t ctx{};
    bool ok;
    Vp9Decoder() { ok = vpx_codec_dec_init(&ctx, vpx_codec_vp9_dx(), nullptr, 0) == VPX_CODEC_OK; }
    ~Vp9Decoder() { if (ok) vpx_codec_destroy(&ctx); }
    bool decode(const std::vector<uint8_t>& data, std::array<int, 3>& yuv, vpx_color_space_t* cs = nullptr) {
        if (!ok || vpx_codec_decode(&ctx, data.data(), unsigned(data.size()), nullptr, 0)) return false;
        vpx_codec_iter_t it = nullptr;
        vpx_image_t* img = vpx_codec_get_frame(&ctx, &it);
        if (!img) return false;
        yuv = {img->planes[0][0], img->planes[1][0], img->planes[2][0]};
        if (cs) *cs = img->cs;
        return true;
    }
};

std::vector<uint8_t> readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

constexpr int kWebmFrames = 150;

void testWebM(const fs::path& root) {
    // 150 frames (keyframes at 0 and 128), alpha rising 0..255 over the run, green over a red last frame.
    auto color = [](int i) { return i == kWebmFrames - 1 ? RGBA{255, 0, 0, 255} : RGBA{0, 255, 0, uint8_t(i * 255 / (kWebmFrames - 2))}; };
    const auto seq = makeSequence(root / vbnx::fromUtf8(kUnicodeName), "WEBM", kWebmFrames, 64, 36, color);
    const fs::path out = root / vbnx::fromUtf8(std::string(kUnicodeName) + ".webm");
    vbnx::exportSequence(seq, optionsFor(Format::webm), out);

    const WebmFile w = Ebml(readAll(out)).parse();
    CHECK(w.ok, "webm parses");
    CHECK(w.codec == "V_VP9" && w.width == 64 && w.height == 36, "webm track");
    CHECK(w.alphaMode == 1, "webm AlphaMode 1");
    CHECK(w.matrix == 1, "webm Colour BT.709");
    CHECK(int(w.blocks.size()) == kWebmFrames, "webm frame count");
    Vp9Decoder colorDec, alphaDec;
    int bad = 0;
    for (int i = 0; i < int(w.blocks.size()); ++i) {
        const auto& b = w.blocks[size_t(i)];
        std::array<int, 3> c{}, a{};
        vpx_color_space_t cs = VPX_CS_UNKNOWN;
        if (b.alpha.empty() || !colorDec.decode(b.color, c, &cs) || !alphaDec.decode(b.alpha, a)) { ++bad; continue; }
        const RGBA src = color(i);
        if (!nearYuv(c, expectedYuv(src[0], src[1], src[2])) || !within(a[0], src[3], 3) || cs != VPX_CS_BT_709) {
            if (bad++ < 3)
                std::printf("  frame %d: Y %d Cb %d Cr %d (cs %d) alpha %d, expected alpha %d\n", i, c[0], c[1], c[2], int(cs), a[0],
                            src[3]);
        }
    }
    CHECK(bad == 0, "webm colour and alpha decode to the source");
}

}  // namespace

void runWindowsEncoderTests() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    TempDir dir;
    try {
        testPNG(dir.path);
        testMP4(dir.path);
        testWebM(dir.path);
        testCancelRemovesPartialOutput(dir.path);
    } catch (const std::exception& e) {
        std::printf("FAIL unexpected exception: %s\n", e.what());
        ++failures;
    }
}
