#include "InkNodeItem.h"
#include "annotation_serializer.h"
#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGMaterial>
#include <QSGMaterialShader>
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
    // attribute), which this shader's layout doesn't fit — and a host holds one ink node.
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

void InkNodeItem::setEnvelope(const QString& e) { if (e == envelope_) return; envelope_ = e; emit envelopeChanged(); markDirty(); }
void InkNodeItem::setFrameW(qreal v) { if (v == frameW_) return; frameW_ = v; emit frameChanged(); markDirty(); }
void InkNodeItem::setFrameH(qreal v) { if (v == frameH_) return; frameH_ = v; emit frameChanged(); markDirty(); }
void InkNodeItem::setSourceW(qreal v) { if (v == sourceW_) return; sourceW_ = v; emit frameChanged(); markDirty(); }
void InkNodeItem::setSourceH(qreal v) { if (v == sourceH_) return; sourceH_ = v; emit frameChanged(); markDirty(); }
void InkNodeItem::setPxScale(qreal v) { if (v == pxScale_) return; pxScale_ = v; emit pxScaleChanged(); update(); }

QSGNode* InkNodeItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData*) {
    const double dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    const float px = float(std::max(1e-3, double(pxScale_) * dpr));
    // The feather's room in item px, quantised to octaves so a pan or a small zoom never re-tessellates.
    const double margin = std::pow(2.0, std::ceil(std::log2(std::max(1e-3, 2.0 / px))));
    if (envelope_.isEmpty() || frameW_ <= 0 || frameH_ <= 0) { delete old; strokeCount_ = vertexCount_ = 0; return nullptr; }
    auto* node = static_cast<QSGGeometryNode*>(old);
    if (!node || dirty_ || margin != builtMargin_) {
        const std::vector<qcv::ActiveStroke> strokes = qcv::AnnotationSerializer::jsonStringToStrokes(envelope_);
        const double widthScale = frameW_ / std::max<qreal>(1.0, sourceW_ > 0 ? sourceW_ : frameW_);
        mn::ink::Mesh mesh;
        for (const qcv::ActiveStroke& s : strokes) mn::ink::tessellateStroke(s, QSizeF(frameW_, frameH_), widthScale, margin, mesh);
        if (mesh.empty()) { delete old; if (strokeCount_ || vertexCount_) { strokeCount_ = vertexCount_ = 0; emit statsChanged(); } dirty_ = false; builtMargin_ = margin; return nullptr; }
        if (!node) {
            node = new QSGGeometryNode;
            auto* mat = new InkMaterial; mat->pxScale = px;
            node->setMaterial(mat); node->setFlag(QSGNode::OwnsMaterial);
            node->setFlag(QSGNode::OwnsGeometry);
        }
        auto* geom = new QSGGeometry(inkAttributes(), int(mesh.verts.size()));
        geom->setDrawingMode(QSGGeometry::DrawTriangles);
        std::memcpy(geom->vertexData(), mesh.verts.data(), mesh.verts.size() * sizeof(mn::ink::Vertex));
        node->setGeometry(geom);
        node->markDirty(QSGNode::DirtyGeometry);
        dirty_ = false;
        builtMargin_ = margin;
        const int sc = int(strokes.size()), vc = int(mesh.verts.size());
        if (sc != strokeCount_ || vc != vertexCount_) { strokeCount_ = sc; vertexCount_ = vc; emit statsChanged(); }
    }
    if (auto* mat = static_cast<InkMaterial*>(node->material()); mat->pxScale != px) { mat->pxScale = px; node->markDirty(QSGNode::DirtyMaterial); }
    return node;
}
