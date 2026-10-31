import AVFoundation
import CoreVideo
import Foundation
import VBNCore

/// Where an export stands, with the rate and time-left estimate both front ends display.
public struct ExportProgress: Equatable {
    public let done: Int
    public let total: Int
    public let elapsed: TimeInterval

    public init(done: Int, total: Int, elapsed: TimeInterval) {
        self.done = done; self.total = total; self.elapsed = elapsed
    }

    public var fraction: Double { Double(done) / Double(max(total, 1)) }
    public var percent: Int { done * 100 / max(total, 1) }
    /// Frames per second so far; nil for the first 0.3 s, when the figure is still noise.
    public var framesPerSecond: Double? { done > 0 && elapsed > 0.3 ? Double(done) / elapsed : nil }
    public var remaining: TimeInterval? { framesPerSecond.map { Double(total - done) / $0 } }

    /// "3 s", "1 min 05 s".
    public static func formatDuration(_ seconds: TimeInterval) -> String {
        let s = Int(seconds.rounded(.up))
        return s < 60 ? "\(s) s" : String(format: "%d min %02d s", s / 60, s % 60)
    }
}

/// Converts a `FrameSequence` to a PNG sequence, MP4 or WebM.
///
/// Frames are decoded in parallel batches; PNGs are also encoded in parallel, video frames are handed
/// to the encoder in order. Synchronous: call it off the main thread. `progress` is called on the
/// calling thread, at most once per percent or every 1/30 s, and always for the last frame;
/// `isCancelled` is polled between frames.
public final class SequenceExporter {
    public let sequence: FrameSequence
    public let options: ExportOptions
    public let output: URL
    private let ffmpeg: URL?

    public init(sequence: FrameSequence, options: ExportOptions, output: URL, ffmpeg: URL? = FFmpeg.locate()) {
        self.sequence = sequence; self.options = options; self.output = output; self.ffmpeg = ffmpeg
    }

    public func run(progress: (ExportProgress) -> Void = { _ in }, isCancelled: () -> Bool = { false }) throws {
        guard options.fps.isFinite, options.fps > 0, options.fps <= 1000 else { throw ExportError.invalidFPS(options.fps) }
        let frames = sequence.frames
        guard !frames.isEmpty else { throw ExportError.noFrames(sequence.directory) }
        if options.format.needsFFmpeg, ffmpeg == nil { throw ExportError.ffmpegMissing }

        let start = Date()
        let first = try Self.decode(frames[0])
        let sink: FrameSink
        switch options.format {
        case .png: sink = try PNGSink(directory: output)
        case .mp4: sink = try MP4Sink(output: output, options: options, width: first.width, height: first.height)
        case .webm: sink = try FFmpegSink(ffmpeg: ffmpeg!, output: output, options: options,
                                          width: first.width, height: first.height)
        }

        var done = 0
        var lastReport = (percent: -1, time: Date.distantPast)
        func report() {
            let now = Date()
            let p = ExportProgress(done: done, total: frames.count, elapsed: now.timeIntervalSince(start))
            guard p.percent != lastReport.percent || now.timeIntervalSince(lastReport.time) >= 1 / 30 || done == frames.count
            else { return }
            lastReport = (p.percent, now)
            progress(p)
        }

        do {
            let batchSize = max(2, ProcessInfo.processInfo.activeProcessorCount)
            var offset = 0
            while offset < frames.count {
                if isCancelled() { throw ExportError.cancelled }
                let batch = Array(frames[offset ..< min(offset + batchSize, frames.count)])
                // Each slot holds the decoded image for an in-order sink, or nil once a concurrent sink took it.
                var results = [Result<VBNImage?, Error>?](repeating: nil, count: batch.count)
                let base = offset
                results.withUnsafeMutableBufferPointer { slots in
                    DispatchQueue.concurrentPerform(iterations: batch.count) { i in
                        slots[i] = Result {
                            let image = base + i == 0 ? first : try Self.decode(batch[i])
                            guard image.width == first.width, image.height == first.height else {
                                throw ExportError.sizeMismatch(frame: batch[i].url.lastPathComponent,
                                                               expected: "\(first.width)×\(first.height)",
                                                               got: "\(image.width)×\(image.height)")
                            }
                            if sink.acceptsConcurrentFrames { try sink.append(image, frame: batch[i]); return nil }
                            return image
                        }
                    }
                }
                for (i, result) in results.enumerated() {
                    if isCancelled() { throw ExportError.cancelled }
                    if let image = try result!.get() { try sink.append(image, frame: batch[i]) }
                    done += 1
                    report()
                }
                offset += batch.count
            }
            try sink.finish()
        } catch {
            sink.abort()
            throw error
        }
    }

    private static func decode(_ frame: FrameSequence.Frame) throws -> VBNImage {
        do { return try VBNImage.decodeRGBA(data: Data(contentsOf: frame.url)) }
        catch { throw ExportError.decodeFailed(frame: frame.url.lastPathComponent, message: error.localizedDescription) }
    }
}

// MARK: - Sinks

private protocol FrameSink: AnyObject {
    /// True when `append` may be called from several threads at once, in any order.
    var acceptsConcurrentFrames: Bool { get }
    func append(_ image: VBNImage, frame: FrameSequence.Frame) throws
    func finish() throws
    /// Stops and removes partial output.
    func abort()
}

/// One PNG per frame, named after the source frame. Frames are encoded and written concurrently.
private final class PNGSink: FrameSink {
    let acceptsConcurrentFrames = true
    let directory: URL
    private var written: [URL] = []
    private let lock = NSLock()

    init(directory: URL) throws {
        self.directory = directory
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    }

    func append(_ image: VBNImage, frame: FrameSequence.Frame) throws {
        let name = frame.url.deletingPathExtension().lastPathComponent
        let url = directory.appendingPathComponent(name).appendingPathExtension("png")
        try VBNImage.pngData(from: image.cgImage()).write(to: url)
        lock.lock(); written.append(url); lock.unlock()
    }

    func finish() throws {}

    func abort() {
        lock.lock(); defer { lock.unlock() }
        for url in written { try? FileManager.default.removeItem(at: url) }
    }
}

/// H.264 MP4 via AVAssetWriter. No alpha: frames are flattened onto the background colour.
private final class MP4Sink: FrameSink {
    let acceptsConcurrentFrames = false
    private let writer: AVAssetWriter
    private let input: AVAssetWriterInput
    private let adaptor: AVAssetWriterInputPixelBufferAdaptor
    private let output: URL
    private let background: RGBColor
    private let width: Int, height: Int
    /// 1000 ticks per frame: 50 fps -> timescale 50000, 29.97 -> 29970.
    private let timescale: CMTimeScale
    private var frameIndex: CMTimeValue = 0

    init(output: URL, options: ExportOptions, width: Int, height: Int) throws {
        self.output = output; background = options.background
        // H.264 4:2:0 needs even dimensions: pad right/bottom with the background.
        self.width = (width + 1) & ~1
        self.height = (height + 1) & ~1
        timescale = CMTimeScale((options.fps * 1000).rounded())

        try? FileManager.default.removeItem(at: output)
        writer = try AVAssetWriter(outputURL: output, fileType: .mp4)
        let settings: [String: Any] = [
            AVVideoCodecKey: AVVideoCodecType.h264,
            AVVideoWidthKey: self.width,
            AVVideoHeightKey: self.height,
            AVVideoCompressionPropertiesKey: [
                AVVideoAverageBitRateKey: max(2_000_000, Int(Double(self.width * self.height) * options.fps * 0.2)),
                AVVideoProfileLevelKey: AVVideoProfileLevelH264HighAutoLevel,
                AVVideoExpectedSourceFrameRateKey: options.fps,
            ],
            // HD broadcast is BT.709. Untagged, VideoToolbox picks the SD (601) matrix and colours drift.
            AVVideoColorPropertiesKey: [
                AVVideoColorPrimariesKey: AVVideoColorPrimaries_ITU_R_709_2,
                AVVideoTransferFunctionKey: AVVideoTransferFunction_ITU_R_709_2,
                AVVideoYCbCrMatrixKey: AVVideoYCbCrMatrix_ITU_R_709_2,
            ],
        ]
        input = AVAssetWriterInput(mediaType: .video, outputSettings: settings)
        input.expectsMediaDataInRealTime = false
        adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input, sourcePixelBufferAttributes: [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
            kCVPixelBufferWidthKey as String: self.width,
            kCVPixelBufferHeightKey as String: self.height,
        ])
        guard writer.canAdd(input) else { throw ExportError.writerFailed("cannot add a video track") }
        writer.add(input)
        guard writer.startWriting() else { throw ExportError.writerFailed(writer.error?.localizedDescription ?? "cannot start") }
        writer.startSession(atSourceTime: .zero)
    }

    func append(_ image: VBNImage, frame: FrameSequence.Frame) throws {
        while !input.isReadyForMoreMediaData {
            if writer.status == .failed { throw failure() }
            Thread.sleep(forTimeInterval: 0.002)
        }
        guard let pool = adaptor.pixelBufferPool else { throw failure() }
        var buffer: CVPixelBuffer?
        CVPixelBufferPoolCreatePixelBuffer(nil, pool, &buffer)
        guard let buffer else { throw ExportError.writerFailed("no pixel buffer") }
        fill(buffer, from: image)
        let time = CMTime(value: frameIndex * 1000, timescale: timescale)
        guard adaptor.append(buffer, withPresentationTime: time) else { throw failure() }
        frameIndex += 1
    }

    /// RGBA (straight alpha) -> opaque BGRA composited over the background; the padding is plain background.
    private func fill(_ buffer: CVPixelBuffer, from image: VBNImage) {
        CVPixelBufferLockBaseAddress(buffer, [])
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        let base = CVPixelBufferGetBaseAddress(buffer)!.assumingMemoryBound(to: UInt8.self)
        let stride = CVPixelBufferGetBytesPerRow(buffer)
        let bg = background
        let bgB = UInt16(bg.b), bgG = UInt16(bg.g), bgR = UInt16(bg.r)
        func paint(_ d: UnsafeMutablePointer<UInt8>) { d[0] = bg.b; d[1] = bg.g; d[2] = bg.r; d[3] = 255 }
        image.rgba.withUnsafeBufferPointer { src in
            for y in 0 ..< image.height {
                let row = base + y * stride
                var s = y * image.width * 4
                for x in 0 ..< image.width {
                    let d = row + x * 4
                    let a = UInt16(src[s + 3]), ia = 255 - a
                    d[0] = UInt8((UInt16(src[s + 2]) * a + bgB * ia + 127) / 255)
                    d[1] = UInt8((UInt16(src[s + 1]) * a + bgG * ia + 127) / 255)
                    d[2] = UInt8((UInt16(src[s]) * a + bgR * ia + 127) / 255)
                    d[3] = 255
                    s += 4
                }
                for x in image.width ..< width { paint(row + x * 4) }
            }
        }
        for y in image.height ..< height {
            for x in 0 ..< width { paint(base + y * stride + x * 4) }
        }
    }

    func finish() throws {
        input.markAsFinished()
        let done = DispatchSemaphore(value: 0)
        writer.finishWriting { done.signal() }
        done.wait()
        guard writer.status == .completed else { throw failure() }
    }

    func abort() {
        if writer.status == .writing { writer.cancelWriting() }
        try? FileManager.default.removeItem(at: output)
    }

    private func failure() -> ExportError {
        .writerFailed(writer.error?.localizedDescription ?? "status \(writer.status.rawValue)")
    }
}

/// WebM VP9 + alpha: raw RGBA frames piped into ffmpeg.
private final class FFmpegSink: FrameSink {
    let acceptsConcurrentFrames = false
    private let process = Process()
    private let stdin = Pipe()
    private let stderr = Pipe()
    private let output: URL
    private var errorText = Data()
    private let errorLock = NSLock()

    init(ffmpeg: URL, output: URL, options: ExportOptions, width: Int, height: Int) throws {
        self.output = output
        // A dead ffmpeg must surface as a write error (EPIPE) on this pipe, not kill the process with SIGPIPE.
        _ = fcntl(stdin.fileHandleForWriting.fileDescriptor, F_SETNOSIGPIPE, 1)
        process.executableURL = ffmpeg
        process.arguments = FFmpeg.webmArguments(width: width, height: height, options: options, output: output)
        process.standardInput = stdin
        process.standardOutput = FileHandle.nullDevice
        process.standardError = stderr
        stderr.fileHandleForReading.readabilityHandler = { [weak self] h in
            let chunk = h.availableData
            guard let self, !chunk.isEmpty else { return }
            self.errorLock.lock(); self.errorText.append(chunk); self.errorLock.unlock()
        }
        try process.run()
    }

    func append(_ image: VBNImage, frame: FrameSequence.Frame) throws {
        do { try image.rgba.withUnsafeBytes { try stdin.fileHandleForWriting.write(contentsOf: $0) } }
        catch { throw failure(fallback: error.localizedDescription) }
    }

    func finish() throws {
        try? stdin.fileHandleForWriting.close()
        process.waitUntilExit()
        stderr.fileHandleForReading.readabilityHandler = nil
        guard process.terminationStatus == 0 else { throw failure(fallback: "") }
    }

    func abort() {
        try? stdin.fileHandleForWriting.close()
        if process.isRunning { process.terminate() }
        process.waitUntilExit()
        stderr.fileHandleForReading.readabilityHandler = nil
        try? FileManager.default.removeItem(at: output)
    }

    private func failure(fallback: String) -> ExportError {
        if process.isRunning { try? stdin.fileHandleForWriting.close(); process.waitUntilExit() }
        errorLock.lock(); defer { errorLock.unlock() }
        let text = String(decoding: errorText, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
        return .ffmpegFailed(status: process.isRunning ? -1 : process.terminationStatus,
                             message: text.isEmpty ? fallback : text)
    }
}

// MARK: - ffmpeg

public enum FFmpeg {
    /// The ffmpeg used for WebM: `$UNVBN_FFMPEG`, else the bundled `unVBN.app/Contents/Helpers/ffmpeg`
    /// (found from the app in Contents/MacOS/ or from the `unvbn` tool in Contents/Helpers/, symlinks
    /// resolved). Debug builds also fall back to a Homebrew install; release builds never use a system ffmpeg.
    public static func locate() -> URL? {
        var candidates: [URL] = []
        if let env = ProcessInfo.processInfo.environment["UNVBN_FFMPEG"], !env.isEmpty {
            candidates.append(URL(fileURLWithPath: env))
        }
        if let exe = Bundle.main.executableURL?.resolvingSymlinksInPath() {
            let dir = exe.deletingLastPathComponent()
            candidates.append(dir.appendingPathComponent("ffmpeg"))
            candidates.append(dir.deletingLastPathComponent().appendingPathComponent("Helpers/ffmpeg"))
        }
        #if DEBUG
        candidates += ["/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg"].map(URL.init(fileURLWithPath:))
        #endif
        return candidates.first { FileManager.default.isExecutableFile(atPath: $0.path) }
    }

    static func webmArguments(width: Int, height: Int, options: ExportOptions, output: URL) -> [String] {
        ["-hide_banner", "-loglevel", "error", "-nostdin", "-y",
         "-f", "rawvideo", "-pix_fmt", "rgba", "-s", "\(width)x\(height)", "-framerate", fpsString(options.fps),
         "-i", "pipe:0",
         "-c:v", "libvpx-vp9", "-pix_fmt", "yuva420p", "-crf", "\(options.webmCRF)", "-b:v", "0",
         "-row-mt", "1", "-deadline", "good", "-cpu-used", "4",
         output.path]
    }

    /// 50 -> "50", 29.97 -> "29.97", 23.976 -> "23.976".
    public static func fpsString(_ fps: Double) -> String {
        fps == fps.rounded() ? String(Int(fps)) : String(fps)
    }
}

/// Exports several sequences one after the other with the same options, as the app's batch export and the
/// CLI do. A sequence that fails is recorded and the next one runs; cancelling stops the batch.
public struct BatchExporter {
    public typealias Job = (sequence: FrameSequence, output: URL)

    public enum Event {
        /// Job `index` (0-based) begins.
        case started(index: Int)
        /// `overall` counts the frames of the whole batch, so a bar, rate and time left never jump back;
        /// `current` counts those of the running job.
        case progress(index: Int, overall: ExportProgress, current: ExportProgress)
        /// Job `index` ended; `error` is nil when it succeeded.
        case finished(index: Int, error: Error?)
    }

    public struct Result {
        public var exported: [URL] = []
        public var failures: [(sequence: FrameSequence, error: Error)] = []
        public var cancelled = false
    }

    public let jobs: [Job]
    public let options: ExportOptions
    private let ffmpeg: URL?

    public init(jobs: [Job], options: ExportOptions, ffmpeg: URL? = FFmpeg.locate()) {
        self.jobs = jobs; self.options = options; self.ffmpeg = ffmpeg
    }

    /// Synchronous, like `SequenceExporter.run`: call it off the main thread.
    public func run(_ handle: (Event) -> Void = { _ in }, isCancelled: () -> Bool = { false }) -> Result {
        let total = jobs.reduce(0) { $0 + $1.sequence.frames.count }
        let start = Date()
        var result = Result(), framesBefore = 0
        for (index, job) in jobs.enumerated() {
            if isCancelled() { result.cancelled = true; break }
            handle(.started(index: index))
            let offset = framesBefore
            do {
                try SequenceExporter(sequence: job.sequence, options: options, output: job.output, ffmpeg: ffmpeg).run(
                    progress: { p in
                        let overall = ExportProgress(done: offset + p.done, total: total, elapsed: Date().timeIntervalSince(start))
                        handle(.progress(index: index, overall: overall, current: p))
                    },
                    isCancelled: isCancelled)
                result.exported.append(job.output)
                handle(.finished(index: index, error: nil))
            } catch ExportError.cancelled {
                result.cancelled = true
                handle(.finished(index: index, error: ExportError.cancelled))
                break
            } catch {
                result.failures.append((job.sequence, error))
                handle(.finished(index: index, error: error))
            }
            framesBefore += job.sequence.frames.count
        }
        return result
    }
}
