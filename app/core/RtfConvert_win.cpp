// RTF → HTML on Windows via a hidden RichEdit control + TOM (the per-OS
// native-converter ruling, mirroring RtfConvert_mac.mm).
//
// ============================ WINDOWS SESSION =============================
// STATUS: DRAFT authored on macOS 2026-08-18 — never compiled on Windows.
// This file only builds on WIN32 (CMake three-way pick in app/CMakeLists.txt
// + tests/CMakeLists.txt), so the mac/Linux builds stay green regardless.
//
// Finish checklist for the Windows session:
//   1. Build: expect header/link fixups (richedit.h/richole.h/tom.h come
//      with the Windows SDK; __uuidof(ITextDocument) needs MSVC — the
//      Windows kit is MSVC, so fine).
//   2. Run the suite: test 29 ("RTF import") self-enables wherever
//      rtfImportSupported() is true and covers bold/italic/underline +
//      paragraph splits against a literal RTF fixture.
//   3. Manual: import a WordPad-authored .rtf (colors, bullets, sizes).
//   4. While there, the OTHER Windows verifications owed to the program:
//      packages open/export (miniz is portable C — expected fine), video
//      STREAMING from a package (`ffmpeg -protocols` on the Windows
//      vendored build must list `subfile`; if absent, packaged video falls
//      back to extract-on-demand automatically), async import/export smoke.
// ==========================================================================
//
// Shape: Msftedit's RICHEDIT50W control parses the RTF (EM_STREAMIN), then
// TOM (ITextDocument) walks paragraphs and equal-format character runs into
// minimal HTML for the shared walker. List paragraphs group into <ul>/<ol>.

#include "RtfConvert.h"

#include <QFile>
#include <QVarLengthArray>

#include <windows.h>
#include <richedit.h>
#include <richole.h>
#include <tom.h>

namespace {

HMODULE msftedit() {
    static HMODULE h = ::LoadLibraryW(L"Msftedit.dll");
    return h;
}

struct StreamCtx {
    const QByteArray* bytes;
    qsizetype pos = 0;
};

DWORD CALLBACK streamInCb(DWORD_PTR cookie, LPBYTE buf, LONG cb, LONG* pcb) {
    auto* c = reinterpret_cast<StreamCtx*>(cookie);
    const qsizetype n = qMin<qsizetype>(cb, c->bytes->size() - c->pos);
    memcpy(buf, c->bytes->constData() + c->pos, static_cast<size_t>(n));
    c->pos += n;
    *pcb = static_cast<LONG>(n);
    return 0;
}

QString htmlEscape(const QString& s) {
    QString out = s;
    out.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    out.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    out.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    return out;
}

QString colorHex(long c) {   // COLORREF 0x00BBGGRR → #rrggbb
    return QStringLiteral("#%1%2%3")
        .arg(c & 0xff, 2, 16, QLatin1Char('0'))
        .arg((c >> 8) & 0xff, 2, 16, QLatin1Char('0'))
        .arg((c >> 16) & 0xff, 2, 16, QLatin1Char('0'));
}

struct RunFmt {
    bool b = false, i = false, u = false, strike = false;
    long fg = tomAutoColor, bg = tomAutoColor;
    bool operator==(const RunFmt& o) const {
        return b == o.b && i == o.i && u == o.u && strike == o.strike
            && fg == o.fg && bg == o.bg;
    }
};

RunFmt fontFmt(ITextFont* f) {
    RunFmt r;
    long v = 0;
    if (SUCCEEDED(f->GetBold(&v))) r.b = v == tomTrue;
    if (SUCCEEDED(f->GetItalic(&v))) r.i = v == tomTrue;
    if (SUCCEEDED(f->GetUnderline(&v))) r.u = v != tomNone && v != tomFalse;
    if (SUCCEEDED(f->GetStrikeThrough(&v))) r.strike = v == tomTrue;
    f->GetForeColor(&r.fg);
    f->GetBackColor(&r.bg);
    return r;
}

QString openRun(const RunFmt& f) {
    QString out;
    if (f.b) out += QLatin1String("<b>");
    if (f.i) out += QLatin1String("<i>");
    if (f.u) out += QLatin1String("<u>");
    if (f.strike) out += QLatin1String("<s>");
    QString style;
    if (f.fg != tomAutoColor) style += QStringLiteral("color:%1;").arg(colorHex(f.fg));
    if (f.bg != tomAutoColor)
        style += QStringLiteral("background-color:%1;").arg(colorHex(f.bg));
    if (!style.isEmpty())
        out += QStringLiteral("<span style=\"%1\">").arg(style);
    return out;
}

QString closeRun(const RunFmt& f) {
    QString out;
    if (f.fg != tomAutoColor || f.bg != tomAutoColor) out += QLatin1String("</span>");
    if (f.strike) out += QLatin1String("</s>");
    if (f.u) out += QLatin1String("</u>");
    if (f.i) out += QLatin1String("</i>");
    if (f.b) out += QLatin1String("</b>");
    return out;
}

} // namespace

namespace mn {

bool rtfImportSupported() { return msftedit() != nullptr; }

QString rtfFileToHtml(const QString& path) {
    if (!msftedit()) return {};
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray rtf = f.readAll();
    if (rtf.isEmpty()) return {};

    // Hidden RichEdit (message-only parent keeps it off screen entirely).
    HWND hwnd = ::CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                                  ES_MULTILINE | WS_CHILD,
                                  0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                  ::GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return {};

    QString html;
    do {
        StreamCtx ctx{&rtf, 0};
        EDITSTREAM es{};
        es.dwCookie = reinterpret_cast<DWORD_PTR>(&ctx);
        es.pfnCallback = streamInCb;
        ::SendMessageW(hwnd, EM_STREAMIN, SF_RTF, reinterpret_cast<LPARAM>(&es));
        if (es.dwError != 0) break;

        IRichEditOle* ole = nullptr;
        if (!::SendMessageW(hwnd, EM_GETOLEINTERFACE, 0,
                            reinterpret_cast<LPARAM>(&ole)) || !ole)
            break;
        ITextDocument* doc = nullptr;
        const HRESULT qi = ole->QueryInterface(__uuidof(ITextDocument),
                                               reinterpret_cast<void**>(&doc));
        ole->Release();
        if (FAILED(qi) || !doc) break;

        ITextRange* whole = nullptr;
        long docEnd = 0;
        if (SUCCEEDED(doc->Range(0, 0, &whole)) && whole) {
            whole->MoveEnd(tomStory, 1, nullptr);
            whole->GetEnd(&docEnd);
            whole->Release();
        }

        // Paragraph loop: [paraStart, paraEnd) per tomParagraph unit; inside,
        // batch equal-format character runs.
        long pos = 0;
        int listOpen = 0;   // 0 none, 1 <ul>, 2 <ol>
        while (pos < docEnd) {
            ITextRange* para = nullptr;
            if (FAILED(doc->Range(pos, pos, &para)) || !para) break;
            para->MoveEnd(tomParagraph, 1, nullptr);
            long pStart = 0, pEnd = 0;
            para->GetStart(&pStart);
            para->GetEnd(&pEnd);
            if (pEnd <= pos) { para->Release(); break; }   // no forward progress

            // Paragraph kind: bullet/numbered list?
            int wantList = 0;
            {
                ITextPara* pp = nullptr;
                if (SUCCEEDED(para->GetPara(&pp)) && pp) {
                    long lt = tomListNone;
                    pp->GetListType(&lt);
                    if ((lt & 0xf) == tomListBullet) wantList = 1;
                    else if ((lt & 0xf) >= tomListNumberAsArabic
                             && (lt & 0xf) <= tomListNumberAsUCRoman) wantList = 2;
                    pp->Release();
                }
            }
            if (wantList != listOpen) {
                if (listOpen == 1) html += QLatin1String("</ul>");
                if (listOpen == 2) html += QLatin1String("</ol>");
                if (wantList == 1) html += QLatin1String("<ul>");
                if (wantList == 2) html += QLatin1String("<ol>");
                listOpen = wantList;
            }
            html += wantList ? QLatin1String("<li>") : QLatin1String("<p>");

            // Character-run batching within the paragraph body (excluding the
            // trailing paragraph mark).
            long runStart = pStart;
            RunFmt cur;
            bool haveFmt = false;
            QString runText;
            auto flushRun = [&] {
                if (runText.isEmpty()) return;
                html += openRun(cur) + htmlEscape(runText) + closeRun(cur);
                runText.clear();
            };
            for (long cpos = pStart; cpos < pEnd; ++cpos) {
                ITextRange* ch = nullptr;
                if (FAILED(doc->Range(cpos, cpos + 1, &ch)) || !ch) break;
                BSTR bs = nullptr;
                ch->GetText(&bs);
                QString t = bs ? QString::fromWCharArray(bs) : QString();
                if (bs) ::SysFreeString(bs);
                // Paragraph mark / trailing CR: end of body.
                if (t == QLatin1String("\r") || t == QLatin1String("\n")) {
                    ch->Release();
                    break;
                }
                ITextFont* font = nullptr;
                RunFmt f;
                if (SUCCEEDED(ch->GetFont(&font)) && font) {
                    f = fontFmt(font);
                    font->Release();
                }
                ch->Release();
                if (!haveFmt) { cur = f; haveFmt = true; runStart = cpos; }
                else if (!(f == cur)) { flushRun(); cur = f; runStart = cpos; }
                runText += t;
            }
            Q_UNUSED(runStart);
            flushRun();
            html += wantList ? QLatin1String("</li>") : QLatin1String("</p>");
            pos = pEnd;
            para->Release();
        }
        if (listOpen == 1) html += QLatin1String("</ul>");
        if (listOpen == 2) html += QLatin1String("</ol>");
        doc->Release();
    } while (false);

    ::DestroyWindow(hwnd);
    if (html.isEmpty()) return {};
    return QStringLiteral("<html><body>%1</body></html>").arg(html);
}

} // namespace mn
