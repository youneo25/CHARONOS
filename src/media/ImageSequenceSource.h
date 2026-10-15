#pragma once

#include "FrameSource.h"

#include <QStringList>

class ImageSequenceSource final : public FrameSource {
public:
    bool open(const QStringList& files, double fps, QString* error);
    const MediaInfo& info() const override { return m_info; }
    bool decode(int64_t index, QImage& out, QString* error, const FrameSink* sink = nullptr) override;
    bool supportsParallelDecode() const override { return true; }

    const QStringList& files() const { return m_files; }

    // "shot_0012.png" -> every sibling "shot_####.png", naturally sorted. Returns {file} if no numbering.
    static QStringList expandSequence(const QString& file);
    static QStringList naturalSorted(QStringList files);
    static bool isImageFile(const QString& path);

private:
    MediaInfo m_info;
    QStringList m_files;
};
