#pragma once
// GPU ink (canvas B5, re-targeted for 2.0 stage 1): ONE stroke envelope — the engine's QCView schema
// {version, coordinate_system:"normalized", shapes[]} — drawn as capsule-SDF quads over a FRAME:
// points are fractions of (frameW, frameH), stroke widths are source px scaled by frameW/sourceW,
// exactly what qcv::paintStroke draws for the same envelope, so an image's or a PDF page's media
// ink matches its export and (in 2.0) the canvas. Rebuilds when the envelope, the frame, the
// source size or the feather's octave change. No model dependency: the host binds `envelope`
// (e.g. blockModel.mediaPageInk(row, page)). Stage 2 adds the canvas' world-ink source on top.
#include "ink_tessellate.h"
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

class InkNodeItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString envelope READ envelope WRITE setEnvelope NOTIFY envelopeChanged)
    Q_PROPERTY(qreal frameW READ frameW WRITE setFrameW NOTIFY frameChanged)      // the display frame, item px
    Q_PROPERTY(qreal frameH READ frameH WRITE setFrameH NOTIFY frameChanged)
    Q_PROPERTY(qreal sourceW READ sourceW WRITE setSourceW NOTIFY frameChanged)   // the media's intrinsic px (stroke-width units)
    Q_PROPERTY(qreal sourceH READ sourceH WRITE setSourceH NOTIFY frameChanged)
    Q_PROPERTY(qreal pxScale READ pxScale WRITE setPxScale NOTIFY pxScaleChanged) // screen px per item px (the view zoom; DPR applied here)
    Q_PROPERTY(int strokeCount READ strokeCount NOTIFY statsChanged)
    Q_PROPERTY(int vertexCount READ vertexCount NOTIFY statsChanged)
public:
    explicit InkNodeItem(QQuickItem* parent = nullptr);
    QString envelope() const { return envelope_; }
    void setEnvelope(const QString& e);
    qreal frameW() const { return frameW_; }
    void setFrameW(qreal v);
    qreal frameH() const { return frameH_; }
    void setFrameH(qreal v);
    qreal sourceW() const { return sourceW_; }
    void setSourceW(qreal v);
    qreal sourceH() const { return sourceH_; }
    void setSourceH(qreal v);
    qreal pxScale() const { return pxScale_; }
    void setPxScale(qreal v);
    int strokeCount() const { return strokeCount_; }
    int vertexCount() const { return vertexCount_; }
signals:
    void envelopeChanged();
    void frameChanged();
    void pxScaleChanged();
    void statsChanged();
protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
private:
    void markDirty() { dirty_ = true; update(); }
    QString envelope_;
    qreal frameW_ = 0, frameH_ = 0, sourceW_ = 0, sourceH_ = 0, pxScale_ = 1;
    bool dirty_ = true;
    double builtMargin_ = -1;
    int strokeCount_ = 0, vertexCount_ = 0;
};
