#include "io/byte_range_device.h"

#include <QDateTime>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <limits>
#include <thread>

using namespace Licasa;

class ByteRangeDeviceTest final : public QObject {
    Q_OBJECT

  private:
    static QString writeFixture(QTemporaryDir& directory, const QByteArray& bytes)
    {
        const QString path = directory.filePath(QStringLiteral("compound.bin"));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
            return {};
        }
        file.close();
        return path;
    }

  private slots:
    void checkedArithmeticRejectsOverflow()
    {
        const auto sum = checkedByteOffsetAdd(12, 30);
        QVERIFY(sum.has_value());
        QCOMPARE(*sum, quint64(42));
        QVERIFY(!checkedByteOffsetAdd(std::numeric_limits<quint64>::max(), 1));
        QVERIFY(!checkedByteOffsetAdd(std::numeric_limits<quint64>::max() - 2, 3));

        const auto valid = checkedByteRange(100, 10, 90);
        QVERIFY(valid.has_value());
        QCOMPARE(valid->offset, quint64(10));
        QCOMPARE(valid->length, quint64(90));
        QVERIFY(!checkedByteRange(100, 101, 0));
        QVERIFY(!checkedByteRange(100, 99, 2));
        QVERIFY(!checkedByteRange(100, std::numeric_limits<quint64>::max(), 1));
        QVERIFY(!checkedByteRange(std::numeric_limits<quint64>::max(),
                                  std::numeric_limits<quint64>::max() - 1, 2));
    }

    void zeroLengthAndExactEofAreValid()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = writeFixture(directory, QByteArray("0123456789", 10));
        QVERIFY(!path.isEmpty());

        ByteRangeDevice zero(path, 10, 0);
        QVERIFY(zero.open(QIODevice::ReadOnly));
        QCOMPARE(zero.size(), qint64(0));
        QCOMPARE(zero.pos(), qint64(0));
        QVERIFY(zero.atEnd());
        QCOMPARE(zero.read(1), QByteArray());
        QVERIFY(zero.seek(0));
        QVERIFY(!zero.seek(1));

        ByteRangeDevice suffix(path, 4, 6);
        QVERIFY(suffix.open(QIODevice::ReadOnly));
        QCOMPARE(suffix.readAll(), QByteArray("456789", 6));
        QCOMPARE(suffix.pos(), qint64(6));
        QVERIFY(suffix.atEnd());
        QVERIFY(suffix.seek(6));
        QCOMPARE(suffix.read(1), QByteArray());
    }

    void readsAndSeeksCannotEscapeRange()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = writeFixture(directory, QByteArray("ABCDEFGHIJKLMNO", 15));
        QVERIFY(!path.isEmpty());

        ByteRangeDevice device(path, 5, 5);
        QVERIFY(device.open(QIODevice::ReadOnly));
        QVERIFY(!device.isSequential());
        QCOMPARE(device.size(), qint64(5));
        QCOMPARE(device.bytesAvailable(), qint64(5));

        QCOMPARE(device.read(100), QByteArray("FGHIJ", 5));
        QCOMPARE(device.pos(), qint64(5));
        QCOMPARE(device.bytesAvailable(), qint64(0));
        QVERIFY(!device.seek(-1));
        QVERIFY(!device.seek(6));
        QVERIFY(device.seek(2));
        QCOMPARE(device.read(2), QByteArray("HI", 2));
        QCOMPARE(device.pos(), qint64(4));

        device.close();
        QVERIFY(!device.open(QIODevice::WriteOnly));
        QVERIFY(device.errorString().contains(QStringLiteral("read-only")));
    }

    void hugeMetadataIsRejectedBeforeSignedConversion()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = writeFixture(directory, QByteArray("small", 5));
        QVERIFY(!path.isEmpty());

        const quint64 tooLarge = quint64(std::numeric_limits<qint64>::max()) + 1;
        ByteRangeDevice offset(path, tooLarge, 0);
        QVERIFY(!offset.open(QIODevice::ReadOnly));
        QVERIFY(offset.errorString().contains(QStringLiteral("seekable")));

        ByteRangeDevice length(path, 0, tooLarge);
        QVERIFY(!length.open(QIODevice::ReadOnly));
        QVERIFY(length.errorString().contains(QStringLiteral("seekable")));
    }

    void truncationAfterOpenFailsWithoutEscaping()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = writeFixture(directory, QByteArray("0123456789ABCDEF", 16));
        QVERIFY(!path.isEmpty());

        ByteRangeDevice device(path, 8, 8);
        QVERIFY(device.open(QIODevice::ReadOnly));
        QCOMPARE(device.read(2), QByteArray("89", 2));
        QCOMPARE(device.pos(), qint64(2));

        QFile shrink(path);
        QVERIFY(shrink.open(QIODevice::ReadWrite));
        QVERIFY(shrink.resize(10));
        shrink.close();

        char byte = '\0';
        QCOMPARE(device.read(&byte, 1), qint64(-1));
        QCOMPARE(device.pos(), qint64(2));
        QVERIFY(device.errorString().contains(QStringLiteral("truncated")));
        QVERIFY(!device.seek(0));
    }

    void pinnedIdentityRejectsReplacementBeforeOpen()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = writeFixture(directory, QByteArray("ORIGINAL", 8));
        QVERIFY(!path.isEmpty());

        QFile original(path);
        QVERIFY(original.open(QIODevice::ReadOnly));
        QString reason;
        const auto identity = externalFileIdentity(original, &reason);
        QVERIFY2(identity.has_value(), qPrintable(reason));
        original.close();

        const QString replacement = directory.filePath(QStringLiteral("replacement.bin"));
        QFile replacementFile(replacement);
        QVERIFY(replacementFile.open(QIODevice::WriteOnly));
        QCOMPARE(replacementFile.write("REPLACED", 8), qint64(8));
        replacementFile.close();
        QVERIFY(QFile::remove(path));
        QVERIFY(QFile::rename(replacement, path));

        ByteRangeDevice device(path, 0, identity->size, *identity);
        QVERIFY(!device.open(QIODevice::ReadOnly));
        QVERIFY(device.errorString().contains(QStringLiteral("changed after Live Photo pairing")));
    }

    void pinnedDescriptorCannotBeRedirectedByPathReplacement()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = writeFixture(directory, QByteArray("ORIGINAL", 8));
        QVERIFY(!path.isEmpty());

        QFile original(path);
        QVERIFY(original.open(QIODevice::ReadOnly));
        QString reason;
        const auto identity = externalFileIdentity(original, &reason);
        QVERIFY2(identity.has_value(), qPrintable(reason));
        original.close();

        ByteRangeDevice device(path, 0, identity->size, *identity);
        QVERIFY2(device.open(QIODevice::ReadOnly), qPrintable(device.errorString()));

        const QString replacement = directory.filePath(QStringLiteral("replacement.bin"));
        QFile replacementFile(replacement);
        QVERIFY(replacementFile.open(QIODevice::WriteOnly));
        QCOMPARE(replacementFile.write("REPLACED", 8), qint64(8));
        replacementFile.close();
        QVERIFY(QFile::remove(path));
        QVERIFY(QFile::rename(replacement, path));

        // The opened descriptor still refers to the validated inode. Replacing
        // the directory entry cannot redirect a live playback session; the
        // link/ctime change is detected and the pinned read fails closed.
        char byte = '\0';
        QCOMPARE(device.read(&byte, 1), qint64(-1));
        QVERIFY(device.errorString().contains(QStringLiteral("changed after Live Photo pairing")));
        QFile current(path);
        QVERIFY(current.open(QIODevice::ReadOnly));
        QCOMPARE(current.readAll(), QByteArray("REPLACED", 8));
    }

    void pinnedIdentityRejectsInPlaceMutation()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = writeFixture(directory, QByteArray("0123456789ABCDEF", 16));
        QVERIFY(!path.isEmpty());

        QFile original(path);
        QVERIFY(original.open(QIODevice::ReadOnly));
        QString reason;
        const auto identity = externalFileIdentity(original, &reason);
        QVERIFY2(identity.has_value(), qPrintable(reason));
        original.close();

        ByteRangeDevice device(path, 0, identity->size, *identity);
        QVERIFY2(device.open(QIODevice::ReadOnly), qPrintable(device.errorString()));
        QCOMPARE(device.read(2), QByteArray("01", 2));

        QFile mutate(path);
        QVERIFY(mutate.open(QIODevice::ReadWrite));
        QVERIFY(mutate.seek(4));
        QCOMPARE(mutate.write("XX", 2), qint64(2));
        QVERIFY(mutate.flush());
        // The hosted builder can report coarse timestamps for rapid writes.
        // Force a distinct mtime so this test checks identity revalidation.
        QVERIFY(mutate.setFileTime(QDateTime::fromSecsSinceEpoch(identity->modifiedSeconds - 10),
                                   QFileDevice::FileModificationTime));
        mutate.close();

        char byte = '\0';
        QCOMPARE(device.read(&byte, 1), qint64(-1));
        QVERIFY(device.errorString().contains(QStringLiteral("changed after Live Photo pairing")));
    }

    void concurrentCancellationStopsSubsequentOperations()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = writeFixture(directory, QByteArray(1024 * 1024, 'x'));
        QVERIFY(!path.isEmpty());

        std::atomic_bool cancelled{false};
        ByteRangeDevice device(path, 0, 1024 * 1024, &cancelled);
        QVERIFY(device.open(QIODevice::ReadOnly));
        QCOMPARE(device.read(16), QByteArray(16, 'x'));

        std::thread cancelThread(
            [&cancelled] { cancelled.store(true, std::memory_order_relaxed); });
        cancelThread.join();

        char byte = '\0';
        QCOMPARE(device.read(&byte, 1), qint64(-1));
        QVERIFY(device.errorString().contains(QStringLiteral("cancelled")));
        QVERIFY(!device.seek(0));
    }
};

QTEST_MAIN(ByteRangeDeviceTest)
#include "tst_byte_range_device.moc"
