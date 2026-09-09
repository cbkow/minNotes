// SpellService — the GUI-thread face of the checker (context property `spell`).
// Follows the active BlockModel, keeps results keyed by block id (delegates
// are pooled, rows shift), debounces edits per block (~300 ms), runs the
// engine on ONE persistent worker thread (Hunspell is not thread-safe and the
// GUI never touches it), and hands QML filtered issue lists that depend on
// `revision`. Quick-free (Core + Gui) so the headless test target links it.
#pragma once

#include "SpellTypes.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

class BlockModel;
class SpellEngine;

class SpellService : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool checkSpelling READ checkSpelling WRITE setCheckSpelling NOTIFY checkSpellingChanged)
    Q_PROPERTY(bool checkGrammar READ checkGrammar WRITE setCheckGrammar NOTIFY checkGrammarChanged)
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(QString ignoredRulesJson READ ignoredRulesJson WRITE setIgnoredRulesJson NOTIFY ignoredRulesChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
public:
    explicit SpellService(const QString& appVersion, QObject* parent = nullptr);
    ~SpellService() override;

    void setModel(BlockModel* m);
    BlockModel* model() const { return model_; }

    bool checkSpelling() const { return checkSpelling_; }
    void setCheckSpelling(bool on);
    bool checkGrammar() const { return checkGrammar_; }
    void setCheckGrammar(bool on);
    int revision() const { return revision_; }
    QString ignoredRulesJson() const;
    void setIgnoredRulesJson(const QString& json);
    bool ready() const { return ready_; }

    // Issues for a text block / a table text cell, filtered by the toggles,
    // the user + ignored words, the ignored rules, and the caret (an issue
    // under a collapsed caret is withheld — the word is still being typed).
    // An unchecked key queues a front (visible-first) job and returns [].
    Q_INVOKABLE QVariantList issuesForRow(int row, int caretCol = -1);
    Q_INVOKABLE QVariantList issuesForCell(int row, int r, int c, int caretCol = -1);
    Q_INVOKABLE QVariantMap issueAt(int row, int col);               // {} = none; upgrades lazy suggestions
    Q_INVOKABLE QVariantMap cellIssueAt(int row, int r, int c, int col);
    Q_INVOKABLE void flushRow(int row);                              // bypass the debounce (focus-out, after a replace)
    Q_INVOKABLE void requestSuggestions(int row);                    // menu open: suggestions for EVERY issue in the block
    Q_INVOKABLE void addToDictionary(const QString& word);
    Q_INVOKABLE void ignoreWord(const QString& word);                // session only
    Q_INVOKABLE void ignoreRule(const QString& ruleId);
    Q_INVOKABLE void unignoreRule(const QString& ruleId);
    Q_INVOKABLE QStringList ignoredRules() const;
    Q_INVOKABLE int cachedEntries() const { return cache_.size(); }

    // Synchronous check on the caller's thread with a private engine (tests).
    Q_INVOKABLE QVariantList checkTextNow(const QString& text);
    // Tests: pump events until the worker queue is drained (or the timeout).
    Q_INVOKABLE bool waitIdle(int timeoutMs = 5000);

    struct Result {
        quint64 gen = 0;
        QString key;
        QString checkedText;
        std::vector<spell::SpellIssue> issues;
        bool suggestionsComputed = true;
    };
    void applyResult(const Result& r);          // public for the tests

signals:
    void checkSpellingChanged();
    void checkGrammarChanged();
    void revisionChanged();
    void ignoredRulesChanged();
    void readyChanged();

private:
    struct Job {
        quint64 gen = 0;
        QString key;
        QString text;
        std::vector<std::pair<int,int>> excluded;
        bool wantSuggestions = true;
        std::function<void(SpellEngine&)> control;   // control jobs carry no text
    };
    struct Entry {
        QString checkedText;
        std::vector<spell::SpellIssue> issues;
        bool pending = false;
        bool suggestionsComputed = true;
    };
    static QString cellKey(const QString& id, int r, int c);
    void onDataChanged(const QModelIndex& tl, const QModelIndex& br, const QList<int>& roles);
    void markDirty(const QString& id, int row);
    void flushDue();
    void submit(Job j, bool front);
    void submitControl(std::function<void(SpellEngine&)> fn);
    void scheduleRow(int row, bool front, bool wantSuggestions);
    bool jobForKey(const QString& key, int rowHint, bool wantSug, Job* out);
    int rowForKey(const QString& id, int hint) const;
    void backgroundTick();
    void workerMain();
    QVariantList filtered(const Entry& e, const QString& textNow, int caretCol) const;
    static QVariantMap toVariant(const spell::SpellIssue& is);
    std::vector<std::pair<int,int>> excludedSpans(const QVariantList& spans) const;

    BlockModel* model_ = nullptr;
    quint64 gen_ = 0;
    int revision_ = 0;
    QString appVersion_;
    QHash<QString, Entry> cache_;
    QHash<QString, int> dirty_;                  // key → row hint
    QHash<QString, int> rowHint_;                // block id → last row seen
    QTimer debounce_, passTimer_;
    int passRow_ = 0;
    std::thread worker_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    bool quit_ = false, busy_ = false;
    std::unique_ptr<SpellEngine> syncEngine_;    // checkTextNow only
    QSet<QString> userWords_, ignoredWords_, ignoredRules_;
    bool checkSpelling_ = true, checkGrammar_ = true, ready_ = false;
    std::vector<QMetaObject::Connection> conns_;
};
