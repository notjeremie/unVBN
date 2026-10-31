import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

/// Header fields of a Vizrt `.vbn` image (see FORMAT.md).
public struct VBNHeader: Equatable {
    public static let magic: UInt32 = 0x900D_F11E
    public static let size = 0xC0

    public enum PixelFormat: Equatable { case rgba8888, dxt5 }

    public var version: UInt32
    public var width: Int
    public var height: Int
    public var channels: UInt32
    public var rawFormat: UInt32
    public var pixelFormat: PixelFormat
    public var compressed: Bool
    public var payloadSize: Int

    public init(bytes b: [UInt8]) throws {
        guard b.count >= 4 else { throw VBNError.truncated("header") }
        let magic = readU32(b, 0)
        guard magic == Self.magic else { throw VBNError.badMagic(magic) }
        guard b.count >= Self.size else { throw VBNError.truncated("header") }
        version = readU32(b, 0x04)
        guard version == 3 else { throw VBNError.unsupportedVersion(version) }
        let w = readU32(b, 0x0C), h = readU32(b, 0x10)
        channels = readU32(b, 0x14)
        // 16384 is far above any Viz texture; it keeps a corrupt header from asking for gigabytes.
        guard w > 0, h > 0, w <= 16384, h <= 16384, channels == 4 else {
            throw VBNError.invalidDimensions(width: w, height: h, channels: channels)
        }
        width = Int(w); height = Int(h)
        rawFormat = readU32(b, 0x18)
        guard rawFormat & ~0x1FF == 0 else { throw VBNError.unknownFormat(rawFormat) }
        switch rawFormat & 0xFF {
        case 0: pixelFormat = .rgba8888
        case 3: pixelFormat = .dxt5
        default: throw VBNError.unknownFormat(rawFormat)
        }
        compressed = rawFormat & 0x100 != 0
        payloadSize = Int(readU32(b, 0x24))
    }

    /// Bytes of pixel data once decompressed.
    public var pixelDataSize: Int {
        switch pixelFormat {
        case .rgba8888: return width * height * 4
        case .dxt5: return BC3.payloadSize(width: width, height: height)
        }
    }
}

/// A decoded `.vbn` image: RGBA8, non-premultiplied, rows top-down (upright).
public struct VBNImage {
    public let width: Int
    public let height: Int
    public let rgba: [UInt8]
    public let header: VBNHeader

    public static func decodeRGBA(data: Data) throws -> VBNImage {
        let bytes = [UInt8](data)
        let header = try VBNHeader(bytes: bytes)
        let start = VBNHeader.size
        guard bytes.count >= start + header.payloadSize else {
            throw VBNError.truncated("payload: \(header.payloadSize) bytes declared, \(bytes.count - start) present")
        }
        var payload = Array(bytes[start ..< start + header.payloadSize])
        if header.compressed { payload = try QuickLZ.decompress(payload) }
        guard payload.count >= header.pixelDataSize else {
            throw VBNError.truncated("pixel data: \(header.pixelDataSize) bytes expected, \(payload.count) present")
        }

        let w = header.width, h = header.height
        var stored: [UInt8]
        switch header.pixelFormat {
        case .dxt5:
            stored = try BC3.decode(payload, width: w, height: h)
        case .rgba8888:
            // Uncompressed pixels are stored R, G, B, A (verified on real files: a red logo decodes red).
            stored = Array(payload[0 ..< w * h * 4])
        }

        // Rows are stored bottom-up (OpenGL): flip.
        let row = w * 4
        var upright = [UInt8](repeating: 0, count: w * h * 4)
        for y in 0 ..< h {
            let s = (h - 1 - y) * row
            upright.replaceSubrange(y * row ..< (y + 1) * row, with: stored[s ..< s + row])
        }
        return VBNImage(width: w, height: h, rgba: upright, header: header)
    }

    /// Decodes a `.vbn` file into an upright CGImage with (non-premultiplied) alpha.
    public static func decode(data: Data) throws -> CGImage {
        try decodeRGBA(data: data).cgImage()
    }

    public func cgImage() throws -> CGImage {
        let provider = CGDataProvider(data: Data(rgba) as CFData)!
        guard let image = CGImage(
            width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: width * 4,
            space: CGColorSpace(name: CGColorSpace.sRGB)!,
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue),
            provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent
        ) else { throw VBNError.corrupt("could not build a CGImage") }
        return image
    }

    /// PNG encoding of a CGImage.
    public static func pngData(from image: CGImage) throws -> Data {
        let out = NSMutableData()
        guard let dest = CGImageDestinationCreateWithData(out, UTType.png.identifier as CFString, 1, nil) else {
            throw VBNError.corrupt("could not create a PNG encoder")
        }
        CGImageDestinationAddImage(dest, image, nil)
        guard CGImageDestinationFinalize(dest) else { throw VBNError.corrupt("PNG encoding failed") }
        return out as Data
    }

    /// Convenience: `.vbn` bytes to PNG bytes.
    public static func pngData(fromVBN data: Data) throws -> Data {
        try pngData(from: decode(data: data))
    }
}
