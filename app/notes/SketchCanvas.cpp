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
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

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
    invalidateBounds();
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
    const bool accept = panMode_ || armed() || inSelectMode();
    setAcceptedMouseButtons(accept ? Qt::LeftButton : Qt::NoButton);
    if (panMode_)            setCursor(QCursor(panning_ ? Qt::ClosedHandCursor
                                                        : Qt::OpenHandCursor));
    else if (armed())        setCursor(QCursor(Qt::CrossCursor));
    else if (inSelectMode()) setCursor(QCursor(Qt::ArrowCursor));
    else                     setCursor(QCursor());
}

void SketchCanvas::setPanMode(bool on)
{
    if (on == panMode_) return;
    panMode_ = on;
    if (!on) panning_ = false;
    applyAcceptedButtons();
    emit panModeChanged();
    update();   // hide/show the brush circle
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
    invalidateBounds();
    pushViewport();
    emit sourceWidthChanged();
    update();
}

void SketchCanvas::setSourceHeight(int h)
{
    if (h == sourceHeight_) return;
    sourceHeight_ = h;
    invalidateBounds();
    pushViewport();
    emit sourceHeightChanged();
    update();
}

void SketchCanvas::setCameraEnabled(bool on)
{
    if (on == cameraEnabled_) return;
    cameraEnabled_ = on;
    annot_.setUnclamped(on);   // camera mode captures overflow ink
    pushViewport();
    emit cameraEnabledChanged();
    update();
}

void SketchCanvas::setPanX(qreal x)
{
    if (qFuzzyCompare(x, panX_)) return;
    panX_ = x;
    pushViewport();
    emit cameraChanged();
    update();
}

void SketchCanvas::setPanY(qreal y)
{
    if (qFuzzyCompare(y, panY_)) return;
    panY_ = y;
    pushViewport();
    emit cameraChanged();
    update();
}

void SketchCanvas::setZoom(qreal z)
{
    z = std::clamp(z, 0.02, 8.0);   // hard sanity bounds; QML applies the UX range
    if (qFuzzyCompare(z, zoom_)) return;
    zoom_ = z;
    pushViewport();
    emit cameraChanged();
    update();
}

void SketchCanvas::setFrameBorderColor(const QColor &c)
{
    if (c == frameBorderColor_) return;
    frameBorderColor_ = c;
    emit frameBorderColorChanged();
    update();
}

qreal SketchCanvas::effZoom() const
{
    if (cameraEnabled_) return zoom_;
    return sourceWidth_ > 0 ? width() / double(sourceWidth_) : 1.0;
}

QRectF SketchCanvas::frameScreenRect() const
{
    if (!cameraEnabled_) return QRectF(0, 0, width(), height());
    const int sw = sourceWidth_ > 0 ? sourceWidth_ : 1;
    const int sh = sourceHeight_ > 0 ? sourceHeight_ : sw;
    return QRectF(panX_, panY_, sw * zoom_, sh * zoom_);
}

QPointF SketchCanvas::screenToNorm(QPointF pos) const
{
    const QRectF f = frameScreenRect();
    if (f.width() <= 0.0 || f.height() <= 0.0) return {};
    return QPointF((pos.x() - f.x()) / f.width(), (pos.y() - f.y()) / f.height());
}

QPointF SketchCanvas::normToScreen(QPointF norm) const
{
    const QRectF f = frameScreenRect();
    return QPointF(f.x() + norm.x() * f.width(), f.y() + norm.y() * f.height());
}

void SketchCanvas::pushViewport()
{
    const QRectF f = frameScreenRect();
    annot_.setViewportRect(f.topLeft(), f.size());
}

double SketchCanvas::hitTolNorm() const
{
    const QRectF f = frameScreenRect();
    return f.width() > 0.0 ? 12.0 / f.width() : 0.012;
}

QRectF SketchCanvas::contentBoundsNorm() const
{
    if (contentBounds_) return *contentBounds_;
    const double srcW = sourceWidth_  > 0 ? sourceWidth_  : 480.0;
    const double srcH = sourceHeight_ > 0 ? sourceHeight_ : srcW;
    QRectF acc;
    auto add = [&acc](const QRectF &r) { acc = acc.isValid() ? acc.united(r) : r; };
    for (const qcv::ActiveStroke &s : strokes_) {
        const QRectF b = qcv::strokeBoundsNorm(s);
        if (b.isNull()) continue;
        const double px = s.strokeWidth / (2.0 * srcW);
        const double py = s.strokeWidth / (2.0 * srcH);
        add(b.adjusted(-px, -py, px, py));
    }
    for (const SketchImage &im : images_) add(im.rect);
    contentBounds_ = acc;
    return acc;
}

bool SketchCanvas::hasOverflow() const
{
    const QRectF b = contentBoundsNorm();
    return b.isValid() && !QRectF(0, 0, 1, 1).contains(b);
}

void SketchCanvas::paint(QPainter *p)
{
    if (width() <= 0 || height() <= 0) return;
    const QRectF f = frameScreenRect();
    if (f.width() <= 0 || f.height() <= 0) return;
    p->setRenderHint(QPainter::Antialiasing, true);
    p->setRenderHint(QPainter::SmoothPixmapTransform, true);

    // Scene painter, frame space via translate; paintStroke's (w,h) is the
    // frame's SCREEN size, so normalized coords land in screen px and
    // effZoom() carries source-px stroke widths to screen.
    const double scale = effZoom();
    auto drawScene = [&](QPainter *pp) {
        pp->translate(f.topLeft());
        for (const SketchImage &im : images_) {
            const QImage &img = imageFor(im.src);
            if (img.isNull()) continue;
            const QRectF target(im.rect.x() * f.width(), im.rect.y() * f.height(),
                                im.rect.width() * f.width(), im.rect.height() * f.height());
            pp->drawImage(target, img);
        }
        for (const qcv::ActiveStroke &s : strokes_)
            qcv::paintStroke(*pp, s, f.width(), f.height(), scale);
        qcv::ActiveStroke live;
        if (annot_.snapshotActiveStroke(live))
            qcv::paintStroke(*pp, live, f.width(), f.height(), scale);
    };

    // Ghost pass (tab only, and only when overflow exists — or mid-gesture,
    // so the live stroke stays visible while crossing the edge): the whole
    // scene unclipped at 35%. The clipped full-opacity pass paints on top.
    if (cameraEnabled_ && (drawing_ || moving_ || resizing_ || hasOverflow())) {
        p->save();
        p->setOpacity(0.35);
        drawScene(p);
        p->restore();
    }

    // Main pass, clipped to the frame — overflow ink is invisible here (the
    // embed's hard clip; in legacy mode the frame IS the item, a no-op).
    p->save();
    p->setClipRect(f);
    drawScene(p);
    p->restore();

    // Frame border (camera mode): drawn here, never a lagging QML sibling.
    if (cameraEnabled_) {
        QPen bp(frameBorderColor_, 1.0); bp.setCosmetic(true);
        p->setBrush(Qt::NoBrush); p->setPen(bp);
        p->drawRect(f);
    }

    // Brush-size cursor: the exact mark the armed tool would make at this
    // zoom (marks are canvas-absolute — the circle IS the feedback that the
    // panel number means source px).
    if (cameraEnabled_ && armed() && hoverValid_ && !drawing_ && !panMode_
        && annot_.activeTool() != qcv::DrawingTool::Eraser) {
        const double r = std::max(1.0, annot_.strokeWidth() * effZoom() * 0.5);
        QPen cp(QColor(160, 160, 160, 200), 1.0); cp.setCosmetic(true);
        p->setBrush(Qt::NoBrush); p->setPen(cp);
        p->drawEllipse(hoverPos_, r, r);
    }

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
    if (panMode_) {
        panning_ = true;
        lastPanPos_ = e->position();
        applyAcceptedButtons();   // closed hand
        e->accept();
        return;
    }
    if (inSelectMode()) { selectPress(e->position()); e->accept(); return; }
    route(qcv::PointerPhase::Press, e->position(), qint64(e->timestamp()));
    e->accept();
}

void SketchCanvas::mouseMoveEvent(QMouseEvent *e)
{
    if (panning_) {
        const QPointF d = e->position() - lastPanPos_;
        lastPanPos_ = e->position();
        setPanX(panX_ + d.x());
        setPanY(panY_ + d.y());
        emit userCameraInput();
        e->accept();
        return;
    }
    if (inSelectMode()) { selectMove(e->position()); e->accept(); return; }
    route(qcv::PointerPhase::Move, e->position(), qint64(e->timestamp()));
    e->accept();
}

void SketchCanvas::mouseReleaseEvent(QMouseEvent *e)
{
    if (panning_) {
        panning_ = false;
        applyAcceptedButtons();   // back to open hand
        e->accept();
        return;
    }
    if (inSelectMode()) { selectRelease(); e->accept(); return; }
    route(qcv::PointerPhase::Release, e->position(), qint64(e->timestamp()));
    e->accept();
}

void SketchCanvas::wheelEvent(QWheelEvent *e)
{
    if (!cameraEnabled_) { e->ignore(); return; }
    if (e->modifiers() & Qt::ControlModifier) {
        // ⌘/Ctrl-wheel: zoom anchored at the pointer.
        const double factor = std::pow(1.2, e->angleDelta().y() / 120.0);
        zoomAboutPoint(zoom_ * factor, e->position());
    } else {
        // Plain scroll / trackpad: pan. pixelDelta is the trackpad's true
        // gesture; wheel notches fall back to angleDelta.
        const QPoint px = e->pixelDelta();
        const QPointF d = !px.isNull()
            ? QPointF(px)
            : QPointF(e->angleDelta().x(), e->angleDelta().y()) / 2.0;
        setPanX(panX_ + d.x());
        setPanY(panY_ + d.y());
    }
    emit userCameraInput();
    e->accept();
}

bool SketchCanvas::event(QEvent *ev)
{
    if (cameraEnabled_ && ev->type() == QEvent::NativeGesture) {
        auto *g = static_cast<QNativeGestureEvent *>(ev);
        if (g->gestureType() == Qt::ZoomNativeGesture) {
            zoomAboutPoint(zoom_ * (1.0 + g->value()), g->position());
            emit userCameraInput();
            return true;
        }
    }
    return QQuickPaintedItem::event(ev);
}

void SketchCanvas::zoomAboutPoint(qreal newZoom, QPointF anchor)
{
    newZoom = std::clamp(newZoom, 0.02, 8.0);
    if (qFuzzyCompare(newZoom, zoom_)) return;
    // The world point under `anchor` stays under it: pan' = a − (a − pan)·z'/z.
    const qreal r = newZoom / zoom_;
    panX_ = anchor.x() - (anchor.x() - panX_) * r;
    panY_ = anchor.y() - (anchor.y() - panY_) * r;
    zoom_ = newZoom;
    pushViewport();
    emit cameraChanged();
    update();
}

void SketchCanvas::geometryChange(const QRectF &newGeo, const QRectF &oldGeo)
{
    QQuickPaintedItem::geometryChange(newGeo, oldGeo);
    pushViewport();
}

void SketchCanvas::route(qcv::PointerPhase phase, QPointF pos, qint64 tMs)
{
    pushViewport();
    // Eraser gesture bracket. Begin BEFORE dispatching Press — the annotator
    // erases at the press point itself, so the working copy must exist first.
    if (phase == qcv::PointerPhase::Press
        && annot_.activeTool() == qcv::DrawingTool::Eraser) {
        eraseGesture_ = true;
        eraseDirty_   = false;
        eraseStrokes_ = qcv::AnnotationSerializer::jsonStringToStrokes(data_);
    }
    annot_.onPointerEvent(phase, pos, tMs);
    if (phase == qcv::PointerPhase::Press)   setDrawing(true);
    if (phase == qcv::PointerPhase::Release) {
        if (eraseGesture_) {
            if (eraseDirty_)
                emit edited(qcv::AnnotationSerializer::strokesToJsonString(eraseStrokes_));
            eraseGesture_ = false;
        }
        setDrawing(false);
    }
    update();   // live stroke repaint
}

void SketchCanvas::cancelStroke()
{
    annot_.cancelActiveStroke();
    if (eraseGesture_) {          // Esc mid-erase: drop the working copy —
        eraseGesture_ = false;    // nothing reached the model, so restoring
        eraseDirty_   = false;    // the paint source undoes the whole drag.
        strokes_ = qcv::AnnotationSerializer::jsonStringToStrokes(data_);
        invalidateBounds();
    }
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

// QCView's eraser hit-test shape (see VideoAnnotator::eraseAt): bounding box
// + tolerance, last-drawn first, one hit per event. Tolerances are now the
// caller's: `tol` is a SCREEN-px slop converted to normalized (constant feel
// at any zoom — the DocInkCanvas kHitTolPx pattern), `srcW` converts the
// source-px stroke width to its normalized half-width.
static int eraseHitIndex(const std::vector<qcv::ActiveStroke> &strokes, QPointF norm,
                         double tol, double srcW)
{
    for (int i = int(strokes.size()) - 1; i >= 0; --i) {
        const qcv::ActiveStroke &s = strokes[size_t(i)];
        if (s.points.empty()) continue;
        double minX = s.points.front().x(), maxX = minX;
        double minY = s.points.front().y(), maxY = minY;
        for (const QPointF &p : s.points) {
            minX = std::min(minX, p.x()); maxX = std::max(maxX, p.x());
            minY = std::min(minY, p.y()); maxY = std::max(maxY, p.y());
        }
        const double pad = std::max(tol, double(s.strokeWidth) / (2.0 * srcW));
        if (norm.x() >= minX - pad && norm.x() <= maxX + pad
            && norm.y() >= minY - pad && norm.y() <= maxY + pad)
            return i;
    }
    return -1;
}

void SketchCanvas::eraseAt(QPointF norm)
{
    const double tol = hitTolNorm();
    const double srcW = sourceWidth_ > 0 ? sourceWidth_ : 480.0;
    // The annotator only fires this during a press-drag, i.e. inside a
    // gesture; the direct path stays as a safety net.
    if (!eraseGesture_) {
        std::vector<qcv::ActiveStroke> strokes =
            qcv::AnnotationSerializer::jsonStringToStrokes(data_);
        const int hitIdx = eraseHitIndex(strokes, norm, tol, srcW);
        if (hitIdx < 0) return;
        strokes.erase(strokes.begin() + hitIdx);
        emit edited(qcv::AnnotationSerializer::strokesToJsonString(strokes));
        return;
    }
    const int hitIdx = eraseHitIndex(eraseStrokes_, norm, tol, srcW);
    if (hitIdx < 0) return;
    eraseStrokes_.erase(eraseStrokes_.begin() + hitIdx);
    eraseDirty_ = true;
    strokes_ = eraseStrokes_;   // live paint feedback; the model commits on release
    invalidateBounds();
    if (selKind_ == SelStroke && selIdx_ >= int(strokes_.size())) clearSelection();
    update();
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
    return qcv::strokeBoundsNorm(strokes_[size_t(idx)]);   // oval-aware (shared)
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
    const double tol = hitTolNorm();
    const double srcW = sourceWidth_ > 0 ? sourceWidth_ : 480.0;
    for (int i = int(strokes_.size()) - 1; i >= 0; --i) {
        const QRectF b = strokeBoundsNorm(i);
        if (b.isNull()) continue;
        const double pad = std::max(tol, double(strokes_[size_t(i)].strokeWidth) / (2.0 * srcW));
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
    const QPointF norm = screenToNorm(pos);
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
    // No bounds clamp: overflow is legal (capture-and-hide) — the frame clips
    // display in the embed/exports; the tab ghosts it.
    const double dx = dNorm.x(), dy = dNorm.y();
    if (dx == 0.0 && dy == 0.0) return;
    if (selKind_ == SelStroke) {
        qcv::ActiveStroke &s = strokes_[size_t(selIdx_)];
        // Oval stores {center, radii}: move the centre only (translating the
        // radii vector would resize it).
        if (s.tool == qcv::DrawingTool::Oval && !s.points.empty())
            s.points[0] += QPointF(dx, dy);
        else
            for (QPointF &pt : s.points) pt += QPointF(dx, dy);
    } else {
        images_[size_t(selIdx_)].rect.translate(dx, dy);
    }
    moveDirty_ = true;
    invalidateBounds();
    update();
}

void SketchCanvas::selectMove(QPointF pos)
{
    if (width() <= 0 || height() <= 0) return;
    const QPointF norm = screenToNorm(pos);
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
        invalidateBounds();
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
    const QRectF f = frameScreenRect();
    QRectF r(f.x() + b.x() * f.width(), f.y() + b.y() * f.height(),
             b.width() * f.width(), b.height() * f.height());
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
    // No far-corner clamp: resizing past the frame is legal overflow.
    if (s <= 0) return;
    moveDirty_ = true;
    invalidateBounds();
    if (selKind_ == SelStroke && origPoints_.size() == strokes_[size_t(selIdx_)].points.size()) {
        qcv::ActiveStroke &st = strokes_[size_t(selIdx_)];
        std::vector<QPointF> &pts = st.points;
        if (st.tool == qcv::DrawingTool::Oval && pts.size() >= 2) {
            pts[0] = pivot + (origPoints_[0] - pivot) * s;   // centre scales about pivot
            pts[1] = origPoints_[1] * s;                     // radii scale by s
        } else {
            for (size_t i = 0; i < pts.size(); ++i)
                pts[i] = pivot + (origPoints_[i] - pivot) * s;
        }
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
    if (inSelectMode() && selKind_ != SelNone && !panMode_) {
        const int h = handleAtPx(e->position());
        if (h == 0 || h == 3)      setCursor(QCursor(Qt::SizeFDiagCursor));   // TL / BR
        else if (h == 1 || h == 2) setCursor(QCursor(Qt::SizeBDiagCursor));   // TR / BL
        else                       setCursor(QCursor(Qt::ArrowCursor));
    }
    // Brush-size cursor anchor (armed camera mode): repaint tracks the hover.
    if (cameraEnabled_ && armed() && !panMode_) {
        hoverPos_ = e->position();
        hoverValid_ = true;
        update();
    }
    QQuickPaintedItem::hoverMoveEvent(e);
}

void SketchCanvas::hoverLeaveEvent(QHoverEvent *e)
{
    if (hoverValid_) { hoverValid_ = false; update(); }
    QQuickPaintedItem::hoverLeaveEvent(e);
}
