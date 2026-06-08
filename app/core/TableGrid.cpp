#include "TableGrid.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <algorithm>

// ---- helpers ---------------------------------------------------------------

void TableGrid::normalize() {
    cols_ = std::max(0, cols_);
    for (auto& row : cells_) row.resize(cols_);
    colWidths_.resize(cols_, 0);
    colAligns_.resize(cols_, 0);
    colBg_.resize(cols_); colFg_.resize(cols_);
    rowBg_.resize(rows()); rowFg_.resize(rows());
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

static bool inCell(int r, int c, int rows, int cols) { return r >= 0 && r < rows && c >= 0 && c < cols; }
QString TableGrid::cellBg(int r, int c) const { return inCell(r,c,rows(),cols_) ? cells_[r][c].bg : QString(); }
void TableGrid::setCellBg(int r, int c, const QString& h) { if (inCell(r,c,rows(),cols_)) cells_[r][c].bg = h; }
QString TableGrid::cellFg(int r, int c) const { return inCell(r,c,rows(),cols_) ? cells_[r][c].fg : QString(); }
void TableGrid::setCellFg(int r, int c, const QString& h) { if (inCell(r,c,rows(),cols_)) cells_[r][c].fg = h; }
QJsonArray TableGrid::cellSpans(int r, int c) const { return inCell(r,c,rows(),cols_) ? cells_[r][c].spans : QJsonArray(); }
void TableGrid::setCellSpans(int r, int c, const QJsonArray& s) { if (inCell(r,c,rows(),cols_)) cells_[r][c].spans = s; }
QString TableGrid::cellMedia(int r, int c) const { return inCell(r,c,rows(),cols_) ? cells_[r][c].media : QString(); }
void TableGrid::setCellMedia(int r, int c, const QString& m) { if (inCell(r,c,rows(),cols_)) cells_[r][c].media = m; }

QString TableGrid::rowBg(int r) const { return (r >= 0 && r < (int)rowBg_.size()) ? rowBg_[r] : QString(); }
void TableGrid::setRowBg(int r, const QString& h) { if (r >= 0 && r < (int)rowBg_.size()) rowBg_[r] = h; }
QString TableGrid::rowFg(int r) const { return (r >= 0 && r < (int)rowFg_.size()) ? rowFg_[r] : QString(); }
void TableGrid::setRowFg(int r, const QString& h) { if (r >= 0 && r < (int)rowFg_.size()) rowFg_[r] = h; }
QString TableGrid::colBg(int c) const { return (c >= 0 && c < (int)colBg_.size()) ? colBg_[c] : QString(); }
void TableGrid::setColBg(int c, const QString& h) { if (c >= 0 && c < (int)colBg_.size()) colBg_[c] = h; }
QString TableGrid::colFg(int c) const { return (c >= 0 && c < (int)colFg_.size()) ? colFg_[c] : QString(); }
void TableGrid::setColFg(int c, const QString& h) { if (c >= 0 && c < (int)colFg_.size()) colFg_[c] = h; }

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
    normalize();
    at = std::clamp(at, 0, rows());
    cells_.insert(cells_.begin() + at, std::vector<Cell>(cols_));
    rowBg_.insert(rowBg_.begin() + at, QString());
    rowFg_.insert(rowFg_.begin() + at, QString());
}

void TableGrid::insertCol(int at) {
    normalize();
    at = std::clamp(at, 0, cols_);
    for (auto& row : cells_) row.insert(row.begin() + at, Cell{});
    colWidths_.insert(colWidths_.begin() + at, 0);
    colAligns_.insert(colAligns_.begin() + at, 0);
    colBg_.insert(colBg_.begin() + at, QString());
    colFg_.insert(colFg_.begin() + at, QString());
    ++cols_;
}

void TableGrid::deleteRow(int at) {
    if (at < 0 || at >= rows() || rows() <= 1) return;   // keep at least one row
    normalize();
    cells_.erase(cells_.begin() + at);
    rowBg_.erase(rowBg_.begin() + at);
    rowFg_.erase(rowFg_.begin() + at);
    headerRows_ = std::clamp(headerRows_, 0, rows());
}

void TableGrid::deleteCol(int at) {
    if (at < 0 || at >= cols_ || cols_ <= 1) return;     // keep at least one column
    normalize();
    for (auto& row : cells_) row.erase(row.begin() + at);
    colWidths_.erase(colWidths_.begin() + at);
    colAligns_.erase(colAligns_.begin() + at);
    colBg_.erase(colBg_.begin() + at);
    colFg_.erase(colFg_.begin() + at);
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

    // Per-row / per-column colours — only written when any is set (keep compact).
    auto colorArr = [](const std::vector<QString>& v, const char* key, QJsonObject& obj) {
        bool any = false; for (const QString& s : v) if (!s.isEmpty()) { any = true; break; }
        if (!any) return;
        QJsonArray arr; for (const QString& s : v) arr.append(s);
        obj.insert(QLatin1String(key), arr);
    };
    colorArr(rowBg_, "rbg", o); colorArr(rowFg_, "rfg", o);
    colorArr(colBg_, "cbg", o); colorArr(colFg_, "cfg", o);

    QJsonArray rowsArr;
    for (const auto& row : cells_) {
        QJsonArray cellsArr;
        for (const Cell& cell : row) {
            if (cell.plain()) { cellsArr.append(cell.text); continue; }
            QJsonObject co;
            co.insert(QStringLiteral("t"), cell.text);
            if (!cell.bg.isEmpty())    co.insert(QStringLiteral("bg"), cell.bg);
            if (!cell.fg.isEmpty())    co.insert(QStringLiteral("fg"), cell.fg);
            if (!cell.spans.isEmpty()) co.insert(QStringLiteral("s"), cell.spans);
            if (!cell.media.isEmpty()) co.insert(QStringLiteral("m"), cell.media);
            cellsArr.append(co);
        }
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
        for (const QJsonValue& cv : cellsArr) {
            Cell cell;
            if (cv.isObject()) {                         // rich cell {t,s,bg,fg,m}
                const QJsonObject co = cv.toObject();
                cell.text  = co.value(QStringLiteral("t")).toString();
                cell.bg    = co.value(QStringLiteral("bg")).toString();
                cell.fg    = co.value(QStringLiteral("fg")).toString();
                cell.spans = co.value(QStringLiteral("s")).toArray();
                cell.media = co.value(QStringLiteral("m")).toString();
            } else {                                     // plain string cell
                cell.text = cv.toString();
            }
            row.push_back(std::move(cell));
        }
        maxCols = std::max<int>(maxCols, static_cast<int>(row.size()));
        g.cells_.push_back(std::move(row));
    }
    g.cols_ = std::max(1, maxCols);

    for (const QJsonValue& v : o.value(QStringLiteral("w")).toArray()) g.colWidths_.push_back(v.toInt(0));
    for (const QJsonValue& v : o.value(QStringLiteral("a")).toArray()) g.colAligns_.push_back(v.toInt(0));
    for (const QJsonValue& v : o.value(QStringLiteral("rbg")).toArray()) g.rowBg_.push_back(v.toString());
    for (const QJsonValue& v : o.value(QStringLiteral("rfg")).toArray()) g.rowFg_.push_back(v.toString());
    for (const QJsonValue& v : o.value(QStringLiteral("cbg")).toArray()) g.colBg_.push_back(v.toString());
    for (const QJsonValue& v : o.value(QStringLiteral("cfg")).toArray()) g.colFg_.push_back(v.toString());
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
    g.headerRows_ = 1;
    g.normalize();
    return g;
}
