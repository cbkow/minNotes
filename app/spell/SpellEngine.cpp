#include "SpellEngine.h"
#include "SpellTokenizer.h"

bool SpellEngine::init(const QString& affPath, const QString& dicPath, const QByteArray& rulesXml,
                       const QStringList& userWords, QString* error) {
    if (!hs_.load(affPath, dicPath, error)) return false;
    for (const QString& w : userWords) addUserWord(w.trimmed());
    if (!rulesXml.isEmpty() && !rules_.load(rulesXml, error)) return false;
    return true;
}

std::vector<spell::SpellIssue> SpellEngine::checkText(const QString& text,
                                                      const std::vector<std::pair<int,int>>& excluded,
                                                      const Options& opt) const {
    std::vector<spell::SpellIssue> out;
    if (text.isEmpty() || !hs_.isLoaded()) return out;
    auto excludedAt = [&](int s, int e) {
        for (const auto& x : excluded) if (s < x.second && e > x.first) return true;
        return false;
    };
    const spell::KnownWordFn known = [this](const QString& w) { return hs_.spell(w); };

    if (opt.spelling) {
        int suggested = 0;
        for (const spell::RawWord& w : spell::rawWords(text, known)) {
            const QString norm = spell::normalizeApostrophes(w.text);
            if (!spell::spellCheckable(norm) || excludedAt(w.s, w.e)) continue;
            if (w.s > 0 && (text[w.s - 1] == QLatin1Char('#') || text[w.s - 1] == QLatin1Char('@'))) continue;
            const QString lower = norm.toLower();
            if (userWords_.contains(lower) || ignoredWords_.contains(lower)) continue;
            if (hs_.spell(norm)) continue;
            spell::SpellIssue is;
            is.s = w.s; is.e = w.e; is.kind = spell::IssueKind::Spelling;
            is.message = QStringLiteral("Possible spelling mistake");
            if (opt.wantSuggestions && suggested < opt.maxSuggestedIssues) {
                is.suggestions = hs_.suggest(norm, 5);
                ++suggested;
            } else {
                is.suggestionsComputed = false;
            }
            out.push_back(is);
        }
    }
    if (opt.grammar && rules_.ruleCount() > 0) {
        std::vector<spell::SentenceSpan> sents;
        const auto toks = spell::tokenize(text, known, &sents);
        for (const GrammarRules::Hit& h : rules_.check(text, toks, sents, disabledRules_)) {
            if (excludedAt(h.s, h.e)) continue;
            spell::SpellIssue is;
            is.s = h.s; is.e = h.e; is.kind = spell::IssueKind::Grammar;
            is.ruleId = h.ruleId; is.ruleName = h.ruleName; is.message = h.message;
            is.suggestions = h.suggestions.mid(0, 5);
            out.push_back(is);
        }
    }
    std::sort(out.begin(), out.end(), [](const spell::SpellIssue& a, const spell::SpellIssue& b) { return a.s < b.s; });
    return out;
}
