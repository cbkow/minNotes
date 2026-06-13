#include "SketchCanvas.h"

#include "annotation_serializer.h"
#include "annotation_thumbnail.h"

#include <QCursor>
#include <QHoverEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QUrl>

SketchCanvas::SketchCanvas(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    setAcceptedMouseButtons(Qt::NoButton);   // disarmed → events pass through

    annot_.setStrokeFinalizedCallback(
        [this](std::unique_ptr<qcv::ActiveStroke> s) { commitStroke(std::move(s)); });
    annot_.setEraseAtCallback([this](QPointF norm) { eraseAt(norm); });
}

SketchCanvas::~SketchCanvas() = default;

void SketchCanvas::setData(const QString &data)
{
    if (data == data_) return;
    data_ = data;
    strokes_ = qcv::AnnotationSerializer::jsonStringToStrokes(data_);
    parseImages(data_);
    // Keep the selection only if its index still resolves (sizes shift on
    // undo / external edits); otherwise drop it.
    if ((selKind_ == SelStroke && selIdx_ >= int(strokes_.size()))
        || (selKind_ == SelImage && selIdx_ >= int(images_.size())))
        clearSelection();
    emit dataChanged();
    update();
}

void SketchCanvas::parseImages(const QString &data)
{
    images_.clear();
    if (data.isEmpty()) return;
    const QJsonObject root = QJsonDocument::fromJson(data.toUtf8()).object();
    const QJsonArray arr = root.value(QStringLiteral("images")).toArray();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        const QString src = o.value(QStringLiteral("src")).toString();
        if (src.isEmpty()) continue;
        SketchImage im;
        im.src  = src;
        im.rect = QRectF(o.value(QStringLiteral("x")).toDouble(),
                         o.value(QStringLiteral("y")).toDouble(),
                         o.value(QStringLiteral("w")).toDouble(),
                         o.value(QStringLiteral("h")).toDouble());
        images_.push_back(im);
    }
}

const QImage &SketchCanvas::imageFor(const QString &src)
{
    auto it = imgCache_.constFind(src);
    if (it != imgCache_.constEnd()) return it.value();
    const QString path = src.startsWith(QLatin1String("file:")) ? QUrl(src).toLocalFile() : src;
    return *imgCache_.insert(src, QImage(path));
}

void SketchCanvas::setTool(const QString &tool)
{
    if (tool == toolName_) return;
    toolName_ = tool;

    qcv::DrawingTool t = qcv::DrawingTool::None;
    if      (tool == QLatin1String("freehand")) t = qcv::DrawingTool::Freehand;
    else if (tool == QLatin1String("rect"))     t = qcv::DrawingTool::Rectangle;
    else if (tool == QLatin1String("oval"))     t = qcv::DrawingTool::Oval;
    else if (tool == QLatin1String("arrow"))    t = qcv::DrawingTool::Arrow;
    else if (tool == QLatin1String("line"))     t = qcv::DrawingTool::Line;
    else if (tool == QLatin1String("eraser"))   t = qcv::DrawingTool::Eraser;

    drawToolActive_ = (t != qcv::DrawingTool::None);
    annot_.setActiveTool(t);
    annot_.setMode(t == qcv::DrawingTool::None ? qcv::ViewportMode::Playback
                                               : qcv::ViewportMode::Annotation);
    if (drawToolActive_) clearSelection();   // arming a draw tool leaves select
    applyAcceptedButtons();
    setDrawing(false);
    emit toolChanged();
    update();
}

void SketchCanvas::setSelectable(bool s)
{
    if (s == selectable_) return;
    selectable_ = s;
    setAcceptHoverEvents(s);   // resize-cursor feedback — full-frame canvas only
    if (!s) clearSelection();
    applyAcceptedButtons();
    emit selectableChanged();
}

void SketchCanvas::applyAcceptedButtons()
{
    const bool accept = armed() || inSelectMode();
    setAcceptedMouseButtons(accept ? Qt::LeftButton : Qt::NoButton);
    if (armed())            setCursor(QCursor(Qt::CrossCursor));
    else if (inSelectMode()) setCursor(QCursor(Qt::ArrowCursor));
    else                    setCursor(QCursor());
}

void SketchCanvas::setColor(const QColor &c)
{
    if (c == annot_.drawingColor()) return;
    annot_.setDrawingColor(c);
    emit colorChanged();
}

void SketchCanvas::setStrokeWidth(qreal w)
{
    if (qFuzzyCompare(float(w), annot_.strokeWidth())) return;
    annot_.setStrokeWidth(float(w));
    emit strokeWidthChanged();
}

void SketchCanvas::setSourceWidth(int w)
{
    if (w == sourceWidth_) return;
    sourceWidth_ = w;
    emit sourceWidthChanged();
    update();
}

void SketchCanvas::paint(QPainter *p)
{
    if (width() <= 0 || height() <= 0) return;
    p->setRenderHint(QPainter::Antialiasing, true);
    p->setRenderHint(QPainter::SmoothPixmapTransform, true);
    // Image layer (beneath the ink): normalized rect → display px.
    for (const SketchImage &im : images_) {
        const QImage &img = imageFor(im.src);
        if (img.isNull()) continue;
        const QRectF target(im.rect.x() * width(), im.rect.y() * height(),
                            im.rect.width() * width(), im.rect.height() * height());
        p->drawImage(target, img);
    }
    const double scale = sourceWidth_ > 0 ? width() / double(sourceWidth_) : 1.0;
    for (const qcv::ActiveStroke &s : strokes_)
        qcv::paintStroke(*p, s, width(), height(), scale);
    qcv::ActiveStroke live;
    if (annot_.snapshotActiveStroke(live))
        qcv::paintStroke(*p, live, width(), height(), scale);

    // Selection affordance. An image gets a translucent accent wash + solid
    // border (a thin outline is lost against the picture); a stroke gets a
    // dashed box just outside its bounds. Both get corner resize handles.
    if (selKind_ != SelNone) {
        const QRectF r = selDisplayRect();
        if (r.isValid()) {
            const QColor accent(0x01, 0x89, 0xf1);   // family accent
            if (selKind_ == SelImage) {
                p->fillRect(r, QColor(accent.red(), accent.green(), accent.blue(), 60));
                QPen pen(accent, 2.0); pen.setCosmetic(true);
                p->setBrush(Qt::NoBrush); p->setPen(pen);
                p->drawRect(r);
            } else {
                QPen pen(accent, 1.5, Qt::DashLine); pen.setCosmetic(true);
                p->setBrush(Qt::NoBrush); p->setPen(pen);
                p->drawRect(r);
            }
            const double hs = 4.5;   // handle half-size
            const QPointF cs[4] = { r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight() };
            p->setBrush(accent);
            p->setPen(QPen(QColor(255, 255, 255), 1.0));
            for (const QPointF &c : cs)
                p->drawRect(QRectF(c.x() - hs, c.y() - hs, hs * 2, hs * 2));
        }
    }
}

void SketchCanvas::mousePressEvent(QMouseEvent *e)
{
    if (inSelectMode()) { selectPress(e->position()); e->accept(); return; }
    route(qcv::PointerPhase::Press, e->position(), qint64(e->timestamp()));
    e->accept();
}

void SketchCanvas::mouseMoveEvent(QMouseEvent *e)
{
    if (inSelectMode()) { selectMove(e->position()); e->accept(); return; }
    route(qcv::PointerPhase::Move, e->position(), qint64(e->timestamp()));
    e->accept();
}

void SketchCanvas::mouseReleaseEvent(QMouseEvent *e)
{
    if (inSelectMode()) { selectRelease(); e->accept(); return; }
    route(qcv::PointerPhase::Release, e->position(), qint64(e->timestamp()));
    e->accept();
}

void SketchCanvas::geometryChange(const QRectF &newGeo, const QRectF &oldGeo)
{
    QQuickPaintedItem::geometryChange(newGeo, oldGeo);
    annot_.setViewportRect(QPointF(0, 0), newGeo.size());
}

void SketchCanvas::route(qcv::PointerPhase phase, QPointF pos, qint64 tMs)
{
    annot_.setViewportRect(QPointF(0, 0), size());
    annot_.onPointerEvent(phase, pos, tMs);
    if (phase == qcv::PointerPhase::Press)   setDrawing(true);
    if (phase == qcv::PointerPhase::Release) setDrawing(false);
    update();   // live stroke repaint
}

void SketchCanvas::cancelStroke()
{
    annot_.cancelActiveStroke();
    setDrawing(false);
    update();
}

void SketchCanvas::commitStroke(std::unique_ptr<qcv::ActiveStroke> stroke)
{
    if (!stroke) return;
    std::vector<qcv::ActiveStroke> strokes =
        qcv::AnnotationSerializer::jsonStringToStrokes(data_);
    strokes.push_back(*stroke);
    // QML commits this to the model; the merged content flows back through
    // the data binding (the model is the single source of truth).
    emit edited(qcv::AnnotationSerializer::strokesToJsonString(strokes));
}

void SketchCanvas::eraseAt(QPointF norm)
{
    std::vector<qcv::ActiveStroke> strokes =
        qcv::AnnotationSerializer::jsonStringToStrokes(data_);
    if (strokes.empty()) return;

    // QCView's hit-test verbatim (see VideoAnnotator::eraseAt).
    const double tol = 0.012;
    int hitIdx = -1;
    for (int i = int(strokes.size()) - 1; i >= 0; --i) {
        const qcv::ActiveStroke &s = strokes[size_t(i)];
        if (s.points.empty()) continue;
        double minX = s.points.front().x(), maxX = minX;
        double minY = s.points.front().y(), maxY = minY;
        for (const QPointF &p : s.points) {
            minX = std::min(minX, p.x()); maxX = std::max(maxX, p.x());
            minY = std::min(minY, p.y()); maxY = std::max(maxY, p.y());
        }
        const double pad = std::max(tol, double(s.strokeWidth) / 1920.0);
        if (norm.x() >= minX - pad && norm.x() <= maxX + pad
            && norm.y() >= minY - pad && norm.y() <= maxY + pad) {
            hitIdx = i;
            break;
        }
    }
    if (hitIdx < 0) return;

    strokes.erase(strokes.begin() + hitIdx);
    emit edited(qcv::AnnotationSerializer::strokesToJsonString(strokes));
}

void SketchCanvas::setDrawing(bool d)
{
    if (d == drawing_) return;
    drawing_ = d;
    emit drawingChanged();
}

// ---- Selection / move ------------------------------------------------------

void SketchCanvas::clearSelection()
{
    moving_ = false;
    if (selKind_ == SelNone) return;
    selKind_ = SelNone; selIdx_ = -1;
    emit selectionChanged();
    update();
}

QRectF SketchCanvas::strokeBoundsNorm(int idx) const
{
    if (idx < 0 || idx >= int(strokes_.size())) return {};
    const qcv::ActiveStroke &s = strokes_[size_t(idx)];
    if (s.points.empty()) return {};
    double minX = s.points.front().x(), maxX = minX;
    double minY = s.points.front().y(), maxY = minY;
    for (const QPointF &pt : s.points) {
        minX = std::min(minX, pt.x()); maxX = std::max(maxX, pt.x());
        minY = std::min(minY, pt.y()); maxY = std::max(maxY, pt.y());
    }
    return QRectF(minX, minY, maxX - minX, maxY - minY);
}

QRectF SketchCanvas::selBoundsNorm() const
{
    if (selKind_ == SelStroke) return strokeBoundsNorm(selIdx_);
    if (selKind_ == SelImage && selIdx_ >= 0 && selIdx_ < int(images_.size()))
        return images_[size_t(selIdx_)].rect;
    return {};
}

int SketchCanvas::hitTest(QPointF norm, SelKind &kindOut) const
{
    // Strokes are drawn on top of images → test them first (reverse = topmost).
    const double tol = 0.012;
    for (int i = int(strokes_.size()) - 1; i >= 0; --i) {
        const QRectF b = strokeBoundsNorm(i);
        if (b.isNull()) continue;
        const double pad = std::max(tol, double(strokes_[size_t(i)].strokeWidth) / 1920.0);
        if (b.adjusted(-pad, -pad, pad, pad).contains(norm)) { kindOut = SelStroke; return i; }
    }
    for (int i = int(images_.size()) - 1; i >= 0; --i)
        if (images_[size_t(i)].rect.contains(norm)) { kindOut = SelImage; return i; }
    kindOut = SelNone;
    return -1;
}

void SketchCanvas::selectPress(QPointF pos)
{
    if (width() <= 0 || height() <= 0) return;
    // A press on the current selection's corner handle starts a resize.
    if (selKind_ != SelNone) {
        const int h = handleAtPx(pos);
        if (h >= 0) { beginResize(h); return; }
    }
    const QPointF norm(pos.x() / width(), pos.y() / height());
    SelKind k = SelNone;
    const int idx = hitTest(norm, k);
    if (k == SelNone) { clearSelection(); return; }
    if (k != selKind_ || idx != selIdx_) { selKind_ = k; selIdx_ = idx; emit selectionChanged(); }
    moving_ = true;
    moveDirty_ = false;
    lastNorm_ = norm;
    update();
}

void SketchCanvas::translateSelection(QPointF dNorm)
{
    if (selKind_ == SelNone) return;
    const QRectF b = selBoundsNorm();
    if (!b.isValid()) return;
    // Clamp the delta so the element's bounds stay within [0,1].
    double dx = std::clamp(dNorm.x(), -b.left(), 1.0 - b.right());
    double dy = std::clamp(dNorm.y(), -b.top(),  1.0 - b.bottom());
    if (dx == 0.0 && dy == 0.0) return;
    if (selKind_ == SelStroke) {
        for (QPointF &pt : strokes_[size_t(selIdx_)].points) pt += QPointF(dx, dy);
    } else {
        images_[size_t(selIdx_)].rect.translate(dx, dy);
    }
    moveDirty_ = true;
    update();
}

void SketchCanvas::selectMove(QPointF pos)
{
    if (width() <= 0 || height() <= 0) return;
    const QPointF norm(pos.x() / width(), pos.y() / height());
    if (resizing_) { resizeTo(norm); return; }
    if (!moving_) return;
    translateSelection(norm - lastNorm_);
    lastNorm_ = norm;
}

void SketchCanvas::selectRelease()
{
    if (!moving_ && !resizing_) return;
    moving_ = false; resizing_ = false; grabCorner_ = -1;
    if (!moveDirty_) return;   // a click-to-select must not commit a no-op undo step
    moveDirty_ = false;
    if (selKind_ == SelStroke) {
        emit edited(qcv::AnnotationSerializer::strokesToJsonString(strokes_));
    } else if (selKind_ == SelImage && selIdx_ < int(images_.size())) {
        const QRectF r = images_[size_t(selIdx_)].rect;
        emit imageRectChanged(selIdx_, r.x(), r.y(), r.width(), r.height());
    }
}

void SketchCanvas::deleteSelection()
{
    if (selKind_ == SelStroke && selIdx_ < int(strokes_.size())) {
        strokes_.erase(strokes_.begin() + selIdx_);
        const int wasIdx = selIdx_;
        clearSelection();
        Q_UNUSED(wasIdx);
        emit edited(qcv::AnnotationSerializer::strokesToJsonString(strokes_));
    } else if (selKind_ == SelImage && selIdx_ < int(images_.size())) {
        const int idx = selIdx_;
        clearSelection();
        emit imageRemoved(idx);
    }
}

QRectF SketchCanvas::selDisplayRect() const
{
    const QRectF b = selBoundsNorm();
    if (!b.isValid() || width() <= 0 || height() <= 0) return {};
    QRectF r(b.x() * width(), b.y() * height(), b.width() * width(), b.height() * height());
    if (selKind_ == SelStroke) r = r.adjusted(-3, -3, 3, 3);   // breathing room around ink
    return r;
}

int SketchCanvas::handleAtPx(QPointF px) const
{
    const QRectF r = selDisplayRect();
    if (!r.isValid()) return -1;
    const double tol = 9.0;
    const QPointF cs[4] = { r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight() };
    for (int i = 0; i < 4; ++i)
        if (QLineF(px, cs[i]).length() <= tol) return i;
    return -1;
}

void SketchCanvas::beginResize(int corner)
{
    grabCorner_ = corner;
    origBounds_ = selBoundsNorm();
    if (selKind_ == SelStroke && selIdx_ >= 0 && selIdx_ < int(strokes_.size()))
        origPoints_ = strokes_[size_t(selIdx_)].points;   // absolute base (no drift)
    resizing_ = true;
    moving_ = false;
    moveDirty_ = false;
}

void SketchCanvas::resizeTo(QPointF norm)
{
    if (!origBounds_.isValid()) return;
    const double L = origBounds_.left(), R = origBounds_.right();
    const double T = origBounds_.top(),  B = origBounds_.bottom();
    // The grabbed corner moves; the opposite corner (pivot) stays fixed.
    QPointF grab, pivot;
    switch (grabCorner_) {
        case 0: grab = {L, T}; pivot = {R, B}; break;   // TL
        case 1: grab = {R, T}; pivot = {L, B}; break;   // TR
        case 2: grab = {L, B}; pivot = {R, T}; break;   // BL
        default: grab = {R, B}; pivot = {L, T}; break;  // BR
    }
    const double dxs = grab.x() - pivot.x();   // signed extents (orig)
    const double dys = grab.y() - pivot.y();
    const double origW = std::abs(dxs), origH = std::abs(dys);
    if (origW <= 0 || origH <= 0) return;
    const double newW = std::abs(norm.x() - pivot.x());
    const double newH = std::abs(norm.y() - pivot.y());
    double s = std::max(newW / origW, newH / origH);              // proportional
    s = std::max(s, std::max(0.03 / origW, 0.03 / origH));        // min size
    // Clamp so the moving (far) corner stays on the canvas.
    const double sMaxX = dxs > 0 ? (1.0 - pivot.x()) / dxs : (dxs < 0 ? -pivot.x() / dxs : 1e9);
    const double sMaxY = dys > 0 ? (1.0 - pivot.y()) / dys : (dys < 0 ? -pivot.y() / dys : 1e9);
    s = std::min(s, std::min(sMaxX, sMaxY));
    if (s <= 0) return;
    moveDirty_ = true;
    if (selKind_ == SelStroke && origPoints_.size() == strokes_[size_t(selIdx_)].points.size()) {
        std::vector<QPointF> &pts = strokes_[size_t(selIdx_)].points;
        for (size_t i = 0; i < pts.size(); ++i)
            pts[i] = pivot + (origPoints_[i] - pivot) * s;
    } else if (selKind_ == SelImage) {
        const QPointF far = pivot + QPointF(dxs, dys) * s;
        images_[size_t(selIdx_)].rect =
            QRectF(QPointF(std::min(pivot.x(), far.x()), std::min(pivot.y(), far.y())),
                   QPointF(std::max(pivot.x(), far.x()), std::max(pivot.y(), far.y())));
    }
    update();
}

void SketchCanvas::hoverMoveEvent(QHoverEvent *e)
{
    if (inSelectMode() && selKind_ != SelNone) {
        const int h = handleAtPx(e->position());
        if (h == 0 || h == 3)      setCursor(QCursor(Qt::SizeFDiagCursor));   // TL / BR
        else if (h == 1 || h == 2) setCursor(QCursor(Qt::SizeBDiagCursor));   // TR / BL
        else                       setCursor(QCursor(Qt::ArrowCursor));
    }
    QQuickPaintedItem::hoverMoveEvent(e);
}
