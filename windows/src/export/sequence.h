// .vbn frame sequences and export naming. Port of Sources/VBNExport/FrameSequence.swift and the naming
// half of ExportOptions.swift. Portable C++17 (std::filesystem): also built and tested on macOS.
#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace vbnx {

namespace fs = std::filesystem;

/// A path as UTF-8, for messages and names (paths stay wide on Windows).
std::string utf8(const fs::path& p);
fs::path fromUtf8(const std::string& s);

class Error : public std::runtime_error {
public:
    enum class Kind { notFound, noFrames, sizeMismatch, decodeFailed, invalidFPS, encoderFailed, ioFailed, cancelled };
    Error(Kind kind, const std::string& message) : std::runtime_error(message), kind(kind) {}
    Kind kind;

    static Error notFound(const fs::path& p) { return {Kind::notFound, utf8(p) + " does not exist."}; }
    static Error noFrames(const fs::path& p) { return {Kind::noFrames, "No .vbn frames found at " + utf8(p) + "."}; }
    static Error cancelled() { return {Kind::cancelled, "Export cancelled."}; }
};

struct Frame {
    fs::path path;
    /// Frame number parsed from the file name (none for a file without trailing digits).
    std::optional<long> number;
};

/// An ordered run of `.vbn` frames sharing a name prefix: `NAME_00000.vbn` ... `NAME_00749.vbn`.
struct Sequence {
    /// Prefix shared by the frames, without the trailing separator (`NAME` for `NAME_00012.vbn`). UTF-8.
    std::string name;
    /// Folder holding the frames.
    fs::path directory;
    std::vector<Frame> frames;

    /// Frame numbers absent between the first and last frame.
    std::vector<long> missingNumbers() const;
};

/// Every sequence found at `path`: all sequences in a folder (not recursive), or, for a `.vbn` file, the
/// sequence it belongs to. Sequences are sorted by name (natural order), frames by number. Throws Error.
std::vector<Sequence> findSequences(const fs::path& path);

struct ParsedName {
    std::string prefix;
    long number;
};
/// `NAME_00012.vbn` -> ("NAME", 12). One separator `_`, `-`, `.` or space before the digits is dropped.
std::optional<ParsedName> parseFrameName(const std::string& fileName);

/// Case-insensitive natural order ("Logo 2" < "Logo 10"), like Finder and Explorer.
bool naturalLess(const std::string& a, const std::string& b);

enum class Format { png, mp4, webm };

/// "png", "mp4", "webm".
const char* formatId(Format f);
std::optional<Format> parseFormat(const std::string& s);
/// "PNG sequence", "MP4 (H.264)", "WebM (VP9 + alpha)".
const char* formatName(Format f);
/// "mp4", "webm"; nullptr for PNG, whose output is a folder.
const char* formatExtension(Format f);

/// `folder/NAME.ext`, or the folder `folder/NAME_png` for PNG.
fs::path outputNamed(Format f, const std::string& name, const fs::path& folder);
/// Default output next to the sequence folder: `NAME.mp4`, or the folder `NAME_png`.
fs::path defaultOutput(Format f, const Sequence& s);
/// Outputs for exporting several sequences into one folder, in the same order. Each is named after its
/// sequence; a name shared by several sequences gets its folder added, `NAME (FOLDER)`, then ` 2`, ` 3`...
/// if that is still taken, so no export overwrites another.
std::vector<fs::path> batchOutputs(Format f, const std::vector<Sequence>& sequences, const fs::path& folder);
/// Default outputs for several sequences: each next to its own folder, with shared names told apart as in
/// `batchOutputs`.
std::vector<fs::path> defaultOutputs(Format f, const std::vector<Sequence>& sequences);

}  // namespace vbnx
