import struct
def c565(c): return (((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)
def bc3(data, w, h):
    out = bytearray(w * h * 4); bw = (w + 3) // 4; bh = (h + 3) // 4; p = 0
    for by in range(bh):
        for bx in range(bw):
            a0, a1 = data[p], data[p + 1]
            abits = int.from_bytes(data[p + 2:p + 8], 'little')
            al = [a0, a1] + ([((6 - i) * a0 + (i + 1) * a1) // 7 for i in range(6)] if a0 > a1 else [((4 - i) * a0 + (i + 1) * a1) // 5 for i in range(4)] + [0, 255])
            c0, c1 = struct.unpack_from('<HH', data, p + 8); bits = struct.unpack_from('<I', data, p + 12)[0]
            r0, r1 = c565(c0), c565(c1)
            cl = [r0, r1, tuple((2 * a + b) // 3 for a, b in zip(r0, r1)), tuple((a + 2 * b) // 3 for a, b in zip(r0, r1))]
            for j in range(16):
                x, y = bx * 4 + (j & 3), by * 4 + (j >> 2)
                if x < w and y < h:
                    o = (y * w + x) * 4; r, g, b = cl[(bits >> (2 * j)) & 3]
                    out[o:o + 4] = bytes((r, g, b, al[(abits >> (3 * j)) & 7]))
            p += 16
    return bytes(out)
