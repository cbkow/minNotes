#pragma once
#include <QString>
#include <QStringList>
#include <QJsonArray>
#include <vector>

// The table data tier — a pure 2D grid plus column metadata, with compact JSON
// round-trip and TSV/CSV/HTML import-export. Knows nothing about blocks, Qt UI,
// or undo: it is the self-contained "plugin core" that a table block serializes
// into its `content`, and the target a clipboard/CSV importer builds. All editor
// integration lives in BlockModel; all rendering in TableView.qml.
//
// JSON shape (compact, stored in the block's content column):
//   {"cols":3,"header":1,"w":[0,120,0],"a":[0,0,2],
//    "rbg":[...],"rfg":[...],"cbg":[...],"cfg":[...],
//    "rows":[["a","b"],[{"t":"c","s":[…],"bg":"#…","fg":"#…","m":"{…}"}]]}
//   w/a: per-column width/align. r*/c*: per-row/per-column bg/fg colour (hex, ""=none).
//   A cell is a bare string when plain, else an object {t,s(spans),bg,fg,m(media)}.
//   header: number of leading header rows. Missing fields default leniently.
class TableGrid {
public:
    // A cell is a bare string when it has only text; richer cells carry inline
    // spans, a background/foreground colour, and/or a media descriptor.
    struct Cell {
        QString text;
        QString bg, fg;        // cell background / text colour hex ("" = inherit)
        QJsonArray spans;      // inline spans [{s,e,k,u}] ("" = none)
        QString media;         // media descriptor JSON ("" = text cell)
        bool plain() const { return bg.isEmpty() && fg.isEmpty() && spans.isEmpty() && media.isEmpty(); }
    };

    int rows() const { return static_cast<int>(cells_.size()); }
    int cols() const { return cols_; }

    QString cellText(int r, int c) const;
    void setCellText(int r, int c, const QString& t);
    QString cellBg(int r, int c) const;
    void    setCellBg(int r, int c, const QString& hex);
    QString cellFg(int r, int c) const;
    void    setCellFg(int r, int c, const QString& hex);
    QJsonArray cellSpans(int r, int c) const;
    void       setCellSpans(int r, int c, const QJsonArray& spans);
    QString cellMedia(int r, int c) const;
    void    setCellMedia(int r, int c, const QString& mediaJson);

    QString rowBg(int r) const;  void setRowBg(int r, const QString& hex);
    QString rowFg(int r) const;  void setRowFg(int r, const QString& hex);
    QString colBg(int c) const;  void setColBg(int c, const QString& hex);
    QString colFg(int c) const;  void setColFg(int c, const QString& hex);

    int  colWidth(int c) const;            // 0 = auto-size
    void setColWidth(int c, int w);
    int  colAlign(int c) const;            // 0 left, 1 center, 2 right
    void setColAlign(int c, int a);
    int  headerRows() const { return headerRows_; }
    void setHeaderRows(int n);

    // Structure ops (clamped; keep every row rectangular at `cols_`).
    void insertRow(int at);
    void insertCol(int at);
    void deleteRow(int at);
    void deleteCol(int at);

    // Serialization / interchange.
    QString toJson() const;
    static TableGrid fromJson(const QString& json);
    static TableGrid fromTSV(const QString& tsv);   // tabs split cells, \n splits rows
    static TableGrid fromCSV(const QString& csv);   // RFC-4180-ish (quoted fields)
    QString toTSV() const;
    QString toHtml() const;

    static TableGrid makeEmpty(int rows, int cols);
    bool isValid() const { return cols_ > 0 && !cells_.empty(); }

private:
    void normalize();                       // pad/trim every row to cols_, size meta vectors

    std::vector<std::vector<Cell>> cells_;  // cells_[row][col]
    int cols_ = 0;
    std::vector<int> colWidths_;            // length cols_, 0 = auto
    std::vector<int> colAligns_;            // length cols_, 0 = left
    std::vector<QString> rowBg_, rowFg_;    // per-row colour (length rows())
    std::vector<QString> colBg_, colFg_;    // per-column colour (length cols_)
    int headerRows_ = 1;
};
