import AppKit
import SwiftUI
import UniformTypeIdentifiers
import VBNExport

/// State of the export panel: the sequences found in what was dropped, the chosen settings and the job.
/// One folder or frame: pick one of its sequences. Several folders: a batch, every sequence exported in
/// turn with the same settings.
@MainActor
final class ExportModel: ObservableObject {
    @Published var sequences: [FrameSequence] = []
    @Published var selected = 0
    @Published private(set) var isBatch = false
    /// Missing frame numbers per sequence, counted once at load (the view redraws on every progress tick).
    @Published private(set) var missingCounts: [Int] = []
    /// While a batch runs: the sequence being exported, 1-based.
    @Published private(set) var batchIndex: Int?
    private var isLoading = false
    @Published var format: ExportFormat = .webm
    @Published var fps: Double = 50
    @Published var background: Color = .black
    /// The running export's progress; nil when idle. AppDelegate observes it for the window title and Dock tile.
    @Published private(set) var progress: ExportProgress?
    @Published private(set) var status: String?
    @Published private(set) var lastOutputs: [URL] = []
    @Published var errorMessage: String?

    let ffmpeg = FFmpeg.locate()
    private var job: Task<Void, Never>?

    var sequence: FrameSequence? { sequences.indices.contains(selected) ? sequences[selected] : nil }
    var isRunning: Bool { progress != nil }

    /// Frames and missing frame numbers of what Export exports: every sequence in a batch, else the selected one.
    var frameCounts: (frames: Int, missing: Int) {
        let shown = isBatch ? Array(sequences.indices) : sequences.indices.contains(selected) ? [selected] : []
        return shown.reduce((0, 0)) { ($0.0 + sequences[$1].frames.count, $0.1 + missingCounts[$1]) }
    }

    /// Loads what was dropped or opened, scanning the folders off the main thread (they may be on a network
    /// share). Several items make a batch; an item without frames is skipped and reported, the others load.
    func load(_ urls: [URL]) {
        guard !isRunning, !isLoading, !urls.isEmpty else { return }
        isLoading = true
        Task {
            let scan = await Task.detached(priority: .userInitiated) { Self.scan(urls) }.value
            isLoading = false
            status = nil; lastOutputs = []
            errorMessage = scan.problems.isEmpty ? nil : scan.problems.joined(separator: "\n")
            guard !scan.found.isEmpty else { return }
            sequences = scan.found
            missingCounts = scan.found.map(\.missingNumbers.count)
            selected = 0
            isBatch = urls.count > 1
        }
    }

    private nonisolated static func scan(_ urls: [URL]) -> (found: [FrameSequence], problems: [String]) {
        var found: [FrameSequence] = [], problems: [String] = []
        for url in urls {
            do { found += try FrameSequence.find(at: url) } catch { problems.append(error.localizedDescription) }
        }
        return (found, problems)
    }

    func chooseAndExport() {
        guard !isRunning else { return }
        let jobs: [BatchExporter.Job]
        if isBatch {
            guard let outputs = askBatchDestination() else { return }
            jobs = zip(sequences, outputs).map { ($0, $1) }
        } else {
            guard let sequence, let output = askDestination(for: sequence) else { return }
            jobs = [(sequence, output)]
        }
        var options = ExportOptions(format: format, fps: fps)
        options.background = rgb(background)

        progress = ExportProgress(done: 0, total: jobs.reduce(0) { $0 + $1.sequence.frames.count }, elapsed: 0)
        batchIndex = nil
        status = nil; lastOutputs = []; errorMessage = nil
        let exporter = BatchExporter(jobs: jobs, options: options, ffmpeg: ffmpeg)
        let isBatch = self.isBatch
        job = Task.detached(priority: .userInitiated) { [weak self] in
            let start = Date()
            let result = exporter.run({ event in
                switch event {
                case .started(let index) where isBatch:
                    Task { @MainActor in self?.batchIndex = index + 1 }
                case let .progress(_, overall, _):
                    Task { @MainActor in if self?.isRunning == true { self?.progress = overall } }
                default: break
                }
            }, isCancelled: { Task.isCancelled })
            await self?.finish(result, count: jobs.count, seconds: Date().timeIntervalSince(start))
        }
    }

    func cancel() { job?.cancel() }

    private func finish(_ result: BatchExporter.Result, count: Int, seconds: TimeInterval) {
        progress = nil; batchIndex = nil; job = nil
        lastOutputs = result.exported
        errorMessage = result.failures.isEmpty ? nil : result.failures.map {
            count > 1 ? "\($0.sequence.name): \($0.error.localizedDescription)" : $0.error.localizedDescription
        }.joined(separator: "\n")
        let time = String(format: "%.1f s", seconds), exported = result.exported.count
        if count == 1 {
            if let output = result.exported.first { status = "Exported \(output.lastPathComponent) in \(time)." }
        } else if result.cancelled {
            status = "Cancelled after \(exported) of \(count) sequences."
        } else {
            status = exported == count ? "Exported \(count) sequences in \(time)." : "Exported \(exported) of \(count) sequences in \(time)."
        }
        if !result.cancelled, exported > 0 { NSApp.requestUserAttention(.informationalRequest) }
    }

    /// "Sequence 2 / 5 · Rendering frame 312 / 750 · 85 fps · 3 s left" (frames counted over the whole batch)
    var progressLabel: String {
        guard let p = progress else { return "" }
        var parts = ["Rendering frame \(p.done) / \(p.total)"]
        if let i = batchIndex { parts.insert("Sequence \(i) / \(sequences.count)", at: 0) }
        if let fps = p.framesPerSecond { parts.append(String(format: "%.0f fps", fps)) }
        if let left = p.remaining { parts.append(left < 1 ? "almost done" : ExportProgress.formatDuration(left) + " left") }
        return parts.joined(separator: " · ")
    }

    func revealOutput() {
        if !lastOutputs.isEmpty { NSWorkspace.shared.activateFileViewerSelecting(lastOutputs) }
    }

    /// One folder for the whole batch. Outputs that already exist are replaced only after asking.
    private func askBatchDestination() -> [URL]? {
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.canCreateDirectories = true
        panel.directoryURL = sequences.first?.directory.deletingLastPathComponent()
        panel.prompt = "Export All Here"
        panel.message = format == .png
            ? "Each of the \(sequences.count) sequences goes into its own NAME_png folder."
            : "Each of the \(sequences.count) sequences becomes its own .\(format.fileExtension ?? "") file."
        guard panel.runModal() == .OK, let folder = panel.url else { return nil }
        let outputs = format.batchOutputs(for: sequences, in: folder)
        let existing = outputs.filter { FileManager.default.fileExists(atPath: $0.path) }
        if !existing.isEmpty {
            let alert = NSAlert()
            alert.alertStyle = .warning
            alert.messageText = existing.count == 1 ? "\u{201C}\(existing[0].lastPathComponent)\u{201D} already exists."
                                                    : "\(existing.count) of the exports already exist in this folder."
            alert.informativeText = "Replacing them overwrites their contents."
            alert.addButton(withTitle: "Replace")
            alert.addButton(withTitle: "Cancel")
            guard alert.runModal() == .alertFirstButtonReturn else { return nil }
        }
        return outputs
    }

    private func askDestination(for sequence: FrameSequence) -> URL? {
        let suggested = format.defaultOutput(for: sequence)
        if let ext = format.fileExtension {
            let panel = NSSavePanel()
            panel.nameFieldStringValue = suggested.lastPathComponent
            panel.directoryURL = suggested.deletingLastPathComponent()
            if let type = UTType(filenameExtension: ext) { panel.allowedContentTypes = [type] }
            return panel.runModal() == .OK ? panel.url : nil
        }
        // PNG: pick a parent folder; frames go into NAME_png/ inside it.
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.canCreateDirectories = true
        panel.directoryURL = suggested.deletingLastPathComponent()
        panel.prompt = "Export Here"
        panel.message = "The PNG frames go into a new folder \u{201C}\(suggested.lastPathComponent)\u{201D}."
        return panel.runModal() == .OK ? panel.url?.appendingPathComponent(suggested.lastPathComponent, isDirectory: true) : nil
    }

    private func rgb(_ color: Color) -> VBNExport.RGBColor {
        let c = NSColor(color).usingColorSpace(.sRGB) ?? .black
        func byte(_ v: CGFloat) -> UInt8 { UInt8(max(0, min(255, (v * 255).rounded()))) }
        return VBNExport.RGBColor(r: byte(c.redComponent), g: byte(c.greenComponent), b: byte(c.blueComponent))
    }
}

struct MainView: View {
    @ObservedObject var model: ExportModel
    let openFile: () -> Void
    let exportPNG: () -> Void
    let canExportPNG: () -> Bool
    @State private var dropTargeted = false

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            dropZone
            if let seq = model.sequence { settings(for: seq) }
            if let message = model.errorMessage {
                Text(message).foregroundStyle(.red).fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 0)
            HStack {
                Button("Open .vbn in Preview\u{2026}", action: openFile)
                Button("Export PNG\u{2026}", action: exportPNG).disabled(!canExportPNG())
                Spacer()
            }
            .controlSize(.small)
            .foregroundStyle(.secondary)
        }
        .padding(20)
        .frame(width: 480, height: 400, alignment: .topLeading)
    }

    private var dropZone: some View {
        RoundedRectangle(cornerRadius: 10)
            .strokeBorder(style: StrokeStyle(lineWidth: 1.5, dash: [6, 4]))
            .foregroundStyle(dropTargeted ? Color.accentColor : Color.secondary.opacity(0.5))
            .background(RoundedRectangle(cornerRadius: 10).fill(dropTargeted ? Color.accentColor.opacity(0.08) : .clear))
            .overlay(
                VStack(spacing: 4) {
                    Text(model.sequence == nil ? "Drop .vbn sequence folders or a frame" : "Drop other sequences")
                        .font(.headline)
                    Text("Several folders export one after the other, with the same settings")
                        .font(.caption).foregroundStyle(.secondary)
                }
            )
            .frame(height: model.sequence == nil ? 140 : 70)
            .onDrop(of: [.fileURL], isTargeted: $dropTargeted) { providers in
                guard !providers.isEmpty, !model.isRunning else { return false }
                Task { @MainActor in model.load(await Self.urls(from: providers)) }
                return true
            }
            .animation(.easeOut(duration: 0.15), value: model.sequence == nil)
    }

    /// The dropped file URLs, in drop order.
    private static func urls(from providers: [NSItemProvider]) async -> [URL] {
        var urls: [URL] = []
        for provider in providers {
            let url: URL? = await withCheckedContinuation { done in
                _ = provider.loadObject(ofClass: URL.self) { url, _ in done.resume(returning: url) }
            }
            if let url { urls.append(url) }
        }
        return urls
    }

    @ViewBuilder
    private func settings(for seq: FrameSequence) -> some View {
        Form {
            if model.isBatch {
                LabeledContent("Sequences") {
                    Text("\(model.sequences.count): " + model.sequences.map(\.name).joined(separator: ", "))
                        .lineLimit(2).truncationMode(.tail)
                        .help(model.sequences.map { "\($0.name) (\($0.frames.count))" }.joined(separator: "\n"))
                }
            } else if model.sequences.count > 1 {
                Picker("Sequence", selection: $model.selected) {
                    ForEach(model.sequences.indices, id: \.self) { i in
                        Text("\(model.sequences[i].name) (\(model.sequences[i].frames.count))").tag(i)
                    }
                }
            } else {
                LabeledContent("Sequence", value: seq.name)
            }
            LabeledContent("Frames") {
                let counts = model.frameCounts
                Text("\(counts.frames)" + (counts.missing > 0 ? " (\(counts.missing) missing, skipped)" : ""))
            }
            Picker("Format", selection: $model.format) {
                ForEach(ExportFormat.allCases) { f in
                    Text(f.displayName).tag(f)
                }
            }
            if model.format.needsFFmpeg, model.ffmpeg == nil {
                Text("WebM needs the ffmpeg bundled with release builds of unVBN.")
                    .font(.caption).foregroundStyle(.orange)
            }
            if model.format != .png {
                Picker("Frame rate", selection: $model.fps) {
                    ForEach([23.976, 24, 25, 29.97, 30, 50, 59.94, 60], id: \.self) { fps in
                        Text(FFmpeg.fpsString(fps) + " fps").tag(fps)
                    }
                }
            }
            if model.format == .mp4 {
                ColorPicker("Background", selection: $model.background, supportsOpacity: false)
            }
        }
        .formStyle(.columns)
        .disabled(model.isRunning)

        HStack(spacing: 10) {
            if let progress = model.progress {
                VStack(alignment: .leading, spacing: 4) {
                    ProgressView(value: progress.fraction)
                        .progressViewStyle(.linear)
                        .animation(.linear(duration: 0.1), value: progress.fraction)
                    HStack {
                        Text(model.progressLabel).monospacedDigit()
                        Spacer()
                        Text("\(progress.percent) %").monospacedDigit()
                    }
                    .font(.caption).foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity)
                Button("Cancel", action: model.cancel)
            } else {
                if let status = model.status {
                    Text(status).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                    if !model.lastOutputs.isEmpty { Button("Show in Finder", action: model.revealOutput).controlSize(.small) }
                }
                Spacer()
                Button(model.isBatch ? "Export All\u{2026}" : "Export\u{2026}", action: model.chooseAndExport)
                    .keyboardShortcut(.defaultAction)
                    .disabled(model.format.needsFFmpeg && model.ffmpeg == nil)
            }
        }
    }
}

/// Progress bar drawn over the Dock icon while a sequence renders.
@MainActor
final class DockProgress {
    static let shared = DockProgress()
    private var bar: NSProgressIndicator?

    func update(_ fraction: Double?) {
        let tile = NSApp.dockTile
        guard let fraction else {
            tile.contentView = nil; bar = nil
            tile.display()
            return
        }
        if bar == nil {
            let size = tile.size
            let icon = NSImageView(frame: NSRect(origin: .zero, size: size))
            icon.image = NSApp.applicationIconImage
            let bar = NSProgressIndicator(frame: NSRect(x: size.width * 0.12, y: size.height * 0.1,
                                                         width: size.width * 0.76, height: size.height * 0.12))
            bar.style = .bar
            bar.isIndeterminate = false
            bar.minValue = 0; bar.maxValue = 1
            icon.addSubview(bar)
            tile.contentView = icon
            self.bar = bar
        }
        bar?.doubleValue = fraction
        tile.display()
    }
}
