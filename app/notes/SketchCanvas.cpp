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
#include <QVariant>
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
    parseTexts(data_);
    invalidateBounds();
    // Keep selected items only while their indices still resolve (sizes
    // shift on undo / external edits); drop the rest.
    pruneSelection();
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

void SketchCanvas::parseTexts(const QString &data)
{
    texts_.clear();
    if (data.isEmpty()) return;
    const QJsonObject root = QJsonDocument::fromJson(data.toUtf8()).object();
    for (mn::SketchTextSpec &spec : mn::parseSketchTexts(root)) {
        SketchText t;
        t.spec = std::move(spec);
        texts_.push_back(std::move(t));
    }
    refreshTextHeights();
}

void SketchCanvas::refreshTextHeights()
{
    // Heights derive from (text, w×srcW, size, family) — recomputed whenever
    // any input changes; never at paint time.
    const double srcW = sourceWidth_  > 0 ? sourceWidth_  : 480.0;
    const double srcH = sourceHeight_ > 0 ? sourceHeight_ : srcW;
    for (SketchText &t : texts_) {
        t.spec.family = fontFamily_;   // "" → helper probe inside
        t.hNorm = mn::sketchTextHeightSrc(t.spec, srcW) / srcH;
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

    // The text tool places boxes via textCreateRequested — no stroke engine.
    textToolArmed_ = (tool == QLatin1String("text"));
    drawToolActive_ = (t != qcv::DrawingTool::None);
    annot_.setActiveTool(t);
    annot_.setMode(t == qcv::DrawingTool::None ? qcv::ViewportMode::Playback
                                               : qcv::ViewportMode::Annotation);
    if (armed()) clearSelection();   // arming a placing tool leaves select
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
    else if (textToolArmed_) setCursor(QCursor(Qt::IBeamCursor));
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
    refreshTextHeights();
    invalidateBounds();
    pushViewport();
    emit sourceWidthChanged();
    update();
}

void SketchCanvas::setSourceHeight(int h)
{
    if (h == sourceHeight_) return;
    sourceHeight_ = h;
    refreshTextHeights();
    invalidateBounds();
    pushViewport();
    emit sourceHeightChanged();
    update();
}

void SketchCanvas::setFontFamily(const QString &f)
{
    if (f == fontFamily_) return;
    fontFamily_ = f;
    refreshTextHeights();
    invalidateBounds();
    emit fontFamilyChanged();
    update();
}

void SketchCanvas::setEditingTextIndex(int i)
{
    if (i == editingTextIndex_) return;
    editingTextIndex_ = i;
    emit editingTextIndexChanged();
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
    for (const SketchText &t : texts_)
        if (t.hNorm > 0) add(QRectF(t.spec.x, t.spec.y, t.spec.w, t.hNorm));
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
    const double srcW = sourceWidth_  > 0 ? sourceWidth_  : f.width() / scale;
    const double srcH = sourceHeight_ > 0 ? sourceHeight_ : f.height() / scale;
    auto drawScene = [&](QPainter *pp) {
        pp->translate(f.topLeft());
        for (const SketchImage &im : images_) {
            const QImage &img = imageFor(im.src);
            if (img.isNull()) continue;
            const QRectF target(im.rect.x() * f.width(), im.rect.y() * f.height(),
                                im.rect.width() * f.width(), im.rect.height() * f.height());
            pp->drawImage(target, img);
        }
        // Text between images and ink — strokes can circle/arrow over labels.
        // The overlay-edited element is skipped (no double-vision).
        for (int i = 0; i < int(texts_.size()); ++i)
            if (i != editingTextIndex_)
                mn::paintSketchText(*pp, texts_[size_t(i)].spec, srcW, srcH, scale);
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
    if (cameraEnabled_ && drawToolActive_ && hoverValid_ && !drawing_ && !panMode_
        && annot_.activeTool() != qcv::DrawingTool::Eraser) {
        const double r = std::max(1.0, annot_.strokeWidth() * effZoom() * 0.5);
        QPen cp(QColor(160, 160, 160, 200), 1.0); cp.setCosmetic(true);
        p->setBrush(Qt::NoBrush); p->setPen(cp);
        p->drawEllipse(hoverPos_, r, r);
    }

    // Selection affordance. EVERY selected item gets its outline (image =
    // translucent accent wash + solid border, a thin outline is lost against
    // the picture; stroke/text = dashed box just outside the bounds). Resize
    // handles + wrap grips appear only for a SINGLE selection — groups move
    // and delete as one, no group scale.
    if (!sel_.empty()) {
        const QColor accent(0x01, 0x89, 0xf1);   // family accent
        for (const SelItem &it : sel_) {
            const QRectF r = itemDisplayRect(it);
            if (!r.isValid()) continue;
            if (it.kind == SelImage) {
                p->fillRect(r, QColor(accent.red(), accent.green(), accent.blue(), 60));
                QPen pen(accent, 2.0); pen.setCosmetic(true);
                p->setBrush(Qt::NoBrush); p->setPen(pen);
                p->drawRect(r);
            } else {
                QPen pen(accent, 1.5, Qt::DashLine); pen.setCosmetic(true);
                p->setBrush(Qt::NoBrush); p->setPen(pen);
                p->drawRect(r);
            }
        }
        const QRectF r = selDisplayRect();   // valid only for a single selection
        if (r.isValid()) {
            const double hs = 4.5;   // handle half-size
            const QPointF cs[4] = { r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight() };
            p->setBrush(accent);
            p->setPen(QPen(QColor(255, 255, 255), 1.0));
            for (const QPointF &c : cs)
                p->drawRect(QRectF(c.x() - hs, c.y() - hs, hs * 2, hs * 2));
            if (sel_[0].kind == SelText) {   // wrap-width grips at the mid-edges
                const QPointF ms[2] = { QPointF(r.left(),  r.center().y()),
                                        QPointF(r.right(), r.center().y()) };
                for (const QPointF &c : ms)
                    p->drawRect(QRectF(c.x() - hs, c.y() - hs, hs * 2, hs * 2));
            }
        }
    }

    // Live marquee: accent hairline + whisper fill (the drag-ghost language).
    if (marquee_ && marqueeRect_.isValid()) {
        const QRectF f2 = frameScreenRect();
        const QRectF mr(f2.x() + marqueeRect_.x() * f2.width(),
                        f2.y() + marqueeRect_.y() * f2.height(),
                        marqueeRect_.width() * f2.width(),
                        marqueeRect_.height() * f2.height());
        const QColor accent(0x01, 0x89, 0xf1);
        p->fillRect(mr, QColor(accent.red(), accent.green(), accent.blue(), 20));
        QPen pen(accent, 1.0, Qt::DashLine); pen.setCosmetic(true);
        p->setBrush(Qt::NoBrush); p->setPen(pen);
        p->drawRect(mr);
    }
}

void SketchCanvas::mousePressEvent(QMouseEvent *e)
{
    // Hold the grab: hosted inside a Flickable (the PDF tab's page list) the
    // parent would otherwise steal the drag at its flick threshold and turn a
    // half-drawn stroke into a scroll. Only reached while armed/selectable —
    // disarmed embeds accept NoButton and never see the press.
    setKeepMouseGrab(true);
    if (panMode_) {
        panning_ = true;
        lastPanPos_ = e->position();
        applyAcceptedButtons();   // closed hand
        e->accept();
        return;
    }
    if (textToolArmed_) {   // place a text box: the QML overlay takes it from here
        emit textCreateRequested(screenToNorm(e->position()).x(),
                                 screenToNorm(e->position()).y());
        e->accept();
        return;
    }
    if (inSelectMode()) { selectPress(e->position(), e->modifiers()); e->accept(); return; }
    route(qcv::PointerPhase::Press, e->position(), qint64(e->timestamp()));
    e->accept();
}

void SketchCanvas::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (inSelectMode()) {
        SelKind k = SelNone;
        const int idx = hitTest(screenToNorm(e->position()), k);
        if (k == SelText) {
            // The first press of the double-click armed a move — cancel it or
            // a 1px jitter between clicks commits a phantom move txn.
            moving_ = false;
            moveDirty_ = false;
            marquee_ = false;
            if (sel_.size() != 1 || sel_[0].kind != SelText || sel_[0].idx != idx) {
                sel_.clear();
                sel_.push_back({SelText, idx});
                emit selectionChanged();
                update();
            }
            emit textEditRequested(idx);
            e->accept();
            return;
        }
    }
    QQuickPaintedItem::mouseDoubleClickEvent(e);
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
    pruneSelection();
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
    marquee_ = false;
    if (sel_.empty()) return;
    sel_.clear();
    emit selectionChanged();
    update();
}

bool SketchCanvas::selContains(SelKind k, int idx) const
{
    for (const SelItem &it : sel_)
        if (it.kind == k && it.idx == idx) return true;
    return false;
}

void SketchCanvas::toggleSel(SelKind k, int idx)
{
    for (auto it = sel_.begin(); it != sel_.end(); ++it) {
        if (it->kind == k && it->idx == idx) {
            sel_.erase(it);
            emit selectionChanged();
            update();
            return;
        }
    }
    sel_.push_back({k, idx});
    emit selectionChanged();
    update();
}

void SketchCanvas::pruneSelection()
{
    const size_t before = sel_.size();
    sel_.erase(std::remove_if(sel_.begin(), sel_.end(), [this](const SelItem &it) {
        return (it.kind == SelStroke && it.idx >= int(strokes_.size()))
            || (it.kind == SelImage  && it.idx >= int(images_.size()))
            || (it.kind == SelText   && it.idx >= int(texts_.size()))
            || it.idx < 0;
    }), sel_.end());
    if (sel_.size() != before) {
        if (sel_.empty()) moving_ = false;
        emit selectionChanged();
        update();
    }
}

QRectF SketchCanvas::strokeBoundsNorm(int idx) const
{
    if (idx < 0 || idx >= int(strokes_.size())) return {};
    return qcv::strokeBoundsNorm(strokes_[size_t(idx)]);   // oval-aware (shared)
}

QRectF SketchCanvas::itemBoundsNorm(const SelItem &it) const
{
    if (it.kind == SelStroke) return strokeBoundsNorm(it.idx);
    if (it.kind == SelImage && it.idx >= 0 && it.idx < int(images_.size()))
        return images_[size_t(it.idx)].rect;
    if (it.kind == SelText && it.idx >= 0 && it.idx < int(texts_.size())) {
        const SketchText &t = texts_[size_t(it.idx)];
        return QRectF(t.spec.x, t.spec.y, t.spec.w, t.hNorm);
    }
    return {};
}

QRectF SketchCanvas::selBoundsNorm() const
{
    QRectF u;
    for (const SelItem &it : sel_) {
        const QRectF b = itemBoundsNorm(it);
        if (!b.isValid()) continue;
        u = u.isValid() ? u.united(b) : b;
    }
    return u;
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
    // Text boxes sit over images, under ink: derived rect, zero tolerance.
    for (int i = int(texts_.size()) - 1; i >= 0; --i) {
        const SketchText &t = texts_[size_t(i)];
        if (QRectF(t.spec.x, t.spec.y, t.spec.w, t.hNorm).contains(norm)) {
            kindOut = SelText; return i;
        }
    }
    for (int i = int(images_.size()) - 1; i >= 0; --i)
        if (images_[size_t(i)].rect.contains(norm)) { kindOut = SelImage; return i; }
    kindOut = SelNone;
    return -1;
}

void SketchCanvas::selectPress(QPointF pos, Qt::KeyboardModifiers mods)
{
    if (width() <= 0 || height() <= 0) return;
    const bool shift = mods.testFlag(Qt::ShiftModifier);
    // A press on the single selection's corner handle starts a resize
    // (groups move/delete only — no group scale in M1).
    if (sel_.size() == 1 && !shift) {
        const int h = handleAtPx(pos);
        if (h >= 0) { beginResize(h); return; }
    }
    const QPointF norm = screenToNorm(pos);
    SelKind k = SelNone;
    const int idx = hitTest(norm, k);
    if (k == SelNone) {
        // Empty canvas: a plain press clears and starts a marquee; a shift
        // press keeps the set (additive marquee). A no-drag release makes
        // the tiny marquee select nothing, so click-to-deselect survives.
        if (!shift) clearSelection();
        marquee_ = true;
        marqueeAnchor_ = norm;
        marqueeRect_ = QRectF(norm, norm);
        update();
        return;
    }
    if (shift) { toggleSel(k, idx); return; }   // membership toggle, no move
    if (!selContains(k, idx)) {
        // Not in the set → this press replaces the selection. In the set →
        // keep the group and drag it as one.
        sel_.clear();
        sel_.push_back({k, idx});
        emit selectionChanged();
    }
    moving_ = true;
    moveDirty_ = false;
    lastNorm_ = norm;
    update();
}

void SketchCanvas::translateSelection(QPointF dNorm)
{
    if (sel_.empty()) return;
    // No bounds clamp: overflow is legal (capture-and-hide) — the frame clips
    // display in the embed/exports; the tab ghosts it.
    const double dx = dNorm.x(), dy = dNorm.y();
    if (dx == 0.0 && dy == 0.0) return;
    for (const SelItem &it : sel_) {
        if (it.kind == SelStroke && it.idx < int(strokes_.size())) {
            qcv::ActiveStroke &s = strokes_[size_t(it.idx)];
            // Oval stores {center, radii}: move the centre only (translating
            // the radii vector would resize it).
            if (s.tool == qcv::DrawingTool::Oval && !s.points.empty())
                s.points[0] += QPointF(dx, dy);
            else
                for (QPointF &pt : s.points) pt += QPointF(dx, dy);
        } else if (it.kind == SelText && it.idx < int(texts_.size())) {
            texts_[size_t(it.idx)].spec.x += dx;
            texts_[size_t(it.idx)].spec.y += dy;
        } else if (it.kind == SelImage && it.idx < int(images_.size())) {
            images_[size_t(it.idx)].rect.translate(dx, dy);
        }
    }
    moveDirty_ = true;
    invalidateBounds();
    update();
}

void SketchCanvas::selectMove(QPointF pos)
{
    if (width() <= 0 || height() <= 0) return;
    const QPointF norm = screenToNorm(pos);
    if (marquee_) {
        marqueeRect_ = QRectF(marqueeAnchor_, norm).normalized();
        update();
        return;
    }
    if (resizing_) { resizeTo(norm); return; }
    if (!moving_) return;
    translateSelection(norm - lastNorm_);
    lastNorm_ = norm;
}

void SketchCanvas::selectRelease()
{
    if (marquee_) {
        // Finalize the rubber band: everything intersecting joins the set.
        // (A tiny no-drag marquee selects nothing — that's the deselect click.)
        marquee_ = false;
        bool changed = false;
        const double minSpan = hitTolNorm() * 0.5;
        if (marqueeRect_.width() > minSpan || marqueeRect_.height() > minSpan) {
            auto addHits = [&](SelKind k, int count) {
                for (int i = 0; i < count; ++i) {
                    if (selContains(k, i)) continue;
                    const QRectF b = itemBoundsNorm({k, i});
                    if (b.isValid() && b.intersects(marqueeRect_)) {
                        sel_.push_back({k, i});
                        changed = true;
                    }
                }
            };
            addHits(SelStroke, int(strokes_.size()));
            addHits(SelText,   int(texts_.size()));
            addHits(SelImage,  int(images_.size()));
        }
        if (changed) emit selectionChanged();
        update();
        return;
    }
    if (!moving_ && !resizing_) return;
    moving_ = false; resizing_ = false; grabCorner_ = -1;
    if (!moveDirty_) return;   // a click-to-select must not commit a no-op undo step
    moveDirty_ = false;
    // Commit the gesture. Strokes are one whole-blob edited(); images/texts
    // are per-index signals. When more than one signal fires (a group), the
    // bracket signals let QML fold the model calls into ONE undo step.
    bool anyStroke = false;
    std::vector<int> imgIdx, txtIdx;
    for (const SelItem &it : sel_) {
        if (it.kind == SelStroke) anyStroke = true;
        else if (it.kind == SelImage && it.idx < int(images_.size())) imgIdx.push_back(it.idx);
        else if (it.kind == SelText && it.idx < int(texts_.size()))  txtIdx.push_back(it.idx);
    }
    const int signalCount = (anyStroke ? 1 : 0) + int(imgIdx.size()) + int(txtIdx.size());
    if (signalCount > 1) emit groupCommitBegan();
    if (anyStroke)
        emit edited(qcv::AnnotationSerializer::strokesToJsonString(strokes_));
    for (int i : imgIdx) {
        const QRectF r = images_[size_t(i)].rect;
        emit imageRectChanged(i, r.x(), r.y(), r.width(), r.height());
    }
    for (int i : txtIdx) {
        const mn::SketchTextSpec &t = texts_[size_t(i)].spec;
        emit textBoxChanged(i, t.x, t.y, t.w, t.size);
    }
    if (signalCount > 1) emit groupCommitEnded();
}

void SketchCanvas::deleteSelection()
{
    if (sel_.empty()) return;
    // Partition, then delete: strokes fold into one edited(); images/texts
    // emit per-index removals in DESCENDING order so the model's shifting
    // arrays never invalidate a pending index.
    std::vector<int> strokeIdx, imgIdx, txtIdx;
    for (const SelItem &it : sel_) {
        if (it.kind == SelStroke && it.idx < int(strokes_.size())) strokeIdx.push_back(it.idx);
        else if (it.kind == SelImage && it.idx < int(images_.size())) imgIdx.push_back(it.idx);
        else if (it.kind == SelText && it.idx < int(texts_.size()))  txtIdx.push_back(it.idx);
    }
    std::sort(strokeIdx.rbegin(), strokeIdx.rend());
    std::sort(imgIdx.rbegin(), imgIdx.rend());
    std::sort(txtIdx.rbegin(), txtIdx.rend());
    const int signalCount = (strokeIdx.empty() ? 0 : 1) + int(imgIdx.size()) + int(txtIdx.size());
    if (signalCount == 0) return;
    clearSelection();
    if (signalCount > 1) emit groupCommitBegan();
    if (!strokeIdx.empty()) {
        for (int i : strokeIdx) strokes_.erase(strokes_.begin() + i);
        invalidateBounds();
        emit edited(qcv::AnnotationSerializer::strokesToJsonString(strokes_));
    }
    for (int i : imgIdx) emit imageRemoved(i);
    for (int i : txtIdx) emit textRemoved(i);
    if (signalCount > 1) emit groupCommitEnded();
}

QRectF SketchCanvas::itemDisplayRect(const SelItem &it) const
{
    const QRectF b = itemBoundsNorm(it);
    if (!b.isValid() || width() <= 0 || height() <= 0) return {};
    const QRectF f = frameScreenRect();
    QRectF r(f.x() + b.x() * f.width(), f.y() + b.y() * f.height(),
             b.width() * f.width(), b.height() * f.height());
    if (it.kind == SelStroke) r = r.adjusted(-3, -3, 3, 3);   // breathing room around ink
    return r;
}

QRectF SketchCanvas::selDisplayRect() const
{
    // Handles target the SINGLE selection; groups have per-item outlines only.
    return sel_.size() == 1 ? itemDisplayRect(sel_[0]) : QRectF();
}

int SketchCanvas::handleAtPx(QPointF px) const
{
    const QRectF r = selDisplayRect();
    if (!r.isValid()) return -1;
    const double tol = 9.0;
    const QPointF cs[4] = { r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight() };
    for (int i = 0; i < 4; ++i)
        if (QLineF(px, cs[i]).length() <= tol) return i;
    if (sel_.size() == 1 && sel_[0].kind == SelText) {   // wrap-width grips (corners win ties)
        const QPointF ms[2] = { QPointF(r.left(),  r.center().y()),
                                QPointF(r.right(), r.center().y()) };
        for (int i = 0; i < 2; ++i)
            if (QLineF(px, ms[i]).length() <= tol) return 4 + i;
    }
    return -1;
}

void SketchCanvas::beginResize(int corner)
{
    // Only reachable with a single selection (handleAtPx gates on it).
    if (sel_.size() != 1) return;
    const SelItem it = sel_[0];
    grabCorner_ = corner;
    origBounds_ = itemBoundsNorm(it);
    if (it.kind == SelStroke && it.idx >= 0 && it.idx < int(strokes_.size()))
        origPoints_ = strokes_[size_t(it.idx)].points;   // absolute base (no drift)
    if (it.kind == SelText && it.idx >= 0 && it.idx < int(texts_.size())) {
        origTextW_    = texts_[size_t(it.idx)].spec.w;
        origTextSize_ = texts_[size_t(it.idx)].spec.size;
    }
    resizing_ = true;
    moving_ = false;
    moveDirty_ = false;
}

void SketchCanvas::resizeTo(QPointF norm)
{
    if (!origBounds_.isValid() || sel_.size() != 1) return;
    const SelKind selKind = sel_[0].kind;
    const int selIdx = sel_[0].idx;
    // Wrap-width drag (SelText mid-edge grips): the opposite edge stays
    // fixed, the text reflows live and its height re-derives.
    if (grabCorner_ >= 4 && selKind == SelText
        && selIdx >= 0 && selIdx < int(texts_.size())) {
        SketchText &t = texts_[size_t(selIdx)];
        const double srcW = sourceWidth_  > 0 ? sourceWidth_  : 480.0;
        const double srcH = sourceHeight_ > 0 ? sourceHeight_ : srcW;
        const double minW = 2.0 * t.spec.size / srcW;   // 2em floor
        const double fixedX = grabCorner_ == 4 ? origBounds_.right()
                                               : origBounds_.left();
        const double newW = std::max(minW, grabCorner_ == 4 ? fixedX - norm.x()
                                                            : norm.x() - fixedX);
        t.spec.x = grabCorner_ == 4 ? fixedX - newW : fixedX;
        t.spec.w = newW;
        t.hNorm = mn::sketchTextHeightSrc(t.spec, srcW) / srcH;   // reflow
        moveDirty_ = true;
        invalidateBounds();
        update();
        return;
    }
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
    if (selKind == SelStroke && selIdx < int(strokes_.size())
        && origPoints_.size() == strokes_[size_t(selIdx)].points.size()) {
        qcv::ActiveStroke &st = strokes_[size_t(selIdx)];
        std::vector<QPointF> &pts = st.points;
        if (st.tool == qcv::DrawingTool::Oval && pts.size() >= 2) {
            pts[0] = pivot + (origPoints_[0] - pivot) * s;   // centre scales about pivot
            pts[1] = origPoints_[1] * s;                     // radii scale by s
        } else {
            for (size_t i = 0; i < pts.size(); ++i)
                pts[i] = pivot + (origPoints_[i] - pivot) * s;
        }
    } else if (selKind == SelText && selIdx < int(texts_.size())) {
        // Corner scale: w AND size scale together (w/size ratio constant ⇒
        // identical wrap points), position scales about the pivot.
        SketchText &t = texts_[size_t(selIdx)];
        t.spec.x = pivot.x() + (origBounds_.left() - pivot.x()) * s;
        t.spec.y = pivot.y() + (origBounds_.top()  - pivot.y()) * s;
        t.spec.w = origTextW_ * s;
        t.spec.size = origTextSize_ * s;
        const double srcW = sourceWidth_  > 0 ? sourceWidth_  : 480.0;
        const double srcH = sourceHeight_ > 0 ? sourceHeight_ : srcW;
        t.hNorm = mn::sketchTextHeightSrc(t.spec, srcW) / srcH;
        invalidateBounds();
    } else if (selKind == SelImage && selIdx < int(images_.size())) {
        const QPointF far = pivot + QPointF(dxs, dys) * s;
        images_[size_t(selIdx)].rect =
            QRectF(QPointF(std::min(pivot.x(), far.x()), std::min(pivot.y(), far.y())),
                   QPointF(std::max(pivot.x(), far.x()), std::max(pivot.y(), far.y())));
    }
    update();
}

QVariantMap SketchCanvas::textElementAt(int idx) const
{
    QVariantMap m;
    if (idx < 0 || idx >= int(texts_.size())) return m;
    const mn::SketchTextSpec &t = texts_[size_t(idx)].spec;
    m.insert(QStringLiteral("x"), t.x);
    m.insert(QStringLiteral("y"), t.y);
    m.insert(QStringLiteral("w"), t.w);
    m.insert(QStringLiteral("text"), t.text);
    m.insert(QStringLiteral("size"), t.size);
    m.insert(QStringLiteral("color"), t.color.name());
    return m;
}

void SketchCanvas::hoverMoveEvent(QHoverEvent *e)
{
    if (inSelectMode() && sel_.size() == 1 && !panMode_) {
        const int h = handleAtPx(e->position());
        if (h == 0 || h == 3)      setCursor(QCursor(Qt::SizeFDiagCursor));   // TL / BR
        else if (h == 1 || h == 2) setCursor(QCursor(Qt::SizeBDiagCursor));   // TR / BL
        else if (h >= 4)           setCursor(QCursor(Qt::SizeHorCursor));     // wrap grips
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
