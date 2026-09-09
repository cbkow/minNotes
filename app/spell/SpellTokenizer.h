// The two token views the checker needs, approximating LanguageTool's English
// word tokenizer so its rules match the way they were written:
//   rawWords  — spelling candidates: letter/digit runs with internal apostrophes
//               and hyphens (a hyphenated compound stays whole iff the dictionary
//               knows it, else its parts are checked separately);
//   tokenize  — the grammar stream: contractions split ("don't" → "do" + "n't",
//               "it's" → "it" + "'s"), every other non-space char its own token,
//               URLs / emails one token, SENT_START / SENT_END pseudo-tokens at
//               sentence bounds, whitespace-before recorded.
// Quick-free; pure functions.
#pragma once

#include "SpellTypes.h"
#include <functional>

namespace spell {

using KnownWordFn = std::function<bool(const QString&)>;

std::vector<RawWord> rawWords(const QString& text, const KnownWordFn& known);
std::vector<SpellToken> tokenize(const QString& text, const KnownWordFn& known,
                                 std::vector<SentenceSpan>* sentences);
// Words the spell pass never flags: shorter than 2, any digit, ALLCAPS,
// camelCase, underscores, hashtags/handles are handled by the caller.
bool spellCheckable(const QString& word);
QString normalizeApostrophes(QString w);   // ’ ‘ ` ´ → '

} // namespace spell
