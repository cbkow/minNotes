#pragma once
// The PREVIEW PASS (2026-10-21, tier 1): files the app cannot show as-is (Photoshop, HEIC, RAW, Radiance HDR) are
// rendered ONCE into a flat 8-bit preview PNG that rides the image plumbing (the block / card shows the preview
// with a format pill; the original is referenced in place, never copied, never modified). Backends:
//   · Radiance HDR (.hdr / .rgbe) — stb_image, everywhere: linear float → clamp → 2.2 gamma (ufb's thumbnail
//     curve; "a recognisable preview, not a colour-accurate view" — EXR is deliberately NOT here, 2026-10-21 ruling).
//   · Photoshop PSD / PSB (tier 2) — psd_sdk's merged composite, everywhere (PreviewPsd.cpp); a file saved without
//     "Maximize Compatibility" has none and falls through to the OS stage on macOS.
//   · macOS: ImageIO (CGImageSource) for HEIC / HEIF, camera RAW and PSDs psd_sdk declined, then QuickLook for
//     whatever ImageIO declines (anything with a QuickLook plugin). HEIC on Windows is tier 3 (libheif).
// Illustrator files are not here either: an .ai saved PDF-compatible IS a PDF and goes through Qt PDF (MediaStore::isPdfPath).
#include <QImage>
#include <QString>

namespace mn::preview {

// The default long-edge cap for a stored preview.
constexpr int kMaxEdge = 2048;

// True when `ext` (lower-case, no dot) has a preview backend ON THIS PLATFORM.
bool supportsExt(const QString& ext);
// The pill's label for an extension ("PSD", "HEIC", "RAW", "HDR"); "" when unknown.
QString formatLabel(const QString& ext);
// Render the file's preview, long edge ≤ maxEdge. Null on failure (no backend, unreadable, too large).
QImage render(const QString& path, int maxEdge = kMaxEdge);

// Backends (internal; exposed for tests).
QImage renderHdr(const QString& path, int maxEdge);
QImage renderPsd(const QString& path, int maxEdge);      // psd_sdk: the merged composite; null without one
QImage renderNative(const QString& path, int maxEdge);   // ImageIO → QuickLook on macOS; null elsewhere

}  // namespace mn::preview
