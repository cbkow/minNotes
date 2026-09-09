#include "SpellService.h"
#include "SpellDictionaryFiles.h"
#include "SpellEngine.h"
#include "SpellTokenizer.h"
#include "../core/BlockModel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <chrono>

namespace {
constexpr int kDebounceMs = 300;
constexpr int kPassChunk = 32;
constexpr int kSpanCode = 3, kSpanChoice = 10;   // BlockModel::SpanCode / SpanChoice
}

SpellService::SpellService(const QString& appVersion, QObject* parent)
    : QObject(parent), appVersion_(appVersion) {
    debounce_.setSingleShot(true);
    debounce_.setInterval(kDebounceMs);
    connect(&debounce_, &QTimer::timeout, this, &SpellService::flushDue);
    passTimer_.setSingleShot(true);
    passTimer_.setInterval(0);
    connect(&passTimer_, &QTimer::timeout, this, &SpellService::backgroundTick);
    worker_ = std::thread([this] { workerMain(); });
}

SpellService::~SpellService() {
    { std::lock_guard<std::mutex> lk(mu_); quit_ = true; queue_.clear(); }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

QString SpellService::cellKey(const QString& id, int r, int c) {
    return id + QLatin1Char(':') + QString::number(r) + QLatin1Char(':') + QString::number(c);
}

void SpellService::setModel(BlockModel* m) {
    if (m == model_) return;
    for (const auto& c : conns_) QObject::disconnect(c);
    conns_.clear();
    model_ = m;
    ++gen_;
    cache_.clear(); dirty_.clear(); rowHint_.clear();
    { std::lock_guard<std::mutex> lk(mu_); queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
          [](const Job& j) { return !j.control; }), queue_.end()); }
    debounce_.stop();
    passRow_ = 0;
    if (model_) {
        conns_.push_back(connect(model_, &QAbstractItemModel::dataChanged, this, &SpellService::onDataChanged));
        conns_.push_back(connect(model_, &QObject::destroyed, this, [this] { setModel(nullptr); }));
        conns_.push_back(connect(model_, &QAbstractItemModel::modelReset, this, [this] { cache_.clear(); passRow_ = 0; passTimer_.start(); ++revision_; emit revisionChanged(); }));
        passTimer_.start();
    }
    ++revision_;
    emit revisionChanged();
}

void SpellService::setCheckSpelling(bool on) {
    if (checkSpelling_ == on) return;
    checkSpelling_ = on;
    emit checkSpellingChanged();
    ++revision_; emit revisionChanged();
}
void SpellService::setCheckGrammar(bool on) {
    if (checkGrammar_ == on) return;
    checkGrammar_ = on;
    emit checkGrammarChanged();
    ++revision_; emit revisionChanged();
}

QString SpellService::ignoredRulesJson() const {
    QJsonArray a;
    for (const QString& r : ignoredRules_) a.append(r);
    return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
}
void SpellService::setIgnoredRulesJson(const QString& json) {
    QSet<QString> s;
    for (const QJsonValue& v : QJsonDocument::fromJson(json.toUtf8()).array()) if (v.isString()) s.insert(v.toString());
    if (s == ignoredRules_) return;
    ignoredRules_ = s;
    submitControl([s](SpellEngine& e) { e.setDisabledRules(s); });
    emit ignoredRulesChanged();
    ++revision_; emit revisionChanged();
}
QStringList SpellService::ignoredRules() const { return QStringList(ignoredRules_.begin(), ignoredRules_.end()); }

void SpellService::ignoreRule(const QString& ruleId) {
    if (ruleId.isEmpty() || ignoredRules_.contains(ruleId)) return;
    ignoredRules_.insert(ruleId);
    const QSet<QString> s = ignoredRules_;
    submitControl([s](SpellEngine& e) { e.setDisabledRules(s); });
    emit ignoredRulesChanged();
    ++revision_; emit revisionChanged();
}
void SpellService::unignoreRule(const QString& ruleId) {
    if (!ignoredRules_.remove(ruleId)) return;
    const QSet<QString> s = ignoredRules_;
    submitControl([s](SpellEngine& e) { e.setDisabledRules(s); });
    emit ignoredRulesChanged();
    ++revision_; emit revisionChanged();
}
void SpellService::ignoreWord(const QString& word) {
    const QString w = spell::normalizeApostrophes(word.trimmed());
    if (w.isEmpty()) return;
    ignoredWords_.insert(w.toLower());
    const QSet<QString> s = ignoredWords_;
    submitControl([s](SpellEngine& e) { e.setIgnoredWords(s); });
    ++revision_; emit revisionChanged();
}
void SpellService::addToDictionary(const QString& word) {
    const QString w = word.trimmed();
    if (w.isEmpty()) return;
    userWords_.insert(w.toLower());
    spell::appendUserDictionary(w);
    submitControl([w](SpellEngine& e) { e.addUserWord(w); });
    ++revision_; emit revisionChanged();
}

// ---- model following -----------------------------------------------------

void SpellService::onDataChanged(const QModelIndex& tl, const QModelIndex& br, const QList<int>& roles) {
    Q_UNUSED(roles);
    if (!model_) return;
    for (int row = tl.row(); row <= br.row(); ++row) {
        if (row < 0 || row >= model_->rowCountQml()) continue;
        markDirty(model_->idForRow(row), row);
    }
}

void SpellService::markDirty(const QString& id, int row) {
    if (id.isEmpty()) return;
    dirty_.insert(id, row);
    rowHint_.insert(id, row);
    debounce_.start();
}

int SpellService::rowForKey(const QString& id, int hint) const {
    if (!model_) return -1;
    if (hint >= 0 && hint < model_->rowCountQml() && model_->idForRow(hint) == id) return hint;
    return model_->rowForId(id);
}

std::vector<std::pair<int,int>> SpellService::excludedSpans(const QVariantList& spans) const {
    std::vector<std::pair<int,int>> out;
    for (const QVariant& v : spans) {
        const QVariantMap m = v.toMap();
        const int k = m.value(QStringLiteral("k")).toInt();
        if (k == kSpanCode || k == kSpanChoice)
            out.emplace_back(m.value(QStringLiteral("s")).toInt(), m.value(QStringLiteral("e")).toInt());
    }
    return out;
}

// Queue the check(s) for a block id: one job for a text block, one per text
// cell for a table (typed choice/check body cells skipped).
void SpellService::scheduleRow(int row, bool front, bool wantSug) {
    if (!model_ || row < 0 || row >= model_->rowCountQml()) return;
    const int type = model_->typeForRow(row);
    const QString id = model_->idForRow(row);
    rowHint_.insert(id, row);
    if (type == BlockModel::Table) {
        const int rows = model_->tableRows(row), cols = model_->tableColumns(row), hdr = model_->tableHeaderRows(row);
        for (int c = 0; c < cols; ++c) {
            const int kind = model_->tableColumnKind(row, c);
            for (int r = 0; r < rows; ++r) {
                if (r >= hdr && kind != 0) continue;
                Job j; j.gen = gen_; j.key = cellKey(id, r, c);
                j.text = model_->tableCell(row, r, c);
                j.excluded = excludedSpans(model_->tableCellSpans(row, r, c));
                j.wantSuggestions = wantSug;
                Entry& e = cache_[j.key]; e.pending = true;
                if (j.text.isEmpty()) { e.checkedText.clear(); e.issues.clear(); e.pending = false; continue; }
                submit(std::move(j), front);
            }
        }
        return;
    }
    const bool textish = type == BlockModel::Paragraph || type == BlockModel::Heading || type == BlockModel::Quote
        || type == BlockModel::ListItem || type == BlockModel::TaskListItem || type == BlockModel::OrderedListItem;
    if (!textish) { cache_.remove(id); return; }
    Job j; j.gen = gen_; j.key = id;
    j.text = model_->contentForRow(row);
    j.excluded = excludedSpans(model_->spansForRow(row));
    j.wantSuggestions = wantSug;
    Entry& e = cache_[id]; e.pending = true;
    if (j.text.isEmpty()) { e.checkedText.clear(); e.issues.clear(); e.pending = false; return; }
    submit(std::move(j), front);
}

void SpellService::flushDue() {
    if (!model_) { dirty_.clear(); return; }
    const QHash<QString, int> due = dirty_;
    dirty_.clear();
    for (auto it = due.constBegin(); it != due.constEnd(); ++it) {
        const int row = rowForKey(it.key(), it.value());
        if (row < 0) { cache_.remove(it.key()); continue; }
        scheduleRow(row, /*front=*/true, /*wantSug=*/true);
    }
}

void SpellService::flushRow(int row) {
    if (!model_ || row < 0 || row >= model_->rowCountQml()) return;
    const QString id = model_->idForRow(row);
    const bool wasDirty = dirty_.remove(id) > 0;
    // Every caret move between blocks lands here: only re-check when the
    // block actually changed since its last result (or a check is owed).
    if (!wasDirty && model_->typeForRow(row) != BlockModel::Table) {
        const auto it = cache_.constFind(id);
        if (it != cache_.constEnd() && !it->pending && it->checkedText == model_->contentForRow(row)) return;
    }
    scheduleRow(row, true, true);
}

void SpellService::requestSuggestions(int row) {
    if (!model_ || row < 0 || row >= model_->rowCountQml()) return;
    const QString id = model_->idForRow(row);
    const auto it = cache_.constFind(id);
    if (it != cache_.constEnd() && it->suggestionsComputed && !it->pending
        && it->checkedText == model_->contentForRow(row)) return;
    scheduleRow(row, true, true);
}

void SpellService::backgroundTick() {
    if (!model_) return;
    const int n = model_->rowCountQml();
    int done = 0;
    while (passRow_ < n && done < kPassChunk) {
        const int row = passRow_++;
        const QString id = model_->idForRow(row);
        const int type = model_->typeForRow(row);
        const bool have = (type == BlockModel::Table) ? cache_.contains(cellKey(id, 0, 0)) : cache_.contains(id);
        if (have) continue;
        scheduleRow(row, /*front=*/false, /*wantSug=*/false);
        ++done;
    }
    if (passRow_ < n) passTimer_.start();
}

// ---- worker ---------------------------------------------------------------

void SpellService::submit(Job j, bool front) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                    [&](const Job& q) { return !q.control && q.key == j.key; }), queue_.end());
        if (front) queue_.push_front(std::move(j)); else queue_.push_back(std::move(j));
    }
    cv_.notify_one();
}

void SpellService::submitControl(std::function<void(SpellEngine&)> fn) {
    Job j; j.gen = gen_; j.control = std::move(fn);
    { std::lock_guard<std::mutex> lk(mu_); queue_.push_front(std::move(j)); }
    cv_.notify_one();
}

void SpellService::workerMain() {
    std::unique_ptr<SpellEngine> engine;
    bool initTried = false;
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return quit_ || !queue_.empty(); });
            if (quit_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
        }
        if (!engine && !initTried) {
            initTried = true;
            QString err;
            const QString dir = spell::ensureDictionaryExtracted(appVersion_, &err);
            auto e = std::make_unique<SpellEngine>();
            if (!dir.isEmpty() && e->init(dir + QStringLiteral("/en.aff"), dir + QStringLiteral("/en.dic"),
                                          spell::loadRulesXml(), spell::loadUserDictionary(), &err)) {
                engine = std::move(e);
                QMetaObject::invokeMethod(this, [this] { ready_ = true; emit readyChanged(); }, Qt::QueuedConnection);
            } else {
                qWarning("SpellService: engine init failed: %s", qPrintable(err));
            }
        }
        if (engine) {
            if (job.control) {
                job.control(*engine);
            } else {
                SpellEngine::Options opt;
                opt.spelling = true; opt.grammar = true; opt.wantSuggestions = job.wantSuggestions;
                opt.maxSuggestedIssues = 1000;                  // "Fix all in block" needs every one (79 in 335 ms measured)
                Result res;
                res.gen = job.gen; res.key = job.key; res.checkedText = job.text;
                res.suggestionsComputed = job.wantSuggestions;
                res.issues = engine->checkText(job.text, job.excluded, opt);
                QMetaObject::invokeMethod(this, [this, res] { applyResult(res); }, Qt::QueuedConnection);
            }
        }
        { std::lock_guard<std::mutex> lk(mu_); busy_ = false; }
    }
}

void SpellService::applyResult(const Result& r) {
    if (r.gen != gen_) return;                           // another document by now
    Entry& e = cache_[r.key];
    e.checkedText = r.checkedText;
    e.issues = r.issues;
    e.pending = false;
    e.suggestionsComputed = r.suggestionsComputed;
    ++revision_;
    emit revisionChanged();
}

// ---- reads ----------------------------------------------------------------

QVariantMap SpellService::toVariant(const spell::SpellIssue& is) {
    QVariantMap m;
    m.insert(QStringLiteral("s"), is.s);
    m.insert(QStringLiteral("e"), is.e);
    m.insert(QStringLiteral("kind"), int(is.kind));
    m.insert(QStringLiteral("ruleId"), is.ruleId);
    m.insert(QStringLiteral("ruleName"), is.ruleName);
    m.insert(QStringLiteral("message"), is.message);
    m.insert(QStringLiteral("suggestions"), is.suggestions);
    m.insert(QStringLiteral("suggestionsComputed"), is.suggestionsComputed);
    return m;
}

QVariantList SpellService::filtered(const Entry& e, const QString& textNow, int caretCol) const {
    QVariantList out;
    for (const spell::SpellIssue& is : e.issues) {
        if (is.kind == spell::IssueKind::Spelling) {
            if (!checkSpelling_) continue;
            const QString w = textNow.mid(is.s, is.e - is.s).toLower();
            if (userWords_.contains(w) || ignoredWords_.contains(w)) continue;
        } else {
            if (!checkGrammar_ || ignoredRules_.contains(is.ruleId)) continue;
        }
        if (caretCol >= is.s && caretCol <= is.e) continue;
        out.push_back(toVariant(is));
    }
    return out;
}

QVariantList SpellService::issuesForRow(int row, int caretCol) {
    if (!model_ || row < 0 || row >= model_->rowCountQml() || (!checkSpelling_ && !checkGrammar_)) return {};
    const QString id = model_->idForRow(row);
    const QString text = model_->contentForRow(row);
    const auto it = cache_.constFind(id);
    if (it == cache_.constEnd() || (it->checkedText != text && !it->pending)) {
        if (!dirty_.contains(id)) scheduleRow(row, true, false);
        return {};
    }
    if (it->checkedText != text) return {};
    return filtered(*it, text, caretCol);
}

QVariantList SpellService::issuesForCell(int row, int r, int c, int caretCol) {
    if (!model_ || row < 0 || row >= model_->rowCountQml() || (!checkSpelling_ && !checkGrammar_)) return {};
    const QString key = cellKey(model_->idForRow(row), r, c);
    const QString text = model_->tableCell(row, r, c);
    const auto it = cache_.constFind(key);
    if (it == cache_.constEnd() || (it->checkedText != text && !it->pending)) {
        if (!dirty_.contains(model_->idForRow(row))) scheduleRow(row, true, false);
        return {};
    }
    if (it->checkedText != text) return {};
    return filtered(*it, text, caretCol);
}

QVariantMap SpellService::issueAt(int row, int col) {
    for (const QVariant& v : issuesForRow(row, -1)) {
        const QVariantMap m = v.toMap();
        if (col >= m.value(QStringLiteral("s")).toInt() && col <= m.value(QStringLiteral("e")).toInt()) {
            if (!m.value(QStringLiteral("suggestionsComputed")).toBool()) scheduleRow(row, true, true);
            return m;
        }
    }
    return {};
}

QVariantMap SpellService::cellIssueAt(int row, int r, int c, int col) {
    for (const QVariant& v : issuesForCell(row, r, c, -1)) {
        const QVariantMap m = v.toMap();
        if (col >= m.value(QStringLiteral("s")).toInt() && col <= m.value(QStringLiteral("e")).toInt()) {
            if (!m.value(QStringLiteral("suggestionsComputed")).toBool()) scheduleRow(row, true, true);
            return m;
        }
    }
    return {};
}

QVariantList SpellService::checkTextNow(const QString& text) {
    if (!syncEngine_) {
        QString err;
        const QString dir = spell::ensureDictionaryExtracted(appVersion_, &err);
        auto e = std::make_unique<SpellEngine>();
        if (dir.isEmpty() || !e->init(dir + QStringLiteral("/en.aff"), dir + QStringLiteral("/en.dic"),
                                      spell::loadRulesXml(), spell::loadUserDictionary(), &err)) {
            qWarning("SpellService: sync engine init failed: %s", qPrintable(err));
            return {};
        }
        syncEngine_ = std::move(e);
    }
    syncEngine_->setIgnoredWords(ignoredWords_);
    syncEngine_->setDisabledRules(ignoredRules_);
    QVariantList out;
    SpellEngine::Options opt;
    for (const spell::SpellIssue& is : syncEngine_->checkText(text, {}, opt)) out.push_back(toVariant(is));
    return out;
}

bool SpellService::waitIdle(int timeoutMs) {
    QElapsedTimer t; t.start();
    for (;;) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        bool idle;
        { std::lock_guard<std::mutex> lk(mu_); idle = queue_.empty() && !busy_; }
        if (idle && !debounce_.isActive() && !passTimer_.isActive()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);   // drain queued applyResult calls
            return true;
        }
        if (t.elapsed() > timeoutMs) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
