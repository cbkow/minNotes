#include "CanvasExporter.h"
#include "BlockModel.h"
#include "CanvasDocument.h"
#include "FileLinks.h"
#include "MediaStore.h"
#include "../notes/annotation_thumbnail.h"
#include "../notes/active_stroke.h"
#include <QDir>
#include <QRegularExpression>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSaveFile>
#include <QUrl>
#include <algorithm>
#include <cmath>

namespace {

QString esc(const QString& s) {
    QString o = s;
    o.replace(QLatin1Char('&'), QLatin1String("&amp;")).replace(QLatin1Char('<'), QLatin1String("&lt;"))
     .replace(QLatin1Char('>'), QLatin1String("&gt;")).replace(QLatin1Char('"'), QLatin1String("&quot;"));
    return o;
}
QString px(double v) { return QString::number(std::round(v * 100) / 100, 'f', 2).remove(QRegularExpression(QStringLiteral("\\.?0+$"))) + QStringLiteral("px"); }
QString num(double v) { return QString::number(std::round(v * 100) / 100, 'f', 2).remove(QRegularExpression(QStringLiteral("\\.?0+$"))); }
QString localPathOf(const QString& urlOrPath) { const QUrl u(urlOrPath); return u.isLocalFile() ? u.toLocalFile() : urlOrPath; }

QJsonObject mediaOf(const Document::Element& e) {
    return QJsonDocument::fromJson(e.json.toUtf8()).object().value(QStringLiteral("media")).toObject();
}
QString nameOf(const QJsonObject& md) {
    const QString n = md.value(QStringLiteral("name")).toString();
    if (!n.isEmpty()) return n;
    const QJsonValue s = md.value(QStringLiteral("src"));
    QString path = s.isString() ? s.toString() : QString();
    if (s.isObject()) { const QJsonArray rel = s.toObject().value(QStringLiteral("rel")).toArray(); if (!rel.isEmpty()) path = rel.last().toString(); }
    return path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
}
// A media element's local path (blocking resolve), "" for remote / unresolvable.
QString localPath(const CanvasDocument* c, const QJsonObject& md) {
    const QJsonValue src = md.value(QStringLiteral("src"));
    if (src.isString() && src.toString().startsWith(QLatin1String("http"))) return QString();
    const QString url = c->mediaStore() ? c->mediaStore()->resolveUrl(src) : QString();
    if (url.isEmpty()) return QString();
    const QUrl u(url);
    return u.isLocalFile() ? u.toLocalFile() : QString();
}

// The ink envelope's shapes (element-local px) → qcv strokes with NORMALISED points over (w, h) — what the
// app's painter and tessellator both take.
std::vector<qcv::ActiveStroke> strokesOf(const QString& json, double w, double h) {
    std::vector<qcv::ActiveStroke> out;
    const QJsonArray shapes = QJsonDocument::fromJson(json.toUtf8()).object().value(QStringLiteral("shapes")).toArray();
    for (const QJsonValue& v : shapes) {
        const QJsonObject o = v.toObject();
        qcv::ActiveStroke s;
        const QString t = o.value(QStringLiteral("type")).toString();
        s.tool = t == QLatin1String("freehand") ? qcv::DrawingTool::Freehand : t == QLatin1String("rectangle") || t == QLatin1String("rect") ? qcv::DrawingTool::Rectangle
               : t == QLatin1String("oval") ? qcv::DrawingTool::Oval : t == QLatin1String("arrow") ? qcv::DrawingTool::Arrow : t == QLatin1String("line") ? qcv::DrawingTool::Line : qcv::DrawingTool::None;
        if (s.tool == qcv::DrawingTool::None) continue;
        const QJsonArray c = o.value(QStringLiteral("color")).toArray();
        if (c.size() >= 3) s.color = QColor::fromRgbF(c.at(0).toDouble(), c.at(1).toDouble(), c.at(2).toDouble(), c.size() > 3 ? c.at(3).toDouble() : 1.0);
        s.strokeWidth = float(o.value(QStringLiteral("stroke_width")).toDouble(4.0));
        s.filled = o.value(QStringLiteral("filled")).toBool(false);
        for (const QJsonValue& pv : o.value(QStringLiteral("points")).toArray()) {
            const QJsonArray p = pv.toArray();
            if (p.size() >= 2) s.points.push_back(QPointF(p.at(0).toDouble() / (w > 0 ? w : 1), p.at(1).toDouble() / (h > 0 ? h : 1)));
        }
        if (!s.points.empty()) out.push_back(s);
    }
    return out;
}
QString cssColor(const QColor& c) { return c.alphaF() < 0.999 ? QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(num(c.alphaF())) : c.name(); }

// The strokes as SVG, element-local px (the painter's geometry, including the 25° arrow head).
QString inkSvg(const std::vector<qcv::ActiveStroke>& strokes, double w, double h) {
    QString out;
    auto P = [&](const QPointF& n) { return QPointF(n.x() * w, n.y() * h); };
    for (const qcv::ActiveStroke& s : strokes) {
        const double penW = std::max(1.0, double(s.strokeWidth));
        const QString stroke = cssColor(s.color);
        const QString common = QStringLiteral(" stroke=\"%1\" stroke-width=\"%2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"").arg(stroke, num(penW));
        switch (s.tool) {
        case qcv::DrawingTool::Freehand: {
            if (s.points.size() == 1) { const QPointF c = P(s.points[0]); out += QStringLiteral("<circle cx=\"%1\" cy=\"%2\" r=\"%3\" fill=\"%4\"/>").arg(num(c.x()), num(c.y()), num(penW / 2), stroke); break; }
            QString d;
            for (size_t i = 0; i < s.points.size(); ++i) { const QPointF p = P(s.points[i]); d += (i == 0 ? QStringLiteral("M") : QStringLiteral("L")) + num(p.x()) + QLatin1Char(' ') + num(p.y()); }
            out += QStringLiteral("<path d=\"%1\" fill=\"none\"%2/>").arg(d, common);
            break;
        }
        case qcv::DrawingTool::Line: {
            if (s.points.size() < 2) break;
            const QPointF a = P(s.points[0]), b = P(s.points[1]);
            out += QStringLiteral("<line x1=\"%1\" y1=\"%2\" x2=\"%3\" y2=\"%4\"%5/>").arg(num(a.x()), num(a.y()), num(b.x()), num(b.y()), common);
            break;
        }
        case qcv::DrawingTool::Rectangle: {
            if (s.points.size() < 4) break;
            const QRectF r = QRectF(P(s.points[0]), P(s.points[2])).normalized();
            out += QStringLiteral("<rect x=\"%1\" y=\"%2\" width=\"%3\" height=\"%4\" fill=\"%5\"%6/>")
                       .arg(num(r.x()), num(r.y()), num(r.width()), num(r.height()), s.filled ? stroke : QStringLiteral("none"), s.filled ? QString() : common);
            break;
        }
        case qcv::DrawingTool::Oval: {
            if (s.points.size() < 2) break;
            const QPointF c = P(s.points[0]);
            out += QStringLiteral("<ellipse cx=\"%1\" cy=\"%2\" rx=\"%3\" ry=\"%4\" fill=\"%5\"%6/>")
                       .arg(num(c.x()), num(c.y()), num(s.points[1].x() * w), num(s.points[1].y() * h), s.filled ? stroke : QStringLiteral("none"), s.filled ? QString() : common);
            break;
        }
        case qcv::DrawingTool::Arrow: {
            if (s.points.size() < 2) break;
            const QPointF a = P(s.points[0]), b = P(s.points[1]);
            double dx = b.x() - a.x(), dy = b.y() - a.y();
            const double len = std::sqrt(dx * dx + dy * dy);
            if (len < 0.001) break;
            dx /= len; dy /= len;
            const double arrowSize = 4.5 + penW * 2.3, cosA = std::cos(0.436332), sinA = std::sin(0.436332);
            const QPointF e(b.x() - dx * arrowSize * cosA, b.y() - dy * arrowSize * cosA);
            const QPointF a1(b.x() - arrowSize * (dx * cosA - dy * sinA), b.y() - arrowSize * (dy * cosA + dx * sinA));
            const QPointF a2(b.x() - arrowSize * (dx * cosA + dy * sinA), b.y() - arrowSize * (dy * cosA - dx * sinA));
            out += QStringLiteral("<line x1=\"%1\" y1=\"%2\" x2=\"%3\" y2=\"%4\"%5/>").arg(num(a.x()), num(a.y()), num(e.x()), num(e.y()), common);
            out += QStringLiteral("<polygon points=\"%1 %2 %3 %4 %5 %6\" fill=\"%7\"/>").arg(num(b.x()), num(b.y()), num(a1.x()), num(a1.y()), num(a2.x()), num(a2.y()), stroke);
            break;
        }
        default: break;
        }
    }
    return out;
}

const char* kCanvasCss = R"CSS(
html,body{height:100%;margin:0;overflow:hidden;background:var(--desk);background-image:none}
#mn-canvas{position:fixed;inset:0;overflow:hidden;cursor:grab;touch-action:none}
#mn-canvas.panning{cursor:grabbing}
#world{position:absolute;left:0;top:0;transform-origin:0 0}
.el{position:absolute;box-sizing:border-box}
.page{background:var(--bg);border:1px solid var(--border);overflow:hidden}
.page main{max-width:none;margin:0;width:100%;box-sizing:border-box;padding:24px}
.page .more{position:absolute;left:0;right:0;bottom:0;height:72px;padding:0 24px 20px 0;display:flex;align-items:flex-end;justify-content:flex-end;
background:linear-gradient(to bottom,rgba(24,24,23,0),var(--bg) 60%);color:var(--subtle);font-size:13px}
figure.el{margin:0;background:var(--bg);border:1px solid var(--border);overflow:hidden;display:flex;flex-direction:column}
figure.el img{display:block;width:100%;height:auto;flex:0 0 auto}
figure.el .poster{width:100%;flex:1 1 auto;object-fit:contain;background:#000}
figure.el figcaption,.card .cap{padding:8px 12px;font-size:13px;color:var(--muted);line-height:1.4}
.name{color:var(--text);font-size:14px;display:block}
.meta{color:var(--muted);font-size:13px;display:block}
.path{display:block;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12px;color:var(--subtle);word-break:break-all}
a.ufblink{display:block;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12px;color:var(--chiptext);word-break:break-all;text-decoration:none}
.card{background:var(--bg);border:1px solid var(--border);display:flex;align-items:center;gap:12px;padding:0 12px}
.card .glyph{flex:0 0 24px;width:24px;height:24px;color:var(--muted)}
.card .cap{padding:0;min-width:0;flex:1 1 auto}
.card .name,.card .path,.card .meta{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
a.bookmark{color:inherit;text-decoration:none}
svg.el{overflow:visible}
.text{white-space:pre-wrap;color:var(--text);font-family:'Aspekta',system-ui,sans-serif;line-height:1.4}
#mn-hud{position:fixed;right:12px;bottom:12px;font:12px ui-monospace,SFMono-Regular,Menlo,monospace;color:var(--muted);background:var(--bg);border:1px solid var(--border);padding:4px 8px;user-select:none}
#mn-hud b{color:var(--text);font-weight:600;cursor:pointer}
@media print{#mn-hud{display:none}}
)CSS";

const char* kCanvasScript = R"JS(<script>
(function(){
var v=document.getElementById('mn-canvas'),w=document.getElementById('world'),hud=document.getElementById('mn-hud');
var W=+w.dataset.w,H=+w.dataset.h,z=1,px=0,py=0;
function apply(){w.style.transform='translate('+px+'px,'+py+'px) scale('+z+')';hud.firstChild.textContent=Math.round(z*100)+'% ';}
function fit(){var pad=48,zx=(v.clientWidth-2*pad)/W,zy=(v.clientHeight-2*pad)/H;z=Math.max(0.05,Math.min(4,Math.min(zx,zy)));px=(v.clientWidth-W*z)/2;py=(v.clientHeight-H*z)/2;apply();}
function zoomAt(nz,x,y){nz=Math.max(0.05,Math.min(4,nz));px=x-(x-px)*nz/z;py=y-(y-py)*nz/z;z=nz;apply();}
v.addEventListener('wheel',function(e){e.preventDefault();var r=v.getBoundingClientRect(),x=e.clientX-r.left,y=e.clientY-r.top;
if(e.ctrlKey||e.metaKey){zoomAt(z*Math.pow(2,-e.deltaY/400),x,y);}else{px-=e.deltaX;py-=e.deltaY;apply();}},{passive:false});
var drag=null;
v.addEventListener('pointerdown',function(e){if(e.target.closest('a'))return;drag={x:e.clientX-px,y:e.clientY-py};v.classList.add('panning');v.setPointerCapture(e.pointerId);});
v.addEventListener('pointermove',function(e){if(!drag)return;px=e.clientX-drag.x;py=e.clientY-drag.y;apply();});
v.addEventListener('pointerup',function(){drag=null;v.classList.remove('panning');});
document.addEventListener('keydown',function(e){if(e.target.tagName==='INPUT')return;var cx=v.clientWidth/2,cy=v.clientHeight/2;
if(e.key==='+'||e.key==='=')zoomAt(z*1.25,cx,cy);else if(e.key==='-')zoomAt(z/1.25,cx,cy);else if(e.key==='0')fit();else if(e.key==='1')zoomAt(1,cx,cy);else return;e.preventDefault();});
hud.querySelector('b').addEventListener('click',fit);
window.addEventListener('resize',fit);fit();
})();
</script>
)JS";

} // namespace

QString CanvasExporter::toHtml(const Options& opt, Exporter::AssetSink& sink) const {
    if (!canvas_) return {};
    CanvasDocument* c = canvas_;
    const QRectF b = c->canvasBounds();
    const double ox = -b.x() + 24, oy = -b.y() + 24;            // the world starts at the bounds' top-left (+ a margin)
    const double W = std::max(1.0, b.width() + 48), H = std::max(1.0, b.height() + 48);
    QString body;
    body += QStringLiteral("<div id=\"mn-canvas\"><div id=\"world\" data-w=\"%1\" data-h=\"%2\" style=\"width:%3;height:%4\">\n").arg(num(W), num(H), px(W), px(H));
    Exporter::Options popt; popt.includeVideoNotes = opt.includeVideoNotes; popt.ufbLinks = opt.ufbLinks;
    auto pathLines = [&](const QString& path) {
        if (path.isEmpty()) return QString();
        QString s = QStringLiteral("<span class=\"path\">%1</span>").arg(esc(QDir::toNativeSeparators(path)));
        if (opt.ufbLinks) s += QStringLiteral("<a class=\"ufblink\" href=\"%1\">%1</a>").arg(esc(mn::ufbLinkFor(path)));
        return s;
    };
    auto style = [&](const Document::Element& e) {
        return QStringLiteral("left:%1;top:%2;width:%3;height:%4").arg(px(e.x + ox), px(e.y + oy), px(e.w), px(e.h));
    };
    for (const Document::Element& e : c->elements()) {   // z order
        if (e.kind == QLatin1String("page")) {
            BlockModel* m = c->pageModel(e.id);
            if (!m) continue;
            const double measure = std::max(320.0, e.w - 48);
            m->setContentWidth(measure);
            Exporter ex; ex.setModel(m);
            const QString frag = ex.toHtml(popt, sink, -1, -1, /*fragment*/true);
            // The card's cap: rows whose top lies inside the element's height; the rest hide under a note.
            const int n = m->rowCountQml();
            int shown = n;
            if (n > 0 && m->totalHeight() + 48 > e.h + 1) { shown = std::clamp(m->rowForY(std::max(0.0, e.h - 48)), 0, n); }
            const int below = std::max(0, n - shown);
            body += QStringLiteral("<section class=\"el page\" id=\"p-%1\" style=\"%2\"><main><header class=\"doctitle\">%3</header>\n%4</main>%5</section>\n")
                        .arg(esc(e.id), style(e), esc(c->pageTitle(e.id)), frag,
                             below > 0 ? QStringLiteral("<div class=\"more\">%1 more block%2</div>").arg(below).arg(below == 1 ? QString() : QStringLiteral("s")) : QString());
        } else if (e.kind == QLatin1String("media")) {
            const QJsonObject md = mediaOf(e);
            const QString kind = md.value(QStringLiteral("kind")).toString(QStringLiteral("image"));
            const QString name = nameOf(md);
            const QString path = localPath(c, md);
            const QString src = md.value(QStringLiteral("src")).isString() ? md.value(QStringLiteral("src")).toString() : QString();
            if (kind == QLatin1String("image")) {
                QString img = src.startsWith(QLatin1String("http")) ? src : (path.isEmpty() ? QString() : sink.addFile(path, name.isEmpty() ? QStringLiteral("image") : name));
                body += QStringLiteral("<figure class=\"el media image\" style=\"%1\">%2</figure>\n")
                            .arg(style(e), img.isEmpty() ? QStringLiteral("<figcaption>%1</figcaption>").arg(esc(name)) : QStringLiteral("<img src=\"%1\" alt=\"%2\">").arg(esc(img), esc(name)));
            } else if (kind == QLatin1String("video") || kind == QLatin1String("pdf")) {
                QImage poster;
                if (!path.isEmpty()) poster = kind == QLatin1String("video") ? MediaStore::extractFrame(path, 0, 960) : MediaStore::renderPdfPage(path, 0, 960);
                const QString pimg = poster.isNull() ? QString() : sink.addImage(poster, name + QStringLiteral("-poster"));
                const QString meta = kind == QLatin1String("video")
                    ? QStringLiteral("%1 × %2 · %3 fps").arg(md.value(QStringLiteral("w")).toInt()).arg(md.value(QStringLiteral("h")).toInt()).arg(num(md.value(QStringLiteral("fps")).toDouble()))
                    : QStringLiteral("%1 page%2").arg(md.value(QStringLiteral("pages")).toInt()).arg(md.value(QStringLiteral("pages")).toInt() == 1 ? QString() : QStringLiteral("s"));
                body += QStringLiteral("<figure class=\"el media %1\" style=\"%2\">%3<figcaption><span class=\"name\">%4</span><span class=\"meta\">%5</span>%6</figcaption></figure>\n")
                            .arg(kind, style(e), pimg.isEmpty() ? QString() : QStringLiteral("<img class=\"poster\" src=\"%1\" alt=\"%2\">").arg(esc(pimg), esc(name)), esc(name), meta, pathLines(path));
            } else if (kind == QLatin1String("bookmark")) {
                const QString title = md.value(QStringLiteral("title")).toString(), host = md.value(QStringLiteral("host")).toString();
                body += QStringLiteral("<a class=\"el card bookmark\" href=\"%1\" style=\"%2\"><span class=\"glyph\">&#x1F310;</span><span class=\"cap\"><span class=\"name\">%3</span><span class=\"meta\">%4</span><span class=\"path\">%1</span></span></a>\n")
                            .arg(esc(src), style(e), esc(title.isEmpty() ? host : title), esc(host));
            } else if (kind == QLatin1String("folder")) {
                const int count = md.value(QStringLiteral("count")).toInt();
                QStringList entries; for (const QJsonValue& v : md.value(QStringLiteral("entries")).toArray()) entries << v.toString();
                const QString meta = QStringLiteral("%1 item%2%3").arg(count).arg(count == 1 ? QString() : QStringLiteral("s"), entries.isEmpty() ? QString() : QStringLiteral(" · ") + esc(entries.join(QStringLiteral(", "))));
                body += QStringLiteral("<div class=\"el card folder\" style=\"%1\"><span class=\"glyph\">&#x1F4C1;</span><span class=\"cap\"><span class=\"name\">%2</span><span class=\"meta\">%3</span>%4</span></div>\n")
                            .arg(style(e), esc(name), meta, pathLines(path));
            } else if (kind == QLatin1String("audio")) {
                const double ms = md.value(QStringLiteral("durMs")).toDouble();
                const int sec = int(std::round(ms / 1000)), mm = sec / 60, ss = sec % 60;
                body += QStringLiteral("<div class=\"el card audio\" style=\"%1\"><span class=\"glyph\">&#x1F50A;</span><span class=\"cap\"><span class=\"name\">%2</span><span class=\"meta\">%3:%4</span>%5</span></div>\n")
                            .arg(style(e), esc(name)).arg(mm).arg(ss, 2, 10, QLatin1Char('0')).arg(pathLines(path));
            } else {   // file chip (and any unknown kind)
                body += QStringLiteral("<div class=\"el card file\" style=\"%1\"><span class=\"glyph\">&#x1F4C4;</span><span class=\"cap\"><span class=\"name\">%2</span>%3</span></div>\n")
                            .arg(style(e), esc(name), pathLines(path));
            }
        } else if (e.kind == QLatin1String("ink")) {
            const auto strokes = strokesOf(e.json, e.w, e.h);
            body += QStringLiteral("<svg class=\"el ink\" style=\"%1\" viewBox=\"0 0 %2 %3\" xmlns=\"http://www.w3.org/2000/svg\">%4</svg>\n")
                        .arg(style(e), num(e.w), num(e.h), inkSvg(strokes, e.w, e.h));
        } else if (e.kind == QLatin1String("shape")) {
            const QJsonObject o = QJsonDocument::fromJson(e.json.toUtf8()).object();
            if (o.value(QStringLiteral("shape")).toString() == QLatin1String("text")) {
                const double size = o.value(QStringLiteral("fontSize")).toDouble(24);
                const QString fill = o.value(QStringLiteral("fill")).toString();
                body += QStringLiteral("<div class=\"el text\" style=\"%1;font-size:%2;padding:%3%4\">%5</div>\n")
                            .arg(style(e), px(size), px(std::round(size * 0.3)), fill.isEmpty() ? QString() : QStringLiteral(";color:%1").arg(esc(fill)), esc(o.value(QStringLiteral("text")).toString()));
            }
        }
        // connectors: reserved; nothing is emitted until their renderer lands
    }
    body += QStringLiteral("</div></div>\n<div id=\"mn-hud\">100% <b title=\"Fit (0)\">fit</b> · drag to pan · ⌘/ctrl-wheel to zoom</div>\n");
    return QStringLiteral("<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
                          "<meta name=\"generator\" content=\"minNotes\">\n<title>%1</title>\n<style>%2%3</style>\n</head>\n<body class=\"canvas\">\n%4%5</body>\n</html>\n")
        .arg(esc(c->documentName()), Exporter::pageCss(), QLatin1String(kCanvasCss), body, QLatin1String(kCanvasScript));
}

QImage CanvasExporter::toPoster(const Options& opt) const {
    if (!canvas_) return {};
    CanvasDocument* c = canvas_;
    const QRectF b = c->canvasBounds();
    const double W = std::max(1.0, b.width() + 48), H = std::max(1.0, b.height() + 48);
    const int pw = std::max(64, opt.posterWidth);
    const double s = pw / W;                                  // poster px per world px
    const int ph = std::max(1, int(std::round(H * s)));
    QImage img(pw, ph, QImage::Format_ARGB32_Premultiplied);
    img.fill(QColor(0x12, 0x12, 0x11));                       // the desk
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QColor surface(0x18, 0x18, 0x17), border(0x2a, 0x2a, 0x2a), text(0xe4, 0xe3, 0xe2), muted(0x8a, 0x8a, 0x8a), bar(0x33, 0x33, 0x33);
    auto rectOf = [&](const Document::Element& e) { return QRectF((e.x - b.x() + 24) * s, (e.y - b.y() + 24) * s, e.w * s, e.h * s); };
    for (const Document::Element& e : c->elements()) {
        const QRectF r = rectOf(e);
        if (e.kind == QLatin1String("page")) {
            p.fillRect(r, surface);
            p.setPen(QPen(border, 1)); p.setBrush(Qt::NoBrush); p.drawRect(r);
            BlockModel* m = c->pageModel(e.id);
            const double inset = 24 * s;
            QFont tf(QStringLiteral("Aspekta")); tf.setPixelSize(std::max(4, int(std::round(28 * s)))); tf.setBold(true);
            p.setFont(tf); p.setPen(text);
            const QString title = c->pageTitle(e.id);
            const QRectF tr(r.x() + inset, r.y() + inset, r.width() - 2 * inset, 40 * s);
            if (tf.pixelSize() >= 6) p.drawText(tr, Qt::AlignLeft | Qt::AlignTop, QFontMetricsF(tf).elidedText(title, Qt::ElideRight, tr.width()));
            if (m) {   // greek bars: one per row inside the card, as wide as its text (capped)
                p.setClipRect(r.adjusted(1, 1, -1, -1));
                const int n = m->rowCountQml();
                for (int row = 1; row < n; ++row) {
                    const double y = m->yForRow(row), hgt = std::clamp(m->heightForRow(row) * 0.45, 2.0, 12.0);   // a text line's worth, never a slab (tall media / cell rows)
                    if (y + 24 > e.h) break;
                    const double len = std::min(1.0, 0.25 + std::min<qsizetype>(60, m->contentForRow(row).size()) / 80.0);
                    p.fillRect(QRectF(r.x() + inset, r.y() + (y + 24) * s + hgt * s * 0.5, (e.w - 48) * len * s, std::max(1.0, hgt * s)), bar);
                }
                p.setClipping(false);
            }
        } else if (e.kind == QLatin1String("media")) {
            const QJsonObject md = mediaOf(e);
            const QString kind = md.value(QStringLiteral("kind")).toString(QStringLiteral("image"));
            const QString path = localPath(c, md);
            QImage thumb;
            if (!path.isEmpty()) {
                if (kind == QLatin1String("image")) thumb.load(path);
                else if (kind == QLatin1String("video")) thumb = MediaStore::extractFrame(path, 0, int(r.width()) + 1);
                else if (kind == QLatin1String("pdf")) thumb = MediaStore::renderPdfPage(path, 0, int(r.width()) + 1);
            }
            p.fillRect(r, surface);
            if (!thumb.isNull()) {
                QRectF dst = r;
                const double ar = thumb.width() / double(std::max(1, thumb.height()));
                if (dst.width() / dst.height() > ar) { const double nw = dst.height() * ar; dst = QRectF(dst.x() + (dst.width() - nw) / 2, dst.y(), nw, dst.height()); }
                else { const double nh = dst.width() / ar; dst = QRectF(dst.x(), dst.y() + (dst.height() - nh) / 2, dst.width(), nh); }
                p.drawImage(dst, thumb);
            } else {
                QFont f(QStringLiteral("Aspekta")); f.setPixelSize(std::max(4, int(std::round(14 * s))));
                p.setFont(f); p.setPen(text);
                if (f.pixelSize() >= 6) p.drawText(r.adjusted(12 * s, 0, -12 * s, 0), Qt::AlignLeft | Qt::AlignVCenter, QFontMetricsF(f).elidedText(nameOf(md), Qt::ElideMiddle, r.width() - 24 * s));
            }
            p.setPen(QPen(border, 1)); p.setBrush(Qt::NoBrush); p.drawRect(r);
        } else if (e.kind == QLatin1String("ink")) {
            p.save(); p.translate(r.topLeft());
            for (const qcv::ActiveStroke& st : strokesOf(e.json, e.w, e.h)) qcv::paintStroke(p, st, r.width(), r.height(), s);
            p.restore();
        } else if (e.kind == QLatin1String("shape")) {
            const QJsonObject o = QJsonDocument::fromJson(e.json.toUtf8()).object();
            if (o.value(QStringLiteral("shape")).toString() == QLatin1String("text")) {
                QFont f(QStringLiteral("Aspekta")); f.setPixelSize(std::max(2, int(std::round(o.value(QStringLiteral("fontSize")).toDouble(24) * s))));
                const QColor col(o.value(QStringLiteral("fill")).toString());
                p.setFont(f); p.setPen(col.isValid() ? col : text);
                if (f.pixelSize() >= 4) p.drawText(r.adjusted(4 * s, 4 * s, -4 * s, -4 * s), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, o.value(QStringLiteral("text")).toString());
            }
        }
    }
    p.end();
    Q_UNUSED(muted);
    return img;
}

QVariantMap CanvasExporter::scan() const {
    QVariantMap m;
    int pages = 0, media = 0, videos = 0, pdfs = 0, ink = 0, shapes = 0, refPaths = 0;
    if (canvas_) for (const Document::Element& e : canvas_->elements()) {
        if (e.kind == QLatin1String("page")) ++pages;
        else if (e.kind == QLatin1String("ink")) ++ink;
        else if (e.kind == QLatin1String("shape")) ++shapes;
        else if (e.kind == QLatin1String("media")) {
            ++media;
            const QString k = mediaOf(e).value(QStringLiteral("kind")).toString(QStringLiteral("image"));
            if (k == QLatin1String("video")) ++videos; else if (k == QLatin1String("pdf")) ++pdfs;
            if (k != QLatin1String("image") && k != QLatin1String("bookmark")) ++refPaths;
        }
    }
    m.insert(QStringLiteral("pages"), pages); m.insert(QStringLiteral("media"), media); m.insert(QStringLiteral("videos"), videos);
    m.insert(QStringLiteral("pdfs"), pdfs); m.insert(QStringLiteral("ink"), ink); m.insert(QStringLiteral("shapes"), shapes); m.insert(QStringLiteral("refPaths"), refPaths);
    return m;
}

bool CanvasExporter::fail(const QString& why) { lastError_ = why; emit lastErrorChanged(); return false; }

bool CanvasExporter::exportHtml(const QString& fileUrlOrPath, bool includeVideoNotes, bool ufbLinks) {
    if (!canvas_) return fail(QStringLiteral("no canvas"));
    const QString path = localPathOf(fileUrlOrPath);
    if (path.isEmpty()) return fail(QStringLiteral("no path"));
    Options opt; opt.includeVideoNotes = includeVideoNotes; opt.ufbLinks = ufbLinks;
    Exporter::DataUriSink sink;
    const QString html = toHtml(opt, sink);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return fail(QStringLiteral("%1 — %2").arg(QDir::toNativeSeparators(path), f.errorString()));
    f.write(html.toUtf8());
    if (!f.commit()) return fail(QStringLiteral("%1 — %2").arg(QDir::toNativeSeparators(path), f.errorString()));
    return true;
}

bool CanvasExporter::exportPoster(const QString& fileUrlOrPath, int width) {
    if (!canvas_) return fail(QStringLiteral("no canvas"));
    const QString path = localPathOf(fileUrlOrPath);
    if (path.isEmpty()) return fail(QStringLiteral("no path"));
    Options opt; opt.posterWidth = width;
    const QImage img = toPoster(opt);
    if (img.isNull() || !img.save(path)) return fail(QStringLiteral("%1 — poster not written").arg(QDir::toNativeSeparators(path)));
    return true;
}
