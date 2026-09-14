#pragma once
#include <QString>
#include <QStringList>
#include <QJsonArray>
#include <vector>

// The table IMPORT IR (SR-4 S10): a pure 2D grid plus column metadata that the
// CSV/TSV/XLSX/ODS readers build and BlockModel::gridSpecsFromTable turns into a
// table's records and cells. Knows nothing about blocks, Qt UI, or undo; the
// grid-paste path also reads TSV through it.
//
// JSON shape (compact; the converter's input):
//   {"cols":3,"header":1,"w":[0,120,0],"a":[0,0,2],
//    "rbg":[...],"rfg":[...],"cbg":[...],"cfg":[...],
//    "rows":[["a","b"],[{"t":"c","s":[…],"bg":"#…","fg":"#…","m":"{…}"}]]}
//   w/a: per-column width/align. r*/c*: per-row/per-column bg/fg colour (hex, ""=none).
//   A cell is a bare string when plain, else an object {t,s(spans),bg,fg,m(media)}.
//   header: number of leading header rows. Missing fields default leniently.
class TableGrid {
public:
    // A cell is a bare string when it has only text; richer cells carry inline
    // spans, a background/foreground colour, a media descriptor, and/or (in a
    // choice column) the id of the selected option.
    struct Cell {
        QString text;
        QString bg, fg;        // cell background / text colour hex ("" = inherit)
        QJsonArray spans;      // inline spans [{s,e,k,u}] ("" = none)
        QString media;         // media descriptor JSON ("" = text cell)
        QString choice;        // selected option id in a choice column ("" = none)
        bool plain() const { return bg.isEmpty() && fg.isEmpty() && spans.isEmpty()
                                    && media.isEmpty() && choice.isEmpty(); }
    };

    // Column type. Text is the default; Choice hoists a shared option set out of
    // the cells (each cell then stores only the selected option's stable id); Check
    // is a tri-state task checkbox per cell (0 todo / 1 doing / 2 done), the same
    // primitive as a block task list. The enum is left open for later kinds.
    enum ColKind { ColText = 0, ColChoice = 1, ColCheck = 2 };
    struct Option { QString id, label, color; };   // color = "" inherits cell text colour
    struct ColType { int kind = ColText; std::vector<Option> options; };

    int rows() const { return static_cast<int>(cells_.size()); }
    int cols() const { return cols_; }

    QString cellText(int r, int c) const;
    void setCellText(int r, int c, const QString& t);
    QString cellBg(int r, int c) const;
    void    setCellBg(int r, int c, const QString& hex);
    QString cellFg(int r, int c) const;
    QJsonArray cellSpans(int r, int c) const;
    QString cellMedia(int r, int c) const;
    void    setCellMedia(int r, int c, const QString& mediaJson);

    // Choice columns: a shared, ordered option set per column; cells reference an
    // option by its stable id. Switching kind cleans up (text→choice clears the
    // column's existing cell text — "start empty"; choice→text drops options +
    // every cell's selection).
    int     colKind(int c) const;                                 // 0 text, 1 choice, 2 check
    void    setColKind(int c, int kind);
    std::vector<Option> colOptions(int c) const;
    void    addOption(int c, const QString& id, const QString& label, const QString& color);
    QString cellChoice(int r, int c) const;                       // selected option id ("" = none)
    void    setCellChoice(int r, int c, const QString& id);
    int     cellCheck(int r, int c) const;                        // check column: 0/1/2 (stored in `choice`)
    QString optionLabel(int c, const QString& id) const;          // "" if not found

    // Row / column colours (hex, "" = none).
    QString rowBg(int r) const;
    QString rowFg(int r) const;
    QString colBg(int c) const;
    QString colFg(int c) const;

    int  colWidth(int c) const;            // 0 = auto-size
    void setColWidth(int c, int w);
    int  colAlign(int c) const;            // 0 left, 1 center, 2 right
    void setColAlign(int c, int a);
    int  headerRows() const { return headerRows_; }
    void setHeaderRows(int n);

    void deleteRow(int at);
    void deleteCol(int at);
    // Import janitor: drop TRAILING rows/columns that are fully empty (no
    // text, no media in any cell). Sheet/CSV exports pad to the sheet edge;
    // that padding should never reach the document. Typed (non-text)
    // columns stop the column trim — they're deliberate structure. Keeps
    // at least 1x1 (the deleteRow/deleteCol floors).
    void trimTrailingEmpty();

    // Serialization / interchange.
    QString toJson() const;
    static TableGrid fromJson(const QString& json);
    static TableGrid fromTSV(const QString& tsv);   // tabs split cells, \n splits rows
    static TableGrid fromCSV(const QString& csv);   // RFC-4180-ish (quoted fields)

    static TableGrid makeEmpty(int rows, int cols);
    bool isValid() const { return cols_ > 0 && !cells_.empty(); }

private:
    void normalize();                       // pad/trim every row to cols_, size meta vectors

    std::vector<std::vector<Cell>> cells_;  // cells_[row][col]
    int cols_ = 0;
    std::vector<int> colWidths_;            // length cols_, 0 = auto
    std::vector<int> colAligns_;            // length cols_, 0 = left
    std::vector<ColType> colTypes_;         // length cols_, default {ColText, {}}
    std::vector<QString> rowBg_, rowFg_;    // per-row colour (length rows())
    std::vector<QString> colBg_, colFg_;    // per-column colour (length cols_)
    int headerRows_ = 1;
};
