// SketchCanvas — the Sketch block's stroke surface: one QQuickPaintedItem
// that renders a sketch's strokes from the block content JSON and (when a
// tool is armed) captures drawing. VideoAnnotator's sibling, with the
// opposite persistence story: edits round-trip through the BLOCK MODEL —
// the canvas emits edited(strokesJson), QML commits it via
// blockModel.sketchSetShapes (beginTxn → DOCUMENT undo), and the data
// binding feeds the merged content back. No internal undo stacks here;
// ⌘Z in a sketch tab is plain document undo.
//
// Used twice: disarmed inline (the passive MediaBlock render — refuses
// mouse events, pure painter) and armed in the full-frame sketch tab
// (direct mouse, per the full-frame-tab pattern). Coordinates are
// normalized [0,1] of the canvas — the QCView stroke schema verbatim, so
// the shared engine (serializer / modeler / paintStroke) does all of it.

#pragma once

#include "active_stroke.h"
#include "sketch_text.h"
#include "viewport_annotator.h"

#include <QColor>
#include <QHash>
#include <QImage>
#include <QQuickPaintedItem>
#include <QRectF>
#include <QString>
#include <QVariant>
#include <QtQmlIntegration>

#include <optional>
#include <vector>

class SketchCanvas : public QQuickPaintedItem
{
    Q_OBJECT
    // The block's content JSON (canvas meta + strokes); strokes re-parse on
    // every change — the model is the single source of truth.
    Q_PROPERTY(QString data READ data WRITE setData NOTIFY dataChanged FINAL)
    // "" disarmed | freehand | rect | oval | arrow | line | eraser
    Q_PROPERTY(QString tool READ tool WRITE setTool NOTIFY toolChanged FINAL)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged FINAL)
    Q_PROPERTY(qreal strokeWidth READ strokeWidth WRITE setStrokeWidth NOTIFY strokeWidthChanged FINAL)
    // Canvas intrinsic width in source px — stroke widths are stored in
    // source units (QCView semantics); rendering scales by width()/sourceWidth.
    Q_PROPERTY(int sourceWidth READ sourceWidth WRITE setSourceWidth NOTIFY sourceWidthChanged FINAL)
    Q_PROPERTY(int sourceHeight READ sourceHeight WRITE setSourceHeight NOTIFY sourceHeightChanged FINAL)
    // Camera mode (the full-frame tab): the item fills the stage and the
    // canvas FRAME floats inside it at pan/zoom — panX/panY = the frame's
    // top-left in item px, zoom = screen px per source px. Capture goes
    // unclamped (overflow ink), painting clips to the frame. Disabled
    // (default, the inline embed): the frame IS the item and zoom derives
    // from width()/sourceWidth — bit-identical to the pre-camera behavior.
    Q_PROPERTY(bool cameraEnabled READ cameraEnabled WRITE setCameraEnabled NOTIFY cameraEnabledChanged FINAL)
    Q_PROPERTY(qreal panX READ panX WRITE setPanX NOTIFY cameraChanged FINAL)
    Q_PROPERTY(qreal panY READ panY WRITE setPanY NOTIFY cameraChanged FINAL)
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY cameraChanged FINAL)
    // Frame-border tone, bound from QML Theme (drawn in C++ so it can never
    // lag the camera by a frame).
    Q_PROPERTY(QColor frameBorderColor READ frameBorderColor WRITE setFrameBorderColor NOTIFY frameBorderColorChanged FINAL)
    // Space held (the tab drives it): mouse drags pan the camera instead of
    // drawing/selecting; open/closed-hand cursors.
    Q_PROPERTY(bool panMode READ panMode WRITE setPanMode NOTIFY panModeChanged FINAL)
    // Text-element font, QML-bound from Theme (identical to the editing
    // overlay's by construction — never resolved independently in C++ here).
    Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY fontFamilyChanged FINAL)
    // Index of the text element the QML overlay is editing (hidden from
    // paint so there's no double-vision); -1 = none.
    Q_PROPERTY(int editingTextIndex READ editingTextIndex WRITE setEditingTextIndex NOTIFY editingTextIndexChanged FINAL)
    Q_PROPERTY(bool armed READ armed NOTIFY toolChanged FINAL)
    Q_PROPERTY(bool drawing READ isDrawing NOTIFY drawingChanged FINAL)
    Q_PROPERTY(bool empty READ empty NOTIFY dataChanged FINAL)
    // When true and no draw tool is armed, the canvas is in select/move mode
    // (click an element to select, drag to move, Delete to remove). The inline
    // embed leaves this false (passive); the full-frame tab sets it true.
    Q_PROPERTY(bool selectable READ selectable WRITE setSelectable NOTIFY selectableChanged FINAL)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged FINAL)
    // The selected TEXT element's index (-1 unless a chip is selected) — the
    // Inspector Size slider retargets it.
    Q_PROPERTY(int selectedTextIndex READ selectedTextIndex NOTIFY selectionChanged FINAL)
    // Signed content bbox (normalized; strokes padded by half their width) and
    // whether ink lives beyond the frame. Consumed by fit-to-ink zoom, the
    // ghost pass, and the tab's overflow indicator.
    Q_PROPERTY(QRectF contentBoundsNorm READ contentBoundsNorm NOTIFY dataChanged FINAL)
    Q_PROPERTY(bool hasOverflow READ hasOverflow NOTIFY dataChanged FINAL)
    QML_ELEMENT

public:
    explicit SketchCanvas(QQuickItem *parent = nullptr);
    ~SketchCanvas() override;

    void paint(QPainter *p) override;

    QString data() const { return data_; }
    void setData(const QString &data);
    QString tool() const { return toolName_; }
    void setTool(const QString &tool);
    QColor color() const { return annot_.drawingColor(); }
    void setColor(const QColor &c);
    qreal strokeWidth() const { return annot_.strokeWidth(); }
    void setStrokeWidth(qreal w);
    int sourceWidth() const { return sourceWidth_; }
    void setSourceWidth(int w);
    int sourceHeight() const { return sourceHeight_; }
    void setSourceHeight(int h);
    bool cameraEnabled() const { return cameraEnabled_; }
    void setCameraEnabled(bool on);
    qreal panX() const { return panX_; }
    void setPanX(qreal x);
    qreal panY() const { return panY_; }
    void setPanY(qreal y);
    qreal zoom() const { return zoom_; }
    void setZoom(qreal z);
    QColor frameBorderColor() const { return frameBorderColor_; }
    void setFrameBorderColor(const QColor &c);
    bool panMode() const { return panMode_; }
    void setPanMode(bool on);
    QString fontFamily() const { return fontFamily_; }
    void setFontFamily(const QString &f);
    int editingTextIndex() const { return editingTextIndex_; }
    void setEditingTextIndex(int i);
    // A placing tool is armed (draw tool or text tool — not select).
    bool armed() const { return drawToolActive_ || textToolArmed_; }
    bool isDrawing() const { return drawing_; }
    bool empty() const { return strokes_.empty() && images_.empty() && texts_.empty(); }
    bool selectable() const { return selectable_; }
    void setSelectable(bool s);
    bool hasSelection() const { return selKind_ != SelNone; }
    int selectedTextIndex() const { return selKind_ == SelText ? selIdx_ : -1; }
    QRectF contentBoundsNorm() const;
    bool hasOverflow() const;

    Q_INVOKABLE void cancelStroke();      // Esc mid-drag
    Q_INVOKABLE void clearSelection();    // Esc / click empty
    Q_INVOKABLE void deleteSelection();   // Delete / Backspace
    // The overlay's read of one text element: {x, y, w, text, size, color}.
    Q_INVOKABLE QVariantMap textElementAt(int index) const;
    // Chip glyph color for a fill (black/white by luminance) — the overlay
    // asks HERE so QML and C++ can never disagree on the formula.
    Q_INVOKABLE QColor textInkFor(const QColor &bg) const {
        return mn::sketchTextInkColor(bg);
    }

signals:
    void dataChanged();
    void toolChanged();
    void colorChanged();
    void strokeWidthChanged();
    void sourceWidthChanged();
    void sourceHeightChanged();
    void cameraEnabledChanged();
    void cameraChanged();
    void frameBorderColorChanged();
    void panModeChanged();
    void fontFamilyChanged();
    void editingTextIndexChanged();
    // Any direct camera input on the canvas (wheel/pinch/pan-drag) — the tab
    // flips its "user owns the camera" flag so resizes stop re-fitting.
    void userCameraInput();
    void drawingChanged();
    void selectableChanged();
    void selectionChanged();
    // A finished mutation (stroke committed / erased / moved / deleted) — the
    // stroke JSON in the engine's schema. QML commits it to the block model.
    void edited(const QString &strokesJson);
    // Image-element edits are index-based so the portable (doc-relative) src is
    // never round-tripped through the canvas. QML commits via the block model.
    void imageRectChanged(int index, qreal x, qreal y, qreal w, qreal h);
    void imageRemoved(int index);
    // Text-element edits (index-based, the image pattern): geometry from
    // move/scale/wrap drags, removal from Delete.
    void textBoxChanged(int index, qreal x, qreal y, qreal w, qreal size);
    void textRemoved(int index);
    // Text-tool click on the canvas (normalized point) → the QML overlay
    // opens a create session; double-click a text element → re-edit.
    void textCreateRequested(qreal nx, qreal ny);
    void textEditRequested(int index);

protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;   // re-edit a text box
    void hoverMoveEvent(QHoverEvent *e) override;   // resize cursors + brush circle
    void hoverLeaveEvent(QHoverEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;       // camera: scroll pan / ⌘-zoom
    bool event(QEvent *ev) override;                // camera: native pinch
    void geometryChange(const QRectF &newGeo, const QRectF &oldGeo) override;

private:
    // --- Camera mapping. World = normalized × (sourceW, sourceH) source px;
    // screen = world × zoom + pan. The frame's screen rect is the single
    // basis: the annotator gets it as its viewport, paint clips to it, and
    // both normalization directions go through it.
    qreal effZoom() const;               // screen px per source px
    QRectF frameScreenRect() const;      // the canvas frame in item px
    QPointF screenToNorm(QPointF pos) const;
    QPointF normToScreen(QPointF norm) const;
    void pushViewport();                 // re-sync annot_'s viewport rect
    void zoomAboutPoint(qreal newZoom, QPointF anchor);   // keep anchor fixed
    // Hit slop: 12 SCREEN px expressed in normalized units — constant feel at
    // any zoom (marks are canvas-absolute; tolerances are screen-absolute).
    double hitTolNorm() const;
    void invalidateBounds() { contentBounds_.reset(); }

    void route(qcv::PointerPhase phase, QPointF pos, qint64 tMs);
    void commitStroke(std::unique_ptr<qcv::ActiveStroke> stroke);
    void eraseAt(QPointF norm);
    void setDrawing(bool d);
    void parseImages(const QString &data);     // pull the `images` array from data_
    void parseTexts(const QString &data);      // pull `texts[]`; derives heights
    void refreshTextHeights();                 // recompute hNorm (font/size deps)
    const QImage &imageFor(const QString &src);   // cached load (src = resolved URL/path)

    // --- Selection / move (select mode = selectable_ && no tool armed) ---
    enum SelKind { SelNone, SelStroke, SelImage, SelText };
    // Select mode = selectable and not holding a placing tool — so the explicit
    // "select" tool AND a bare disarm ("") both land here.
    bool inSelectMode() const { return selectable_ && !drawToolActive_ && !textToolArmed_; }
    void applyAcceptedButtons();               // accept mouse iff drawing or selecting
    void selectPress(QPointF pos);
    void selectMove(QPointF pos);
    void selectRelease();
    int  hitTest(QPointF norm, SelKind &kindOut) const;   // topmost element, or SelNone
    QRectF strokeBoundsNorm(int idx) const;    // normalized bbox of a stroke
    QRectF selBoundsNorm() const;              // normalized bbox of the current selection
    QRectF selDisplayRect() const;             // px rect for outline + handles
    // Handle under px: 0=TL 1=TR 2=BL 3=BR corners; 4=left-mid 5=right-mid
    // (SelText only — the wrap-width grips); -1 = none.
    int  handleAtPx(QPointF px) const;
    void translateSelection(QPointF dNorm);    // move (clamped to canvas), live
    void beginResize(int corner);              // grab a handle; pivot = opposite corner
    void resizeTo(QPointF norm);               // proportional scale about the pivot, live

    // A raster image embedded in the sketch (rendered beneath strokes). rect is
    // normalized [0,1] of the canvas; src is a loadable URL/path (resolved by the
    // model before binding).
    struct SketchImage { QString src; QRectF rect; };

    QString data_;
    QString toolName_;
    int sourceWidth_ = 0;
    int sourceHeight_ = 0;
    bool cameraEnabled_ = false;
    qreal panX_ = 0.0, panY_ = 0.0, zoom_ = 1.0;
    QColor frameBorderColor_ = QColor(0x33, 0x33, 0x33);
    bool panMode_ = false;               // space held
    bool panning_ = false;               // pan-drag in progress
    QPointF lastPanPos_;
    QPointF hoverPos_;                   // brush-circle anchor
    bool hoverValid_ = false;
    bool drawing_ = false;
    bool selectable_ = false;
    bool drawToolActive_ = false;              // a real draw tool is armed (not select)
    bool textToolArmed_ = false;               // the text tool (places boxes, no strokes)

    qcv::ViewportAnnotator annot_;
    std::vector<qcv::ActiveStroke> strokes_;   // parsed from data_
    // Eraser gesture (press→release): hits erase an in-flight WORKING COPY,
    // committed as ONE edited() on release — one document-undo step per
    // gesture. Also the correctness fix: the model round-trip that refreshes
    // data_ lags the drag, so the old per-hit re-parse of data_ could
    // resurrect a stroke erased earlier in the same drag.
    bool eraseGesture_ = false;
    bool eraseDirty_   = false;
    std::vector<qcv::ActiveStroke> eraseStrokes_;   // working copy during the gesture
    // A text element: the persisted spec + the DERIVED wrapped height
    // (normalized), computed eagerly at parse so paint/bounds/hit never run
    // layout per frame.
    struct SketchText { mn::SketchTextSpec spec; double hNorm = 0; };

    std::vector<SketchImage> images_;          // parsed from data_ (under the strokes)
    std::vector<SketchText> texts_;            // parsed from data_ (over images, under ink)
    QString fontFamily_;                       // QML-bound (Theme); "" → helper probe
    int editingTextIndex_ = -1;                // hidden from paint while overlaid
    QHash<QString, QImage> imgCache_;          // src → decoded image
    mutable std::optional<QRectF> contentBounds_;   // lazy signed bbox cache

    SelKind selKind_ = SelNone;                // current selection
    int     selIdx_ = -1;
    bool    moving_ = false;                   // move-drag in progress
    bool    resizing_ = false;                 // handle-drag in progress
    bool    moveDirty_ = false;                // the drag actually changed something
    QPointF lastNorm_;                         // last pointer pos (normalized)
    int     grabCorner_ = -1;                  // handle being dragged
    QRectF  origBounds_;                       // selection bounds at resize start (norm)
    std::vector<QPointF> origPoints_;          // stroke points at resize start (absolute scale)
    double  origTextW_ = 0, origTextSize_ = 0; // text w/size at resize start
};
