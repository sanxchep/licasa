#include "imaging/format_support.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

class NavigationRuntimeTest final : public QObject {
    Q_OBJECT

  private slots:
    void preservesLocaleFilenameOrder_data()
    {
        QTest::addColumn<QString>("localeName");
        QTest::newRow("C") << QStringLiteral("C");
        QTest::newRow("German") << QStringLiteral("de_DE");
        QTest::newRow("English") << QStringLiteral("en_US");
        QTest::newRow("Swedish") << QStringLiteral("sv_SE");
    }

    void preservesLocaleFilenameOrder()
    {
        QFETCH(QString, localeName);
        const QLocale previous;
        QLocale::setDefault(QLocale(localeName));
        struct RestoreLocale {
            QLocale previous;
            ~RestoreLocale() { QLocale::setDefault(previous); }
        } restore{previous};

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QStringList names;
        const QStringList prefixes = {QStringLiteral("a"),    QStringLiteral("A"),
                                      QString::fromUtf8("ä"), QString::fromUtf8("å"),
                                      QString::fromUtf8("é"), QString::fromUtf8("é"),
                                      QStringLiteral("z"),    QString::fromUtf8("中文")};
        for (int index = 0; index < 160; ++index) {
            const QString name = prefixes.at(index % prefixes.size()) +
                                 QString::number(160 - index) + QStringLiteral(".png");
            QFile image(directory.filePath(name));
            QVERIFY(image.open(QIODevice::WriteOnly));
            names.append(name);
        }
        std::sort(names.begin(), names.end(), [](const QString& left, const QString& right) {
            const int comparison = left.localeAwareCompare(right);
            return comparison < 0 || (comparison == 0 && left < right);
        });

        Licasa::FormatSupport formats;
        QUrl current = QUrl::fromLocalFile(directory.filePath(names.last()));
        for (const QString& expected : std::as_const(names)) {
            current = formats.adjacentImage(current, 1);
            QCOMPARE(current, QUrl::fromLocalFile(directory.filePath(expected)));
        }
    }

    void keepsReadableFileAdmissionAndDirectoryInvalidation()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        for (const QString& name :
             {QStringLiteral("a.png"), QStringLiteral("b.txt"), QStringLiteral("c.PNG"),
              QStringLiteral("d.jpg"), QStringLiteral("unreadable.png")}) {
            QFile file(directory.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("directory.png")));
        QVERIFY(QFile::link(directory.filePath(QStringLiteral("c.PNG")),
                            directory.filePath(QStringLiteral("linked.png"))));
        QVERIFY(QFile::setPermissions(directory.filePath(QStringLiteral("unreadable.png")), {}));
        QFile unreadable(directory.filePath(QStringLiteral("unreadable.png")));
        QVERIFY2(!unreadable.open(QIODevice::ReadOnly),
                 "The unreadable fixture requires desktop credentials.");

        const auto expectedNames = [&directory] {
            QStringList names =
                QDir(directory.path())
                    .entryList(QDir::Files | QDir::Readable | QDir::NoDotAndDotDot, QDir::NoSort);
            names.removeIf([](const QString& name) {
                const QString suffix = QFileInfo(name).suffix().toLower();
                return suffix != QStringLiteral("png") && suffix != QStringLiteral("jpg");
            });
            std::sort(names.begin(), names.end(), [](const QString& left, const QString& right) {
                const int comparison = left.localeAwareCompare(right);
                return comparison < 0 || (comparison == 0 && left < right);
            });
            return names;
        };
        Licasa::FormatSupport formats;
        const QUrl first = QUrl::fromLocalFile(directory.filePath(QStringLiteral("a.png")));
        const auto compareNeighbors = [&] {
            QList<QUrl> expected;
            const QStringList names = expectedNames();
            const int firstIndex = names.indexOf(QStringLiteral("a.png"));
            for (int step = 1; step < names.size(); ++step) {
                expected.append(QUrl::fromLocalFile(
                    directory.filePath(names.at((firstIndex + step) % names.size()))));
            }
            QCOMPARE(formats.nearbyImages(first, 100, 1), expected);
        };
        compareNeighbors();
        QTest::qWait(5);
        QFile added(directory.filePath(QStringLiteral("added.png")));
        QVERIFY(added.open(QIODevice::WriteOnly));
        added.close();
        compareNeighbors();
        QTest::qWait(5);
        QVERIFY(QFile::remove(directory.filePath(QStringLiteral("c.PNG"))));
        compareNeighbors();
    }
};

QTEST_GUILESS_MAIN(NavigationRuntimeTest)
#include "tst_navigation_runtime.moc"
