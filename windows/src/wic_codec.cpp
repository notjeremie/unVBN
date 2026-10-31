// unVBN WIC codec: lets Windows (Explorer thumbnails and preview, photo viewers, any WIC-based app)
// open Vizrt .vbn images directly. Decode only: one frame, 32bpp BGRA with straight alpha.
//
// Registration (DllRegisterServer / DllUnregisterServer, also run by the MSI) is the single source of
// truth for the registry entries; see register* below.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <wincodec.h>

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

/// Header plus the largest legal raw BGRA payload (16384 x 16384 x 4).
constexpr ULONGLONG kMaxFileSize = vbn::kHeaderSize + ULONGLONG(vbn::kMaxDimension) * vbn::kMaxDimension * 4;

// MARK: - Frame

class Frame final : public IWICBitmapFrameDecode {
public:
    explicit Frame(std::shared_ptr<const vbn::Image> image) : image_(std::move(image)) { ++g_objects; }
    ~Frame() { --g_objects; }

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
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
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
    std::atomic<ULONG> refs_{1};
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

/// Reads the rest of `stream` (from its current position). Fails above kMaxFileSize.
/// Streams may return fewer bytes than asked before the end (the Explorer thumbnail host does):
/// only a read of 0 bytes means end of stream.
HRESULT readAll(IStream* stream, std::vector<uint8_t>& out) {
    STATSTG stat{};
    if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart <= kMaxFileSize)
        out.reserve(size_t(stat.cbSize.QuadPart));
    BYTE chunk[64 * 1024];
    for (;;) {
        ULONG got = 0;
        const HRESULT hr = stream->Read(chunk, sizeof chunk, &got);
        if (FAILED(hr)) return hr;
        if (got == 0) return S_OK;
        if (out.size() + got > kMaxFileSize) return WINCODEC_ERR_BADIMAGE;
        out.insert(out.end(), chunk, chunk + got);
    }
}

class Decoder final : public IWICBitmapDecoder {
public:
    Decoder() { ++g_objects; }
    ~Decoder() { --g_objects; }

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
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
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
        IWICImagingFactory* factory = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return hr;
        IWICComponentInfo* component = nullptr;
        hr = factory->CreateComponentInfo(CLSID_UnVBNDecoder, &component);
        if (SUCCEEDED(hr)) {
            hr = component->QueryInterface(IID_PPV_ARGS(info));
            component->Release();
        }
        factory->Release();
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
    std::atomic<ULONG> refs_{1};
    SRWLOCK lock_ = SRWLOCK_INIT;
    std::shared_ptr<const vbn::Image> image_;
};

// MARK: - Class factory

class Factory final : public IClassFactory {
public:
    Factory() { ++g_objects; }
    ~Factory() { --g_objects; }

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
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        Decoder* decoder = new (std::nothrow) Decoder();
        if (!decoder) return E_OUTOFMEMORY;
        const HRESULT hr = decoder->QueryInterface(iid, out);
        decoder->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) ++g_locks; else --g_locks;
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
};

// MARK: - Registry

const std::wstring kClasses = L"SOFTWARE\\Classes\\";
const std::wstring kDecoderKey = kClasses + L"CLSID\\" UNVBN_DECODER_CLSID_STR;
// CATID_WICBitmapDecoders: WIC enumerates decoders listed under this category.
const std::wstring kInstanceKey =
    kClasses + L"CLSID\\{7ED96837-96F0-4812-B211-F13C24117ED3}\\Instance\\" UNVBN_DECODER_CLSID_STR;
const std::wstring kProgId = L"unVBN.vbnfile";
const std::wstring kExtKey = kClasses + L".vbn";
const std::wstring kThumbnailKey = kExtKey + L"\\ShellEx\\{e357fccd-a995-4576-b01f-234630154e96}";
const std::wstring kPreviewKey = kClasses + L"SystemFileAssociations\\.vbn\\ShellEx\\ContextMenuHandlers\\ShellImagePreview";
const std::wstring kKindMapKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\KindMap";
const wchar_t* const kPhotoThumbnailProvider = L"{C7657C4A-9F68-40fa-A4DF-96BC08EB3551}";
const wchar_t* const kPhotoViewerPreview = L"{FFE2A43C-56B9-4bf5-9A79-CC6D4285608A}";
const wchar_t* const kMime = L"image/x-vizrt-vbn";
const wchar_t* const kFriendlyName = L"unVBN Vizrt VBN Decoder";
// Markers recording what registration created, so unregistering never removes another app's association.
const wchar_t* const kCreatedExtMarker = L"unVBN.CreatedKey";
const wchar_t* const kAddedThumbnailMarker = L"unVBN.AddedThumbnail";

HRESULT check(LSTATUS status) { return status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(status); }

bool keyExists(const std::wstring& path) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    RegCloseKey(key);
    return true;
}

bool readString(const std::wstring& path, const wchar_t* name, std::wstring& value) {
    wchar_t buffer[512];
    DWORD size = sizeof buffer;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, path.c_str(), name, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS)
        return false;
    value = buffer;
    return true;
}

HRESULT setValue(const std::wstring& path, const wchar_t* name, DWORD type, const void* data, DWORD size) {
    HKEY key = nullptr;
    LSTATUS s = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
    if (s != ERROR_SUCCESS) return check(s);
    s = RegSetValueExW(key, name, 0, type, static_cast<const BYTE*>(data), size);
    RegCloseKey(key);
    return check(s);
}

HRESULT setString(const std::wstring& path, const wchar_t* name, const std::wstring& value) {
    return setValue(path, name, REG_SZ, value.c_str(), DWORD((value.size() + 1) * sizeof(wchar_t)));
}

HRESULT setDword(const std::wstring& path, const wchar_t* name, DWORD value) {
    return setValue(path, name, REG_DWORD, &value, sizeof value);
}

HRESULT setBinary(const std::wstring& path, const wchar_t* name, std::initializer_list<BYTE> bytes) {
    std::vector<BYTE> v(bytes);
    return setValue(path, name, REG_BINARY, v.data(), DWORD(v.size()));
}

void deleteTree(const std::wstring& path) { RegDeleteTreeW(HKEY_LOCAL_MACHINE, path.c_str()); }

void deleteValue(const std::wstring& path, const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    RegDeleteValueW(key, name);
    RegCloseKey(key);
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

/// Makes Explorer treat .vbn as a picture: thumbnails, preview, Kind, "Open with". An existing
/// association (e.g. from a Vizrt install) keeps its default program and thumbnail handler.
HRESULT registerShell() {
    TRY(setString(kClasses + kProgId, nullptr, L"Vizrt image"));
    TRY(setString(kClasses + kProgId, L"FriendlyTypeName", L"Vizrt image"));

    std::wstring current;
    const bool extExisted = keyExists(kExtKey);
    if (!extExisted) TRY(setDword(kExtKey, kCreatedExtMarker, 1));
    if (!readString(kExtKey, nullptr, current) || current.empty()) TRY(setString(kExtKey, nullptr, kProgId));
    if (!readString(kExtKey, L"Content Type", current)) TRY(setString(kExtKey, L"Content Type", kMime));
    if (!readString(kExtKey, L"PerceivedType", current)) TRY(setString(kExtKey, L"PerceivedType", L"image"));
    TRY(setString(kExtKey + L"\\OpenWithProgids", kProgId.c_str(), L""));
    TRY(setString(kExtKey + L"\\OpenWithList\\PhotoViewer.dll", nullptr, L""));
    if (!readString(kThumbnailKey, nullptr, current)) {
        TRY(setString(kThumbnailKey, nullptr, kPhotoThumbnailProvider));
        if (extExisted) TRY(setDword(kExtKey, kAddedThumbnailMarker, 1));
    }
    TRY(setString(kPreviewKey, nullptr, kPhotoViewerPreview));
    TRY(setString(kKindMapKey, L".vbn", L"picture"));
    return S_OK;
}

void unregisterShell() {
    DWORD created = 0, addedThumbnail = 0, size = sizeof(DWORD);
    RegGetValueW(HKEY_LOCAL_MACHINE, kExtKey.c_str(), kCreatedExtMarker, RRF_RT_REG_DWORD, nullptr, &created, &size);
    size = sizeof(DWORD);
    RegGetValueW(HKEY_LOCAL_MACHINE, kExtKey.c_str(), kAddedThumbnailMarker, RRF_RT_REG_DWORD, nullptr, &addedThumbnail,
                 &size);
    if (created) {
        deleteTree(kExtKey);
    } else {
        std::wstring current;
        if (readString(kExtKey, nullptr, current) && current == kProgId) deleteValue(kExtKey, nullptr);
        deleteValue(kExtKey + L"\\OpenWithProgids", kProgId.c_str());
        if (addedThumbnail) deleteTree(kThumbnailKey);
        deleteValue(kExtKey, kAddedThumbnailMarker);
        deleteEmptyKeys(kExtKey + L"\\OpenWithProgids", kExtKey);
        deleteEmptyKeys(kThumbnailKey, kExtKey);
    }
    deleteTree(kClasses + kProgId);
    deleteTree(kPreviewKey);
    deleteEmptyKeys(kClasses + L"SystemFileAssociations\\.vbn\\ShellEx\\ContextMenuHandlers",
                    kClasses + L"SystemFileAssociations");
    std::wstring kind;
    if (readString(kKindMapKey, L".vbn", kind) && kind == L"picture") deleteValue(kKindMapKey, L".vbn");
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
    if (clsid != CLSID_UnVBNDecoder) return CLASS_E_CLASSNOTAVAILABLE;
    Factory* factory = new (std::nothrow) Factory();
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
    if (SUCCEEDED(hr)) hr = registerShell();
    if (FAILED(hr)) return hr;
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

STDAPI DllUnregisterServer() {
    deleteTree(kDecoderKey);
    deleteTree(kInstanceKey);
    unregisterShell();
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}
