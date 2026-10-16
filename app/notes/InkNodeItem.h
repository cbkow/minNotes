#pragma once
// Canvas B5: a block run's margin ink as scene-graph geometry (no QPainter, no texture upload).
// One QSGTransformNode per anchor block (its placement origin) over one QSGGeometryNode of capsule
// quads (ink_tessellate) drawn by InkMaterial (ink.vert/.frag). Placement is the SHARED
// mn::inkPlacement — the same maths DocInkCanvas uses for the page — with the run's first y
// subtracted (originY) so the item lives inside a card. Rebuilds on the model's ink, layout and
// structure signals and on its own row range / scale; the matrices and geometry are refreshed in
// updatePaintNode (the GUI thread is blocked there, so reading the model is safe).
#include "ink_tessellate.h"
#include "BlockModel.h"   // QPointer needs the full type
#include <QQuickItem>
#include <QPointer>
#include <QtQml/qqmlregistration.h>
#include <vector>


class InkNodeItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(BlockModel* model READ model WRITE setModel NOTIFY modelChanged)
    Q_PROPERTY(int rowLo READ rowLo WRITE setRowLo NOTIFY rangeChanged)
    Q_PROPERTY(int rowHi READ rowHi WRITE setRowHi NOTIFY rangeChanged)
    Q_PROPERTY(qreal originY READ originY WRITE setOriginY NOTIFY placementChanged)
    Q_PROPERTY(qreal leftEdge READ leftEdge WRITE setLeftEdge NOTIFY placementChanged)
    Q_PROPERTY(qreal pxScale READ pxScale WRITE setPxScale NOTIFY pxScaleChanged)   // screen px per local px (the camera zoom; the DPR is applied here)
    Q_PROPERTY(int anchorCount READ anchorCount NOTIFY statsChanged)              // probe: anchors with geometry in the last build
    Q_PROPERTY(int vertexCount READ vertexCount NOTIFY statsChanged)
public:
    explicit InkNodeItem(QQuickItem* parent = nullptr);
    BlockModel* model() const { return model_; }
    void setModel(BlockModel* m);
    int rowLo() const { return rowLo_; }
    void setRowLo(int v);
    int rowHi() const { return rowHi_; }
    void setRowHi(int v);
    qreal originY() const { return originY_; }
    void setOriginY(qreal v);
    qreal leftEdge() const { return leftEdge_; }
    void setLeftEdge(qreal v);
    qreal pxScale() const { return pxScale_; }
    void setPxScale(qreal v);
    int anchorCount() const { return anchorCount_; }
    int vertexCount() const { return vertexCount_; }
    // Probe: the item-px position of an anchor's first stroke point (the page/canvas placement check).
    Q_INVOKABLE QPointF firstPointOf(int row) const;
signals:
    void modelChanged();
    void rangeChanged();
    void placementChanged();
    void pxScaleChanged();
    void statsChanged();
protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
private:
    void markDirty();
    QPointer<BlockModel> model_;
    std::vector<QMetaObject::Connection> conns_;
    int rowLo_ = -1, rowHi_ = -1;
    qreal originY_ = 0, leftEdge_ = 0, pxScale_ = 1;
    bool dirty_ = true;
    double builtMargin_ = -1;
    int anchorCount_ = 0, vertexCount_ = 0;
};
