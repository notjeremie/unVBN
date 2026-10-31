import Foundation
import VBNCore
import VBNExport

let usage = """
usage: unvbn <folder | frame.vbn>... [-f png|mp4|webm|prores] [--fps 50] [-o OUTPUT] [--background #000000] [--crf 30]
       unvbn info <folder | frame.vbn>...

Converts Vizrt .vbn sequences (NAME_00000.vbn, NAME_00001.vbn, ...) to a PNG sequence, MP4 (H.264),
WebM (VP9 + alpha) or MOV (ProRes 4444 + alpha). A folder holding several sequences converts each one.

  -f, --format      png (default; a folder of PNGs), mp4, webm, prores
  --fps             frames per second (default 50)
  -o, --output      output file (or folder for png). Default: next to the sequence folder,
                    NAME.mp4 / NAME.webm / NAME.mov / NAME_png/. Only with a single sequence.
  --background      MP4 only (no alpha): colour behind transparent pixels (default #000000)
  --crf             WebM quality, 0 (best) to 63 (default 30)
"""

func fail(_ message: String, code: Int32 = 1) -> Never {
    FileHandle.standardError.write(Data("unvbn: \(message)\n".utf8))
    exit(code)
}

var args = Array(CommandLine.arguments.dropFirst())
if args.isEmpty || args.contains("-h") || args.contains("--help") { print(usage); exit(args.isEmpty ? 1 : 0) }

let infoOnly = args.first == "info"
if infoOnly { args.removeFirst() }

var options = ExportOptions(format: .png)
var outputPath: String?
var inputs: [String] = []
var i = 0
func value() -> String {
    i += 1
    guard i < args.count else { fail("\(args[i - 1]) needs a value") }
    return args[i]
}
while i < args.count {
    switch args[i] {
    case "-f", "--format":
        let v = value()
        guard let f = ExportFormat(rawValue: v.lowercased()) else { fail("unknown format \(v) (png, mp4, webm, prores)") }
        options.format = f
    case "--fps":
        let v = value()
        guard let fps = Double(v), fps > 0 else { fail("invalid --fps \(v)") }
        options.fps = fps
    case "-o", "--output": outputPath = value()
    case "--background":
        let v = value()
        guard let c = RGBColor(hex: v) else { fail("invalid --background \(v) (use #RRGGBB)") }
        options.background = c
    case "--crf":
        let v = value()
        guard let crf = Int(v), (0 ... 63).contains(crf) else { fail("invalid --crf \(v) (0-63)") }
        options.webmCRF = crf
    case let a where a.hasPrefix("-"): fail("unknown option \(a)\n\n\(usage)")
    default: inputs.append(args[i])
    }
    i += 1
}
guard !inputs.isEmpty else { fail("no input\n\n\(usage)") }

var sequences: [FrameSequence] = []
for input in inputs {
    do { sequences += try FrameSequence.find(at: URL(fileURLWithPath: input)) }
    catch { fail(error.localizedDescription) }
}

if infoOnly {
    for seq in sequences {
        var line = "\(seq.name): \(seq.frames.count) frame\(seq.frames.count == 1 ? "" : "s") in \(seq.directory.path)"
        if let data = try? Data(contentsOf: seq.frames[0].url), let h = try? VBNHeader(bytes: [UInt8](data.prefix(VBNHeader.size))) {
            line += ", \(h.width)×\(h.height) \(h.pixelFormat == .dxt5 ? "DXT5" : "BGRA")\(h.compressed ? " QuickLZ" : "")"
        }
        let missing = seq.missingNumbers
        if !missing.isEmpty { line += ", \(missing.count) missing frame number(s)" }
        print(line)
    }
    exit(0)
}

if outputPath != nil, sequences.count > 1 { fail("-o works with a single sequence; \(sequences.count) were found") }

var failures = 0
for seq in sequences {
    let out = outputPath.map { URL(fileURLWithPath: $0) } ?? options.format.defaultOutput(for: seq)
    let missing = seq.missingNumbers
    if !missing.isEmpty {
        FileHandle.standardError.write(Data("unvbn: warning: \(seq.name) has \(missing.count) missing frame number(s); they are skipped\n".utf8))
    }
    FileHandle.standardError.write(Data("\(seq.name): \(seq.frames.count) frames -> \(out.path)\n".utf8))
    let start = Date()
    var lastPercent = -1
    do {
        try SequenceExporter(sequence: seq, options: options, output: out).run(progress: { done, total in
            let percent = done * 100 / total
            guard percent != lastPercent else { return }
            lastPercent = percent
            FileHandle.standardError.write(Data("\r  \(percent)% (\(done)/\(total))".utf8))
        })
        FileHandle.standardError.write(Data(String(format: "\r  done in %.1f s          \n", Date().timeIntervalSince(start)).utf8))
    } catch {
        FileHandle.standardError.write(Data("\n".utf8))
        FileHandle.standardError.write(Data("unvbn: \(seq.name): \(error.localizedDescription)\n".utf8))
        failures += 1
    }
}
exit(failures == 0 ? 0 : 1)
