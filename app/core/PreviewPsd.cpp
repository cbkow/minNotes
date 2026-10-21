// The preview pass's Photoshop backend (tier 2, 2026-10-21): psd_sdk reads the MERGED COMPOSITE Photoshop saves with
// "Maximize Compatibility" (on by default) — no layer compositing here. A file saved without it has no composite and
// renders null (macOS then tries ImageIO / QuickLook; Windows shows a file card). Ported from ufb's PsdBackend.
// Supported: 8 / 16-bit RGB(A), 8 / 16-bit grayscale, 8-bit CMYK (the naive inverted-ink formula, no ICC — a
// recognisable preview, not a proof). 32-bit float, Lab, duotone, indexed and multichannel decline.
#include "PreviewRender.h"

#include "Psd.h"
#include "PsdMallocAllocator.h"
#include "PsdDocument.h"
#include "PsdParseDocument.h"
#include "PsdParseImageDataSection.h"
#include "PsdImageDataSection.h"
#include "PsdPlanarImage.h"
#include "PsdColorMode.h"
#ifdef Q_OS_WIN
#  include "PsdNativeFile.h"
#else
#  include "PsdSyncFile.h"
#endif

#include <QImage>
#include <cstdint>

namespace mn::preview {

namespace {
namespace ps = PSD_NAMESPACE;
#ifdef Q_OS_WIN
using PsdFileImpl = ps::NativeFile;
#else
using PsdFileImpl = PsdSyncFile;
#endif

template <typename T> inline uint8_t to8(T v);
template <> inline uint8_t to8<uint8_t>(uint8_t v) { return v; }
template <> inline uint8_t to8<uint16_t>(uint16_t v) { return uint8_t(v >> 8); }

template <typename T>
void interleaveRgba(const T* r, const T* g, const T* b, const T* a, uint8_t* dst, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        dst[i * 4 + 0] = to8(r[i]); dst[i * 4 + 1] = to8(g[i]); dst[i * 4 + 2] = to8(b[i]);
        dst[i * 4 + 3] = a ? to8(a[i]) : 0xFF;
    }
}
template <typename T>
void interleaveGray(const T* k, const T* a, uint8_t* dst, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const uint8_t v = to8(k[i]);
        dst[i * 4 + 0] = v; dst[i * 4 + 1] = v; dst[i * 4 + 2] = v;
        dst[i * 4 + 3] = a ? to8(a[i]) : 0xFF;
    }
}
// PSD stores CMYK inverted (0 = full ink): R = Cinv·Kinv / 255 and so on.
void cmykToRgba(const uint8_t* c, const uint8_t* m, const uint8_t* y, const uint8_t* k, uint8_t* dst, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        dst[i * 4 + 0] = uint8_t((unsigned(c[i]) * k[i]) / 255);
        dst[i * 4 + 1] = uint8_t((unsigned(m[i]) * k[i]) / 255);
        dst[i * 4 + 2] = uint8_t((unsigned(y[i]) * k[i]) / 255);
        dst[i * 4 + 3] = 0xFF;
    }
}
}  // namespace

QImage renderPsd(const QString& path, int maxEdge) {
    ps::MallocAllocator allocator;
    PsdFileImpl file(&allocator);
    if (!file.OpenRead(path.toStdWString().c_str())) return {};
    ps::Document* doc = ps::CreateDocument(&file, &allocator);
    if (!doc) { file.Close(); return {}; }
    struct Cleanup { ps::Document*& d; ps::MallocAllocator& a; PsdFileImpl& f; ~Cleanup() { ps::DestroyDocument(d, &a); f.Close(); } } cleanup{doc, allocator, file};

    if (doc->imageDataSection.length == 0) return {};   // saved without "Maximize Compatibility": no composite
    const bool rgb = doc->colorMode == ps::colorMode::RGB, gray = doc->colorMode == ps::colorMode::GRAYSCALE, cmyk = doc->colorMode == ps::colorMode::CMYK;
    const bool depth8or16 = doc->bitsPerChannel == 8 || doc->bitsPerChannel == 16;
    if (!(((rgb || gray) && depth8or16) || (cmyk && doc->bitsPerChannel == 8))) return {};
    if (qint64(doc->width) * qint64(doc->height) > 64LL * 1024 * 1024) return {};   // the 64 MP guard (w·h·4 + the SDK's channel buffers)

    ps::ImageDataSection* img = ps::ParseImageDataSection(doc, &file, &allocator);
    if (!img) return {};
    const unsigned w = doc->width, h = doc->height;
    const size_t n = size_t(w) * size_t(h);
    QImage out(int(w), int(h), QImage::Format_RGBA8888);
    bool ok = !out.isNull();
    if (ok) {
        const auto ch = [&](unsigned i) { return img->images[i].data; };
        if (cmyk) {
            ok = img->imageCount >= 4;
            if (ok) cmykToRgba(static_cast<const uint8_t*>(ch(0)), static_cast<const uint8_t*>(ch(1)), static_cast<const uint8_t*>(ch(2)), static_cast<const uint8_t*>(ch(3)), out.bits(), n);
        } else if (gray) {
            ok = img->imageCount >= 1;
            const bool alpha = img->imageCount >= 2;
            if (ok && doc->bitsPerChannel == 8) interleaveGray(static_cast<const uint8_t*>(ch(0)), alpha ? static_cast<const uint8_t*>(ch(1)) : nullptr, out.bits(), n);
            else if (ok) interleaveGray(static_cast<const uint16_t*>(ch(0)), alpha ? static_cast<const uint16_t*>(ch(1)) : nullptr, out.bits(), n);
        } else {
            ok = img->imageCount >= 3;
            const bool alpha = img->imageCount >= 4;
            if (ok && doc->bitsPerChannel == 8)
                interleaveRgba(static_cast<const uint8_t*>(ch(0)), static_cast<const uint8_t*>(ch(1)), static_cast<const uint8_t*>(ch(2)), alpha ? static_cast<const uint8_t*>(ch(3)) : nullptr, out.bits(), n);
            else if (ok)
                interleaveRgba(static_cast<const uint16_t*>(ch(0)), static_cast<const uint16_t*>(ch(1)), static_cast<const uint16_t*>(ch(2)), alpha ? static_cast<const uint16_t*>(ch(3)) : nullptr, out.bits(), n);
        }
    }
    ps::DestroyImageDataSection(img, &allocator);
    if (!ok) return {};
    const int edge = std::max(out.width(), out.height());
    if (maxEdge > 0 && edge > maxEdge)
        out = out.scaled(out.width() * maxEdge / edge, out.height() * maxEdge / edge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return out;
}

}  // namespace mn::preview
