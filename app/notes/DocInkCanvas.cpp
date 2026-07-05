#include "DocInkCanvas.h"
#include "annotation_thumbnail.h"   // qcv::paintStroke
#include "../core/BlockModel.h"

#include <QCursor>
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>
#include <climits>

namespace {
// Eraser/selection hit tolerance in page px (the qcv normalized 0.012 ≈ 23px
// on a 1920 frame; page-space picks a similar feel).
constexpr double kHitTolPx = 12.0;
// MediaBlock renders its frame at y: 6 inside the cell (12px total v-pad).
constexpr double kMediaTopPad = 6.0;
} // namespace

DocInkCanvas::DocInkCanvas(QQuickItem* parent)
    : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    setAcceptedMouseButtons(Qt::NoButton);   // disarmed → events pass through

    annot_.setStrokeFinalizedCallback(
        [this](std::unique_ptr<qcv::ActiveStroke> s) { commitStroke(std::move(s)); });
    annot_.setEraseAtCallback([this](QPointF norm) { eraseAt(norm); });
}

DocInkCanvas::~DocInkCanvas() = default;

QObject* DocInkCanvas::modelObject() const { return model_; }

void DocInkCanvas::setModelObject(QObject* m)
{
    auto* bm = qobject_cast<BlockModel*>(m);
    if (bm == model_) return;
    if (model_) model_->disconnect(this);
    model_ = bm;
    if (model_) {
        connect(model_, &BlockModel::inkChanged, this, [this] { rebuildCache(); });
        connect(model_, &BlockModel::documentChanged, this, [this] { rebuildCache(); });
        // Geometry-only shifts (heights settle, rows move) just repaint.
        connect(model_, &BlockModel::layoutChangedSpike, this, [this] { update(); });
        connect(model_, &BlockModel::contentChangedSpike, this, [this] { update(); });
    }
    rebuildCache();
    emit modelChanged();
}

void DocInkCanvas::setContentX(qreal v)
{
    if (qFuzzyCompare(contentX_, v)) return;
    contentX_ = v; emit transformChanged(); update();
}
void DocInkCanvas::setContentY(qreal v)
{
    if (qFuzzyCompare(contentY_, v)) return;
    contentY_ = v; emit transformChanged(); update();
}
void DocInkCanvas::setLeftEdgeContent(qreal v)
{
    if (qFuzzyCompare(leftEdgeContent_, v)) return;
    leftEdgeContent_ = v; emit transformChanged(); update();
}
void DocInkCanvas::setPageWidth(qreal v)
{
    if (qFuzzyCompare(pageWidth_, v)) return;
    pageWidth_ = v; emit transformChanged(); update();
}

void DocInkCanvas::setTool(const QString& tool)
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
    annot_.cancelActiveStroke();
    annot_.setActiveTool(t);
    annot_.setMode(drawToolActive_ ? qcv::ViewportMode::Annotation
                                   : qcv::ViewportMode::Playback);
    setDrawing(false);
    applyAcceptedButtons();
    emit toolChanged();
    update();
}

void DocInkCanvas::setColor(const QColor& c)
{
    if (c == annot_.drawingColor()) return;
    annot_.setDrawingColor(c);
    emit colorChanged();
}

void DocInkCanvas::setStrokeWidth(qreal w)
{
    if (qFuzzyCompare(qreal(annot_.strokeWidth()), w)) return;
    annot_.setStrokeWidth(float(w));
    emit strokeWidthChanged();
}

void DocInkCanvas::setInkMode(bool on)
{
    if (on == inkMode_) return;
    inkMode_ = on;
    if (!on) {
        annot_.cancelActiveStroke();
        eraseGesture_ = false; eraseDirty_.clear();
        setDrawing(false);
        clearSelection();
        rebuildCache();          // drop any uncommitted erase-gesture edits
    }
    applyAcceptedButtons();
    emit inkModeChanged();
}

void DocInkCanvas::applyAcceptedButtons()
{
    // Armed (ink mode) → own left-button gestures; otherwise refuse mouse so
    // everything falls through to the editor's central mouse layer.
    setAcceptedMouseButtons(inkMode_ ? Qt::LeftButton : Qt::NoButton);
    // Mode cursor: crosshair while a draw/erase tool is armed, the plain
    // arrow in select mode, and no override at all outside ink mode.
    if (inkMode_ && drawToolActive_) setCursor(Qt::CrossCursor);
    else if (inkMode_)               setCursor(Qt::ArrowCursor);
    else                             unsetCursor();
}

void DocInkCanvas::rebuildCache()
{
    cache_.clear();
    int n = 0;
    if (model_) {
        const QStringList ids = model_->inkBlockIds();
        for (const QString& id : ids) {
            mn::DocInkAnchor a;
            if (mn::docInkFromJson(model_->inkForBlock(id), a) && !a.strokes.empty()) {
                n += int(a.strokes.size());
                cache_.insert(id, std::move(a));
            }
        }
    }
    if (selIdx_ >= 0) {
        auto it = cache_.constFind(selBlockId_);
        if (it == cache_.constEnd() || selIdx_ >= int(it->strokes.size()))
            clearSelection();
    }
    if (n != strokeCount_) { strokeCount_ = n; emit strokeCountChanged(); }
    update();
}

DocInkCanvas::Placement DocInkCanvas::placementFor(const QString& blockId,
                                                   mn::DocInkAnchor::Space space) const
{
    Placement pl;
    if (!model_) return pl;
    const int row = model_->rowForId(blockId);
    if (row < 0) return pl;
    pl.row = row;
    pl.space = space;
    const qreal top = model_->yForRow(row);
    if (space == mn::DocInkAnchor::Frame) {
        const qreal fw = std::max<qreal>(1.0, model_->mediaDispWidth(row));
        const qreal fh = std::max<qreal>(1.0, model_->mediaDisplayHeight(row));
        pl.origin = QPointF(leftEdgeContent_, top + kMediaTopPad);
        pl.scale = QSizeF(fw, fh);
        pl.widthScale = fw / std::max(1, model_->mediaW(row));
    } else {
        pl.origin = QPointF(leftEdgeContent_ + pageWidth_ / 2.0, top);
        pl.scale = QSizeF(1, 1);
        pl.widthScale = 1.0;
    }
    pl.valid = true;
    return pl;
}

QPointF DocInkCanvas::localToContent(const Placement& pl, QPointF local) const
{
    return QPointF(pl.origin.x() + local.x() * pl.scale.width(),
                   pl.origin.y() + local.y() * pl.scale.height());
}

QPointF DocInkCanvas::contentToLocal(const Placement& pl, QPointF content) const
{
    return QPointF((content.x() - pl.origin.x()) / pl.scale.width(),
                   (content.y() - pl.origin.y()) / pl.scale.height());
}

void DocInkCanvas::paint(QPainter* p)
{
    if (!model_) return;
    const QRectF viewport(0, 0, width(), height());

    for (auto it = cache_.constBegin(); it != cache_.constEnd(); ++it) {
        const Placement pl = placementFor(it.key(), it->space);
        if (!pl.valid) continue;
        for (int i = 0; i < int(it->strokes.size()); ++i) {
            const qcv::ActiveStroke& src = it->strokes[size_t(i)];
            if (src.points.empty()) continue;
            // Transform into ITEM px (paintStroke w=h=1 passes points through).
            qcv::ActiveStroke s = src;
            qreal minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
            for (QPointF& pt : s.points) {
                const QPointF c = localToContent(pl, pt);
                pt = QPointF(c.x() - contentX_, c.y() - contentY_);
                minX = std::min(minX, pt.x()); maxX = std::max(maxX, pt.x());
                minY = std::min(minY, pt.y()); maxY = std::max(maxY, pt.y());
            }
            const double penW = std::max(1.0, double(s.strokeWidth) * pl.widthScale);
            const QRectF bbox(QPointF(minX - penW, minY - penW),
                              QPointF(maxX + penW, maxY + penW));
            if (!bbox.intersects(viewport)) continue;
            qcv::paintStroke(*p, s, 1.0, 1.0, pl.widthScale);

            if (it.key() == selBlockId_ && i == selIdx_) {   // selection outline
                QPen pen(QColor(0x01, 0x89, 0xf1));          // Theme accent
                pen.setStyle(Qt::DashLine);
                pen.setWidthF(1.0);
                p->setPen(pen);
                p->setBrush(Qt::NoBrush);
                p->drawRect(bbox.adjusted(2, 2, -2, -2));
            }
        }
    }

    // Live (in-flight) stroke — normalized item coords, exact sketch formula.
    qcv::ActiveStroke live;
    if (annot_.snapshotActiveStroke(live) && !live.points.empty())
        qcv::paintStroke(*p, live, width(), height(), 1.0);
}

// ---- Capture -----------------------------------------------------------

void DocInkCanvas::mousePressEvent(QMouseEvent* e)
{
    if (inSelectMode()) { selectPress(e->position()); e->accept(); return; }
    route(qcv::PointerPhase::Press, e->position(), qint64(e->timestamp()));
    e->accept();
}

void DocInkCanvas::mouseMoveEvent(QMouseEvent* e)
{
    if (inSelectMode()) { selectMove(e->position()); e->accept(); return; }
    route(qcv::PointerPhase::Move, e->position(), qint64(e->timestamp()));
    e->accept();
}

void DocInkCanvas::mouseReleaseEvent(QMouseEvent* e)
{
    if (inSelectMode()) { selectRelease(); e->accept(); return; }
    route(qcv::PointerPhase::Release, e->position(), qint64(e->timestamp()));
    e->accept();
}

void DocInkCanvas::geometryChange(const QRectF& newGeo, const QRectF& oldGeo)
{
    QQuickPaintedItem::geometryChange(newGeo, oldGeo);
    annot_.setViewportRect(QPointF(0, 0), newGeo.size());
}

void DocInkCanvas::route(qcv::PointerPhase phase, QPointF pos, qint64 tMs)
{
    annot_.setViewportRect(QPointF(0, 0), size());
    if (phase == qcv::PointerPhase::Press) {
        // Freeze the content offsets: a wheel-scroll mid-stroke must not skew
        // where the finished stroke anchors.
        pressContentX_ = contentX_;
        pressContentY_ = contentY_;
        if (annot_.activeTool() == qcv::DrawingTool::Eraser) {
            eraseGesture_ = true;
            eraseDirty_.clear();
        }
    }
    annot_.onPointerEvent(phase, pos, tMs);
    if (phase == qcv::PointerPhase::Press)   setDrawing(true);
    if (phase == qcv::PointerPhase::Release) {
        flushEraseGesture();
        eraseGesture_ = false;
        setDrawing(false);
    }
    update();
}

void DocInkCanvas::setDrawing(bool d)
{
    if (d == drawing_) return;
    drawing_ = d;
    emit drawingChanged();
}

void DocInkCanvas::cancelStroke()
{
    annot_.cancelActiveStroke();
    if (eraseGesture_) {          // Esc mid-erase: nothing reached the model —
        eraseGesture_ = false;    // re-parsing restores the erased strokes.
        eraseDirty_.clear();
        rebuildCache();
    }
    setDrawing(false);
    update();
}

void DocInkCanvas::commitStroke(std::unique_ptr<qcv::ActiveStroke> stroke)
{
    if (!model_ || !stroke || stroke->points.empty()) return;

    // Normalized item coords → content coords (frozen offsets).
    std::vector<QPointF> content;
    content.reserve(stroke->points.size());
    qreal minCy = 1e18;
    for (const QPointF& np : stroke->points) {
        const QPointF c(np.x() * width() + pressContentX_,
                        np.y() * height() + pressContentY_);
        minCy = std::min(minCy, c.y());
        content.push_back(c);
    }

    // Topmost overlapped block = the anchor. Clamp covers strokes drawn
    // above the first / below the last block.
    const qreal total = std::max<qreal>(1.0, model_->totalHeight());
    const int row = model_->rowForY(std::clamp(minCy, qreal(0), total - 1));
    if (row < 0 || row >= model_->rowCountQml()) return;

    const bool media = model_->typeForRow(row) == BlockModel::Media;
    const auto space = media ? mn::DocInkAnchor::Frame : mn::DocInkAnchor::Px;
    const Placement pl = placementFor(model_->idForRow(row), space);
    if (!pl.valid) return;

    qcv::ActiveStroke s = *stroke;
    for (size_t i = 0; i < content.size(); ++i)
        s.points[i] = contentToLocal(pl, content[i]);
    if (space == mn::DocInkAnchor::Frame)   // width stored in media-intrinsic px
        s.strokeWidth = float(double(s.strokeWidth) / pl.widthScale);

    mn::DocInkAnchor a;
    mn::docInkFromJson(model_->inkForRow(row), a);
    a.space = space;   // anchor type determines the space, always
    a.strokes.push_back(std::move(s));
    model_->setBlockInk(row, mn::docInkToJson(a));   // one undo step
}

// ---- Eraser --------------------------------------------------------------

bool DocInkCanvas::hitTest(QPointF contentPt, QString& blockIdOut, int& idxOut) const
{
    for (auto it = cache_.constBegin(); it != cache_.constEnd(); ++it) {
        const Placement pl = placementFor(it.key(), it->space);
        if (!pl.valid) continue;
        const QPointF local = contentToLocal(pl, contentPt);
        // Tolerance expressed in local units (px anchors: page px; frame
        // anchors: fractions of the frame).
        const double tolX = kHitTolPx / pl.scale.width();
        const double tolY = kHitTolPx / pl.scale.height();
        for (int i = int(it->strokes.size()) - 1; i >= 0; --i) {   // last-drawn first
            const qcv::ActiveStroke& s = it->strokes[size_t(i)];
            if (s.points.empty()) continue;
            double minX = s.points.front().x(), maxX = minX;
            double minY = s.points.front().y(), maxY = minY;
            for (const QPointF& p : s.points) {
                minX = std::min(minX, p.x()); maxX = std::max(maxX, p.x());
                minY = std::min(minY, p.y()); maxY = std::max(maxY, p.y());
            }
            const double padX = tolX + double(s.strokeWidth) / (2.0 * pl.scale.width());
            const double padY = tolY + double(s.strokeWidth) / (2.0 * pl.scale.height());
            if (local.x() >= minX - padX && local.x() <= maxX + padX
                && local.y() >= minY - padY && local.y() <= maxY + padY) {
                blockIdOut = it.key();
                idxOut = i;
                return true;
            }
        }
    }
    return false;
}

void DocInkCanvas::eraseAt(QPointF norm)
{
    if (!model_) return;
    const QPointF contentPt(norm.x() * width() + pressContentX_,
                            norm.y() * height() + pressContentY_);
    QString id; int idx = -1;
    if (!hitTest(contentPt, id, idx)) return;
    auto it = cache_.find(id);
    if (it == cache_.end()) return;
    it->strokes.erase(it->strokes.begin() + idx);   // live feedback; commit on release
    eraseDirty_.insert(id);
    if (id == selBlockId_) clearSelection();
    if (strokeCount_ > 0) { --strokeCount_; emit strokeCountChanged(); }
    update();
}

void DocInkCanvas::flushEraseGesture()
{
    if (!eraseGesture_ || eraseDirty_.isEmpty() || !model_) return;
    int lo = INT_MAX, hi = -1;
    for (const QString& id : eraseDirty_) {
        const int row = model_->rowForId(id);
        if (row < 0) continue;
        lo = std::min(lo, row); hi = std::max(hi, row);
    }
    if (hi < 0) { eraseDirty_.clear(); return; }
    model_->beginGroup(lo, hi);        // ONE undo step per press→release
    for (const QString& id : eraseDirty_) {
        const int row = model_->rowForId(id);
        if (row < 0) continue;
        const auto it = cache_.constFind(id);
        model_->setBlockInk(row, (it == cache_.constEnd() || it->strokes.empty())
                                     ? QString() : mn::docInkToJson(*it));
    }
    model_->endGroup();
    eraseDirty_.clear();
}

// ---- Selection / move ------------------------------------------------------

void DocInkCanvas::clearSelection()
{
    moving_ = false; moveDirty_ = false;
    if (selIdx_ < 0) return;
    selBlockId_.clear(); selIdx_ = -1;
    emit selectionChanged();
    update();
}

void DocInkCanvas::selectPress(QPointF itemPos)
{
    const QPointF contentPt(itemPos.x() + contentX_, itemPos.y() + contentY_);
    QString id; int idx = -1;
    if (hitTest(contentPt, id, idx)) {
        if (id != selBlockId_ || idx != selIdx_) {
            selBlockId_ = id; selIdx_ = idx;
            emit selectionChanged();
        }
        moving_ = true; moveDirty_ = false;
        lastContentPt_ = contentPt;
        update();
    } else {
        clearSelection();
    }
}

void DocInkCanvas::selectMove(QPointF itemPos)
{
    if (!moving_ || selIdx_ < 0) return;
    auto it = cache_.find(selBlockId_);
    if (it == cache_.end() || selIdx_ >= int(it->strokes.size())) return;
    const Placement pl = placementFor(selBlockId_, it->space);
    if (!pl.valid) return;
    const QPointF contentPt(itemPos.x() + contentX_, itemPos.y() + contentY_);
    const QPointF dLocal((contentPt.x() - lastContentPt_.x()) / pl.scale.width(),
                         (contentPt.y() - lastContentPt_.y()) / pl.scale.height());
    if (dLocal.isNull()) return;
    for (QPointF& p : it->strokes[size_t(selIdx_)].points) p += dLocal;
    lastContentPt_ = contentPt;
    moveDirty_ = true;
    update();
}

void DocInkCanvas::selectRelease()
{
    if (!moving_) return;
    moving_ = false;
    if (!moveDirty_ || selIdx_ < 0 || !model_) return;
    moveDirty_ = false;

    auto it = cache_.find(selBlockId_);
    if (it == cache_.end() || selIdx_ >= int(it->strokes.size())) return;
    const Placement oldPl = placementFor(selBlockId_, it->space);
    if (!oldPl.valid) return;

    // Re-anchor by the topmost rule: a big move can land the stroke on a
    // different block.
    qcv::ActiveStroke moved = it->strokes[size_t(selIdx_)];
    qreal minCy = 1e18;
    for (const QPointF& p : moved.points)
        minCy = std::min(minCy, localToContent(oldPl, p).y());
    const qreal total = std::max<qreal>(1.0, model_->totalHeight());
    const int newRow = model_->rowForY(std::clamp(minCy, qreal(0), total - 1));
    const int oldRow = oldPl.row;

    if (newRow == oldRow || newRow < 0) {
        model_->setBlockInk(oldRow, mn::docInkToJson(*it));   // one undo step
        return;
    }

    // Convert into the new anchor's space and commit both blobs as one step.
    const bool media = model_->typeForRow(newRow) == BlockModel::Media;
    const auto newSpace = media ? mn::DocInkAnchor::Frame : mn::DocInkAnchor::Px;
    const Placement newPl = placementFor(model_->idForRow(newRow), newSpace);
    if (!newPl.valid) return;
    for (QPointF& p : moved.points)
        p = contentToLocal(newPl, localToContent(oldPl, p));
    // stroke_width: convert between the two spaces' width units via page px.
    moved.strokeWidth = float(double(moved.strokeWidth) * oldPl.widthScale / newPl.widthScale);

    mn::DocInkAnchor oldAnchor = *it;
    oldAnchor.strokes.erase(oldAnchor.strokes.begin() + selIdx_);
    mn::DocInkAnchor newAnchor;
    mn::docInkFromJson(model_->inkForRow(newRow), newAnchor);
    newAnchor.space = newSpace;
    newAnchor.strokes.push_back(std::move(moved));

    model_->beginGroup(std::min(oldRow, newRow), std::max(oldRow, newRow));
    model_->setBlockInk(oldRow, oldAnchor.strokes.empty() ? QString()
                                                          : mn::docInkToJson(oldAnchor));
    model_->setBlockInk(newRow, mn::docInkToJson(newAnchor));
    model_->endGroup();
    clearSelection();
}

void DocInkCanvas::deleteSelection()
{
    if (selIdx_ < 0 || !model_) return;
    auto it = cache_.find(selBlockId_);
    if (it == cache_.end() || selIdx_ >= int(it->strokes.size())) { clearSelection(); return; }
    const int row = model_->rowForId(selBlockId_);
    if (row < 0) { clearSelection(); return; }
    mn::DocInkAnchor a = *it;
    a.strokes.erase(a.strokes.begin() + selIdx_);
    clearSelection();
    model_->setBlockInk(row, a.strokes.empty() ? QString() : mn::docInkToJson(a));
}
