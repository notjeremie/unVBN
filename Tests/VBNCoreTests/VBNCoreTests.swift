import CoreGraphics
import XCTest
@testable import VBNCore

final class VBNCoreTests: XCTestCase {
    private func pixel(_ img: VBNImage, _ x: Int, _ y: Int) -> [UInt8] {
        let o = (y * img.width + x) * 4
        return Array(img.rgba[o ..< o + 4])
    }

    private func assert4x4(_ img: VBNImage, file: StaticString = #filePath, line: UInt = #line) {
        XCTAssertEqual(img.width, 4, file: file, line: line)
        XCTAssertEqual(img.height, 4, file: file, line: line)
        for y in 0 ..< 4 { for x in 0 ..< 4 {
            XCTAssertEqual(pixel(img, x, y), expectedRGBA4x4(x: x, y: y), "pixel \(x),\(y)", file: file, line: line)
        } }
    }

    // MARK: Raw BGRA

    func testRawBGRA4x4IsUprightRGBA() throws {
        let img = try VBNImage.decodeRGBA(data: makeVBN(width: 4, height: 4, format: 0x000, payload: bgra4x4Stored))
        assert4x4(img)
        XCTAssertEqual(img.header.pixelFormat, .bgra8888)
        XCTAssertFalse(img.header.compressed)
    }

    func testCGImageHasAlphaAndSize() throws {
        let cg = try VBNImage.decode(data: makeVBN(width: 4, height: 4, format: 0x000, payload: bgra4x4Stored))
        XCTAssertEqual(cg.width, 4); XCTAssertEqual(cg.height, 4)
        XCTAssertEqual(cg.alphaInfo, .last)
        let png = try VBNImage.pngData(from: cg)
        XCTAssertEqual(Array(png.prefix(4)), [0x89, 0x50, 0x4E, 0x47])
    }

    // MARK: DXT5

    /// 8x4 = two BC3 blocks side by side.
    /// Block 0: alpha a0=255 > a1=0 (8-value mode), index of pixel j = j % 8;
    ///          colours c0 = 0xF800 red, c1 = 0x001F blue, index of pixel j = j % 4.
    /// Block 1: alpha a0=0 <= a1=255 (6-value mode + 0/255), index j % 8; colour index 0, c0 = 0x07E0 green.
    private func dxt8x4() -> [UInt8] {
        func alphaBits() -> [UInt8] {
            var bits: UInt64 = 0
            for j in 0 ..< 16 { bits |= UInt64(j % 8) << (3 * UInt64(j)) }
            return (0 ..< 6).map { UInt8((bits >> (8 * UInt64($0))) & 0xff) }
        }
        var colourBits: UInt32 = 0
        for j in 0 ..< 16 { colourBits |= UInt32(j % 4) << (2 * UInt32(j)) }
        let b0: [UInt8] = [255, 0] + alphaBits() + [0x00, 0xF8, 0x1F, 0x00] + le32(colourBits)
        let b1: [UInt8] = [0, 255] + alphaBits() + [0xE0, 0x07, 0xFF, 0xFF] + le32(0)
        return b0 + b1
    }

    func testDXT5Blocks() throws {
        let img = try VBNImage.decodeRGBA(data: makeVBN(width: 8, height: 4, format: 0x003, payload: dxt8x4()))
        XCTAssertEqual(img.header.pixelFormat, .dxt5)
        let alpha8: [UInt8] = [255, 0, 218, 182, 145, 109, 72, 36]   // ((6-i)*255)/7 ...
        let alpha6: [UInt8] = [0, 255, 51, 102, 153, 204, 0, 255]     // ((i+1)*255)/5 ..., 0, 255
        let colours: [[UInt8]] = [[255, 0, 0], [0, 0, 255], [170, 0, 85], [85, 0, 170]]
        for j in 0 ..< 16 {
            let sx = j & 3, sy = j >> 2           // position inside the block, stored order
            let y = 3 - sy                         // upright
            XCTAssertEqual(pixel(img, sx, y), colours[j % 4] + [alpha8[j % 8]], "block0 j=\(j)")
            XCTAssertEqual(pixel(img, 4 + sx, y), [0, 255, 0, alpha6[j % 8]], "block1 j=\(j)")
        }
    }

    func testDXT5PartialBlocksCropped() throws {
        // 5x3 needs 2x1 blocks; pixels outside the image are dropped.
        let img = try VBNImage.decodeRGBA(data: makeVBN(width: 5, height: 3, format: 0x003, payload: dxt8x4()))
        XCTAssertEqual(img.rgba.count, 5 * 3 * 4)
        XCTAssertEqual(pixel(img, 4, 2), [0, 255, 0, 0])  // stored row 0 of block 1, j = 0
    }

    // MARK: QuickLZ

    func testQuickLZAllLiterals() throws {
        XCTAssertEqual(try QuickLZ.decompress(qlzAllLiterals(bgra4x4Stored)), bgra4x4Stored)
        let img = try VBNImage.decodeRGBA(data: makeVBN(width: 4, height: 4, format: 0x100, payload: qlzAllLiterals(bgra4x4Stored)))
        assert4x4(img)
        XCTAssertTrue(img.header.compressed)
    }

    func testQuickLZLongAllLiteralsCrossesManyControlWords() throws {
        let data = (0 ..< 1000).map { UInt8(truncatingIfNeeded: $0 &* 7 &+ 3) }
        XCTAssertEqual(try QuickLZ.decompress(qlzAllLiterals(data)), data)
    }

    func testQuickLZBackReference() throws {
        // 4 literals "ABCD", then a 2-byte match token (offset 4, length 18: overlapping copy),
        // then 26 + 16 literals: 64 bytes total.
        let head: [UInt8] = [0x41, 0x42, 0x43, 0x44]
        let token: UInt32 = (4 << 6) | ((18 - 3) << 2) | 2   // fetch & 3 == 2 -> off = >>6, ml = ((>>2)&15)+3
        let lit1 = (0 ..< 26).map { UInt8(100 + $0) }
        let lit2 = (0 ..< 16).map { UInt8(200 + $0) }
        let body = le32((1 << 4) | (1 << 31)) + head + [UInt8(token & 0xff), UInt8(token >> 8)] + lit1
            + le32(0x8000_0000) + lit2
        var expected = head
        for i in 0 ..< 18 { expected.append(head[i % 4]) }
        expected += lit1 + lit2
        XCTAssertEqual(expected.count, 64)
        XCTAssertEqual(try QuickLZ.decompress(qlzBlock(body: body, dsize: 64)), expected)
    }

    func testQuickLZOneByteTokenAndQLZDXT5() throws {
        // A 1-byte token (fetch & 3 == 0): offset = byte >> 2, length 3. Wrap the DXT5 fixture: 32 bytes.
        let dxt = dxt8x4()
        let data = Array(dxt[0 ..< 16]) + Array(dxt[16 ..< 32])
        XCTAssertEqual(try QuickLZ.decompress(qlzAllLiterals(data)), data)
        // "xyz" then match offset 3 len 3 -> "xyzxyz", then literals to 20 bytes.
        let lits = (0 ..< 14).map { UInt8(0x30 + $0) }
        let body = le32((1 << 3) | (1 << 31)) + [0x78, 0x79, 0x7A] + [UInt8(3 << 2)] + lits
        XCTAssertEqual(try QuickLZ.decompress(qlzBlock(body: body, dsize: 20)), [0x78, 0x79, 0x7A, 0x78, 0x79, 0x7A] + lits)
        let img = try VBNImage.decodeRGBA(data: makeVBN(width: 8, height: 4, format: 0x103, payload: qlzAllLiterals(dxt)))
        XCTAssertEqual(img.header.pixelFormat, .dxt5)
        XCTAssertEqual(img.rgba, try VBNImage.decodeRGBA(data: makeVBN(width: 8, height: 4, format: 0x003, payload: dxt)).rgba)
    }

    func testQuickLZStoredBlock() throws {
        let block: [UInt8] = [0x4E] + le32(9 + 64) + le32(64) + bgra4x4Stored
        assert4x4(try VBNImage.decodeRGBA(data: makeVBN(width: 4, height: 4, format: 0x100, payload: block)))
    }

    // MARK: Errors

    func testBadMagic() {
        XCTAssertThrowsError(try VBNImage.decodeRGBA(data: makeVBN(width: 4, height: 4, format: 0, payload: bgra4x4Stored, magic: 0x1234_5678))) {
            XCTAssertEqual($0 as? VBNError, .badMagic(0x1234_5678))
        }
        XCTAssertThrowsError(try VBNImage.decodeRGBA(data: Data([0x89, 0x50, 0x4E, 0x47])))
    }

    func testTruncated() {
        let full = makeVBN(width: 4, height: 4, format: 0, payload: bgra4x4Stored)
        for n in [0, 3, 100, 0xC0, 0xC0 + 10] {
            XCTAssertThrowsError(try VBNImage.decodeRGBA(data: full.prefix(n)), "length \(n)") { err in
                guard case .truncated = err as? VBNError else { return XCTFail("\(n): \(err)") }
            }
        }
        // Header claims a payload that is too small for the pixels.
        XCTAssertThrowsError(try VBNImage.decodeRGBA(data: makeVBN(width: 4, height: 4, format: 0, payload: Array(bgra4x4Stored.prefix(60)))))
        // Compressed stream cut short.
        let q = qlzAllLiterals(bgra4x4Stored)
        XCTAssertThrowsError(try VBNImage.decodeRGBA(data: makeVBN(width: 4, height: 4, format: 0x100, payload: Array(q.prefix(40)))))
    }

    func testUnknownFormat() {
        for f: UInt32 in [0x001, 0x005, 0x203, 0x10003] {
            XCTAssertThrowsError(try VBNImage.decodeRGBA(data: makeVBN(width: 4, height: 4, format: f, payload: bgra4x4Stored))) {
                XCTAssertEqual($0 as? VBNError, .unknownFormat(f))
            }
        }
    }

    func testCorruptBackReference() {
        let body = le32(1 | (1 << 31)) + [UInt8(8 << 2)] + [UInt8](repeating: 0, count: 40)
        XCTAssertThrowsError(try QuickLZ.decompress(qlzBlock(body: body, dsize: 30)))
    }
}
