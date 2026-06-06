#pragma once
#include <QString>
#include <QStringList>
#include <vector>

// The table data tier — a pure 2D grid plus column metadata, with compact JSON
// round-trip and TSV/CSV/HTML import-export. Knows nothing about blocks, Qt UI,
// or undo: it is the self-contained "plugin core" that a table block serializes
// into its `content`, and the target a clipboard/CSV importer builds. All editor
// integration lives in BlockModel; all rendering in TableView.qml.
//
// JSON shape (compact, stored in the block's content column):
//   {"cols":3,"header":1,"w":[0,120,0],"a":[0,0,2],
//    "rows":[["a","b","c"],["d","e","f"]]}
//   w: per-column width (0 = auto). a: per-column align (0 left,1 center,2 right).
//   header: number of leading header rows. Missing fields default leniently.
class TableGrid {
public:
    struct Cell { QString text; };   // spans reserved for a later pass

    int rows() const { return static_cast<int>(cells_.size()); }
    int cols() const { return cols_; }

    QString cellText(int r, int c) const;
    void setCellText(int r, int c, const QString& t);

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
    int headerRows_ = 1;
};
