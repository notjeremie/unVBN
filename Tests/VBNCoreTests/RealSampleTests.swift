import CoreGraphics
import Foundation
import ImageIO
import XCTest
@testable import VBNCore

/// Compares the Swift decoder against PNGs made by the Python reference, for real Viz files.
/// Real files belong to their owners and are not in the repo: the test is skipped when the folder is absent.
/// Folder: env VBN_SAMPLES_DIR (pass TEST_RUNNER_VBN_SAMPLES_DIR=... to xcodebuild), each `X.vbn` next to `X.png`.
final class RealSampleTests: XCTestCase {
    static let defaultDir = NSHomeDirectory() + "/Documents/Coding/vbn-samples"   // real samples, kept OUT of the repo

    func testRealSamplesMatchReferencePNGs() throws {
        let dir = ProcessInfo.processInfo.environment["VBN_SAMPLES_DIR"] ?? Self.defaultDir
        guard let walker = FileManager.default.enumerator(atPath: dir) else {
            throw XCTSkip("no sample folder at \(dir)")
        }
        let pairs = walker.compactMap { $0 as? String }.filter { $0.hasSuffix(".vbn") }.sorted()
            .map { URL(fileURLWithPath: dir).appendingPathComponent($0) }
            .filter { FileManager.default.fileExists(atPath: $0.deletingPathExtension().appendingPathExtension("png").path) }
        guard !pairs.isEmpty else { throw XCTSkip("no .vbn/.png pairs in \(dir)") }

        for vbn in pairs {
            let img = try VBNImage.decodeRGBA(data: Data(contentsOf: vbn))
            let ref = try loadRGBA(vbn.deletingPathExtension().appendingPathExtension("png"))
            XCTAssertEqual(ref.w, img.width, vbn.lastPathComponent)
            XCTAssertEqual(ref.h, img.height, vbn.lastPathComponent)
            guard ref.w == img.width, ref.h == img.height else { continue }
            let upright = maxDiff(img.rgba, ref.px, w: img.width, h: img.height, flip: false)
            let flipped = maxDiff(img.rgba, ref.px, w: img.width, h: img.height, flip: true)
            print("[real] \(vbn.lastPathComponent) \(img.width)x\(img.height) fmt 0x\(String(img.header.rawFormat, radix: 16)): max diff upright \(upright), vs flipped reference \(flipped)")
            XCTAssertLessThanOrEqual(upright, 1, "\(vbn.lastPathComponent): reference PNG differs (flipped diff \(flipped))")
        }
    }

    private func maxDiff(_ a: [UInt8], _ b: [UInt8], w: Int, h: Int, flip: Bool) -> Int {
        var m = 0
        for y in 0 ..< h {
            let by = flip ? h - 1 - y : y
            for i in 0 ..< w * 4 { m = max(m, abs(Int(a[y * w * 4 + i]) - Int(b[by * w * 4 + i]))) }
        }
        return m
    }

    /// Raw (non-premultiplied) RGBA8 of a PNG, as stored.
    private func loadRGBA(_ url: URL) throws -> (w: Int, h: Int, px: [UInt8]) {
        let src = try XCTUnwrap(CGImageSourceCreateWithURL(url as CFURL, nil))
        let cg = try XCTUnwrap(CGImageSourceCreateImageAtIndex(src, 0, nil))
        XCTAssertEqual(cg.bitsPerPixel, 32); XCTAssertEqual(cg.alphaInfo, .last)
        let data = try XCTUnwrap(cg.dataProvider?.data) as Data
        var px = [UInt8](); px.reserveCapacity(cg.width * cg.height * 4)
        for y in 0 ..< cg.height {
            let s = y * cg.bytesPerRow
            px += data[s ..< s + cg.width * 4]
        }
        return (cg.width, cg.height, px)
    }
}
