#include "InkNodeItem.h"
#include "BlockModel.h"
#include "doc_ink.h"
#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QSGTransformNode>
#include <QMatrix4x4>
#include <cmath>
#include <cstring>

using namespace Qt::StringLiterals;

namespace {

class InkShader : public QSGMaterialShader {
public:
    InkShader() {
        setShaderFileName(VertexStage, u":/mn/notes/shaders/ink.vert.qsb"_s);
        setShaderFileName(FragmentStage, u":/mn/notes/shaders/ink.frag.qsb"_s);
    }
    bool updateUniformData(RenderState& st, QSGMaterial* newM, QSGMaterial* oldM) override;
};

class InkMaterial : public QSGMaterial {
public:
    // NoBatching: the batch renderer rewrites a merged batch's vertex shader (inserting its z-order
    // attribute), which this shader's layout doesn't fit — and a card holds a handful of ink nodes.
    InkMaterial() { setFlag(Blending, true); setFlag(NoBatching, true); }
    QSGMaterialType* type() const override { static QSGMaterialType t; return &t; }
    int compare(const QSGMaterial* o) const override {
        const float b = static_cast<const InkMaterial*>(o)->pxScale;
        return pxScale < b ? -1 : pxScale > b ? 1 : 0;
    }
    QSGMaterialShader* createShader(QSGRendererInterface::RenderMode) const override { return new InkShader; }
    float pxScale = 1.0f;
};

bool InkShader::updateUniformData(RenderState& st, QSGMaterial* newM, QSGMaterial* oldM) {
    bool changed = false;
    QByteArray* buf = st.uniformData();
    if (st.isMatrixDirty()) { std::memcpy(buf->data(), st.combinedMatrix().constData(), 64); changed = true; }
    if (st.isOpacityDirty()) { const float o = st.opacity(); std::memcpy(buf->data() + 64, &o, 4); changed = true; }
    auto* m = static_cast<InkMaterial*>(newM);
    if (!oldM || static_cast<InkMaterial*>(oldM)->pxScale != m->pxScale) { std::memcpy(buf->data() + 68, &m->pxScale, 4); changed = true; }
    return changed;
}

const QSGGeometry::AttributeSet& inkAttributes() {
    static const QSGGeometry::Attribute attrs[] = {
        QSGGeometry::Attribute::createWithAttributeType(0, 2, QSGGeometry::FloatType, QSGGeometry::PositionAttribute),
        QSGGeometry::Attribute::createWithAttributeType(1, 2, QSGGeometry::FloatType, QSGGeometry::UnknownAttribute),
        QSGGeometry::Attribute::createWithAttributeType(2, 2, QSGGeometry::FloatType, QSGGeometry::UnknownAttribute),
        QSGGeometry::Attribute::createWithAttributeType(3, 1, QSGGeometry::FloatType, QSGGeometry::UnknownAttribute),
        QSGGeometry::Attribute::createWithAttributeType(4, 4, QSGGeometry::UnsignedByteType, QSGGeometry::ColorAttribute),
    };
    static const QSGGeometry::AttributeSet set = { 5, sizeof(mn::ink::Vertex), attrs };
    return set;
}
static_assert(sizeof(mn::ink::Vertex) == 32, "ink vertex layout");

} // namespace

InkNodeItem::InkNodeItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
}

void InkNodeItem::markDirty() { dirty_ = true; update(); }

void InkNodeItem::setModel(BlockModel* m) {
    if (m == model_) return;
    for (auto& c : conns_) QObject::disconnect(c);
    conns_.clear();
    model_ = m;
    if (m) {
        conns_.push_back(connect(m, &BlockModel::inkChanged, this, &InkNodeItem::markDirty));
        conns_.push_back(connect(m, &BlockModel::layoutChangedSpike, this, &InkNodeItem::markDirty));
        conns_.push_back(connect(m, &BlockModel::structureChangedSpike, this, &InkNodeItem::markDirty));
    }
    emit modelChanged();
    markDirty();
}
void InkNodeItem::setRowLo(int v) { if (v == rowLo_) return; rowLo_ = v; emit rangeChanged(); markDirty(); }
void InkNodeItem::setRowHi(int v) { if (v == rowHi_) return; rowHi_ = v; emit rangeChanged(); markDirty(); }
void InkNodeItem::setOriginY(qreal v) { if (v == originY_) return; originY_ = v; emit placementChanged(); markDirty(); }
void InkNodeItem::setLeftEdge(qreal v) { if (v == leftEdge_) return; leftEdge_ = v; emit placementChanged(); markDirty(); }
void InkNodeItem::setPxScale(qreal v) { if (v == pxScale_) return; pxScale_ = v; emit pxScaleChanged(); update(); }

QPointF InkNodeItem::firstPointOf(int row) const {
    if (!model_) return {};
    mn::DocInkAnchor a;
    if (!mn::docInkFromJson(model_->inkForRow(row), a) || a.strokes.empty() || a.strokes[0].points.empty()) return {};
    const mn::InkPlacement pl = mn::inkPlacement(*model_, row, a.space, leftEdge_, model_->pageWidth(), 0.0);
    const QPointF p = a.strokes[0].points[0];
    return QPointF(pl.origin.x() + p.x() * pl.scale.width(), pl.origin.y() - originY_ + p.y() * pl.scale.height());
}

QSGNode* InkNodeItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData*) {
    const double dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    const float px = float(std::max(1e-3, double(pxScale_) * dpr));
    // The feather's room in local px, quantised to octaves so a pan or a small zoom never re-tessellates.
    const double margin = std::pow(2.0, std::ceil(std::log2(std::max(1e-3, 2.0 / px))));
    if (!model_ || rowLo_ < 0 || rowHi_ < rowLo_) { delete old; anchorCount_ = vertexCount_ = 0; return nullptr; }
    QSGNode* root = old ? old : new QSGNode;
    if (dirty_ || margin != builtMargin_) {
        root->removeAllChildNodes();   // children were allocated with OwnedByParent
        int anchors = 0, verts = 0;
        const int n = model_->rowCountQml();
        for (int row = std::max(0, rowLo_); row <= rowHi_ && row < n; ++row) {
            const QString json = model_->inkForRow(row);
            if (json.isEmpty()) continue;
            mn::DocInkAnchor a;
            if (!mn::docInkFromJson(json, a) || a.strokes.empty()) continue;
            const mn::InkPlacement pl = mn::inkPlacement(*model_, row, a.space, leftEdge_, model_->pageWidth(), 0.0);
            mn::ink::Mesh mesh;
            for (const qcv::ActiveStroke& s : a.strokes) mn::ink::tessellateStroke(s, pl.scale, pl.widthScale, margin, mesh);
            if (mesh.empty()) continue;
            auto* t = new QSGTransformNode;
            QMatrix4x4 m; m.translate(float(pl.origin.x()), float(pl.origin.y() - originY_));
            t->setMatrix(m);
            auto* g = new QSGGeometryNode;
            auto* geom = new QSGGeometry(inkAttributes(), int(mesh.verts.size()));
            geom->setDrawingMode(QSGGeometry::DrawTriangles);
            std::memcpy(geom->vertexData(), mesh.verts.data(), mesh.verts.size() * sizeof(mn::ink::Vertex));
            g->setGeometry(geom); g->setFlag(QSGNode::OwnsGeometry);
            auto* mat = new InkMaterial; mat->pxScale = px;
            g->setMaterial(mat); g->setFlag(QSGNode::OwnsMaterial);
            t->appendChildNode(g);
            root->appendChildNode(t);
            ++anchors; verts += int(mesh.verts.size());
        }
        dirty_ = false;
        builtMargin_ = margin;
        if (anchors != anchorCount_ || verts != vertexCount_) { anchorCount_ = anchors; vertexCount_ = verts; emit statsChanged(); }
    } else {
        for (QSGNode* t = root->firstChild(); t; t = t->nextSibling())
            if (QSGNode* g = t->firstChild())
                if (auto* mat = static_cast<InkMaterial*>(static_cast<QSGGeometryNode*>(g)->material()); mat->pxScale != px) {
                    mat->pxScale = px; g->markDirty(QSGNode::DirtyMaterial);
                }
    }
    return root;
}
