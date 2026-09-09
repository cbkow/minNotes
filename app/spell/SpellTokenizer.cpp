#include "SpellTokenizer.h"

#include <QRegularExpression>
#include <QSet>

namespace spell {

namespace {

bool isApos(QChar c) {
    return c == QLatin1Char('\'') || c == QChar(0x2019) || c == QChar(0x2018)
        || c == QChar(0x0060) || c == QChar(0x00B4);
}
bool isWordChar(QChar c) { return c.isLetterOrNumber(); }
// LanguageTool's extra word characters (currency, percent, ampersand, degree…):
// "50%" and "$100" are single tokens there, so they are here too.
bool isExtraWordChar(QChar c) {
    static const QString extra = QStringLiteral("±§©@€£¥$%‰‱&°");
    return extra.contains(c);
}

// Protected ranges (URLs / emails) so their punctuation never splits.
struct Range { int s, e; TokKind kind; };
std::vector<Range> protectedRanges(const QString& text) {
    static const QRegularExpression url(QStringLiteral(R"((?:https?://|www\.)[^\s<>"']+)"),
                                        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression mail(QStringLiteral(R"([\w.+\-]+@[\w\-]+(?:\.[\w\-]+)+)"),
                                         QRegularExpression::UseUnicodePropertiesOption);
    std::vector<Range> out;
    for (auto it = url.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        int e = int(m.capturedEnd());
        while (e > m.capturedStart() && QStringLiteral(".,;:!?)").contains(text[e - 1])) --e;   // trailing punctuation
        out.push_back({int(m.capturedStart()), e, TokKind::Url});
    }
    for (auto it = mail.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        bool inside = false;
        for (const Range& r : out) if (m.capturedStart() < r.e && m.capturedEnd() > r.s) inside = true;
        if (!inside) out.push_back({int(m.capturedStart()), int(m.capturedEnd()), TokKind::Email});
    }
    std::sort(out.begin(), out.end(), [](const Range& a, const Range& b) { return a.s < b.s; });
    return out;
}

// A word run: letters/digits with internal apostrophes (letter ' letter) and
// internal hyphens (wordchar - wordchar). Returns the end index.
int scanWord(const QString& t, int i, bool extra = false) {
    const int n = t.size();
    int j = i;
    while (j < n) {
        if (isWordChar(t[j]) || (extra && isExtraWordChar(t[j]))) { ++j; continue; }
        if (isApos(t[j]) && j + 1 < n && t[j + 1].isLetter() && j > i) { ++j; continue; }
        if (t[j] == QLatin1Char('-') && j + 1 < n && isWordChar(t[j + 1]) && j > i) { ++j; continue; }
        break;
    }
    return j;
}

} // namespace

QString normalizeApostrophes(QString w) {
    for (QChar& c : w) if (isApos(c)) c = QLatin1Char('\'');
    return w;
}

bool spellCheckable(const QString& word) {
    if (word.size() < 2) return false;
    bool allUpper = true, sawLower = false;
    for (int i = 0; i < word.size(); ++i) {
        const QChar c = word[i];
        if (c.isDigit() || c == QLatin1Char('_')) return false;
        if (!c.isLetter() && c != QLatin1Char('\'') && c != QLatin1Char('-')) return false;
        if (c.isLetter()) {
            if (c.isLower()) { sawLower = true; allUpper = false; }
            else if (c.isUpper() && sawLower && i > 0 && word[i - 1].isLetter()) return false;   // camelCase
        }
    }
    if (allUpper) return false;   // ALLCAPS (initialisms, codes)
    return true;
}

std::vector<RawWord> rawWords(const QString& text, const KnownWordFn& known) {
    std::vector<RawWord> out;
    const auto prot = protectedRanges(text);
    size_t pi = 0;
    const int n = text.size();
    int i = 0;
    while (i < n) {
        while (pi < prot.size() && prot[pi].e <= i) ++pi;
        if (pi < prot.size() && i >= prot[pi].s) { i = prot[pi].e; continue; }
        if (!isWordChar(text[i])) { ++i; continue; }
        int j = scanWord(text, i);
        if (pi < prot.size() && j > prot[pi].s) j = prot[pi].s;
        const QString w = text.mid(i, j - i);
        if (w.contains(QLatin1Char('-')) && !(known && known(normalizeApostrophes(w)))) {
            int k = i;                                          // split at hyphens
            while (k < j) {
                int m = k;
                while (m < j && text[m] != QLatin1Char('-')) ++m;
                if (m > k) out.push_back({k, m, text.mid(k, m - k)});
                k = m + 1;
            }
        } else {
            out.push_back({i, j, w});
        }
        i = j;
    }
    return out;
}

std::vector<SpellToken> tokenize(const QString& text, const KnownWordFn& known,
                                 std::vector<SentenceSpan>* sentences) {
    std::vector<SpellToken> raw;   // without pseudo-tokens
    const auto prot = protectedRanges(text);
    size_t pi = 0;
    const int n = text.size();
    auto push = [&](int s, int e, TokKind k) {
        SpellToken t; t.s = s; t.e = e; t.kind = k;
        t.text = normalizeApostrophes(text.mid(s, e - s));
        t.spaceBefore = s > 0 && text[s - 1].isSpace();
        raw.push_back(std::move(t));
    };
    // LanguageTool's EnglishWordTokenizer contraction patterns (v6.8), applied
    // to a word that carries an apostrophe: a recognised negation splits into
    // verb + "n't", a clitic into base + "'re" (etc.); any other apostrophe
    // word stays whole when the dictionary knows it ("o'clock"), else splits
    // AT the apostrophe into word / ' / word — which is what the typo rules
    // ("odn't", "your'e", "can'r") are written against.
    static const QRegularExpression special(QStringLiteral("^(fo'c'sle|rec'[ds]|OK'd|cc'[ds]|DJ'd|[pd]m'd|rsvp'd)$"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression negation(QStringLiteral("^(')?(are|is|were|was|do|does|did|have|has|had|wo|would|ca|could|sha|should|must|ai|ought|might|need|may|am|dare|das|dass|hai|used|use)(n't)$"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression clitic(QStringLiteral("^(.+)('(?:m|re|ll|ve|d|s))(['-]?)$"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tWas(QStringLiteral("^('t)(was|were|is)$"), QRegularExpression::CaseInsensitiveOption);
    auto pushWordPart = [&](int s, int e) {
        const QString w = normalizeApostrophes(text.mid(s, e - s));
        bool allDigit = !w.isEmpty();
        for (QChar c : w) if (!c.isDigit()) { allDigit = false; break; }
        if (allDigit) { push(s, e, TokKind::Number); return; }
        if (!w.contains(QLatin1Char('\''))) { push(s, e, TokKind::Word); return; }
        if (special.match(w).hasMatch()) { push(s, e, TokKind::Word); return; }
        auto m = negation.match(w);
        if (m.hasMatch()) {
            int at = s;
            for (int g = 1; g <= 3; ++g) {
                const int len = int(m.capturedLength(g));
                if (len > 0) { push(at, at + len, TokKind::Word); at += len; }
            }
            return;
        }
        m = clitic.match(w);
        if (m.hasMatch()) {
            int at = s;
            for (int g = 1; g <= 3; ++g) {
                const int len = int(m.capturedLength(g));
                if (len > 0) { push(at, at + len, g == 3 ? TokKind::Punct : TokKind::Word); at += len; }
            }
            return;
        }
        m = tWas.match(w);
        if (m.hasMatch()) {
            push(s, s + int(m.capturedLength(1)), TokKind::Word);
            push(s + int(m.capturedLength(1)), e, TokKind::Word);
            return;
        }
        if (known && known(w)) { push(s, e, TokKind::Word); return; }
        int k = s;                                   // split at every apostrophe
        for (int q = s; q < e; ++q) {
            if (!isApos(text[q])) continue;
            if (q > k) push(k, q, TokKind::Word);
            push(q, q + 1, TokKind::Punct);
            k = q + 1;
        }
        if (k < e) push(k, e, TokKind::Word);
    };
    int i = 0;
    while (i < n) {
        while (pi < prot.size() && prot[pi].e <= i) ++pi;
        if (pi < prot.size() && i >= prot[pi].s) { push(prot[pi].s, prot[pi].e, prot[pi].kind); i = prot[pi].e; continue; }
        const QChar c = text[i];
        if (c.isSpace()) { ++i; continue; }
        if (isWordChar(c) || isExtraWordChar(c)) {
            int j = scanWord(text, i, /*extra=*/true);
            if (pi < prot.size() && j > prot[pi].s) j = prot[pi].s;
            pushWordPart(i, j);
            i = j;
            continue;
        }
        push(i, i + 1, TokKind::Punct);
        ++i;
    }

    // Sentence bounds: after . ! ? (plus closing quotes/brackets) when the next
    // token starts upper/digit/quote and the word before isn't an abbreviation
    // or a lone capital; and at every newline.
    static const QSet<QString> abbrev = {
        QStringLiteral("mr"), QStringLiteral("mrs"), QStringLiteral("ms"), QStringLiteral("dr"),
        QStringLiteral("prof"), QStringLiteral("st"), QStringLiteral("vs"), QStringLiteral("etc"),
        QStringLiteral("e.g"), QStringLiteral("i.e"), QStringLiteral("no"), QStringLiteral("fig"),
        QStringLiteral("inc"), QStringLiteral("ltd"), QStringLiteral("jr"), QStringLiteral("sr"),
        QStringLiteral("approx"), QStringLiteral("dept"), QStringLiteral("est"), QStringLiteral("min"),
        QStringLiteral("sec"), QStringLiteral("vol"), QStringLiteral("rev"), QStringLiteral("ep") };
    auto isTerminal = [](const SpellToken& t) {
        return t.kind == TokKind::Punct && (t.text == QLatin1String(".") || t.text == QLatin1String("!")
                                            || t.text == QLatin1String("?") || t.text == QStringLiteral("…"));
    };
    auto isCloser = [](const SpellToken& t) {
        return t.kind == TokKind::Punct && QStringLiteral("\"'”’)]").contains(t.text);
    };
    auto newlineBetween = [&](int a, int b) {   // any '\n' in text[a, b)
        for (int k = a; k < b; ++k) if (text[k] == QLatin1Char('\n')) return true;
        return false;
    };
    std::vector<int> bounds;   // index of the first token of each sentence after the first
    for (size_t k = 0; k + 1 < raw.size(); ++k) {
        const SpellToken& t = raw[k];
        const SpellToken& nx = raw[k + 1];
        if (newlineBetween(t.e, nx.s)) { bounds.push_back(int(k + 1)); continue; }
        if (!isTerminal(t)) continue;
        if (isTerminal(nx) || isCloser(nx)) continue;   // "?!" / '."' → the boundary is after the run
        // The word before the terminator.
        int p = int(k) - 1;
        while (p >= 0 && isTerminal(raw[size_t(p)])) --p;
        if (p >= 0 && raw[size_t(p)].kind == TokKind::Word && t.text == QLatin1String(".")) {
            const QString w = raw[size_t(p)].text.toLower();
            if (abbrev.contains(w) || (w.size() == 1 && raw[size_t(p)].text[0].isUpper())) continue;
        }
        const QChar first = nx.text.isEmpty() ? QChar() : nx.text[0];
        if (nx.kind == TokKind::Number || first.isUpper()
            || QStringLiteral("\"“‘'([").contains(nx.text))
            bounds.push_back(int(k + 1));
    }
    // Closers after a terminator stay with the preceding sentence: move any
    // bound that lands on a closer past the closer run.
    for (int& b : bounds)
        while (b < int(raw.size()) && (isCloser(raw[size_t(b)]) || isTerminal(raw[size_t(b)]))) ++b;
    std::sort(bounds.begin(), bounds.end());
    bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());

    std::vector<SpellToken> out;
    out.reserve(raw.size() + 2 * (bounds.size() + 1));
    if (sentences) sentences->clear();
    size_t bi = 0; int sentIdx = 0;
    auto openSentence = [&](int at) {
        SpellToken ss; ss.s = ss.e = at; ss.kind = TokKind::SentStart; ss.sentence = sentIdx;
        if (sentences) sentences->push_back({int(out.size()), 0});
        out.push_back(ss);
    };
    auto closeSentence = [&](int at) {
        SpellToken se; se.s = se.e = at; se.kind = TokKind::SentEnd; se.sentence = sentIdx;
        out.push_back(se);
        if (sentences) sentences->back().endTok = int(out.size());
        ++sentIdx;
    };
    openSentence(raw.empty() ? 0 : raw[0].s);
    for (size_t k = 0; k < raw.size(); ++k) {
        if (bi < bounds.size() && int(k) == bounds[bi] && k > 0) {
            closeSentence(raw[k - 1].e);
            openSentence(raw[k].s);
            ++bi;
        }
        SpellToken t = raw[k]; t.sentence = sentIdx;
        out.push_back(std::move(t));
    }
    closeSentence(raw.empty() ? 0 : raw.back().e);
    return out;
}

} // namespace spell
