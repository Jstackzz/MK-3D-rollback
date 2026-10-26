#include "Xiso.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

#include <algorithm>
#include <cstring>

namespace xiso {

namespace {

constexpr qint64 kSector = 2048;
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";  // the volume descriptor, at sector 32 of the file system
constexpr int kMagicSize = 20;
constexpr quint8 kAttrDirectory = 0x10;
// where the file system starts: a plain XISO, then whole-disc dumps of the three disc layouts
constexpr qint64 kKnownBases[] = {0, 0x18300000, 0xFD90000, 0x2080000};
constexpr qint64 kScanLimit = qint64(2) << 30;
constexpr int kMaxDepth = 32;
constexpr int kMaxEntries = 200000;
constexpr qint64 kChunk = 4 << 20;

quint16 u16(const char* p) { return quint16(quint8(p[0]) | quint8(p[1]) << 8); }
quint32 u32(const char* p) {
    return quint32(quint8(p[0])) | quint32(quint8(p[1])) << 8 | quint32(quint8(p[2])) << 16 |
           quint32(quint8(p[3])) << 24;
}

// a name from the disc that is safe to make a file of: no separators, no drive, not . or ..
bool safeName(const QString& name) {
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")) return false;
    for (QChar c : name)
        if (c == u'/' || c == u'\\' || c == u':' || c.unicode() < 0x20) return false;
    return true;
}

QString native(const QString& p) { return QDir::toNativeSeparators(p); }

}  // namespace

bool Image::open(const QString& path, QString* error) {
    f_.setFileName(path);
    if (!f_.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot open %1: %2").arg(native(path), f_.errorString());
        return false;
    }
    if (!findBase(error)) return false;
    QByteArray vd;
    if (!f_.seek(base_ + 32 * kSector) || (vd = f_.read(28)).size() != 28) {
        *error = QStringLiteral("The image is cut short.");
        return false;
    }
    entries_.clear();
    return walk(u32(vd.constData() + 20), u32(vd.constData() + 24), QString(), 0, error);
}

bool Image::findBase(QString* error) {
    const QByteArray magic(kMagic, kMagicSize);
    for (qint64 base : kKnownBases) {
        if (f_.seek(base + 32 * kSector) && f_.read(kMagicSize) == magic) {
            base_ = base;
            return true;
        }
    }
    // else the volume descriptor at any sector of the first part of the image
    for (qint64 pos = 0; pos < kScanLimit; pos += kChunk) {
        if (!f_.seek(pos)) break;
        const QByteArray data = f_.read(kChunk);
        if (data.size() < kMagicSize) break;
        for (qsizetype at = 0; at + kMagicSize <= data.size(); at += kSector) {
            if (pos + at >= 32 * kSector && std::memcmp(data.constData() + at, kMagic, kMagicSize) == 0) {
                base_ = pos + at - 32 * kSector;
                return true;
            }
        }
    }
    *error = QStringLiteral("This is not an Xbox disc image (no Xbox file system in it).");
    return false;
}

bool Image::walk(quint32 sector, quint32 size, const QString& prefix, int depth, QString* error) {
    if (size == 0) return true;
    if (depth > kMaxDepth) {
        *error = QStringLiteral("The disc's folders nest too deep.");
        return false;
    }
    QByteArray data;
    if (!f_.seek(base_ + qint64(sector) * kSector) || (data = f_.read(size)).size() != qsizetype(size)) {
        *error = QStringLiteral("The image is cut short (a folder lies past its end).");
        return false;
    }
    // each folder is a binary tree of entries: u16 left, u16 right (in 4-byte units), u32 sector, u32 size,
    // u8 attributes, u8 name length, the name
    QList<qsizetype> stack{0};
    QSet<qsizetype> seen;
    while (!stack.isEmpty()) {
        const qsizetype off = stack.takeLast();
        if (seen.contains(off) || off + 14 > data.size()) continue;
        seen.insert(off);
        const char* p = data.constData() + off;
        const quint16 left = u16(p), right = u16(p + 2);
        if (left == 0xFFFF && right == 0xFFFF) continue;  // padding
        const quint8 attrs = quint8(p[12]), nameLen = quint8(p[13]);
        if (off + 14 + nameLen > data.size()) continue;
        const QString name = QString::fromLatin1(p + 14, nameLen);
        if (!safeName(name)) {
            *error = QStringLiteral("The disc has a file name that cannot be used: %1").arg(name);
            return false;
        }
        if (entries_.size() >= kMaxEntries) {
            *error = QStringLiteral("The disc lists too many files.");
            return false;
        }
        const Entry e{prefix + name, u32(p + 4), u32(p + 8), (attrs & kAttrDirectory) != 0};
        entries_.append(e);
        if (e.dir && !walk(e.sector, e.size, e.path + u'/', depth + 1, error)) return false;
        if (left) stack.append(qsizetype(left) * 4);
        if (right) stack.append(qsizetype(right) * 4);
    }
    return true;
}

qint64 Image::totalBytes() const {
    qint64 n = 0;
    for (const Entry& e : entries_)
        if (!e.dir) n += e.size;
    return n;
}

int Image::fileCount() const {
    return int(std::count_if(entries_.cbegin(), entries_.cend(), [](const Entry& e) { return !e.dir; }));
}

const Entry* Image::find(const QString& path) const {
    for (const Entry& e : entries_)
        if (!e.dir && e.path.compare(path, Qt::CaseInsensitive) == 0) return &e;
    return nullptr;
}

bool Image::read(const Entry& e, QByteArray* out, QString* error) {
    if (!f_.seek(base_ + qint64(e.sector) * kSector) || (*out = f_.read(e.size)).size() != qsizetype(e.size)) {
        *error = QStringLiteral("The image is cut short (%1 lies past its end).").arg(e.path);
        return false;
    }
    return true;
}

bool Image::extractAll(const QString& outDir, const std::atomic<bool>& cancel, std::atomic<qint64>& bytesDone,
                       std::atomic<int>& filesDone, QString* error) {
    const QDir out(outDir);
    // in the order the files lie on the disc: one pass from start to end
    QList<const Entry*> order;
    for (const Entry& e : entries_) order.append(&e);
    std::stable_sort(order.begin(), order.end(), [](const Entry* a, const Entry* b) { return a->sector < b->sector; });
    QByteArray buf(kChunk, Qt::Uninitialized);
    for (const Entry* e : order) {
        const QString target = out.filePath(e->path);
        if (e->dir) {
            if (!QDir().mkpath(target)) {
                *error = QStringLiteral("Cannot make the folder %1.").arg(native(target));
                return false;
            }
            continue;
        }
        if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
            *error = QStringLiteral("Cannot make the folder %1.").arg(native(QFileInfo(target).absolutePath()));
            return false;
        }
        QFile dst(target);
        if (!dst.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            *error = QStringLiteral("Cannot write %1: %2").arg(native(target), dst.errorString());
            return false;
        }
        if (!f_.seek(base_ + qint64(e->sector) * kSector)) {
            *error = QStringLiteral("The image is cut short (%1 lies past its end).").arg(e->path);
            return false;
        }
        for (qint64 left = e->size; left > 0;) {
            if (cancel) {
                dst.close();
                dst.remove();
                *error = QStringLiteral("Cancelled.");
                return false;
            }
            const qint64 n = qMin(left, kChunk);
            if (f_.read(buf.data(), n) != n) {
                dst.close();
                dst.remove();
                *error = QStringLiteral("The image is cut short (%1 lies past its end).").arg(e->path);
                return false;
            }
            if (dst.write(buf.constData(), n) != n) {
                const QString why = dst.errorString();
                dst.close();
                dst.remove();
                *error = QStringLiteral("Cannot write %1: %2 (is the disk full?)").arg(native(target), why);
                return false;
            }
            left -= n;
            bytesDone += n;
        }
        ++filesDone;
    }
    return true;
}

}  // namespace xiso
