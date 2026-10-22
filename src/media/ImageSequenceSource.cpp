#include "ImageSequenceSource.h"
#include "Lang.h"

#include <QCollator>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>

#include <algorithm>

bool ImageSequenceSource::isImageFile(const QString& path)
{
    static const QStringList exts = { "png", "jpg", "jpeg", "bmp", "tif", "tiff", "webp", "tga", "gif" };
    return exts.contains(QFileInfo(path).suffix().toLower());
}

QStringList ImageSequenceSource::naturalSorted(QStringList files)
{
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(files.begin(), files.end(), [&](const QString& a, const QString& b) { return collator.compare(a, b) < 0; });
    return files;
}

QStringList ImageSequenceSource::expandSequence(const QString& file)
{
    const QFileInfo fi(file);
    static const QRegularExpression re(QStringLiteral("^(.*?)(\\d+)$"));
    const auto m = re.match(fi.completeBaseName());
    if (!m.hasMatch()) return { file };

    const QString prefix = m.captured(1);
    const QString suffix = fi.suffix();
    const QRegularExpression sibling(
        QStringLiteral("^%1\\d+\\.%2$").arg(QRegularExpression::escape(prefix), QRegularExpression::escape(suffix)),
        QRegularExpression::CaseInsensitiveOption);

    QStringList out;
    const QDir dir = fi.absoluteDir();
    for (const QString& name : dir.entryList(QDir::Files))
        if (sibling.match(name).hasMatch()) out << dir.absoluteFilePath(name);
    return out.size() > 1 ? naturalSorted(out) : QStringList{ file };
}

bool ImageSequenceSource::open(const QStringList& files, double fps, QString* error)
{
    m_files = files;
    if (m_files.isEmpty()) {
        if (error) *error = L("No image files.", "이미지 파일이 없습니다.");
        return false;
    }
    QImageReader reader(m_files.first());
    const QSize size = reader.size();
    if (!size.isValid()) {
        if (error) *error = L("Can't read image: %1", "이미지를 읽을 수 없습니다: %1").arg(m_files.first());
        return false;
    }
    m_info = {};
    m_info.path = QFileInfo(m_files.first()).absolutePath();
    m_info.width = size.width();
    m_info.height = size.height();
    m_info.fps = fps > 0 ? fps : 24.0;
    m_info.frameCount = m_files.size();
    m_info.duration = m_files.size() / m_info.fps;
    m_info.codec = QStringLiteral("Image sequence (%1)").arg(QFileInfo(m_files.first()).suffix().toUpper());
    m_info.pixelFormat = QStringLiteral("RGBA");
    m_info.isImageSequence = true;
    m_info.decoderName = QStringLiteral("Qt image reader");
    return true;
}

bool ImageSequenceSource::decode(int64_t index, QImage& out, QString* error)
{
    if (index < 0 || index >= m_files.size()) return false;
    QImageReader reader(m_files.at(static_cast<qsizetype>(index)));
    reader.setAutoTransform(true);
    QImage img = reader.read();
    if (img.isNull()) {
        if (error) *error = reader.errorString();
        return false;
    }
    // Sequences with mixed resolutions are fitted to the first frame's size.
    if (img.size() != QSize(m_info.width, m_info.height))
        img = img.scaled(m_info.width, m_info.height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    out = img.convertToFormat(QImage::Format_RGBA8888);
    return true;
}
