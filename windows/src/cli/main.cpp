// unvbn.exe: the command-line converter, same usage and output as the macOS `unvbn` (CLI/main.swift).
#include <windows.h>
#include <io.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "export/exporter.h"
#include "vbn_decode.h"

using namespace vbnx;

namespace {

const char* kUsage =
    "usage: unvbn <folder | frame.vbn>... [-f png|mp4|webm] [--fps 50] [-o OUTPUT] [--background #000000] [--crf 30]\n"
    "       unvbn info <folder | frame.vbn>...\n"
    "\n"
    "Converts Vizrt .vbn sequences (NAME_00000.vbn, NAME_00001.vbn, ...) to a PNG sequence, MP4 (H.264)\n"
    "or WebM (VP9 + alpha). A folder holding several sequences converts each one.\n"
    "\n"
    "  -f, --format      png (default; a folder of PNGs), mp4, webm\n"
    "  --fps             frames per second (default 50)\n"
    "  -o, --output      output file (or folder for png). Default: next to the sequence folder,\n"
    "                    NAME.mp4 / NAME.webm / NAME_png\\ (\"NAME (FOLDER)\" when several sequences\n"
    "                    share a name). Only with a single sequence.\n"
    "  --background      MP4 only (no alpha): colour behind transparent pixels (default #000000)\n"
    "  --crf             WebM quality, 0 (best) to 63 (default 30)\n";

/// Writes UTF-8 text: as UTF-16 to a console (so any name shows), as UTF-8 bytes to a pipe or file.
void write(FILE* stream, const std::string& text) {
    HANDLE h = GetStdHandle(stream == stdout ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
    DWORD mode;
    if (GetConsoleMode(h, &mode)) {
        const std::wstring w = fromUtf8(text).wstring();
        DWORD written;
        WriteConsoleW(h, w.data(), DWORD(w.size()), &written, nullptr);
    } else {
        std::fwrite(text.data(), 1, text.size(), stream);
        std::fflush(stream);
    }
}

void eprint(const std::string& s, const char* terminator = "\n") { write(stderr, s + terminator); }

[[noreturn]] void fail(const std::string& message, int code = 1) {
    eprint("unvbn: " + message);
    std::exit(code);
}

std::string format(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    return buf;
}

/// Rendering progress on stderr: a redrawn bar on a console, a line every 10 % when redirected.
///   [██████████████░░░░░░░░░░]  58%  435/750  92 fps  ETA 3 s
class ProgressBar {
public:
    ProgressBar() {
        HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
        DWORD mode;
        tty_ = GetConsoleMode(h, &mode) != 0;
        // Erase-to-end-of-line needs virtual terminal processing (Windows 10+); pad with spaces otherwise.
        vt_ = tty_ && SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }

    void update(const Progress& p) {
        if (!tty_ && p.percent() / 10 == lastDrawn_ / 10 && p.done != p.total) return;
        lastDrawn_ = p.percent();
        const int width = 24, filled = p.done * width / (p.total > 0 ? p.total : 1);
        std::string line = "[";
        for (int i = 0; i < width; ++i) line += i < filled ? "\xE2\x96\x88" : "\xE2\x96\x91";  // █ ░
        line += "]" + format(" %3d%%  %d/%d", p.percent(), p.done, p.total);
        const auto fps = p.framesPerSecond();
        const auto left = p.remaining();
        if (fps && left) line += format("  %.0f fps  ETA ", *fps) + Progress::formatDuration(*left);
        if (tty_) eprint("\r  " + line + clearToEnd(), "");
        else eprint("  " + line);
    }

    void finish(size_t frames, double seconds) {
        eprint((tty_ ? "\r" : "") + format("  done: %zu frames in %.1f s", frames, seconds) + (tty_ ? clearToEnd() : ""));
    }

    /// Ends a half-drawn bar line before an error message.
    void interrupt() {
        if (tty_ && lastDrawn_ >= 0) eprint("");
    }

private:
    std::string clearToEnd() const { return vt_ ? "\x1B[K" : std::string(12, ' '); }
    bool tty_ = false, vt_ = false;
    int lastDrawn_ = -1;
};

std::atomic<bool> interrupted{false};

BOOL WINAPI onCtrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        interrupted = true;  // the exporter stops between frames and removes the partial output
        return TRUE;
    }
    return FALSE;
}

std::string headerSummary(const fs::path& frame) {
    std::ifstream in(frame, std::ios::binary);
    uint8_t bytes[vbn::kHeaderSize];
    if (!in.read(reinterpret_cast<char*>(bytes), sizeof bytes)) return "";
    vbn::Header h;
    std::string error;
    if (!vbn::parseHeader(bytes, sizeof bytes, h, error)) return "";
    return ", " + std::to_string(h.width) + "\xC3\x97" + std::to_string(h.height) + (h.dxt5 ? " DXT5" : " RGBA") +
           (h.compressed ? " QuickLZ" : "");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(utf8(fs::path(argv[i])));
    auto has = [&](const char* a) { return std::find(args.begin(), args.end(), a) != args.end(); };
    if (args.empty() || has("-h") || has("--help") || has("/?")) {
        write(stdout, kUsage);
        return args.empty() ? 1 : 0;
    }

    const bool infoOnly = args[0] == "info";
    if (infoOnly) args.erase(args.begin());

    Options options;
    std::string outputPath;
    std::vector<std::string> inputs;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&]() -> std::string {
            if (++i >= args.size()) fail(a + " needs a value");
            return args[i];
        };
        if (a == "-f" || a == "--format") {
            const std::string v = value();
            const auto f = parseFormat(v);
            if (!f) fail("unknown format " + v + " (png, mp4, webm)");
            options.format = *f;
        } else if (a == "--fps") {
            const std::string v = value();
            char* end = nullptr;
            const double fps = std::strtod(v.c_str(), &end);
            if (end == v.c_str() || *end || !(fps > 0)) fail("invalid --fps " + v);
            options.fps = fps;
        } else if (a == "-o" || a == "--output") {
            outputPath = value();
        } else if (a == "--background") {
            const std::string v = value();
            const auto c = parseHexColor(v);
            if (!c) fail("invalid --background " + v + " (use #RRGGBB)");
            options.background = *c;
        } else if (a == "--crf") {
            const std::string v = value();
            char* end = nullptr;
            const long crf = std::strtol(v.c_str(), &end, 10);
            if (end == v.c_str() || *end || crf < 0 || crf > 63) fail("invalid --crf " + v + " (0-63)");
            options.webmCRF = int(crf);
        } else if (a.size() > 1 && a[0] == '-') {
            fail("unknown option " + a + "\n\n" + kUsage);
        } else {
            inputs.push_back(a);
        }
    }
    if (inputs.empty()) fail(std::string("no input\n\n") + kUsage);

    std::vector<Sequence> sequences;
    for (const auto& input : inputs) {
        try {
            auto found = findSequences(fromUtf8(input));
            sequences.insert(sequences.end(), found.begin(), found.end());
        } catch (const std::exception& e) {
            fail(e.what());
        }
    }

    if (infoOnly) {
        for (const auto& seq : sequences) {
            const size_t n = seq.frames.size();
            std::string line = seq.name + ": " + std::to_string(n) + " frame" + (n == 1 ? "" : "s") + " in " + utf8(seq.directory);
            line += headerSummary(seq.frames[0].path);
            const size_t missing = seq.missingNumbers().size();
            if (missing) line += ", " + std::to_string(missing) + " missing frame number(s)";
            write(stdout, line + "\n");
        }
        return 0;
    }

    if (!outputPath.empty() && sequences.size() > 1)
        fail("-o works with a single sequence; " + std::to_string(sequences.size()) + " were found");

    const auto outputs = outputPath.empty() ? defaultOutputs(options.format, sequences)
                                            : std::vector<fs::path>{fs::absolute(fromUtf8(outputPath))};
    std::vector<Job> jobs;
    for (size_t i = 0; i < sequences.size(); ++i) jobs.push_back({sequences[i], outputs[i]});

    SetConsoleCtrlHandler(onCtrl, TRUE);
    ProgressBar bar;
    auto start = std::chrono::steady_clock::now();
    const auto result = runBatch(jobs, options, [&](const BatchEvent& e) {
        const Job& job = jobs[e.index];
        switch (e.type) {
        case BatchEvent::Type::started: {
            const size_t missing = job.sequence.missingNumbers().size();
            if (missing)
                eprint("unvbn: warning: " + job.sequence.name + " has " + std::to_string(missing) +
                       " missing frame number(s); they are skipped");
            eprint(job.sequence.name + ": " + std::to_string(job.sequence.frames.size()) + " frames -> " + utf8(job.output));
            bar = ProgressBar();
            start = std::chrono::steady_clock::now();
            break;
        }
        case BatchEvent::Type::progress: bar.update(e.current); break;
        case BatchEvent::Type::finished:
            if (e.error.empty()) {
                bar.finish(job.sequence.frames.size(),
                           std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
            } else {
                bar.interrupt();
                eprint("unvbn: " + job.sequence.name + ": " + e.error);
            }
            break;
        }
    }, [] { return interrupted.load(); });
    if (result.cancelled) return 130;
    return result.failures.empty() ? 0 : 1;
}
