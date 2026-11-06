#pragma once
#include <QObject>
#include <QString>
#include "PathMap.h"

// QML bridge for the cross-OS path mappings (the `pathMap` context property).
// Owns the canonical mapping list, persists it to QSettings, and pushes it into
// mn::setActiveMappings() so the per-document MediaStore resolve path sees edits
// immediately. The Path Mappings dialog reads/writes the list as a JSON array.
class PathMapController : public QObject {
    Q_OBJECT
public:
    explicit PathMapController(QObject* parent = nullptr);

    // JSON array of {id,label,win,mac,lin,enabled} — what the dialog binds to.
    // File URLs for QML (min-paths, canon §22 row 124): percent-encoded, `file:///C:/…` on Windows and `file:///…` on
    // POSIX, "" for a path that cannot be expressed. The QML image sources and the lightbox built these by hand with no
    // encoding — a '#', '%' or '?' in a path broke them.
    Q_INVOKABLE QString fileUrl(const QString& path) const;
    Q_INVOKABLE QString localPath(const QString& fileUrl) const;
    Q_INVOKABLE QString mappingsJson() const;
    // Replace the whole list (the dialog's Done): parse, persist, publish, notify.
    Q_INVOKABLE void setMappingsJson(const QString& json);
    // A fresh stable volume id (ULID) for a newly-added row.
    Q_INVOKABLE QString newId() const;

signals:
    // Mappings changed → open documents must re-resolve media (BlockModel bumps
    // its media revision so mediaUrl()/image-provider URLs re-evaluate).
    void mappingsChanged();

private:
    void load();        // QSettings → mn::setActiveMappings
    void persist();     // mn::activeMappings → QSettings
};
