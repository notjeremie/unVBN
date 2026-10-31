// DXT5 / BC3 decoder, ported 1:1 from reference/dxt.py (integer rounding included).

public enum BC3 {
    /// Size in bytes of a BC3 payload for the given image size.
    public static func payloadSize(width: Int, height: Int) -> Int {
        ((width + 3) / 4) * ((height + 3) / 4) * 16
    }

    /// Decodes BC3 blocks to RGBA8 (non-premultiplied), rows in stored order.
    public static func decode(_ data: [UInt8], width w: Int, height h: Int) throws -> [UInt8] {
        guard data.count >= payloadSize(width: w, height: h) else { throw VBNError.truncated("DXT5 payload") }
        var out = [UInt8](repeating: 0, count: w * h * 4)
        let bw = (w + 3) / 4, bh = (h + 3) / 4
        var p = 0
        var al = [Int](repeating: 0, count: 8)
        var cl = [(Int, Int, Int)](repeating: (0, 0, 0), count: 4)
        for by in 0 ..< bh {
            for bx in 0 ..< bw {
                let a0 = Int(data[p]), a1 = Int(data[p + 1])
                var abits: UInt64 = 0
                for i in 0 ..< 6 { abits |= UInt64(data[p + 2 + i]) << (8 * UInt64(i)) }
                al[0] = a0; al[1] = a1
                if a0 > a1 {
                    for i in 0 ..< 6 { al[2 + i] = ((6 - i) * a0 + (i + 1) * a1) / 7 }
                } else {
                    for i in 0 ..< 4 { al[2 + i] = ((4 - i) * a0 + (i + 1) * a1) / 5 }
                    al[6] = 0; al[7] = 255
                }
                let c0 = Int(data[p + 8]) | Int(data[p + 9]) << 8
                let c1 = Int(data[p + 10]) | Int(data[p + 11]) << 8
                let bits = readU32(data, p + 12)
                let r0 = c565(c0), r1 = c565(c1)
                cl[0] = r0; cl[1] = r1
                cl[2] = ((2 * r0.0 + r1.0) / 3, (2 * r0.1 + r1.1) / 3, (2 * r0.2 + r1.2) / 3)
                cl[3] = ((r0.0 + 2 * r1.0) / 3, (r0.1 + 2 * r1.1) / 3, (r0.2 + 2 * r1.2) / 3)
                for j in 0 ..< 16 {
                    let x = bx * 4 + (j & 3), y = by * 4 + (j >> 2)
                    guard x < w, y < h else { continue }
                    let o = (y * w + x) * 4
                    let c = cl[Int((bits >> (2 * UInt32(j))) & 3)]
                    out[o] = UInt8(c.0); out[o + 1] = UInt8(c.1); out[o + 2] = UInt8(c.2)
                    out[o + 3] = UInt8(al[Int((abits >> (3 * UInt64(j))) & 7)])
                }
                p += 16
            }
        }
        return out
    }

    @inline(__always)
    static func c565(_ c: Int) -> (Int, Int, Int) {
        (((c >> 11) & 31) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31)
    }
}
