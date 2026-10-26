#pragma once
// Original Xbox disc images (XDVDFS, "XISO"), read straight from the image file: the file table, one file, or every
// file into a folder. A plain XISO has the file system at offset 0; a whole-disc dump has it behind the video
// partition. The launcher's disc step, without Python (tools/py/xiso.py is the same reader).

#include <QFile>
#include <QList>
#include <QString>

#include <atomic>

namespace xiso {

struct Entry {
    QString path;  // relative, '/' separated
    quint32 sector = 0;
    quint32 size = 0;
    bool dir = false;
};

class Image {
public:
    bool open(const QString& path, QString* error);
    const QList<Entry>& entries() const { return entries_; }
    qint64 base() const { return base_; }
    qint64 totalBytes() const;  // of the files
    int fileCount() const;
    const Entry* find(const QString& path) const;  // case-insensitive
    bool read(const Entry& e, QByteArray* out, QString* error);
    // every file under outDir; bytesDone and filesDone count up as it goes; false on an error or when cancel is set
    bool extractAll(const QString& outDir, const std::atomic<bool>& cancel, std::atomic<qint64>& bytesDone,
                    std::atomic<int>& filesDone, QString* error);

private:
    bool findBase(QString* error);
    bool walk(quint32 sector, quint32 size, const QString& prefix, int depth, QString* error);

    QFile f_;
    qint64 base_ = 0;
    QList<Entry> entries_;
};

}  // namespace xiso
