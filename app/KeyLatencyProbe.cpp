#include "KeyLatencyProbe.h"
#include <QQuickWindow>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QTimer>
#include <QScreen>
#include <QCoreApplication>
#include <QQuickItem>
#include <QChildEvent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QUrl>
#include <QDebug>
#include <algorithm>
#include <csignal>

static double pct(QVector<double> v, double p) {
    if (v.isEmpty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[qBound(0, int(p * (v.size() - 1) + 0.5), v.size() - 1)];
}

KeyLatencyProbe::KeyLatencyProbe(QQuickWindow* w, int synthKeys, QObject* parent)
    : QObject(parent), win_(w), synthLeft_(synthKeys) {
    t0_.start();
    w->installEventFilter(this);
    QCoreApplication::instance()->installEventFilter(this);   // ChildAdded/Removed of QQuickItems (MN_KEYLAT_CHILDREN=1 prints them)
    // Both signals come from the render thread (threaded loop) — stamp into atomics, report later.
    connect(w, &QQuickWindow::afterAnimating, this, [this] {   // GUI thread: polish + animation advance are done
        if (pending_.load() && updNs_ > 0 && polNs_ == 0) polNs_ = t0_.nsecsElapsed();
        if (glideTrace_ && glideUpdNs_ > 0) { glidePolish_ << (t0_.nsecsElapsed() - glideUpdNs_) / 1e6; glideUpdNs_ = 0; }
    });
    connect(w, &QQuickWindow::beforeSynchronizing, this, [this] {
        if (pending_.load() && syncNs_.load() == 0) syncNs_.store(t0_.nsecsElapsed());
    }, Qt::DirectConnection);
    connect(w, &QQuickWindow::frameSwapped, this, [this] {
        ++swaps_;
        if (glideTrace_) { QMutexLocker l(&swapMx_); glideSwapNs_ << t0_.nsecsElapsed(); }
        if (pending_.load() && syncNs_.load() > 0 && swapNs_.load() == 0) {
            swapNs_.store(t0_.nsecsElapsed());
            QMetaObject::invokeMethod(this, [this] { report(); }, Qt::QueuedConnection);
        }
    }, Qt::DirectConnection);
    const QScreen* s = w->screen();
    qInfo().noquote() << QStringLiteral("[keylat] screen %1 Hz  window %2x%3 @%4x  loop %5  api %6  synth %7")
        .arg(s ? s->refreshRate() : 0.0).arg(w->width()).arg(w->height()).arg(w->devicePixelRatio())
        .arg(QString::fromLocal8Bit(qgetenv("QSG_RENDER_LOOP").isEmpty() ? QByteArray("default") : qgetenv("QSG_RENDER_LOOP")))
        .arg(int(w->rendererInterface() ? w->rendererInterface()->graphicsApi() : 0)).arg(synthKeys);
    if (synthKeys > 0) {
        // Phase 1: N keys. Phase 2: six mouse-wheel notches at the window centre start a SmoothWheel
        // glide; the swaps in the next 1.2 s give the glide's frames/s (what an unthrottled render
        // loop would do to it). Then the summary, then exit.
        auto* t = new QTimer(this);
        t->setInterval(75);
        auto* phase = new int(0);
        connect(t, &QTimer::timeout, this, [this, t, phase] {
            if (*phase == 0) {
                if (synthLeft_-- <= 0) { *phase = 1; t->setInterval(30); synthLeft_ = 6; return; }
                const QString ch = QStringLiteral("abcdefghijklmnopqrstuvwxyz").mid(synthLeft_ % 26, 1);
                QCoreApplication::postEvent(win_, new QKeyEvent(QEvent::KeyPress, Qt::Key_A + synthLeft_ % 26, Qt::NoModifier, ch));
                QCoreApplication::postEvent(win_, new QKeyEvent(QEvent::KeyRelease, Qt::Key_A + synthLeft_ % 26, Qt::NoModifier, ch));
            } else if (*phase == 1) {
                if (synthLeft_-- <= 0) { *phase = 2; swaps_.store(0); glideTrace_ = true; t->setInterval(1200); return; }
                const QPointF pos(win_->width() / 2.0, win_->height() / 2.0);
                QCoreApplication::postEvent(win_, new QWheelEvent(pos, win_->mapToGlobal(pos.toPoint()), QPoint(), QPoint(0, -120),
                                                                  Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false));
            } else if (*phase == 2) {
                glideTrace_ = false;
                QVector<double> iv; { QMutexLocker l(&swapMx_); for (int i = 1; i < glideSwapNs_.size(); ++i) iv << (glideSwapNs_[i] - glideSwapNs_[i - 1]) / 1e6; }
                int late = 0; for (double d : iv) if (d > 25.0) ++late;
                qInfo().noquote() << QStringLiteral("[keylat] GLIDE pacing: swap interval p50/p95/max %1/%2/%3 ms, %4 gaps > 25 ms of %5")
                    .arg(pct(iv, .5), 0, 'f', 1).arg(pct(iv, .95), 0, 'f', 1).arg(iv.isEmpty() ? 0.0 : *std::max_element(iv.begin(), iv.end()), 0, 'f', 1).arg(late).arg(iv.size());
                *phase = 3; swaps_.store(0); idleTrace_ = true; t->setInterval(3000);   // then 3 s of nothing
            } else {
                t->stop();
                qInfo().noquote() << QStringLiteral("[keylat] IDLE %1 swaps in 3 s with no input (caret blink = ~6)").arg(swaps_.load());
                qInfo().noquote() << QStringLiteral("[keylat] GLIDE %1 frames/s over 1.2 s after six wheel notches; polish per frame p50/p95 %2/%3 ms (n %4)")
                    .arg(glideSwapNs_.size() / 1.2, 0, 'f', 0).arg(pct(glidePolish_, .5), 0, 'f', 2).arg(pct(glidePolish_, .95), 0, 'f', 2).arg(glidePolish_.size());
                auto dump = [](const char* what, const QHash<QString, int>& h) {
                    QList<QPair<int, QString>> v; for (auto it = h.cbegin(); it != h.cend(); ++it) v.append({it.value(), it.key()});
                    std::sort(v.begin(), v.end(), [](const QPair<int, QString>& a, const QPair<int, QString>& b) { return a.first > b.first; });
                    int total = 0; for (const auto& e : v) total += e.first;
                    qInfo().noquote() << QStringLiteral("[keylat] GLIDE %1: %2 total").arg(what).arg(total);
                    for (int i = 0; i < v.size() && i < 12; ++i) qInfo().noquote() << QStringLiteral("[keylat]    %1  %2").arg(v[i].first, 5).arg(v[i].second);
                };
                dump("visibility flips", glideVis_); dump("children changes", glideChildren_);
                summary(); QCoreApplication::exit(0);
            }
        });
        QTimer::singleShot(2500, t, [t] { t->start(); });   // after the document is up
    }
    // MN_KEYLAT_TRAP=1: stop for an attached lldb once the document is up (so a breakpoint set after
    // this point skips the thousands of start-up hits): lldb -b ... -o 'process launch ...' -o 'breakpoint set ...' -o continue
#ifndef Q_OS_WIN
    if (qEnvironmentVariableIntValue("MN_KEYLAT_TRAP") > 0) QTimer::singleShot(2400, this, [] { raise(SIGSTOP); });   // (POSIX only: no SIGSTOP on Windows)
#endif
}

QString KeyLatencyProbe::chainOf(QQuickItem* item, int depth) {
    QString chain;
    for (QQuickItem* p = item; p && depth-- > 0; p = p->parentItem()) {
        const QQmlContext* ctx = qmlContext(p);
        chain += QStringLiteral("%1%2[%3] ").arg(p == item ? "" : "< ", QString::fromLatin1(p->metaObject()->className()), ctx ? ctx->baseUrl().fileName() : QStringLiteral("-"));
    }
    return chain;
}

// Every item in the tree reports its effective-visibility flips (MN_KEYLAT_CHILDREN=1 names them).
void KeyLatencyProbe::hookVisibility(QQuickItem* item) {
    if (!item) return;
    connect(item, &QQuickItem::visibleChanged, this, [this, item] {
        if (glideTrace_) { ++glideVis_[chainOf(item, 3)]; return; }
        if (!pending_.load()) return;
        ++visFlips_;
        static const bool verbose = qEnvironmentVariableIntValue("MN_KEYLAT_CHILDREN") > 0;
        if (verbose && visFlips_ <= 12) {
            QString chain;
            for (QQuickItem* p = item; p && chain.count(QLatin1Char('<')) < 4; p = p->parentItem()) {
                const QQmlContext* ctx = qmlContext(p);
                chain += QStringLiteral("%1%2[%3] ").arg(p == item ? "" : "< ", QString::fromLatin1(p->metaObject()->className()), ctx ? ctx->baseUrl().fileName() : QStringLiteral("-"));
            }
            qInfo().noquote() << QStringLiteral("[keylat]    visible -> %1: %2").arg(item->isVisible() ? "on" : "off", chain);
        }
    });
    // QML-created items get their parent without a ChildAdded event (QQml_setParent_noEvent), so the
    // Repeater/Loader churn shows up here instead: the PARENT's childrenChanged.
    connect(item, &QQuickItem::childrenChanged, this, [this, item] {
        if (glideTrace_) {   // name the delegate type by its first QML-declared property (index/rr/h/sr/span/bg…)
            QString kind;
            if (!item->childItems().isEmpty()) {
                const QMetaObject* mo = item->childItems().last()->metaObject();
                kind = QString::fromLatin1(mo->className());
                for (int i = mo->propertyOffset(); i < mo->propertyCount() && i < mo->propertyOffset() + 3; ++i) kind += QLatin1Char('.') + QString::fromLatin1(mo->property(i).name());
            }
            ++glideChildren_[QStringLiteral("%1 in %2").arg(kind, chainOf(item, 1))]; return;
        }
        if (!pending_.load()) return;
        ++childEvents_;
        static const bool verbose = qEnvironmentVariableIntValue("MN_KEYLAT_CHILDREN") > 0;
        if (verbose && childEvents_ <= 12) {
            QString chain;
            for (QQuickItem* p = item; p && chain.count(QLatin1Char('<')) < 4; p = p->parentItem()) {
                const QQmlContext* ctx = qmlContext(p);
                chain += QStringLiteral("%1%2[%3] ").arg(p == item ? "" : "< ", QString::fromLatin1(p->metaObject()->className()), ctx ? ctx->baseUrl().fileName() : QStringLiteral("-"));
            }
            QString newest;
            if (!item->childItems().isEmpty()) {
                QQuickItem* c = item->childItems().last();
                newest = QStringLiteral(" last child %1 at %2,%3 %4x%5 color %6 z %7").arg(QString::fromLatin1(c->metaObject()->className()))
                    .arg(c->x()).arg(c->y()).arg(c->width()).arg(c->height()).arg(c->property("color").toString()).arg(c->z());
            }
            qInfo().noquote() << QStringLiteral("[keylat]    children changed (%1 now): %2%3").arg(item->childItems().size()).arg(chain, newest);
        }
    });
    for (QQuickItem* c : item->childItems()) hookVisibility(c);
}

bool KeyLatencyProbe::eventFilter(QObject* o, QEvent* e) {
    if ((e->type() == QEvent::ChildAdded || e->type() == QEvent::ChildRemoved) && pending_.load()) {
        QObject* c = static_cast<QChildEvent*>(e)->child();
        if (qobject_cast<QQuickItem*>(c) || qobject_cast<QQuickItem*>(o)) {
            ++childEvents_;
            static const bool verbose = qEnvironmentVariableIntValue("MN_KEYLAT_CHILDREN") > 0;
            if (verbose && childEvents_ <= 12) {
                auto where = [](QObject* x) {
                    const QQmlContext* ctx = x ? qmlContext(x) : nullptr;
                    return QStringLiteral("%1[%2]").arg(x ? QString::fromLatin1(x->metaObject()->className()) : QStringLiteral("?"),
                                                        ctx ? ctx->baseUrl().fileName() : QStringLiteral("-"));
                };
                qInfo().noquote() << QStringLiteral("[keylat]    child %1: %2 -> parent %3").arg(e->type() == QEvent::ChildAdded ? "added" : "removed", where(c), where(o));
            }
        }
    }
    if (o == win_ && e->type() == QEvent::UpdateRequest && pending_.load() && updNs_ == 0 && syncNs_.load() == 0)
        updNs_ = t0_.nsecsElapsed();
    if (o == win_ && e->type() == QEvent::UpdateRequest && glideTrace_) glideUpdNs_ = t0_.nsecsElapsed();   // the render loop's (display-link or timer) tick the change waited for
    if (o == win_ && e->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(e);
        const bool typed = ke->text().size() == 1 && ke->text().at(0) >= QChar(' ')
                           && !(ke->modifiers() & (Qt::ControlModifier | Qt::MetaModifier | Qt::AltModifier));
        if (typed && !pending_.load()) {
            if (!hooked_) { hooked_ = true; hookVisibility(win_->contentItem()); }   // the tree is complete by the first key
            const qint64 now = t0_.nsecsElapsed();
            // Cocoa stamps NSEvent.timestamp (ms since boot, the same clock as QElapsedTimer here).
            queueMs_ = ke->timestamp() > 0 ? double(t0_.msecsSinceReference() + now / 1000000) - double(ke->timestamp()) : -1.0;
            key_ = ke->text();
            syncNs_.store(0); swapNs_.store(0); turnNs_ = 0; updNs_ = 0; polNs_ = 0; childEvents_ = 0; visFlips_ = 0;
            keyNs_.store(now);
            pending_.store(true);
            QTimer::singleShot(0, this, [this] { turnNs_ = t0_.nsecsElapsed() - keyNs_.load(); });
        }
    }
    return false;
}

void KeyLatencyProbe::report() {
    if (!pending_.load()) return;
    pending_.store(false);
    const double turn = turnNs_ / 1e6, sync = (syncNs_.load() - keyNs_.load()) / 1e6, swap = (swapNs_.load() - keyNs_.load()) / 1e6;
    const double upd = updNs_ > 0 ? (updNs_ - keyNs_.load()) / 1e6 : -1.0;
    const double pol = polNs_ > 0 ? (polNs_ - keyNs_.load()) / 1e6 : -1.0;
    turn_ << turn; sync_ << sync; swap_ << swap; if (upd >= 0) upd_ << upd; if (pol >= 0) pol_ << pol;
    if (queueMs_ >= 0) queue_ << queueMs_;
    ++keys_;
    qInfo().noquote() << QStringLiteral("[keylat] '%1'  queue %2 ms  turn %3 ms  upd +%4 ms  polished +%5 ms  sync +%6 ms  swap +%7 ms  children %8  vis %9")
        .arg(key_).arg(queueMs_ >= 0 ? QString::number(queueMs_, 'f', 1) : QStringLiteral("-"))
        .arg(turn, 0, 'f', 2).arg(upd, 0, 'f', 1).arg(pol, 0, 'f', 1).arg(sync, 0, 'f', 1).arg(swap, 0, 'f', 1).arg(childEvents_).arg(visFlips_);
}

void KeyLatencyProbe::summary() {
    qInfo().noquote() << QStringLiteral("[keylat] SUMMARY keys %1  (p50/p95 ms)  queue %2/%3  turn %4/%5  upd %6/%7  polished %12/%13  sync %8/%9  swap %10/%11")
        .arg(keys_)
        .arg(pct(queue_, .5), 0, 'f', 1).arg(pct(queue_, .95), 0, 'f', 1)
        .arg(pct(turn_, .5), 0, 'f', 2).arg(pct(turn_, .95), 0, 'f', 2)
        .arg(pct(upd_, .5), 0, 'f', 1).arg(pct(upd_, .95), 0, 'f', 1)
        .arg(pct(sync_, .5), 0, 'f', 1).arg(pct(sync_, .95), 0, 'f', 1)
        .arg(pct(swap_, .5), 0, 'f', 1).arg(pct(swap_, .95), 0, 'f', 1)
        .arg(pct(pol_, .5), 0, 'f', 1).arg(pct(pol_, .95), 0, 'f', 1);
}
