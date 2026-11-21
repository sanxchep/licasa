#pragma once

// Test-only. Builds Ultra HDR JPEG variants from a checked-in fixture without
// the gain-map codec: another primary image, an EXIF orientation, or damage.
// The donor's gain-map JPEG and its ISO/MPF metadata are kept byte for byte.

#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QImageWriter>
#include <QSize>
#include <QtEndian>

#include <array>

namespace Licasa::TestSupport {
// Four flat quadrants: top-left, top-right, bottom-left, bottom-right.
inline constexpr std::array<QRgb, 4> quadrantColors{qRgb(200, 40, 40), qRgb(40, 200, 40),
                                                    qRgb(40, 40, 200), qRgb(230, 230, 230)};

inline QImage quadrantImage(QSize size)
{
    QImage image(size, QImage::Format_RGB888);
    for (int y = 0; y < size.height(); ++y)
        for (int x = 0; x < size.width(); ++x)
            image.setPixel(x, y, quadrantColors[size_t((y >= size.height() / 2) * 2 + (x >= size.width() / 2))]);
    return image;
}

struct MpfLayout {
    qsizetype tiff = -1;      // Offset of the MPF TIFF header in the file.
    quint32 primaryBytes = 0; // Length of the primary JPEG.
    quint32 gainBytes = 0;
    quint32 gainOffset = 0; // Relative to the TIFF header.
    bool valid() const { return tiff > 0; }
};

// The layout written by libultrahdr: big-endian TIFF, three tags, two entries.
inline MpfLayout mpfLayout(const QByteArray& bytes)
{
    const auto marker = bytes.indexOf(QByteArray("MPF\0", 4));
    if (marker < 0 || marker + 4 + 78 > bytes.size() || bytes.mid(marker + 4, 2) != "MM") return {};
    const char* tiff = bytes.constData() + marker + 4;
    MpfLayout layout{marker + 4, qFromBigEndian<quint32>(tiff + 54), qFromBigEndian<quint32>(tiff + 70),
                     qFromBigEndian<quint32>(tiff + 74)};
    const bool consistent = layout.primaryBytes > 0 && layout.gainBytes > 0
        && qsizetype(layout.primaryBytes) + layout.gainBytes == bytes.size()
        && layout.tiff + layout.gainOffset == layout.primaryBytes;
    return consistent ? layout : MpfLayout{};
}

namespace Detail {
// Splits the segments before the first scan into metadata (APPn, COM) and the rest.
inline bool splitJpeg(const QByteArray& jpeg, QByteArray& metadata, QByteArray& coding)
{
    if (!jpeg.startsWith("\xff\xd8")) return false;
    qsizetype position = 2;
    while (position + 4 <= jpeg.size() && uchar(jpeg[position]) == 0xff) {
        const uchar marker = uchar(jpeg[position + 1]);
        if (marker == 0xda) {
            coding += jpeg.mid(position); // The scan and everything after it.
            return true;
        }
        const auto length = qsizetype(qFromBigEndian<quint16>(jpeg.constData() + position + 2)) + 2;
        if (length < 4 || position + length > jpeg.size()) return false;
        ((marker >= 0xe0 && marker <= 0xef) || marker == 0xfe ? metadata : coding) += jpeg.mid(position, length);
        position += length;
    }
    return false;
}
} // namespace Detail

// Replaces the donor's primary image. The donor's metadata segments, including
// its colour profile and multi-picture index, come first; the gain map follows.
inline QByteArray gainMapJpegWithPrimary(const QByteArray& donor, const QImage& primary)
{
    const auto layout = mpfLayout(donor);
    QByteArray encoded;
    QBuffer buffer(&encoded);
    QImageWriter writer(&buffer, "jpeg");
    writer.setQuality(100);
    if (!layout.valid() || !buffer.open(QIODevice::WriteOnly) || !writer.write(primary)) return {};
    QByteArray donorMetadata, donorCoding, ownMetadata, ownCoding;
    if (!Detail::splitJpeg(donor.left(layout.primaryBytes), donorMetadata, donorCoding)
        || !Detail::splitJpeg(encoded, ownMetadata, ownCoding)) return {};
    QByteArray result = QByteArray("\xff\xd8", 2) + donorMetadata + ownCoding;
    const auto marker = result.indexOf(QByteArray("MPF\0", 4));
    if (marker < 0) return {};
    char* tiff = result.data() + marker + 4;
    qToBigEndian<quint32>(quint32(result.size()), tiff + 54);
    qToBigEndian<quint32>(quint32(result.size() - (marker + 4)), tiff + 74);
    result += donor.mid(layout.primaryBytes);
    return mpfLayout(result).valid() ? result : QByteArray();
}

// Adds an EXIF block with only an orientation tag to the primary image.
inline QByteArray withExifOrientation(QByteArray jpeg, int orientation)
{
    const auto layout = mpfLayout(jpeg);
    if (!layout.valid() || orientation < 1 || orientation > 8) return {};
    QByteArray exif = QByteArray::fromHex("ffe1002245786966000049492a0008000000010012010300010000000100000000000000");
    qToLittleEndian<quint16>(quint16(orientation), exif.data() + 28);
    // The index moves with the gain map, so its relative offset stays valid.
    qToBigEndian<quint32>(layout.primaryBytes + quint32(exif.size()), jpeg.data() + layout.tiff + 54);
    jpeg.insert(2, exif);
    return mpfLayout(jpeg).valid() ? jpeg : QByteArray();
}

// Size of the image as displayed for an EXIF orientation.
inline QSize displayedSize(QSize stored, int orientation)
{
    return orientation >= 5 ? stored.transposed() : stored;
}

// Where a displayed pixel comes from in the stored image. Written from the
// EXIF definition, independently of any image library.
inline QPoint storedPoint(QSize stored, int orientation, QPoint shown)
{
    const int w = stored.width() - 1, h = stored.height() - 1, x = shown.x(), y = shown.y();
    switch (orientation) {
    case 2: return {w - x, y};
    case 3: return {w - x, h - y};
    case 4: return {x, h - y};
    case 5: return {y, x};
    case 6: return {y, h - x};
    case 7: return {w - y, h - x};
    case 8: return {w - y, x};
    default: return {x, y};
    }
}

// Colour of quadrantImage(stored) at a displayed pixel.
inline QColor displayedQuadrant(QSize stored, int orientation, QPoint shown)
{
    const QPoint source = storedPoint(stored, orientation, shown);
    return QColor(quadrantColors[size_t((source.y() >= stored.height() / 2) * 2 + (source.x() >= stored.width() / 2))]);
}

inline bool closeTo(const QColor& actual, const QColor& expected, int tolerance)
{
    return qAbs(actual.red() - expected.red()) <= tolerance && qAbs(actual.green() - expected.green()) <= tolerance
        && qAbs(actual.blue() - expected.blue()) <= tolerance;
}

// Checks the four quadrant centres of an upright image against the stored
// quadrants and reports the first mismatch.
inline QString quadrantMismatch(const QImage& upright, QSize stored, int orientation, int tolerance = 12)
{
    const QSize shown = displayedSize(stored, orientation);
    if (upright.size() != shown)
        return QStringLiteral("size %1x%2, expected %3x%4").arg(upright.width()).arg(upright.height())
            .arg(shown.width()).arg(shown.height());
    for (const QPoint point : {QPoint(shown.width() / 4, shown.height() / 4), QPoint(shown.width() * 3 / 4, shown.height() / 4),
                               QPoint(shown.width() / 4, shown.height() * 3 / 4),
                               QPoint(shown.width() * 3 / 4, shown.height() * 3 / 4)}) {
        const QColor actual = upright.pixelColor(point), expected = displayedQuadrant(stored, orientation, point);
        if (!closeTo(actual, expected, tolerance))
            return QStringLiteral("pixel %1,%2 is %3, expected %4").arg(point.x()).arg(point.y())
                .arg(actual.name(), expected.name());
    }
    return {};
}
} // namespace Licasa::TestSupport
