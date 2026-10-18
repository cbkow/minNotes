#include "MediaHost.h"
#include "../core/BlockModel.h"
#include "../core/CanvasDocument.h"
#include "../core/MediaStore.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

MediaHost::MediaHost(QObject* parent) : QObject(parent) {}

QObject* MediaHost::model() const { return model_.data(); }
QObject* MediaHost::canvas() const { return canvas_.data(); }

void MediaHost::setModel(QObject* m) {
    auto* bm = qobject_cast<BlockModel*>(m);
    if (bm == model_.data()) return;
    model_ = bm;
    emit sourceChanged();
    rewire(); refresh();
}
void MediaHost::setBlockId(const QString& id) {
    if (id == blockId_) return;
    blockId_ = id;
    emit sourceChanged();
    rewire(); refresh();
}
void MediaHost::setCanvas(QObject* c) {
    auto* cd = qobject_cast<CanvasDocument*>(c);
    if (cd == canvas_.data()) return;
    canvas_ = cd;
    emit sourceChanged();
    rewire(); refresh();
}
void MediaHost::setElementId(const QString& id) {
    if (id == elementId_) return;
    elementId_ = id;
    emit sourceChanged();
    rewire(); refresh();
}

// Only the ACTIVE source is connected: a host parked on the re-pointing `blockModel` context
// property with no block id listens to nothing, so a keystroke in the page never reaches it.
void MediaHost::rewire() {
    for (auto& c : conns_) QObject::disconnect(c);
    conns_.clear();
    if (elementMode()) {
        conns_ << connect(canvas_, &CanvasDocument::elementsChanged, this, &MediaHost::refresh)
               << connect(canvas_, &CanvasDocument::inkChanged, this, &MediaHost::refresh)
               << connect(canvas_, &CanvasDocument::documentChanged, this, &MediaHost::refresh)
               << connect(canvas_, &CanvasDocument::undoStackChanged, this, &MediaHost::changed);
    } else if (model_ && !blockId_.isEmpty()) {
        conns_ << connect(model_, &BlockModel::structureChangedSpike, this, &MediaHost::refresh)
               << connect(model_, &BlockModel::contentChangedSpike, this, &MediaHost::refresh)
               << connect(model_, &BlockModel::documentChanged, this, &MediaHost::refresh)
               << connect(model_, &BlockModel::undoStackChanged, this, &MediaHost::changed);
    }
}

QJsonObject MediaHost::descriptor() const {
    if (elementMode()) {
        const QString j = canvas_->elementJson(elementId_);
        return QJsonDocument::fromJson(j.toUtf8()).object().value(QStringLiteral("media")).toObject();
    }
    if (model_ && row_ >= 0)
        return QJsonDocument::fromJson(model_->contentForRow(row_).toUtf8()).object();
    return {};
}
QJsonValue MediaHost::src() const { return descriptor().value(QStringLiteral("src")); }

void MediaHost::refresh() {
    const bool wasValid = valid_;
    const QString oldKind = kind_, oldAnchor = anchor_;
    const int oldRow = row_;
    valid_ = false; row_ = -1; kind_.clear(); anchor_.clear();
    if (elementMode()) {
        if (canvas_->elementKind(elementId_) == QLatin1String("media")) {
            const QJsonObject d = descriptor();
            valid_ = true;
            kind_ = d.value(QStringLiteral("kind")).toString(QStringLiteral("image"));
            anchor_ = canvas_->mediaAnchorPath(d.value(QStringLiteral("src")).toVariant());
        }
    } else if (model_ && !blockId_.isEmpty()) {
        const int r = model_->rowForId(blockId_);
        if (r >= 0 && model_->typeForRow(r) == 3) {
            valid_ = true; row_ = r;
            kind_ = model_->mediaKind(r);
            anchor_ = model_->mediaAnchorPath(r);
        }
    }
    ++revision_;
    if (valid_ != wasValid || kind_ != oldKind || anchor_ != oldAnchor || row_ != oldRow) emit identityChanged();
    emit changed();
    if (wasValid && !valid_) emit gone();
}

QString MediaHost::url() const {
    if (!valid_) return {};
    return elementMode() ? canvas_->resolveMediaUrl(src().toVariant()) : model_->mediaUrl(row_);
}
QString MediaHost::viewPath() const {
    if (!valid_) return {};
    return elementMode() ? canvas_->resolveMediaPath(src().toVariant()) : model_->mediaViewPath(row_);
}
QString MediaHost::playbackSource() const {
    if (!valid_) return {};
    return elementMode() ? canvas_->mediaPlaybackSource(src().toVariant()) : model_->mediaPlaybackSource(row_);
}
QString MediaHost::fileName() const {
    if (!valid_) return {};
    if (!elementMode()) return model_->mediaFileName(row_);
    const QJsonObject d = descriptor();
    const QString n = d.value(QStringLiteral("name")).toString();
    if (!n.isEmpty()) return n;
    const QJsonValue s = d.value(QStringLiteral("src"));
    QString path = s.isString() ? s.toString() : QString();
    if (s.isObject()) { const auto rel = s.toObject().value(QStringLiteral("rel")).toArray(); if (!rel.isEmpty()) path = rel.last().toString(); }
    return path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
}
bool MediaHost::extracting() const {
    if (!valid_) return false;
    return elementMode() ? canvas_->mediaExtracting(src().toVariant()) : model_->mediaExtracting(row_);
}
int MediaHost::w() const      { return valid_ ? (elementMode() ? descriptor().value(QStringLiteral("w")).toInt() : model_->mediaW(row_)) : 0; }
int MediaHost::h() const      { return valid_ ? (elementMode() ? descriptor().value(QStringLiteral("h")).toInt() : model_->mediaH(row_)) : 0; }
int MediaHost::frames() const { return valid_ ? (elementMode() ? descriptor().value(QStringLiteral("frames")).toInt() : model_->mediaFrames(row_)) : 0; }
int MediaHost::pages() const  { return valid_ ? (elementMode() ? descriptor().value(QStringLiteral("pages")).toInt() : model_->mediaPdfPages(row_)) : 0; }
double MediaHost::fps() const { return valid_ ? (elementMode() ? descriptor().value(QStringLiteral("fps")).toDouble() : model_->mediaFps(row_)) : 0.0; }
qreal MediaHost::durMs() const{ return valid_ ? (elementMode() ? descriptor().value(QStringLiteral("durMs")).toDouble() : model_->mediaDurationMs(row_)) : 0.0; }

bool MediaHost::canUndo() const { return elementMode() ? (canvas_ && canvas_->canUndo()) : (model_ && model_->canUndo()); }
bool MediaHost::canRedo() const { return elementMode() ? (canvas_ && canvas_->canRedo()) : (model_ && model_->canRedo()); }

QString MediaHost::pageInk(int page) const {
    if (!valid_) return {};
    return elementMode() ? canvas_->mediaElementInk(elementId_, page) : model_->mediaPageInk(row_, page);
}
bool MediaHost::setPageInk(int page, const QString& envelope) {
    if (!valid_) return false;
    if (elementMode()) return canvas_->setMediaElementInk(elementId_, page, envelope);
    model_->setMediaPageInk(row_, page, envelope);
    return true;
}
void MediaHost::undo() { if (elementMode()) { if (canvas_) canvas_->undo(); } else if (model_) model_->undo(); }
void MediaHost::redo() { if (elementMode()) { if (canvas_) canvas_->redo(); } else if (model_) model_->redo(); }
