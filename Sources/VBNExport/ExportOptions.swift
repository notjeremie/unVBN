import Foundation

public enum ExportFormat: String, CaseIterable, Identifiable {
    /// One PNG per frame, with alpha.
    case png
    /// H.264 in MP4. No alpha: frames are flattened onto `ExportOptions.background`.
    case mp4
    /// VP9 with alpha in WebM, encoded by the bundled ffmpeg.
    case webm

    public var id: String { rawValue }

    public var displayName: String {
        switch self {
        case .png: return "PNG sequence"
        case .mp4: return "MP4 (H.264)"
        case .webm: return "WebM (VP9 + alpha)"
        }
    }

    /// Extension of the output file; nil when the output is a folder of PNGs.
    public var fileExtension: String? {
        switch self {
        case .png: return nil
        case .mp4: return "mp4"
        case .webm: return "webm"
        }
    }

    public var needsFFmpeg: Bool { self == .webm }

    /// Default output next to the sequence folder: `NAME.mp4`, or the folder `NAME_png`.
    public func defaultOutput(for sequence: FrameSequence) -> URL {
        output(named: sequence.name, in: sequence.directory.deletingLastPathComponent())
    }

    /// Outputs for exporting several sequences into one folder, in the same order. Each is named after
    /// its sequence; a name shared by several sequences gets its folder added, `NAME (FOLDER)`, then
    /// ` 2`, ` 3`… if that is still taken, so no export overwrites another.
    public func batchOutputs(for sequences: [FrameSequence], in folder: URL) -> [URL] {
        var counts: [String: Int] = [:]
        for seq in sequences { counts[seq.name.lowercased(), default: 0] += 1 }
        var used = Set<String>()
        return sequences.map { seq in
            let base = counts[seq.name.lowercased()]! > 1 ? "\(seq.name) (\(seq.directory.lastPathComponent))" : seq.name
            var name = base, n = 2
            while !used.insert(name.lowercased()).inserted { name = "\(base) \(n)"; n += 1 }
            return output(named: name, in: folder)
        }
    }

    /// Default outputs for several sequences: each next to its own folder, as `defaultOutput`, with
    /// shared names told apart as in `batchOutputs`.
    public func defaultOutputs(for sequences: [FrameSequence]) -> [URL] {
        var outputs = [URL?](repeating: nil, count: sequences.count)
        let byParent = Dictionary(grouping: sequences.indices) { sequences[$0].directory.deletingLastPathComponent().standardizedFileURL }
        for (parent, indices) in byParent {
            for (i, url) in zip(indices, batchOutputs(for: indices.map { sequences[$0] }, in: parent)) { outputs[i] = url }
        }
        return outputs.map { $0! }
    }

    /// `folder/NAME.ext`, or the folder `folder/NAME_png` for PNG.
    private func output(named name: String, in folder: URL) -> URL {
        if let ext = fileExtension { return folder.appendingPathComponent(name).appendingPathExtension(ext) }
        return folder.appendingPathComponent(name + "_png", isDirectory: true)
    }
}

public struct RGBColor: Equatable {
    public var r: UInt8, g: UInt8, b: UInt8
    public init(r: UInt8, g: UInt8, b: UInt8) { self.r = r; self.g = g; self.b = b }
    public static let black = RGBColor(r: 0, g: 0, b: 0)

    /// `#RRGGBB` or `RRGGBB`.
    public init?(hex: String) {
        let s = hex.hasPrefix("#") ? String(hex.dropFirst()) : hex
        guard s.count == 6, let v = UInt32(s, radix: 16) else { return nil }
        self.init(r: UInt8(v >> 16 & 0xff), g: UInt8(v >> 8 & 0xff), b: UInt8(v & 0xff))
    }
}

public struct ExportOptions: Equatable {
    public var format: ExportFormat
    /// Frames per second. 50 matches 1080i50 / 1080p50 broadcast graphics.
    public var fps: Double
    /// Background for formats without alpha (MP4).
    public var background: RGBColor
    /// VP9 constant quality (0 best … 63 worst) for WebM.
    public var webmCRF: Int

    public init(format: ExportFormat, fps: Double = 50, background: RGBColor = .black, webmCRF: Int = 30) {
        self.format = format; self.fps = fps; self.background = background; self.webmCRF = webmCRF
    }
}

public enum ExportError: LocalizedError, Equatable {
    case notFound(URL)
    case noFrames(URL)
    case sizeMismatch(frame: String, expected: String, got: String)
    case decodeFailed(frame: String, message: String)
    case invalidFPS(Double)
    case ffmpegMissing
    case ffmpegFailed(status: Int32, message: String)
    case writerFailed(String)
    case cancelled

    public var errorDescription: String? {
        switch self {
        case .notFound(let url): return "\(url.path) does not exist."
        case .noFrames(let url): return "No .vbn frames found at \(url.path)."
        case let .sizeMismatch(frame, expected, got):
            return "\(frame) is \(got), but the sequence is \(expected). All frames must have the same size."
        case let .decodeFailed(frame, message): return "Could not read \(frame): \(message)"
        case .invalidFPS(let fps): return "Invalid frame rate: \(fps)."
        case .ffmpegMissing: return "WebM export needs ffmpeg, which was not found inside unVBN.app."
        case let .ffmpegFailed(status, message): return "ffmpeg failed (exit \(status)): \(message)"
        case .writerFailed(let message): return "Video encoding failed: \(message)"
        case .cancelled: return "Export cancelled."
        }
    }
}
