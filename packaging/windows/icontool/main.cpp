// icontool — render an SVG to a multi-resolution Windows .ico (and a PNG master).
//
// Qt has no .ico writer, so we rasterise the SVG with QSvgRenderer at each icon
// size and hand-assemble the ICONDIR/ICONDIRENTRY structure with PNG-encoded
// payloads (supported by Explorer/taskbar on Windows Vista+; our floor is Win10).
// This is the Windows analog of the mac master-render step (rsvg-convert + magick)
// — kept as a tool because ImageMagick isn't a build dependency on Windows.
//
//   icontool <in.svg> <out.ico> [masterPng]
#include <QGuiApplication>
#include <QSvgRenderer>
#include <QImage>
#include <QPainter>
#include <QBuffer>
#include <QByteArray>
#include <QFile>
#include <QList>
#include <cstdio>

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr, "usage: icontool <in.svg> <out.ico> [masterPng]\n");
        return 2;
    }
    const QString in = QString::fromLocal8Bit(argv[1]);
    const QString outIco = QString::fromLocal8Bit(argv[2]);

    QSvgRenderer renderer(in);
    if (!renderer.isValid()) {
        std::fprintf(stderr, "icontool: invalid SVG: %s\n", argv[1]);
        return 1;
    }

    const QList<int> sizes = {16, 24, 32, 48, 64, 96, 128, 256};
    struct Entry { int size; QByteArray png; };
    QList<Entry> entries;
    for (int s : sizes) {
        QImage img(s, s, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        renderer.render(&p);
        p.end();

        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG");
        buf.close();
        entries.push_back({s, png});

        if (s == 256 && argc > 3)
            img.save(QString::fromLocal8Bit(argv[3]), "PNG");
    }

    QFile f(outIco);
    if (!f.open(QIODevice::WriteOnly)) {
        std::fprintf(stderr, "icontool: cannot write %s\n", argv[2]);
        return 1;
    }
    auto put16 = [&](quint16 v) { char b[2] = { char(v & 0xff), char((v >> 8) & 0xff) }; f.write(b, 2); };
    auto put32 = [&](quint32 v) { char b[4] = { char(v & 0xff), char((v >> 8) & 0xff),
                                                 char((v >> 16) & 0xff), char((v >> 24) & 0xff) }; f.write(b, 4); };
    auto put8  = [&](quint8 v)  { char b = char(v); f.write(&b, 1); };

    put16(0);                         // idReserved
    put16(1);                         // idType = icon
    put16(quint16(entries.size()));   // idCount

    quint32 offset = 6 + 16u * quint32(entries.size());
    for (const Entry &e : entries) {
        const quint8 dim = (e.size >= 256) ? 0 : quint8(e.size); // 0 means 256
        put8(dim); put8(dim);         // bWidth, bHeight
        put8(0);                      // bColorCount (0 = >256 colors)
        put8(0);                      // bReserved
        put16(1);                     // wPlanes
        put16(32);                    // wBitCount
        put32(quint32(e.png.size())); // dwBytesInRes
        put32(offset);                // dwImageOffset
        offset += quint32(e.png.size());
    }
    for (const Entry &e : entries)
        f.write(e.png);
    f.close();

    std::printf("icontool: wrote %s (%lld sizes)\n", argv[2], static_cast<long long>(entries.size()));
    return 0;
}
