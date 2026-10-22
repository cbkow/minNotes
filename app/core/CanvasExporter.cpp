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
#include <QImageReader>
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
// A text chip's look (CanvasShapeItem's rule): no fill / "transparent" = PLAIN text in its own colour (or the
// theme's text); a fill = a box with contrast glyphs (black over a light fill, white over a dark one).
struct TextChipLook { bool plain = true; QColor fill; QColor text; };
TextChipLook textChipLook(const QJsonObject& o) {
    TextChipLook l;
    const QString fill = o.value(QStringLiteral("fill")).toString();
    l.plain = fill.isEmpty() || fill == QLatin1String("transparent") || !QColor::isValidColorName(fill);
    if (l.plain) { const QString c = o.value(QStringLiteral("color")).toString(); if (QColor::isValidColorName(c)) l.text = QColor(c); return l; }
    l.fill = QColor(fill);
    l.text = (l.fill.redF() * 0.299 + l.fill.greenF() * 0.587 + l.fill.blueF() * 0.114) > 0.6 ? QColor(Qt::black) : QColor(Qt::white);
    return l;
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

// The canvas export's own chrome (2026-10-18 redesign): a 30-px tab strip over the world — "Canvas" plus a tab per
// opened element — and a READER below it that hosts ONE DOM copy of each page (moved out of its card and back) or a
// media element's reader main (moved out of #mn-store). House style: radius 0, 13-px chrome, the strip a half-step
// darker than the field, the active tab in the selection grey.
const char* kCanvasCss = R"CSS(
html,body{height:100%;margin:0;overflow:hidden;background:var(--bg)}
.el.frame{--tint:#8a8a8a;box-sizing:border-box;border-radius:6px;border:1px solid color-mix(in srgb,var(--tint) 35%,transparent);background:color-mix(in srgb,var(--tint) 5%,transparent)}
.el.frame.tinted{border-color:color-mix(in srgb,var(--tint) 50%,transparent);background:color-mix(in srgb,var(--tint) 10%,transparent)}
.el.frame .ftitle{height:28px;padding:0 10px;font:600 13px/28px Inter,Aspekta,-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;color:var(--tint);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;background:color-mix(in srgb,var(--tint) 6%,transparent);border-radius:6px 6px 0 0}
#mn-tabs{position:fixed;top:0;left:0;right:0;height:30px;z-index:5;display:flex;align-items:stretch;background:#151514;border-bottom:1px solid var(--border);
font:13px/1 Inter,Aspekta,-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;user-select:none}
#mn-tabs .tabs{display:flex;align-items:stretch;flex:1 1 auto;min-width:0;overflow:hidden}
#mn-tabs .tab{display:flex;align-items:center;gap:8px;max-width:260px;padding:0 12px;border:0;border-right:1px solid var(--border);background:transparent;color:var(--muted);font:inherit;cursor:pointer;white-space:nowrap}
#mn-tabs .tab span{overflow:hidden;text-overflow:ellipsis}
#mn-tabs .tab:hover{color:var(--text)}
#mn-tabs .tab.on{background:var(--divider);color:var(--bright)}
#mn-tabs .tab .x{display:inline-flex;align-items:center;justify-content:center;width:16px;height:16px;color:var(--subtle);font-size:14px;line-height:1}
#mn-tabs .tab .x:hover{color:var(--bright);background:var(--border)}
#mn-tabs .tab,#mn-tabs .tab .x{transition:background-color 100ms,color 100ms}
#mn-tabs .tab .x{border-radius:var(--rs)}
#mn-tabs .end{display:flex;align-items:center;gap:14px;padding:0 12px;color:var(--muted);flex:none}
#mn-hud{font:12px ui-monospace,SFMono-Regular,Menlo,monospace;color:var(--muted)}
#mn-hud b{color:var(--text);font-weight:600;cursor:pointer}
body.reading #mn-hud{display:none}
#mn-canvas{position:fixed;left:0;right:0;top:30px;bottom:0;overflow:hidden;cursor:grab;touch-action:none}
#mn-canvas.panning{cursor:grabbing}
#mn-canvas[hidden]{display:none}
#world{position:absolute;left:0;top:0;transform-origin:0 0}
.el{position:absolute;box-sizing:border-box}
.el[data-open]{cursor:pointer}
.page{background:var(--bg);border:1px solid var(--border);border-radius:var(--r);overflow:hidden}
#world .page main{max-width:none;margin:0;width:100%;box-sizing:border-box;padding:24px}
#world .page main .cap{display:none}
.page .more{position:absolute;left:0;right:0;bottom:0;height:72px;padding:0 24px 20px 0;display:flex;align-items:flex-end;justify-content:flex-end;
background:linear-gradient(to bottom,rgba(24,24,23,0),var(--bg) 60%);color:var(--subtle);font-size:13px;pointer-events:none}
figure.el{margin:0;background:var(--bg);border:1px solid var(--border);border-radius:var(--r);overflow:hidden;display:flex;flex-direction:column}
figure.el img{display:block;width:100%;height:auto;flex:0 0 auto}
figure.el.cover img,figure.el.cover .poster{height:100%;object-fit:cover}
figure.el .poster{width:100%;flex:1 1 auto;object-fit:contain;background:#000;min-height:0}
figure.el.pdf .poster{background:#fff}
figure.el .inkwrap{position:relative;flex:1 1 auto;min-height:0;display:flex}
figure.el .inkwrap .ink{position:absolute;left:0;top:0;width:100%;height:100%;object-fit:contain}
figure.el.image img{cursor:zoom-in}
figure.el figcaption,.card .cap{padding:8px 12px;font-size:13px;color:var(--muted);line-height:1.4}
.name{color:var(--text);font-size:14px;display:block}
.meta{color:var(--muted);font-size:13px;display:block}
.path{display:block;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12px;color:var(--subtle);word-break:break-all}
a.ufblink{display:block;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12px;color:var(--chiptext);word-break:break-all;text-decoration:none}
.card{background:var(--bg);border:1px solid var(--border);border-radius:var(--r);display:flex;align-items:center;gap:12px;padding:0 12px}
.card .glyph{flex:0 0 24px;width:24px;height:24px;color:var(--muted)}
.card .cap{padding:0;min-width:0;flex:1 1 auto}
.card .name,.card .path,.card .meta{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
a.bookmark{color:inherit;text-decoration:none}
svg.el{overflow:visible}
.text{white-space:pre-wrap;color:var(--text);font-family:'Aspekta',system-ui,sans-serif;line-height:1.4}
.text.auto{white-space:pre}
#mn-reader{position:fixed;left:0;right:0;top:30px;bottom:0;overflow:auto;background:var(--bg)}
#mn-reader[hidden],#mn-store{display:none}
#mn-reader main{max-width:none;margin:0;padding:48px 48px 96px}
html.cap #mn-reader main{max-width:856px;margin:0 auto}
#mn-reader main.reader figure.ref img{max-width:100%}
#mn-reader main.reader.pdf figure.ref img:not(.ink){background:#fff}
#mn-reader main.reader .vnotes{margin-top:24px}
@media print{#mn-tabs{display:none}}
)CSS";

// Pan/zoom over the world (⌘/ctrl-wheel zooms about the pointer, wheel pans, drag pans; + − 0 1 keys), the tab strip
// (a click on an openable element — never a drag — opens its tab; × closes; Escape returns to the canvas), the reader
// (one DOM copy per page: its <main> moves from the card into the reader and back), hash routing (#canvas / #pm-<id>
// / #m-<id>) and the pages' "Readable width" cap (html.cap, remembered per browser).
const char* kCanvasScript = R"JS(<script>
(function(){
var v=document.getElementById('mn-canvas'),w=document.getElementById('world'),hud=document.getElementById('mn-hud');
var rd=document.getElementById('mn-reader'),st=document.getElementById('mn-store'),tabs=document.querySelector('#mn-tabs .tabs');
var W=+w.dataset.w,H=+w.dataset.h,z=1,px=0,py=0;
function apply(){w.style.transform='translate('+px+'px,'+py+'px) scale('+z+')';hud.firstChild.textContent=Math.round(z*100)+'% ';}
var needFit=false;
function fit(){if(v.hidden){needFit=true;return;}needFit=false;var pad=48,zx=(v.clientWidth-2*pad)/W,zy=(v.clientHeight-2*pad)/H;z=Math.max(0.05,Math.min(4,Math.min(zx,zy)));px=(v.clientWidth-W*z)/2;py=(v.clientHeight-H*z)/2;apply();}
function zoomAt(nz,x,y){nz=Math.max(0.05,Math.min(4,nz));px=x-(x-px)*nz/z;py=y-(y-py)*nz/z;z=nz;apply();}
v.addEventListener('wheel',function(e){e.preventDefault();var r=v.getBoundingClientRect(),x=e.clientX-r.left,y=e.clientY-r.top;
if(e.ctrlKey||e.metaKey){zoomAt(z*Math.pow(2,-e.deltaY/400),x,y);}else{px-=e.deltaX;py-=e.deltaY;apply();}},{passive:false});
var drag=null;
v.addEventListener('pointerdown',function(e){if(e.target.closest('a'))return;drag={x:e.clientX-px,y:e.clientY-py,sx:e.clientX,sy:e.clientY,t:e.target,cap:false};});
v.addEventListener('pointermove',function(e){if(!drag)return;if(!drag.cap){if(Math.abs(e.clientX-drag.sx)<=4&&Math.abs(e.clientY-drag.sy)<=4)return;drag.cap=true;v.classList.add('panning');v.setPointerCapture(e.pointerId);}px=e.clientX-drag.x;py=e.clientY-drag.y;apply();});
v.addEventListener('pointerup',function(e){if(!drag)return;var d=drag;drag=null;v.classList.remove('panning');
if(d.cap)return;
if(d.t.tagName==='IMG'&&d.t.className.indexOf('poster')<0)return;
if(d.t.closest('.cb[data-task]'))return;
var o=d.t.closest('[data-open]');if(o)show(o.dataset.open);});
document.addEventListener('keydown',function(e){if(e.target.tagName==='INPUT'||document.querySelector('#mn-lb.open'))return;
if(e.key==='Escape'){if(cur!=='canvas'){show('canvas');e.preventDefault();}return;}
if(cur!=='canvas')return;var cx=v.clientWidth/2,cy=v.clientHeight/2;
if(e.key==='+'||e.key==='=')zoomAt(z*1.25,cx,cy);else if(e.key==='-')zoomAt(z/1.25,cx,cy);else if(e.key==='0')fit();else if(e.key==='1')zoomAt(1,cx,cy);else return;e.preventDefault();});
hud.querySelector('b').addEventListener('click',fit);
window.addEventListener('resize',fit);fit();
var cur='canvas',open={};
function home(m){var h=m.dataset.home;if(h){var s=document.getElementById(h);if(s){s.insertBefore(m,s.firstChild);return;}}st.appendChild(m);}
function tabFor(id){if(open[id])return open[id];var m=document.getElementById(id),b=document.createElement('button');b.className='tab';b.dataset.view=id;
b.innerHTML='<span></span><i class="x" title="Close">&times;</i>';b.firstChild.textContent=m.dataset.title||id;tabs.appendChild(b);open[id]=b;return b;}
function show(id){if(id!=='canvas'&&!document.getElementById(id))id='canvas';
var m=rd.firstElementChild;if(m)home(m);
if(id==='canvas'){v.hidden=false;rd.hidden=true;document.body.classList.remove('reading');}
else{tabFor(id);rd.appendChild(document.getElementById(id));v.hidden=true;rd.hidden=false;rd.scrollTop=0;document.body.classList.add('reading');}
cur=id;[].forEach.call(tabs.children,function(b){b.classList.toggle('on',b.dataset.view===id);});
var h='#'+id;if(location.hash!==h){try{history.replaceState(null,'',h);}catch(e){location.hash=h;}}
if(id==='canvas'&&needFit)fit();}
function close(id){var b=open[id];if(!b)return;delete open[id];b.parentNode.removeChild(b);if(cur===id)show('canvas');}
tabs.addEventListener('click',function(e){var x=e.target.closest('.x'),b=e.target.closest('.tab');if(!b)return;if(x)close(b.dataset.view);else show(b.dataset.view);});
window.addEventListener('hashchange',function(){var id=location.hash.slice(1)||'canvas';if(id!==cur)show(id);});
document.addEventListener('click',function(e){if(!e.target.closest('header.doctitle .cap'))return;var on=document.documentElement.classList.toggle('cap');try{localStorage.setItem('mn.readableWidth',on?'1':'0');}catch(x){}});
try{if(localStorage.getItem('mn.readableWidth')==='1')document.documentElement.classList.add('cap');}catch(e){}
var h0=location.hash.slice(1);if(h0&&h0!=='canvas')show(h0);
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
    QString body, store;
    int inkLayers = 0;                                          // stacked ink layers (note thumbs, PDF page ink) → the Annotations toggle
    body += QStringLiteral("<div id=\"mn-canvas\"><div id=\"world\" data-w=\"%1\" data-h=\"%2\" style=\"width:%3;height:%4\">\n").arg(num(W), num(H), px(W), px(H));
    Exporter::Options popt; popt.includeVideoNotes = opt.includeVideoNotes; popt.ufbLinks = opt.ufbLinks;
    auto pathLines = [&](const QString& path) {
        if (path.isEmpty()) return QString();
        QString s = QStringLiteral("<span class=\"path\">%1</span>").arg(esc(QDir::toNativeSeparators(path)));
        if (opt.ufbLinks) s += QStringLiteral("<a class=\"ufblink\" href=\"%1\">%1</a>").arg(esc(mn::ufbLinkFor(path)));
        return s;
    };
    // The reader's reference caption (the page export's .fname/.fpath/.fmeta typography).
    auto refCaption = [&](const QString& name, const QString& path, const QString& meta) {
        QString s = QStringLiteral("<figcaption><div class=\"fname\">%1</div>").arg(esc(name));
        if (!path.isEmpty()) {
            s += QStringLiteral("<div class=\"fpath\">%1</div>").arg(esc(QDir::toNativeSeparators(path)));
            if (opt.ufbLinks) s += QStringLiteral("<a class=\"ufblink\" href=\"%1\">%1</a>").arg(esc(mn::ufbLinkFor(path)));
        }
        return s + QStringLiteral("<div class=\"fmeta\">%1</div></figcaption>").arg(meta);
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
            inkLayers += frag.count(QLatin1String("<img class=\"ink\""));
            // The card's cap: rows whose top lies inside the element's height; the rest hide under a note.
            const int n = m->rowCountQml();
            int shown = n;
            if (n > 0 && m->totalHeight() + 48 > e.h + 1) { shown = std::clamp(m->rowForY(std::max(0.0, e.h - 48)), 0, n); }
            const int below = std::max(0, n - shown);
            const QString title = c->pageTitle(e.id);
            // ONE copy of the page: its <main> lives in the card and moves into the reader when its tab opens.
            body += QStringLiteral("<section class=\"el page\" id=\"p-%1\" data-open=\"pm-%1\" style=\"%2\">"
                                   "<main id=\"pm-%1\" data-home=\"p-%1\" data-title=\"%3\"><header class=\"doctitle\"><span>%3</span>"
                                   "<button class=\"cap\" type=\"button\" title=\"Readable width — the app's 760 measure\">Readable width</button></header>\n%4</main>%5</section>\n")
                        .arg(esc(e.id), style(e), esc(title), frag,
                             below > 0 ? QStringLiteral("<div class=\"more\">%1 more block%2 — open the page</div>").arg(below).arg(below == 1 ? QString() : QStringLiteral("s")) : QString());
        } else if (e.kind == QLatin1String("media")) {
            const QJsonObject md = mediaOf(e);
            const QString kind = md.value(QStringLiteral("kind")).toString(QStringLiteral("image"));
            const QString name = nameOf(md);
            const QString path = localPath(c, md);
            const QString src = md.value(QStringLiteral("src")).isString() ? md.value(QStringLiteral("src")).toString() : QString();
            if (kind == QLatin1String("image")) {
                QString img = src.startsWith(QLatin1String("http")) ? src : (path.isEmpty() ? QString() : sink.addFile(path, name.isEmpty() ? QStringLiteral("image") : name));
                QString inner = img.isEmpty() ? QStringLiteral("<figcaption>%1</figcaption>").arg(esc(name)) : QStringLiteral("<img src=\"%1\" alt=\"%2\">").arg(esc(img), esc(name));
                if (!img.isEmpty() && !path.isEmpty()) {   // the image's frame ink (json.ink["0"]) as a toggleable layer, lightbox-aware
                    const QString env = c->mediaElementInk(e.id, 0);
                    const QSize isz = env.isEmpty() ? QSize() : QImageReader(path).size();
                    const QImage ov = isz.isValid() ? Exporter::inkOverlay(env, md.value(QStringLiteral("w")).toInt(isz.width()), md.value(QStringLiteral("h")).toInt(isz.height()), isz.scaled(1280, 1280, Qt::KeepAspectRatio).expandedTo(QSize(1, 1))) : QImage();
                    const QString ovSrc = ov.isNull() ? QString() : sink.addImage(ov, (name.isEmpty() ? QStringLiteral("image") : name) + QStringLiteral("-ink"));
                    if (!ovSrc.isEmpty()) { ++inkLayers; inner = QStringLiteral("<div class=\"inkwrap\">%1<img class=\"ink\" src=\"%2\" alt=\"\" style=\"left:0;top:0;width:100%\"></div>").arg(inner, esc(ovSrc)); }
                }
                body += QStringLiteral("<figure class=\"el media image%1\" style=\"%2\">%3</figure>\n").arg(c->inGridCell(e.id) ? QStringLiteral(" cover") : QString(), style(e), inner);
            } else if (kind == QLatin1String("video") || kind == QLatin1String("pdf")) {
                const bool video = kind == QLatin1String("video");
                const QString base = name.isEmpty() ? kind : name;
                // The poster (frame 0 / page 1) renders ONCE at reader size; the card shows the same asset small.
                QImage poster;
                if (!path.isEmpty()) poster = video ? MediaStore::extractFrame(path, 0, 1280) : MediaStore::renderPdfPage(path, 0, 1280);
                const QString pimg = poster.isNull() ? QString() : sink.addImage(poster, base + (video ? QStringLiteral("-poster") : QStringLiteral("-page1")));
                const int pages = md.value(QStringLiteral("pages")).toInt();
                QString meta = video
                    ? QStringLiteral("%1 × %2 · %3 fps · %4").arg(md.value(QStringLiteral("w")).toInt()).arg(md.value(QStringLiteral("h")).toInt())
                          .arg(num(md.value(QStringLiteral("fps")).toDouble()), Exporter::durationText(md.value(QStringLiteral("durMs")).toDouble()))
                    : QStringLiteral("%1 page%2").arg(pages).arg(pages == 1 ? QString() : QStringLiteral("s"));
                // The reader: the poster large with its reference caption, then the clip's QCView notes as the
                // filmstrip (thumb + ink layer, timecode, text, addressed) or the PDF's inked pages.
                QString cardInk;                                            // page 1's ink layer, shared by the card
                QString reader = QStringLiteral("<figure class=\"ref\">%1%2</figure>")
                                     .arg(pimg.isEmpty() ? QString() : QStringLiteral("<img src=\"%1\" alt=\"%2\">").arg(esc(pimg), esc(name)), QString());
                if (!video && !pimg.isEmpty() && !poster.isNull()) {   // page 1's element ink rides the poster as a toggleable layer
                    const QImage ov = Exporter::inkOverlay(c->mediaElementInk(e.id, 0), md.value(QStringLiteral("w")).toInt(), md.value(QStringLiteral("h")).toInt(), poster.size());
                    if (!ov.isNull()) {
                        const QString ovSrc = sink.addImage(ov, base + QStringLiteral("-page1-ink"));
                        if (!ovSrc.isEmpty()) {
                            ++inkLayers; cardInk = ovSrc;
                            reader = QStringLiteral("<figure class=\"ref\"><div class=\"inkwrap\"><img src=\"%1\" alt=\"%2\"><img class=\"ink\" src=\"%3\" alt=\"\"></div></figure>").arg(esc(pimg), esc(name), esc(ovSrc));
                        }
                    }
                }
                reader.insert(reader.lastIndexOf(QLatin1String("</figure>")), refCaption(name, path, esc(kind) + QStringLiteral(" · ") + meta));
                const QString cardPoster = pimg.isEmpty() ? QString()
                    : !cardInk.isEmpty() ? QStringLiteral("<div class=\"inkwrap\"><img class=\"poster\" src=\"%1\" alt=\"%2\"><img class=\"ink\" src=\"%3\" alt=\"\"></div>").arg(esc(pimg), esc(name), esc(cardInk))
                    : QStringLiteral("<img class=\"poster\" src=\"%1\" alt=\"%2\">").arg(esc(pimg), esc(name));
                int notes = 0;
                if (video && opt.includeVideoNotes && !path.isEmpty()) {
                    const QString vn = Exporter::videoNotesHtml(path, sink, inkLayers);
                    notes = vn.count(QLatin1String("<div class=\"notecard"));
                    reader += vn;
                } else if (!video && !path.isEmpty()) {
                    for (int pg = 1; pg < pages; ++pg) {   // every OTHER page carrying ink: clean base + toggleable overlay
                        const QString env = c->mediaElementInk(e.id, pg);
                        if (env.isEmpty()) continue;
                        const QImage pgImg = MediaStore::renderPdfPage(path, pg, 1280);
                        if (pgImg.isNull()) continue;
                        const QString baseSrc = sink.addImage(pgImg, base + QStringLiteral("-page%1").arg(pg + 1));
                        if (baseSrc.isEmpty()) continue;
                        const QImage ov = Exporter::inkOverlay(env, md.value(QStringLiteral("w")).toInt(), md.value(QStringLiteral("h")).toInt(), pgImg.size());
                        const QString ovSrc = ov.isNull() ? QString() : sink.addImage(ov, base + QStringLiteral("-page%1-ink").arg(pg + 1));
                        if (!ovSrc.isEmpty()) ++inkLayers;
                        reader += QStringLiteral("<figure class=\"ref\">%1<figcaption><div class=\"fmeta\">%2 · page %3 of %4</div></figcaption></figure>")
                                      .arg(ovSrc.isEmpty() ? QStringLiteral("<img src=\"%1\" alt=\"%2\">").arg(esc(baseSrc), esc(name))
                                                           : QStringLiteral("<div class=\"inkwrap\"><img src=\"%1\" alt=\"%2\"><img class=\"ink\" src=\"%3\" alt=\"\"></div>").arg(esc(baseSrc), esc(name), esc(ovSrc)),
                                           esc(name)).arg(pg + 1).arg(pages);
                    }
                }
                if (notes > 0) meta += QStringLiteral(" · %1 note%2").arg(notes).arg(notes == 1 ? QString() : QStringLiteral("s"));
                store += QStringLiteral("<main class=\"reader %1\" id=\"m-%2\" data-title=\"%3\"><header class=\"doctitle\"><span>%3</span></header>\n%4</main>\n")
                             .arg(kind, esc(e.id), esc(name), reader);
                body += QStringLiteral("<figure class=\"el media %1\" data-open=\"m-%2\" style=\"%3\">%4<figcaption><span class=\"name\">%5</span><span class=\"meta\">%6</span>%7</figcaption></figure>\n")
                            .arg(kind, esc(e.id), style(e), cardPoster, esc(name), meta, pathLines(path));
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
        } else if (e.kind == QLatin1String("group")) {   // a frame (2026-10-22): the tinted rect + title under its children (z order draws it first)
            const QJsonObject o = QJsonDocument::fromJson(e.json.toUtf8()).object();
            const QString tint = o.value(QStringLiteral("tint")).toString();
            body += QStringLiteral("<div class=\"el frame%1\" style=\"%2%3\"><div class=\"ftitle\">%4</div></div>\n")
                        .arg(tint.isEmpty() ? QString() : QStringLiteral(" tinted"), style(e), tint.isEmpty() ? QString() : QStringLiteral(";--tint:") + tint, esc(o.value(QStringLiteral("title")).toString()));
        } else if (e.kind == QLatin1String("shape")) {
            const QJsonObject o = QJsonDocument::fromJson(e.json.toUtf8()).object();
            if (o.value(QStringLiteral("shape")).toString() == QLatin1String("text")) {
                const double size = o.value(QStringLiteral("fontSize")).toDouble(14);
                const TextChipLook look = textChipLook(o);
                body += QStringLiteral("<div class=\"el text%1\" style=\"%2;font-size:%3;padding:%4;color:%5%6\">%7</div>\n")
                            .arg((look.plain ? QString() : QStringLiteral(" box")) + (o.value(QStringLiteral("fixedW")).toBool() ? QString() : QStringLiteral(" auto")), style(e), px(size), px(std::round(size * 0.3)), esc(look.text.isValid() ? cssColor(look.text) : QStringLiteral("var(--text)")),
                                 look.plain ? QString() : QStringLiteral(";background:%1").arg(cssColor(look.fill)), esc(o.value(QStringLiteral("text")).toString()));
            }
        }
        // connectors: reserved; nothing is emitted until their renderer lands
    }
    body += QStringLiteral("</div></div>\n<div id=\"mn-reader\" hidden></div>\n<div id=\"mn-store\">\n%1</div>\n").arg(store);
    // The strip: Canvas + (opened tabs, by the script) · the zoom HUD.
    // No Annotations toggle (user ruling 2026-10-18; the page export dropped its own 2026-10-21): ink layers always show.
    (void)inkLayers;
    const QString toggle;
    const QString strip = QStringLiteral("<nav id=\"mn-tabs\"><div class=\"tabs\"><button class=\"tab on\" data-view=\"canvas\"><span>Canvas</span></button></div>"
                                         "<div class=\"end\"><span id=\"mn-hud\">100% <b title=\"Fit (0)\">fit</b></span></div></nav>\n");
    return QStringLiteral("<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
                          "<meta name=\"generator\" content=\"minNotes\">\n<title>%1</title>\n<style>%2%3</style>\n</head>\n<body class=\"canvas\">\n%4%5%6%7%8%9</body>\n</html>\n")
        .arg(esc(c->documentName()), Exporter::pageCss(), QString::fromUtf8(kCanvasCss), toggle, strip, body, Exporter::lightboxHtml(), QString::fromUtf8(kCanvasScript), Exporter::taskScript());
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
        if (e.kind == QLatin1String("group")) {   // a frame (2026-10-22)
            const QJsonObject o = QJsonDocument::fromJson(e.json.toUtf8()).object();
            const QString tintS = o.value(QStringLiteral("tint")).toString();
            QColor tint = tintS.isEmpty() ? muted : QColor(tintS);
            if (!tint.isValid()) tint = muted;
            QColor fill = tint; fill.setAlphaF(tintS.isEmpty() ? 0.05 : 0.10);
            QColor line = tint; line.setAlphaF(tintS.isEmpty() ? 0.35 : 0.5);
            p.fillRect(r, fill);
            p.setPen(QPen(line, 1)); p.setBrush(Qt::NoBrush); p.drawRect(r);
            QFont tf(QStringLiteral("Aspekta")); tf.setPixelSize(std::max(4, int(std::round(13 * s)))); tf.setBold(true);
            p.setFont(tf); p.setPen(tint);
            p.drawText(QRectF(r.x() + 10 * s, r.y(), std::max(1.0, r.width() - 20 * s), 28 * s), Qt::AlignVCenter | Qt::AlignLeft, o.value(QStringLiteral("title")).toString());
            continue;
        }
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
            if (!thumb.isNull() && c->inGridCell(e.id)) {   // a board's cell: cover-crop the picture to the cell (stage 2)
                const double ar = thumb.width() / double(std::max(1, thumb.height())), car = r.width() / std::max(1.0, r.height());
                QRect src = thumb.rect();
                if (ar > car) { const int w = int(std::round(thumb.height() * car)); src = QRect((thumb.width() - w) / 2, 0, w, thumb.height()); }
                else { const int h = int(std::round(thumb.width() / car)); src = QRect(0, (thumb.height() - h) / 2, thumb.width(), h); }
                p.drawImage(r, thumb, src);
            } else if (!thumb.isNull()) {
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
                const double size = o.value(QStringLiteral("fontSize")).toDouble(14);
                QFont f(QStringLiteral("Aspekta")); f.setPixelSize(std::max(2, int(std::round(size * s))));
                const TextChipLook look = textChipLook(o);
                if (!look.plain) p.fillRect(r, look.fill);
                p.setFont(f); p.setPen(look.text.isValid() ? look.text : text);
                const double pad = std::round(size * 0.3) * s;
                if (f.pixelSize() >= 4) p.drawText(r.adjusted(pad, pad, -pad, -pad), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, o.value(QStringLiteral("text")).toString());
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
