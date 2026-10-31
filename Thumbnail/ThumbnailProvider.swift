import CoreGraphics
import Foundation
import QuickLookThumbnailing
import VBNCore

/// Finder thumbnails: the decoded .vbn drawn aspect-fit into the requested size.
final class ThumbnailProvider: QLThumbnailProvider {
    override func provideThumbnail(for request: QLFileThumbnailRequest,
                                   _ handler: @escaping (QLThumbnailReply?, Error?) -> Void) {
        let image: CGImage
        do {
            image = try VBNImage.decode(data: Data(contentsOf: request.fileURL))
        } catch {
            handler(nil, error)
            return
        }
        let max = request.maximumSize
        let scale = min(max.width / CGFloat(image.width), max.height / CGFloat(image.height))
        let size = CGSize(width: (CGFloat(image.width) * scale).rounded(),
                          height: (CGFloat(image.height) * scale).rounded())
        handler(QLThumbnailReply(contextSize: size) { context in
            context.interpolationQuality = .high
            context.draw(image, in: CGRect(origin: .zero, size: size))
            return true
        }, nil)
    }
}
