#include "imaging/format_support.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class FormatNavigationTest final : public QObject {
    Q_OBJECT

  private slots:
    void browsesReadableImagesInFilenameOrder();
    void handlesLargeDirectoriesAndMissingCurrentFile();
    void keepsFiveNeighborsOnEitherSide();
};

void FormatNavigationTest::browsesReadableImagesInFilenameOrder()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    for (const QString& name : {QStringLiteral("b photo.png"), QStringLiteral("a.jpg"),
                                QStringLiteral("C.PNG"), QStringLiteral("notes.txt")}) {
        QFile file(directory.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
    }

    const Licasa::FormatSupport formats;
    const QUrl first = QUrl::fromLocalFile(directory.filePath(QStringLiteral("a.jpg")));
    const QUrl second = QUrl::fromLocalFile(directory.filePath(QStringLiteral("b photo.png")));
    const QUrl third = QUrl::fromLocalFile(directory.filePath(QStringLiteral("C.PNG")));
    QCOMPARE(formats.adjacentImage(first, 1), second);
    QCOMPARE(formats.adjacentImage(second, -1), first);
    QCOMPARE(formats.adjacentImage(third, 1), first);
    QCOMPARE(formats.adjacentImage(first, -1), third);
    const QList<QUrl> smallWindow = formats.nearbyImages(first, 5);
    QCOMPARE(smallWindow.size(), 2);
    QCOMPARE(smallWindow.at(0), second);
    QCOMPARE(smallWindow.at(1), third);
    QVERIFY(formats.adjacentImage(first, 0).isEmpty());
    QVERIFY(formats.adjacentImage(QUrl(QStringLiteral("https://example.org/a.png")), 1).isEmpty());
}

void FormatNavigationTest::handlesLargeDirectoriesAndMissingCurrentFile()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    for (int index = 0; index < 250; ++index) {
        const QString name = QStringLiteral("image-%1.png").arg(index, 3, 10, QLatin1Char('0'));
        QFile file(directory.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
    }

    const Licasa::FormatSupport formats;
    const auto image = [&directory](int index) {
        return QUrl::fromLocalFile(
            directory.filePath(QStringLiteral("image-%1.png").arg(index, 3, 10, QLatin1Char('0'))));
    };
    QCOMPARE(formats.adjacentImage(image(100), 1), image(101));
    QCOMPARE(formats.adjacentImage(image(100), -1), image(99));
    QCOMPARE(formats.adjacentImage(image(249), 1), image(0));
    QVERIFY(formats.adjacentImage(image(250), -1).isEmpty());
}

void FormatNavigationTest::keepsFiveNeighborsOnEitherSide()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto image = [&directory](int index) {
        return QUrl::fromLocalFile(
            directory.filePath(QStringLiteral("image-%1.png").arg(index, 2, 10, QLatin1Char('0'))));
    };
    for (int index = 0; index < 16; ++index) {
        QFile file(image(index).toLocalFile());
        QVERIFY(file.open(QIODevice::WriteOnly));
    }

    const Licasa::FormatSupport formats;
    const QList<QUrl> aroundSeven = formats.nearbyImages(image(7), 5);
    QCOMPARE(aroundSeven.size(), 10);
    for (int distance = 1; distance <= 5; ++distance) {
        QCOMPARE(aroundSeven.at((distance - 1) * 2), image(7 + distance));
        QCOMPARE(aroundSeven.at((distance - 1) * 2 + 1), image(7 - distance));
    }
    const QList<QUrl> movingRight = formats.nearbyImages(image(7), 5, 1);
    const QList<QUrl> movingLeft = formats.nearbyImages(image(7), 5, -1);
    for (int distance = 1; distance <= 5; ++distance) {
        QCOMPARE(movingRight.at(distance - 1), image(7 + distance));
        QCOMPARE(movingRight.at(5 + distance - 1), image(7 - distance));
        QCOMPARE(movingLeft.at(distance - 1), image(7 - distance));
        QCOMPARE(movingLeft.at(5 + distance - 1), image(7 + distance));
    }
    const QList<QUrl> aroundEight = formats.nearbyImages(image(8), 5);
    QCOMPARE(aroundEight.size(), 10);
    QVERIFY(!aroundEight.contains(image(2)));
    QVERIFY(aroundEight.contains(image(13)));
    QCOMPARE(formats.adjacentImage(image(15), 1), image(0));
}

QTEST_GUILESS_MAIN(FormatNavigationTest)
#include "tst_format_navigation.moc"
