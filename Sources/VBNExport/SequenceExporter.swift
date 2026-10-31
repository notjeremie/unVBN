import AVFoundation
import CoreVideo
import Foundation
import VBNCore

/// Converts a `FrameSequence` to a PNG sequence, MP4, WebM or ProRes 4444 MOV.
///
/// Frames are decoded in parallel batches and handed in order to a sink. Synchronous: call it off the
/// main thread. `progress` gets (frames done, total); `isCancelled` is polled between frames.
public final class SequenceExporter {
    public let sequence: FrameSequence
    public let options: ExportOptions
    public let output: URL
    private let ffmpeg: URL?

    public init(sequence: FrameSequence, options: ExportOptions, output: URL, ffmpeg: URL? = FFmpeg.locate()) {
        self.sequence = sequence; self.options = options; self.output = output; self.ffmpeg = ffmpeg
    }

    public func run(progress: (Int, Int) -> Void = { _, _ in }, isCancelled: () -> Bool = { false }) throws {
        guard options.fps.isFinite, options.fps > 0, options.fps <= 1000 else { throw ExportError.invalidFPS(options.fps) }
        let frames = sequence.frames
        guard !frames.isEmpty else { throw ExportError.noFrames(sequence.directory) }
        if options.format.needsFFmpeg, ffmpeg == nil { throw ExportError.ffmpegMissing }

        let first = try decode(frames[0])
        let sink: FrameSink
        switch options.format {
        case .png: sink = try PNGSink(directory: output)
        case .mp4, .prores: sink = try AVSink(output: output, options: options, width: first.width, height: first.height)
        case .webm: sink = try FFmpegSink(ffmpeg: ffmpeg!, output: output, options: options,
                                          width: first.width, height: first.height)
        }

        var done = 0
        do {
            let batchSize = max(2, ProcessInfo.processInfo.activeProcessorCount)
            var start = 0
            while start < frames.count {
                if isCancelled() { throw ExportError.cancelled }
                let batch = Array(frames[start ..< min(start + batchSize, frames.count)])
                var decoded = [Result<VBNImage, Error>?](repeating: nil, count: batch.count)
                decoded.withUnsafeMutableBufferPointer { slots in
                    DispatchQueue.concurrentPerform(iterations: batch.count) { i in
                        slots[i] = Result { start + i == 0 ? first : try self.decode(batch[i]) }
                    }
                }
                for (i, result) in decoded.enumerated() {
                    if isCancelled() { throw ExportError.cancelled }
                    let image = try result!.get()
                    guard image.width == first.width, image.height == first.height else {
                        throw ExportError.sizeMismatch(frame: batch[i].url.lastPathComponent,
                                                       expected: "\(first.width)×\(first.height)",
                                                       got: "\(image.width)×\(image.height)")
                    }
                    try sink.append(image, index: start + i, name: batch[i].url.deletingPathExtension().lastPathComponent)
                    done += 1
                    progress(done, frames.count)
                }
                start += batch.count
            }
            try sink.finish()
        } catch {
            sink.abort()
            throw error
        }
    }

    private func decode(_ frame: FrameSequence.Frame) throws -> VBNImage {
        do { return try VBNImage.decodeRGBA(data: Data(contentsOf: frame.url)) }
        catch { throw ExportError.writerFailed("\(frame.url.lastPathComponent): \(error.localizedDescription)") }
    }
}

// MARK: - Sinks

private protocol FrameSink {
    func append(_ image: VBNImage, index: Int, name: String) throws
    func finish() throws
    /// Stops and removes partial output.
    func abort()
}

/// One PNG per frame, named after the source frame.
private final class PNGSink: FrameSink {
    let directory: URL
    private var written: [URL] = []

    init(directory: URL) throws {
        self.directory = directory
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    }

    func append(_ image: VBNImage, index: Int, name: String) throws {
        let url = directory.appendingPathComponent(name).appendingPathExtension("png")
        try VBNImage.pngData(from: image.cgImage()).write(to: url, options: .atomic)
        written.append(url)
    }

    func finish() throws {}

    func abort() { for url in written { try? FileManager.default.removeItem(at: url) } }
}

/// H.264 MP4 (flattened onto a background) or ProRes 4444 MOV (straight alpha), via AVAssetWriter.
private final class AVSink: FrameSink {
    private let writer: AVAssetWriter
    private let input: AVAssetWriterInput
    private let adaptor: AVAssetWriterInputPixelBufferAdaptor
    private let output: URL
    private let options: ExportOptions
    private let width: Int, height: Int
    private let timescale: CMTimeScale
    private let frameTicks: CMTimeValue

    init(output: URL, options: ExportOptions, width: Int, height: Int) throws {
        self.output = output; self.options = options
        // H.264 4:2:0 needs even dimensions: pad right/bottom with the background.
        let flatten = options.format == .mp4
        self.width = flatten ? (width + 1) & ~1 : width
        self.height = flatten ? (height + 1) & ~1 : height
        // Exact rational time base: 50 -> 50/1, 29.97 -> 30000/1001-ish via 1000x scale.
        timescale = CMTimeScale((options.fps * 1000).rounded())
        frameTicks = 1000

        try? FileManager.default.removeItem(at: output)
        writer = try AVAssetWriter(outputURL: output, fileType: flatten ? .mp4 : .mov)

        var settings: [String: Any] = [AVVideoWidthKey: self.width, AVVideoHeightKey: self.height]
        if flatten {
            settings[AVVideoCodecKey] = AVVideoCodecType.h264
            settings[AVVideoCompressionPropertiesKey] = [
                AVVideoAverageBitRateKey: max(2_000_000, Int(Double(self.width * self.height) * options.fps * 0.2)),
                AVVideoProfileLevelKey: AVVideoProfileLevelH264HighAutoLevel,
                AVVideoExpectedSourceFrameRateKey: options.fps,
            ]
        } else {
            settings[AVVideoCodecKey] = AVVideoCodecType.proRes4444
        }
        // HD broadcast is BT.709. Untagged, VideoToolbox picks the SD (601) matrix and colours drift.
        settings[AVVideoColorPropertiesKey] = [
            AVVideoColorPrimariesKey: AVVideoColorPrimaries_ITU_R_709_2,
            AVVideoTransferFunctionKey: AVVideoTransferFunction_ITU_R_709_2,
            AVVideoYCbCrMatrixKey: AVVideoYCbCrMatrix_ITU_R_709_2,
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

    func append(_ image: VBNImage, index: Int, name: String) throws {
        while !input.isReadyForMoreMediaData {
            if writer.status == .failed { throw failure() }
            Thread.sleep(forTimeInterval: 0.002)
        }
        guard let pool = adaptor.pixelBufferPool else { throw failure() }
        var buffer: CVPixelBuffer?
        CVPixelBufferPoolCreatePixelBuffer(nil, pool, &buffer)
        guard let buffer else { throw ExportError.writerFailed("no pixel buffer") }
        fill(buffer, from: image)
        let time = CMTime(value: CMTimeValue(index) * frameTicks, timescale: timescale)
        guard adaptor.append(buffer, withPresentationTime: time) else { throw failure() }
    }

    /// RGBA (straight) -> BGRA. MP4: composite over the background, padding included. ProRes: straight copy.
    private func fill(_ buffer: CVPixelBuffer, from image: VBNImage) {
        CVPixelBufferLockBaseAddress(buffer, [])
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        let base = CVPixelBufferGetBaseAddress(buffer)!.assumingMemoryBound(to: UInt8.self)
        let stride = CVPixelBufferGetBytesPerRow(buffer)
        let bg = options.background
        let flatten = options.format == .mp4
        image.rgba.withUnsafeBufferPointer { src in
            for y in 0 ..< height {
                let row = base + y * stride
                for x in 0 ..< width {
                    let d = row + x * 4
                    guard x < image.width, y < image.height else {
                        d[0] = bg.b; d[1] = bg.g; d[2] = bg.r; d[3] = flatten ? 255 : 0
                        continue
                    }
                    let s = (y * image.width + x) * 4
                    let r = src[s], g = src[s + 1], b = src[s + 2], a = src[s + 3]
                    if flatten {
                        let a16 = UInt16(a), ia = 255 - a16
                        d[0] = UInt8((UInt16(b) * a16 + UInt16(bg.b) * ia + 127) / 255)
                        d[1] = UInt8((UInt16(g) * a16 + UInt16(bg.g) * ia + 127) / 255)
                        d[2] = UInt8((UInt16(r) * a16 + UInt16(bg.r) * ia + 127) / 255)
                        d[3] = 255
                    } else {
                        d[0] = b; d[1] = g; d[2] = r; d[3] = a
                    }
                }
            }
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
    private let process = Process()
    private let stdin = Pipe()
    private let stderr = Pipe()
    private let output: URL
    private var errorText = Data()
    private let errorLock = NSLock()

    init(ffmpeg: URL, output: URL, options: ExportOptions, width: Int, height: Int) throws {
        self.output = output
        // A dead ffmpeg must surface as a write error, not kill us with SIGPIPE.
        signal(SIGPIPE, SIG_IGN)
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

    func append(_ image: VBNImage, index: Int, name: String) throws {
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
    /// resolved), else a Homebrew install (handy for development builds without the bundled binary).
    public static func locate() -> URL? {
        let fm = FileManager.default
        var candidates: [URL] = []
        if let env = ProcessInfo.processInfo.environment["UNVBN_FFMPEG"], !env.isEmpty {
            candidates.append(URL(fileURLWithPath: env))
        }
        if let exe = Bundle.main.executableURL?.resolvingSymlinksInPath() {
            let dir = exe.deletingLastPathComponent()
            candidates.append(dir.appendingPathComponent("ffmpeg"))
            candidates.append(dir.deletingLastPathComponent().appendingPathComponent("Helpers/ffmpeg"))
        }
        candidates += ["/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg"].map(URL.init(fileURLWithPath:))
        return candidates.first { fm.isExecutableFile(atPath: $0.path) }
    }

    static func webmArguments(width: Int, height: Int, options: ExportOptions, output: URL) -> [String] {
        ["-hide_banner", "-loglevel", "error", "-nostdin", "-y",
         "-f", "rawvideo", "-pix_fmt", "rgba", "-s", "\(width)x\(height)", "-framerate", fpsString(options.fps),
         "-i", "pipe:0",
         "-c:v", "libvpx-vp9", "-pix_fmt", "yuva420p", "-crf", "\(options.webmCRF)", "-b:v", "0",
         "-row-mt", "1", "-deadline", "good", "-cpu-used", "4",
         output.path]
    }

    /// 50 -> "50", 29.97 -> "29.97".
    static func fpsString(_ fps: Double) -> String {
        fps == fps.rounded() ? String(Int(fps)) : String(fps)
    }
}
