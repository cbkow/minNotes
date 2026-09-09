// ClipboardPaster — the in-app rich paste engine (0.5.0). Decodes an
// application/x-minnotes-blocks payload and lands it in a document with the
// tab-merge fidelity rules: comment threads re-minted (or re-anchored when
// a cut is pasted back into the same document), block ink laid per row
// (width-migrated), collected assets copied into the destination sidecar
// (same-document pastes reuse the very same file — no re-encode).
//
// Runs in the DocumentMerger shape: plan on the GUI thread (no side
// effects), chunked byte-weighted copy on a worker when assets must travel
// (cancel/failure rolls back every file made), then the model apply lands
// queued on the GUI thread as ONE undo entry — the selection delete, the
// splice and the ink together. No assets to copy → applied synchronously.
#pragma once

#include "AssetTransfer.h"
#include "BlockClipboard.h"
#include "BlockModel.h"

#include <QObject>
#include <QString>
#include <atomic>
#include <thread>
#include <vector>

class ClipboardPaster : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString currentItem READ currentItem NOTIFY progressChanged)
public:
    explicit ClipboardPaster(QObject* parent = nullptr) : QObject(parent) {}
    ~ClipboardPaster() override;

    bool running() const { return running_; }
    double progress() const { return progress_; }
    QString currentItem() const { return currentItem_; }

    // Paste `json` at (row, col) of `dest`, first deleting the selection
    // [selLo,selLoCol]..[selHi,selHiCol] when selLo >= 0 (block-grain at
    // opaque ends). pasteFinished(ok, caretRow, caretCol, error) follows —
    // synchronously when nothing needs copying. No-op while running.
    Q_INVOKABLE void startPaste(BlockModel* dest, const QString& json, int row, int col,
                                int selLo = -1, int selLoCol = 0, int selHi = -1, int selHiCol = 0);
    Q_INVOKABLE void cancel() { cancel_ = true; }

    // Synchronous core (headless tests): the same plan → copy → apply.
    static bool pasteBlocks(BlockModel* dest, const QString& json, int row, int col,
                            int selLo, int selLoCol, int selHi, int selHiCol,
                            int* caretRow = nullptr, int* caretCol = nullptr,
                            QString* error = nullptr);

signals:
    void runningChanged();
    void progressChanged();
    void pasteFinished(bool ok, int caretRow, int caretCol, const QString& error);

private:
    struct Job {
        BlockClipboard::Payload payload;
        std::vector<BlockModel::ThreadImport> threads;   // NEW ids already minted
        AssetTransfer::Plan assets;
        int row = 0, col = 0;
        int selLo = -1, selLoCol = 0, selHi = -1, selHiCol = 0;
        QString anchorId;          // dest block id of the target row
        QString refuse;            // non-empty = refused at plan time
    };
    static Job planPaste(BlockModel* dest, const QString& json, int row, int col,
                         int selLo, int selLoCol, int selHi, int selHiCol);
    static bool applyPaste(BlockModel* dest, Job& job, int* caretRow, int* caretCol);
    void setProgress(double p, const QString& item);

    bool running_ = false;
    double progress_ = 0.0;
    QString currentItem_;
    std::atomic<bool> cancel_{false};
    std::thread worker_;
};
