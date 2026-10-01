#include "app/application_settings.h"
#include "io/external_file_identity.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class BackgroundModeTests final : public QObject {
    Q_OBJECT

  private slots:
    void nativeLoginEntryIsEnabledByDefault();
    void snapRemovesOnlyItsLegacyLoginEntry();
    void snapLeavesOversizedLoginEntryUntouched();
};

void BackgroundModeTests::nativeLoginEntryIsEnabledByDefault()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    qputenv("XDG_CONFIG_HOME", home.path().toUtf8());
    qunsetenv("SNAP_NAME");

    Licasa::BackgroundModeManager manager;
    QVERIFY(manager.enabled());
    manager.syncAutostart();

    QFile entry(home.filePath(QStringLiteral("autostart/licasa.desktop")));
    QVERIFY(entry.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray content = entry.readAll();
    QVERIFY(content.contains("--background"));
    QVERIFY(content.contains("Exec="));
#if defined(Q_OS_UNIX)
    const auto before = Licasa::externalFileIdentity(entry);
    QVERIFY(before.has_value());
    entry.close();
    manager.syncAutostart();
    QVERIFY(entry.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto after = Licasa::externalFileIdentity(entry);
    QVERIFY(after.has_value());
    QCOMPARE(after->device, before->device);
    QCOMPARE(after->inode, before->inode);
#endif
}

void BackgroundModeTests::snapRemovesOnlyItsLegacyLoginEntry()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    qputenv("XDG_CONFIG_HOME", home.path().toUtf8());
    qputenv("SNAP_NAME", "licasa");
    QVERIFY(QDir().mkpath(home.filePath(QStringLiteral("autostart"))));
    QFile entry(home.filePath(QStringLiteral("autostart/licasa.desktop")));
    QVERIFY(entry.open(QIODevice::WriteOnly | QIODevice::Text));
    QVERIFY(entry.write("[Desktop Entry]\nComment=Start Licasa in background at login\n") > 0);
    entry.close();

    Licasa::BackgroundModeManager manager;
    manager.syncAutostart();
    QVERIFY(!entry.exists());

    QVERIFY(entry.open(QIODevice::WriteOnly | QIODevice::Text));
    QVERIFY(entry.write("[Desktop Entry]\nComment=Other launcher\n") > 0);
    entry.close();
    manager.syncAutostart();
    QVERIFY(entry.exists());
}

void BackgroundModeTests::snapLeavesOversizedLoginEntryUntouched()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    qputenv("XDG_CONFIG_HOME", home.path().toUtf8());
    qputenv("SNAP_NAME", "licasa");
    QVERIFY(QDir().mkpath(home.filePath(QStringLiteral("autostart"))));
    QFile entry(home.filePath(QStringLiteral("autostart/licasa.desktop")));
    QVERIFY(entry.open(QIODevice::WriteOnly | QIODevice::Text));
    const QByteArray content =
        QByteArray("Comment=Start Licasa in background at login\n") + QByteArray(4096, 'x');
    QCOMPARE(entry.write(content), qint64(content.size()));
    entry.close();

    Licasa::BackgroundModeManager manager;
    manager.syncAutostart();
    QVERIFY(entry.exists());
}

QTEST_GUILESS_MAIN(BackgroundModeTests)
#include "tst_background_mode.moc"
