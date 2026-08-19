#include "DocInkCanvas.h"
#include "annotation_thumbnail.h"   // qcv::paintStroke
#include "../core/BlockModel.h"

#include <QCursor>
#include <QLineF>
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

    // The text tool places chips via textCreateRequested — no stroke engine.
    textToolArmed_ = (tool == QLatin1String("text"));
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

void DocInkCanvas::setTextSize(qreal s)
{
    if (qFuzzyCompare(textSize_, s)) return;
    textSize_ = s;
    emit textSizeChanged();
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
    if (inkMode_ && textToolArmed_)       setCursor(Qt::IBeamCursor);
    else if (inkMode_ && drawToolActive_) setCursor(Qt::CrossCursor);
    else if (inkMode_)                    setCursor(Qt::ArrowCursor);
    else                                  unsetCursor();
}

void DocInkCanvas::rebuildCache()
{
    cache_.clear();
    int n = 0;
    if (model_) {
        const QStringList ids = model_->inkBlockIds();
        for (const QString& id : ids) {
            mn::DocInkAnchor a;
            if (mn::docInkFromJson(model_->inkForBlock(id), a)
                && (!a.strokes.empty() || !a.texts.empty())) {
                n += int(a.strokes.size()) + int(a.texts.size());
                cache_.insert(id, std::move(a));
            }
        }
    }
    rebuildTextHeights();
    pruneSelection();
    if (n != strokeCount_) { strokeCount_ = n; emit strokeCountChanged(); }
    update();
}

void DocInkCanvas::setTextFamily(const QString& f)
{
    if (f == textFamily_) return;
    textFamily_ = f;
    rebuildTextHeights();
    emit textFamilyChanged();
    update();
}

double DocInkCanvas::textLocalHeight(const mn::DocInkAnchor& a, const Placement& pl,
                                     const mn::SketchTextSpec& t) const
{
    mn::SketchTextSpec spec = t;
    spec.family = textFamily_;
    if (a.space == mn::DocInkAnchor::Frame) {
        // Layout at media-INTRINSIC px; local units are display-frame
        // fractions (aspect preserved, so hSrc/mediaH is the local height).
        const double mw = pl.row >= 0 && model_ ? std::max(1, model_->mediaW(pl.row)) : 1.0;
        const double mh = pl.row >= 0 && model_ ? std::max(1, model_->mediaH(pl.row)) : 1.0;
        return mn::sketchTextHeightSrc(spec, mw) / mh;
    }
    return mn::sketchTextHeightSrc(spec, 1.0);   // px space: local IS page px
}

void DocInkCanvas::rebuildTextHeights()
{
    textH_.clear();
    for (auto it = cache_.constBegin(); it != cache_.constEnd(); ++it) {
        if (it->texts.empty()) continue;
        const Placement pl = placementFor(it.key(), it->space);
        std::vector<double> hs;
        hs.reserve(it->texts.size());
        for (const mn::SketchTextSpec& t : it->texts)
            hs.push_back(pl.valid ? textLocalHeight(*it, pl, t) : 0.0);
        textH_.insert(it.key(), std::move(hs));
    }
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

        // Text chips paint BEFORE the anchor's strokes (z-order rule: ink
        // can circle/arrow over labels). Geometry from the cached local
        // heights; the per-space helper compositions from the plan.
        if (!it->texts.empty()) {
            const QPointF originItem(pl.origin.x() - contentX_,
                                     pl.origin.y() - contentY_);
            const auto th = textH_.constFind(it.key());
            for (int i = 0; i < int(it->texts.size()); ++i) {
                mn::SketchTextSpec spec = it->texts[size_t(i)];
                spec.family = textFamily_;
                const double hLocal =
                    (th != textH_.constEnd() && i < int(th->size()))
                        ? (*th)[size_t(i)] : 0.0;
                const QRectF itemRect(
                    originItem.x() + spec.x * pl.scale.width(),
                    originItem.y() + spec.y * pl.scale.height(),
                    spec.w * pl.scale.width(), hLocal * pl.scale.height());
                if (!itemRect.intersects(viewport)) continue;
                p->save();
                p->translate(originItem);
                if (it->space == mn::DocInkAnchor::Frame)
                    mn::paintSketchText(*p, spec,
                                        std::max(1, model_->mediaW(pl.row)),
                                        std::max(1, model_->mediaH(pl.row)),
                                        pl.widthScale);
                else
                    mn::paintSketchText(*p, spec, 1.0, 1.0, 1.0);
                p->restore();

                if (selContains(it.key(), SelText, i)) {
                    // Dashed accent box; grips (4 corners + 2 mid-edge wrap)
                    // only when this chip is the SINGLE selection — groups
                    // move/delete only (the SketchCanvas chrome, item px).
                    const QColor accent(0x01, 0x89, 0xf1);
                    QPen pen(accent, 1.0, Qt::DashLine);
                    p->setPen(pen);
                    p->setBrush(Qt::NoBrush);
                    p->drawRect(itemRect);
                    if (sel_.size() == 1) {
                        const double hs = 4.5;
                        const QPointF gs[6] = {
                            itemRect.topLeft(), itemRect.topRight(),
                            itemRect.bottomLeft(), itemRect.bottomRight(),
                            QPointF(itemRect.left(),  itemRect.center().y()),
                            QPointF(itemRect.right(), itemRect.center().y()) };
                        p->setBrush(accent);
                        p->setPen(QPen(QColor(255, 255, 255), 1.0));
                        for (const QPointF& g : gs)
                            p->drawRect(QRectF(g.x() - hs, g.y() - hs, hs * 2, hs * 2));
                    }
                }
            }
        }

        for (int i = 0; i < int(it->strokes.size()); ++i) {
            const qcv::ActiveStroke& src = it->strokes[size_t(i)];
            if (src.points.empty()) continue;
            // Transform into ITEM px (paintStroke w=h=1 passes points through).
            // Positions go through the anchor transform; an oval's radii
            // point (points[1]) is a VECTOR — scale only, never translate.
            qcv::ActiveStroke s = src;
            const bool oval = (s.tool == qcv::DrawingTool::Oval && s.points.size() >= 2);
            for (size_t pi = 0; pi < s.points.size(); ++pi) {
                QPointF& pt = s.points[pi];
                if (oval && pi == 1) {
                    pt = QPointF(pt.x() * pl.scale.width(), pt.y() * pl.scale.height());
                } else {
                    const QPointF c = localToContent(pl, pt);
                    pt = QPointF(c.x() - contentX_, c.y() - contentY_);
                }
            }
            const double penW = std::max(1.0, double(s.strokeWidth) * pl.widthScale);
            // strokeBoundsNorm knows the oval center±radii box; points are
            // item px here, so its "normalized" units are already px.
            const QRectF bbox = qcv::strokeBoundsNorm(s)
                                    .adjusted(-penW, -penW, penW, penW);
            if (!bbox.intersects(viewport)) continue;
            qcv::paintStroke(*p, s, 1.0, 1.0, pl.widthScale);

            if (selContains(it.key(), SelStroke, i)) {   // selection outline
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

    // Live marquee: accent hairline + whisper fill (the drag-ghost language).
    // Stored in content px (scroll-glued); painted in item px.
    if (marquee_ && marqueeRect_.isValid()) {
        const QRectF mr = marqueeRect_.translated(-contentX_, -contentY_);
        const QColor accent(0x01, 0x89, 0xf1);
        p->fillRect(mr, QColor(accent.red(), accent.green(), accent.blue(), 20));
        QPen pen(accent, 1.0, Qt::DashLine);
        p->setPen(pen);
        p->setBrush(Qt::NoBrush);
        p->drawRect(mr);
    }
}

// ---- Capture -----------------------------------------------------------

void DocInkCanvas::mousePressEvent(QMouseEvent* e)
{
    if (inkMode_ && textToolArmed_) {
        // Place a chip: same topmost-anchor rule as a finished stroke; emit
        // ANCHOR-LOCAL geometry (size space-converted) for the QML overlay.
        const QPointF contentPt(e->position().x() + contentX_,
                                e->position().y() + contentY_);
        int row = -1; Placement pl;
        if (anchorAtContent(contentPt, row, pl)) {
            const QPointF local = contentToLocal(pl, contentPt);
            const bool frame = pl.space == mn::DocInkAnchor::Frame;
            const double lsize = frame ? textSize_ / pl.widthScale : textSize_;
            const double defWPage = std::min(0.5 * pageWidth_,
                                             std::max(240.0, 2.0 * textSize_));
            const double lw = frame ? std::min(0.9, defWPage / pl.scale.width())
                                    : defWPage;
            emit textCreateRequested(row, local.x(), local.y(), lw, lsize);
        }
        e->accept();
        return;
    }
    if (inSelectMode()) { selectPress(e->position(), e->modifiers()); e->accept(); return; }
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

    // Topmost overlapped block = the anchor (shared rule, see anchorAtContent).
    int row = -1; Placement pl;
    if (!anchorAtContent(QPointF(0, minCy), row, pl)) return;
    const auto space = pl.space;

    qcv::ActiveStroke s = *stroke;
    for (size_t i = 0; i < content.size(); ++i)
        s.points[i] = contentToLocal(pl, content[i]);
    // An oval's points[1] is its RADII — a VECTOR, not a position. The
    // position transform above bakes the anchor origin + press-time scroll
    // into it, which is the "circles morph when anything moves" bug: the
    // radius then depends on where the page was scrolled when you drew.
    // Vectors convert by scale alone.
    if (s.tool == qcv::DrawingTool::Oval && s.points.size() >= 2)
        s.points[1] = QPointF(stroke->points[1].x() * width() / pl.scale.width(),
                              stroke->points[1].y() * height() / pl.scale.height());
    if (space == mn::DocInkAnchor::Frame)   // width stored in media-intrinsic px
        s.strokeWidth = float(double(s.strokeWidth) / pl.widthScale);

    mn::DocInkAnchor a;
    mn::docInkFromJson(model_->inkForRow(row), a);
    a.space = space;   // anchor type determines the space, always
    a.strokes.push_back(std::move(s));
    model_->setBlockInk(row, mn::docInkToJson(a));   // one undo step
}

int DocInkCanvas::selectedTextRow() const
{
    if (sel_.size() != 1 || sel_[0].kind != SelText || !model_) return -1;
    return model_->rowForId(sel_[0].blockId);
}

qreal DocInkCanvas::inkTextSizeScale(int row) const
{
    if (!model_ || row < 0 || row >= model_->rowCountQml()) return 1.0;
    if (model_->typeForRow(row) != BlockModel::Media) return 1.0;
    const Placement pl = placementFor(model_->idForRow(row), mn::DocInkAnchor::Frame);
    return pl.valid && pl.widthScale > 0 ? pl.widthScale : 1.0;
}

bool DocInkCanvas::anchorAtContent(QPointF contentPt, int& rowOut, Placement& plOut) const
{
    if (!model_) return false;
    const qreal total = std::max<qreal>(1.0, model_->totalHeight());
    const int row = model_->rowForY(std::clamp(contentPt.y(), qreal(0), total - 1));
    if (row < 0 || row >= model_->rowCountQml()) return false;
    const bool media = model_->typeForRow(row) == BlockModel::Media;
    const auto space = media ? mn::DocInkAnchor::Frame : mn::DocInkAnchor::Px;
    plOut = placementFor(model_->idForRow(row), space);
    if (!plOut.valid) return false;
    rowOut = row;
    return true;
}

// ---- Text chips ----------------------------------------------------------

namespace {
// 2em width floor in the anchor's local units.
double inkTextMinW(mn::DocInkAnchor::Space space, double size, double mediaW)
{
    return space == mn::DocInkAnchor::Frame ? (2.0 * size) / std::max(1.0, mediaW)
                                            : 2.0 * size;
}
} // namespace

int DocInkCanvas::inkAddText(int row, qreal x, qreal y, qreal w,
                             const QString& text, qreal size,
                             const QString& colorHex)
{
    if (!model_ || row < 0 || row >= model_->rowCountQml()) return -1;
    if (text.trimmed().isEmpty() || size <= 0) return -1;
    const bool media = model_->typeForRow(row) == BlockModel::Media;
    const auto space = media ? mn::DocInkAnchor::Frame : mn::DocInkAnchor::Px;
    mn::DocInkAnchor a;
    mn::docInkFromJson(model_->inkForRow(row), a);
    a.space = space;   // anchor type determines the space, always
    mn::SketchTextSpec t;
    t.text = text;
    t.x = x; t.y = y;
    t.w = std::max<double>(w, inkTextMinW(space, size, model_->mediaW(row)));
    t.size = size;
    t.color = QColor(colorHex);
    a.texts.push_back(std::move(t));
    model_->setBlockInk(row, mn::docInkToJson(a));   // one undo step
    return int(a.texts.size()) - 1;
}

void DocInkCanvas::inkSetText(int row, int idx, const QString& text)
{
    if (!model_ || row < 0 || row >= model_->rowCountQml()) return;
    mn::DocInkAnchor a;
    if (!mn::docInkFromJson(model_->inkForRow(row), a)) return;
    if (idx < 0 || idx >= int(a.texts.size())) return;
    if (a.texts[size_t(idx)].text == text) return;   // no txn
    if (text.trimmed().isEmpty())
        a.texts.erase(a.texts.begin() + idx);        // blank commit = delete
    else
        a.texts[size_t(idx)].text = text;
    model_->setBlockInk(row, mn::docInkToJson(a));   // "" when anchor empties
}

void DocInkCanvas::inkSetTextBox(int row, int idx, qreal x, qreal y,
                                 qreal w, qreal size)
{
    if (!model_ || row < 0 || row >= model_->rowCountQml() || size <= 0) return;
    mn::DocInkAnchor a;
    if (!mn::docInkFromJson(model_->inkForRow(row), a)) return;
    if (idx < 0 || idx >= int(a.texts.size())) return;
    mn::SketchTextSpec& t = a.texts[size_t(idx)];
    t.x = x; t.y = y;
    t.w = std::max<double>(w, inkTextMinW(a.space, size, model_->mediaW(row)));
    t.size = size;
    model_->setBlockInk(row, mn::docInkToJson(a));
}

void DocInkCanvas::inkRemoveText(int row, int idx)
{
    if (!model_ || row < 0 || row >= model_->rowCountQml()) return;
    mn::DocInkAnchor a;
    if (!mn::docInkFromJson(model_->inkForRow(row), a)) return;
    if (idx < 0 || idx >= int(a.texts.size())) return;
    a.texts.erase(a.texts.begin() + idx);
    model_->setBlockInk(row, mn::docInkToJson(a));   // "" when anchor empties
}

QVariantMap DocInkCanvas::inkTextAt(int row, int idx) const
{
    QVariantMap out;
    if (!model_ || row < 0 || row >= model_->rowCountQml()) return out;
    mn::DocInkAnchor a;
    if (!mn::docInkFromJson(model_->inkForRow(row), a)) return out;
    if (idx < 0 || idx >= int(a.texts.size())) return out;
    const mn::SketchTextSpec& t = a.texts[size_t(idx)];
    out.insert(QStringLiteral("x"), t.x);
    out.insert(QStringLiteral("y"), t.y);
    out.insert(QStringLiteral("w"), t.w);
    out.insert(QStringLiteral("text"), t.text);
    out.insert(QStringLiteral("size"), t.size);
    out.insert(QStringLiteral("color"), t.color.name());
    return out;
}

QVariantMap DocInkCanvas::inkTextOverlayGeom(int row, qreal lx, qreal ly,
                                             qreal lw, qreal lsize) const
{
    QVariantMap out;
    out.insert(QStringLiteral("valid"), false);
    if (!model_ || row < 0 || row >= model_->rowCountQml()) return out;
    const bool media = model_->typeForRow(row) == BlockModel::Media;
    const auto space = media ? mn::DocInkAnchor::Frame : mn::DocInkAnchor::Px;
    const Placement pl = placementFor(model_->idForRow(row), space);
    if (!pl.valid) return out;
    const double sizeScale = space == mn::DocInkAnchor::Frame ? pl.widthScale : 1.0;
    out.insert(QStringLiteral("valid"), true);
    out.insert(QStringLiteral("x"), pl.origin.x() + lx * pl.scale.width() - contentX_);
    out.insert(QStringLiteral("y"), pl.origin.y() + ly * pl.scale.height() - contentY_);
    out.insert(QStringLiteral("w"), lw * pl.scale.width());
    out.insert(QStringLiteral("sizePx"), lsize * sizeScale);
    out.insert(QStringLiteral("padPx"), lsize * 0.4 * sizeScale);
    return out;
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
            // Oval-aware bounds (center ± radii); plain point bbox otherwise.
            const QRectF b = qcv::strokeBoundsNorm(s);
            const double padX = tolX + double(s.strokeWidth) / (2.0 * pl.scale.width());
            const double padY = tolY + double(s.strokeWidth) / (2.0 * pl.scale.height());
            if (local.x() >= b.left() - padX && local.x() <= b.right() + padX
                && local.y() >= b.top() - padY && local.y() <= b.bottom() + padY) {
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
    // Indices in this anchor shifted — drop its selected items (rare: the
    // eraser is armed, so a live selection is already unusual).
    const size_t before = sel_.size();
    sel_.erase(std::remove_if(sel_.begin(), sel_.end(),
        [&id](const SelItem& s) { return s.blockId == id; }), sel_.end());
    if (sel_.size() != before) emit selectionChanged();
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
        model_->setBlockInk(row,
            (it == cache_.constEnd() || (it->strokes.empty() && it->texts.empty()))
                ? QString() : mn::docInkToJson(*it));
    }
    model_->endGroup();
    eraseDirty_.clear();
}

// ---- Selection / move ------------------------------------------------------

void DocInkCanvas::clearSelection()
{
    moving_ = false; moveDirty_ = false;
    resizing_ = false; grabCorner_ = -1;
    marquee_ = false;
    if (sel_.empty()) return;
    sel_.clear();
    emit selectionChanged();
    update();
}

bool DocInkCanvas::selContains(const QString& blockId, SelKind k, int idx) const
{
    for (const SelItem& it : sel_)
        if (it.kind == k && it.idx == idx && it.blockId == blockId) return true;
    return false;
}

void DocInkCanvas::toggleSel(const QString& blockId, SelKind k, int idx)
{
    for (auto it = sel_.begin(); it != sel_.end(); ++it) {
        if (it->kind == k && it->idx == idx && it->blockId == blockId) {
            sel_.erase(it);
            emit selectionChanged();
            update();
            return;
        }
    }
    sel_.push_back({blockId, k, idx});
    emit selectionChanged();
    update();
}

void DocInkCanvas::pruneSelection()
{
    const size_t before = sel_.size();
    sel_.erase(std::remove_if(sel_.begin(), sel_.end(), [this](const SelItem& s) {
        auto it = cache_.constFind(s.blockId);
        if (it == cache_.constEnd() || s.idx < 0) return true;
        return s.kind == SelStroke ? s.idx >= int(it->strokes.size())
                                   : s.idx >= int(it->texts.size());
    }), sel_.end());
    if (sel_.size() != before) {
        if (sel_.empty()) { moving_ = false; resizing_ = false; }
        emit selectionChanged();
        update();
    }
}

QRectF DocInkCanvas::itemContentRect(const SelItem& s) const
{
    auto it = cache_.constFind(s.blockId);
    if (it == cache_.constEnd()) return {};
    const Placement pl = placementFor(s.blockId, it->space);
    if (!pl.valid) return {};
    if (s.kind == SelStroke) {
        if (s.idx < 0 || s.idx >= int(it->strokes.size())) return {};
        const QRectF b = qcv::strokeBoundsNorm(it->strokes[size_t(s.idx)]);   // local units
        if (b.isNull()) return {};
        return QRectF(localToContent(pl, b.topLeft()),
                      localToContent(pl, b.bottomRight())).normalized();
    }
    if (s.idx < 0 || s.idx >= int(it->texts.size())) return {};
    const mn::SketchTextSpec& t = it->texts[size_t(s.idx)];
    const auto th = textH_.constFind(s.blockId);
    const double hLocal = (th != textH_.constEnd() && s.idx < int(th->size()))
                              ? (*th)[size_t(s.idx)] : 0.0;
    return QRectF(localToContent(pl, QPointF(t.x, t.y)),
                  localToContent(pl, QPointF(t.x + t.w, t.y + hLocal))).normalized();
}

void DocInkCanvas::selectPress(QPointF itemPos, Qt::KeyboardModifiers mods)
{
    const QPointF contentPt(itemPos.x() + contentX_, itemPos.y() + contentY_);
    const bool shift = mods.testFlag(Qt::ShiftModifier);
    // A press on the single selected chip's grip starts a resize (groups
    // move/delete only — no group scale in M1).
    if (sel_.size() == 1 && sel_[0].kind == SelText && !shift) {
        const int h = textHandleAt(itemPos);
        if (h >= 0) {
            auto it = cache_.constFind(sel_[0].blockId);
            if (it != cache_.constEnd() && sel_[0].idx < int(it->texts.size())) {
                const mn::SketchTextSpec& t = it->texts[size_t(sel_[0].idx)];
                const auto th = textH_.constFind(sel_[0].blockId);
                const double hLocal =
                    (th != textH_.constEnd() && sel_[0].idx < int(th->size()))
                        ? (*th)[size_t(sel_[0].idx)] : 0.0;
                origLocalRect_ = QRectF(t.x, t.y, t.w, hLocal);
                origTextW_ = t.w;
                origTextSize_ = t.size;
                grabCorner_ = h;
                resizing_ = true;
                moving_ = false;
                moveDirty_ = false;
                return;
            }
        }
    }
    QString id; SelKind k = SelNone; int idx = -1;
    if (!hitTestAny(contentPt, id, k, idx)) {
        // Empty press: plain clears and starts a marquee; shift keeps the set
        // (additive marquee). Content-px coords keep the band scroll-glued.
        if (!shift) clearSelection();
        marquee_ = true;
        marqueeAnchor_ = contentPt;
        marqueeRect_ = QRectF(contentPt, contentPt);
        update();
        return;
    }
    if (shift) { toggleSel(id, k, idx); return; }   // membership toggle, no move
    if (!selContains(id, k, idx)) {
        // Not in the set → replace; in the set → keep the group and drag it.
        sel_.clear();
        sel_.push_back({id, k, idx});
        emit selectionChanged();
    }
    moving_ = true; moveDirty_ = false;
    lastContentPt_ = contentPt;
    update();
}

bool DocInkCanvas::hitTestAny(QPointF contentPt, QString& blockIdOut,
                              SelKind& kindOut, int& idxOut) const
{
    // Strokes paint on top of chips — test them first.
    if (hitTest(contentPt, blockIdOut, idxOut)) { kindOut = SelStroke; return true; }
    for (auto it = cache_.constBegin(); it != cache_.constEnd(); ++it) {
        if (it->texts.empty()) continue;
        const Placement pl = placementFor(it.key(), it->space);
        if (!pl.valid) continue;
        const QPointF local = contentToLocal(pl, contentPt);
        const auto th = textH_.constFind(it.key());
        for (int i = int(it->texts.size()) - 1; i >= 0; --i) {
            const mn::SketchTextSpec& t = it->texts[size_t(i)];
            const double hLocal = (th != textH_.constEnd() && i < int(th->size()))
                                      ? (*th)[size_t(i)] : 0.0;
            if (QRectF(t.x, t.y, t.w, hLocal).contains(local)) {
                blockIdOut = it.key(); kindOut = SelText; idxOut = i;
                return true;
            }
        }
    }
    kindOut = SelNone;
    return false;
}

QRectF DocInkCanvas::selTextItemRect() const
{
    // Grips target the SINGLE selected chip; groups have outlines only.
    if (sel_.size() != 1 || sel_[0].kind != SelText) return {};
    const QRectF c = itemContentRect(sel_[0]);
    if (!c.isValid()) return {};
    return c.translated(-contentX_, -contentY_);
}

int DocInkCanvas::textHandleAt(QPointF itemPos) const
{
    const QRectF r = selTextItemRect();
    if (!r.isValid()) return -1;
    const double tol = 9.0;   // screen px (tolerances are screen-absolute)
    const QPointF gs[6] = { r.topLeft(), r.topRight(), r.bottomLeft(),
                            r.bottomRight(),
                            QPointF(r.left(),  r.center().y()),
                            QPointF(r.right(), r.center().y()) };
    for (int i = 0; i < 6; ++i)
        if (QLineF(itemPos, gs[i]).length() <= tol) return i;
    return -1;
}

void DocInkCanvas::selectMove(QPointF itemPos)
{
    const QPointF contentPt(itemPos.x() + contentX_, itemPos.y() + contentY_);
    if (marquee_) {
        marqueeRect_ = QRectF(marqueeAnchor_, contentPt).normalized();
        update();
        return;
    }
    if (resizing_) { textResizeTo(contentPt); return; }
    if (!moving_ || sel_.empty()) return;
    const QPointF dContent = contentPt - lastContentPt_;
    if (dContent.isNull()) return;
    // Group translate: the delta is CONTENT px; each item converts through
    // ITS anchor's placement scale, so mixed px/frame selections move in
    // lockstep on screen.
    for (const SelItem& s : sel_) {
        auto it = cache_.find(s.blockId);
        if (it == cache_.end()) continue;
        const Placement pl = placementFor(s.blockId, it->space);
        if (!pl.valid) continue;
        const QPointF dLocal(dContent.x() / pl.scale.width(),
                             dContent.y() / pl.scale.height());
        if (s.kind == SelText) {
            if (s.idx >= int(it->texts.size())) continue;
            it->texts[size_t(s.idx)].x += dLocal.x();
            it->texts[size_t(s.idx)].y += dLocal.y();
        } else {
            if (s.idx >= int(it->strokes.size())) continue;
            // translate positions; an oval's radii vector is translation-immune
            auto& mv = it->strokes[size_t(s.idx)];
            const bool oval = (mv.tool == qcv::DrawingTool::Oval && mv.points.size() >= 2);
            for (size_t pi = 0; pi < mv.points.size(); ++pi) {
                if (oval && pi == 1) continue;
                mv.points[pi] += dLocal;
            }
        }
    }
    lastContentPt_ = contentPt;
    moveDirty_ = true;
    update();
}

void DocInkCanvas::textResizeTo(QPointF contentPt)
{
    if (sel_.size() != 1 || sel_[0].kind != SelText || !origLocalRect_.isValid()) return;
    const QString selBlockId = sel_[0].blockId;
    const int selIdx = sel_[0].idx;
    auto it = cache_.find(selBlockId);
    if (it == cache_.end() || selIdx >= int(it->texts.size())) return;
    const Placement pl = placementFor(selBlockId, it->space);
    if (!pl.valid) return;
    const QPointF local = contentToLocal(pl, contentPt);
    mn::SketchTextSpec& t = it->texts[size_t(selIdx)];
    const double mediaW = pl.row >= 0 && model_ ? model_->mediaW(pl.row) : 1.0;
    if (grabCorner_ >= 4) {
        // Mid-edge wrap drag: the opposite edge stays fixed; text reflows.
        const double minW = (it->space == mn::DocInkAnchor::Frame)
            ? (2.0 * t.size) / std::max(1.0, mediaW) : 2.0 * t.size;
        const double fixedX = grabCorner_ == 4 ? origLocalRect_.right()
                                               : origLocalRect_.left();
        const double newW = std::max(minW, grabCorner_ == 4 ? fixedX - local.x()
                                                            : local.x() - fixedX);
        t.x = grabCorner_ == 4 ? fixedX - newW : fixedX;
        t.w = newW;
    } else {
        // Corner: proportional scale of size+w about the opposite corner —
        // the w/size ratio stays constant, so wrap points are preserved.
        QPointF pivot;
        switch (grabCorner_) {
            case 0: pivot = origLocalRect_.bottomRight(); break;   // TL
            case 1: pivot = origLocalRect_.bottomLeft();  break;   // TR
            case 2: pivot = origLocalRect_.topRight();    break;   // BL
            default: pivot = origLocalRect_.topLeft();    break;   // BR
        }
        const double ow = origLocalRect_.width(), oh = origLocalRect_.height();
        if (ow <= 0 || oh <= 0) return;
        double s = std::max(std::abs(local.x() - pivot.x()) / ow,
                            std::abs(local.y() - pivot.y()) / oh);
        const double minW = (it->space == mn::DocInkAnchor::Frame)
            ? (2.0 * origTextSize_) / std::max(1.0, mediaW) : 2.0 * origTextSize_;
        s = std::max(s, minW / std::max(1e-9, origTextW_));
        t.x = pivot.x() + (origLocalRect_.left() - pivot.x()) * s;
        t.y = pivot.y() + (origLocalRect_.top()  - pivot.y()) * s;
        t.w = origTextW_ * s;
        t.size = origTextSize_ * s;
    }
    // Live reflow: refresh this anchor's cached heights.
    auto th = textH_.find(selBlockId);
    if (th != textH_.end() && selIdx < int(th->size()))
        (*th)[size_t(selIdx)] = textLocalHeight(*it, pl, t);
    moveDirty_ = true;
    update();
}

void DocInkCanvas::selectRelease()
{
    if (marquee_) {
        // Finalize the rubber band: everything intersecting joins the set.
        // (A no-drag band selects nothing — that's the deselect click.)
        marquee_ = false;
        bool changed = false;
        if (marqueeRect_.width() > 2.0 || marqueeRect_.height() > 2.0) {
            for (auto it = cache_.constBegin(); it != cache_.constEnd(); ++it) {
                for (int i = 0; i < int(it->strokes.size()); ++i) {
                    if (selContains(it.key(), SelStroke, i)) continue;
                    const QRectF b = itemContentRect({it.key(), SelStroke, i});
                    if (b.isValid() && b.intersects(marqueeRect_)) {
                        sel_.push_back({it.key(), SelStroke, i});
                        changed = true;
                    }
                }
                for (int i = 0; i < int(it->texts.size()); ++i) {
                    if (selContains(it.key(), SelText, i)) continue;
                    const QRectF b = itemContentRect({it.key(), SelText, i});
                    if (b.isValid() && b.intersects(marqueeRect_)) {
                        sel_.push_back({it.key(), SelText, i});
                        changed = true;
                    }
                }
            }
        }
        if (changed) emit selectionChanged();
        update();
        return;
    }
    if (resizing_) {
        resizing_ = false; grabCorner_ = -1;
        const bool dirty = moveDirty_;
        moveDirty_ = false;
        if (!dirty || sel_.size() != 1 || sel_[0].kind != SelText || !model_) return;
        auto rit = cache_.find(sel_[0].blockId);
        const int rrow = model_->rowForId(sel_[0].blockId);
        if (rit != cache_.end() && rrow >= 0)
            model_->setBlockInk(rrow, mn::docInkToJson(*rit));   // one undo step
        return;
    }
    if (!moving_) return;
    moving_ = false;
    if (!moveDirty_ || sel_.empty() || !model_) return;
    moveDirty_ = false;

    // Group commit with BATCHED re-anchoring: every moved element
    // independently lands on the topmost block its (already-moved) geometry
    // overlaps — one drag can carry items to different blocks. Build the
    // result anchors in working copies, then commit every touched blob as
    // ONE grouped undo step.
    struct Departure { QString fromId; SelKind kind; int idx; int newRow; };
    std::vector<Departure> departures;
    QSet<QString> touched;   // sources always commit (cache_ moved them in place)
    for (const SelItem& s : sel_) {
        auto it = cache_.constFind(s.blockId);
        if (it == cache_.constEnd()) continue;
        const Placement oldPl = placementFor(s.blockId, it->space);
        if (!oldPl.valid) continue;
        touched.insert(s.blockId);
        int newRow = -1;
        if (s.kind == SelStroke) {
            if (s.idx >= int(it->strokes.size())) continue;
            // Oval-aware top edge (the radii point is not a position — a raw
            // min-over-points would read it as one).
            const qreal minCy = localToContent(
                oldPl, qcv::strokeBoundsNorm(it->strokes[size_t(s.idx)]).topLeft()).y();
            const qreal total = std::max<qreal>(1.0, model_->totalHeight());
            newRow = model_->rowForY(std::clamp(minCy, qreal(0), total - 1));
        } else {
            if (s.idx >= int(it->texts.size())) continue;
            const mn::SketchTextSpec& t = it->texts[size_t(s.idx)];
            const qreal topCy = localToContent(oldPl, QPointF(t.x, t.y)).y();
            int r = -1; Placement pl2;
            if (anchorAtContent(QPointF(0, topCy), r, pl2)) newRow = r;
        }
        if (newRow >= 0 && newRow != oldPl.row)
            departures.push_back({s.blockId, s.kind, s.idx, newRow});
    }

    QHash<QString, mn::DocInkAnchor> result;   // working copies to commit
    auto workFor = [&](const QString& id) -> mn::DocInkAnchor& {
        auto rit = result.find(id);
        if (rit != result.end()) return *rit;
        auto cit = cache_.constFind(id);
        if (cit != cache_.constEnd()) return *result.insert(id, *cit);
        mn::DocInkAnchor a;
        const int r = model_->rowForId(id);
        if (r >= 0) mn::docInkFromJson(model_->inkForRow(r), a);
        return *result.insert(id, a);
    };
    for (const QString& id : touched) workFor(id);

    // Convert departures FROM the working copies while indices are valid;
    // removals and arrivals apply after (removals DESC per source anchor).
    struct Arrival { QString toId; mn::DocInkAnchor::Space space = mn::DocInkAnchor::Px;
                     SelKind kind = SelNone;
                     qcv::ActiveStroke stroke; mn::SketchTextSpec text; };
    std::vector<Arrival> arrivals;
    for (const Departure& d : departures) {
        const mn::DocInkAnchor& from = workFor(d.fromId);
        const Placement oldPl = placementFor(d.fromId, from.space);
        if (!oldPl.valid) continue;
        const QString toId = model_->idForRow(d.newRow);
        if (toId.isEmpty()) continue;
        // Convert into the new anchor's space: positions re-anchor through
        // content px; oval radii VECTORS and widths/sizes by the two spaces'
        // scale ratios (page-px-preserving — the standing re-anchor rule).
        const bool media = model_->typeForRow(d.newRow) == BlockModel::Media;
        const auto newSpace = media ? mn::DocInkAnchor::Frame : mn::DocInkAnchor::Px;
        const Placement newPl = placementFor(toId, newSpace);
        if (!newPl.valid) continue;
        Arrival a; a.toId = toId; a.space = newSpace; a.kind = d.kind;
        if (d.kind == SelStroke) {
            if (d.idx >= int(from.strokes.size())) continue;
            qcv::ActiveStroke moved = from.strokes[size_t(d.idx)];
            const bool movedOval = (moved.tool == qcv::DrawingTool::Oval && moved.points.size() >= 2);
            for (size_t pi = 0; pi < moved.points.size(); ++pi) {
                QPointF& p = moved.points[pi];
                if (movedOval && pi == 1)
                    p = QPointF(p.x() * oldPl.scale.width() / newPl.scale.width(),
                                p.y() * oldPl.scale.height() / newPl.scale.height());
                else
                    p = contentToLocal(newPl, localToContent(oldPl, p));
            }
            moved.strokeWidth = float(double(moved.strokeWidth) * oldPl.widthScale / newPl.widthScale);
            a.stroke = std::move(moved);
        } else {
            if (d.idx >= int(from.texts.size())) continue;
            mn::SketchTextSpec moved = from.texts[size_t(d.idx)];
            const QPointF newLocal =
                contentToLocal(newPl, localToContent(oldPl, QPointF(moved.x, moved.y)));
            moved.x = newLocal.x();
            moved.y = newLocal.y();
            moved.w = moved.w * oldPl.scale.width() / newPl.scale.width();
            moved.size = moved.size * oldPl.widthScale / newPl.widthScale;
            a.text = std::move(moved);
        }
        arrivals.push_back(std::move(a));
        touched.insert(toId);
    }
    QHash<QString, std::vector<int>> rmStrokes, rmTexts;
    for (const Departure& d : departures)
        (d.kind == SelStroke ? rmStrokes : rmTexts)[d.fromId].push_back(d.idx);
    for (auto it = rmStrokes.begin(); it != rmStrokes.end(); ++it) {
        std::sort(it->rbegin(), it->rend());
        mn::DocInkAnchor& a = workFor(it.key());
        for (int i : *it)
            if (i >= 0 && i < int(a.strokes.size())) a.strokes.erase(a.strokes.begin() + i);
    }
    for (auto it = rmTexts.begin(); it != rmTexts.end(); ++it) {
        std::sort(it->rbegin(), it->rend());
        mn::DocInkAnchor& a = workFor(it.key());
        for (int i : *it)
            if (i >= 0 && i < int(a.texts.size())) a.texts.erase(a.texts.begin() + i);
    }
    for (Arrival& a : arrivals) {
        mn::DocInkAnchor& to = workFor(a.toId);
        to.space = a.space;
        if (a.kind == SelStroke) to.strokes.push_back(std::move(a.stroke));
        else                     to.texts.push_back(std::move(a.text));
    }

    int lo = INT_MAX, hi = -1;
    for (const QString& id : touched) {
        const int r = model_->rowForId(id);
        if (r >= 0) { lo = std::min(lo, r); hi = std::max(hi, r); }
    }
    if (hi < 0) { clearSelection(); return; }
    const bool group = touched.size() > 1;
    if (group) model_->beginGroup(lo, hi);
    for (const QString& id : touched) {
        const int r = model_->rowForId(id);
        if (r < 0) continue;
        const mn::DocInkAnchor& a = result[id];
        model_->setBlockInk(r, (a.strokes.empty() && a.texts.empty())
                                   ? QString() : mn::docInkToJson(a));
    }
    if (group) model_->endGroup();
    clearSelection();
}

void DocInkCanvas::deleteSelection()
{
    if (sel_.empty() || !model_) return;
    // Group per anchor, erase DESCENDING, commit all touched blobs as ONE
    // grouped undo step (single-anchor deletes stay a plain single txn).
    QHash<QString, std::vector<int>> rmStrokes, rmTexts;
    for (const SelItem& s : sel_)
        (s.kind == SelStroke ? rmStrokes : rmTexts)[s.blockId].push_back(s.idx);
    QSet<QString> touched;
    QHash<QString, mn::DocInkAnchor> result;
    auto applyRm = [&](QHash<QString, std::vector<int>>& rm, bool strokes) {
        for (auto it = rm.begin(); it != rm.end(); ++it) {
            auto cit = cache_.constFind(it.key());
            if (cit == cache_.constEnd()) continue;
            if (!result.contains(it.key())) result.insert(it.key(), *cit);
            mn::DocInkAnchor& a = result[it.key()];
            std::sort(it->rbegin(), it->rend());
            for (int i : *it) {
                if (strokes) {
                    if (i >= 0 && i < int(a.strokes.size()))
                        a.strokes.erase(a.strokes.begin() + i);
                } else {
                    if (i >= 0 && i < int(a.texts.size()))
                        a.texts.erase(a.texts.begin() + i);
                }
            }
            touched.insert(it.key());
        }
    };
    applyRm(rmStrokes, true);
    applyRm(rmTexts, false);
    clearSelection();
    if (touched.isEmpty()) return;
    int lo = INT_MAX, hi = -1;
    for (const QString& id : touched) {
        const int r = model_->rowForId(id);
        if (r >= 0) { lo = std::min(lo, r); hi = std::max(hi, r); }
    }
    if (hi < 0) return;
    const bool group = touched.size() > 1;
    if (group) model_->beginGroup(lo, hi);
    for (const QString& id : touched) {
        const int r = model_->rowForId(id);
        if (r < 0) continue;
        const mn::DocInkAnchor& a = result[id];
        model_->setBlockInk(r, (a.strokes.empty() && a.texts.empty())
                                   ? QString() : mn::docInkToJson(a));
    }
    if (group) model_->endGroup();
}

void DocInkCanvas::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (inSelectMode()) {
        const QPointF contentPt(e->position().x() + contentX_,
                                e->position().y() + contentY_);
        QString id; SelKind k = SelNone; int idx = -1;
        if (hitTestAny(contentPt, id, k, idx) && k == SelText) {
            // The first press of the double-click armed a move — cancel it or
            // a 1px jitter commits a phantom move txn (disarm-first rule).
            moving_ = false;
            moveDirty_ = false;
            marquee_ = false;
            if (sel_.size() != 1 || sel_[0].kind != SelText
                || sel_[0].idx != idx || sel_[0].blockId != id) {
                sel_.clear();
                sel_.push_back({id, SelText, idx});
                emit selectionChanged();
                update();
            }
            const int row = model_ ? model_->rowForId(id) : -1;
            if (row >= 0) emit textEditRequested(row, idx);
            e->accept();
            return;
        }
    }
    QQuickPaintedItem::mouseDoubleClickEvent(e);
}
