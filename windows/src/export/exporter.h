// Sequence export: options, progress, the per-sequence exporter and the batch runner. Port of
// Sources/VBNExport (SequenceExporter, BatchExporter). Portable; the encoders (sinks) are platform code,
// see sinks.h.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sequence.h"
#include "vbn_decode.h"

namespace vbnx {

struct RGB {
    uint8_t r = 0, g = 0, b = 0;
    bool operator==(const RGB& o) const { return r == o.r && g == o.g && b == o.b; }
};
/// `#RRGGBB` or `RRGGBB`.
std::optional<RGB> parseHexColor(const std::string& s);

struct Options {
    Format format = Format::png;
    /// Frames per second. 50 matches 1080i50 / 1080p50 broadcast graphics.
    double fps = 50;
    /// Background for formats without alpha (MP4).
    RGB background;
    /// VP9 constant quality (0 best ... 63 worst) for WebM.
    int webmCRF = 30;
};

/// 50 -> "50", 29.97 -> "29.97", 23.976 -> "23.976".
std::string fpsString(double fps);

/// Where an export stands, with the rate and time-left estimate both front ends display.
struct Progress {
    int done = 0, total = 0;
    double elapsed = 0;  // seconds

    double fraction() const { return double(done) / double(total > 0 ? total : 1); }
    int percent() const { return done * 100 / (total > 0 ? total : 1); }
    /// Frames per second so far; none for the first 0.3 s, when the figure is still noise.
    std::optional<double> framesPerSecond() const;
    std::optional<double> remaining() const;
    /// "3 s", "1 min 05 s".
    static std::string formatDuration(double seconds);
};

/// Receives decoded frames and writes them out.
class FrameSink {
public:
    virtual ~FrameSink() = default;
    /// True when `append` may be called from several threads at once, in any order.
    virtual bool acceptsConcurrentFrames() const = 0;
    virtual void append(const vbn::Image& image, const Frame& frame) = 0;
    virtual void finish() = 0;
    /// Stops and removes partial output. Never throws.
    virtual void abort() noexcept = 0;
};

using SinkFactory =
    std::function<std::unique_ptr<FrameSink>(const Options&, const fs::path& output, uint32_t width, uint32_t height)>;

/// The platform's encoders (sinks_win.cpp).
std::unique_ptr<FrameSink> makeSink(const Options& options, const fs::path& output, uint32_t width, uint32_t height);

using ProgressFn = std::function<void(const Progress&)>;
using CancelledFn = std::function<bool()>;

/// Converts a sequence to a PNG sequence, MP4 or WebM. Frames are decoded in parallel batches; PNGs are
/// also encoded in parallel, video frames are handed to the encoder in order. Synchronous: call it off the
/// UI thread. `progress` is called on the calling thread at most once per percent or every 1/30 s, and
/// always for the last frame; `cancelled` is polled between frames. Throws Error.
void exportSequence(const Sequence& sequence, const Options& options, const fs::path& output,
                    const ProgressFn& progress = nullptr, const CancelledFn& cancelled = nullptr,
                    const SinkFactory& sinkFactory = makeSink);

struct Job {
    Sequence sequence;
    fs::path output;
};

struct BatchEvent {
    enum class Type { started, progress, finished } type;
    size_t index;
    /// progress: frames of the whole batch, so a bar, rate and time left never jump back.
    Progress overall;
    /// progress: frames of the running job.
    Progress current;
    /// finished: empty when the job succeeded.
    std::string error;
    bool cancelled = false;
};

struct BatchResult {
    std::vector<fs::path> exported;
    std::vector<std::pair<size_t, std::string>> failures;  // job index, message
    bool cancelled = false;
};

/// Exports several sequences one after the other with the same options. A sequence that fails is recorded
/// and the next one runs; cancelling stops the batch. Synchronous, like exportSequence.
BatchResult runBatch(const std::vector<Job>& jobs, const Options& options,
                     const std::function<void(const BatchEvent&)>& handle = nullptr,
                     const CancelledFn& cancelled = nullptr, const SinkFactory& sinkFactory = makeSink);

}  // namespace vbnx
