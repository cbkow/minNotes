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
#include "viewport_annotator.h"

#include <QColor>
#include <QHash>
#include <QImage>
#include <QQuickPaintedItem>
#include <QRectF>
#include <QString>
#include <QtQmlIntegration>

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
    Q_PROPERTY(bool armed READ armed NOTIFY toolChanged FINAL)
    Q_PROPERTY(bool drawing READ isDrawing NOTIFY drawingChanged FINAL)
    Q_PROPERTY(bool empty READ empty NOTIFY dataChanged FINAL)
    // When true and no draw tool is armed, the canvas is in select/move mode
    // (click an element to select, drag to move, Delete to remove). The inline
    // embed leaves this false (passive); the full-frame tab sets it true.
    Q_PROPERTY(bool selectable READ selectable WRITE setSelectable NOTIFY selectableChanged FINAL)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged FINAL)
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
    bool armed() const { return drawToolActive_; }   // a real draw tool (not select)
    bool isDrawing() const { return drawing_; }
    bool empty() const { return strokes_.empty() && images_.empty(); }
    bool selectable() const { return selectable_; }
    void setSelectable(bool s);
    bool hasSelection() const { return selKind_ != SelNone; }

    Q_INVOKABLE void cancelStroke();      // Esc mid-drag
    Q_INVOKABLE void clearSelection();    // Esc / click empty
    Q_INVOKABLE void deleteSelection();   // Delete / Backspace

signals:
    void dataChanged();
    void toolChanged();
    void colorChanged();
    void strokeWidthChanged();
    void sourceWidthChanged();
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

protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void hoverMoveEvent(QHoverEvent *e) override;   // resize-cursor feedback
    void geometryChange(const QRectF &newGeo, const QRectF &oldGeo) override;

private:
    void route(qcv::PointerPhase phase, QPointF pos, qint64 tMs);
    void commitStroke(std::unique_ptr<qcv::ActiveStroke> stroke);
    void eraseAt(QPointF norm);
    void setDrawing(bool d);
    void parseImages(const QString &data);     // pull the `images` array from data_
    const QImage &imageFor(const QString &src);   // cached load (src = resolved URL/path)

    // --- Selection / move (select mode = selectable_ && no tool armed) ---
    enum SelKind { SelNone, SelStroke, SelImage };
    // Select mode = selectable and not holding a real draw tool — so the explicit
    // "select" tool AND a bare disarm ("") both land here.
    bool inSelectMode() const { return selectable_ && !drawToolActive_; }
    void applyAcceptedButtons();               // accept mouse iff drawing or selecting
    void selectPress(QPointF pos);
    void selectMove(QPointF pos);
    void selectRelease();
    int  hitTest(QPointF norm, SelKind &kindOut) const;   // topmost element, or SelNone
    QRectF strokeBoundsNorm(int idx) const;    // normalized bbox of a stroke
    QRectF selBoundsNorm() const;              // normalized bbox of the current selection
    QRectF selDisplayRect() const;             // px rect for outline + handles
    int  handleAtPx(QPointF px) const;         // corner under px (0=TL 1=TR 2=BL 3=BR), or -1
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
    bool drawing_ = false;
    bool selectable_ = false;
    bool drawToolActive_ = false;              // a real draw tool is armed (not select)

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
    std::vector<SketchImage> images_;          // parsed from data_ (under the strokes)
    QHash<QString, QImage> imgCache_;          // src → decoded image

    SelKind selKind_ = SelNone;                // current selection
    int     selIdx_ = -1;
    bool    moving_ = false;                   // move-drag in progress
    bool    resizing_ = false;                 // handle-drag in progress
    bool    moveDirty_ = false;                // the drag actually changed something
    QPointF lastNorm_;                         // last pointer pos (normalized)
    int     grabCorner_ = -1;                  // handle being dragged
    QRectF  origBounds_;                       // selection bounds at resize start (norm)
    std::vector<QPointF> origPoints_;          // stroke points at resize start (absolute scale)
};
