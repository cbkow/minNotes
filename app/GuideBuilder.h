// The User Guide (2026-10-22): the docs' markdown + screenshots travel in the app as Qt resources (:/guide); on demand
// they are UNPACKED to a scratch folder and Main.qml builds a canvas from them — one tinted frame per section holding
// the section's page beside its screenshots as cards — saved under Application Support per app version (the user's copy:
// annotate it; a new version builds afresh). The markdown is cleaned on the way: the site's front matter dropped,
// cross-page links flattened to their text (the app has no linking), the image lines lifted out (they become the cards).
#pragma once
#include <QObject>
#include <QVariantList>
#include <memory>

class QTemporaryDir;

class GuideBuilder : public QObject {
    Q_OBJECT
public:
    explicit GuideBuilder(QObject* parent = nullptr);
    ~GuideBuilder() override;
    Q_INVOKABLE QString guidePath() const;        // …/Guide/<version>/Guide.mnd (its folder created)
    Q_INVOKABLE bool exists() const;
    Q_INVOKABLE bool remove();                    // this version's guide folder (the probe rebuilds)
    Q_INVOKABLE void sweepOldVersions();          // other versions' folders under Guide/
    Q_INVOKABLE QString unpack();                 // resources → a scratch folder; "" on failure
    Q_INVOKABLE QVariantList sections() const;    // after unpack: [{file, title, tint, path, images: [abs…]}] in reading order
    Q_INVOKABLE void cleanup();                   // the scratch folder
private:
    QString guideDir() const;
    std::unique_ptr<QTemporaryDir> scratch_;
    QVariantList sections_;
};
