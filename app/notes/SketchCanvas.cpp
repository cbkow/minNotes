#include "SketchCanvas.h"

#include "annotation_serializer.h"
#include "annotation_thumbnail.h"

#include <QCursor>
#include <QMouseEvent>
#include <QPainter>

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
    emit dataChanged();
    update();
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
    const double scale = sourceWidth_ > 0 ? width() / double(sourceWidth_) : 1.0;
    for (const qcv::ActiveStroke &s : strokes_)
        qcv::paintStroke(*p, s, width(), height(), scale);
    qcv::ActiveStroke live;
    if (annot_.snapshotActiveStroke(live))
        qcv::paintStroke(*p, live, width(), height(), scale);
}

void SketchCanvas::mousePressEvent(QMouseEvent *e)
{
    route(qcv::PointerPhase::Press, e->position(), qint64(e->timestamp()));
    e->accept();
}

void SketchCanvas::mouseMoveEvent(QMouseEvent *e)
{
    route(qcv::PointerPhase::Move, e->position(), qint64(e->timestamp()));
    e->accept();
}

void SketchCanvas::mouseReleaseEvent(QMouseEvent *e)
{
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
