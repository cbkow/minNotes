// SpellEngine — the synchronous checker: tokenizer + Hunspell + the grammar
// rule interpreter + the ignore sets. Owned by the service's worker thread
// (never touched from the GUI); tests instantiate their own.
#pragma once

#include "GrammarRules.h"
#include "HunspellBridge.h"
#include "SpellTypes.h"

#include <QSet>
#include <QString>
#include <QStringList>
#include <utility>
#include <vector>

class SpellEngine {
public:
    struct Options {
        bool spelling = true;
        bool grammar = true;
        bool wantSuggestions = true;   // false = background pass (no Hunspell::suggest)
        int maxSuggestedIssues = 20;
    };
    bool init(const QString& affPath, const QString& dicPath, const QByteArray& rulesXml,
              const QStringList& userWords, QString* error = nullptr);
    bool ready() const { return hs_.isLoaded(); }
    int ruleCount() const { return rules_.ruleCount(); }
    QStringList ruleCompileErrors() const { return rules_.compileErrorIds(); }

    // `excluded` = [s,e) ranges never flagged (inline code, choice chips).
    std::vector<spell::SpellIssue> checkText(const QString& text,
                                             const std::vector<std::pair<int,int>>& excluded,
                                             const Options& opt) const;
    QStringList suggest(const QString& word) const { return hs_.suggest(word); }
    bool known(const QString& word) const { return hs_.spell(word); }
    void addUserWord(const QString& w) { hs_.addWord(w); userWords_.insert(w.toLower()); }
    void setIgnoredWords(const QSet<QString>& s) { ignoredWords_ = s; }
    void setDisabledRules(const QSet<QString>& s) { disabledRules_ = s; }
    QString ruleName(const QString& id) const { return rules_.ruleName(id); }

private:
    HunspellBridge hs_;
    GrammarRules rules_;
    QSet<QString> userWords_, ignoredWords_, disabledRules_;
};
