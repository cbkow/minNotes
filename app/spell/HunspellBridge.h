// The only translation unit that sees hunspell.hxx. NOT thread-safe: one
// instance per thread (the checker's worker owns one; tests own their own).
#pragma once

#include <QString>
#include <QStringList>
#include <memory>

class HunspellBridge {
public:
    HunspellBridge();
    ~HunspellBridge();
    bool load(const QString& affPath, const QString& dicPath, QString* error = nullptr);
    bool isLoaded() const;
    bool spell(const QString& word) const;
    QStringList suggest(const QString& word, int max = 5) const;
    void addWord(const QString& word);       // runtime only (the user dictionary file is separate)
private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
