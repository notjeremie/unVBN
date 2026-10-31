import AppKit
import SwiftUI
import UniformTypeIdentifiers
import VBNExport

/// State of the export panel: the sequences found in what was dropped, the chosen settings and the job.
@MainActor
final class ExportModel: ObservableObject {
    @Published var sequences: [FrameSequence] = []
    @Published var selected = 0
    @Published var format: ExportFormat = .webm
    @Published var fps: Double = 50
    @Published var background: Color = .black
    @Published private(set) var progress: Double?
    @Published private(set) var status: String?
    @Published private(set) var lastOutput: URL?
    @Published var errorMessage: String?

    let ffmpeg = FFmpeg.locate()
    private var cancelFlag = CancelFlag()

    var sequence: FrameSequence? { sequences.indices.contains(selected) ? sequences[selected] : nil }
    var isRunning: Bool { progress != nil }

    func load(_ url: URL) {
        do {
            sequences = try FrameSequence.find(at: url)
            selected = 0
            status = nil; lastOutput = nil; errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func chooseAndExport() {
        guard let sequence, !isRunning else { return }
        guard let output = askDestination(for: sequence) else { return }
        var options = ExportOptions(format: format, fps: fps)
        options.background = rgb(background)

        let flag = CancelFlag()
        cancelFlag = flag
        progress = 0; status = "Exporting \(sequence.frames.count) frames…"; lastOutput = nil; errorMessage = nil
        let ffmpeg = self.ffmpeg
        Task.detached(priority: .userInitiated) {
            let start = Date()
            do {
                try SequenceExporter(sequence: sequence, options: options, output: output, ffmpeg: ffmpeg).run(
                    progress: { done, total in Task { @MainActor in self.progress = Double(done) / Double(total) } },
                    isCancelled: { flag.isSet })
                let seconds = Date().timeIntervalSince(start)
                await MainActor.run {
                    self.progress = nil
                    self.lastOutput = output
                    self.status = String(format: "Exported %@ in %.1f s.", output.lastPathComponent, seconds)
                }
            } catch {
                await MainActor.run {
                    self.progress = nil
                    self.status = nil
                    if (error as? ExportError) != .cancelled { self.errorMessage = error.localizedDescription }
                }
            }
        }
    }

    func cancel() { cancelFlag.set() }

    func revealOutput() {
        if let lastOutput { NSWorkspace.shared.activateFileViewerSelecting([lastOutput]) }
    }

    private func askDestination(for sequence: FrameSequence) -> URL? {
        let suggested = format.defaultOutput(for: sequence)
        if format.fileExtension != nil {
            let panel = NSSavePanel()
            panel.nameFieldStringValue = suggested.lastPathComponent
            panel.directoryURL = suggested.deletingLastPathComponent()
            if let ext = format.fileExtension, let type = UTType(filenameExtension: ext) { panel.allowedContentTypes = [type] }
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

/// Thread-safe cancel switch read by the export thread.
final class CancelFlag: @unchecked Sendable {
    private let lock = NSLock()
    private var value = false
    var isSet: Bool { lock.lock(); defer { lock.unlock() }; return value }
    func set() { lock.lock(); value = true; lock.unlock() }
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
                    Text(model.sequence == nil ? "Drop a .vbn sequence folder or frame" : "Drop another sequence")
                        .font(.headline)
                    Text("Exports PNG frames, MP4, WebM with alpha or ProRes 4444")
                        .font(.caption).foregroundStyle(.secondary)
                }
            )
            .frame(height: model.sequence == nil ? 140 : 70)
            .onDrop(of: [.fileURL], isTargeted: $dropTargeted) { providers in
                guard let provider = providers.first else { return false }
                _ = provider.loadObject(ofClass: URL.self) { url, _ in
                    if let url { Task { @MainActor in model.load(url) } }
                }
                return true
            }
            .animation(.easeOut(duration: 0.15), value: model.sequence == nil)
    }

    @ViewBuilder
    private func settings(for seq: FrameSequence) -> some View {
        Form {
            if model.sequences.count > 1 {
                Picker("Sequence", selection: $model.selected) {
                    ForEach(model.sequences.indices, id: \.self) { i in
                        Text("\(model.sequences[i].name) (\(model.sequences[i].frames.count))").tag(i)
                    }
                }
            } else {
                LabeledContent("Sequence", value: seq.name)
            }
            LabeledContent("Frames") {
                let missing = seq.missingNumbers.count
                Text("\(seq.frames.count)" + (missing > 0 ? " (\(missing) missing, skipped)" : ""))
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
                        Text(fps == fps.rounded() ? "\(Int(fps)) fps" : String(format: "%.3g fps", fps)).tag(fps)
                    }
                }
            }
            if !model.format.keepsAlpha {
                ColorPicker("Background", selection: $model.background, supportsOpacity: false)
            }
        }
        .formStyle(.columns)
        .disabled(model.isRunning)

        HStack(spacing: 10) {
            if let progress = model.progress {
                ProgressView(value: progress).frame(maxWidth: .infinity)
                Button("Cancel", action: model.cancel)
            } else {
                if let status = model.status {
                    Text(status).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                    if model.lastOutput != nil { Button("Show in Finder", action: model.revealOutput).controlSize(.small) }
                }
                Spacer()
                Button("Export\u{2026}", action: model.chooseAndExport)
                    .keyboardShortcut(.defaultAction)
                    .disabled(model.format.needsFFmpeg && model.ffmpeg == nil)
            }
        }
    }
}
