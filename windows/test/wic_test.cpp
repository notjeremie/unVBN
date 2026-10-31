// Windows integration test: decodes .vbn files through WIC itself (as Explorer and photo viewers do),
// so it only passes when the codec is registered (regsvr32 or the MSI).
//   wic_test                    codec must be registered and decode correctly
//   wic_test --expect-missing   codec must NOT be registered (after uninstall)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <cstdio>
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

template <class T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

static std::wstring writeTemp(const wchar_t* name, const Bytes& data) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    std::wstring path = std::wstring(dir) + name;
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    return path;
}

/// Decodes `path` through WIC and compares every pixel with the portable decoder.
static void checkThroughWIC(IWICImagingFactory* factory, const std::wstring& path, const Bytes& file, const char* label) {
    std::printf("-- %s\n", label);
    Com<IWICBitmapDecoder> decoder;
    HRESULT hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                    &decoder);
    CHECK(SUCCEEDED(hr), "WIC finds a decoder");
    if (FAILED(hr)) { std::printf("     hr=0x%08lX\n", static_cast<unsigned long>(hr)); return; }

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
    factory->CreateFormatConverter(&converter);
    CHECK(SUCCEEDED(converter->Initialize(frame.p, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
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

        const Bytes raw = fx::makeVBN(4, 4, 0x000, fx::bgra4x4Stored());
        const std::wstring rawPath = writeTemp(L"unvbn_wic_raw.vbn", raw);

        if (expectMissing) {
            Com<IWICBitmapDecoder> decoder;
            const HRESULT hr = factory->CreateDecoderFromFilename(rawPath.c_str(), nullptr, GENERIC_READ,
                                                                  WICDecodeMetadataCacheOnDemand, &decoder);
            CHECK(hr == WINCODEC_ERR_COMPONENTNOTFOUND, "codec is no longer registered");
            Com<IWICComponentInfo> info;
            CHECK(FAILED(factory->CreateComponentInfo(CLSID_UnVBNDecoder, &info)), "component info gone");
        } else {
            Com<IWICComponentInfo> info;
            CHECK(SUCCEEDED(factory->CreateComponentInfo(CLSID_UnVBNDecoder, &info)), "codec registered with WIC");

            checkThroughWIC(factory.p, rawPath, raw, "raw BGRA 4x4");
            const Bytes dxt = fx::makeVBN(8, 4, 0x003, fx::dxt8x4());
            checkThroughWIC(factory.p, writeTemp(L"unvbn_wic_dxt5.vbn", dxt), dxt, "DXT5 8x4");
            const Bytes qlz = fx::makeVBN(8, 4, 0x103, fx::qlzAllLiterals(fx::dxt8x4()));
            checkThroughWIC(factory.p, writeTemp(L"unvbn_wic_qlz.vbn", qlz), qlz, "QuickLZ + DXT5 8x4");
            // Found by its magic number even without the .vbn extension.
            checkThroughWIC(factory.p, writeTemp(L"unvbn_wic_noext.bin", raw), raw, "content sniffing (.bin)");

            Com<IWICBitmapDecoder> decoder;
            const HRESULT hr = factory->CreateDecoderFromFilename(
                writeTemp(L"unvbn_wic_bad.vbn", fx::makeVBN(4, 4, 0x100, fx::qlzBlock(fx::le32(0x80000000u), 0x40000000u))).c_str(),
                nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
            CHECK(FAILED(hr), "corrupt / hostile file refused");

            // Informational: the Explorer thumbnail path (can be unavailable on headless CI machines).
            Com<IShellItemImageFactory> shellImage;
            if (SUCCEEDED(SHCreateItemFromParsingName(rawPath.c_str(), nullptr, IID_PPV_ARGS(&shellImage)))) {
                HBITMAP bitmap = nullptr;
                const HRESULT th = shellImage->GetImage(SIZE{64, 64}, SIIGBF_THUMBNAILONLY, &bitmap);
                std::printf("info Explorer thumbnail: %s (hr=0x%08lX)\n", SUCCEEDED(th) ? "produced" : "not produced",
                            static_cast<unsigned long>(th));
                if (bitmap) DeleteObject(bitmap);
            }
        }
        result = failures ? 1 : 0;
        std::printf(failures ? "%d failure(s)\n" : "all WIC checks passed\n", failures);
    }
    CoUninitialize();
    return result;
}
