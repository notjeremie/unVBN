import struct, sys, zlib
def qlz3(src):
    flags = src[0]
    if flags & 2: csize, dsize = struct.unpack_from('<II', src, 1); p = 9
    else: csize, dsize = src[1], src[2]; p = 3
    if not flags & 1: return bytes(src[p:p + dsize])          # stored
    src = bytes(src) + b'\0' * 8
    dst = bytearray(); last = dsize - 1; cword = 1
    while True:
        if cword == 1: cword = struct.unpack_from('<I', src, p)[0]; p += 4
        fetch = struct.unpack_from('<I', src, p)[0]
        if cword & 1:
            cword >>= 1
            if fetch & 3 == 0: off = (fetch & 0xff) >> 2; ml = 3; p += 1
            elif fetch & 2 == 0: off = (fetch & 0xffff) >> 2; ml = 3; p += 2
            elif fetch & 1 == 0: off = (fetch & 0xffff) >> 6; ml = ((fetch >> 2) & 15) + 3; p += 2
            elif fetch & 127 != 3: off = (fetch >> 7) & 0x1ffff; ml = ((fetch >> 2) & 0x1f) + 2; p += 3
            else: off = fetch >> 15; ml = ((fetch >> 7) & 255) + 3; p += 4
            s = len(dst) - off
            for i in range(ml): dst.append(dst[s + i])
        else:
            if len(dst) < last - 10:
                dst.append(src[p]); p += 1; cword >>= 1
            else:
                while len(dst) <= last:
                    if cword == 1: p += 4; cword = 1 << 31
                    dst.append(src[p]); p += 1; cword >>= 1
                return bytes(dst)
def read(path):
    b = open(path, 'rb').read()
    magic, ver, a, w, h, bpp = struct.unpack_from('<6I', b, 0)
    return b, dict(magic=hex(magic), ver=ver, a=a, w=w, h=h, bpp=bpp, f18=struct.unpack_from('<4I', b, 0x18))
def png(path, w, h, rgba):
    raw = b''.join(b'\0' + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    ch = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + ch(b'IDAT', zlib.compress(raw, 6)) + ch(b'IEND', b''))
if __name__ == '__main__':
    for f in sys.argv[1:]:
        b, hd = read(f)
        try:
            px = qlz3(b[0xc0:])
            ok = len(px) == hd['w'] * hd['h'] * hd['bpp']
            print(f.split('/')[-1], hd, 'qlz hdr', b[0xc0:0xc9].hex(), 'out', len(px), 'OK' if ok else 'SIZE MISMATCH')
            if ok and hd['bpp'] == 4:
                bgra = px; rgba = bytearray(len(bgra)); rgba[0::4] = bgra[2::4]; rgba[1::4] = bgra[1::4]; rgba[2::4] = bgra[0::4]; rgba[3::4] = bgra[3::4]
                png(f[:-4] + '.png', hd['w'], hd['h'], bytes(rgba))
        except Exception as e: print(f.split('/')[-1], hd, 'ERR', repr(e), b[0xc0:0xc9].hex())
