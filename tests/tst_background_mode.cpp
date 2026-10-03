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
    void snapServiceStatusTracksSnapctl();
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

void BackgroundModeTests::snapServiceStatusTracksSnapctl()
{
#if defined(Q_OS_UNIX)
    QTemporaryDir tools;
    QVERIFY(tools.isValid());
    const QString statusPath = tools.filePath(QStringLiteral("service-status"));
    QFile command(tools.filePath(QStringLiteral("snapctl")));
    QVERIFY(command.open(QIODevice::WriteOnly | QIODevice::Text));
    const QByteArray script = QByteArray("#!/bin/sh\ncat '") + statusPath.toUtf8() + "'\n";
    QCOMPARE(command.write(script), qint64(script.size()));
    command.close();
    QVERIFY(command.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    const QByteArray originalPath = qgetenv("PATH");
    qputenv("PATH", tools.path().toUtf8() + ':' + originalPath);
    qputenv("SNAP_NAME", "licasa");

    auto writeStatus = [&statusPath](const QByteArray& output) {
        QFile status(statusPath);
        if (!status.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        return status.write(output) == output.size();
    };

    Licasa::BackgroundModeManager manager;
    QVERIFY(manager.snapService());
    QVERIFY(writeStatus("Service Startup Current Notes\nlicasa.background enabled active user\n"));
    manager.refreshSnapServiceStatus();
    QTRY_COMPARE(manager.snapServiceStatus(), QStringLiteral("active"));

    QVERIFY(
        writeStatus("Service Startup Current Notes\nlicasa.background enabled inactive user\n"));
    manager.refreshSnapServiceStatus();
    QTRY_COMPARE(manager.snapServiceStatus(), QStringLiteral("inactive"));

    QVERIFY(writeStatus("Service Startup Current Notes\nother.background enabled active user\n"));
    manager.refreshSnapServiceStatus();
    QTRY_COMPARE(manager.snapServiceStatus(), QStringLiteral("unavailable"));
    qputenv("PATH", originalPath);
#else
    QSKIP("Snap services are only available on Unix");
#endif
}

QTEST_GUILESS_MAIN(BackgroundModeTests)
#include "tst_background_mode.moc"
