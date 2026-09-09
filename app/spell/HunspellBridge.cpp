#include "HunspellBridge.h"

#include <hunspell.hxx>
#include <QFile>
#include <QFileInfo>

struct HunspellBridge::Impl {
    std::unique_ptr<Hunspell> h;
};

HunspellBridge::HunspellBridge() : d_(std::make_unique<Impl>()) {}
HunspellBridge::~HunspellBridge() = default;

bool HunspellBridge::load(const QString& affPath, const QString& dicPath, QString* error) {
    if (!QFileInfo::exists(affPath) || !QFileInfo::exists(dicPath)) {
        if (error) *error = QStringLiteral("dictionary files missing");
        return false;
    }
    d_->h = std::make_unique<Hunspell>(affPath.toUtf8().constData(), dicPath.toUtf8().constData());
    const std::string enc = d_->h->get_dict_encoding();
    if (enc != "UTF-8") {
        if (error) *error = QStringLiteral("dictionary encoding is %1, expected UTF-8").arg(QString::fromStdString(enc));
        d_->h.reset();
        return false;
    }
    return true;
}

bool HunspellBridge::isLoaded() const { return static_cast<bool>(d_->h); }

bool HunspellBridge::spell(const QString& word) const {
    if (!d_->h) return true;
    return d_->h->spell(word.toStdString());
}

QStringList HunspellBridge::suggest(const QString& word, int max) const {
    QStringList out;
    if (!d_->h) return out;
    auto append = [&](const std::vector<std::string>& v, bool recap) {
        for (const std::string& s : v) {
            QString q = QString::fromStdString(s);
            if (recap && !q.isEmpty()) q[0] = q[0].toUpper();
            if (!out.contains(q)) out << q;
        }
    };
    // A Capitalised word ranks far better through its lowercase form ("Teh":
    // Hunspell puts "The" tenth, "the" first for "teh") — suggest from the
    // lowercase form, re-capitalise, then merge the original-case list.
    const bool capitalised = word.size() > 1 && word[0].isUpper() && word.mid(1).toLower() == word.mid(1);
    if (capitalised) append(d_->h->suggest(word.toLower().toStdString()), true);
    append(d_->h->suggest(word.toStdString()), false);
    return out.mid(0, max);
}

void HunspellBridge::addWord(const QString& word) {
    if (d_->h && !word.isEmpty()) d_->h->add(word.toStdString());
}
