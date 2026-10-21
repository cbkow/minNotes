// The preview pass's macOS backend: ImageIO first (CGImageSource reads PSD, HEIC / HEIF and camera RAW natively and
// hands back a thumbnail capped at maxEdge without decoding the whole file), then QuickLook for whatever ImageIO
// declines (PSB, formats with a QuickLook plugin). Compiled with -fobjc-arc (QLThumbnailGenerationRequest is alloc/init'd).
#include "PreviewRender.h"

#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <QuickLookThumbnailing/QuickLookThumbnailing.h>

namespace mn::preview {

namespace {

// Premultiplied RGBA CGImage → QImage, drawn into the QImage's own buffer through a matching bitmap context.
QImage cgImageToQImage(CGImageRef cg) {
    if (!cg) return {};
    const size_t w = CGImageGetWidth(cg), h = CGImageGetHeight(cg);
    if (w == 0 || h == 0) return {};
    QImage img(static_cast<int>(w), static_cast<int>(h), QImage::Format_RGBA8888_Premultiplied);
    if (img.isNull()) return {};
    img.fill(Qt::transparent);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    if (!cs) return {};
    CGContextRef ctx = CGBitmapContextCreate(img.bits(), w, h, 8, static_cast<size_t>(img.bytesPerLine()), cs,
                                             kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (!ctx) return {};
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), cg);
    CGContextRelease(ctx);
    return img;
}

QImage viaImageIO(NSURL* url, int maxEdge) {
    CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
    if (!src) return {};
    NSDictionary* opts = @{ (__bridge id)kCGImageSourceCreateThumbnailFromImageAlways: @YES,
                            (__bridge id)kCGImageSourceCreateThumbnailWithTransform: @YES,
                            (__bridge id)kCGImageSourceThumbnailMaxPixelSize: @(maxEdge) };
    CGImageRef cg = CGImageSourceCreateThumbnailAtIndex(src, 0, (__bridge CFDictionaryRef)opts);
    CFRelease(src);
    if (!cg) return {};
    QImage img = cgImageToQImage(cg);
    CGImageRelease(cg);
    return img;
}

QImage viaQuickLook(NSURL* url, int maxEdge) {
    QLThumbnailGenerationRequest* req =
        [[QLThumbnailGenerationRequest alloc] initWithFileAtURL:url
                                                          size:CGSizeMake(maxEdge, maxEdge)
                                                         scale:1.0
                                           representationTypes:QLThumbnailGenerationRequestRepresentationTypeThumbnail];
    __block QImage result;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [[QLThumbnailGenerator sharedGenerator]
        generateBestRepresentationForRequest:req
                           completionHandler:^(QLThumbnailRepresentation* thumb, NSError*) {
                               @autoreleasepool { if (thumb) result = cgImageToQImage([thumb CGImage]); }
                               dispatch_semaphore_signal(sem);
                           }];
    // Bounded: a stuck generation returns null instead of pinning the caller.
    if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)10 * NSEC_PER_SEC)) != 0) return {};
    return result;
}

}  // namespace

QImage renderNative(const QString& path, int maxEdge) {
    if (path.isEmpty()) return {};
    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:path.toNSString()];
        if (!url) return {};
        QImage img = viaImageIO(url, maxEdge);
        if (img.isNull()) img = viaQuickLook(url, maxEdge);
        return img;
    }
}

}  // namespace mn::preview
