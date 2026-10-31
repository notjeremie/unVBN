import AppKit
import SwiftUI
import UniformTypeIdentifiers
import VBNCore

@main
enum Main {
    static let delegate = AppDelegate()
    static func main() {
        let app = NSApplication.shared
        app.delegate = delegate
        app.setActivationPolicy(.regular)
        app.run()
    }
}

extension UTType {
    static let vbn = UTType(importedAs: "com.vizrt.vbn", conformingTo: .image)
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var window: NSWindow?
    private var openedFiles = false
    /// The last .vbn opened, for "Export PNG...".
    private(set) var lastOpened: URL?

    func applicationWillFinishLaunching(_ notification: Notification) {
        NSApp.mainMenu = makeMainMenu()
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // When launched to open files, the open event arrives around launch; give it a moment
        // so a plain double-click on a .vbn never flashes the window.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) { [weak self] in
            guard let self, !self.openedFiles else { return }
            self.showWindow()
        }
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        if !flag { showWindow() }
        return true
    }

    func application(_ application: NSApplication, open urls: [URL]) {
        openedFiles = true
        for url in urls { openInPreview(url) }
    }

    // MARK: - Convert and open

    func openInPreview(_ url: URL) {
        do {
            let png = try convert(url)
            lastOpened = url
            NSDocumentController.shared.noteNewRecentDocumentURL(url)
            let config = NSWorkspace.OpenConfiguration()
            config.activates = true
            if let preview = NSWorkspace.shared.urlForApplication(withBundleIdentifier: "com.apple.Preview") {
                NSWorkspace.shared.open([png], withApplicationAt: preview, configuration: config) { _, error in
                    if let error { DispatchQueue.main.async { self.report(error, for: url) } }
                }
            } else {
                NSWorkspace.shared.open(png)
            }
        } catch {
            report(error, for: url)
        }
    }

    /// Decodes a .vbn and writes it to `$TMPDIR/VBN/<name>.png`.
    private func convert(_ url: URL) throws -> URL {
        let png = try VBNImage.pngData(fromVBN: Data(contentsOf: url))
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("VBN", isDirectory: true)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let out = dir.appendingPathComponent(url.deletingPathExtension().lastPathComponent).appendingPathExtension("png")
        try png.write(to: out, options: .atomic)
        return out
    }

    private func report(_ error: Error, for url: URL) {
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = "Could not open \u{201C}\(url.lastPathComponent)\u{201D}"
        alert.informativeText = error.localizedDescription
        NSApp.activate(ignoringOtherApps: true)
        alert.runModal()
    }

    // MARK: - Actions

    @objc func openDocument(_ sender: Any?) {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.vbn]
        panel.allowsMultipleSelection = true
        guard panel.runModal() == .OK else { return }
        for url in panel.urls { openInPreview(url) }
    }

    @objc func exportPNG(_ sender: Any?) {
        guard let source = lastOpened else { return }
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.png]
        panel.nameFieldStringValue = source.deletingPathExtension().lastPathComponent + ".png"
        guard panel.runModal() == .OK, let dest = panel.url else { return }
        do {
            try VBNImage.pngData(fromVBN: Data(contentsOf: source)).write(to: dest, options: .atomic)
        } catch {
            report(error, for: source)
        }
    }

    @objc func validateMenuItem(_ item: NSMenuItem) -> Bool {
        item.action == #selector(exportPNG(_:)) ? lastOpened != nil : true
    }

    // MARK: - UI

    private func showWindow() {
        if window == nil {
            let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 420, height: 220),
                             styleMask: [.titled, .closable, .miniaturizable], backing: .buffered, defer: false)
            w.title = "VBN"
            w.isReleasedWhenClosed = false
            w.contentView = NSHostingView(rootView: WelcomeView(
                open: { [weak self] in self?.openDocument(nil) },
                export: { [weak self] in self?.exportPNG(nil) },
                canExport: { [weak self] in self?.lastOpened != nil }
            ))
            w.center()
            window = w
        }
        window?.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    private func makeMainMenu() -> NSMenu {
        let main = NSMenu()

        let appItem = NSMenuItem()
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "About VBN", action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)), keyEquivalent: "")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Hide VBN", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        appMenu.addItem(withTitle: "Quit VBN", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu
        main.addItem(appItem)

        let fileItem = NSMenuItem()
        let fileMenu = NSMenu(title: "File")
        fileMenu.addItem(withTitle: "Open .vbn\u{2026}", action: #selector(openDocument(_:)), keyEquivalent: "o").target = self
        fileMenu.addItem(withTitle: "Export PNG\u{2026}", action: #selector(exportPNG(_:)), keyEquivalent: "e").target = self
        fileMenu.addItem(.separator())
        fileMenu.addItem(withTitle: "Close Window", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        fileItem.submenu = fileMenu
        main.addItem(fileItem)

        return main
    }
}

struct WelcomeView: View {
    let open: () -> Void
    let export: () -> Void
    let canExport: () -> Bool
    @State private var refresh = 0

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Vizrt .vbn viewer").font(.headline)
            Text("Double-click a .vbn file to open it in Preview as a PNG. Space bar in Finder shows a Quick Look preview, and Finder draws thumbnails. This window only appears when VBN is launched on its own.")
                .fixedSize(horizontal: false, vertical: true)
                .foregroundStyle(.secondary)
            Spacer(minLength: 0)
            HStack {
                Button("Open .vbn\u{2026}", action: open).keyboardShortcut(.defaultAction)
                Button("Export PNG\u{2026}", action: export).disabled(!canExport())
            }
        }
        .padding(20)
        .frame(width: 420, height: 220, alignment: .topLeading)
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in refresh += 1 }
        .id(refresh)
    }
}
