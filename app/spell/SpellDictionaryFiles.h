// Where the checker's files live. The dictionary ships as a Qt resource
// (:/spell/en/en.aff + en.dic) but Hunspell wants paths, so it is copied once
// into AppDataLocation/spelling/<appVersion>/ (older version dirs pruned).
// The user's added words are a plain UTF-8 line file beside them. Quick-free;
// safe to call from the worker.
#pragma once

#include <QString>
#include <QStringList>

namespace spell {

QString spellingDir();                                              // AppDataLocation/spelling
QString ensureDictionaryExtracted(const QString& version, QString* error = nullptr);   // returns the version dir
QStringList loadUserDictionary();
bool appendUserDictionary(const QString& word);
QByteArray loadRulesXml();                                          // :/spell/en/grammar-rules.xml

} // namespace spell
