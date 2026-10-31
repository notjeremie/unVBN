// Draws the unVBN app icon ("broadcast badge") and writes a 1024×1024 PNG.
// Usage: swift scripts/make-icon.swift <out.png>   (scripts/make-icon.sh builds the whole icon set)
import AppKit

let S: CGFloat = 1024
let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "icon-1024.png"

let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(S), pixelsHigh: Int(S), bitsPerSample: 8,
                           samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
                           bytesPerRow: 0, bitsPerPixel: 0)!
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
let ctx = NSGraphicsContext.current!.cgContext

// macOS icon grid: 824-pt tile centred on the 1024 canvas, ~185-pt corner radius.
let tile = CGRect(x: 100, y: 100, width: 824, height: 824)
let shape = CGPath(roundedRect: tile, cornerWidth: 185, cornerHeight: 185, transform: nil)

// Soft drop shadow under the tile.
ctx.saveGState()
ctx.setShadow(offset: CGSize(width: 0, height: -12), blur: 28, color: NSColor.black.withAlphaComponent(0.35).cgColor)
ctx.addPath(shape); ctx.setFillColor(NSColor.black.cgColor); ctx.fillPath()
ctx.restoreGState()

// Dark tile with a vertical gradient.
ctx.saveGState()
ctx.addPath(shape); ctx.clip()
let grad = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(),
                      colors: [NSColor(srgbRed: 0.20, green: 0.21, blue: 0.25, alpha: 1).cgColor,
                               NSColor(srgbRed: 0.06, green: 0.065, blue: 0.08, alpha: 1).cgColor] as CFArray,
                      locations: [0, 1])!
ctx.drawLinearGradient(grad, start: CGPoint(x: 0, y: tile.maxY), end: CGPoint(x: 0, y: tile.minY), options: [])
ctx.restoreGState()

// Hairline highlight on the edge.
ctx.addPath(shape); ctx.setStrokeColor(NSColor.white.withAlphaComponent(0.10).cgColor); ctx.setLineWidth(4); ctx.strokePath()

// "VBN" wordmark.
let font = NSFont.systemFont(ofSize: 290, weight: .black)
let attrs: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: NSColor.white, .kern: 6]
let word = NSAttributedString(string: "VBN", attributes: attrs)
let ws = word.size()
let textY = 520 - ws.height / 2 + 20
word.draw(at: CGPoint(x: (S - ws.width) / 2, y: textY))

// Lower-third bar under the wordmark.
let barW = ws.width * 0.72, barH: CGFloat = 22
let bar = CGRect(x: (S - barW) / 2, y: textY - 40, width: barW, height: barH)
ctx.addPath(CGPath(roundedRect: bar, cornerWidth: barH / 2, cornerHeight: barH / 2, transform: nil))
ctx.setFillColor(NSColor.white.withAlphaComponent(0.32).cgColor); ctx.fillPath()

// Red "on air" dot with a glow, top-right.
let dotR: CGFloat = 46
let dot = CGRect(x: tile.maxX - 150 - dotR, y: tile.maxY - 150 - dotR, width: dotR * 2, height: dotR * 2)
let red = NSColor(srgbRed: 1.0, green: 0.23, blue: 0.19, alpha: 1).cgColor
ctx.saveGState()
ctx.setShadow(offset: .zero, blur: 40, color: red)
ctx.setFillColor(red); ctx.fillEllipse(in: dot)
ctx.restoreGState()

NSGraphicsContext.restoreGraphicsState()
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: out))
print("wrote \(out)")
