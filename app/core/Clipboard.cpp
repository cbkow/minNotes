#include "Clipboard.h"
#include <QGuiApplication>
#include <QClipboard>
#include <QMimeData>
#include <QUrl>
#include <QImage>
#include "BlockClipboard.h"

Clipboard::Clipboard(QObject* parent) : QObject(parent), clip_(QGuiApplication::clipboard()) {}

QString Clipboard::readText() const { return clip_->text(); }

QString Clipboard::readHtml() const {
    const QMimeData* m = clip_->mimeData();
    return (m && m->hasHtml()) ? m->html() : QString();
}

bool Clipboard::hasImage() const {
    const QMimeData* m = clip_->mimeData();
    return m && m->hasImage();
}

bool Clipboard::hasHtml() const {
    const QMimeData* m = clip_->mimeData();
    return m && m->hasHtml();
}

QStringList Clipboard::readUrls() const {
    QStringList out;
    const QMimeData* m = clip_->mimeData();
    if (m && m->hasUrls())
        for (const QUrl& u : m->urls())
            if (u.isLocalFile()) out << u.toString();   // copied FILES (Finder/Preview)
    return out;
}

void Clipboard::writeText(const QString& text) { clip_->setText(text); }

void Clipboard::writeTable(const QString& tsv, const QString& html) {
    auto* m = new QMimeData;               // clipboard takes ownership
    m->setText(tsv);
    if (!html.isEmpty()) m->setHtml(html);
    clip_->setMimeData(m);
}

bool Clipboard::writeImageFromFile(const QString& fileUrl) {
    QString path = fileUrl;
    if (path.startsWith(QLatin1String("file:"))) path = QUrl(fileUrl).toLocalFile();
    if (path.isEmpty()) return false;
    QImage img(path);
    if (img.isNull()) return false;
    clip_->setImage(img);
    return true;
}

bool Clipboard::hasBlocks() const {
    const QMimeData* m = clip_->mimeData();
    return m && m->hasFormat(QLatin1String(BlockClipboard::kMime));
}

QString Clipboard::readBlocks() const {
    const QMimeData* m = clip_->mimeData();
    if (!m || !m->hasFormat(QLatin1String(BlockClipboard::kMime))) return {};
    return QString::fromUtf8(m->data(QLatin1String(BlockClipboard::kMime)));
}

void Clipboard::writeBlocks(const QString& blocksJson, const QString& text,
                            const QString& html, const QString& imageFileUrl) {
    auto* m = new QMimeData;               // clipboard takes ownership
    if (!blocksJson.isEmpty()) m->setData(QLatin1String(BlockClipboard::kMime), blocksJson.toUtf8());
    m->setText(text);
    if (!html.isEmpty()) m->setHtml(html);
    if (!imageFileUrl.isEmpty()) {
        QString path = imageFileUrl;
        if (path.startsWith(QLatin1String("file:"))) path = QUrl(imageFileUrl).toLocalFile();
        QImage img(path);
        if (!img.isNull()) m->setImageData(img);
    }
    clip_->setMimeData(m);
}
