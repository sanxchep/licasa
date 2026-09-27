#include "io/atomic_file_copy.h"

#include <QDir>
#include <QTemporaryDir>
#include <QTest>

#include <limits>

using namespace Licasa;

namespace {
bool writeBytes(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray readBytes(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

// Deterministically change the source after its last read, before commit. This
// exercises the final identity check without a timing-dependent writer thread.
class MutatingFile final : public QFile {
  public:
    using QFile::QFile;

  protected:
    qint64 readData(char* data, qint64 size) override
    {
        const qint64 result = QFile::readData(data, size);
        QFile writer(fileName());
        if (writer.open(QIODevice::Append)) {
            writer.write("changed");
            writer.close();
        }
        return result;
    }
};
} // namespace

class AtomicFileCopyTest final : public QObject {
    Q_OBJECT

  private slots:
    void boundedRangeCopyReplacesDestination()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QByteArray payload(800003, Qt::Uninitialized);
        for (qsizetype i = 0; i < payload.size(); ++i) {
            payload[i] = char(i % 251);
        }
        const QString sourcePath = dir.filePath("source");
        const QString destination = dir.filePath("copy");
        const QByteArray original = "prefix" + payload + "trailer";
        QVERIFY(writeBytes(sourcePath, original));
        QVERIFY(writeBytes(destination, "old destination"));
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::ReadOnly | QIODevice::Unbuffered));
        const auto identity = externalFileIdentity(source);
        QVERIFY(identity);
        const QString error =
            copyFileRangeAtomically(source, *identity, {6, quint64(payload.size())}, destination);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(readBytes(destination), payload);
        QCOMPARE(readBytes(sourcePath), original);
    }

    void invalidRangePreservesDestination_data()
    {
        QTest::addColumn<quint64>("offset");
        QTest::addColumn<quint64>("length");
        QTest::newRow("past-end") << quint64(7) << quint64(1);
        QTest::newRow("too-long") << quint64(2) << quint64(5);
        QTest::newRow("overflow") << quint64(1) << std::numeric_limits<quint64>::max();
    }

    void invalidRangePreservesDestination()
    {
        QFETCH(quint64, offset);
        QFETCH(quint64, length);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sourcePath = dir.filePath("source");
        const QString destination = dir.filePath("copy");
        QVERIFY(writeBytes(sourcePath, "source"));
        QVERIFY(writeBytes(destination, "keep"));
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::ReadOnly | QIODevice::Unbuffered));
        const auto identity = externalFileIdentity(source);
        QVERIFY(identity);
        QVERIFY(
            !copyFileRangeAtomically(source, *identity, {offset, length}, destination).isEmpty());
        QCOMPARE(readBytes(destination), QByteArray("keep"));
    }

    void linkedDestinationIsRejected_data()
    {
        QTest::addColumn<bool>("hardLink");
        QTest::newRow("symlink") << false;
        QTest::newRow("hard-link") << true;
    }

    void linkedDestinationIsRejected()
    {
        QFETCH(bool, hardLink);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sourcePath = dir.filePath("source");
        const QString destination = dir.filePath("alias");
        QVERIFY(writeBytes(sourcePath, "source"));
        if (hardLink) {
            QCOMPARE(::link(QFile::encodeName(sourcePath).constData(),
                            QFile::encodeName(destination).constData()),
                     0);
        } else {
            QVERIFY(QFile::link(sourcePath, destination));
        }
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::ReadOnly | QIODevice::Unbuffered));
        const auto identity = externalFileIdentity(source);
        QVERIFY(identity);
        QVERIFY(!copyFileRangeAtomically(source, *identity, {0, 6}, destination).isEmpty());
        QCOMPARE(readBytes(sourcePath), QByteArray("source"));
        QCOMPARE(readBytes(destination), QByteArray("source"));
    }

    void sourceChangeAfterLastReadCancelsCommit()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sourcePath = dir.filePath("source");
        const QString destination = dir.filePath("copy");
        QVERIFY(writeBytes(sourcePath, "source"));
        QVERIFY(writeBytes(destination, "keep"));
        MutatingFile source(sourcePath);
        QVERIFY(source.open(QIODevice::ReadOnly | QIODevice::Unbuffered));
        const auto identity = externalFileIdentity(source);
        QVERIFY(identity);
        QVERIFY(!copyFileRangeAtomically(source, *identity, {0, 6}, destination).isEmpty());
        QCOMPARE(readBytes(destination), QByteArray("keep"));
        QCOMPARE(readBytes(sourcePath), QByteArray("sourcechanged"));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden).size(), 2);
    }

    void changedIdentityIsRejectedBeforeCopy()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sourcePath = dir.filePath("source");
        const QString destination = dir.filePath("copy");
        QVERIFY(writeBytes(sourcePath, "source"));
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::ReadOnly | QIODevice::Unbuffered));
        const auto identity = externalFileIdentity(source);
        QVERIFY(identity);
        QVERIFY(writeBytes(sourcePath, "replacement"));
        QVERIFY(!copyFileRangeAtomically(source, *identity, {0, 6}, destination).isEmpty());
        QVERIFY(!QFile::exists(destination));
    }

    void destinationFailurePreservesSource()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sourcePath = dir.filePath("source");
        QVERIFY(writeBytes(sourcePath, "source"));
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::ReadOnly | QIODevice::Unbuffered));
        const auto identity = externalFileIdentity(source);
        QVERIFY(identity);
        QVERIFY(!copyFileRangeAtomically(source, *identity, {0, 6}, dir.path()).isEmpty());
        QCOMPARE(readBytes(sourcePath), QByteArray("source"));
    }
};

QTEST_GUILESS_MAIN(AtomicFileCopyTest)
#include "tst_atomic_file_copy.moc"
