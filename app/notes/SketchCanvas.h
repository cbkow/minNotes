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
#include <QQuickPaintedItem>
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
    bool armed() const { return !toolName_.isEmpty(); }
    bool isDrawing() const { return drawing_; }
    bool empty() const { return strokes_.empty(); }

    Q_INVOKABLE void cancelStroke();   // Esc mid-drag

signals:
    void dataChanged();
    void toolChanged();
    void colorChanged();
    void strokeWidthChanged();
    void sourceWidthChanged();
    void drawingChanged();
    // A finished mutation (stroke committed / stroke erased) — the stroke
    // JSON in the engine's schema. QML commits it to the block model.
    void edited(const QString &strokesJson);

protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void geometryChange(const QRectF &newGeo, const QRectF &oldGeo) override;

private:
    void route(qcv::PointerPhase phase, QPointF pos, qint64 tMs);
    void commitStroke(std::unique_ptr<qcv::ActiveStroke> stroke);
    void eraseAt(QPointF norm);
    void setDrawing(bool d);

    QString data_;
    QString toolName_;
    int sourceWidth_ = 0;
    bool drawing_ = false;

    qcv::ViewportAnnotator annot_;
    std::vector<qcv::ActiveStroke> strokes_;   // parsed from data_
};
