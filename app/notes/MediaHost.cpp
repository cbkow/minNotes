#include "MediaHost.h"
#include "../core/BlockModel.h"
#include "../core/CanvasDocument.h"
#include "../core/MediaStore.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include "annotation_serializer.h"
#include <cmath>

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

// --- drawing on a page (2026-10-19) ---------------------------------------------------------------------------
namespace {
QString compactJson(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }
double segmentDistance(const QPointF& p, const QPointF& a, const QPointF& b) {
    const QPointF d = b - a;
    const double l2 = d.x() * d.x() + d.y() * d.y();
    double t = l2 > 0 ? ((p.x() - a.x()) * d.x() + (p.y() - a.y()) * d.y()) / l2 : 0.0;
    t = std::max(0.0, std::min(1.0, t));
    const QPointF q = a + t * d;
    return std::hypot(p.x() - q.x(), p.y() - q.y());
}
// The distance in frame px from `p` to a normalized stroke drawn in a frameW × frameH frame.
double strokeDistance(const qcv::ActiveStroke& s, const QPointF& p, double frameW, double frameH) {
    auto px = [&](const QPointF& n) { return QPointF(n.x() * frameW, n.y() * frameH); };
    if (s.points.empty()) return 1e9;
    if (s.tool == qcv::DrawingTool::Oval && s.points.size() >= 2) {   // {centre, radii}: how far from the ring
        const QPointF c = px(s.points[0]);
        const double rx = std::abs(s.points[1].x()) * frameW, ry = std::abs(s.points[1].y()) * frameH;
        if (rx < 1 || ry < 1) return std::hypot(p.x() - c.x(), p.y() - c.y());
        const double d = std::hypot((p.x() - c.x()) / rx, (p.y() - c.y()) / ry);
        return std::abs(d - 1.0) * std::min(rx, ry);
    }
    if (s.points.size() == 1) { const QPointF a = px(s.points[0]); return std::hypot(p.x() - a.x(), p.y() - a.y()); }
    double best = 1e9;
    const size_t n = s.points.size(), segs = s.tool == qcv::DrawingTool::Rectangle ? n : n - 1;   // a rect closes
    for (size_t i = 0; i < segs; ++i) best = std::min(best, segmentDistance(p, px(s.points[i]), px(s.points[(i + 1) % n])));
    return best;
}
}

bool MediaHost::addPageStroke(int page, const QString& localEnvelope, qreal ox, qreal oy, qreal frameW, qreal frameH) {
    if (!valid_ || frameW <= 0 || frameH <= 0) return false;
    const QJsonObject in = QJsonDocument::fromJson(localEnvelope.toUtf8()).object();
    const QJsonArray added = in.value(QStringLiteral("shapes")).toArray();
    if (added.isEmpty()) return false;
    const bool local = in.value(QStringLiteral("coordinate_system")).toString() == QLatin1String("element-local");
    const double sourceW = w() > 0 ? w() : frameW;
    QJsonObject env = QJsonDocument::fromJson(pageInk(page).toUtf8()).object();
    if (env.isEmpty()) {
        env.insert(QStringLiteral("version"), QStringLiteral("2.0"));
        env.insert(QStringLiteral("coordinate_system"), QStringLiteral("normalized"));
    }
    QJsonArray shapes = env.value(QStringLiteral("shapes")).toArray();
    for (const QJsonValue& v : added) {
        QJsonObject s = v.toObject();
        const bool oval = s.value(QStringLiteral("type")).toString() == QLatin1String("oval");
        QJsonArray pts = s.value(QStringLiteral("points")).toArray();
        for (int k = 0; k < pts.size(); ++k) {
            const QJsonArray p = pts.at(k).toArray();
            if (p.size() < 2) continue;
            double x = p.at(0).toDouble(), y = p.at(1).toDouble();
            if (local) {
                if (!(oval && k == 1)) { x += ox; y += oy; }   // the oval's radii are a size, not a position
                x /= frameW; y /= frameH;
            }
            pts.replace(k, QJsonArray{ x, y });
        }
        s.insert(QStringLiteral("points"), pts);
        if (local) s.insert(QStringLiteral("stroke_width"), s.value(QStringLiteral("stroke_width")).toDouble() * sourceW / frameW);
        shapes.append(s);
    }
    env.insert(QStringLiteral("shapes"), shapes);
    return setPageInk(page, compactJson(env));
}

bool MediaHost::erasePageStrokeAt(int page, qreal fx, qreal fy, qreal frameW, qreal frameH) {
    if (!valid_ || frameW <= 0 || frameH <= 0) return false;
    QJsonObject env = QJsonDocument::fromJson(pageInk(page).toUtf8()).object();
    QJsonArray shapes = env.value(QStringLiteral("shapes")).toArray();
    const double widthK = frameW / (w() > 0 ? double(w()) : frameW);   // source px → frame px
    int hit = -1;
    for (int i = shapes.size() - 1; i >= 0 && hit < 0; --i) {   // topmost first
        qcv::ActiveStroke s;
        if (!qcv::AnnotationSerializer::jsonToStroke(shapes.at(i).toObject(), s)) continue;
        const double tol = std::max(6.0, double(s.strokeWidth) * widthK / 2.0 + 4.0);
        if (strokeDistance(s, QPointF(fx, fy), frameW, frameH) <= tol) hit = i;
    }
    if (hit < 0) return false;
    shapes.removeAt(hit);
    if (shapes.isEmpty()) env.remove(QStringLiteral("shapes")); else env.insert(QStringLiteral("shapes"), shapes);
    const bool bare = shapes.isEmpty() && !env.contains(QStringLiteral("texts"));
    return setPageInk(page, bare ? QString() : compactJson(env));
}
