#pragma once
// Dev-only (speed programme, 2026-10-16): the keystroke's FULL pipeline, not just the GUI-thread
// work the type probe times. For every typed key it logs
//   queue  — OS event timestamp → the key reaching the window (input queue),
//   turn   — the key's synchronous work until the event loop is idle again,
//   sync   — key → the scene graph's next sync (how long the render loop made the change wait),
//   swap   — key → that frame's swap (what the type probe cannot see: the render loop + present).
//   upd/polished — when the render loop's UpdateRequest arrived and when polish finished (the
//                  gap is the GUI's per-frame polish: Qt 6.11 walks the whole item tree there
//                  whenever ANY item's effective visibility / z / child set changed — 5 ms here),
//   children/vis — QQuickItem child-set changes and effective-visibility flips inside the key
//                  (each one triggers that walk; MN_KEYLAT_CHILDREN=1 names the items).
// Armed by `--perf-log` (logs real keys, with the OS queue time) or `--key-probe=N` (posts N
// synthetic keys ~13/s to the window, then six wheel notches for the glide's frames/s, prints the
// percentiles and exits — the A/B harness for render-loop settings: MN_NOVSYNC=1,
// QT_QPA_UPDATE_IDLE_TIME, QSG_RENDER_LOOP). MN_KEYLAT_TRAP=1 stops for lldb once the document is up.
#include <QObject>
#include <QElapsedTimer>
#include <QVector>
#include <QHash>
#include <atomic>

class QQuickWindow;
class QQuickItem;

class KeyLatencyProbe : public QObject {
    Q_OBJECT
public:
    explicit KeyLatencyProbe(QQuickWindow* w, int synthKeys = 0, QObject* parent = nullptr);
    bool eventFilter(QObject* o, QEvent* e) override;
private:
    void report();
    void summary();
    QQuickWindow* win_;
    QElapsedTimer t0_;
    std::atomic<bool> pending_{false};
    std::atomic<qint64> keyNs_{0}, syncNs_{0}, swapNs_{0};
    std::atomic<int> swaps_{0};          // every swap, for the frames/s of a run or a glide
    qint64 updNs_ = 0, polNs_ = 0;      // polNs_: afterAnimating — polish done, about to hand over to the render thread                   // the UpdateRequest the key's change was rendered from
    qint64 turnNs_ = 0;
    double queueMs_ = -1.0;
    QString key_;
    QVector<double> turn_, sync_, swap_, queue_, upd_, pol_;
    int synthLeft_;
    int keys_ = 0;
    int childEvents_ = 0, visFlips_ = 0;   // visFlips_: effective-visibility changes since the key (each one ALSO forces the stacking walk: qquickitem.cpp:6751)
    void hookVisibility(QQuickItem* item);
    bool hooked_ = false;
    bool glideTrace_ = false;            // phase 2 of --key-probe: aggregate flips/churn by item instead of per key
    QHash<QString, int> glideVis_, glideChildren_;
    qint64 glideUpdNs_ = 0; QVector<double> glidePolish_;   // per-frame polish during the glide
    static QString chainOf(QQuickItem* item, int depth);                // QQuickItem children added/removed since the key (each one forces the window's full stacking walk)
};
