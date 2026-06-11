// VideoAnnotator — the studio stage's stroke layer: one QQuickPaintedItem
// that captures drawing input, renders the current frame's strokes (live +
// committed), and owns the ANNOTATION undo stacks. Replaces QCView's
// Metal/D3D11 stroke pass + the WindowManager glue with QPainter + direct
// item mouse events (the studio is a full-frame tab — no central mouse
// layer, per the editor reactivity rules).
//
// The item is sized to the video frame box exactly, so the viewport rect is
// trivially (0,0,width,height) and normalized [0..1] stroke coords match
// QCView's frame-normalized space byte-for-byte. strokeWidth is stored in
// SOURCE pixels (QCView semantics); rendering scales by width()/sourceWidth.
//
// Undo is QCView's model: per-stroke entries {timecode, prior JSON, wasNew}
// in memory only, NEVER beginTxn/endTxn — video notes live outside the
// document DB. ⌘Z routes here while the studio is open (Editor.qml).

#pragma once

#include "VideoNotesModel.h"
#include "active_stroke.h"
#include "viewport_annotator.h"

#include <QColor>
#include <QList>
#include <QQuickPaintedItem>
#include <QString>
#include <QTimer>
#include <QtQmlIntegration>

#include <vector>

class VideoAnnotator : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(VideoNotesModel *notes READ notes WRITE setNotes NOTIFY notesChanged FINAL)
    Q_PROPERTY(int frame READ frame WRITE setFrame NOTIFY frameChanged FINAL)
    Q_PROPERTY(int sourceWidth READ sourceWidth WRITE setSourceWidth NOTIFY sourceWidthChanged FINAL)
    // "" disarmed | freehand | rect | oval | arrow | line | eraser
    Q_PROPERTY(QString tool READ tool WRITE setTool NOTIFY toolChanged FINAL)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged FINAL)
    Q_PROPERTY(qreal strokeWidth READ strokeWidth WRITE setStrokeWidth NOTIFY strokeWidthChanged FINAL)
    Q_PROPERTY(bool armed READ armed NOTIFY toolChanged FINAL)
    Q_PROPERTY(bool drawing READ isDrawing NOTIFY drawingChanged FINAL)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoStacksChanged FINAL)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoStacksChanged FINAL)
    QML_ELEMENT

public:
    explicit VideoAnnotator(QQuickItem *parent = nullptr);
    ~VideoAnnotator() override;

    void paint(QPainter *p) override;

    VideoNotesModel *notes() const { return notes_; }
    void setNotes(VideoNotesModel *notes);
    int frame() const { return frame_; }
    void setFrame(int frame);
    int sourceWidth() const { return sourceWidth_; }
    void setSourceWidth(int w);
    QString tool() const { return toolName_; }
    void setTool(const QString &tool);
    QColor color() const { return annot_.drawingColor(); }
    void setColor(const QColor &c);
    qreal strokeWidth() const { return annot_.strokeWidth(); }
    void setStrokeWidth(qreal w);
    bool armed() const { return !toolName_.isEmpty(); }
    bool isDrawing() const { return drawing_; }
    bool canUndo() const { return !undo_.isEmpty(); }
    bool canRedo() const { return !redo_.isEmpty(); }

    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    // Esc mid-drag: drop the in-flight stroke, no commit.
    Q_INVOKABLE void cancelStroke();

signals:
    void notesChanged();
    void frameChanged();
    void sourceWidthChanged();
    void toolChanged();
    void colorChanged();
    void strokeWidthChanged();
    void drawingChanged();
    void undoStacksChanged();
    void strokeStarted();   // QML pauses playback — strokes pin to a frame

protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void geometryChange(const QRectF &newGeo, const QRectF &oldGeo) override;

private:
    struct UndoEntry {
        QString timecode;
        int     frame = 0;
        QString prior;          // annotation_data before the op ("" = none)
        bool    wasNew = false; // the op created the note → inverse deletes it
    };

    void route(qcv::PointerPhase phase, QPointF pos, qint64 tMs);
    void commitStroke(std::unique_ptr<qcv::ActiveStroke> stroke);
    void eraseAt(QPointF norm);
    void refreshStrokes();
    void scheduleThumb(const QString &timecode);
    void setDrawing(bool d);

    VideoNotesModel *notes_ = nullptr;
    int frame_ = 0;
    int sourceWidth_ = 0;
    QString toolName_;
    bool drawing_ = false;

    qcv::ViewportAnnotator annot_;
    std::vector<qcv::ActiveStroke> strokes_;   // committed, current frame
    QList<UndoEntry> undo_, redo_;

    // One annotated-thumb write per drawing burst (QCView's 400 ms rule);
    // a new stroke press cancels a pending write so it can't fire mid-drag.
    QTimer thumbTimer_;
    QString pendingThumbTc_;
};
