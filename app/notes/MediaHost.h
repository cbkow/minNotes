#pragma once
// MediaHost (2.0 phase A2) — the media-source seam the full-frame views bind to. The PDF tab and
// the video studio used to read `blockModel.<x>(activeRow)` for everything; a canvas media ELEMENT
// has no row. The host is built from EITHER (model, blockId) OR (canvas, elementId) — the element
// wins when both are set — and answers the same questions: kind, urls, sizes, fps/frames/pages,
// the sidecar anchor path (the key the playhead map and VideoNotesModel use), per-page ink with its
// writer routed to the owning undo log (the page's or the canvas's). Pure glue over the existing
// getters; nothing here touches the QCView sidecar or the player.
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariant>
#include <QtQmlIntegration>

class BlockModel;
class CanvasDocument;

class MediaHost : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QObject* model READ model WRITE setModel NOTIFY sourceChanged)
    Q_PROPERTY(QString blockId READ blockId WRITE setBlockId NOTIFY sourceChanged)
    Q_PROPERTY(QObject* canvas READ canvas WRITE setCanvas NOTIFY sourceChanged)
    Q_PROPERTY(QString elementId READ elementId WRITE setElementId NOTIFY sourceChanged)
    // Identity (NOTIFY identityChanged: the host now points at a different media, or none).
    Q_PROPERTY(bool valid READ valid NOTIFY identityChanged)
    Q_PROPERTY(int row READ row NOTIFY identityChanged)              // the block's row; -1 for an element
    Q_PROPERTY(QString kind READ kind NOTIFY identityChanged)        // "image" | "video" | "pdf" | "file" | …
    Q_PROPERTY(QString key READ key NOTIFY identityChanged)          // == anchorPath: the stable per-media key
    Q_PROPERTY(QString anchorPath READ anchorPath NOTIFY identityChanged)
    // Facts that may change under the same identity (NOTIFY changed; `revision` bumps with them).
    Q_PROPERTY(QString url READ url NOTIFY changed)
    Q_PROPERTY(QString viewPath READ viewPath NOTIFY changed)
    Q_PROPERTY(QString playbackSource READ playbackSource NOTIFY changed)
    Q_PROPERTY(QString fileName READ fileName NOTIFY changed)
    Q_PROPERTY(bool extracting READ extracting NOTIFY changed)
    Q_PROPERTY(int w READ w NOTIFY changed)
    Q_PROPERTY(int h READ h NOTIFY changed)
    Q_PROPERTY(int frames READ frames NOTIFY changed)
    Q_PROPERTY(int pages READ pages NOTIFY changed)
    Q_PROPERTY(double fps READ fps NOTIFY changed)
    Q_PROPERTY(qreal durMs READ durMs NOTIFY changed)
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)
public:
    explicit MediaHost(QObject* parent = nullptr);

    QObject* model() const;
    void setModel(QObject* m);
    QString blockId() const { return blockId_; }
    void setBlockId(const QString& id);
    QObject* canvas() const;
    void setCanvas(QObject* c);
    QString elementId() const { return elementId_; }
    void setElementId(const QString& id);

    bool valid() const { return valid_; }
    int row() const { return row_; }
    QString kind() const { return kind_; }
    QString key() const { return anchor_; }
    QString anchorPath() const { return anchor_; }
    QString url() const;
    QString viewPath() const;
    QString playbackSource() const;
    QString fileName() const;
    bool extracting() const;
    int w() const;
    int h() const;
    int frames() const;
    int pages() const;
    double fps() const;
    qreal durMs() const;
    int revision() const { return revision_; }
    bool canUndo() const;
    bool canRedo() const;

    Q_INVOKABLE QString pageInk(int page) const;                       // "" = none
    Q_INVOKABLE bool setPageInk(int page, const QString& envelope);    // → the owner's undo log
    Q_INVOKABLE void undo();                                           // the owner's log
    Q_INVOKABLE void redo();
    // Drawing on a page in the full-frame view (2026-10-19). `localEnvelope` is what CanvasInkCapture commits over
    // the PAGE ITEM at zoom 1 (element-local px; its bbox at (ox, oy) in a page frame of frameW × frameH px): the
    // stroke is normalized over the frame, its width converted to SOURCE px (the page ink's unit — the renderer
    // scales by frameW / sourceW) and appended to the page's envelope → one owner-log entry per stroke.
    Q_INVOKABLE bool addPageStroke(int page, const QString& localEnvelope, qreal ox, qreal oy, qreal frameW, qreal frameH);
    // The eraser: removes the topmost stroke within reach of (fx, fy) frame px (half its pen + 4, at least 6).
    Q_INVOKABLE bool erasePageStrokeAt(int page, qreal fx, qreal fy, qreal frameW, qreal frameH);

signals:
    void sourceChanged();
    void identityChanged();
    void changed();
    void gone();      // was valid, no longer resolves (block deleted / element removed / file closed)

private:
    bool elementMode() const { return canvas_ && !elementId_.isEmpty(); }
    void rewire();    // (dis)connect the active source's signals
    void refresh();   // recompute identity, bump revision, emit
    QJsonObject descriptor() const;   // the media descriptor json (block content / element json.media)
    QJsonValue src() const;

    QPointer<BlockModel> model_;
    QPointer<CanvasDocument> canvas_;
    QString blockId_, elementId_;
    QList<QMetaObject::Connection> conns_;
    bool valid_ = false;
    int row_ = -1;
    QString kind_, anchor_;
    int revision_ = 0;
};
