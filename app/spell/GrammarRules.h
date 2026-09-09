// GrammarRules — an interpreter for the parse-free subset of LanguageTool's
// pattern-rule XML (app/spell/en/grammar-rules.xml, produced by
// scripts/trim-lt-rules.py). A rule is antipattern* + (pattern | regexp) +
// message + suggestion*; patterns are token sequences with literal / regex /
// set text, exceptions (current / previous / next scope), skip, min/max
// repetition, negate, spacebefore, case rules, a marker span, and
// SENT_START / SENT_END pseudo-tokens. No tagger, no morphology — those rules
// never reach this file. Quick-free; the tests load the same XML.
#pragma once

#include "SpellTypes.h"

#include <QByteArray>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <memory>
#include <vector>

class GrammarRules {
public:
    GrammarRules();
    ~GrammarRules();

    bool load(const QByteArray& xml, QString* error = nullptr);
    int ruleCount() const;
    int compileErrors() const;                 // rules dropped for an uncompilable regex
    QStringList compileErrorIds() const;

    struct Hit {
        int s = 0, e = 0;                      // [s, e) into the checked text
        QString ruleId, ruleName, message;
        QStringList suggestions;
    };
    std::vector<Hit> check(const QString& text, const std::vector<spell::SpellToken>& toks,
                           const std::vector<spell::SentenceSpan>& sents,
                           const QSet<QString>& disabledRuleIds) const;
    QString ruleName(const QString& id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
