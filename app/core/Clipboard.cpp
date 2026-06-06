#include "Clipboard.h"
#include <QGuiApplication>
#include <QClipboard>
#include <QMimeData>

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

void Clipboard::writeText(const QString& text) { clip_->setText(text); }

void Clipboard::writeTable(const QString& tsv, const QString& html) {
    auto* m = new QMimeData;               // clipboard takes ownership
    m->setText(tsv);
    if (!html.isEmpty()) m->setHtml(html);
    clip_->setMimeData(m);
}
