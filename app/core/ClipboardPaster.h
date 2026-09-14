// ClipboardPaster — the in-app rich paste engine (0.5.0). Decodes an
// application/x-mnd-blocks payload and lands it in a document with the
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
    // intoCells (Paste Special ▸ Paste into cells, S8d): a grid fills the target table by position
    // even when it carries header rows (they paste as content).
    Q_INVOKABLE void startPaste(BlockModel* dest, const QString& json, int row, int col,
                                int selLo = -1, int selLoCol = 0, int selHi = -1, int selHiCol = 0,
                                bool intoCells = false);
    Q_INVOKABLE void cancel() { cancel_ = true; }

    // The paste ROUTER (SR-4 S8d2, R-I9 8a): the order doPaste tries the clipboard's flavours in,
    // as a pure decision over what the clipboard holds and where the caret is — testable headless.
    // Returns one action name; the editor performs it and, when a flavour yields nothing, masks it
    // in the input and asks again. Actions: nothing · sketchUrls · sketchRaster · blocks ·
    // codeVerbatim · html · urls · raster · tableTsv · tableFromTsv · text.
    struct PasteInput {
        bool hasBlocks = false;        // a non-empty x-mnd-blocks payload
        bool hasHtml = false;
        bool hasImage = false;         // raster bytes
        bool bareRemoteImage = false;  // the HTML is a lone remote <img> (a browser's Copy Image)
        bool noTable = false;          // a tabular text already failed to make a table
        int urls = 0;                  // copied files
        QString text;
    };
    struct PasteTarget {
        bool sketchTab = false;        // a sketch tab is open
        bool codeBlock = false;        // the caret is in a code block (no multi-block selection)
        bool inTable = false;          // the caret is in a table's cell
    };
    static QString route(const PasteInput& in, const PasteTarget& at);
    // Rectangular grid signal for a plain-text paste: every non-empty line carries the SAME
    // number of tabs (≥ 1) across ≥ 2 rows — tab-indented prose or code isn't a table.
    static bool looksTabular(const QString& text);
    Q_INVOKABLE QString routePaste(const QVariantMap& input, const QVariantMap& target) const;

    // Synchronous core (headless tests): the same plan → copy → apply.
    static bool pasteBlocks(BlockModel* dest, const QString& json, int row, int col,
                            int selLo, int selLoCol, int selHi, int selHiCol,
                            int* caretRow = nullptr, int* caretCol = nullptr,
                            QString* error = nullptr, bool intoCells = false);

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
        bool intoCells = false;
    };
    static Job planPaste(BlockModel* dest, const QString& json, int row, int col,
                         int selLo, int selLoCol, int selHi, int selHiCol, bool intoCells = false);
    static bool applyPaste(BlockModel* dest, Job& job, int* caretRow, int* caretCol);
    void setProgress(double p, const QString& item);

    bool running_ = false;
    double progress_ = 0.0;
    QString currentItem_;
    std::atomic<bool> cancel_{false};
    std::thread worker_;
};
