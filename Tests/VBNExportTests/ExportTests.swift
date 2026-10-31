import AVFoundation
import XCTest
@testable import VBNCore
@testable import VBNExport

/// Uncompressed `.vbn` (format 0, bytes R, G, B, A) filled with one straight-alpha colour.
private func solidVBN(width: Int, height: Int, rgba: (UInt8, UInt8, UInt8, UInt8)) -> Data {
    var payload = [UInt8]()
    payload.reserveCapacity(width * height * 4)
    for _ in 0 ..< width * height { payload += [rgba.0, rgba.1, rgba.2, rgba.3] }
    return makeVBN(width: width, height: height, format: 0, payload: payload)
}

final class ExportTests: XCTestCase {
    private var dir: URL!

    override func setUpWithError() throws {
        dir = FileManager.default.temporaryDirectory.appendingPathComponent("unvbn-tests-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws { try? FileManager.default.removeItem(at: dir) }

    /// Writes `count` frames `NAME_00000.vbn`… into `dir/seq/` and returns that sequence.
    private func makeSequence(_ name: String = "LOGO", count: Int, width: Int = 7, height: Int = 5,
                              color: (Int) -> (UInt8, UInt8, UInt8, UInt8) = { i in (UInt8(i * 20 % 256), 100, 50, 128) }) throws -> FrameSequence {
        let seqDir = dir.appendingPathComponent("seq-\(name)")
        try FileManager.default.createDirectory(at: seqDir, withIntermediateDirectories: true)
        for i in 0 ..< count {
            try solidVBN(width: width, height: height, rgba: color(i))
                .write(to: seqDir.appendingPathComponent(String(format: "%@_%05d.vbn", name, i)))
        }
        return try XCTUnwrap(FrameSequence.find(at: seqDir).first)
    }

    // MARK: sequence detection

    func testParseFrameNames() {
        XCTAssertEqual(FrameSequence.parse("NAME_00012.vbn")?.prefix, "NAME")
        XCTAssertEqual(FrameSequence.parse("NAME_00012.vbn")?.number, 12)
        XCTAssertEqual(FrameSequence.parse("Logo-3.VBN")?.prefix, "Logo")
        XCTAssertEqual(FrameSequence.parse("0042.vbn")?.prefix, "")
        XCTAssertNil(FrameSequence.parse("still.vbn"))
        XCTAssertNil(FrameSequence.parse("NAME_00012.png"))
    }

    func testFindGroupsSortsAndReportsGaps() throws {
        let data = solidVBN(width: 2, height: 2, rgba: (1, 2, 3, 4))
        for n in [10, 2, 1, 3, 5] { try data.write(to: dir.appendingPathComponent("A_\(String(format: "%05d", n)).vbn")) }
        try data.write(to: dir.appendingPathComponent("B_00000.vbn"))
        try data.write(to: dir.appendingPathComponent("still.vbn"))
        try Data("x".utf8).write(to: dir.appendingPathComponent("A_00004.png"))

        let all = try FrameSequence.find(at: dir)
        XCTAssertEqual(all.map(\.name), ["A", "B", "still"])
        let a = all[0]
        XCTAssertEqual(a.frames.map(\.number), [1, 2, 3, 5, 10])
        XCTAssertEqual(a.missingNumbers, [4, 6, 7, 8, 9])

        // A single frame finds its whole sequence.
        let fromFrame = try FrameSequence.find(at: dir.appendingPathComponent("A_00003.vbn"))
        XCTAssertEqual(fromFrame.count, 1)
        XCTAssertEqual(fromFrame[0].frames.count, 5)
    }

    func testFindErrors() {
        XCTAssertThrowsError(try FrameSequence.find(at: dir.appendingPathComponent("nope"))) {
            XCTAssertEqual($0 as? ExportError, .notFound(self.dir.appendingPathComponent("nope")))
        }
        XCTAssertThrowsError(try FrameSequence.find(at: dir)) { XCTAssertEqual($0 as? ExportError, .noFrames(self.dir)) }
    }

    // MARK: PNG

    func testPNGSequence() throws {
        let seq = try makeSequence(count: 3)
        let out = dir.appendingPathComponent("png")
        var progress: [Int] = []
        try SequenceExporter(sequence: seq, options: .init(format: .png), output: out).run(progress: { progress.append($0.done) })
        XCTAssertEqual(progress, [1, 2, 3])
        let names = try FileManager.default.contentsOfDirectory(atPath: out.path).sorted()
        XCTAssertEqual(names, ["LOGO_00000.png", "LOGO_00001.png", "LOGO_00002.png"])
        let src = try XCTUnwrap(CGImageSourceCreateWithURL(out.appendingPathComponent(names[1]) as CFURL, nil))
        let img = try XCTUnwrap(CGImageSourceCreateImageAtIndex(src, 0, nil))
        XCTAssertEqual(img.width, 7)
        XCTAssertEqual(img.height, 5)
        XCTAssertNotEqual(img.alphaInfo, .none)
    }

    // MARK: MP4

    /// Reads every frame of a movie as BGRA and returns the frame count, the first frame's centre pixel, size and duration.
    private func readMovie(_ url: URL) async throws -> (count: Int, pixel: [UInt8], size: CGSize, duration: Double) {
        let asset = AVURLAsset(url: url)
        let tracks = try await asset.loadTracks(withMediaType: .video)
        let track = try XCTUnwrap(tracks.first)
        let size = try await track.load(.naturalSize)
        let duration = try await asset.load(.duration).seconds
        let reader = try AVAssetReader(asset: asset)
        let out = AVAssetReaderTrackOutput(track: track, outputSettings: [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
        ])
        reader.add(out)
        XCTAssertTrue(reader.startReading())
        var count = 0
        var pixel: [UInt8] = []
        while let sample = out.copyNextSampleBuffer() {
            guard let buf = CMSampleBufferGetImageBuffer(sample) else { continue }
            if count == 0 {
                CVPixelBufferLockBaseAddress(buf, .readOnly)
                let p = CVPixelBufferGetBaseAddress(buf)!.assumingMemoryBound(to: UInt8.self)
                // Sample the middle of the frame (codecs smear edges).
                let y = CVPixelBufferGetHeight(buf) / 2, x = CVPixelBufferGetWidth(buf) / 2
                let o = y * CVPixelBufferGetBytesPerRow(buf) + x * 4
                pixel = [p[o], p[o + 1], p[o + 2], p[o + 3]]
                CVPixelBufferUnlockBaseAddress(buf, .readOnly)
            }
            count += 1
        }
        return (count, pixel, size, duration)
    }

    func testMP4FlattensOntoBackgroundAndPadsToEven() async throws {
        // Colour (200,100,50) at alpha 128 flattened over white.
        let seq = try makeSequence(count: 10, width: 64, height: 48) { _ in (200, 100, 50, 128) }
        let out = dir.appendingPathComponent("out.mp4")
        try SequenceExporter(sequence: seq, options: .init(format: .mp4, fps: 50, background: .init(r: 255, g: 255, b: 255)),
                             output: out).run()
        let movie = try await readMovie(out)
        XCTAssertEqual(movie.count, 10)
        XCTAssertEqual(movie.duration, 0.2, accuracy: 0.001)
        // Expected over white: 200*.502+255*.498 ≈ 227, 100→178, 50→152. BGRA order; lossy, so ±8.
        XCTAssertEqual(Double(movie.pixel[2]), 227, accuracy: 8)
        XCTAssertEqual(Double(movie.pixel[1]), 178, accuracy: 8)
        XCTAssertEqual(Double(movie.pixel[0]), 152, accuracy: 8)

        let odd = try makeSequence("ODD", count: 2, width: 7, height: 5)
        let oddOut = dir.appendingPathComponent("odd.mp4")
        try SequenceExporter(sequence: odd, options: .init(format: .mp4), output: oddOut).run()
        let oddMovie = try await readMovie(oddOut)
        XCTAssertEqual(oddMovie.size, CGSize(width: 8, height: 6))
    }

    func testFractionalFrameRate() async throws {
        let seq = try makeSequence(count: 30, width: 16, height: 16)
        let out = dir.appendingPathComponent("ntsc.mp4")
        try SequenceExporter(sequence: seq, options: .init(format: .mp4, fps: 29.97), output: out).run()
        let movie = try await readMovie(out)
        XCTAssertEqual(movie.duration, 30 / 29.97, accuracy: 0.001)
    }

    // MARK: WebM

    func testWebM() throws {
        guard let ffmpeg = FFmpeg.locate() else { throw XCTSkip("no ffmpeg") }
        let seq = try makeSequence(count: 10, width: 64, height: 48)
        let out = dir.appendingPathComponent("out.webm")
        try SequenceExporter(sequence: seq, options: .init(format: .webm), output: out, ffmpeg: ffmpeg).run()
        let head = try [UInt8](Data(contentsOf: out).prefix(4))
        XCTAssertEqual(head, [0x1A, 0x45, 0xDF, 0xA3], "EBML magic")
    }

    func testWebMWithoutFFmpegFails() throws {
        let seq = try makeSequence(count: 1)
        XCTAssertThrowsError(try SequenceExporter(sequence: seq, options: .init(format: .webm),
                                                  output: dir.appendingPathComponent("x.webm"), ffmpeg: nil).run()) {
            XCTAssertEqual($0 as? ExportError, .ffmpegMissing)
        }
    }

    func testFFmpegFailureIsReported() throws {
        // /usr/bin/false exits 1 immediately: must surface as ffmpegFailed, not crash on SIGPIPE.
        let seq = try makeSequence(count: 40, width: 256, height: 256)
        XCTAssertThrowsError(try SequenceExporter(sequence: seq, options: .init(format: .webm),
                                                  output: dir.appendingPathComponent("x.webm"),
                                                  ffmpeg: URL(fileURLWithPath: "/usr/bin/false")).run()) { error in
            guard case .ffmpegFailed = error as? ExportError else { return XCTFail("\(error)") }
        }
    }

    // MARK: errors

    func testCancelRemovesPartialOutput() throws {
        let seq = try makeSequence(count: 30, width: 16, height: 16)
        let out = dir.appendingPathComponent("cancel.mp4")
        var done = 0
        XCTAssertThrowsError(try SequenceExporter(sequence: seq, options: .init(format: .mp4), output: out)
            .run(progress: { done = $0.done }, isCancelled: { done >= 3 })) {
            XCTAssertEqual($0 as? ExportError, .cancelled)
        }
        XCTAssertFalse(FileManager.default.fileExists(atPath: out.path))
    }

    func testSizeMismatch() throws {
        let seq = try makeSequence(count: 3, width: 8, height: 8)
        try solidVBN(width: 4, height: 4, rgba: (0, 0, 0, 255)).write(to: seq.frames[2].url)
        XCTAssertThrowsError(try SequenceExporter(sequence: seq, options: .init(format: .png),
                                                  output: dir.appendingPathComponent("p")).run()) {
            XCTAssertEqual($0 as? ExportError, .sizeMismatch(frame: "LOGO_00002.vbn", expected: "8×8", got: "4×4"))
        }
    }

    func testInvalidFPS() throws {
        let seq = try makeSequence(count: 1)
        XCTAssertThrowsError(try SequenceExporter(sequence: seq, options: .init(format: .png, fps: 0),
                                                  output: dir.appendingPathComponent("p")).run())
    }

    func testOptionsHelpers() {
        XCTAssertEqual(RGBColor(hex: "#FF8000"), RGBColor(r: 255, g: 128, b: 0))
        XCTAssertEqual(RGBColor(hex: "00ff00"), RGBColor(r: 0, g: 255, b: 0))
        XCTAssertNil(RGBColor(hex: "#12345"))
        XCTAssertEqual(FFmpeg.fpsString(50), "50")
        XCTAssertEqual(FFmpeg.fpsString(29.97), "29.97")
        XCTAssertEqual(FFmpeg.fpsString(23.976), "23.976")
    }

    func testProgressMaths() {
        let early = ExportProgress(done: 5, total: 750, elapsed: 0.1)
        XCTAssertNil(early.framesPerSecond, "no rate during the first 0.3 s")
        XCTAssertNil(early.remaining)
        let mid = ExportProgress(done: 375, total: 750, elapsed: 3)
        XCTAssertEqual(mid.percent, 50)
        XCTAssertEqual(mid.fraction, 0.5)
        XCTAssertEqual(mid.framesPerSecond, 125)
        XCTAssertEqual(mid.remaining, 3)
        XCTAssertEqual(ExportProgress.formatDuration(2.2), "3 s")
        XCTAssertEqual(ExportProgress.formatDuration(65), "1 min 05 s")
    }

    func testCorruptFrameReportsDecodeError() throws {
        let seq = try makeSequence(count: 3, width: 8, height: 8)
        try Data("not a vbn".utf8).write(to: seq.frames[1].url)
        XCTAssertThrowsError(try SequenceExporter(sequence: seq, options: .init(format: .png),
                                                  output: dir.appendingPathComponent("p")).run()) { error in
            guard case .decodeFailed(let frame, _) = error as? ExportError else { return XCTFail("\(error)") }
            XCTAssertEqual(frame, "LOGO_00001.vbn")
        }
    }
}
