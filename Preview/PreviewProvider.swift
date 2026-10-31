import Foundation
import QuickLookUI
import UniformTypeIdentifiers
import VBNCore

/// Data-based Quick Look preview: hands Quick Look a PNG of the decoded .vbn.
final class PreviewProvider: QLPreviewProvider, QLPreviewingController {
    func providePreview(for request: QLFilePreviewRequest) async throws -> QLPreviewReply {
        let image = try VBNImage.decode(data: Data(contentsOf: request.fileURL))
        let png = try VBNImage.pngData(from: image)
        let reply = QLPreviewReply(dataOfContentType: .png,
                                   contentSize: CGSize(width: image.width, height: image.height)) { _ in png }
        reply.title = request.fileURL.lastPathComponent
        return reply
    }
}
