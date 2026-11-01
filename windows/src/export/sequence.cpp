#include "sequence.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace vbnx {

std::string utf8(const fs::path& p) { return p.u8string(); }
fs::path fromUtf8(const std::string& s) { return fs::u8path(s); }

namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

std::string lowered(std::string s) {
    for (char& c : s) c = lower(c);
    return s;
}

/// `C:\a\seq\` and `C:\a\.\seq` -> `C:\a\seq`: absolute, normalised, no trailing separator, so
/// `parent_path()` and `filename()` mean the parent folder and the folder's own name.
fs::path clean(const fs::path& p) {
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    if (ec) abs = p;
    abs = abs.lexically_normal();
    if (abs.filename().empty() && abs.has_parent_path() && abs.parent_path() != abs) abs = abs.parent_path();
    return abs;
}

bool hasVBNExtension(const fs::path& p) { return lowered(utf8(p.extension())) == ".vbn"; }

std::vector<Sequence> group(const fs::path& dir) {
    std::map<std::string, std::vector<Frame>> groups;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& file = it->path();
        const std::string name = utf8(file.filename());
        // Hidden files, including the `._NAME.vbn` AppleDouble files Macs leave on shared drives.
        if (name.empty() || name[0] == '.' || !hasVBNExtension(file)) continue;
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc)) continue;
        const auto parsed = parseFrameName(name);
        const std::string key = parsed ? parsed->prefix : utf8(file.stem());
        groups[key].push_back({file, parsed ? std::optional<long>(parsed->number) : std::nullopt});
    }
    if (ec) throw Error(Error::Kind::ioFailed, "Cannot read " + utf8(dir) + ": " + ec.message());

    std::vector<Sequence> result;
    for (auto& [name, frames] : groups) {
        std::sort(frames.begin(), frames.end(), [](const Frame& a, const Frame& b) {
            return std::make_tuple(a.number.value_or(0), utf8(a.path.filename())) <
                   std::make_tuple(b.number.value_or(0), utf8(b.path.filename()));
        });
        result.push_back({name.empty() ? utf8(dir.filename()) : name, dir, std::move(frames)});
    }
    std::sort(result.begin(), result.end(), [](const Sequence& a, const Sequence& b) { return naturalLess(a.name, b.name); });
    return result;
}

}  // namespace

std::vector<long> Sequence::missingNumbers() const {
    std::vector<long> numbers;
    for (const Frame& f : frames)
        if (f.number) numbers.push_back(*f.number);
    std::vector<long> missing;
    if (numbers.empty() || long(numbers.size()) >= numbers.back() - numbers.front() + 1) return missing;
    const std::set<long> present(numbers.begin(), numbers.end());
    for (long n = numbers.front(); n <= numbers.back(); ++n)
        if (!present.count(n)) missing.push_back(n);
    return missing;
}

std::vector<Sequence> findSequences(const fs::path& input) {
    const fs::path path = clean(input);
    std::error_code ec;
    const auto status = fs::status(path, ec);
    if (ec || !fs::exists(status)) throw Error::notFound(input);
    if (fs::is_directory(status)) {
        auto groups = group(path);
        if (groups.empty()) throw Error::noFrames(input);
        return groups;
    }
    const auto parsed = parseFrameName(utf8(path.filename()));
    for (auto& seq : group(path.parent_path())) {
        const bool contains = std::any_of(seq.frames.begin(), seq.frames.end(),
                                          [&](const Frame& f) { return f.path == path; });
        if (contains || (parsed && seq.name == parsed->prefix)) return {std::move(seq)};
    }
    throw Error::noFrames(input);
}

std::optional<ParsedName> parseFrameName(const std::string& fileName) {
    if (fileName.size() < 4 || lowered(fileName.substr(fileName.size() - 4)) != ".vbn") return std::nullopt;
    const std::string stem = fileName.substr(0, fileName.size() - 4);
    size_t start = stem.size();
    while (start > 0 && isDigit(stem[start - 1])) --start;
    const size_t digits = stem.size() - start;
    if (digits == 0 || digits > 9) return std::nullopt;
    std::string prefix = stem.substr(0, start);
    if (!prefix.empty() && std::string("_-. ").find(prefix.back()) != std::string::npos) prefix.pop_back();
    return ParsedName{prefix, std::stol(stem.substr(start))};
}

bool naturalLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (isDigit(a[i]) && isDigit(b[j])) {
            size_t ie = i, je = j;
            while (ie < a.size() && isDigit(a[ie])) ++ie;
            while (je < b.size() && isDigit(b[je])) ++je;
            size_t is = i, js = j;
            while (is + 1 < ie && a[is] == '0') ++is;
            while (js + 1 < je && b[js] == '0') ++js;
            if (ie - is != je - js) return ie - is < je - js;
            const int c = a.compare(is, ie - is, b, js, je - js);
            if (c != 0) return c < 0;
            i = ie; j = je;
            continue;
        }
        const char ca = lower(a[i]), cb = lower(b[j]);
        if (ca != cb) return static_cast<unsigned char>(ca) < static_cast<unsigned char>(cb);
        ++i; ++j;
    }
    if (a.size() - i != b.size() - j) return a.size() - i < b.size() - j;
    return a < b;  // equal ignoring case: a stable tie-break
}

const char* formatId(Format f) {
    switch (f) {
    case Format::png: return "png";
    case Format::mp4: return "mp4";
    case Format::webm: return "webm";
    }
    return "";
}

std::optional<Format> parseFormat(const std::string& s) {
    const std::string l = lowered(s);
    for (Format f : {Format::png, Format::mp4, Format::webm})
        if (l == formatId(f)) return f;
    return std::nullopt;
}

const char* formatName(Format f) {
    switch (f) {
    case Format::png: return "PNG sequence";
    case Format::mp4: return "MP4 (H.264)";
    case Format::webm: return "WebM (VP9 + alpha)";
    }
    return "";
}

const char* formatExtension(Format f) {
    switch (f) {
    case Format::png: return nullptr;
    case Format::mp4: return "mp4";
    case Format::webm: return "webm";
    }
    return nullptr;
}

fs::path outputNamed(Format f, const std::string& name, const fs::path& folder) {
    if (const char* ext = formatExtension(f)) return folder / fromUtf8(name + "." + ext);
    return folder / fromUtf8(name + "_png");
}

fs::path defaultOutput(Format f, const Sequence& s) { return outputNamed(f, s.name, s.directory.parent_path()); }

namespace {

/// batchOutputs for `sequences[i]`, i in `indices`.
std::vector<fs::path> namedOutputs(Format f, const std::vector<Sequence>& sequences, const std::vector<size_t>& indices,
                                   const fs::path& folder) {
    std::map<std::string, int> counts;
    for (size_t i : indices) ++counts[lowered(sequences[i].name)];
    std::set<std::string> used;
    std::vector<fs::path> outputs;
    for (size_t i : indices) {
        const Sequence& s = sequences[i];
        const std::string base = counts[lowered(s.name)] > 1 ? s.name + " (" + utf8(s.directory.filename()) + ")" : s.name;
        std::string name = base;
        for (int n = 2; !used.insert(lowered(name)).second; ++n) name = base + " " + std::to_string(n);
        outputs.push_back(outputNamed(f, name, folder));
    }
    return outputs;
}

}  // namespace

std::vector<fs::path> batchOutputs(Format f, const std::vector<Sequence>& sequences, const fs::path& folder) {
    std::vector<size_t> all(sequences.size());
    for (size_t i = 0; i < all.size(); ++i) all[i] = i;
    return namedOutputs(f, sequences, all, folder);
}

std::vector<fs::path> defaultOutputs(Format f, const std::vector<Sequence>& sequences) {
    // Group by parent folder, case-insensitively as Windows paths are; names only clash within a folder.
    std::map<std::string, std::vector<size_t>> byParent;
    for (size_t i = 0; i < sequences.size(); ++i)
        byParent[lowered(utf8(sequences[i].directory.parent_path().lexically_normal()))].push_back(i);
    std::vector<fs::path> outputs(sequences.size());
    for (const auto& [key, indices] : byParent) {
        const auto named = namedOutputs(f, sequences, indices, sequences[indices[0]].directory.parent_path());
        for (size_t k = 0; k < indices.size(); ++k) outputs[indices[k]] = named[k];
    }
    return outputs;
}

}  // namespace vbnx
