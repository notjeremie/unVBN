import Foundation

/// Builds a synthetic `.vbn` file: 192-byte header + payload.
func makeVBN(width: Int, height: Int, format: UInt32, payload: [UInt8], magic: UInt32 = 0x900D_F11E) -> Data {
    var h = [UInt8](repeating: 0, count: 0xC0)
    func put(_ v: UInt32, _ at: Int) { for i in 0 ..< 4 { h[at + i] = UInt8((v >> (8 * UInt32(i))) & 0xff) } }
    put(magic, 0x00); put(3, 0x04); put(4, 0x08)
    put(UInt32(width), 0x0C); put(UInt32(height), 0x10); put(4, 0x14)
    put(format, 0x18); put(5, 0x1C); put(UInt32(payload.count), 0x24); put(UInt32(payload.count), 0x40)
    return Data(h + payload)
}

func le32(_ v: UInt32) -> [UInt8] { (0 ..< 4).map { UInt8((v >> (8 * UInt32($0))) & 0xff) } }

/// QuickLZ L3 block, 9-byte header (flags 0x4F), from an already-built body (control words + data).
func qlzBlock(body: [UInt8], dsize: Int) -> [UInt8] {
    [0x4F] + le32(UInt32(9 + body.count)) + le32(UInt32(dsize)) + body
}

/// All-literal QuickLZ L3 stream: control word 0x80000000 (31 literal bits + sentinel), 31 bytes, repeat.
func qlzAllLiterals(_ data: [UInt8]) -> [UInt8] {
    var body = [UInt8]()
    var i = 0
    while i < data.count {
        body += le32(0x8000_0000)
        body += data[i ..< min(i + 31, data.count)]
        i += 31
    }
    return qlzBlock(body: body, dsize: data.count)
}

/// 4x4 BGRA test pixels in STORED (bottom-up) order. Stored row r, column x:
/// B = x, G = r, R = 0x10 * (x + 1) + r, A = 0x80 + 4 * r + x.
let bgra4x4Stored: [UInt8] = {
    var out = [UInt8]()
    for r in 0 ..< 4 {
        for x in 0 ..< 4 {
            let red: Int = 0x10 * (x + 1) + r
            let alpha: Int = 0x80 + 4 * r + x
            out += [UInt8(x), UInt8(r), UInt8(red), UInt8(alpha)]
        }
    }
    return out
}()

/// Expected upright RGBA for `bgra4x4Stored`: upright row y = stored row 3 - y.
func expectedRGBA4x4(x: Int, y: Int) -> [UInt8] {
    let r = 3 - y
    let red: Int = 0x10 * (x + 1) + r
    let alpha: Int = 0x80 + 4 * r + x
    return [UInt8(red), UInt8(r), UInt8(x), UInt8(alpha)]
}
