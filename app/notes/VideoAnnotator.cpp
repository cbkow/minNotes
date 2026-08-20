#include "VideoAnnotator.h"

#include "annotation_serializer.h"
#include "annotation_thumbnail.h"

#include <QCursor>
#include <QHoverEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>

VideoAnnotator::VideoAnnotator(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    // Disarmed by default → select mode (no draw tool); accepts clicks to select.
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);   // resize-cursor feedback over handles

    annot_.setStrokeFinalizedCallback(
        [this](std::unique_ptr<qcv::ActiveStroke> s) { commitStroke(std::move(s)); });
    annot_.setEraseAtCallback([this](QPointF norm) { eraseAt(norm); });
    annot_.setStrokeStartedCallback([this] {
        thumbTimer_.stop();    // a pending thumb write can't fire mid-stroke
        emit strokeStarted();
    });

    thumbTimer_.setSingleShot(true);
    thumbTimer_.setInterval(400);   // QCView's burst debounce
    connect(&thumbTimer_, &QTimer::timeout, this, [this] {
        if (notes_ && !pendingThumbTc_.isEmpty())
            notes_->writeAnnotatedThumb(pendingThumbTc_);
    });
}

VideoAnnotator::~VideoAnnotator() = default;

void VideoAnnotator::setNotes(VideoNotesModel *notes)
{
    if (notes == notes_) return;
    if (notes_) disconnect(notes_, nullptr, this, nullptr);
    notes_ = notes;
    if (notes_) {
        // External edits (QCView via the watcher, undo, card deletes) all
        // funnel through the revision — re-pull this frame's strokes.
        connect(notes_, &VideoNotesModel::revisionChanged,
                this, [this] { refreshStrokes(); });
        // New media → stale stacks would restore another video's JSON.
        connect(notes_, &VideoNotesModel::mediaPathChanged, this, [this] {
            undo_.clear(); redo_.clear();
            emit undoStacksChanged();
            cancelStroke();
            refreshStrokes();
        });
    }
    emit notesChanged();
    refreshStrokes();
}

void VideoAnnotator::setFrame(int frame)
{
    if (frame == frame_) return;
    frame_ = frame;
    emit frameChanged();
    refreshStrokes();
}

void VideoAnnotator::setSourceWidth(int w)
{
    if (w == sourceWidth_) return;
    sourceWidth_ = w;
    emit sourceWidthChanged();
    update();
}

void VideoAnnotator::setTool(const QString &tool)
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

void VideoAnnotator::applyAcceptedButtons()
{
    setAcceptedMouseButtons(Qt::LeftButton);   // always (draw or select)
    if (armed())             setCursor(QCursor(Qt::CrossCursor));
    else                     setCursor(QCursor(Qt::ArrowCursor));
}

void VideoAnnotator::setColor(const QColor &c)
{
    if (c == annot_.drawingColor()) return;
    annot_.setDrawingColor(c);
    emit colorChanged();
}

void VideoAnnotator::setStrokeWidth(qreal w)
{
    if (qFuzzyCompare(float(w), annot_.strokeWidth())) return;
    annot_.setStrokeWidth(float(w));
    emit strokeWidthChanged();
}

void VideoAnnotator::paint(QPainter *p)
{
    if (width() <= 0 || height() <= 0) return;
    p->setRenderHint(QPainter::Antialiasing, true);
    // strokeWidth is in source pixels; the stage box is display-scaled.
    const double scale = sourceWidth_ > 0 ? width() / double(sourceWidth_) : 1.0;
    for (const qcv::ActiveStroke &s : strokes_)
        qcv::paintStroke(*p, s, width(), height(), scale);
    qcv::ActiveStroke live;
    if (annot_.snapshotActiveStroke(live))
        qcv::paintStroke(*p, live, width(), height(), scale);

    // Selection affordance (the SketchCanvas language): EVERY selected stroke
    // gets its dashed outline; corner resize handles appear only for a SINGLE
    // selection — groups move and delete as one, no group scale.
    if (!sel_.empty()) {
        const QColor accent(0x01, 0x89, 0xf1);
        QPen pen(accent, 1.5, Qt::DashLine); pen.setCosmetic(true);
        p->setBrush(Qt::NoBrush); p->setPen(pen);
        for (int idx : sel_) {
            const QRectF r = itemDisplayRect(idx);
            if (r.isValid()) p->drawRect(r);
        }
        const QRectF r = selDisplayRect();   // valid only for a single selection
        if (r.isValid()) {
            const double hs = 4.5;
            const QPointF cs[4] = { r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight() };
            p->setBrush(accent);
            p->setPen(QPen(QColor(255, 255, 255), 1.0));
            for (const QPointF &c : cs)
                p->drawRect(QRectF(c.x() - hs, c.y() - hs, hs * 2, hs * 2));
        }
    }

    // Live marquee: accent hairline + whisper fill (the drag-ghost language).
    if (marquee_ && marqueeRect_.isValid()) {
        const QRectF mr(marqueeRect_.x() * width(), marqueeRect_.y() * height(),
                        marqueeRect_.width() * width(), marqueeRect_.height() * height());
        const QColor accent(0x01, 0x89, 0xf1);
        p->fillRect(mr, QColor(accent.red(), accent.green(), accent.blue(), 20));
        QPen pen(accent, 1.0, Qt::DashLine); pen.setCosmetic(true);
        p->setBrush(Qt::NoBrush); p->setPen(pen);
        p->drawRect(mr);
    }
}

void VideoAnnotator::mousePressEvent(QMouseEvent *e)
{
    if (inSelectMode()) { selectPress(e->position(), e->modifiers()); e->accept(); return; }
    route(qcv::PointerPhase::Press, e->position(), qint64(e->timestamp()));
    e->accept();
}

void VideoAnnotator::mouseMoveEvent(QMouseEvent *e)
{
    if (inSelectMode()) { selectMove(e->position()); e->accept(); return; }
    route(qcv::PointerPhase::Move, e->position(), qint64(e->timestamp()));
    e->accept();
}

void VideoAnnotator::mouseReleaseEvent(QMouseEvent *e)
{
    if (inSelectMode()) { selectRelease(); e->accept(); return; }
    route(qcv::PointerPhase::Release, e->position(), qint64(e->timestamp()));
    e->accept();
}

void VideoAnnotator::geometryChange(const QRectF &newGeo, const QRectF &oldGeo)
{
    QQuickPaintedItem::geometryChange(newGeo, oldGeo);
    annot_.setViewportRect(QPointF(0, 0), newGeo.size());
}

void VideoAnnotator::route(qcv::PointerPhase phase, QPointF pos, qint64 tMs)
{
    annot_.setViewportRect(QPointF(0, 0), size());
    // Eraser gesture bracket. Begin BEFORE dispatching Press — the annotator
    // erases at the press point itself, so the prior must be captured first.
    if (phase == qcv::PointerPhase::Press
        && annot_.activeTool() == qcv::DrawingTool::Eraser)
        beginEraseGesture();
    annot_.onPointerEvent(phase, pos, tMs);
    if (phase == qcv::PointerPhase::Press)   setDrawing(true);
    if (phase == qcv::PointerPhase::Release) {
        flushEraseGesture();
        eraseGesture_ = false;
        setDrawing(false);
    }
    update();   // live stroke repaint
}

void VideoAnnotator::beginEraseGesture()
{
    if (!notes_) return;
    eraseGesture_ = true;
    eraseDirty_   = false;
    eraseTc_      = notes_->timecodeForFrame(frame_);
    eraseFrame_   = frame_;
    erasePrior_   = notes_->annotationDataFor(eraseTc_);
}

void VideoAnnotator::flushEraseGesture()
{
    if (!eraseGesture_ || !eraseDirty_) return;
    undo_.append({eraseTc_, eraseFrame_, erasePrior_, /*wasNew=*/false});
    redo_.clear();
    emit undoStacksChanged();
    eraseDirty_ = false;
}

void VideoAnnotator::cancelStroke()
{
    annot_.cancelActiveStroke();
    if (eraseGesture_) {   // Esc mid-erase: restore the pre-gesture ink (the
        if (eraseDirty_ && notes_) {   // hits were written to the notes model live)
            notes_->setAnnotationData(eraseTc_, eraseFrame_, erasePrior_);
            scheduleThumb(eraseTc_);
            refreshStrokes();
        }
        eraseGesture_ = false;
        eraseDirty_   = false;
    }
    setDrawing(false);
    update();
}

void VideoAnnotator::commitStroke(std::unique_ptr<qcv::ActiveStroke> stroke)
{
    if (!notes_ || !stroke) return;
    const QString tc = notes_->timecodeForFrame(frame_);
    const bool wasNew = !notes_->hasNote(tc);
    const QString prior = wasNew ? QString() : notes_->annotationDataFor(tc);
    if (wasNew) notes_->addNoteAtFrame(frame_);   // note + clean thumb baseline

    std::vector<qcv::ActiveStroke> strokes =
        qcv::AnnotationSerializer::jsonStringToStrokes(prior);
    strokes.push_back(*stroke);
    const QString json = qcv::AnnotationSerializer::strokesToJsonString(strokes);

    undo_.append({tc, frame_, prior, wasNew});
    redo_.clear();
    emit undoStacksChanged();

    notes_->setAnnotationData(tc, frame_, json);
    scheduleThumb(tc);
    refreshStrokes();
}

void VideoAnnotator::eraseAt(QPointF norm)
{
    if (!notes_) return;
    const QString tc = notes_->timecodeForFrame(frame_);
    const QString data = notes_->annotationDataFor(tc);
    if (data.isEmpty()) return;

    std::vector<qcv::ActiveStroke> strokes =
        qcv::AnnotationSerializer::jsonStringToStrokes(data);

    // QCView's hit-test verbatim: bounding box + normalized tolerance
    // (≈19 px on a 1920-wide source), last-drawn first, one per event.
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

    if (eraseGesture_) {
        // The eraser doesn't pause playback, so the playhead can cross onto a
        // different note mid-drag: close the pending entry and re-anchor the
        // prior on the new timecode. Otherwise just mark the gesture dirty —
        // the single undo entry is pushed on release (flushEraseGesture).
        if (tc != eraseTc_) {
            flushEraseGesture();
            eraseTc_ = tc; eraseFrame_ = frame_; erasePrior_ = data;
        }
        eraseDirty_ = true;
    } else {   // safety net — the annotator only erases inside a press-drag
        undo_.append({tc, frame_, data, false});
        redo_.clear();
        emit undoStacksChanged();
    }

    strokes.erase(strokes.begin() + hitIdx);
    const QString json = strokes.empty()
        ? QString()   // erased-to-zero = empty string → null on disk (QCView)
        : qcv::AnnotationSerializer::strokesToJsonString(strokes);
    notes_->setAnnotationData(tc, frame_, json);
    scheduleThumb(tc);
    refreshStrokes();
}

void VideoAnnotator::undo()
{
    if (!notes_ || undo_.isEmpty()) return;
    const UndoEntry e = undo_.takeLast();
    redo_.append({e.timecode, e.frame, notes_->annotationDataFor(e.timecode),
                  /*wasNew=*/false});
    if (e.wasNew) notes_->removeNoteKeepFiles(e.timecode);   // PNGs stay for redo
    else          notes_->setAnnotationData(e.timecode, e.frame, e.prior);
    emit undoStacksChanged();
    scheduleThumb(e.timecode);
    refreshStrokes();
}

void VideoAnnotator::redo()
{
    if (!notes_ || redo_.isEmpty()) return;
    const UndoEntry e = redo_.takeLast();
    undo_.append({e.timecode, e.frame, notes_->annotationDataFor(e.timecode),
                  /*wasNew=*/!notes_->hasNote(e.timecode)});
    notes_->setAnnotationData(e.timecode, e.frame, e.prior);
    emit undoStacksChanged();
    scheduleThumb(e.timecode);
    refreshStrokes();
}

void VideoAnnotator::refreshStrokes()
{
    strokes_.clear();
    if (notes_) {
        const QString data =
            notes_->annotationDataFor(notes_->timecodeForFrame(frame_));
        if (!data.isEmpty())
            strokes_ = qcv::AnnotationSerializer::jsonStringToStrokes(data);
    }
    pruneSelection();   // frame change / external edit
    update();
}

void VideoAnnotator::scheduleThumb(const QString &timecode)
{
    pendingThumbTc_ = timecode;
    thumbTimer_.start();
}

void VideoAnnotator::setDrawing(bool d)
{
    if (d == drawing_) return;
    drawing_ = d;
    emit drawingChanged();
}

// ---- Selection / move / resize (current frame's strokes) -------------------

void VideoAnnotator::clearSelection()
{
    moving_ = false; resizing_ = false; marquee_ = false; grabCorner_ = -1;
    if (sel_.empty()) return;
    sel_.clear();
    emit selectionChanged();
    update();
}

bool VideoAnnotator::selContains(int idx) const
{
    return std::find(sel_.begin(), sel_.end(), idx) != sel_.end();
}

void VideoAnnotator::toggleSel(int idx)
{
    const auto it = std::find(sel_.begin(), sel_.end(), idx);
    if (it != sel_.end()) sel_.erase(it);
    else                  sel_.push_back(idx);
    emit selectionChanged();
    update();
}

void VideoAnnotator::pruneSelection()
{
    const size_t before = sel_.size();
    sel_.erase(std::remove_if(sel_.begin(), sel_.end(), [this](int idx) {
        return idx < 0 || idx >= int(strokes_.size());
    }), sel_.end());
    if (sel_.size() != before) {
        if (sel_.empty()) { moving_ = false; resizing_ = false; }
        emit selectionChanged();
        update();
    }
}

QRectF VideoAnnotator::strokeBoundsNorm(int idx) const
{
    if (idx < 0 || idx >= int(strokes_.size())) return {};
    return qcv::strokeBoundsNorm(strokes_[size_t(idx)]);   // oval-aware (shared)
}

QRectF VideoAnnotator::selBoundsNorm() const
{
    QRectF u;
    for (int idx : sel_) {
        const QRectF b = strokeBoundsNorm(idx);
        if (!b.isValid()) continue;
        u = u.isValid() ? u.united(b) : b;
    }
    return u;
}

QRectF VideoAnnotator::itemDisplayRect(int idx) const
{
    const QRectF b = strokeBoundsNorm(idx);
    if (!b.isValid() || width() <= 0 || height() <= 0) return {};
    return QRectF(b.x() * width(), b.y() * height(),
                  b.width() * width(), b.height() * height()).adjusted(-3, -3, 3, 3);
}

QRectF VideoAnnotator::selDisplayRect() const
{
    // Handles target — SINGLE selection only (groups move/delete, no scale).
    return sel_.size() == 1 ? itemDisplayRect(sel_[0]) : QRectF();
}

int VideoAnnotator::handleAtPx(QPointF px) const
{
    const QRectF r = selDisplayRect();
    if (!r.isValid()) return -1;
    const double tol = 9.0;
    const QPointF cs[4] = { r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight() };
    for (int i = 0; i < 4; ++i)
        if (QLineF(px, cs[i]).length() <= tol) return i;
    return -1;
}

int VideoAnnotator::hitTest(QPointF norm) const
{
    const double tol = 0.012;
    for (int i = int(strokes_.size()) - 1; i >= 0; --i) {
        const QRectF b = strokeBoundsNorm(i);
        if (b.isNull()) continue;
        const double pad = std::max(tol, double(strokes_[size_t(i)].strokeWidth) / 1920.0);
        if (b.adjusted(-pad, -pad, pad, pad).contains(norm)) return i;
    }
    return -1;
}

void VideoAnnotator::selectPress(QPointF pos, Qt::KeyboardModifiers mods)
{
    if (width() <= 0 || height() <= 0) return;
    const bool shift = mods.testFlag(Qt::ShiftModifier);
    // A press on the single selection's corner handle starts a resize
    // (groups move/delete only — no group scale, the M1 rule).
    if (sel_.size() == 1 && !shift) {
        const int h = handleAtPx(pos);
        if (h >= 0) { beginResize(h); return; }
    }
    const QPointF norm(pos.x() / width(), pos.y() / height());
    const int idx = hitTest(norm);
    if (idx < 0) {
        // Empty stage: a plain press clears and starts a marquee; a shift
        // press keeps the set (additive marquee). A no-drag release makes
        // the tiny marquee select nothing, so click-to-deselect survives.
        // No playback pause here — a bare click must not stop the video.
        if (!shift) clearSelection();
        marquee_ = true;
        marqueeAnchor_ = norm;
        marqueeRect_ = QRectF(norm, norm);
        update();
        return;
    }
    if (shift) { toggleSel(idx); return; }   // membership toggle, no move
    if (!selContains(idx)) {
        // Not in the set → this press replaces the selection. In the set →
        // keep the group and drag it as one.
        sel_.clear();
        sel_.push_back(idx);
        emit selectionChanged();
    }
    moving_ = true; moveDirty_ = false; lastNorm_ = norm;
    emit strokeStarted();   // pause playback so the frame is stable during the drag
    update();
}

void VideoAnnotator::translateSelection(QPointF dNorm)
{
    if (sel_.empty()) return;
    // Clamp by the UNION bounds — the group moves as one and stops when its
    // edge hits the frame (QCView's frame is a hard canvas, unlike the
    // sketch's legal overflow).
    const QRectF b = selBoundsNorm();
    if (!b.isValid()) return;
    const double dx = std::clamp(dNorm.x(), -b.left(), 1.0 - b.right());
    const double dy = std::clamp(dNorm.y(), -b.top(),  1.0 - b.bottom());
    if (dx == 0.0 && dy == 0.0) return;
    for (int idx : sel_) {
        if (idx < 0 || idx >= int(strokes_.size())) continue;
        qcv::ActiveStroke &s = strokes_[size_t(idx)];
        if (s.tool == qcv::DrawingTool::Oval && !s.points.empty())
            s.points[0] += QPointF(dx, dy);   // oval = {center, radii}: move centre only
        else
            for (QPointF &pt : s.points) pt += QPointF(dx, dy);
    }
    moveDirty_ = true;
    update();
}

void VideoAnnotator::beginResize(int corner)
{
    if (sel_.size() != 1) return;   // handles only exist for a single selection
    grabCorner_ = corner;
    origBounds_ = selBoundsNorm();
    if (sel_[0] >= 0 && sel_[0] < int(strokes_.size()))
        origPoints_ = strokes_[size_t(sel_[0])].points;
    resizing_ = true; moving_ = false; moveDirty_ = false;
    emit strokeStarted();   // pause playback during the resize drag
}

void VideoAnnotator::resizeTo(QPointF norm)
{
    if (!origBounds_.isValid()) return;
    const double L = origBounds_.left(), R = origBounds_.right();
    const double T = origBounds_.top(),  B = origBounds_.bottom();
    QPointF grab, pivot;
    switch (grabCorner_) {
        case 0: grab = {L, T}; pivot = {R, B}; break;
        case 1: grab = {R, T}; pivot = {L, B}; break;
        case 2: grab = {L, B}; pivot = {R, T}; break;
        default: grab = {R, B}; pivot = {L, T}; break;
    }
    const double dxs = grab.x() - pivot.x();
    const double dys = grab.y() - pivot.y();
    const double origW = std::abs(dxs), origH = std::abs(dys);
    if (origW <= 0 || origH <= 0) return;
    double s = std::max(std::abs(norm.x() - pivot.x()) / origW,
                        std::abs(norm.y() - pivot.y()) / origH);
    s = std::max(s, std::max(0.03 / origW, 0.03 / origH));
    const double sMaxX = dxs > 0 ? (1.0 - pivot.x()) / dxs : (dxs < 0 ? -pivot.x() / dxs : 1e9);
    const double sMaxY = dys > 0 ? (1.0 - pivot.y()) / dys : (dys < 0 ? -pivot.y() / dys : 1e9);
    s = std::min(s, std::min(sMaxX, sMaxY));
    if (s <= 0) return;
    if (sel_.size() != 1 || sel_[0] < 0 || sel_[0] >= int(strokes_.size())) return;
    if (origPoints_.size() != strokes_[size_t(sel_[0])].points.size()) return;
    qcv::ActiveStroke &st = strokes_[size_t(sel_[0])];
    std::vector<QPointF> &pts = st.points;
    if (st.tool == qcv::DrawingTool::Oval && pts.size() >= 2) {
        pts[0] = pivot + (origPoints_[0] - pivot) * s;   // centre scales about pivot
        pts[1] = origPoints_[1] * s;                     // radii scale by s
    } else {
        for (size_t i = 0; i < pts.size(); ++i) pts[i] = pivot + (origPoints_[i] - pivot) * s;
    }
    moveDirty_ = true;
    update();
}

void VideoAnnotator::selectMove(QPointF pos)
{
    if (width() <= 0 || height() <= 0) return;
    const QPointF norm(pos.x() / width(), pos.y() / height());
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

void VideoAnnotator::selectRelease()
{
    if (marquee_) {
        // Finalize the rubber band: every intersecting stroke joins the set.
        // (A tiny no-drag marquee selects nothing — that's the deselect click.)
        marquee_ = false;
        bool changed = false;
        const double minSpan = 0.006;   // half the hit tolerance
        if (marqueeRect_.width() > minSpan || marqueeRect_.height() > minSpan) {
            for (int i = 0; i < int(strokes_.size()); ++i) {
                if (selContains(i)) continue;
                const QRectF b = strokeBoundsNorm(i);
                if (b.isValid() && b.intersects(marqueeRect_)) {
                    sel_.push_back(i);
                    changed = true;
                }
            }
        }
        if (changed) emit selectionChanged();
        update();
        return;
    }
    if (!moving_ && !resizing_) return;
    moving_ = false; resizing_ = false; grabCorner_ = -1;
    if (!moveDirty_) return;
    moveDirty_ = false;
    commitStrokes();
}

void VideoAnnotator::deleteSelection()
{
    if (sel_.empty()) return;
    // Descending erase so the shifting vector never invalidates a pending
    // index; the whole-frame commit makes the group delete ONE undo entry.
    std::vector<int> idx(sel_.begin(), sel_.end());
    std::sort(idx.rbegin(), idx.rend());
    bool any = false;
    for (int i : idx) {
        if (i < 0 || i >= int(strokes_.size())) continue;
        strokes_.erase(strokes_.begin() + i);
        any = true;
    }
    clearSelection();
    if (any) commitStrokes();
}

void VideoAnnotator::commitStrokes()
{
    if (!notes_) return;
    const QString tc = notes_->timecodeForFrame(frame_);
    const QString prior = notes_->annotationDataFor(tc);   // state before this edit
    undo_.append({tc, frame_, prior, /*wasNew=*/false});
    redo_.clear();
    emit undoStacksChanged();
    const QString json = strokes_.empty()
        ? QString()
        : qcv::AnnotationSerializer::strokesToJsonString(strokes_);
    notes_->setAnnotationData(tc, frame_, json);
    scheduleThumb(tc);
    refreshStrokes();
}

void VideoAnnotator::hoverMoveEvent(QHoverEvent *e)
{
    if (inSelectMode() && sel_.size() == 1) {   // handles exist only for singles
        const int h = handleAtPx(e->position());
        if (h == 0 || h == 3)      setCursor(QCursor(Qt::SizeFDiagCursor));
        else if (h == 1 || h == 2) setCursor(QCursor(Qt::SizeBDiagCursor));
        else                       setCursor(QCursor(Qt::ArrowCursor));
    }
    QQuickPaintedItem::hoverMoveEvent(e);
}
