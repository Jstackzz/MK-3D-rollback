#include "TitleImage.h"

#include <QFile>

#include <algorithm>
#include <cstring>

#include "AppPaths.h"
#include "GameProcess.h"

namespace titleimage {

namespace {

quint32 u32(const QByteArray& d, qsizetype at) {
    quint32 v = 0;
    if (at >= 0 && at + 4 <= d.size()) std::memcpy(&v, d.constData() + at, 4);
    return v;
}

// the named section's bytes, by the xbe's section headers (header addresses are file offsets from the base)
QByteArray section(const QByteArray& xbe, const char* want) {
    if (xbe.size() < 0x124 || !xbe.startsWith("XBEH")) return {};
    const quint32 base = u32(xbe, 0x104), count = u32(xbe, 0x11C), headers = u32(xbe, 0x120) - base;
    for (quint32 i = 0; i < count && i < 256; ++i) {
        const qsizetype h = qsizetype(headers) + i * 0x38;
        const quint32 raw = u32(xbe, h + 12), rawSize = u32(xbe, h + 16), name = u32(xbe, h + 20) - base;
        if (name >= quint32(xbe.size()) || std::strncmp(xbe.constData() + name, want,
                                                        size_t(std::min<qsizetype>(16, xbe.size() - name))) != 0)
            continue;
        return qsizetype(raw) + rawSize <= xbe.size() ? xbe.mid(raw, rawSize) : QByteArray();
    }
    return {};
}

QRgb rgb565(quint16 c) {
    return qRgb(((c >> 11) & 31) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31);
}

QRgb mix(QRgb a, QRgb b, int wa, int wb) {
    const auto m = [&](int x, int y) { return (x * wa + y * wb) / (wa + wb); };
    return qRgba(m(qRed(a), qRed(b)), m(qGreen(a), qGreen(b)), m(qBlue(a), qBlue(b)), m(qAlpha(a), qAlpha(b)));
}

}  // namespace

QImage fromXbe(const QByteArray& xbe) {
    const QByteArray x = section(xbe, "$$XTIMAGE");
    if (x.size() < 32 || !x.startsWith("XPR0")) return {};
    // XPR0: total size, header size; then the texture's header (common, data offset, lock, format, size)
    const quint32 header = u32(x, 8), data = u32(x, 16), format = u32(x, 24);
    const int fmt = int((format >> 8) & 0xFF), w = 1 << ((format >> 20) & 15), h = 1 << ((format >> 24) & 15);
    if ((fmt != 0x0C && fmt != 0x0E && fmt != 0x0F) || w < 4 || h < 4 || w > 1024 || h > 1024) return {};
    const int block = fmt == 0x0C ? 8 : 16;  // DXT1, else DXT3 / DXT5; Xbox compressed textures are not swizzled
    const qsizetype start = qsizetype(header) + data;
    if (start + qsizetype(w / 4) * (h / 4) * block > x.size()) return {};
    QImage img(w, h, QImage::Format_ARGB32);
    const auto* src = reinterpret_cast<const uchar*>(x.constData() + start);
    for (int by = 0; by < h / 4; ++by)
        for (int bx = 0; bx < w / 4; ++bx, src += block) {
            const uchar* c = fmt == 0x0C ? src : src + 8;
            quint16 c0, c1;
            quint32 bits;
            std::memcpy(&c0, c, 2);
            std::memcpy(&c1, c + 2, 2);
            std::memcpy(&bits, c + 4, 4);
            QRgb col[4] = {rgb565(c0), rgb565(c1), 0, 0};
            if (c0 > c1 || fmt != 0x0C) {
                col[2] = mix(col[0], col[1], 2, 1);
                col[3] = mix(col[0], col[1], 1, 2);
            } else {
                col[2] = mix(col[0], col[1], 1, 1);
                col[3] = qRgba(0, 0, 0, 0);  // DXT1's punch-through transparent
            }
            int alpha[16];
            std::fill(alpha, alpha + 16, -1);
            if (fmt == 0x0E) {  // DXT3: 4-bit alpha
                for (int i = 0; i < 16; ++i) alpha[i] = ((src[i / 2] >> ((i & 1) * 4)) & 15) * 17;
            } else if (fmt == 0x0F) {  // DXT5: interpolated alpha
                const int a0 = src[0], a1 = src[1];
                quint64 ab = 0;
                for (int i = 0; i < 6; ++i) ab |= quint64(src[2 + i]) << (8 * i);
                for (int i = 0; i < 16; ++i) {
                    const int k = int((ab >> (3 * i)) & 7);
                    if (k == 0) alpha[i] = a0;
                    else if (k == 1) alpha[i] = a1;
                    else if (a0 > a1) alpha[i] = ((8 - k) * a0 + (k - 1) * a1) / 7;
                    else if (k < 6) alpha[i] = ((6 - k) * a0 + (k - 1) * a1) / 5;
                    else alpha[i] = k == 6 ? 0 : 255;
                }
            }
            for (int i = 0; i < 16; ++i) {
                QRgb p = col[(bits >> (2 * i)) & 3];
                if (alpha[i] >= 0) p = qRgba(qRed(p), qGreen(p), qBlue(p), alpha[i]);
                img.setPixel(bx * 4 + i % 4, by * 4 + i / 4, p);
            }
        }
    return img;
}

QImage forGame(const QString& gameId) {
    QFile f(GameProcess::xbeFile(cfg::gameExe(gameId)));
    if (f.fileName().isEmpty() || !f.open(QIODevice::ReadOnly)) return {};
    return fromXbe(f.readAll());
}

}  // namespace titleimage
