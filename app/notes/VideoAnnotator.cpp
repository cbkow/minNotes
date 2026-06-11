#include "VideoAnnotator.h"

#include "annotation_serializer.h"
#include "annotation_thumbnail.h"

#include <QCursor>
#include <QMouseEvent>
#include <QPainter>

VideoAnnotator::VideoAnnotator(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    // Disarmed by default — clicks fall through to the stage beneath.
    setAcceptedMouseButtons(Qt::NoButton);

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

    annot_.setActiveTool(t);
    annot_.setMode(t == qcv::DrawingTool::None ? qcv::ViewportMode::Playback
                                               : qcv::ViewportMode::Annotation);
    setAcceptedMouseButtons(t == qcv::DrawingTool::None ? Qt::NoButton
                                                        : Qt::LeftButton);
    setCursor(t == qcv::DrawingTool::None ? QCursor()
                                          : QCursor(Qt::CrossCursor));
    setDrawing(false);
    emit toolChanged();
    update();
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
}

void VideoAnnotator::mousePressEvent(QMouseEvent *e)
{
    route(qcv::PointerPhase::Press, e->position(), qint64(e->timestamp()));
    e->accept();
}

void VideoAnnotator::mouseMoveEvent(QMouseEvent *e)
{
    route(qcv::PointerPhase::Move, e->position(), qint64(e->timestamp()));
    e->accept();
}

void VideoAnnotator::mouseReleaseEvent(QMouseEvent *e)
{
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
    annot_.onPointerEvent(phase, pos, tMs);
    if (phase == qcv::PointerPhase::Press)   setDrawing(true);
    if (phase == qcv::PointerPhase::Release) setDrawing(false);
    update();   // live stroke repaint
}

void VideoAnnotator::cancelStroke()
{
    annot_.cancelActiveStroke();
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

    undo_.append({tc, frame_, data, false});
    redo_.clear();
    emit undoStacksChanged();

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
