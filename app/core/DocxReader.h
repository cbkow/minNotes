// DocxReader — .docx → BlockSpecs + comment threads. The DOCX EMITTER'S
// MIRROR: it must read back what we write (direct-formatted headings — bold +
// half-pt sizes 36/32/28/26/24/22, no styles part; numId 1 = bullets, 2 =
// ordered; tri-state tasks as ☐/◐/☑ glyph bullets; code runs = Courier New +
// EFEFEF shading — the w:shd OVERLOAD: that exact pair is code, any other
// fill is a highlight) AND make a decent job of foreign Word documents
// (pStyle Heading1–6 / outlineLvl headings, numbering.xml bullet-vs-decimal,
// named highlight colors, hyperlinks with their underline/color chrome
// suppressed, images by relationship with intrinsic dims from the bytes).
//
// Comments reverse-map to native threads: commentRangeStart/End offsets in
// the flattened paragraph text + comments.xml bodies ("Author — text"; the
// app's messages carry no author field). Cross-paragraph ranges clamp to
// their start paragraph.
#pragma once

#include "BlockModel.h"

class MediaStore;

class DocxReader {
public:
    struct CommentOut {
        int specIndex = -1;      // spec the range anchors in
        int start = 0, end = 0;  // [s,e) over that spec's text
        QStringList messages;    // thread bodies, oldest first
    };
    struct Result {
        std::vector<BlockModel::BlockSpec> specs;
        std::vector<CommentOut> comments;
        bool ok = false;
    };
    // `store` receives embedded images (null = images drop).
    static Result read(const QString& docxPath, MediaStore* store);
};
