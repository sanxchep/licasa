#include "imaging/image_decode_contract.h"

#include <QBuffer>
#include <QColorSpace>
#include <QImageReader>
#include <QTest>
#include <QtEndian>

#include <array>
#include <atomic>

namespace {
class CancellingBuffer final : public QBuffer {
  public:
    CancellingBuffer(QByteArray* bytes, std::atomic_bool& cancellation)
        : QBuffer(bytes), cancellation_(cancellation)
    {}

  protected:
    qint64 readData(char* destination, qint64 maximumBytes) override
    {
        const qint64 count = QBuffer::readData(destination, maximumBytes);
        if (count > 0) {
            consumed_ += count;
            if (consumed_ > 64 * 1024) {
                cancellation_.store(true, std::memory_order_relaxed);
            }
        }
        return count;
    }

  private:
    std::atomic_bool& cancellation_;
    qint64 consumed_ = 0;
};

QByteArray encode(const QImage& image)
{
    QByteArray bytes("qoif", 4);
    char dimensions[8];
    qToBigEndian<quint32>(quint32(image.width()), reinterpret_cast<uchar*>(dimensions));
    qToBigEndian<quint32>(quint32(image.height()), reinterpret_cast<uchar*>(dimensions + 4));
    bytes.append(dimensions, 8);
    bytes.append(char(4));
    bytes.append(char(0));
    std::array<QRgb, 64> index{};
    QRgb previous = qRgba(0, 0, 0, 255);
    int run = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            if (pixel == previous) {
                if (++run == 62) {
                    bytes.append(char(0xc0 | (run - 1)));
                    run = 0;
                }
                continue;
            }
            if (run) {
                bytes.append(char(0xc0 | (run - 1)));
                run = 0;
            }
            const int hash =
                (qRed(pixel) * 3 + qGreen(pixel) * 5 + qBlue(pixel) * 7 + qAlpha(pixel) * 11) & 63;
            const int dr = qRed(pixel) - qRed(previous);
            const int dg = qGreen(pixel) - qGreen(previous);
            const int db = qBlue(pixel) - qBlue(previous);
            if (index[size_t(hash)] == pixel) {
                bytes.append(char(hash));
            } else if (qAlpha(pixel) != qAlpha(previous)) {
                bytes.append(char(0xff));
                bytes.append(char(qRed(pixel)));
                bytes.append(char(qGreen(pixel)));
                bytes.append(char(qBlue(pixel)));
                bytes.append(char(qAlpha(pixel)));
            } else if (dr >= -2 && dr <= 1 && dg >= -2 && dg <= 1 && db >= -2 && db <= 1) {
                bytes.append(char(0x40 | ((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2)));
            } else if (dg >= -32 && dg <= 31 && dr - dg >= -8 && dr - dg <= 7 && db - dg >= -8 &&
                       db - dg <= 7) {
                bytes.append(char(0x80 | (dg + 32)));
                bytes.append(char(((dr - dg + 8) << 4) | (db - dg + 8)));
            } else {
                bytes.append(char(0xfe));
                bytes.append(char(qRed(pixel)));
                bytes.append(char(qGreen(pixel)));
                bytes.append(char(qBlue(pixel)));
            }
            index[size_t(hash)] = pixel;
            previous = pixel;
        }
    }
    if (run) {
        bytes.append(char(0xc0 | (run - 1)));
    }
    bytes.append(QByteArray::fromHex("0000000000000001"));
    return bytes;
}

QImage sourceImage(const QSize& size, int pattern)
{
    QImage source(size, QImage::Format_ARGB32);
    quint32 state = 0x83a3ec7d;
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            const int ordinal = y * size.width() + x;
            const QRgb value =
                pattern == 0   ? qRgba((ordinal / 127) & 255, 53, 109, 201)
                : pattern == 1 ? qRgba((x + y) & 255, (x + y + 19) & 255, (x + y + 7) & 255, 255)
                               : QRgb(state);
            source.setPixel(x, y, value);
        }
    }
    source.setColorSpace(QColorSpace::SRgb);
    return source;
}

QImage decode(QByteArray bytes, const QSize& requested = {},
              const std::atomic_bool* cancel = nullptr)
{
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return {};
    }
    QImageReader reader(&buffer, "qoi");
    Licasa::ImageDecodeContract::configure(reader, 10'000'000, cancel);
    if (requested.isValid()) {
        reader.setScaledSize(requested);
    }
    return reader.read();
}
} // namespace

class QoiRuntimeTest final : public QObject {
    Q_OBJECT

  private slots:
    void initialPixelState_data()
    {
        QTest::addColumn<QByteArray>("chunks");
        QTest::addColumn<QRgb>("expectedPixel");
        for (int slot = 0; slot < 64; ++slot) {
            QTest::newRow(qPrintable(QStringLiteral("index-%1").arg(slot)))
                << QByteArray(1, char(slot)) << qRgba(0, 0, 0, 0);
        }
        QTest::newRow("run") << QByteArray::fromHex("c0") << qRgba(0, 0, 0, 255);
        QTest::newRow("rgb") << QByteArray::fromHex("fe112233") << qRgba(17, 34, 51, 255);
        QTest::newRow("diff") << QByteArray::fromHex("7f") << qRgba(1, 1, 1, 255);
        QTest::newRow("luma") << QByteArray::fromHex("a188") << qRgba(1, 1, 1, 255);
    }

    void initialPixelState()
    {
        QFETCH(QByteArray, chunks);
        QFETCH(QRgb, expectedPixel);
        // Independent QOI 1.0 stream: 1x1 RGBA/sRGB, one opcode, end marker.
        const QByteArray bytes = QByteArray::fromHex("716f696600000001000000010400") + chunks +
                                 QByteArray::fromHex("0000000000000001");
        for (const QSize& target : {QSize(1, 1), QSize(3, 2)}) {
            QImage expected(target, QImage::Format_ARGB32);
            expected.fill(expectedPixel);
            expected.setColorSpace(QColorSpace::SRgb);
            QCOMPARE(decode(bytes, target), expected);
        }
    }

    void transparentIndexAndAlphaTransitions()
    {
        QImage source(67, 3, QImage::Format_ARGB32);
        const QRgb pixels[] = {qRgba(0, 0, 0, 0),   qRgba(0, 0, 0, 0),      qRgba(0, 0, 0, 255),
                               qRgba(0, 0, 0, 255), qRgba(17, 34, 51, 255), qRgba(17, 34, 51, 23),
                               qRgba(0, 0, 0, 0)};
        for (int y = 0; y < source.height(); ++y) {
            for (int x = 0; x < source.width(); ++x) {
                source.setPixel(x, y, pixels[(y * source.width() + x) % std::size(pixels)]);
            }
        }
        source.setColorSpace(QColorSpace::SRgb);
        const QByteArray bytes = encode(source);
        QCOMPARE(uchar(bytes[14]), uchar(0)); // The independent encoder uses the initial index.
        QCOMPARE(decode(bytes), source);
        QImage expected(7, 2, QImage::Format_ARGB32);
        for (int y = 0; y < expected.height(); ++y) {
            for (int x = 0; x < expected.width(); ++x) {
                const int sx = int((2ll * x + 1) * source.width() / (2ll * expected.width()));
                const int sy = int((2ll * y + 1) * source.height() / (2ll * expected.height()));
                expected.setPixel(x, y, source.pixel(sx, sy));
            }
        }
        expected.setColorSpace(QColorSpace::SRgb);
        QCOMPARE(decode(bytes, expected.size()), expected);
    }

    void exactRunsOpcodesAndSampling()
    {
        const QSize sizes[] = {{1, 1}, {3, 2}, {67, 5}, {257, 129}, {100000, 1}, {1, 100000}};
        const QSize targets[] = {{1, 1}, {3, 2}, {17, 11}, {513, 7}, {7, 513}, {99999, 1}};
        for (const QSize& size : sizes) {
            for (int pattern = 0; pattern < 3; ++pattern) {
                const QImage source = sourceImage(size, pattern);
                const QByteArray bytes = encode(source);
                const QImage full = decode(bytes);
                QVERIFY(!full.isNull());
                QCOMPARE(full, source);
                for (const QSize& target : targets) {
                    const QSize sampledSize = target.boundedTo(size);
                    QImage expected(sampledSize, QImage::Format_ARGB32);
                    for (int y = 0; y < sampledSize.height(); ++y) {
                        const int sy =
                            int((2ll * y + 1) * size.height() / (2ll * sampledSize.height()));
                        for (int x = 0; x < sampledSize.width(); ++x) {
                            const int sx =
                                int((2ll * x + 1) * size.width() / (2ll * sampledSize.width()));
                            expected.setPixel(x, y, source.pixel(sx, sy));
                        }
                    }
                    if (sampledSize != target) {
                        expected =
                            expected.scaled(target, Qt::IgnoreAspectRatio, Qt::FastTransformation);
                    }
                    expected.setColorSpace(QColorSpace::SRgb);
                    const QImage actual = decode(bytes, target);
                    QVERIFY(!actual.isNull());
                    QCOMPARE(actual, expected);
                }
            }
        }
    }

    void samplingWidthBoundaries()
    {
        for (int width = 1; width <= 17; ++width) {
            for (int pattern = 0; pattern < 3; ++pattern) {
                const QImage source = sourceImage({width, 7}, pattern);
                const QByteArray bytes = encode(source);
                for (int requestedWidth = 1; requestedWidth <= width + 2; ++requestedWidth) {
                    for (int requestedHeight : {1, 4, 9}) {
                        const QSize target(requestedWidth, requestedHeight);
                        const QSize sampled = target.boundedTo(source.size());
                        QImage expected(sampled, QImage::Format_ARGB32);
                        for (int y = 0; y < sampled.height(); ++y) {
                            const int sy =
                                int((2ll * y + 1) * source.height() / (2ll * sampled.height()));
                            for (int x = 0; x < sampled.width(); ++x) {
                                const int sx =
                                    int((2ll * x + 1) * source.width() / (2ll * sampled.width()));
                                expected.setPixel(x, y, source.pixel(sx, sy));
                            }
                        }
                        if (sampled != target) {
                            expected = expected.scaled(target, Qt::IgnoreAspectRatio,
                                                       Qt::FastTransformation);
                        }
                        expected.setColorSpace(QColorSpace::SRgb);
                        QCOMPARE(decode(bytes, target), expected);
                    }
                }
            }
        }
    }

    void truncatedTagsAndEndMarkerAreRejected()
    {
        const QByteArray bytes = encode(sourceImage({67, 5}, 2));
        for (int length = 0; length < bytes.size(); ++length) {
            QVERIFY(decode(bytes.first(length)).isNull());
        }
        QByteArray corrupt = bytes;
        corrupt[corrupt.size() - 1] = char(0);
        QVERIFY(decode(corrupt).isNull());
    }

    void cancellationDuringInputPublishesNoRaster()
    {
        QByteArray bytes = encode(sourceImage({257, 129}, 2));
        std::atomic_bool cancellation{false};
        CancellingBuffer buffer(&bytes, cancellation);
        QVERIFY(buffer.open(QIODevice::ReadOnly));
        QImageReader reader(&buffer, "qoi");
        Licasa::ImageDecodeContract::configure(reader, 10'000'000, &cancellation);
        QVERIFY(reader.read().isNull());
        QVERIFY(cancellation.load(std::memory_order_relaxed));
    }

    void cancelledDecodePublishesNoRaster()
    {
        const QByteArray bytes = encode(sourceImage({257, 129}, 0));
        std::atomic_bool cancellation{true};
        QVERIFY(decode(bytes, {}, &cancellation).isNull());
    }
};

QTEST_GUILESS_MAIN(QoiRuntimeTest)
#include "tst_qoi_runtime.moc"
