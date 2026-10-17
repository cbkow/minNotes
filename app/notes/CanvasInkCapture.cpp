#include "CanvasInkCapture.h"
#include "annotation_serializer.h"
#include "active_stroke.h"
#if MN_CANVAS_STROKE_MODELER
#include "ink_stroke_modeler_wrapper.h"
#endif
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
qcv::DrawingTool toolFor(const QString& t) {
    if (t == QLatin1String("freehand")) return qcv::DrawingTool::Freehand;
    if (t == QLatin1String("rect")) return qcv::DrawingTool::Rectangle;
    if (t == QLatin1String("oval")) return qcv::DrawingTool::Oval;
    if (t == QLatin1String("arrow")) return qcv::DrawingTool::Arrow;
    if (t == QLatin1String("line")) return qcv::DrawingTool::Line;
    return qcv::DrawingTool::None;
}
}

CanvasInkCapture::CanvasInkCapture(QQuickItem* parent) : QQuickItem(parent) {
#if MN_CANVAS_STROKE_MODELER
    modeler_ = std::make_unique<qcv::InkStrokeModelerWrapper>();
#endif
}

CanvasInkCapture::~CanvasInkCapture() = default;

void CanvasInkCapture::setTool(const QString& t) { if (t == tool_) return; cancelStroke(); tool_ = t; emit toolChanged(); }
void CanvasInkCapture::setColor(const QColor& c) { if (c == color_) return; color_ = c; emit colorChanged(); }
void CanvasInkCapture::setStrokeWidth(qreal w) { if (w == width_) return; width_ = w; emit strokeWidthChanged(); }
void CanvasInkCapture::setZoom(qreal z) { if (z == zoom_ || z <= 0) return; zoom_ = z; emit cameraChanged(); }
void CanvasInkCapture::setPanX(qreal v) { if (v == panX_) return; panX_ = v; emit cameraChanged(); }
void CanvasInkCapture::setPanY(qreal v) { if (v == panY_) return; panY_ = v; emit cameraChanged(); }
void CanvasInkCapture::setActive(bool a) {
    if (a == active_) return;
    active_ = a;
    setAcceptedMouseButtons(a ? Qt::LeftButton : Qt::NoButton);
    if (!a) cancelStroke();
    emit activeChanged();
}

bool CanvasInkCapture::isStrokeTool() const {
    return tool_ == QLatin1String("freehand") || tool_ == QLatin1String("rect") || tool_ == QLatin1String("oval")
        || tool_ == QLatin1String("arrow") || tool_ == QLatin1String("line");
}

void CanvasInkCapture::mousePressEvent(QMouseEvent* e) {
    if (!active_ || e->button() != Qt::LeftButton) { e->ignore(); return; }
    beginStroke(e->position().x(), e->position().y());
    e->accept();
}
void CanvasInkCapture::mouseMoveEvent(QMouseEvent* e) { if (drawing_) movePoint(e->position().x(), e->position().y()); e->accept(); }
void CanvasInkCapture::mouseReleaseEvent(QMouseEvent* e) { if (drawing_) endStroke(e->position().x(), e->position().y()); e->accept(); }

void CanvasInkCapture::beginStroke(qreal x, qreal y) {
    if (tool_ == QLatin1String("eraser")) {
        const QPointF w = toWorld(x, y);
        emit eraseAt(w.x(), w.y());
        return;
    }
    if (!isStrokeTool()) return;
    press_ = current_ = QPointF(x, y);
    freehand_.clear();
    modeled_ = false;
    clock_.start();
    if (tool_ == QLatin1String("freehand")) {
#if MN_CANVAS_STROKE_MODELER
        if (modeler_ && width() > 0 && height() > 0) {
            qcv::InkStrokeModelerWrapper::Config cfg;
            modeler_->BeginStroke(cfg);
            const QPointF norm(x / width(), y / height());
            for (const QPointF& p : modeler_->AddPoint(norm, 0.0, true))
                freehand_.push_back(toWorld(p.x() * width(), p.y() * height()));
            modeled_ = true;
        }
#endif
        if (!modeled_) freehand_.push_back(toWorld(x, y));
    }
    drawing_ = true;
    emit drawingChanged();
    rebuildLive();
}

void CanvasInkCapture::movePoint(qreal x, qreal y) {
    if (!drawing_) return;
    current_ = QPointF(x, y);
    if (tool_ == QLatin1String("freehand")) {
        const double t = clock_.elapsed() / 1000.0;
#if MN_CANVAS_STROKE_MODELER
        if (modeled_ && modeler_) {
            const QPointF norm(x / width(), y / height());
            for (const QPointF& p : modeler_->AddPoint(norm, t, false))
                freehand_.push_back(toWorld(p.x() * width(), p.y() * height()));
        } else
#endif
        {
            Q_UNUSED(t);
            const QPointF w = toWorld(x, y);
            if (freehand_.empty() || freehand_.back() != w) freehand_.push_back(w);
        }
    }
    rebuildLive();
}

void CanvasInkCapture::endStroke(qreal x, qreal y) {
    if (!drawing_) return;
    current_ = QPointF(x, y);
    if (tool_ == QLatin1String("freehand")) {
#if MN_CANVAS_STROKE_MODELER
        if (modeled_ && modeler_) {
            const double t = clock_.elapsed() / 1000.0;
            const QPointF norm(x / width(), y / height());
            for (const QPointF& p : modeler_->EndStroke(norm, t))
                freehand_.push_back(toWorld(p.x() * width(), p.y() * height()));
        } else
#endif
        {
            const QPointF w = toWorld(x, y);
            if (freehand_.empty() || freehand_.back() != w) freehand_.push_back(w);
        }
    }
    drawing_ = false;
    emit drawingChanged();

    // The stroke in world px → its bbox (plus half the pen) → element-local points.
    const std::vector<QPointF> pts = shapePoints();
    std::vector<QPointF> extent = pts;
    if (tool_ == QLatin1String("oval") && pts.size() == 2)   // centre + radii → the bbox corners
        extent = { pts[0] - pts[1], pts[0] + pts[1] };
    if (extent.empty()) { live_.clear(); emit liveChanged(); return; }
    double x0 = std::numeric_limits<double>::max(), y0 = x0, x1 = -x0, y1 = -x0;
    for (const QPointF& p : extent) { x0 = std::min(x0, p.x()); y0 = std::min(y0, p.y()); x1 = std::max(x1, p.x()); y1 = std::max(y1, p.y()); }
    const double half = width_ / 2.0 + 1.0;
    const QRectF rect(QPointF(x0 - half, y0 - half), QPointF(x1 + half, y1 + half));
    qcv::ActiveStroke s;
    s.tool = toolFor(tool_);
    s.color = color_;
    s.strokeWidth = float(width_);
    s.isModeled = modeled_;
    s.isComplete = true;
    for (const QPointF& p : pts) {
        // the oval's radii are a size, not a position: only the centre is rebased
        const bool radii = tool_ == QLatin1String("oval") && &p == &pts[1];
        s.points.push_back(radii ? p : p - rect.topLeft());
        s.timestamps.push_back(0);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), QStringLiteral("2.0"));
    root.insert(QStringLiteral("coordinate_system"), QStringLiteral("element-local"));
    root.insert(QStringLiteral("shapes"), QJsonArray{ qcv::AnnotationSerializer::strokeToJson(s) });
    live_.clear();
    emit liveChanged();
    emit strokeCommitted(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)), rect);
}

void CanvasInkCapture::cancelStroke() {
    if (!drawing_) return;
    drawing_ = false;
    freehand_.clear();
    live_.clear();
    emit drawingChanged();
    emit liveChanged();
}

std::vector<QPointF> CanvasInkCapture::shapePoints() const {
    const QPointF a = toWorld(press_.x(), press_.y()), b = toWorld(current_.x(), current_.y());
    if (tool_ == QLatin1String("freehand")) return freehand_;
    if (tool_ == QLatin1String("rect")) return { QPointF(a.x(), a.y()), QPointF(b.x(), a.y()), QPointF(b.x(), b.y()), QPointF(a.x(), b.y()) };
    if (tool_ == QLatin1String("oval")) { const QPointF c = (a + b) / 2.0; return { c, QPointF(std::abs(b.x() - a.x()) / 2.0, std::abs(b.y() - a.y()) / 2.0) }; }
    return { a, b };   // line / arrow
}

// The preview: the current stroke as a normalized envelope over this item (screen px / size), the
// pen in screen px — what the committed element will look like at this zoom.
void CanvasInkCapture::rebuildLive() {
    if (!drawing_ || width() <= 0 || height() <= 0) { if (!live_.isEmpty()) { live_.clear(); emit liveChanged(); } return; }
    const std::vector<QPointF> pts = shapePoints();
    qcv::ActiveStroke s;
    s.tool = toolFor(tool_);
    s.color = color_;
    s.strokeWidth = float(width_ * zoom_);
    for (size_t i = 0; i < pts.size(); ++i) {
        const QPointF& p = pts[i];
        if (tool_ == QLatin1String("oval") && i == 1) { s.points.push_back(QPointF(p.x() * zoom_ / width(), p.y() * zoom_ / height())); continue; }
        const QPointF sc(p.x() * zoom_ + panX_, p.y() * zoom_ + panY_);
        s.points.push_back(QPointF(sc.x() / width(), sc.y() / height()));
    }
    live_ = qcv::AnnotationSerializer::strokesToJsonString({ s });
    emit liveChanged();
}
