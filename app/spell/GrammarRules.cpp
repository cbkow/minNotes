#include "GrammarRules.h"

#include <QRegularExpression>
#include <QXmlStreamReader>
#include <algorithm>
#include <functional>

using spell::SpellToken;
using spell::SentenceSpan;
using spell::TokKind;

namespace {

enum class TxtKind : uint8_t { Any, Literal, Set, Regex, SentStart, SentEnd };

// Java regex classes that PCRE2 spells differently.
QString javaToPcre(QString rx) {
    static const std::pair<const char*, const char*> map[] = {
        {"\\p{Punct}", "[[:punct:]]"}, {"\\p{Alpha}", "[[:alpha:]]"}, {"\\p{Alnum}", "[[:alnum:]]"},
        {"\\p{Upper}", "\\p{Lu}"}, {"\\p{Lower}", "\\p{Ll}"}, {"\\p{Digit}", "\\d"},
        {"\\p{Space}", "\\s"}, {"\\p{Blank}", "[ \\t]"}, {"\\p{IsAlphabetic}", "\\p{L}"},
        {"\\p{javaUpperCase}", "\\p{Lu}"}, {"\\p{javaLowerCase}", "\\p{Ll}"},
    };
    for (const auto& [j, p] : map) rx.replace(QLatin1String(j), QLatin1String(p));
    // \uXXXX → \x{XXXX}; inline flag groups drop Java's 'u' (PCRE has no such flag).
    static const QRegularExpression uesc(QStringLiteral("\\\\u([0-9A-Fa-f]{4})"));
    rx.replace(uesc, QStringLiteral("\\x{\\1}"));
    static const QRegularExpression flags(QStringLiteral("\\(\\?([a-zA-Z]*)u([a-zA-Z]*)\\)"));
    rx.replace(flags, QStringLiteral("(?\\1\\2)"));
    rx.replace(QStringLiteral("(?)"), QString());
    return rx;
}

struct TextTest {
    TxtKind kind = TxtKind::Any;
    QString lit;                 // Literal (lowercased when !cs)
    QSet<QString> set;           // Set (lowercased when !cs)
    QRegularExpression rx;       // Regex
    bool cs = false;
    bool negate = false;
    bool ok = true;              // false = regex failed to compile
    bool matches(const SpellToken& t) const {
        bool m;
        switch (kind) {
        case TxtKind::Any:       m = true; break;
        case TxtKind::SentStart: m = t.kind == TokKind::SentStart; break;
        case TxtKind::SentEnd:   m = t.kind == TokKind::SentEnd; break;
        case TxtKind::Literal:
            m = t.kind != TokKind::SentStart && t.kind != TokKind::SentEnd
                && (cs ? t.text == lit : t.text.compare(lit, Qt::CaseInsensitive) == 0); break;
        case TxtKind::Set:
            m = t.kind != TokKind::SentStart && t.kind != TokKind::SentEnd
                && set.contains(cs ? t.text : t.text.toLower()); break;
        case TxtKind::Regex:
            m = t.kind != TokKind::SentStart && t.kind != TokKind::SentEnd && rx.match(t.text).hasMatch(); break;
        }
        return negate ? !m : m;
    }
};

// Build the text test from element text + attributes (regexp / case_sensitive / postag).
TextTest makeTextTest(const QString& rawText, bool isRegexp, bool cs, const QString& postag, bool negate, bool* compileOk) {
    TextTest tt; tt.cs = cs; tt.negate = negate;
    if (postag == QLatin1String("SENT_START")) { tt.kind = TxtKind::SentStart; return tt; }
    if (postag == QLatin1String("SENT_END"))   { tt.kind = TxtKind::SentEnd;   return tt; }
    const QString text = rawText.trimmed();
    if (text.isEmpty()) { tt.kind = TxtKind::Any; return tt; }
    if (!isRegexp) { tt.kind = TxtKind::Literal; tt.lit = cs ? text : text.toLower(); return tt; }
    // Pure alternation of plain words → a set (fast, and it feeds the anchor index).
    static const QRegularExpression plainAlt(QStringLiteral(R"(^[\p{L}\p{N}'’\-\.]+(\|[\p{L}\p{N}'’\-\.]+)*$)"),
                                             QRegularExpression::UseUnicodePropertiesOption);
    if (plainAlt.match(text).hasMatch() && !text.contains(QLatin1Char('.'))) {
        tt.kind = TxtKind::Set;
        for (const QString& a : text.split(QLatin1Char('|'))) tt.set.insert(cs ? a : a.toLower());
        return tt;
    }
    tt.kind = TxtKind::Regex;
    QRegularExpression::PatternOptions opts = QRegularExpression::UseUnicodePropertiesOption;
    if (!cs) opts |= QRegularExpression::CaseInsensitiveOption;
    tt.rx = QRegularExpression(QStringLiteral("\\A(?:") + javaToPcre(text) + QStringLiteral(")\\z"), opts);
    if (!tt.rx.isValid()) { tt.ok = false; if (compileOk) *compileOk = false; }
    return tt;
}

struct Exception {
    TextTest test;
    int scope = 0;               // 0 current, -1 previous, +1 next
    int spaceBefore = -1;        // -1 any, 0 no, 1 yes
};

struct TokenSpec {
    TextTest test;
    std::vector<Exception> exc;
    int skip = 0;
    int min = 1, max = 1;
    int spaceBefore = -1;
    int matchRef = -1;           // <token><match no="K"/></token>, 0-based pattern index
    bool marker = false;
    QString anchorKey;           // literal (lowercased) for the anchor index; "" = none
    int anchorAlternatives = 0;
};

struct Pattern {
    std::vector<TokenSpec> toks;
    bool hasMarker = false;
};

struct SugPart {
    bool isMatch = false;
    QString text;                // literal text (with \N backrefs) when !isMatch
    int no = 0;                  // 1-based token reference
    QString conv;                // case_conversion
    bool hasRx = false;
    QRegularExpression rxMatch;
    QString rxReplace;
    bool includeSkipped = false;
};

struct Rule {
    QString id, name, category;
    std::vector<Pattern> anti;
    Pattern pat;
    bool isRegexp = false;
    QRegularExpression rx;
    int mark = 0;
    std::vector<std::vector<SugPart>> suggestions;
    std::vector<SugPart> message;    // message parts: text, or a suggestion index encoded as isMatch=false & no=-k
    std::vector<int> messageSugs;    // which suggestion indices appear in the message (in order)
    bool disabled = false;
};

// One pattern token's capture: token index range [a, b) plus the skipped
// range that FOLLOWED it (for include_skipped).
struct Cap { int a = 0, b = 0, skipEnd = 0; };

bool tokenMatches(const TokenSpec& ts, const std::vector<SpellToken>& toks, int i, int sentFirst, int sentEnd,
                  const std::vector<Cap>& caps) {
    const SpellToken& t = toks[size_t(i)];
    if (ts.matchRef >= 0) {
        if (ts.matchRef >= int(caps.size())) return false;
        const Cap& c = caps[size_t(ts.matchRef)];
        if (c.a >= c.b) return false;
        const QString& ref = toks[size_t(c.a)].text;
        if (t.kind == TokKind::SentStart || t.kind == TokKind::SentEnd) return false;
        if (ts.test.cs ? t.text != ref : t.text.compare(ref, Qt::CaseInsensitive) != 0) return false;
    } else if (!ts.test.matches(t)) {
        return false;
    }
    if (ts.spaceBefore >= 0 && (t.spaceBefore ? 1 : 0) != ts.spaceBefore) return false;
    for (const Exception& ex : ts.exc) {
        if (ex.scope != 0) continue;       // previous/next handled by the caller
        if (ex.test.matches(t) && (ex.spaceBefore < 0 || (t.spaceBefore ? 1 : 0) == ex.spaceBefore)) return false;
    }
    // previous-scope exceptions
    for (const Exception& ex : ts.exc) {
        if (ex.scope != -1) continue;
        if (i - 1 >= sentFirst && ex.test.matches(toks[size_t(i - 1)])) return false;
    }
    Q_UNUSED(sentEnd);
    return true;
}

// Recursive matcher: pattern token k starting at token index i. Returns true
// with `caps` filled on success.
bool matchFrom(const Pattern& p, size_t k, int i, const std::vector<SpellToken>& toks, int sentFirst, int sentEnd,
               std::vector<Cap>& caps) {
    if (k == p.toks.size()) return true;
    const TokenSpec& ts = p.toks[k];
    // Optional token (min 0): try absent first? LT is greedy — try present first, then absent.
    auto tryRun = [&](int start) -> bool {
        int cnt = 0, j = start;
        std::vector<int> ends;
        while (j < sentEnd && (ts.max < 0 || cnt < ts.max) && tokenMatches(ts, toks, j, sentFirst, sentEnd, caps)) {
            ++cnt; ++j; ends.push_back(j);
        }
        // Greedy with backtracking over the repetition count.
        for (int c = int(ends.size()); c >= std::max(1, ts.min); --c) {
            const int end = ends[size_t(c - 1)];
            // next-scope exceptions on a non-skipping token look at the following token
            bool ok = true;
            for (const Exception& ex : ts.exc) {
                if (ex.scope != 1 || ts.skip != 0) continue;
                if (end < sentEnd && ex.test.matches(toks[size_t(end)])) { ok = false; break; }
            }
            if (!ok) continue;
            // skip: the next pattern token may start up to `skip` tokens later
            const int maxSkip = ts.skip < 0 ? (sentEnd - end) : ts.skip;
            for (int sk = 0; sk <= maxSkip; ++sk) {
                const int nextStart = end + sk;
                if (nextStart > sentEnd) break;
                if (sk > 0) {   // next-scope exceptions test every skipped token
                    bool bad = false;
                    for (const Exception& ex : ts.exc) {
                        if (ex.scope != 1) continue;
                        for (int q = end; q < nextStart && !bad; ++q)
                            if (ex.test.matches(toks[size_t(q)])) bad = true;
                    }
                    if (bad) break;
                }
                caps.push_back({start, end, nextStart});
                if (matchFrom(p, k + 1, nextStart, toks, sentFirst, sentEnd, caps)) return true;
                caps.pop_back();
                if (ts.skip == 0) break;
            }
        }
        return false;
    };
    if (i <= sentEnd && tryRun(i)) return true;
    if (ts.min == 0) {
        caps.push_back({i, i, i});
        if (matchFrom(p, k + 1, i, toks, sentFirst, sentEnd, caps)) return true;
        caps.pop_back();
    }
    return false;
}

QString applyCase(QString s, const QString& conv, const QString& ref) {
    if (s.isEmpty()) return s;
    if (conv == QLatin1String("startupper") || conv == QLatin1String("firstupper")) { s[0] = s[0].toUpper(); return s; }
    if (conv == QLatin1String("startlower")) { s[0] = s[0].toLower(); return s; }
    if (conv == QLatin1String("allupper")) return s.toUpper();
    if (conv == QLatin1String("alllower")) return s.toLower();
    if (conv == QLatin1String("preserve")) {
        bool upper = ref.size() > 1; for (QChar c : ref) if (c.isLetter() && !c.isUpper()) { upper = false; break; }
        if (upper) return s.toUpper();
        if (!ref.isEmpty() && ref[0].isUpper()) { s[0] = s[0].toUpper(); return s; }
        return s;
    }
    return s;
}

} // namespace

struct GrammarRules::Impl {
    std::vector<Rule> rules;
    QHash<QString, std::vector<int>> anchorIndex;   // lowercased literal → rule indices
    std::vector<int> unanchored;
    QHash<QString, QString> names;
    QStringList compileErrors;

    // ---- parsing ----
    static QString innerTextOnly(QXmlStreamReader& x) {   // reads until the current element closes; returns concatenated text
        QString out;
        int depth = 1;
        while (!x.atEnd() && depth > 0) {
            x.readNext();
            if (x.isStartElement()) ++depth;
            else if (x.isEndElement()) --depth;
            else if (x.isCharacters()) out += x.text();
        }
        return out;
    }
    static std::vector<SugPart> parseParts(QXmlStreamReader& x, const QString& stopTag, std::vector<int>* messageSugs,
                                           std::vector<std::vector<SugPart>>* sugStore) {
        // Parses mixed content of <suggestion> or <message>: text + <match> (+ nested <suggestion> for message).
        std::vector<SugPart> parts;
        while (!x.atEnd()) {
            x.readNext();
            if (x.isEndElement() && x.name() == stopTag) break;
            if (x.isCharacters()) {
                SugPart p; p.text = x.text().toString(); parts.push_back(p);
            } else if (x.isStartElement() && x.name() == QLatin1String("match")) {
                SugPart p; p.isMatch = true;
                p.no = x.attributes().value(QLatin1String("no")).toInt();
                p.conv = x.attributes().value(QLatin1String("case_conversion")).toString();
                p.includeSkipped = x.attributes().value(QLatin1String("include_skipped")) == QLatin1String("all");
                const QString rm = x.attributes().value(QLatin1String("regexp_match")).toString();
                if (!rm.isEmpty()) {
                    p.hasRx = true;
                    p.rxMatch = QRegularExpression(javaToPcre(rm), QRegularExpression::UseUnicodePropertiesOption);
                    p.rxReplace = x.attributes().value(QLatin1String("regexp_replace")).toString();
                }
                innerTextOnly(x);   // consume to </match>
                parts.push_back(p);
            } else if (x.isStartElement() && x.name() == QLatin1String("suggestion") && sugStore) {
                std::vector<SugPart> inner = parseParts(x, QStringLiteral("suggestion"), nullptr, nullptr);
                sugStore->push_back(inner);
                if (messageSugs) messageSugs->push_back(int(sugStore->size()) - 1);
                SugPart ref; ref.no = -int(sugStore->size());   // negative = suggestion reference
                parts.push_back(ref);
            } else if (x.isStartElement()) {
                innerTextOnly(x);
            }
        }
        return parts;
    }
    Pattern parsePattern(QXmlStreamReader& x, const QString& stopTag, bool* compileOk) {
        Pattern p;
        const bool patCs = x.attributes().value(QLatin1String("case_sensitive")) == QLatin1String("yes");
        bool inMarker = false;
        while (!x.atEnd()) {
            x.readNext();
            if (x.isEndElement() && x.name() == stopTag) break;
            if (x.isStartElement() && x.name() == QLatin1String("marker")) { inMarker = true; p.hasMarker = true; continue; }
            if (x.isEndElement() && x.name() == QLatin1String("marker")) { inMarker = false; continue; }
            if (x.isStartElement() && x.name() == QLatin1String("token")) {
                const auto a = x.attributes();
                TokenSpec ts;
                ts.marker = inMarker;
                const bool cs = a.hasAttribute(QLatin1String("case_sensitive"))
                    ? a.value(QLatin1String("case_sensitive")) == QLatin1String("yes") : patCs;
                const bool isRx = a.value(QLatin1String("regexp")) == QLatin1String("yes");
                const bool neg = a.value(QLatin1String("negate")) == QLatin1String("yes");
                ts.skip = a.hasAttribute(QLatin1String("skip")) ? a.value(QLatin1String("skip")).toInt() : 0;
                ts.min = a.hasAttribute(QLatin1String("min")) ? a.value(QLatin1String("min")).toInt() : 1;
                ts.max = a.hasAttribute(QLatin1String("max")) ? a.value(QLatin1String("max")).toInt() : std::max(1, ts.min);
                if (a.hasAttribute(QLatin1String("spacebefore")))
                    ts.spaceBefore = a.value(QLatin1String("spacebefore")) == QLatin1String("yes") ? 1 : 0;
                const QString postag = a.value(QLatin1String("postag")).toString();
                // Children: text, <exception>, <match>
                QString text;
                int depth = 1;
                while (!x.atEnd() && depth > 0) {
                    x.readNext();
                    if (x.isCharacters()) { if (depth == 1) text += x.text(); }
                    else if (x.isStartElement() && x.name() == QLatin1String("exception")) {
                        const auto ea = x.attributes();
                        Exception ex;
                        const bool ecs = ea.hasAttribute(QLatin1String("case_sensitive"))
                            ? ea.value(QLatin1String("case_sensitive")) == QLatin1String("yes") : cs;
                        const QString sc = ea.value(QLatin1String("scope")).toString();
                        ex.scope = sc == QLatin1String("next") ? 1 : sc == QLatin1String("previous") ? -1 : 0;
                        if (ea.hasAttribute(QLatin1String("spacebefore")))
                            ex.spaceBefore = ea.value(QLatin1String("spacebefore")) == QLatin1String("yes") ? 1 : 0;
                        const QString epos = ea.value(QLatin1String("postag")).toString();
                        const bool erx = ea.value(QLatin1String("regexp")) == QLatin1String("yes");
                        const bool eneg = ea.value(QLatin1String("negate")) == QLatin1String("yes");
                        const QString etext = innerTextOnly(x);
                        ex.test = makeTextTest(etext, erx, ecs, epos, eneg, compileOk);
                        ts.exc.push_back(ex);
                    } else if (x.isStartElement() && x.name() == QLatin1String("match")) {
                        ts.matchRef = x.attributes().value(QLatin1String("no")).toInt();
                        innerTextOnly(x);
                    } else if (x.isStartElement()) { ++depth; }
                    else if (x.isEndElement()) { --depth; }
                }
                ts.test = makeTextTest(text, isRx, cs, postag, neg, compileOk);
                if (ts.test.kind == TxtKind::Literal && !neg && ts.min >= 1) { ts.anchorKey = ts.test.cs ? ts.test.lit.toLower() : ts.test.lit; ts.anchorAlternatives = 1; }
                else if (ts.test.kind == TxtKind::Set && !neg && ts.min >= 1) { ts.anchorAlternatives = ts.test.set.size(); }
                p.toks.push_back(ts);
            }
        }
        if (!p.hasMarker) for (TokenSpec& t : p.toks) t.marker = true;
        return p;
    }

    bool load(const QByteArray& xml, QString* error) {
        rules.clear(); anchorIndex.clear(); unanchored.clear(); names.clear(); compileErrors.clear();
        QXmlStreamReader x(xml);
        QString category;
        while (!x.atEnd()) {
            x.readNext();
            if (!x.isStartElement()) continue;
            if (x.name() == QLatin1String("category")) { category = x.attributes().value(QLatin1String("id")).toString(); continue; }
            if (x.name() != QLatin1String("rule")) continue;
            Rule r; r.category = category;
            r.id = x.attributes().value(QLatin1String("id")).toString();
            r.name = x.attributes().value(QLatin1String("name")).toString();
            bool compileOk = true;
            while (!x.atEnd()) {
                x.readNext();
                if (x.isEndElement() && x.name() == QLatin1String("rule")) break;
                if (!x.isStartElement()) continue;
                const auto tag = x.name();
                if (tag == QLatin1String("antipattern")) r.anti.push_back(parsePattern(x, QStringLiteral("antipattern"), &compileOk));
                else if (tag == QLatin1String("pattern")) r.pat = parsePattern(x, QStringLiteral("pattern"), &compileOk);
                else if (tag == QLatin1String("regexp")) {
                    r.isRegexp = true;
                    const bool cs = x.attributes().value(QLatin1String("case_sensitive")) == QLatin1String("yes");
                    r.mark = x.attributes().value(QLatin1String("mark")).toInt();
                    QRegularExpression::PatternOptions o = QRegularExpression::UseUnicodePropertiesOption;
                    if (!cs) o |= QRegularExpression::CaseInsensitiveOption;
                    r.rx = QRegularExpression(javaToPcre(innerTextOnly(x).trimmed()), o);
                    if (!r.rx.isValid()) compileOk = false;
                }
                else if (tag == QLatin1String("message")) r.message = parseParts(x, QStringLiteral("message"), &r.messageSugs, &r.suggestions);
                else if (tag == QLatin1String("suggestion")) r.suggestions.push_back(parseParts(x, QStringLiteral("suggestion"), nullptr, nullptr));
                else innerTextOnly(x);
            }
            if (!compileOk) { compileErrors << r.id; continue; }
            if (!r.isRegexp && r.pat.toks.empty()) continue;
            names.insert(r.id, r.name);
            const int idx = int(rules.size());
            rules.push_back(std::move(r));
            // Anchor: the required literal/set token with the fewest alternatives.
            const Rule& rr = rules.back();
            int best = -1, bestAlt = 1 << 30;
            if (!rr.isRegexp)
                for (size_t k = 0; k < rr.pat.toks.size(); ++k) {
                    const TokenSpec& t = rr.pat.toks[k];
                    if (t.anchorAlternatives > 0 && t.anchorAlternatives < bestAlt && t.anchorAlternatives <= 64) { best = int(k); bestAlt = t.anchorAlternatives; }
                }
            if (best < 0) { unanchored.push_back(idx); continue; }
            const TokenSpec& t = rr.pat.toks[size_t(best)];
            if (t.test.kind == TxtKind::Literal) anchorIndex[t.anchorKey].push_back(idx);
            else for (const QString& a : t.test.set) anchorIndex[a.toLower()].push_back(idx);
        }
        if (x.hasError()) { if (error) *error = x.errorString(); return false; }
        return true;
    }

    // ---- rendering ----
    QString capText(const QString& text, const std::vector<SpellToken>& toks, const std::vector<Cap>& caps, int no1, bool includeSkipped) const {
        if (no1 > int(caps.size())) no1 = int(caps.size());   // LT tolerates an overshoot: the last token
        if (no1 < 1 || caps.empty()) return {};
        const Cap& c = caps[size_t(no1 - 1)];
        if (c.a >= c.b) return {};
        const int s = toks[size_t(c.a)].s;
        int endTok = c.b - 1;
        if (includeSkipped && c.skipEnd > c.b) endTok = c.skipEnd - 1;
        const int e = toks[size_t(endTok)].e;
        return text.mid(s, e - s);
    }
    QString renderParts(const std::vector<SugPart>& parts, const QString& text, const std::vector<SpellToken>& toks,
                        const std::vector<Cap>& caps, const QRegularExpressionMatch* rxm,
                        const std::vector<std::vector<SugPart>>* sugs) const {
        QString out;
        for (const SugPart& p : parts) {
            if (!p.isMatch && p.no < 0 && sugs) {   // suggestion reference inside a message
                out += QStringLiteral("“") + renderParts((*sugs)[size_t(-p.no - 1)], text, toks, caps, rxm, nullptr) + QStringLiteral("”");
                continue;
            }
            if (!p.isMatch) {
                QString t = p.text;
                for (int n = 9; n >= 1; --n) {
                    const QString ref = QStringLiteral("\\%1").arg(n);
                    if (!t.contains(ref)) continue;
                    const QString v = rxm ? rxm->captured(n) : capText(text, toks, caps, n, false);
                    t.replace(ref, v);
                }
                out += t;
                continue;
            }
            QString v = rxm ? rxm->captured(p.no) : capText(text, toks, caps, p.no, p.includeSkipped);
            const QString ref = v;
            if (p.hasRx && p.rxMatch.isValid()) v.replace(p.rxMatch, p.rxReplace);
            out += applyCase(v, p.conv, ref);
        }
        return out.simplified();
    }

    bool antipatternSuppresses(const Rule& r, const std::vector<SpellToken>& toks, int sentFirst, int sentEnd, int m0, int m1) const {
        for (const Pattern& ap : r.anti) {
            for (int st = sentFirst; st < m1; ++st) {
                std::vector<Cap> c;
                if (!matchFrom(ap, 0, st, toks, sentFirst, sentEnd, c)) continue;
                int a0 = c.front().a, a1 = 0;
                for (const Cap& cc : c) a1 = std::max(a1, cc.b);
                if (a0 < m1 && a1 > m0) return true;
            }
        }
        return false;
    }
};

GrammarRules::GrammarRules() : d_(std::make_unique<Impl>()) {}
GrammarRules::~GrammarRules() = default;
bool GrammarRules::load(const QByteArray& xml, QString* error) { return d_->load(xml, error); }
int GrammarRules::ruleCount() const { return int(d_->rules.size()); }
int GrammarRules::compileErrors() const { return d_->compileErrors.size(); }
QStringList GrammarRules::compileErrorIds() const { return d_->compileErrors; }
QString GrammarRules::ruleName(const QString& id) const { return d_->names.value(id); }

std::vector<GrammarRules::Hit> GrammarRules::check(const QString& text, const std::vector<SpellToken>& toks,
                                                   const std::vector<SentenceSpan>& sents,
                                                   const QSet<QString>& disabled) const {
    std::vector<Hit> hits;
    for (const SentenceSpan& sent : sents) {
        const int sf = sent.firstTok, se = sent.endTok;
        if (se - sf <= 2) continue;   // only the pseudo-tokens
        // Candidate rules for this sentence.
        std::vector<int> cand(d_->unanchored);
        QSet<QString> seenTxt;
        for (int i = sf; i < se; ++i) {
            const QString lt = toks[size_t(i)].text.toLower();
            if (lt.isEmpty() || seenTxt.contains(lt)) continue;
            seenTxt.insert(lt);
            const auto it = d_->anchorIndex.constFind(lt);
            if (it != d_->anchorIndex.constEnd()) cand.insert(cand.end(), it->begin(), it->end());
        }
        std::sort(cand.begin(), cand.end());
        cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
        const QString sentText = text.mid(toks[size_t(sf)].s, toks[size_t(se - 1)].e - toks[size_t(sf)].s);
        const int sentOff = toks[size_t(sf)].s;
        for (int ri : cand) {
            const Rule& r = d_->rules[size_t(ri)];
            if (disabled.contains(r.id)) continue;
            if (r.isRegexp) {
                for (auto it = r.rx.globalMatch(sentText); it.hasNext();) {
                    const auto m = it.next();
                    Hit h; h.s = sentOff + m.capturedStart(r.mark); h.e = sentOff + m.capturedEnd(r.mark);
                    if (h.s < 0 || h.e <= h.s) continue;
                    h.ruleId = r.id; h.ruleName = r.name;
                    for (const auto& sp : r.suggestions) h.suggestions << d_->renderParts(sp, text, toks, {}, &m, nullptr);
                    h.message = d_->renderParts(r.message, text, toks, {}, &m, &r.suggestions);
                    hits.push_back(h);
                }
                continue;
            }
            const TokenSpec& first = r.pat.toks.front();
            for (int i = sf; i < se; ++i) {
                if (first.min >= 1 && first.matchRef < 0 && !first.test.negate
                    && (first.test.kind == TxtKind::Literal || first.test.kind == TxtKind::Set)
                    && !first.test.matches(toks[size_t(i)])) continue;
                std::vector<Cap> caps;
                if (!matchFrom(r.pat, 0, i, toks, sf, se, caps)) continue;
                // marker span over matched, non-empty, non-pseudo tokens
                int ms = -1, me = -1, m0 = 1 << 30, m1 = 0;
                for (size_t k = 0; k < caps.size(); ++k) {
                    const Cap& c = caps[k];
                    if (c.a >= c.b) continue;
                    m0 = std::min(m0, c.a); m1 = std::max(m1, c.b);
                    if (!r.pat.toks[k].marker) continue;
                    for (int q = c.a; q < c.b; ++q) {
                        const SpellToken& t = toks[size_t(q)];
                        if (t.s == t.e) continue;
                        if (ms < 0) ms = t.s;
                        me = t.e;
                    }
                }
                if (ms < 0 || me <= ms) continue;
                if (!r.anti.empty() && d_->antipatternSuppresses(r, toks, sf, se, m0, m1)) continue;
                Hit h; h.s = ms; h.e = me; h.ruleId = r.id; h.ruleName = r.name;
                const QString marked = text.mid(ms, me - ms);
                const bool capFirst = !marked.isEmpty() && marked[0].isUpper();
                for (const auto& sp : r.suggestions) {
                    QString sug = d_->renderParts(sp, text, toks, caps, nullptr, nullptr);
                    if (capFirst && !sug.isEmpty() && sug[0].isLower()) sug[0] = sug[0].toUpper();
                    if (!sug.isEmpty() && sug != marked && !h.suggestions.contains(sug)) h.suggestions << sug;
                }
                h.message = d_->renderParts(r.message, text, toks, caps, nullptr, &r.suggestions);
                hits.push_back(h);
            }
        }
    }
    // Overlap resolution: earliest start wins, longer on ties.
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.s != b.s ? a.s < b.s : a.e > b.e; });
    std::vector<Hit> out;
    for (const Hit& h : hits) {
        if (!out.empty() && h.s < out.back().e) continue;
        out.push_back(h);
    }
    return out;
}
