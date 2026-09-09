// Shared value types for the spell + grammar checker (0.5.0). Header-only,
// Core-only — the engine, the tokenizer and the tests all speak these.
#pragma once

#include <QString>
#include <QStringList>
#include <cstdint>
#include <vector>

namespace spell {

enum class IssueKind : uint8_t { Spelling = 0, Grammar = 1 };

struct SpellIssue {
    int s = 0, e = 0;                 // UTF-16 columns into the block/cell text, [s, e)
    IssueKind kind = IssueKind::Spelling;
    QString ruleId;                   // "" for spelling; LanguageTool rule id for grammar
    QString ruleName;                 // human label ("Ignore rule: …")
    QString message;                  // plain text; suggestions rendered in “quotes”
    QStringList suggestions;          // ≤ 5, ranked
    bool suggestionsComputed = true;  // false = background pass skipped Hunspell::suggest
};

enum class TokKind : uint8_t { Word, Punct, Number, Url, Email, SentStart, SentEnd };

struct SpellToken {
    int s = 0, e = 0;                 // [s, e) into the source text (pseudo-tokens: s == e)
    QString text;                     // surface text, case preserved, apostrophes normalised
    bool spaceBefore = false;         // whitespace immediately precedes it
    TokKind kind = TokKind::Word;
    int sentence = 0;
};

struct SentenceSpan { int firstTok = 0, endTok = 0; };   // token index range, pseudo-tokens included

struct RawWord { int s = 0, e = 0; QString text; };       // a spelling candidate

} // namespace spell
