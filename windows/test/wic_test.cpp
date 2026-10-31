// Windows integration test: decodes .vbn files through WIC itself (as Explorer and photo viewers do),
// so it only passes when the codec is registered (regsvr32 or the MSI).
//   wic_test                    codec must be registered and decode correctly
//   wic_test --expect-missing   codec must NOT be registered (after uninstall)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <propsys.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "../src/guids.h"
#include "../src/vbn_decode.h"
#include "fixtures.h"

using fx::Bytes;

static int failures = 0;
#define CHECK(cond, msg)                                                                       \
    do {                                                                                       \
        if (!(cond)) { ++failures; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } \
        else std::printf("ok   %s\n", msg);                                                    \
    } while (0)

template <class T> using Com = Microsoft::WRL::ComPtr<T>;

static std::wstring writeTemp(const wchar_t* name, const Bytes& data) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    std::wstring path = std::wstring(dir) + name;
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    return path;
}

/// An IStream that hands out at most `chunk` bytes per Read, like the Explorer thumbnail host's streams.
class ShortReadStream final : public IStream {
public:
    ShortReadStream(Bytes data, ULONG chunk) : data_(std::move(data)), chunk_(chunk) {}
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (iid == IID_IUnknown || iid == IID_ISequentialStream || iid == IID_IStream) {
            *out = static_cast<IStream*>(this); AddRef(); return S_OK;
        }
        *out = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override { const ULONG n = --refs_; if (!n) delete this; return n; }
    STDMETHODIMP Read(void* buffer, ULONG size, ULONG* read) override {
        const ULONG n = ULONG(std::min<size_t>({size_t(size), size_t(chunk_), data_.size() - pos_}));
        std::memcpy(buffer, data_.data() + pos_, n);
        pos_ += n;
        if (read) *read = n;
        return n < size ? S_FALSE : S_OK;
    }
    STDMETHODIMP Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* newPos) override {
        const LONGLONG base = origin == STREAM_SEEK_SET ? 0 : origin == STREAM_SEEK_CUR ? LONGLONG(pos_) : LONGLONG(data_.size());
        const LONGLONG target = base + move.QuadPart;
        if (target < 0) return STG_E_INVALIDFUNCTION;
        pos_ = size_t(std::min<LONGLONG>(target, LONGLONG(data_.size())));
        if (newPos) newPos->QuadPart = pos_;
        return S_OK;
    }
    STDMETHODIMP Stat(STATSTG* stat, DWORD) override {
        *stat = {};
        stat->type = STGTY_STREAM;
        stat->cbSize.QuadPart = data_.size();
        return S_OK;
    }
    STDMETHODIMP Write(const void*, ULONG, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP SetSize(ULARGE_INTEGER) override { return E_NOTIMPL; }
    STDMETHODIMP CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    STDMETHODIMP Commit(DWORD) override { return S_OK; }
    STDMETHODIMP Revert() override { return E_NOTIMPL; }
    STDMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP Clone(IStream**) override { return E_NOTIMPL; }

private:
    ULONG refs_ = 1;
    Bytes data_;
    ULONG chunk_;
    size_t pos_ = 0;
};

/// A 256x256 raw RGBA frame: 256 KB, far over one 64 KB read, with a gradient and varying alpha.
static Bytes bigFrame() {
    Bytes px;
    px.reserve(256 * 256 * 4);
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x) px.insert(px.end(), {uint8_t(x), uint8_t(y), uint8_t(x ^ y), uint8_t(255 - y / 2)});
    return fx::makeVBN(256, 256, 0x000, px);
}

static void checkDecoder(IWICImagingFactory* factory, IWICBitmapDecoder* decoder, const Bytes& file);

/// Decodes `path` through WIC and compares every pixel with the portable decoder.
static void checkThroughWIC(IWICImagingFactory* factory, const std::wstring& path, const Bytes& file, const char* label) {
    std::printf("-- %s\n", label);
    Com<IWICBitmapDecoder> decoder;
    HRESULT hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                    &decoder);
    CHECK(SUCCEEDED(hr), "WIC finds a decoder");
    if (FAILED(hr)) { std::printf("     hr=0x%08lX\n", static_cast<unsigned long>(hr)); return; }
    checkDecoder(factory, decoder.Get(), file);
}

/// Same, from a stream that returns at most `chunk` bytes per Read.
static void checkThroughShortReads(IWICImagingFactory* factory, const Bytes& file, ULONG chunk, const char* label) {
    std::printf("-- %s\n", label);
    ShortReadStream* stream = new ShortReadStream(file, chunk);
    Com<IWICBitmapDecoder> decoder;
    const HRESULT hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    stream->Release();
    CHECK(SUCCEEDED(hr), "WIC finds a decoder for the stream");
    if (FAILED(hr)) { std::printf("     hr=0x%08lX\n", static_cast<unsigned long>(hr)); return; }
    checkDecoder(factory, decoder.Get(), file);
}

/// The Explorer thumbnail path end to end: the shell resolving .vbn to unVBN's thumbnail handler.
static void checkThumbnail(const std::wstring& path, const char* label) {
    Com<IShellItemImageFactory> shellImage;
    HRESULT hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&shellImage));
    if (SUCCEEDED(hr)) {
        HBITMAP bitmap = nullptr;
        hr = shellImage->GetImage(SIZE{96, 96}, SIIGBF_THUMBNAILONLY, &bitmap);
        if (bitmap) DeleteObject(bitmap);
    }
    std::printf("     thumbnail hr=0x%08lX\n", static_cast<unsigned long>(hr));
    CHECK(SUCCEEDED(hr), label);
}

/// unVBN's own thumbnail handler, called directly: 256x256 frame -> 64x64 premultiplied BGRA bitmap.
static void checkThumbnailHandler(const Bytes& big) {
    std::printf("-- unVBN thumbnail handler\n");
    Com<IInitializeWithStream> init;
    HRESULT hr = CoCreateInstance(CLSID_UnVBNThumbnail, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&init));
    CHECK(SUCCEEDED(hr), "thumbnail handler registered");
    if (FAILED(hr)) return;
    IStream* stream = SHCreateMemStream(big.data(), UINT(big.size()));
    CHECK(SUCCEEDED(init->Initialize(stream, STGM_READ)), "Initialize(stream)");
    stream->Release();
    Com<IThumbnailProvider> provider;
    init->QueryInterface(IID_PPV_ARGS(&provider));
    HBITMAP bitmap = nullptr;
    WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
    CHECK(provider && SUCCEEDED(provider->GetThumbnail(64, &bitmap, &alpha)) && bitmap, "GetThumbnail(64)");
    if (!bitmap) return;
    DIBSECTION dib{};
    GetObjectW(bitmap, sizeof dib, &dib);
    CHECK(dib.dsBm.bmWidth == 64 && std::abs(dib.dsBm.bmHeight) == 64 && dib.dsBm.bmBitsPixel == 32, "64x64, 32 bpp");
    CHECK(alpha == WTSAT_ARGB, "has alpha");
    // Centre of bigFrame(): R = x ~ 128, G = y ~ 128, A = 255 - y/2 ~ 191, so premultiplied R, G ~ 96.
    // (B = x ^ y averages unpredictably under scaling, so it is not checked.)
    const BYTE* p = static_cast<const BYTE*>(dib.dsBm.bmBits) + 32 * dib.dsBm.bmWidthBytes + 32 * 4;
    std::printf("     centre BGRA = %u %u %u %u\n", p[0], p[1], p[2], p[3]);
    CHECK(std::abs(int(p[1]) - 96) < 14 && std::abs(int(p[2]) - 96) < 14 && std::abs(int(p[3]) - 191) < 14,
          "thumbnail pixels match the image (premultiplied)");
    DeleteObject(bitmap);
}

static void checkDecoder(IWICImagingFactory* factory, IWICBitmapDecoder* decoder, const Bytes& file) {
    GUID container{};
    CHECK(SUCCEEDED(decoder->GetContainerFormat(&container)) && container == GUID_ContainerFormatVBN, "container is .vbn");
    UINT frames = 0;
    CHECK(SUCCEEDED(decoder->GetFrameCount(&frames)) && frames == 1, "one frame");

    Com<IWICBitmapFrameDecode> frame;
    CHECK(SUCCEEDED(decoder->GetFrame(0, &frame)), "GetFrame(0)");
    if (!frame) return;
    vbn::Image expected;
    std::string err;
    vbn::decode(file.data(), file.size(), expected, err);
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    CHECK(w == expected.width && h == expected.height, "size matches");
    WICPixelFormatGUID fmt{};
    frame->GetPixelFormat(&fmt);
    CHECK(fmt == GUID_WICPixelFormat32bppBGRA, "32bpp BGRA");

    Bytes pixels(size_t(w) * h * 4);
    CHECK(SUCCEEDED(frame->CopyPixels(nullptr, w * 4, UINT(pixels.size()), pixels.data())) && pixels == expected.bgra,
          "pixels identical to the portable decoder");

    if (w >= 3 && h >= 3) {  // sub-rectangle with a padded stride
        WICRect r{1, 1, 2, 2};
        Bytes part(16 * 2, 0xEE);
        CHECK(SUCCEEDED(frame->CopyPixels(&r, 16, UINT(part.size()), part.data())), "CopyPixels sub-rectangle");
        bool same = true;
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 8; ++x)
                same &= part[y * 16 + x] == expected.bgra[(size_t(1 + y) * w + 1) * 4 + x];
        CHECK(same, "sub-rectangle pixels");
    }
    WICRect bad{0, 0, INT(w) + 1, 1};
    CHECK(FAILED(frame->CopyPixels(&bad, (w + 1) * 4, (w + 1) * 4, pixels.data())), "out-of-bounds rect rejected");

    // What Explorer does next: convert to premultiplied BGRA for display.
    Com<IWICFormatConverter> converter;
    CHECK(SUCCEEDED(factory->CreateFormatConverter(&converter)) && SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                          WICBitmapPaletteTypeCustom)),
          "converts to 32bpp PBGRA");
}

int wmain(int argc, wchar_t** argv) {
    const bool expectMissing = argc > 1 && std::wstring(argv[1]) == L"--expect-missing";
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    int result = 0;
    {
        Com<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
            return 2;

        const Bytes raw = fx::makeVBN(4, 4, 0x000, fx::rgba4x4Stored());
        const std::wstring rawPath = writeTemp(L"unvbn_wic_raw.vbn", raw);

        if (expectMissing) {
            Com<IWICBitmapDecoder> decoder;
            const HRESULT hr = factory->CreateDecoderFromFilename(rawPath.c_str(), nullptr, GENERIC_READ,
                                                                  WICDecodeMetadataCacheOnDemand, &decoder);
            CHECK(hr == WINCODEC_ERR_COMPONENTNOTFOUND, "codec is no longer registered");
            Com<IWICComponentInfo> info;
            CHECK(FAILED(factory->CreateComponentInfo(CLSID_UnVBNDecoder, &info)), "component info gone");
            Com<IInitializeWithStream> thumb;
            CHECK(FAILED(CoCreateInstance(CLSID_UnVBNThumbnail, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&thumb))),
                  "thumbnail handler gone");
        } else {
            Com<IWICComponentInfo> info;
            CHECK(SUCCEEDED(factory->CreateComponentInfo(CLSID_UnVBNDecoder, &info)), "codec registered with WIC");

            checkThroughWIC(factory.Get(), rawPath, raw, "raw RGBA 4x4");
            const Bytes dxt = fx::makeVBN(8, 4, 0x003, fx::dxt8x4());
            checkThroughWIC(factory.Get(), writeTemp(L"unvbn_wic_dxt5.vbn", dxt), dxt, "DXT5 8x4");
            const Bytes qlz = fx::makeVBN(8, 4, 0x103, fx::qlzAllLiterals(fx::dxt8x4()));
            checkThroughWIC(factory.Get(), writeTemp(L"unvbn_wic_qlz.vbn", qlz), qlz, "QuickLZ + DXT5 8x4");
            // Found by its magic number even without the .vbn extension.
            checkThroughWIC(factory.Get(), writeTemp(L"unvbn_wic_noext.bin", raw), raw, "content sniffing (.bin)");
            // Bigger than one 64 KB read, from a file and from a stream returning 1000 bytes per Read.
            const Bytes big = bigFrame();
            const std::wstring bigPath = writeTemp(L"unvbn_wic_big.vbn", big);
            checkThroughWIC(factory.Get(), bigPath, big, "256x256 raw RGBA (256 KB)");
            checkThroughShortReads(factory.Get(), big, 1000, "256 KB through 1000-byte reads");
            checkThroughShortReads(factory.Get(), dxt, 7, "DXT5 through 7-byte reads (header split)");
            checkThumbnailHandler(big);
            wchar_t handler[64] = {};
            DWORD size = sizeof handler;
            RegGetValueW(HKEY_CLASSES_ROOT, L".vbn\\ShellEx\\{e357fccd-a995-4576-b01f-234630154e96}", nullptr, RRF_RT_REG_SZ,
                         nullptr, handler, &size);
            CHECK(_wcsicmp(handler, UNVBN_THUMBNAIL_CLSID_STR) == 0, ".vbn thumbnails use unVBN's handler");
            std::printf("-- Explorer thumbnails\n");
            checkThumbnail(rawPath, "thumbnail of a small file");
            checkThumbnail(bigPath, "thumbnail of a 256 KB file");

            Com<IWICBitmapDecoder> decoder;
            const HRESULT hr = factory->CreateDecoderFromFilename(
                writeTemp(L"unvbn_wic_bad.vbn", fx::makeVBN(4, 4, 0x100, fx::qlzBlock(fx::le32(0x80000000u), 0x40000000u))).c_str(),
                nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
            CHECK(FAILED(hr), "corrupt / hostile file refused");

        }
        result = failures ? 1 : 0;
        std::printf(failures ? "%d failure(s)\n" : "all WIC checks passed\n", failures);
    }
    CoUninitialize();
    return result;
}
