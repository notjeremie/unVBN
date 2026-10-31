import Foundation
import VBNCore
import VBNExport

let usage = """
usage: unvbn <folder | frame.vbn>... [-f png|mp4|webm] [--fps 50] [-o OUTPUT] [--background #000000] [--crf 30]
       unvbn info <folder | frame.vbn>...

Converts Vizrt .vbn sequences (NAME_00000.vbn, NAME_00001.vbn, ...) to a PNG sequence, MP4 (H.264)
or WebM (VP9 + alpha). A folder holding several sequences converts each one.

  -f, --format      png (default; a folder of PNGs), mp4, webm
  --fps             frames per second (default 50)
  -o, --output      output file (or folder for png). Default: next to the sequence folder,
                    NAME.mp4 / NAME.webm / NAME_png/ ("NAME (FOLDER)" when several sequences
                    share a name). Only with a single sequence.
  --background      MP4 only (no alpha): colour behind transparent pixels (default #000000)
  --crf             WebM quality, 0 (best) to 63 (default 30)
"""

/// Writes to stderr.
func eprint(_ s: String, terminator: String = "\n") { FileHandle.standardError.write(Data((s + terminator).utf8)) }

func fail(_ message: String, code: Int32 = 1) -> Never {
    eprint("unvbn: \(message)")
    exit(code)
}

/// Rendering progress on stderr: a redrawn bar on a terminal, a line every 10 % when piped.
///   [██████████████░░░░░░░░░░]  58%  435/750  92 fps  ETA 3 s
final class ProgressBar {
    private let tty = isatty(STDERR_FILENO) == 1
    private var lastDrawn = -1

    func update(_ p: ExportProgress) {
        // The exporter already reports at most once per percent; when piped, keep one line per 10 %.
        guard tty || p.percent / 10 != lastDrawn / 10 || p.done == p.total else { return }
        lastDrawn = p.percent
        let width = 24, filled = p.done * width / max(p.total, 1)
        var line = "[" + String(repeating: "\u{2588}", count: filled) + String(repeating: "\u{2591}", count: width - filled) + "]"
        line += String(format: " %3d%%  %d/%d", p.percent, p.done, p.total)
        if let fps = p.framesPerSecond, let left = p.remaining {
            line += String(format: "  %.0f fps  ETA ", fps) + ExportProgress.formatDuration(left)
        }
        eprint(tty ? "\r  " + line + "\u{1B}[K" : "  " + line, terminator: tty ? "" : "\n")
    }

    func finish(frames: Int, seconds: Double) {
        eprint((tty ? "\r\u{1B}[K" : "") + String(format: "  done: %d frames in %.1f s", frames, seconds))
    }

    /// Ends a half-drawn bar line before an error message.
    func interrupt() { if tty, lastDrawn >= 0 { eprint("") } }
}

/// Reads only the 192-byte header of a frame.
func header(of url: URL) -> VBNHeader? {
    guard let handle = try? FileHandle(forReadingFrom: url) else { return nil }
    defer { try? handle.close() }
    guard let bytes = try? handle.read(upToCount: VBNHeader.size) else { return nil }
    return try? VBNHeader(bytes: [UInt8](bytes))
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
        guard let f = ExportFormat(rawValue: v.lowercased()) else { fail("unknown format \(v) (png, mp4, webm)") }
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
        if let h = header(of: seq.frames[0].url) {
            line += ", \(h.width)×\(h.height) \(h.pixelFormat == .dxt5 ? "DXT5" : "RGBA")\(h.compressed ? " QuickLZ" : "")"
        }
        let missing = seq.missingNumbers
        if !missing.isEmpty { line += ", \(missing.count) missing frame number(s)" }
        print(line)
    }
    exit(0)
}

if outputPath != nil, sequences.count > 1 { fail("-o works with a single sequence; \(sequences.count) were found") }

let outputs = outputPath.map { [URL(fileURLWithPath: $0)] } ?? options.format.defaultOutputs(for: sequences)
let jobs = zip(sequences, outputs).map { (sequence: $0, output: $1) }
var bar = ProgressBar(), start = Date()
let result = BatchExporter(jobs: jobs, options: options).run { event in
    switch event {
    case .started(let i):
        let (seq, out) = jobs[i]
        let missing = seq.missingNumbers
        if !missing.isEmpty {
            eprint("unvbn: warning: \(seq.name) has \(missing.count) missing frame number(s); they are skipped")
        }
        eprint("\(seq.name): \(seq.frames.count) frames -> \(out.path)")
        bar = ProgressBar(); start = Date()
    case let .progress(_, _, current): bar.update(current)
    case let .finished(i, nil): bar.finish(frames: jobs[i].sequence.frames.count, seconds: Date().timeIntervalSince(start))
    case let .finished(i, error?):
        bar.interrupt()
        eprint("unvbn: \(jobs[i].sequence.name): \(error.localizedDescription)")
    }
}
exit(result.failures.isEmpty ? 0 : 1)
