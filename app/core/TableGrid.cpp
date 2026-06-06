#include "TableGrid.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <algorithm>

// ---- helpers ---------------------------------------------------------------

void TableGrid::normalize() {
    cols_ = std::max(0, cols_);
    for (auto& row : cells_) {
        if (static_cast<int>(row.size()) < cols_) row.resize(cols_);
        else if (static_cast<int>(row.size()) > cols_) row.resize(cols_);
    }
    colWidths_.resize(cols_, 0);
    colAligns_.resize(cols_, 0);
    headerRows_ = std::clamp(headerRows_, 0, rows());
}

// ---- access ----------------------------------------------------------------

QString TableGrid::cellText(int r, int c) const {
    if (r < 0 || r >= rows() || c < 0 || c >= cols_) return QString();
    return cells_[r][c].text;
}

void TableGrid::setCellText(int r, int c, const QString& t) {
    if (r < 0 || r >= rows() || c < 0 || c >= cols_) return;
    cells_[r][c].text = t;
}

int TableGrid::colWidth(int c) const {
    return (c >= 0 && c < static_cast<int>(colWidths_.size())) ? colWidths_[c] : 0;
}
void TableGrid::setColWidth(int c, int w) {
    if (c >= 0 && c < static_cast<int>(colWidths_.size())) colWidths_[c] = std::max(0, w);
}
int TableGrid::colAlign(int c) const {
    return (c >= 0 && c < static_cast<int>(colAligns_.size())) ? colAligns_[c] : 0;
}
void TableGrid::setColAlign(int c, int a) {
    if (c >= 0 && c < static_cast<int>(colAligns_.size())) colAligns_[c] = std::clamp(a, 0, 2);
}
void TableGrid::setHeaderRows(int n) { headerRows_ = std::clamp(n, 0, rows()); }

// ---- structure ops ---------------------------------------------------------

void TableGrid::insertRow(int at) {
    at = std::clamp(at, 0, rows());
    cells_.insert(cells_.begin() + at, std::vector<Cell>(cols_));
}

void TableGrid::insertCol(int at) {
    at = std::clamp(at, 0, cols_);
    for (auto& row : cells_) row.insert(row.begin() + at, Cell{});
    colWidths_.insert(colWidths_.begin() + at, 0);
    colAligns_.insert(colAligns_.begin() + at, 0);
    ++cols_;
}

void TableGrid::deleteRow(int at) {
    if (at < 0 || at >= rows() || rows() <= 1) return;   // keep at least one row
    cells_.erase(cells_.begin() + at);
    headerRows_ = std::clamp(headerRows_, 0, rows());
}

void TableGrid::deleteCol(int at) {
    if (at < 0 || at >= cols_ || cols_ <= 1) return;     // keep at least one column
    for (auto& row : cells_) row.erase(row.begin() + at);
    colWidths_.erase(colWidths_.begin() + at);
    colAligns_.erase(colAligns_.begin() + at);
    --cols_;
}

// ---- JSON ------------------------------------------------------------------

QString TableGrid::toJson() const {
    QJsonObject o;
    o.insert(QStringLiteral("cols"), cols_);
    o.insert(QStringLiteral("header"), headerRows_);

    QJsonArray w; for (int v : colWidths_) w.append(v);
    QJsonArray a; for (int v : colAligns_) a.append(v);
    o.insert(QStringLiteral("w"), w);
    o.insert(QStringLiteral("a"), a);

    QJsonArray rowsArr;
    for (const auto& row : cells_) {
        QJsonArray cellsArr;
        for (const Cell& cell : row) cellsArr.append(cell.text);
        rowsArr.append(cellsArr);
    }
    o.insert(QStringLiteral("rows"), rowsArr);
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

TableGrid TableGrid::fromJson(const QString& json) {
    TableGrid g;
    const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();

    const QJsonArray rowsArr = o.value(QStringLiteral("rows")).toArray();
    int maxCols = o.value(QStringLiteral("cols")).toInt(0);
    for (const QJsonValue& rv : rowsArr) {
        const QJsonArray cellsArr = rv.toArray();
        std::vector<Cell> row;
        for (const QJsonValue& cv : cellsArr) row.push_back(Cell{cv.toString()});
        maxCols = std::max<int>(maxCols, static_cast<int>(row.size()));
        g.cells_.push_back(std::move(row));
    }
    g.cols_ = std::max(1, maxCols);

    for (const QJsonValue& v : o.value(QStringLiteral("w")).toArray()) g.colWidths_.push_back(v.toInt(0));
    for (const QJsonValue& v : o.value(QStringLiteral("a")).toArray()) g.colAligns_.push_back(v.toInt(0));
    g.headerRows_ = o.value(QStringLiteral("header")).toInt(1);

    if (g.cells_.empty()) g = makeEmpty(1, g.cols_);
    g.normalize();
    return g;
}

// ---- import / export -------------------------------------------------------

TableGrid TableGrid::fromTSV(const QString& tsv) {
    TableGrid g;
    const QStringList lines = tsv.split(QLatin1Char('\n'));
    int maxCols = 1;
    for (int i = 0; i < lines.size(); ++i) {
        if (i == lines.size() - 1 && lines[i].isEmpty()) break;   // trailing newline
        const QStringList parts = lines[i].split(QLatin1Char('\t'));
        std::vector<Cell> row;
        for (const QString& p : parts) row.push_back(Cell{p});
        maxCols = std::max<int>(maxCols, static_cast<int>(row.size()));
        g.cells_.push_back(std::move(row));
    }
    if (g.cells_.empty()) return makeEmpty(1, 1);
    g.cols_ = maxCols;
    g.headerRows_ = 1;
    g.normalize();
    return g;
}

TableGrid TableGrid::fromCSV(const QString& csv) {
    // RFC-4180-ish: comma-separated, "quoted" fields may contain commas, newlines,
    // and "" escaped quotes. Parsed char-by-char into a grid.
    TableGrid g;
    std::vector<Cell> row;
    QString field;
    bool inQuotes = false;
    int maxCols = 1;
    auto endField = [&] { row.push_back(Cell{field}); field.clear(); };
    auto endRow = [&] {
        endField();
        maxCols = std::max<int>(maxCols, static_cast<int>(row.size()));
        g.cells_.push_back(std::move(row));
        row.clear();
    };
    for (int i = 0; i < csv.size(); ++i) {
        const QChar ch = csv[i];
        if (inQuotes) {
            if (ch == QLatin1Char('"')) {
                if (i + 1 < csv.size() && csv[i + 1] == QLatin1Char('"')) { field += QLatin1Char('"'); ++i; }
                else inQuotes = false;
            } else field += ch;
        } else if (ch == QLatin1Char('"')) {
            inQuotes = true;
        } else if (ch == QLatin1Char(',')) {
            endField();
        } else if (ch == QLatin1Char('\n')) {
            endRow();
        } else if (ch == QLatin1Char('\r')) {
            // swallow; CRLF handled by the \n
        } else {
            field += ch;
        }
    }
    if (!field.isEmpty() || !row.empty()) endRow();   // last field/row (no trailing newline)
    if (g.cells_.empty()) return makeEmpty(1, 1);
    g.cols_ = maxCols;
    g.headerRows_ = 1;
    g.normalize();
    return g;
}

QString TableGrid::toTSV() const {
    QString out;
    for (int r = 0; r < rows(); ++r) {
        for (int c = 0; c < cols_; ++c) {
            if (c) out += QLatin1Char('\t');
            // strip tabs/newlines so the TSV stays rectangular
            QString t = cells_[r][c].text;
            t.replace(QLatin1Char('\t'), QLatin1Char(' ')).replace(QLatin1Char('\n'), QLatin1Char(' '));
            out += t;
        }
        if (r != rows() - 1) out += QLatin1Char('\n');
    }
    return out;
}

QString TableGrid::toHtml() const {
    QString out = QStringLiteral("<table>");
    for (int r = 0; r < rows(); ++r) {
        out += QStringLiteral("<tr>");
        const bool header = r < headerRows_;
        for (int c = 0; c < cols_; ++c) {
            QString t = cells_[r][c].text.toHtmlEscaped();
            out += header ? QStringLiteral("<th>") : QStringLiteral("<td>");
            out += t;
            out += header ? QStringLiteral("</th>") : QStringLiteral("</td>");
        }
        out += QStringLiteral("</tr>");
    }
    out += QStringLiteral("</table>");
    return out;
}

TableGrid TableGrid::makeEmpty(int rows, int cols) {
    TableGrid g;
    g.cols_ = std::max(1, cols);
    rows = std::max(1, rows);
    g.cells_.assign(rows, std::vector<Cell>(g.cols_));
    g.colWidths_.assign(g.cols_, 0);
    g.colAligns_.assign(g.cols_, 0);
    g.headerRows_ = 1;
    return g;
}
