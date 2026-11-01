#include "exporter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
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

std::string sizeText(uint32_t w, uint32_t h) { return std::to_string(w) + "\xC3\x97" + std::to_string(h); }  // ×

vbn::Image decodeFrame(const Frame& frame) {
    const std::string name = utf8(frame.path.filename());
    std::ifstream in(frame.path, std::ios::binary);
    if (!in) throw Error(Error::Kind::decodeFailed, "Could not read " + name + ": cannot open the file");
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    vbn::Image image;
    std::string error;
    if (!vbn::decode(data.data(), data.size(), image, error))
        throw Error(Error::Kind::decodeFailed, "Could not read " + name + ": " + error);
    return image;
}

}  // namespace

void exportSequence(const Sequence& sequence, const Options& options, const fs::path& output,
                    const ProgressFn& progress, const CancelledFn& cancelled, const SinkFactory& sinkFactory) {
    if (!std::isfinite(options.fps) || options.fps <= 0 || options.fps > 1000)
        throw Error(Error::Kind::invalidFPS, "Invalid frame rate: " + fpsString(options.fps) + ".");
    const auto& frames = sequence.frames;
    if (frames.empty()) throw Error::noFrames(sequence.directory);
    auto isCancelled = [&] { return cancelled && cancelled(); };

    const auto start = Clock::now();
    vbn::Image first = decodeFrame(frames[0]);
    const uint32_t width = first.width, height = first.height;
    std::unique_ptr<FrameSink> sink = sinkFactory(options, output, width, height);
    const bool concurrent = sink->acceptsConcurrentFrames();

    const int total = int(frames.size());
    int done = 0, lastPercent = -1;
    auto lastTime = start - std::chrono::hours(1);
    auto report = [&] {
        if (!progress) return;
        const auto now = Clock::now();
        const Progress p{done, total, std::chrono::duration<double>(now - start).count()};
        if (p.percent() == lastPercent && now - lastTime < std::chrono::milliseconds(33) && done != total) return;
        lastPercent = p.percent();
        lastTime = now;
        progress(p);
    };

    try {
        const size_t batchSize = std::max(2u, std::thread::hardware_concurrency());
        for (size_t offset = 0; offset < frames.size();) {
            if (isCancelled()) throw Error::cancelled();
            const size_t count = std::min(batchSize, frames.size() - offset);
            // Each slot holds the decoded image for an in-order sink (empty once a concurrent sink took it)
            // or the error that frame raised.
            std::vector<vbn::Image> images(count);
            std::vector<std::exception_ptr> errors(count);
            auto work = [&](size_t i) {
                try {
                    const size_t index = offset + i;
                    vbn::Image image = index == 0 ? std::move(first) : decodeFrame(frames[index]);
                    if (image.width != width || image.height != height)
                        throw Error(Error::Kind::sizeMismatch,
                                    utf8(frames[index].path.filename()) + " is " + sizeText(image.width, image.height) +
                                        ", but the sequence is " + sizeText(width, height) +
                                        ". All frames must have the same size.");
                    if (concurrent) sink->append(image, frames[index]);
                    else images[i] = std::move(image);
                } catch (...) {
                    errors[i] = std::current_exception();
                }
            };
            std::vector<std::thread> threads;
            for (size_t i = 1; i < count; ++i) threads.emplace_back(work, i);
            work(0);
            for (auto& t : threads) t.join();

            for (size_t i = 0; i < count; ++i) {
                if (isCancelled()) throw Error::cancelled();
                if (errors[i]) std::rethrow_exception(errors[i]);
                if (!concurrent) {
                    sink->append(images[i], frames[offset + i]);
                    images[i] = {};
                }
                ++done;
                report();
            }
            offset += count;
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
        const int offset = framesBefore;
        try {
            exportSequence(job.sequence, options, job.output, [&](const Progress& p) {
                emit({BatchEvent::Type::progress, index, Progress{offset + p.done, total, secondsSince(start)}, p, {}});
            }, cancelled, sinkFactory);
            result.exported.push_back(job.output);
            emit({BatchEvent::Type::finished, index, {}, {}, {}});
        } catch (const Error& e) {
            if (e.kind == Error::Kind::cancelled) {
                result.cancelled = true;
                emit({BatchEvent::Type::finished, index, {}, {}, e.what(), true});
                break;
            }
            result.failures.push_back({index, e.what()});
            emit({BatchEvent::Type::finished, index, {}, {}, e.what()});
        } catch (const std::exception& e) {
            result.failures.push_back({index, e.what()});
            emit({BatchEvent::Type::finished, index, {}, {}, e.what()});
        }
        framesBefore += int(job.sequence.frames.size());
    }
    return result;
}

}  // namespace vbnx
