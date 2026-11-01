// Windows encoders: PNG through WIC, MP4 (H.264) through Media Foundation, WebM (VP9 + alpha) through
// libvpx and libwebm. Everything ships inside unVBN; nothing depends on codecs the user installed.
#include <windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <numeric>
#include <thread>

#include <mkvmuxer/mkvmuxer.h>
#include <vpx/vp8cx.h>
#include <vpx/vpx_encoder.h>

#include "exporter.h"
#include "yuv.h"

using Microsoft::WRL::ComPtr;

namespace vbnx {
namespace {

std::string hresultText(HRESULT hr) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(hr));
    std::string text = buf;
    wchar_t* message = nullptr;
    if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                       DWORD(hr), 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr) && message) {
        std::string m = utf8(fs::path(message));
        LocalFree(message);
        while (!m.empty() && (m.back() == '\n' || m.back() == '\r' || m.back() == ' ' || m.back() == '.')) m.pop_back();
        if (!m.empty()) text = m + " (" + text + ")";
    }
    return text;
}

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) throw Error(Error::Kind::encoderFailed, std::string(what) + ": " + hresultText(hr));
}

/// COM for the current thread (multithreaded apartment), for as long as the thread lives.
void ensureCom() {
    struct Guard {
        bool owned;
        Guard() : owned(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
        ~Guard() { if (owned) CoUninitialize(); }
    };
    thread_local Guard guard;
}

void removeQuietly(const fs::path& p) {
    std::error_code ec;
    fs::remove(p, ec);
}

// MARK: PNG

/// One PNG per frame, named after the source frame. Frames are encoded and written concurrently.
class PngSink : public FrameSink {
public:
    explicit PngSink(const fs::path& directory) : directory_(directory) {
        std::error_code ec;
        fs::create_directories(directory, ec);
        if (ec) throw Error(Error::Kind::ioFailed, "Cannot create " + utf8(directory) + ": " + ec.message());
    }

    bool acceptsConcurrentFrames() const override { return true; }

    void append(const vbn::Image& image, const Frame& frame) override {
        ensureCom();
        thread_local ComPtr<IWICImagingFactory> factory;
        if (!factory)
            check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "WIC");
        const fs::path out = directory_ / frame.path.stem().concat(L".png");
        {
            std::lock_guard<std::mutex> g(lock_);
            written_.push_back(out);
        }
        const std::string name = utf8(out.filename());
        ComPtr<IWICStream> stream;
        check(factory->CreateStream(&stream), "WIC stream");
        check(stream->InitializeFromFilename(out.c_str(), GENERIC_WRITE), ("Cannot write " + name).c_str());
        ComPtr<IWICBitmapEncoder> encoder;
        check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "PNG encoder");
        check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "PNG encoder");
        ComPtr<IWICBitmapFrameEncode> frameEncode;
        ComPtr<IPropertyBag2> props;
        check(encoder->CreateNewFrame(&frameEncode, &props), "PNG frame");
        check(frameEncode->Initialize(props.Get()), "PNG frame");
        check(frameEncode->SetSize(image.width, image.height), "PNG size");
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        check(frameEncode->SetPixelFormat(&format), "PNG pixel format");
        if (format != GUID_WICPixelFormat32bppBGRA) throw Error(Error::Kind::encoderFailed, "PNG encoder refused BGRA");
        check(frameEncode->WritePixels(image.height, image.width * 4, UINT(image.bgra.size()),
                                       const_cast<BYTE*>(image.bgra.data())),
              ("Cannot write " + name).c_str());
        check(frameEncode->Commit(), ("Cannot write " + name).c_str());
        check(encoder->Commit(), ("Cannot write " + name).c_str());
    }

    void finish() override {}

    void abort() noexcept override {
        std::lock_guard<std::mutex> g(lock_);
        for (const auto& p : written_) removeQuietly(p);
    }

private:
    fs::path directory_;
    std::mutex lock_;
    std::vector<fs::path> written_;
};

// MARK: MP4

/// The frame rate as an exact ratio: 50 -> 50/1, 29.97 -> 2997/100.
std::pair<UINT32, UINT32> fpsRatio(double fps) {
    const UINT32 num = UINT32(std::llround(fps * 1000)), g = std::gcd(num, 1000u);
    return {num / g, 1000 / g};
}

/// H.264 in MP4 through the Media Foundation sink writer and Windows' own encoder. No alpha: frames are
/// flattened onto the background colour, and padded right/bottom with it where the encoder needs a bigger
/// frame: 4:2:0 needs even sizes, and Windows' encoder refuses any side of 32 pixels or less.
/// Its upper limits: 8192 pixels per side, and the frame area of H.264 level 6.2 (8192x4320 passes) on
/// Windows 11, level 5.2 (4096x2304) on Windows 10.
class Mp4Sink : public FrameSink {
public:
    static constexpr uint32_t kMaxSide = 8192;

    Mp4Sink(const fs::path& output, const Options& options, uint32_t width, uint32_t height)
        : output_(output), options_(options), srcW_(width), srcH_(height), w_(paddedSide(width)), h_(paddedSide(height)) {
        if (w_ > kMaxSide || h_ > kMaxSide || macroblocks() > kLevel62Macroblocks)
            throw Error(Error::Kind::encoderFailed,
                        "MP4 on Windows is limited to " + std::to_string(kMaxSide) + " pixels per side and 8192x4320 in area; this sequence is " +
                            std::to_string(width) + "x" + std::to_string(height) + ". Export it as WebM or PNG instead.");
        ensureCom();
        static const HRESULT startup = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(startup))
            throw Error(Error::Kind::encoderFailed,
                        "MP4 export needs Windows Media Foundation (on Windows N editions, install the Media Feature Pack): " +
                            hresultText(startup));
        removeQuietly(output);

        ComPtr<IMFAttributes> attrs;
        check(MFCreateAttributes(&attrs, 2), "Media Foundation");
        attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
        // Windows' software encoder: the same result on every machine (GPU encoders vary and some refuse sizes).
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
        check(MFCreateSinkWriterFromURL(output.c_str(), nullptr, attrs.Get(), &writer_), "Cannot create the MP4 file");

        const auto [num, den] = fpsRatio(options.fps);
        auto common = [&](IMFMediaType* t, const GUID& subtype, bool tagColour) {
            check(t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "media type");
            check(t->SetGUID(MF_MT_SUBTYPE, subtype), "media type");
            check(t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "media type");
            check(MFSetAttributeSize(t, MF_MT_FRAME_SIZE, w_, h_), "media type");
            check(MFSetAttributeRatio(t, MF_MT_FRAME_RATE, num, den), "media type");
            check(MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "media type");
            if (!tagColour) return;
            // HD broadcast is BT.709, and the frames are converted with that matrix (yuv.cpp).
            t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
            t->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
            t->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
            t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        };
        ComPtr<IMFMediaType> out;
        check(MFCreateMediaType(&out), "media type");
        common(out.Get(), MFVideoFormat_H264, true);
        const double bitrate = std::max(2e6, double(w_) * h_ * options.fps * 0.2);
        check(out->SetUINT32(MF_MT_AVG_BITRATE, UINT32(std::min(bitrate, 4e9))), "media type");
        check(out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High), "media type");
        // Left alone, the encoder picks a level up to 5.2 and refuses frames above 4096x2304 in area. Above
        // that, ask for 6.2 (not for smaller frames: a lower level plays on more devices).
        if (macroblocks() > kLevel52Macroblocks) check(out->SetUINT32(MF_MT_MPEG2_LEVEL, 62), "media type");
        check(writer_->AddStream(out.Get(), &stream_), "H.264 encoder");

        ComPtr<IMFMediaType> in;
        check(MFCreateMediaType(&in), "media type");
        common(in.Get(), MFVideoFormat_NV12, true);
        check(in->SetUINT32(MF_MT_DEFAULT_STRIDE, w_), "media type");
        if (const HRESULT hr = writer_->SetInputMediaType(stream_, in.Get(), nullptr); FAILED(hr)) {
            writer_.Reset();
            removeQuietly(output_);
            // Windows 10's encoder ignores the level 6.2 request and stays at 4096x2304 in area.
            if (hr == MF_E_INVALIDMEDIATYPE)
                throw Error(Error::Kind::encoderFailed,
                            "Windows' H.264 encoder on this computer refuses " + std::to_string(width) + "x" + std::to_string(height) +
                                " frames (Windows 10 stops at 4096x2304 in area, Windows 11 at 8192x4320). Export it as WebM or PNG instead.");
            check(hr, "H.264 encoder");
        }
        check(writer_->BeginWriting(), "H.264 encoder");
    }

    bool acceptsConcurrentFrames() const override { return false; }

    void append(const vbn::Image& image, const Frame&) override {
        const auto flat = flattenOnto(image.bgra.data(), srcW_, srcH_, options_.background.r, options_.background.g,
                                      options_.background.b, w_, h_);
        const DWORD size = w_ * h_ * 3 / 2;
        ComPtr<IMFMediaBuffer> buffer;
        check(MFCreateMemoryBuffer(size, &buffer), "frame buffer");
        BYTE* data = nullptr;
        check(buffer->Lock(&data, nullptr, nullptr), "frame buffer");
        bgraToNV12(flat.data(), w_, h_, {data, ptrdiff_t(w_)}, {data + size_t(w_) * h_, ptrdiff_t(w_)});
        buffer->Unlock();
        check(buffer->SetCurrentLength(size), "frame buffer");

        ComPtr<IMFSample> sample;
        check(MFCreateSample(&sample), "frame sample");
        check(sample->AddBuffer(buffer.Get()), "frame sample");
        const LONGLONG t0 = time(frameIndex_), t1 = time(frameIndex_ + 1);
        check(sample->SetSampleTime(t0), "frame sample");
        check(sample->SetSampleDuration(t1 - t0), "frame sample");
        check(writer_->WriteSample(stream_, sample.Get()), "H.264 encoding failed");
        ++frameIndex_;
    }

    void finish() override {
        check(writer_->Finalize(), "Cannot finish the MP4 file");
        writer_.Reset();
    }

    void abort() noexcept override {
        writer_.Reset();
        removeQuietly(output_);
    }

    static uint32_t paddedSide(uint32_t n) { return std::max(34u, (n + 1) & ~1u); }

private:
    // Maximum frame size, in 16x16 macroblocks, of H.264 levels 5.2 and 6.2 (spec table A-1).
    static constexpr uint64_t kLevel52Macroblocks = 36864, kLevel62Macroblocks = 139264;
    uint64_t macroblocks() const { return uint64_t((w_ + 15) / 16) * ((h_ + 15) / 16); }

    /// Frame `i`'s start in 100 ns units.
    LONGLONG time(LONGLONG i) const { return std::llround(double(i) * 1e7 / options_.fps); }

    fs::path output_;
    Options options_;
    uint32_t srcW_, srcH_, w_, h_;
    ComPtr<IMFSinkWriter> writer_;
    DWORD stream_ = 0;
    LONGLONG frameIndex_ = 0;
};

// MARK: WebM

/// libwebm's file writer, opening the file with a wide (Unicode) path.
class WideFileWriter : public mkvmuxer::IMkvWriter {
public:
    explicit WideFileWriter(const fs::path& path) {
        if (_wfopen_s(&file_, path.c_str(), L"wb") != 0) file_ = nullptr;
    }
    ~WideFileWriter() override { close(); }
    bool isOpen() const { return file_ != nullptr; }
    bool close() {
        if (!file_) return true;
        const bool ok = std::fclose(file_) == 0;
        file_ = nullptr;
        return ok;
    }

    mkvmuxer::int32 Write(const void* buf, mkvmuxer::uint32 len) override {
        return file_ && std::fwrite(buf, 1, len, file_) == len ? 0 : -1;
    }
    mkvmuxer::int64 Position() const override { return file_ ? _ftelli64(file_) : -1; }
    mkvmuxer::int32 Position(mkvmuxer::int64 position) override { return file_ ? _fseeki64(file_, position, SEEK_SET) : -1; }
    bool Seekable() const override { return true; }
    void ElementStartNotify(mkvmuxer::uint64, mkvmuxer::int64) override {}

private:
    FILE* file_ = nullptr;
};

/// One libvpx VP9 encoder. The colour and the alpha streams each get one, configured identically so
/// their frames pair up one to one.
class Vp9Encoder {
public:
    struct Packet {
        vpx_codec_pts_t pts;
        bool key;
        std::vector<uint8_t> data;
    };

    Vp9Encoder(uint32_t width, uint32_t height, const Options& options, bool alpha) {
        vpx_codec_enc_cfg_t cfg;
        if (vpx_codec_enc_config_default(vpx_codec_vp9_cx(), &cfg, 0)) fail("configuration");
        cfg.g_w = width;
        cfg.g_h = height;
        // One tick per frame.
        cfg.g_timebase.num = 1000;
        cfg.g_timebase.den = int(std::llround(options.fps * 1000));
        cfg.g_threads = std::min(16u, std::max(1u, std::thread::hardware_concurrency()));
        cfg.rc_end_usage = VPX_Q;  // constant quality, like ffmpeg's -crf N -b:v 0
        // Keyframes only where we force them, on both streams at once (an alpha frame must decode with its colour frame).
        cfg.kf_mode = VPX_KF_DISABLED;
        if (vpx_codec_enc_init(&ctx_, vpx_codec_vp9_cx(), &cfg, 0)) fail("initialisation");
        open_ = true;
        control(VP8E_SET_CPUUSED, 4);
        control(VP8E_SET_CQ_LEVEL, options.webmCRF);
        control(VP9E_SET_ROW_MT, 1);
        if (!alpha) {
            control(VP9E_SET_COLOR_SPACE, VPX_CS_BT_709);
            control(VP9E_SET_COLOR_RANGE, VPX_CR_STUDIO_RANGE);
        }
    }
    ~Vp9Encoder() { if (open_) vpx_codec_destroy(&ctx_); }
    Vp9Encoder(const Vp9Encoder&) = delete;
    Vp9Encoder& operator=(const Vp9Encoder&) = delete;

    /// Encodes `image` (nullptr flushes) and queues the packets that come out; returns how many did.
    size_t encode(const vpx_image_t* image, vpx_codec_pts_t pts, bool forceKey) {
        if (vpx_codec_encode(&ctx_, image, pts, 1, forceKey ? VPX_EFLAG_FORCE_KF : 0, VPX_DL_GOOD_QUALITY)) fail("encoding");
        const size_t before = packets.size();
        vpx_codec_iter_t it = nullptr;
        while (const vpx_codec_cx_pkt_t* pkt = vpx_codec_get_cx_data(&ctx_, &it)) {
            if (pkt->kind != VPX_CODEC_CX_FRAME_PKT) continue;
            const auto* bytes = static_cast<const uint8_t*>(pkt->data.frame.buf);
            packets.push_back({pkt->data.frame.pts, (pkt->data.frame.flags & VPX_FRAME_IS_KEY) != 0,
                               std::vector<uint8_t>(bytes, bytes + pkt->data.frame.sz)});
        }
        return packets.size() - before;
    }

    std::deque<Packet> packets;

private:
    template <typename T>
    void control(int id, T value) {
        if (vpx_codec_control_(&ctx_, id, value)) fail("setting");
    }
    [[noreturn]] void fail(const char* what) {
        const char* detail = open_ ? vpx_codec_error_detail(&ctx_) : nullptr;
        if (open_ && !detail) detail = vpx_codec_error(&ctx_);
        throw Error(Error::Kind::encoderFailed, std::string("VP9 ") + what + " failed" + (detail ? std::string(": ") + detail : std::string()));
    }

    vpx_codec_ctx_t ctx_{};
    bool open_ = false;
};

/// WebM with VP9 colour and a VP9 alpha stream in BlockAdditional (AlphaMode 1), the layout browsers and
/// ffmpeg read as transparent video.
class WebmSink : public FrameSink {
public:
    WebmSink(const fs::path& output, const Options& options, uint32_t width, uint32_t height)
        : output_(output), options_(options), w_(width), h_(height), color_(width, height, options, false),
          alpha_(width, height, options, true),
          colorImage_(vpx_img_alloc(nullptr, VPX_IMG_FMT_I420, width, height, 16), vpx_img_free),
          alphaImage_(vpx_img_alloc(nullptr, VPX_IMG_FMT_I420, width, height, 16), vpx_img_free), writer_(output) {
        if (!writer_.isOpen()) throw Error(Error::Kind::ioFailed, "Cannot create " + utf8(output));
        if (!colorImage_ || !alphaImage_) throw std::bad_alloc();
        colorImage_->cs = VPX_CS_BT_709;
        colorImage_->range = VPX_CR_STUDIO_RANGE;
        // The alpha stream codes alpha as luma; its chroma is a constant mid-grey.
        for (int plane : {VPX_PLANE_U, VPX_PLANE_V})
            for (uint32_t y = 0; y < (height + 1) / 2; ++y)
                std::memset(alphaImage_->planes[plane] + ptrdiff_t(y) * alphaImage_->stride[plane], 128, (width + 1) / 2);

        if (!segment_.Init(&writer_)) throw Error(Error::Kind::encoderFailed, "WebM muxer initialisation failed");
        segment_.set_mode(mkvmuxer::Segment::kFile);
        segment_.OutputCues(true);
        segment_.GetSegmentInfo()->set_writing_app("unVBN");
        track_ = segment_.AddVideoTrack(int(width), int(height), 0);
        auto* video = static_cast<mkvmuxer::VideoTrack*>(segment_.GetTrackByNumber(track_));
        if (!track_ || !video) throw Error(Error::Kind::encoderFailed, "WebM muxer: no video track");
        video->set_codec_id(mkvmuxer::Tracks::kVp9CodecId);
        if (!video->SetAlphaMode(mkvmuxer::VideoTrack::kAlpha)) throw Error(Error::Kind::encoderFailed, "WebM muxer: no alpha mode");
        video->set_frame_rate(options.fps);
        mkvmuxer::Colour colour;
        colour.set_matrix_coefficients(mkvmuxer::Colour::kBt709);
        colour.set_range(mkvmuxer::Colour::kBroadcastRange);
        colour.set_transfer_characteristics(mkvmuxer::Colour::kIturBt709Tc);
        colour.set_primaries(mkvmuxer::Colour::kIturBt709P);
        video->SetColour(colour);
        segment_.CuesTrack(track_);
    }

    bool acceptsConcurrentFrames() const override { return false; }

    void append(const vbn::Image& image, const Frame&) override {
        auto plane = [](const ImagePtr& img, int p) { return PlaneOut{img->planes[p], img->stride[p]}; };
        bgraToI420(image.bgra.data(), w_, h_, plane(colorImage_, VPX_PLANE_Y), plane(colorImage_, VPX_PLANE_U),
                   plane(colorImage_, VPX_PLANE_V));
        bgraAlphaPlane(image.bgra.data(), w_, h_, plane(alphaImage_, VPX_PLANE_Y));
        const bool key = frameIndex_ % kKeyframeInterval == 0;
        color_.encode(colorImage_.get(), frameIndex_, key);
        alpha_.encode(alphaImage_.get(), frameIndex_, key);
        ++frameIndex_;
        mux();
    }

    void finish() override {
        // Drain the encoders' look-ahead until neither has anything left.
        for (;;) {
            const size_t produced = color_.encode(nullptr, frameIndex_, false) + alpha_.encode(nullptr, frameIndex_, false);
            mux();
            if (!produced) break;
        }
        if (muxed_ != frameIndex_ || !color_.packets.empty() || !alpha_.packets.empty())
            throw Error(Error::Kind::encoderFailed, "VP9 colour and alpha streams are out of step");
        if (!segment_.Finalize() || !writer_.close()) throw Error(Error::Kind::ioFailed, "Cannot finish " + utf8(output_));
    }

    void abort() noexcept override {
        writer_.close();
        removeQuietly(output_);
    }

private:
    static constexpr int64_t kKeyframeInterval = 128;  // libvpx's own default distance

    /// Frame `i`'s start in nanoseconds.
    uint64_t time(int64_t i) const { return uint64_t(std::llround(double(i) * 1e9 / options_.fps)); }

    /// Writes every frame for which both the colour and the alpha packet are ready.
    void mux() {
        while (!color_.packets.empty() && !alpha_.packets.empty()) {
            const auto& c = color_.packets.front();
            const auto& a = alpha_.packets.front();
            if (c.pts != a.pts) throw Error(Error::Kind::encoderFailed, "VP9 colour and alpha streams are out of step");
            mkvmuxer::Frame frame;
            if (!frame.Init(c.data.data(), c.data.size()) || !frame.AddAdditionalData(a.data.data(), a.data.size(), 1))
                throw std::bad_alloc();
            frame.set_track_number(track_);
            frame.set_timestamp(time(c.pts));
            frame.set_duration(time(c.pts + 1) - time(c.pts));
            frame.set_is_key(c.key);
            if (!segment_.AddGenericFrame(&frame)) throw Error(Error::Kind::ioFailed, "Cannot write " + utf8(output_));
            color_.packets.pop_front();
            alpha_.packets.pop_front();
            ++muxed_;
        }
    }

    fs::path output_;
    Options options_;
    uint32_t w_, h_;
    Vp9Encoder color_, alpha_;
    using ImagePtr = std::unique_ptr<vpx_image_t, decltype(&vpx_img_free)>;
    ImagePtr colorImage_, alphaImage_;
    WideFileWriter writer_;
    mkvmuxer::Segment segment_;
    uint64_t track_ = 0;
    int64_t frameIndex_ = 0;
    int64_t muxed_ = 0;
};

}  // namespace

std::unique_ptr<FrameSink> makeSink(const Options& options, const fs::path& output, uint32_t width, uint32_t height) {
    switch (options.format) {
    case Format::png: return std::make_unique<PngSink>(output);
    case Format::mp4: return std::make_unique<Mp4Sink>(output, options, width, height);
    case Format::webm: return std::make_unique<WebmSink>(output, options, width, height);
    }
    throw Error(Error::Kind::encoderFailed, "Unknown format");
}

}  // namespace vbnx
