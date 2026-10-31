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

/// Rendering progress on stderr: a redrawn bar on a terminal, a line every 10 % when piped.
///   [██████████████░░░░░░░░░░]  58%  435/750  92 fps  ETA 3s
final class ProgressBar {
    let total: Int
    private let start = Date()
    private let tty = isatty(STDERR_FILENO) == 1
    private var lastDrawn = -1

    init(total: Int) { self.total = max(total, 1) }

    func update(_ done: Int) {
        let percent = done * 100 / total
        if tty {
            // Redraw at most once per percent (and on the last frame).
            guard percent != lastDrawn || done == total else { return }
        } else {
            guard percent / 10 != lastDrawn / 10 || done == total else { return }
        }
        lastDrawn = percent
        let width = 24
        let filled = done * width / total
        let elapsed = Date().timeIntervalSince(start)
        var line = "[" + String(repeating: "\u{2588}", count: filled) + String(repeating: "\u{2591}", count: width - filled) + "]"
        line += String(format: " %3d%%  %d/%d", percent, done, total)
        if done > 0, elapsed > 0.3 {
            let rate = Double(done) / elapsed
            line += String(format: "  %.0f fps  ETA %@", rate, Self.duration(Double(total - done) / rate))
        }
        write(tty ? "\r  " + line + "\u{1B}[K" : "  " + line + "\n")
    }

    func finish(seconds: Double) {
        write((tty ? "\r\u{1B}[K" : "") + String(format: "  done: %d frames in %.1f s\n", total, seconds))
    }

    static func duration(_ s: Double) -> String {
        let n = Int(s.rounded(.up))
        return n < 60 ? "\(n)s" : "\(n / 60)m\(String(format: "%02d", n % 60))s"
    }

    private func write(_ s: String) { FileHandle.standardError.write(Data(s.utf8)) }
}

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
    let bar = ProgressBar(total: seq.frames.count)
    do {
        try SequenceExporter(sequence: seq, options: options, output: out).run(progress: { done, _ in bar.update(done) })
        bar.finish(seconds: Date().timeIntervalSince(start))
    } catch {
        FileHandle.standardError.write(Data("\n".utf8))
        FileHandle.standardError.write(Data("unvbn: \(seq.name): \(error.localizedDescription)\n".utf8))
        failures += 1
    }
}
exit(failures == 0 ? 0 : 1)
