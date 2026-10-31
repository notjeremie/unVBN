// unVBN WIC codec: lets Windows (Explorer thumbnails and preview, photo viewers, any WIC-based app)
// open Vizrt .vbn images directly. Decode only: one frame, 32bpp BGRA with straight alpha.
//
// Registration (DllRegisterServer / DllUnregisterServer, also run by the MSI) is the single source of
// truth for the registry entries; see register* below.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <propsys.h>
#include <shlobj.h>
#include <thumbcache.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "guids.h"
#include "version.h"
#include "vbn_decode.h"

namespace {

HMODULE g_module = nullptr;
std::atomic<long> g_objects{0};
std::atomic<long> g_locks{0};

/// Header plus the largest legal uncompressed RGBA payload (16384 x 16384 x 4).
constexpr ULONGLONG kMaxFileSize = vbn::kHeaderSize + ULONGLONG(vbn::kMaxDimension) * vbn::kMaxDimension * 4;

using Microsoft::WRL::ComPtr;

/// Shared COM lifetime for every object in this DLL: reference count, the DLL-wide object count used by
/// DllCanUnloadNow, and deletion on the last Release. Subclasses only implement QueryInterface.
template <class... Interfaces>
class ComObject : public Interfaces... {
public:
    ComObject() { ++g_objects; }
    virtual ~ComObject() { --g_objects; }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

private:
    std::atomic<ULONG> refs_{1};
};

// MARK: - Frame

class Frame final : public ComObject<IWICBitmapFrameDecode> {
public:
    explicit Frame(std::shared_ptr<const vbn::Image> image) : image_(std::move(image)) {}

    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IWICBitmapSource || iid == IID_IWICBitmapFrameDecode) {
            *out = static_cast<IWICBitmapFrameDecode*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP GetSize(UINT* width, UINT* height) override {
        if (!width || !height) return E_INVALIDARG;
        *width = image_->width;
        *height = image_->height;
        return S_OK;
    }
    STDMETHODIMP GetPixelFormat(WICPixelFormatGUID* format) override {
        if (!format) return E_INVALIDARG;
        *format = GUID_WICPixelFormat32bppBGRA;  // straight (non-premultiplied) alpha, as stored in .vbn
        return S_OK;
    }
    STDMETHODIMP GetResolution(double* dpiX, double* dpiY) override {
        if (!dpiX || !dpiY) return E_INVALIDARG;
        *dpiX = *dpiY = 96.0;
        return S_OK;
    }
    STDMETHODIMP CopyPalette(IWICPalette*) override { return WINCODEC_ERR_PALETTEUNAVAILABLE; }

    STDMETHODIMP CopyPixels(const WICRect* rect, UINT stride, UINT bufferSize, BYTE* buffer) override {
        if (!buffer) return E_INVALIDARG;
        const INT w = INT(image_->width), h = INT(image_->height);
        const WICRect r = rect ? *rect : WICRect{0, 0, w, h};
        if (r.X < 0 || r.Y < 0 || r.Width < 0 || r.Height < 0 || r.X > w - r.Width || r.Y > h - r.Height)
            return E_INVALIDARG;
        if (r.Width == 0 || r.Height == 0) return S_OK;
        const UINT rowBytes = UINT(r.Width) * 4;
        if (stride < rowBytes) return E_INVALIDARG;
        if (ULONGLONG(stride) * UINT(r.Height - 1) + rowBytes > bufferSize) return WINCODEC_ERR_INSUFFICIENTBUFFER;
        const size_t srcStride = size_t(w) * 4;
        for (INT y = 0; y < r.Height; ++y)
            std::memcpy(buffer + size_t(y) * stride, image_->bgra.data() + size_t(r.Y + y) * srcStride + size_t(r.X) * 4,
                        rowBytes);
        return S_OK;
    }

    STDMETHODIMP GetMetadataQueryReader(IWICMetadataQueryReader** reader) override {
        if (reader) *reader = nullptr;
        return WINCODEC_ERR_UNSUPPORTEDOPERATION;
    }
    STDMETHODIMP GetColorContexts(UINT, IWICColorContext**, UINT* actual) override {
        if (!actual) return E_INVALIDARG;
        *actual = 0;
        return S_OK;
    }
    STDMETHODIMP GetThumbnail(IWICBitmapSource** thumbnail) override {
        if (thumbnail) *thumbnail = nullptr;
        return WINCODEC_ERR_CODECNOTHUMBNAIL;  // callers scale the frame
    }

private:
    std::shared_ptr<const vbn::Image> image_;
};

// MARK: - Decoder

/// Reads up to `size` bytes, looping over short reads; `got` is what was read before the end.
HRESULT readUpTo(IStream* stream, BYTE* buffer, ULONG size, ULONG& got) {
    got = 0;
    while (got < size) {
        ULONG n = 0;
        const HRESULT hr = stream->Read(buffer + got, size - got, &n);
        if (FAILED(hr)) return hr;
        if (n == 0) break;
        got += n;
    }
    return S_OK;
}

/// Reads the rest of `stream` (from its current position) straight into `out`, sized from Stat when the
/// stream reports one. Streams may return fewer bytes than asked before the end (the Explorer thumbnail
/// host does), so all reads go through readUpTo. Fails above kMaxFileSize.
HRESULT readAll(IStream* stream, std::vector<uint8_t>& out) {
    STATSTG stat{};
    size_t capacity = 64 * 1024;
    if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart <= kMaxFileSize)
        capacity = std::max(capacity, size_t(stat.cbSize.QuadPart));
    out.resize(capacity);
    size_t used = 0;
    for (;;) {
        if (used == out.size()) {  // full: grow only if the stream really has more
            BYTE next = 0;
            ULONG n = 0;
            const HRESULT hr = readUpTo(stream, &next, 1, n);
            if (FAILED(hr)) return hr;
            if (n == 0) break;
            if (out.size() >= kMaxFileSize) return WINCODEC_ERR_BADIMAGE;
            out.resize(size_t(std::min<ULONGLONG>(ULONGLONG(out.size()) * 2, kMaxFileSize)));
            out[used++] = next;
        }
        const ULONG want = ULONG(std::min<size_t>(out.size() - used, size_t(1) << 30));
        ULONG got = 0;
        const HRESULT hr = readUpTo(stream, out.data() + used, want, got);
        if (FAILED(hr)) return hr;
        used += got;
        if (got < want) break;  // readUpTo stops short only at the end of the stream
    }
    out.resize(used);
    return S_OK;
}

class Decoder final : public ComObject<IWICBitmapDecoder> {
public:
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IWICBitmapDecoder) {
            *out = static_cast<IWICBitmapDecoder*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    /// Looks at the header only and leaves the stream where it was.
    STDMETHODIMP QueryCapability(IStream* stream, DWORD* capability) override {
        if (!stream || !capability) return E_INVALIDARG;
        *capability = 0;
        LARGE_INTEGER zero{};
        ULARGE_INTEGER start{};
        HRESULT hr = stream->Seek(zero, STREAM_SEEK_CUR, &start);
        if (FAILED(hr)) return hr;
        uint8_t header[vbn::kHeaderSize];
        ULONG got = 0;
        hr = readUpTo(stream, header, sizeof header, got);
        LARGE_INTEGER back;
        back.QuadPart = LONGLONG(start.QuadPart);
        stream->Seek(back, STREAM_SEEK_SET, nullptr);
        if (FAILED(hr)) return hr;
        vbn::Header h;
        std::string error;
        if (vbn::parseHeader(header, got, h, error))
            *capability = WICBitmapDecoderCapabilityCanDecodeAllImages | WICBitmapDecoderCapabilityCanDecodeSomeImages;
        return S_OK;
    }

    STDMETHODIMP Initialize(IStream* stream, WICDecodeOptions) override {
        if (!stream) return E_INVALIDARG;
        AcquireSRWLockExclusive(&lock_);
        HRESULT hr = S_OK;
        try {
            std::vector<uint8_t> data;
            hr = readAll(stream, data);
            if (SUCCEEDED(hr)) {
                auto image = std::make_shared<vbn::Image>();
                std::string error;
                if (vbn::decode(data.data(), data.size(), *image, error)) image_ = std::move(image);
                else hr = WINCODEC_ERR_BADIMAGE;
            }
        } catch (const std::bad_alloc&) {
            hr = E_OUTOFMEMORY;
        }
        ReleaseSRWLockExclusive(&lock_);
        return hr;
    }

    STDMETHODIMP GetContainerFormat(GUID* format) override {
        if (!format) return E_INVALIDARG;
        *format = GUID_ContainerFormatVBN;
        return S_OK;
    }

    STDMETHODIMP GetDecoderInfo(IWICBitmapDecoderInfo** info) override {
        if (!info) return E_INVALIDARG;
        *info = nullptr;
        ComPtr<IWICImagingFactory> factory;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        ComPtr<IWICComponentInfo> component;
        if (SUCCEEDED(hr)) hr = factory->CreateComponentInfo(CLSID_UnVBNDecoder, &component);
        if (SUCCEEDED(hr)) hr = component->QueryInterface(IID_PPV_ARGS(info));
        return hr;
    }

    STDMETHODIMP CopyPalette(IWICPalette*) override { return WINCODEC_ERR_PALETTEUNAVAILABLE; }
    STDMETHODIMP GetMetadataQueryReader(IWICMetadataQueryReader** reader) override {
        if (reader) *reader = nullptr;
        return WINCODEC_ERR_UNSUPPORTEDOPERATION;
    }
    STDMETHODIMP GetPreview(IWICBitmapSource** preview) override {
        if (preview) *preview = nullptr;
        return WINCODEC_ERR_UNSUPPORTEDOPERATION;
    }
    STDMETHODIMP GetColorContexts(UINT, IWICColorContext**, UINT* actual) override {
        if (!actual) return E_INVALIDARG;
        *actual = 0;
        return S_OK;
    }
    STDMETHODIMP GetThumbnail(IWICBitmapSource** thumbnail) override {
        if (thumbnail) *thumbnail = nullptr;
        return WINCODEC_ERR_CODECNOTHUMBNAIL;
    }

    STDMETHODIMP GetFrameCount(UINT* count) override {
        if (!count) return E_INVALIDARG;
        AcquireSRWLockShared(&lock_);
        const bool ready = image_ != nullptr;
        ReleaseSRWLockShared(&lock_);
        if (!ready) return WINCODEC_ERR_NOTINITIALIZED;
        *count = 1;
        return S_OK;
    }

    STDMETHODIMP GetFrame(UINT index, IWICBitmapFrameDecode** frame) override {
        if (!frame) return E_INVALIDARG;
        *frame = nullptr;
        AcquireSRWLockShared(&lock_);
        std::shared_ptr<const vbn::Image> image = image_;
        ReleaseSRWLockShared(&lock_);
        if (!image) return WINCODEC_ERR_NOTINITIALIZED;
        if (index != 0) return WINCODEC_ERR_FRAMEMISSING;
        *frame = new (std::nothrow) Frame(std::move(image));
        return *frame ? S_OK : E_OUTOFMEMORY;
    }

private:
    SRWLOCK lock_ = SRWLOCK_INIT;
    std::shared_ptr<const vbn::Image> image_;
};

// MARK: - Explorer thumbnail handler

/// Scales a decoded image to fit inside cx x cx (never upscaling) into a top-down premultiplied DIB.
/// The image is read through a Frame, so the converter and scaler pull rows from it without a full copy.
HRESULT makeThumbnail(std::shared_ptr<const vbn::Image> image, UINT cx, HBITMAP* out) {
    const double scale = std::min(1.0, double(cx) / double(std::max(image->width, image->height)));
    const UINT w = std::max(1u, UINT(image->width * scale + 0.5)), h = std::max(1u, UINT(image->height * scale + 0.5));

    ComPtr<IWICBitmapSource> source;
    source.Attach(new (std::nothrow) Frame(std::move(image)));
    if (!source) return E_OUTOFMEMORY;
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    // Premultiply first, then scale, so transparent edges don't bleed colour.
    ComPtr<IWICBitmapSource> premultiplied;
    if (SUCCEEDED(hr)) hr = WICConvertBitmapSource(GUID_WICPixelFormat32bppPBGRA, source.Get(), &premultiplied);
    ComPtr<IWICBitmapScaler> scaler;
    if (SUCCEEDED(hr)) hr = factory->CreateBitmapScaler(&scaler);
    if (SUCCEEDED(hr)) hr = scaler->Initialize(premultiplied.Get(), w, h, WICBitmapInterpolationModeFant);
    if (FAILED(hr)) return hr;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = LONG(w);
    info.bmiHeader.biHeight = -LONG(h);  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) return E_OUTOFMEMORY;
    hr = scaler->CopyPixels(nullptr, w * 4, w * 4 * h, static_cast<BYTE*>(bits));
    if (FAILED(hr)) { DeleteObject(bitmap); return hr; }
    *out = bitmap;
    return S_OK;
}

/// Explorer's thumbnail handler for .vbn. Draws thumbnails itself rather than relying on the system's
/// PhotoThumbnailProvider, which on Windows 10 does not use third-party WIC codecs.
class ThumbnailProvider final : public ComObject<IInitializeWithStream, IThumbnailProvider> {
public:
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IInitializeWithStream) *out = static_cast<IInitializeWithStream*>(this);
        else if (iid == IID_IThumbnailProvider) *out = static_cast<IThumbnailProvider*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }

    STDMETHODIMP Initialize(IStream* stream, DWORD) override {
        if (!stream) return E_INVALIDARG;
        if (!data_.empty()) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        try {
            return readAll(stream, data_);
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        }
    }

    STDMETHODIMP GetThumbnail(UINT cx, HBITMAP* bitmap, WTS_ALPHATYPE* alpha) override {
        if (!bitmap || !alpha || cx == 0) return E_INVALIDARG;
        *bitmap = nullptr;
        try {
            auto image = std::make_shared<vbn::Image>();
            std::string error;
            if (!vbn::decode(data_.data(), data_.size(), *image, error)) return WINCODEC_ERR_BADIMAGE;
            *alpha = WTSAT_ARGB;
            return makeThumbnail(std::move(image), cx, bitmap);
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        }
    }

private:
    std::vector<uint8_t> data_;
};

// MARK: - Class factory

class Factory final : public ComObject<IClassFactory> {
public:
    explicit Factory(bool thumbnail) : thumbnail_(thumbnail) {}

    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IClassFactory) {
            *out = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        IUnknown* object = thumbnail_ ? static_cast<IInitializeWithStream*>(new (std::nothrow) ThumbnailProvider())
                                      : static_cast<IUnknown*>(static_cast<IWICBitmapDecoder*>(new (std::nothrow) Decoder()));
        if (!object) return E_OUTOFMEMORY;
        const HRESULT hr = object->QueryInterface(iid, out);
        object->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) ++g_locks; else --g_locks;
        return S_OK;
    }

private:
    const bool thumbnail_;
};

// MARK: - Registry

const std::wstring kClasses = L"SOFTWARE\\Classes\\";
const std::wstring kDecoderKey = kClasses + L"CLSID\\" UNVBN_DECODER_CLSID_STR;
// CATID_WICBitmapDecoders: WIC enumerates decoders listed under this category.
const std::wstring kInstanceKey =
    kClasses + L"CLSID\\{7ED96837-96F0-4812-B211-F13C24117ED3}\\Instance\\" UNVBN_DECODER_CLSID_STR;
const std::wstring kProgId = L"unVBN.vbnfile";
const std::wstring kExtKey = kClasses + L".vbn";
// unVBN's shell handlers live under SystemFileAssociations\.vbn, a key only unVBN writes. Explorer looks
// there after the ProgID and the .vbn key, so a handler another program registered for .vbn wins, and
// before SystemFileAssociations\image, so unVBN wins over the generic photo thumbnailer.
const std::wstring kSystemAssocKey = kClasses + L"SystemFileAssociations\\.vbn";
const wchar_t* const kThumbnailShellEx = L"\\ShellEx\\{e357fccd-a995-4576-b01f-234630154e96}";
const std::wstring kThumbnailKey = kSystemAssocKey + kThumbnailShellEx;
const std::wstring kPreviewKey = kSystemAssocKey + L"\\ShellEx\\ContextMenuHandlers\\ShellImagePreview";
// v0.3.0 and earlier put the thumbnail handler in .vbn\ShellEx and kept markers there; removed on sight.
const std::wstring kLegacyThumbnailKey = kExtKey + kThumbnailShellEx;
const std::wstring kKindMapKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\KindMap";
const std::wstring kThumbnailClsidKey = kClasses + L"CLSID\\" UNVBN_THUMBNAIL_CLSID_STR;
const std::wstring kApprovedKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved";
const wchar_t* const kPhotoViewerPreview = L"{FFE2A43C-56B9-4bf5-9A79-CC6D4285608A}";
const wchar_t* const kMime = L"image/x-vizrt-vbn";
const wchar_t* const kFriendlyName = L"unVBN Vizrt VBN Decoder";
const wchar_t* const kLegacyMarkers[] = {L"unVBN.CreatedKey", L"unVBN.AddedThumbnail"};

HRESULT check(LSTATUS status) { return status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(status); }

bool readString(const std::wstring& path, const wchar_t* name, std::wstring& value) {
    wchar_t buffer[512];
    DWORD size = sizeof buffer;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, path.c_str(), name, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS)
        return false;
    value = buffer;
    return true;
}

/// Creates the key if needed and sets the value (HKLM).
HRESULT setValue(const std::wstring& path, const wchar_t* name, DWORD type, const void* data, DWORD size) {
    return check(RegSetKeyValueW(HKEY_LOCAL_MACHINE, path.c_str(), name, type, data, size));
}

HRESULT setString(const std::wstring& path, const wchar_t* name, const std::wstring& value, DWORD type = REG_SZ) {
    return setValue(path, name, type, value.c_str(), DWORD((value.size() + 1) * sizeof(wchar_t)));
}

HRESULT setDword(const std::wstring& path, const wchar_t* name, DWORD value) {
    return setValue(path, name, REG_DWORD, &value, sizeof value);
}

HRESULT setBinary(const std::wstring& path, const wchar_t* name, std::initializer_list<BYTE> bytes) {
    return setValue(path, name, REG_BINARY, bytes.begin(), DWORD(bytes.size()));
}

void deleteTree(const std::wstring& path) { RegDeleteTreeW(HKEY_LOCAL_MACHINE, path.c_str()); }

void deleteValue(const std::wstring& path, const wchar_t* name) {
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, path.c_str(), name);
}

/// Deletes `path` and then its parents up to (excluding) `stop`, as long as each is empty.
void deleteEmptyKeys(std::wstring path, const std::wstring& stop) {
    while (path.size() > stop.size() && path.compare(0, stop.size(), stop) == 0) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key) == ERROR_SUCCESS) {
            DWORD subkeys = 1, values = 1;
            RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr,
                             nullptr, nullptr);
            RegCloseKey(key);
            if (subkeys || values) return;
            RegDeleteKeyW(HKEY_LOCAL_MACHINE, path.c_str());
        }
        path.erase(path.find_last_of(L'\\'));
    }
}

#define TRY(expr)                        \
    do {                                 \
        const HRESULT hr_ = (expr);      \
        if (FAILED(hr_)) return hr_;     \
    } while (0)

HRESULT registerThumbnailHandler(const std::wstring& dllPath) {
    TRY(setString(kThumbnailClsidKey, nullptr, L"unVBN thumbnail handler"));
    TRY(setString(kThumbnailClsidKey + L"\\InprocServer32", nullptr, dllPath));
    TRY(setString(kThumbnailClsidKey + L"\\InprocServer32", L"ThreadingModel", L"Apartment"));
    // Listed as approved, which managed PCs with EnforceShellExtensionSecurity require.
    TRY(setString(kApprovedKey, UNVBN_THUMBNAIL_CLSID_STR, L"unVBN thumbnail handler"));
    return S_OK;
}

HRESULT registerDecoder(const std::wstring& dllPath) {
    const std::wstring& k = kDecoderKey;
    TRY(setString(k, nullptr, kFriendlyName));
    TRY(setString(k, L"FriendlyName", kFriendlyName));
    TRY(setString(k, L"Author", L"unVBN (github.com/notjeremie/unVBN)"));
    TRY(setString(k, L"Description", L"Decodes Vizrt Viz Artist / Viz Engine .vbn images"));
    TRY(setString(k, L"Version", UNVBN_VERSION_W));
    TRY(setString(k, L"Vendor", UNVBN_VENDOR_STR));
    TRY(setString(k, L"ContainerFormat", UNVBN_CONTAINER_STR));
    TRY(setString(k, L"MimeTypes", kMime));
    TRY(setString(k, L"FileExtensions", L".vbn"));
    TRY(setString(k, L"SpecVersion", L"1.0.0.0"));
    TRY(setString(k, L"ColorManagementVersion", L"1.0.0.0"));
    TRY(setDword(k, L"SupportAnimation", 0));
    TRY(setDword(k, L"SupportChromakey", 0));
    TRY(setDword(k, L"SupportLossless", 1));
    TRY(setDword(k, L"SupportMultiframe", 0));
    TRY(setString(k + L"\\InprocServer32", nullptr, dllPath));
    TRY(setString(k + L"\\InprocServer32", L"ThreadingModel", L"Both"));
    TRY(setString(k + L"\\Formats\\{6fddc324-4e03-4bfe-b185-3d77768dc90f}", nullptr, L""));  // 32bppBGRA
    // Content sniffing: the magic number 1E F1 0D 90 at offset 0.
    TRY(setDword(k + L"\\Patterns\\0", L"Position", 0));
    TRY(setDword(k + L"\\Patterns\\0", L"Length", 4));
    TRY(setBinary(k + L"\\Patterns\\0", L"Pattern", {0x1E, 0xF1, 0x0D, 0x90}));
    TRY(setBinary(k + L"\\Patterns\\0", L"Mask", {0xFF, 0xFF, 0xFF, 0xFF}));
    TRY(setString(kInstanceKey, L"CLSID", UNVBN_DECODER_CLSID_STR));
    TRY(setString(kInstanceKey, L"FriendlyName", kFriendlyName));
    return S_OK;
}

/// Removes what v0.3.0 and earlier wrote into the shared .vbn key: its thumbnail entry (only if it is
/// still unVBN's) and the ownership markers.
void removeLegacyEntries() {
    std::wstring current;
    if (readString(kLegacyThumbnailKey, nullptr, current) && _wcsicmp(current.c_str(), UNVBN_THUMBNAIL_CLSID_STR) == 0)
        deleteTree(kLegacyThumbnailKey);
    for (const wchar_t* marker : kLegacyMarkers) deleteValue(kExtKey, marker);
    deleteEmptyKeys(kLegacyThumbnailKey, kExtKey);
}

/// Makes Explorer treat .vbn as a picture: thumbnails (our handler), preview, Kind, "Open with". In the
/// shared .vbn key it only fills values that are missing, so an existing association (e.g. from a Vizrt
/// install) keeps its default program, and adds itself to "Open with".
HRESULT registerShell() {
    TRY(setString(kClasses + kProgId, nullptr, L"Vizrt image"));
    TRY(setString(kClasses + kProgId, L"FriendlyTypeName", L"Vizrt image"));
    // Double-click: Windows Photo Viewer (built into Windows 10/11, reads images through WIC, so it
    // shows .vbn via this codec). Used only while .vbn has no default app the user picked.
    wchar_t viewer[MAX_PATH];
    if (ExpandEnvironmentStringsW(L"%ProgramFiles%\\Windows Photo Viewer\\PhotoViewer.dll", viewer, MAX_PATH) &&
        GetFileAttributesW(viewer) != INVALID_FILE_ATTRIBUTES) {
        TRY(setString(kClasses + kProgId + L"\\shell\\open", nullptr, L"Open with Windows Photo Viewer"));
        TRY(setString(kClasses + kProgId + L"\\shell\\open\\command", nullptr,
                      L"%SystemRoot%\\System32\\rundll32.exe \"%ProgramFiles%\\Windows Photo Viewer\\PhotoViewer.dll\", "
                      L"ImageView_Fullscreen %1",
                      REG_EXPAND_SZ));
    }

    removeLegacyEntries();
    std::wstring current;
    if (!readString(kExtKey, nullptr, current) || current.empty()) TRY(setString(kExtKey, nullptr, kProgId));
    if (!readString(kExtKey, L"Content Type", current)) TRY(setString(kExtKey, L"Content Type", kMime));
    if (!readString(kExtKey, L"PerceivedType", current)) TRY(setString(kExtKey, L"PerceivedType", L"image"));
    TRY(setString(kExtKey + L"\\OpenWithProgids", kProgId.c_str(), L""));
    TRY(setString(kExtKey + L"\\OpenWithList\\PhotoViewer.dll", nullptr, L""));

    TRY(setString(kThumbnailKey, nullptr, UNVBN_THUMBNAIL_CLSID_STR));
    TRY(setString(kPreviewKey, nullptr, kPhotoViewerPreview));
    TRY(setString(kKindMapKey, L".vbn", L"picture"));
    return S_OK;
}

/// Undoes registerShell without any recorded state: values in .vbn are removed only when they are
/// unVBN's, and .vbn itself only when nothing else is left in it.
void unregisterShell() {
    removeLegacyEntries();
    std::wstring current;
    if (readString(kExtKey, nullptr, current) && current == kProgId) deleteValue(kExtKey, nullptr);
    if (readString(kExtKey, L"Content Type", current) && current == kMime) deleteValue(kExtKey, L"Content Type");
    // PerceivedType "image" is left in place when another program still owns .vbn (it is true either way).
    if (!readString(kExtKey, nullptr, current) && readString(kExtKey, L"PerceivedType", current) && current == L"image")
        deleteValue(kExtKey, L"PerceivedType");
    deleteValue(kExtKey + L"\\OpenWithProgids", kProgId.c_str());
    deleteTree(kExtKey + L"\\OpenWithList\\PhotoViewer.dll");
    deleteEmptyKeys(kExtKey + L"\\OpenWithProgids", kClasses);
    deleteEmptyKeys(kExtKey + L"\\OpenWithList", kClasses);

    deleteTree(kClasses + kProgId);
    deleteTree(kThumbnailKey);
    deleteTree(kPreviewKey);
    deleteEmptyKeys(kSystemAssocKey + L"\\ShellEx\\ContextMenuHandlers", kClasses + L"SystemFileAssociations");
    if (readString(kKindMapKey, L".vbn", current) && current == L"picture") deleteValue(kKindMapKey, L".vbn");
}

}  // namespace

// MARK: - DLL exports

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (clsid != CLSID_UnVBNDecoder && clsid != CLSID_UnVBNThumbnail) return CLASS_E_CLASSNOTAVAILABLE;
    Factory* factory = new (std::nothrow) Factory(clsid == CLSID_UnVBNThumbnail);
    if (!factory) return E_OUTOFMEMORY;
    const HRESULT hr = factory->QueryInterface(iid, out);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow() { return g_objects == 0 && g_locks == 0 ? S_OK : S_FALSE; }

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return HRESULT_FROM_WIN32(GetLastError());
    HRESULT hr = registerDecoder(path);
    if (SUCCEEDED(hr)) hr = registerThumbnailHandler(path);
    if (SUCCEEDED(hr)) hr = registerShell();
    if (FAILED(hr)) return hr;
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

STDAPI DllUnregisterServer() {
    deleteTree(kDecoderKey);
    deleteTree(kInstanceKey);
    deleteTree(kThumbnailClsidKey);
    deleteValue(kApprovedKey, UNVBN_THUMBNAIL_CLSID_STR);
    unregisterShell();
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}
