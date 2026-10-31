// QuickLZ 1.5 level-3 decompressor, ported 1:1 from reference/vbn.py (qlz3).
// Bounds are checked everywhere the Python would raise, so corrupt input throws
// instead of reading past the buffer.

public enum QuickLZ {
    /// Decompresses one QuickLZ 1.5 level-3 block (3- or 9-byte header).
    public static func decompress(_ input: [UInt8]) throws -> [UInt8] {
        guard input.count >= 3 else { throw VBNError.truncated("QuickLZ header") }
        let flags = input[0]
        let csize: Int, dsize: Int
        var p: Int
        if flags & 2 != 0 {
            guard input.count >= 9 else { throw VBNError.truncated("QuickLZ header") }
            csize = Int(readU32(input, 1)); dsize = Int(readU32(input, 5)); p = 9
        } else {
            csize = Int(input[1]); dsize = Int(input[2]); p = 3
        }
        if flags & 1 == 0 {  // stored
            guard input.count >= p + dsize else { throw VBNError.truncated("QuickLZ stored block") }
            return Array(input[p ..< p + dsize])
        }
        guard csize >= p, input.count >= csize else { throw VBNError.truncated("QuickLZ block (\(csize) bytes declared, \(input.count) present)") }
        if dsize == 0 { return [] }

        // The reference pads 8 zero bytes so 4-byte fetches near the end stay in range.
        var src = Array(input[0 ..< csize])
        src.append(contentsOf: repeatElement(0, count: 8))

        var dst = [UInt8]()
        dst.reserveCapacity(dsize)
        let last = dsize - 1
        var cword: UInt32 = 1

        func fetch32(_ at: Int) throws -> UInt32 {
            guard at >= 0, at + 4 <= src.count else { throw VBNError.corrupt("QuickLZ stream ends early") }
            return readU32(src, at)
        }

        while true {
            if cword == 1 { cword = try fetch32(p); p += 4 }
            let fetch = try fetch32(p)
            if cword & 1 != 0 {
                cword >>= 1
                let off: Int, ml: Int
                if fetch & 3 == 0 {
                    off = Int((fetch & 0xff) >> 2); ml = 3; p += 1
                } else if fetch & 2 == 0 {
                    off = Int((fetch & 0xffff) >> 2); ml = 3; p += 2
                } else if fetch & 1 == 0 {
                    off = Int((fetch & 0xffff) >> 6); ml = Int((fetch >> 2) & 15) + 3; p += 2
                } else if fetch & 127 != 3 {
                    off = Int((fetch >> 7) & 0x1ffff); ml = Int((fetch >> 2) & 0x1f) + 2; p += 3
                } else {
                    off = Int(fetch >> 15); ml = Int((fetch >> 7) & 255) + 3; p += 4
                }
                let s = dst.count - off
                guard off >= 1, s >= 0 else { throw VBNError.corrupt("QuickLZ back-reference before start of output") }
                guard dst.count + ml <= dsize else { throw VBNError.corrupt("QuickLZ match runs past declared size") }
                for i in 0 ..< ml { dst.append(dst[s + i]) }  // byte by byte: overlapping copies are intended
            } else if dst.count < last - 10 {
                guard p < src.count else { throw VBNError.corrupt("QuickLZ stream ends early") }
                dst.append(src[p]); p += 1; cword >>= 1
            } else {
                // Tail rule: the last bytes are always literals; control words are skipped.
                while dst.count <= last {
                    if cword == 1 { p += 4; cword = 1 << 31 }
                    guard p < src.count else { throw VBNError.corrupt("QuickLZ stream ends early") }
                    dst.append(src[p]); p += 1; cword >>= 1
                }
                return dst
            }
        }
    }
}

@inline(__always)
func readU32(_ b: [UInt8], _ at: Int) -> UInt32 {
    UInt32(b[at]) | UInt32(b[at + 1]) << 8 | UInt32(b[at + 2]) << 16 | UInt32(b[at + 3]) << 24
}
