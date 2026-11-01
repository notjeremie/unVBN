#include "exporter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <fstream>
#include <future>
#include <thread>

namespace vbnx {

std::optional<RGB> parseHexColor(const std::string& s) {
    const std::string hex = !s.empty() && s[0] == '#' ? s.substr(1) : s;
    if (hex.size() != 6) return std::nullopt;
    unsigned v = 0;
    for (char c : hex) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return std::nullopt;
        v = v << 4 | unsigned(d);
    }
    return RGB{uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
}

std::string fpsString(double fps) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", fps);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

bool validFps(double fps) { return std::isfinite(fps) && fps > 0 && fps <= 1000; }

std::string sizeText(uint32_t w, uint32_t h) { return std::to_string(w) + "\xC3\x97" + std::to_string(h); }  // ×

std::optional<vbn::Header> readHeader(const fs::path& frame) {
    std::ifstream in(frame, std::ios::binary);
    uint8_t bytes[vbn::kHeaderSize];
    vbn::Header h;
    std::string error;
    if (!in.read(reinterpret_cast<char*>(bytes), sizeof bytes) || !vbn::parseHeader(bytes, sizeof bytes, h, error))
        return std::nullopt;
    return h;
}

std::string describe(const vbn::Header& h) { return sizeText(h.width, h.height) + (h.dxt5 ? " DXT5" : " RGBA"); }

std::optional<double> Progress::framesPerSecond() const {
    if (done > 0 && elapsed > 0.3) return done / elapsed;
    return std::nullopt;
}

std::optional<double> Progress::remaining() const {
    if (auto fps = framesPerSecond()) return (total - done) / *fps;
    return std::nullopt;
}

std::string Progress::formatDuration(double seconds) {
    const long s = long(std::ceil(seconds));
    char buf[32];
    if (s < 60) std::snprintf(buf, sizeof buf, "%ld s", s);
    else std::snprintf(buf, sizeof buf, "%ld min %02ld s", s / 60, s % 60);
    return buf;
}

namespace {

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }

vbn::Image decodeFrame(const Frame& frame) {
    const std::string name = utf8(frame.path.filename());
    std::ifstream in(frame.path, std::ios::binary | std::ios::ate);
    if (!in) throw Error(Error::Kind::decodeFailed, "Could not read " + name + ": cannot open the file");
    std::vector<uint8_t> data(size_t(in.tellg()));
    if (!in.seekg(0).read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size())))
        throw Error(Error::Kind::decodeFailed, "Could not read " + name + ": read error");
    vbn::Image image;
    std::string error;
    if (!vbn::decode(data.data(), data.size(), image, error))
        throw Error(Error::Kind::decodeFailed, "Could not read " + name + ": " + error);
    return image;
}

}  // namespace

void exportSequence(const Sequence& sequence, const Options& options, const fs::path& output,
                    const ProgressFn& progress, const CancelledFn& cancelled, const SinkFactory& sinkFactory) {
    if (!validFps(options.fps)) throw Error(Error::Kind::invalidFPS, "Invalid frame rate: " + fpsString(options.fps) + ".");
    const auto& frames = sequence.frames;
    if (frames.empty()) throw Error::noFrames(sequence.directory);
    auto isCancelled = [&] { return cancelled && cancelled(); };

    const auto start = Clock::now();
    vbn::Image first = decodeFrame(frames[0]);
    const uint32_t width = first.width, height = first.height;
    std::unique_ptr<FrameSink> sink = sinkFactory(options, output, width, height);
    const bool concurrent = sink->acceptsConcurrentFrames();

    // Frames decoded at once: one per core, within about 1 GB of decoded pixels (a 16384x2160 frame is
    // 141 MB). In-order sinks decode the next batch while the encoder takes this one: two batches in memory.
    const size_t frameBytes = std::max<size_t>(1, size_t(width) * height * 4);
    const size_t budget = (size_t(1) << 30) / (concurrent ? 1 : 2);
    const size_t batchSize = std::clamp<size_t>(budget / frameBytes, 1, std::max(2u, std::thread::hardware_concurrency()));

    // Each slot holds the decoded image for an in-order sink (empty once a concurrent sink took it) or the
    // error that frame raised.
    struct Batch {
        std::vector<vbn::Image> images;
        std::vector<std::exception_ptr> errors;
    };
    auto decodeBatch = [&](size_t offset) {
        const size_t count = std::min(batchSize, frames.size() - offset);
        Batch b{std::vector<vbn::Image>(count), std::vector<std::exception_ptr>(count)};
        auto work = [&](size_t i) {
            try {
                const size_t index = offset + i;
                vbn::Image image = index == 0 ? std::move(first) : decodeFrame(frames[index]);
                if (image.width != width || image.height != height)
                    throw Error(Error::Kind::sizeMismatch,
                                utf8(frames[index].path.filename()) + " is " + sizeText(image.width, image.height) +
                                    ", but the sequence is " + sizeText(width, height) + ". All frames must have the same size.");
                if (concurrent) sink->append(image, frames[index]);
                else b.images[i] = std::move(image);
            } catch (...) {
                b.errors[i] = std::current_exception();
            }
        };
        std::vector<std::thread> threads;
        for (size_t i = 1; i < count; ++i) threads.emplace_back(work, i);
        work(0);
        for (auto& t : threads) t.join();
        return b;
    };

    const int total = int(frames.size());
    int done = 0, lastPercent = -1;
    auto lastTime = start - std::chrono::hours(1);
    auto report = [&] {
        if (!progress) return;
        const Progress p{done, total, secondsSince(start)};
        const auto now = Clock::now();
        if (p.percent() == lastPercent && now - lastTime < std::chrono::milliseconds(33) && done != total) return;
        lastPercent = p.percent();
        lastTime = now;
        progress(p);
    };

    try {
        std::future<Batch> ahead;  // the next batch, decoding while this one is encoded
        for (size_t offset = 0; offset < frames.size();) {
            if (isCancelled()) throw Error::cancelled();
            Batch batch = ahead.valid() ? ahead.get() : decodeBatch(offset);
            const size_t next = offset + batch.images.size();
            if (!concurrent && next < frames.size()) ahead = std::async(std::launch::async, decodeBatch, next);
            for (size_t i = 0; i < batch.images.size(); ++i) {
                if (isCancelled()) throw Error::cancelled();
                if (batch.errors[i]) std::rethrow_exception(batch.errors[i]);
                if (!concurrent) {
                    sink->append(batch.images[i], frames[offset + i]);
                    batch.images[i] = {};
                }
                ++done;
                report();
            }
            offset = next;
        }
        sink->finish();
    } catch (...) {
        sink->abort();
        throw;
    }
}

BatchResult runBatch(const std::vector<Job>& jobs, const Options& options,
                     const std::function<void(const BatchEvent&)>& handle, const CancelledFn& cancelled,
                     const SinkFactory& sinkFactory) {
    int total = 0;
    for (const auto& job : jobs) total += int(job.sequence.frames.size());
    const auto start = Clock::now();
    auto emit = [&](const BatchEvent& e) { if (handle) handle(e); };

    BatchResult result;
    int framesBefore = 0;
    for (size_t index = 0; index < jobs.size(); ++index) {
        if (cancelled && cancelled()) { result.cancelled = true; break; }
        const Job& job = jobs[index];
        emit({BatchEvent::Type::started, index, {}, {}, {}});
        const int offset = framesBefore, frames = int(job.sequence.frames.size());
        const auto jobStart = Clock::now();
        int done = 0;
        try {
            exportSequence(job.sequence, options, job.output, [&](const Progress& p) {
                done = p.done;
                emit({BatchEvent::Type::progress, index, Progress{offset + p.done, total, secondsSince(start)}, p, {}});
            }, cancelled, sinkFactory);
            result.exported.push_back(job.output);
            emit({BatchEvent::Type::finished, index, {}, Progress{frames, frames, secondsSince(jobStart)}, {}});
        } catch (const std::exception& e) {
            const auto* error = dynamic_cast<const Error*>(&e);
            const bool stopped = error && error->kind == Error::Kind::cancelled;
            emit({BatchEvent::Type::finished, index, {}, Progress{done, frames, secondsSince(jobStart)}, e.what(), stopped});
            if (stopped) {
                result.cancelled = true;
                break;
            }
            result.failures.push_back({index, e.what()});
        }
        framesBefore += frames;
    }
    result.seconds = secondsSince(start);
    return result;
}

}  // namespace vbnx
