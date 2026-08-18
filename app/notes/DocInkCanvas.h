// DocInkCanvas — the tier-2 margin-ink layer: ONE viewport-sized
// QQuickPaintedItem over the whole scrolling document (never content-sized —
// that would allocate a totalHeight-tall texture). Renders every visible
// anchor's strokes with the content→viewport transform, and captures new
// strokes when ink mode arms it.
//
// Interaction ownership follows the shipped VideoAnnotator pattern: DISARMED
// it refuses mouse events entirely, so every click/drag falls through to the
// editor's central mouse layer untouched; ARMED (inkMode) it owns the
// gesture. It is a root-level sibling of the Flickable — not a delegate — so
// the "block delegates never own MouseAreas" rule is respected.
//
// Anchoring (PLAN-document-annotations.md, ratified): a finalized stroke
// anchors to the TOPMOST block its bbox overlaps; text-ish anchors store
// block-local PIXELS (Δx from page center, Δy from block top), media anchors
// store FRAME-NORMALIZED fractions (ink scales with the media resize).
// Content offsets are FROZEN at Press so a wheel-scroll mid-stroke cannot
// skew the anchoring. Persistence goes through BlockModel::setBlockInk (one
// undo step per stroke; eraser and re-anchor gestures group their calls).
#pragma once

#include "doc_ink.h"
#include "viewport_annotator.h"

#include <QColor>
#include <QHash>
#include <QQuickPaintedItem>
#include <QSet>
#include <QVariant>

class BlockModel;

class DocInkCanvas : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(QObject* model READ modelObject WRITE setModelObject NOTIFY modelChanged FINAL)
    // Viewport transform inputs, bound from QML (flick.contentX/Y etc.).
    Q_PROPERTY(qreal contentX READ contentX WRITE setContentX NOTIFY transformChanged FINAL)
    Q_PROPERTY(qreal contentY READ contentY WRITE setContentY NOTIFY transformChanged FINAL)
    Q_PROPERTY(qreal leftEdgeContent READ leftEdgeContent WRITE setLeftEdgeContent NOTIFY transformChanged FINAL)
    Q_PROPERTY(qreal pageWidth READ pageWidth WRITE setPageWidth NOTIFY transformChanged FINAL)
    // The Inspector Draw trio ("" disarmed | freehand|rect|oval|arrow|line|
    // eraser|text) + the text-box size (page px; converted per space).
    Q_PROPERTY(QString tool READ tool WRITE setTool NOTIFY toolChanged FINAL)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged FINAL)
    Q_PROPERTY(qreal strokeWidth READ strokeWidth WRITE setStrokeWidth NOTIFY strokeWidthChanged FINAL)
    Q_PROPERTY(qreal textSize READ textSize WRITE setTextSize NOTIFY textSizeChanged FINAL)
    // Annotation mode: accepts mouse iff true (draw when a tool is armed,
    // select/move when not). Render happens regardless of mode.
    Q_PROPERTY(bool inkMode READ inkMode WRITE setInkMode NOTIFY inkModeChanged FINAL)
    Q_PROPERTY(bool drawing READ isDrawing NOTIFY drawingChanged FINAL)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged FINAL)
    // The selected TEXT chip (-1s unless one is selected) — the Inspector
    // Size slider retargets it. Row is the anchor's current model row.
    Q_PROPERTY(int selectedTextRow READ selectedTextRow NOTIFY selectionChanged FINAL)
    Q_PROPERTY(int selectedTextIndex READ selectedTextIndex NOTIFY selectionChanged FINAL)
    // Total annotations (strokes + text chips) across all anchors — feeds
    // the "N hidden" pill. Name kept for QML stability.
    Q_PROPERTY(int strokeCount READ strokeCount NOTIFY strokeCountChanged FINAL)
    // Text-chip font, QML-bound from Theme (the SketchCanvas parity rule —
    // the editing overlay and the paint can never disagree).
    Q_PROPERTY(QString textFamily READ textFamily WRITE setTextFamily NOTIFY textFamilyChanged FINAL)
    QML_ELEMENT

public:
    explicit DocInkCanvas(QQuickItem* parent = nullptr);
    ~DocInkCanvas() override;

    void paint(QPainter* p) override;

    QObject* modelObject() const;
    void setModelObject(QObject* m);
    qreal contentX() const { return contentX_; }
    void setContentX(qreal v);
    qreal contentY() const { return contentY_; }
    void setContentY(qreal v);
    qreal leftEdgeContent() const { return leftEdgeContent_; }
    void setLeftEdgeContent(qreal v);
    qreal pageWidth() const { return pageWidth_; }
    void setPageWidth(qreal v);
    QString tool() const { return toolName_; }
    void setTool(const QString& tool);
    QColor color() const { return annot_.drawingColor(); }
    void setColor(const QColor& c);
    qreal strokeWidth() const { return annot_.strokeWidth(); }
    void setStrokeWidth(qreal w);
    bool inkMode() const { return inkMode_; }
    void setInkMode(bool on);
    bool isDrawing() const { return drawing_; }
    bool hasSelection() const { return selKind_ != SelNone; }
    int strokeCount() const { return strokeCount_; }
    QString textFamily() const { return textFamily_; }
    void setTextFamily(const QString& f);
    qreal textSize() const { return textSize_; }
    void setTextSize(qreal s);

    // --- Text-chip ops (each = one setBlockInk = one undo step; coords and
    // size in ANCHOR-LOCAL units: px anchors page px, frame anchors
    // normalized w/ intrinsic-px size — the stroke conventions). ---
    Q_INVOKABLE int inkAddText(int row, qreal x, qreal y, qreal w,
                               const QString& text, qreal size,
                               const QString& colorHex);
    // Blank (or whitespace) text DELETES the element; unchanged = no txn.
    Q_INVOKABLE void inkSetText(int row, int index, const QString& text);
    Q_INVOKABLE void inkSetTextBox(int row, int index, qreal x, qreal y,
                                   qreal w, qreal size);
    Q_INVOKABLE void inkRemoveText(int row, int index);
    Q_INVOKABLE QVariantMap inkTextAt(int row, int index) const;
    // Anchor-local geometry → ITEM coords for the QML overlay:
    // {valid, x, y, w, sizePx, padPx}. Pure function — the caller's binding
    // supplies reactivity (contentX/contentY/layoutRevision deps).
    Q_INVOKABLE QVariantMap inkTextOverlayGeom(int row, qreal lx, qreal ly,
                                               qreal lw, qreal lsize) const;
    Q_INVOKABLE QColor textInkFor(const QColor& bg) const {
        return mn::sketchTextInkColor(bg);
    }
    // Local size units → page px for a row's anchor space (1.0 for px
    // anchors, widthScale for frame anchors) — the slider's conversion.
    Q_INVOKABLE qreal inkTextSizeScale(int row) const;

    int selectedTextRow() const;
    int selectedTextIndex() const { return selKind_ == SelText ? selIdx_ : -1; }

    Q_INVOKABLE void cancelStroke();      // Esc mid-drag (also aborts an erase gesture)
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void deleteSelection();

    // --- Selection kinds (chips join strokes). Public so hasSelection's
    // inline can see it; QML never touches these values. ---
    enum SelKind { SelNone, SelStroke, SelText };

signals:
    void modelChanged();
    void transformChanged();
    void toolChanged();
    void colorChanged();
    void strokeWidthChanged();
    void inkModeChanged();
    void drawingChanged();
    void selectionChanged();
    void strokeCountChanged();
    void textFamilyChanged();
    void textSizeChanged();
    // Text-tool click resolved to its anchor: the QML overlay opens a create
    // session with these ANCHOR-LOCAL values (lsize already space-converted).
    void textCreateRequested(int row, qreal lx, qreal ly, qreal lw, qreal lsize);
    // Double-click on a chip in select mode → the overlay re-edits it.
    void textEditRequested(int row, int index);

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;   // re-edit a chip
    void geometryChange(const QRectF& newGeo, const QRectF& oldGeo) override;

private:
    // The content-space placement of one anchor: origin/scale that maps its
    // block-local points into content px (and the painter's width scale).
    struct Placement {
        bool valid = false;
        int row = -1;
        mn::DocInkAnchor::Space space = mn::DocInkAnchor::Px;
        QPointF origin;         // content px of the anchor's local (0,0)
        QSizeF scale{1, 1};     // local units → content px multipliers
        double widthScale = 1;  // stroke_width units → page px
    };
    Placement placementFor(const QString& blockId,
                           mn::DocInkAnchor::Space space) const;
    QPointF localToContent(const Placement& pl, QPointF local) const;
    QPointF contentToLocal(const Placement& pl, QPointF content) const;

    void rebuildCache();
    void applyAcceptedButtons();
    void route(qcv::PointerPhase phase, QPointF pos, qint64 tMs);
    void commitStroke(std::unique_ptr<qcv::ActiveStroke> stroke);
    void eraseAt(QPointF norm);
    void setDrawing(bool d);
    bool inSelectMode() const {
        return inkMode_ && !drawToolActive_ && !textToolArmed_;
    }
    // The topmost-overlapped-block anchor rule (shared by stroke commit and
    // text placement): content point → row + placement in the block's space.
    bool anchorAtContent(QPointF contentPt, int& rowOut, Placement& plOut) const;
    // Topmost stroke under a content point: fills blockId/index, or false.
    // (Strokes ONLY — the eraser's view; chips are erase-immune.)
    bool hitTest(QPointF contentPt, QString& blockIdOut, int& idxOut) const;
    // Selection hit: strokes first (they paint on top), then chips.
    bool hitTestAny(QPointF contentPt, QString& blockIdOut,
                    SelKind& kindOut, int& idxOut) const;
    // The selected chip's rect in ITEM px (placement + cached height).
    QRectF selTextItemRect() const;
    // Handle under an item point for the selected chip: 0-3 corners,
    // 4=left-mid 5=right-mid, or -1.
    int textHandleAt(QPointF itemPos) const;
    void textResizeTo(QPointF contentPt);
    void selectPress(QPointF itemPos);
    void selectMove(QPointF itemPos);
    void selectRelease();
    void flushEraseGesture();

    BlockModel* model_ = nullptr;
    qreal contentX_ = 0, contentY_ = 0;
    qreal leftEdgeContent_ = 0, pageWidth_ = 760;
    QString toolName_;
    bool inkMode_ = false;
    bool drawing_ = false;
    bool drawToolActive_ = false;
    bool textToolArmed_ = false;   // the text tool (places chips, no strokes)
    qreal textSize_ = 16;          // Inspector-bound (page px)

    // Per-text derived chip height in ANCHOR-LOCAL units (px anchors: page
    // px; frame anchors: fractions of the display frame). Computed at
    // rebuild — paint/hit never run layout per frame.
    double textLocalHeight(const mn::DocInkAnchor& a, const Placement& pl,
                           const mn::SketchTextSpec& t) const;
    void rebuildTextHeights();

    qcv::ViewportAnnotator annot_;
    QHash<QString, mn::DocInkAnchor> cache_;   // blockId → parsed anchor
    QHash<QString, std::vector<double>> textH_;   // blockId → per-text local height
    QString textFamily_;                       // QML-bound; "" → helper probe
    int strokeCount_ = 0;

    // Content offsets frozen at Press (see header comment).
    qreal pressContentX_ = 0, pressContentY_ = 0;

    // Eraser gesture: cache_ is mutated live for feedback; dirty anchors are
    // committed as ONE grouped undo step on release (the sketch/video rule).
    bool eraseGesture_ = false;
    QSet<QString> eraseDirty_;

    // Selection / move (select mode = inkMode with no draw tool armed).
    QString selBlockId_;
    SelKind selKind_ = SelNone;
    int selIdx_ = -1;
    bool moving_ = false, moveDirty_ = false;
    QPointF lastContentPt_;
    // Chip resize (corner scale / mid-edge wrap drag).
    bool resizing_ = false;
    int grabCorner_ = -1;
    QRectF origLocalRect_;                 // chip rect at grab (local units)
    double origTextW_ = 0, origTextSize_ = 0;
};
