import Foundation

public enum VBNError: Error, LocalizedError, Equatable {
    case badMagic(UInt32)
    case unsupportedVersion(UInt32)
    case truncated(String)
    case unknownFormat(UInt32)
    case invalidDimensions(width: UInt32, height: UInt32, channels: UInt32)
    case corrupt(String)

    public var errorDescription: String? {
        switch self {
        case .badMagic(let m): return String(format: "Not a Vizrt .vbn file (magic 0x%08X, expected 0x900DF11E).", m)
        case .unsupportedVersion(let v): return "Unsupported .vbn version \(v) (only version 3 is known)."
        case .truncated(let what): return "The .vbn file is truncated (\(what))."
        case .unknownFormat(let f): return String(format: "Unknown .vbn pixel format 0x%X (known: RGBA8888, DXT5, optionally QuickLZ-compressed).", f)
        case .invalidDimensions(let w, let h, let c): return "Invalid .vbn dimensions \(w)x\(h), \(c) channels."
        case .corrupt(let why): return "The .vbn file is corrupt: \(why)."
        }
    }
}
