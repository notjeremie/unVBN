import Foundation

/// An ordered run of `.vbn` frames sharing a name prefix: `NAME_00000.vbn` … `NAME_00749.vbn`.
public struct FrameSequence: Equatable {
    public struct Frame: Equatable {
        public let url: URL
        /// Frame number parsed from the file name (nil for a file without trailing digits).
        public let number: Int?
    }

    /// Prefix shared by the frames, without the trailing separator (`NAME` for `NAME_00012.vbn`).
    public let name: String
    /// Folder holding the frames.
    public let directory: URL
    public let frames: [Frame]
    /// Width of the frame number in the source names (5 for `_00012`), reused for PNG output names.
    public let digits: Int

    /// Frame numbers absent between the first and last frame.
    public var missingNumbers: [Int] {
        let numbers = frames.compactMap(\.number)
        guard let first = numbers.first, let last = numbers.last, numbers.count < last - first + 1 else { return [] }
        let present = Set(numbers)
        return (first ... last).filter { !present.contains($0) }
    }

    /// Every sequence found at `url`: all sequences in a folder (not recursive), or, for a `.vbn` file,
    /// the sequence it belongs to. Sequences are sorted by name, frames by number.
    public static func find(at url: URL) throws -> [FrameSequence] {
        var isDir: ObjCBool = false
        guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir) else {
            throw ExportError.notFound(url)
        }
        if isDir.boolValue {
            let groups = try group(contentsOf: url)
            guard !groups.isEmpty else { throw ExportError.noFrames(url) }
            return groups
        }
        let key = parse(url.lastPathComponent)?.prefix
        guard let match = try group(contentsOf: url.deletingLastPathComponent()).first(where: { seq in
            seq.frames.contains { $0.url.standardizedFileURL == url.standardizedFileURL } || (key != nil && seq.name == key)
        }) else { throw ExportError.noFrames(url) }
        return [match]
    }

    private static func group(contentsOf dir: URL) throws -> [FrameSequence] {
        let files = try FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil,
                                                                options: [.skipsHiddenFiles])
            .filter { $0.pathExtension.lowercased() == "vbn" }
        var groups: [String: [(Frame, Int)]] = [:]
        for file in files {
            if let p = parse(file.lastPathComponent) {
                groups[p.prefix, default: []].append((Frame(url: file, number: p.number), p.digits))
            } else {
                let stem = file.deletingPathExtension().lastPathComponent
                groups[stem, default: []].append((Frame(url: file, number: nil), 0))
            }
        }
        return groups.map { name, entries in
            let frames = entries.map(\.0).sorted { ($0.number ?? 0, $0.url.lastPathComponent) < ($1.number ?? 0, $1.url.lastPathComponent) }
            return FrameSequence(name: name.isEmpty ? dir.lastPathComponent : name, directory: dir,
                                 frames: frames, digits: entries.map(\.1).max() ?? 0)
        }.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
    }

    /// `NAME_00012.vbn` -> ("NAME", 12, 5). Separators `_`, `-`, `.` and space before the digits are dropped.
    static func parse(_ fileName: String) -> (prefix: String, number: Int, digits: Int)? {
        guard fileName.lowercased().hasSuffix(".vbn") else { return nil }
        let stem = fileName.dropLast(4)
        let digits = stem.reversed().prefix(while: \.isASCII).prefix(while: \.isNumber)
        guard !digits.isEmpty, digits.count <= 9, let number = Int(String(digits.reversed())) else { return nil }
        var prefix = stem.dropLast(digits.count)
        if let last = prefix.last, "_-. ".contains(last) { prefix = prefix.dropLast() }
        return (String(prefix), number, digits.count)
    }
}
