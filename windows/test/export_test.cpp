// Export tests, ported from Tests/VBNExportTests. The sequence, naming and batch tests are portable (they run
// on macOS too, with a recording sink); on Windows the real encoders are tested as well (export_test_win.cpp).
#include <algorithm>
#include <mutex>

#include "../src/export/yuv.h"
#include "export_test_util.h"

using namespace et;
using vbnx::Error;
using vbnx::Format;

#ifdef _WIN32
void runWindowsEncoderTests();
#endif

namespace {

/// Records what it is given; `abort` deletes the output marker so tests can see it ran.
struct RecordingSink : vbnx::FrameSink {
    struct Log {
        std::mutex lock;
        std::vector<std::string> frames;
        bool finished = false, aborted = false;
    };
    std::shared_ptr<Log> log;
    bool concurrent;
    RecordingSink(std::shared_ptr<Log> log, bool concurrent) : log(std::move(log)), concurrent(concurrent) {}
    bool acceptsConcurrentFrames() const override { return concurrent; }
    void append(const vbn::Image&, const vbnx::Frame& f) override {
        std::lock_guard<std::mutex> g(log->lock);
        log->frames.push_back(vbnx::utf8(f.path.filename()));
    }
    void finish() override { log->finished = true; }
    void abort() noexcept override { log->aborted = true; }
};

vbnx::SinkFactory recorder(std::shared_ptr<RecordingSink::Log> log, bool concurrent = false) {
    return [log, concurrent](const vbnx::Options&, const fs::path&, uint32_t, uint32_t) {
        return std::make_unique<RecordingSink>(log, concurrent);
    };
}

std::vector<std::string> names(const std::vector<fs::path>& paths) {
    std::vector<std::string> out;
    for (const auto& p : paths) out.push_back(vbnx::utf8(p.filename()));
    return out;
}

void testParseFrameNames() {
    auto p = vbnx::parseFrameName("NAME_00012.vbn");
    CHECK(p && p->prefix == "NAME" && p->number == 12, "NAME_00012");
    p = vbnx::parseFrameName("Logo-3.VBN");
    CHECK(p && p->prefix == "Logo" && p->number == 3, "Logo-3.VBN");
    p = vbnx::parseFrameName("0042.vbn");
    CHECK(p && p->prefix.empty() && p->number == 42, "0042.vbn");
    CHECK(!vbnx::parseFrameName("still.vbn"), "still.vbn has no number");
    CHECK(!vbnx::parseFrameName("NAME_00012.png"), "not a .vbn");
    CHECK(!vbnx::parseFrameName("NAME_0123456789.vbn"), "more than 9 digits");
}

void testNaturalOrder() {
    CHECK(vbnx::naturalLess("Logo 2", "Logo 10"), "2 < 10");
    CHECK(!vbnx::naturalLess("Logo 10", "Logo 2"), "10 > 2");
    CHECK(vbnx::naturalLess("alpha", "Beta"), "case-insensitive");
    CHECK(vbnx::naturalLess("a", "ab"), "prefix first");
}

void testFindGroupsSortsAndReportsGaps() {
    TempDir dir;
    const Bytes data = solidVBN(2, 2, {1, 2, 3, 4});
    for (int n : {10, 2, 1, 3, 5}) {
        char name[32];
        std::snprintf(name, sizeof name, "A_%05d.vbn", n);
        writeFile(dir.path / name, data);
    }
    writeFile(dir.path / "B_00000.vbn", data);
    writeFile(dir.path / "still.vbn", data);
    writeFile(dir.path / "A_00004.png", {'x'});
    writeFile(dir.path / "._A_00004.vbn", {'x'});  // AppleDouble file from a Mac: skipped

    const auto all = vbnx::findSequences(dir.path);
    CHECK(all.size() == 3 && all[0].name == "A" && all[1].name == "B" && all[2].name == "still", "sequence names");
    std::vector<long> numbers;
    for (const auto& f : all[0].frames) numbers.push_back(f.number.value_or(-1));
    CHECK((numbers == std::vector<long>{1, 2, 3, 5, 10}), "frames sorted by number");
    CHECK((all[0].missingNumbers() == std::vector<long>{4, 6, 7, 8, 9}), "missing numbers");
    CHECK(all[1].missingNumbers().empty(), "no gaps");

    const auto fromFrame = vbnx::findSequences(dir.path / "A_00003.vbn");
    CHECK(fromFrame.size() == 1 && fromFrame[0].frames.size() == 5, "a frame finds its whole sequence");
    const auto trailing = vbnx::findSequences(fs::path(vbnx::utf8(dir.path) + "/"));
    CHECK(trailing.size() == 3 && trailing[0].directory.filename() == dir.path.filename(), "trailing separator");
}

void testFindErrors() {
    TempDir dir;
    expectError(Error::Kind::notFound, [&] { vbnx::findSequences(dir.path / "nope"); }, "missing path");
    expectError(Error::Kind::noFrames, [&] { vbnx::findSequences(dir.path); }, "empty folder");
}

void testBatchOutputsNeverCollide() {
    auto seq = [](const std::string& name, const std::string& folder) {
        return vbnx::Sequence{name, fs::path("/in") / folder, {}};
    };
    const std::vector<vbnx::Sequence> sequences{seq("LOGO", "a"), seq("Intro", "b"), seq("logo", "c"), seq("LOGO", "a")};
    const fs::path out("/out");
    CHECK((names(vbnx::batchOutputs(Format::webm, sequences, out)) ==
           std::vector<std::string>{"LOGO (a).webm", "Intro.webm", "logo (c).webm", "LOGO (a) 2.webm"}),
          "batch names");
    CHECK(vbnx::batchOutputs(Format::png, {seq("Intro", "b")}, out)[0] == fs::path("/out") / "Intro_png", "png folder");
    // Next to each folder: only sequences that end up in the same place are told apart.
    const vbnx::Sequence elsewhere{"LOGO", fs::path("/other") / "d", {}};
    const auto outputs = vbnx::defaultOutputs(Format::mp4, {seq("LOGO", "a"), elsewhere, seq("LOGO", "b")});
    CHECK(outputs.size() == 3 && outputs[0] == fs::path("/in") / "LOGO (a).mp4" && outputs[1] == fs::path("/other") / "LOGO.mp4" &&
              outputs[2] == fs::path("/in") / "LOGO (b).mp4",
          "default outputs");
}

void testOptionsHelpers() {
    CHECK((vbnx::parseHexColor("#FF8000") == vbnx::RGB{255, 128, 0}), "#FF8000");
    CHECK((vbnx::parseHexColor("00ff00") == vbnx::RGB{0, 255, 0}), "00ff00");
    CHECK(!vbnx::parseHexColor("#12345"), "short hex");
    CHECK(!vbnx::parseHexColor("#12345g"), "bad hex digit");
    CHECK(vbnx::fpsString(50) == "50" && vbnx::fpsString(29.97) == "29.97" && vbnx::fpsString(23.976) == "23.976", "fps");
    CHECK(vbnx::Progress::formatDuration(2.1) == "3 s" && vbnx::Progress::formatDuration(65) == "1 min 05 s", "duration");
    CHECK((vbnx::parseFormat("WebM") == Format::webm) && !vbnx::parseFormat("mov"), "format ids");
}

void testExportFeedsEveryFrameInOrder() {
    TempDir dir;
    const auto seq = makeSequence(dir.path, "LOGO", 23);
    for (bool concurrent : {false, true}) {
        auto log = std::make_shared<RecordingSink::Log>();
        std::vector<int> progress;
        vbnx::exportSequence(seq, {}, dir.path / "out", [&](const vbnx::Progress& p) { progress.push_back(p.done); },
                             nullptr, recorder(log, concurrent));
        std::vector<std::string> expected;
        for (const auto& f : seq.frames) expected.push_back(vbnx::utf8(f.path.filename()));
        auto got = log->frames;
        if (concurrent) std::sort(got.begin(), got.end());
        CHECK(got == expected, concurrent ? "all frames (concurrent)" : "frames in order");
        CHECK(log->finished && !log->aborted, "finished");
        CHECK(!progress.empty() && progress.back() == 23 && std::is_sorted(progress.begin(), progress.end()), "progress");
    }
}

void testSizeMismatchAborts() {
    TempDir dir;
    const auto seq = makeSequence(dir.path, "LOGO", 3, 8, 8);
    writeFile(seq.frames[2].path, solidVBN(4, 4, {0, 0, 0, 255}));
    auto log = std::make_shared<RecordingSink::Log>();
    const std::string message = expectError(Error::Kind::sizeMismatch, [&] {
        vbnx::exportSequence(seq, {}, dir.path / "p", nullptr, nullptr, recorder(log));
    }, "size mismatch");
    CHECK(message == "LOGO_00002.vbn is 4\xC3\x97" "4, but the sequence is 8\xC3\x97" "8. All frames must have the same size.",
          "size mismatch message");
    CHECK(log->aborted && !log->finished, "aborted");
}

void testInvalidFPS() {
    TempDir dir;
    const auto seq = makeSequence(dir.path, "LOGO", 1);
    vbnx::Options options;
    options.fps = 0;
    auto log = std::make_shared<RecordingSink::Log>();
    expectError(Error::Kind::invalidFPS, [&] { vbnx::exportSequence(seq, options, dir.path / "p", nullptr, nullptr, recorder(log)); },
                "fps 0");
}

void testCancelAborts() {
    TempDir dir;
    const auto seq = makeSequence(dir.path, "LOGO", 30);
    auto log = std::make_shared<RecordingSink::Log>();
    int done = 0;
    expectError(Error::Kind::cancelled, [&] {
        vbnx::exportSequence(seq, {}, dir.path / "p", [&](const vbnx::Progress& p) { done = p.done; },
                             [&] { return done >= 3; }, recorder(log));
    }, "cancel");
    CHECK(log->aborted && !log->finished, "cancel aborts the sink");
}

void testBatchContinuesAfterFailureAndCountsAllFrames() {
    TempDir dir;
    const auto a = makeSequence(dir.path, "A", 4), bad = makeSequence(dir.path, "BAD", 3), c = makeSequence(dir.path, "C", 5);
    writeFile(bad.frames[1].path, {'n', 'o', 't'});
    std::vector<vbnx::Job> jobs;
    for (const auto& s : {a, bad, c}) jobs.push_back({s, dir.path / vbnx::fromUtf8(s.name)});
    std::vector<std::string> events;
    std::vector<int> overall;
    auto log = std::make_shared<RecordingSink::Log>();
    const auto result = vbnx::runBatch(jobs, {}, [&](const vbnx::BatchEvent& e) {
        switch (e.type) {
        case vbnx::BatchEvent::Type::started: events.push_back("start " + std::to_string(e.index)); break;
        case vbnx::BatchEvent::Type::progress:
            overall.push_back(e.overall.done);
            CHECK(e.overall.total == 12, "overall total");
            break;
        case vbnx::BatchEvent::Type::finished:
            events.push_back("end " + std::to_string(e.index) + (e.error.empty() ? "" : " failed"));
            break;
        }
    }, nullptr, recorder(log));
    CHECK((events == std::vector<std::string>{"start 0", "end 0", "start 1", "end 1 failed", "start 2", "end 2"}), "events");
    CHECK((result.exported == std::vector<fs::path>{jobs[0].output, jobs[2].output}), "exported");
    CHECK(result.failures.size() == 1 && result.failures[0].first == 1, "failure recorded");
    CHECK(!result.cancelled, "not cancelled");
    CHECK(std::is_sorted(overall.begin(), overall.end()) && overall.back() == 12, "batch progress never goes back");
}

void testBatchCancelStopsTheRest() {
    TempDir dir;
    const auto a = makeSequence(dir.path, "A", 4), b = makeSequence(dir.path, "B", 30);
    const std::vector<vbnx::Job> jobs{{a, dir.path / "A"}, {b, dir.path / "B"}};
    int done = 0;
    auto log = std::make_shared<RecordingSink::Log>();
    const auto result = vbnx::runBatch(jobs, {}, [&](const vbnx::BatchEvent& e) {
        if (e.type == vbnx::BatchEvent::Type::progress) done = e.overall.done;
    }, [&] { return done >= 8; }, recorder(log));
    CHECK(result.cancelled, "cancelled");
    CHECK((result.exported == std::vector<fs::path>{jobs[0].output}), "first job kept");
    CHECK(result.failures.empty(), "cancel is not a failure");
}


void testYuv709() {
    uint8_t y, cb, cr;
    vbnx::rgbToYCbCr709(0, 0, 0, y, cb, cr);
    CHECK(y == 16 && cb == 128 && cr == 128, "black");
    vbnx::rgbToYCbCr709(255, 255, 255, y, cb, cr);
    CHECK(y == 235 && cb == 128 && cr == 128, "white");
    vbnx::rgbToYCbCr709(128, 128, 128, y, cb, cr);
    CHECK(cb == 128 && cr == 128, "grey has no chroma");
    vbnx::rgbToYCbCr709(255, 0, 0, y, cb, cr);
    CHECK(y == 63 && cb == 102 && cr == 240, "BT.709 red");
    vbnx::rgbToYCbCr709(0, 0, 255, y, cb, cr);
    CHECK(y == 32 && cb == 240 && cr == 118, "BT.709 blue");

    // 3x3 odd size: chroma is 2x2, edges clamped; alpha plane is the raw alpha.
    std::vector<uint8_t> bgra;
    for (int i = 0; i < 9; ++i) bgra.insert(bgra.end(), {0, 0, 255, uint8_t(i * 30)});  // red, rising alpha
    uint8_t Y[9], U[4], V[4], A[9];
    vbnx::bgraToI420(bgra.data(), 3, 3, {Y, 3}, {U, 2}, {V, 2});
    CHECK(std::all_of(Y, Y + 9, [](uint8_t v) { return v == 63; }) && std::all_of(U, U + 4, [](uint8_t v) { return v == 102; }) &&
              std::all_of(V, V + 4, [](uint8_t v) { return v == 240; }),
          "I420 keeps straight colour");
    vbnx::bgraAlphaPlane(bgra.data(), 3, 3, {A, 3});
    CHECK(A[0] == 0 && A[4] == 120 && A[8] == 240, "alpha plane");

    // Flatten half-transparent red over white, padded from 3x3 to 4x4.
    std::vector<uint8_t> flat, one;
    vbnx::flattenOnto(std::vector<uint8_t>(36, 0).data(), 3, 3, 255, 255, 255, 4, 4, flat);
    CHECK(flat.size() == 64 && flat[0] == 255 && flat[3] == 255 && flat[63] == 255, "transparent -> background");
    const uint8_t halfRed[4] = {0, 0, 255, 128};
    vbnx::flattenOnto(halfRed, 1, 1, 255, 255, 255, 2, 2, one);
    CHECK(one[0] == 127 && one[1] == 127 && one[2] == 255 && one[4] == 255 && one[7] == 255, "composite and padding");
    uint8_t ny[4], nuv[2];
    vbnx::bgraToNV12(flat.data(), 2, 2, {ny, 2}, {nuv, 2});
    CHECK(ny[0] == 235 && nuv[0] == 128 && nuv[1] == 128, "NV12 white");

    // A tall frame is converted in bands of rows, one per core: each row and each chroma row (including the
    // clamped last one of an odd height) must come out as if converted in one piece.
    const uint32_t W = 3, H = 1001;
    std::vector<uint8_t> tall;
    for (uint32_t r = 0; r < H; ++r)
        for (uint32_t x = 0; x < W; ++x) tall.insert(tall.end(), {uint8_t(r * 7), uint8_t(r * 3), uint8_t(r), uint8_t(r)});
    std::vector<uint8_t> ty(W * H), tu(2 * (H + 1) / 2), tv(2 * (H + 1) / 2), ta(W * H);
    vbnx::bgraToI420(tall.data(), W, H, {ty.data(), W}, {tu.data(), 2}, {tv.data(), 2});
    vbnx::bgraAlphaPlane(tall.data(), W, H, {ta.data(), W});
    bool rowsOK = true;
    for (uint32_t r = 0; r < H; ++r) {
        uint8_t ey, ecb, ecr;
        vbnx::rgbToYCbCr709(uint8_t(r), uint8_t(r * 3), uint8_t(r * 7), ey, ecb, ecr);
        rowsOK &= ty[r * W + 2] == ey && ta[r * W + 1] == uint8_t(r);
        if (r % 2 == 0) {
            const uint32_t r1 = std::min(r + 1, H - 1);
            auto avg = [&](unsigned m) { return (2 * uint8_t(r * m) + 2 * uint8_t(r1 * m) + 2) / 4; };
            vbnx::rgbToYCbCr709(avg(1), avg(3), avg(7), ey, ecb, ecr);
            rowsOK &= tu[r / 2 * 2 + 1] == ecb && tv[r / 2 * 2 + 1] == ecr;
        }
    }
    CHECK(rowsOK, "banded conversion matches row by row");
}

}  // namespace

/// `export_test fixture DIR`: writes two small sequences (LOGO, 12 frames; Intro, 5 frames) into DIR, for
/// the CLI smoke test in CI.
int writeFixture(const char* dir) {
    // Narrow argv is in the system code page on Windows, which is what fs::path(const char*) expects.
    const fs::path root(dir);
    writeFrames(root, "LOGO", 12, 32, 18, [](int i) { return RGBA{uint8_t(i * 20), 100, 50, 200}; });
    writeFrames(root, "Intro", 5, 16, 16, [](int) { return RGBA{0, 0, 255, 255}; });
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "fixture") return writeFixture(argv[2]);
    testParseFrameNames();
    testNaturalOrder();
    testFindGroupsSortsAndReportsGaps();
    testFindErrors();
    testBatchOutputsNeverCollide();
    testOptionsHelpers();
    testYuv709();
    testExportFeedsEveryFrameInOrder();
    testSizeMismatchAborts();
    testInvalidFPS();
    testCancelAborts();
    testBatchContinuesAfterFailureAndCountsAllFrames();
    testBatchCancelStopsTheRest();
#ifdef _WIN32
    runWindowsEncoderTests();
#endif
    std::printf(failures ? "%d failure(s)\n" : "all export tests passed\n", failures);
    return failures ? 1 : 0;
}
