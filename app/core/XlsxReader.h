// XlsxReader — hand-rolled Excel (.xlsx) reader in the DocxReader lane:
// mnpkg::readEntry per part + flat QXmlStreamReader loops, no external deps.
// Sheets become Table blocks in ONE document (a level-2 heading per sheet
// when the workbook has more than one). Images land in table cells BOTH
// ways Excel stores them: drawing-anchored pictures ("place over cells",
// the paste default — anchored to their from-cell) and rich-value in-cell
// images ("place in cell", the vm= chain through xl/metadata + xl/richData).
// v1 lossiness (documented): merged cells keep their value in the top-left
// cell only; date serials stay raw numbers; formulas import their cached
// value.
#pragma once

#include "BlockModel.h"

#include <QString>
#include <vector>

class MediaStore;

class XlsxReader {
public:
    // `store` receives embedded images (null = images are skipped).
    static std::vector<BlockModel::BlockSpec> read(const QString& xlsxPath,
                                                   MediaStore* store = nullptr);
};
