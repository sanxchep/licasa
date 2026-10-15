#include "app/application_settings.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class BackgroundModeTests final : public QObject {
    Q_OBJECT

  private slots:
    void freshNativeInstallIsOffUntilEnabled();
    void legacyNativeAutostartIsRetired();
    void snapUsesOptInSessionAutostart();
    void extractedAppImageUsesAppRun();
    void unrelatedAutostartIsPreserved();
};

void BackgroundModeTests::freshNativeInstallIsOffUntilEnabled()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    qputenv("XDG_CONFIG_HOME", home.path().toUtf8());
    qunsetenv("SNAP_NAME");
    qunsetenv("APPIMAGE");
    qunsetenv("APPDIR");

    Licasa::BackgroundModeManager manager;
    QSignalSpy changed(&manager, &Licasa::BackgroundModeManager::enabledChanged);
    QFile entry(home.filePath(QStringLiteral("autostart/licasa-background.desktop")));
    manager.syncAutostart();
    QVERIFY(!manager.enabled());
    QVERIFY(!entry.exists());

    manager.setEnabled(true);
    QVERIFY(manager.enabled());
    QCOMPARE(changed.size(), 1);
    QVERIFY(entry.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray content = entry.readAll();
    QVERIFY(content.contains("X-Licasa-Background=1\n"));
    QVERIFY(content.contains(" --background\n"));
    entry.close();

    manager.syncAutostart();
    QVERIFY(manager.enabled());
    manager.setEnabled(false);
    QVERIFY(!manager.enabled());
    QVERIFY(!entry.exists());
    QCOMPARE(changed.size(), 2);
}

void BackgroundModeTests::legacyNativeAutostartIsRetired()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    qputenv("XDG_CONFIG_HOME", home.path().toUtf8());
    qunsetenv("SNAP_NAME");
    QVERIFY(QDir().mkpath(home.filePath(QStringLiteral("autostart"))));
    QFile entry(home.filePath(QStringLiteral("autostart/licasa.desktop")));
    QVERIFY(entry.open(QIODevice::WriteOnly | QIODevice::Text));
    QVERIFY(entry.write("[Desktop Entry]\nName=Licasa\n"
                        "Comment=Start Licasa in background at login\n"
                        "Exec=/usr/bin/licasa --background\n") > 0);
    entry.close();

    Licasa::BackgroundModeManager manager;
    manager.syncAutostart();
    QVERIFY(!entry.exists());
    QVERIFY(!manager.enabled());
}

void BackgroundModeTests::snapUsesOptInSessionAutostart()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    qputenv("SNAP_NAME", "licasa");
    qputenv("SNAP_USER_DATA", home.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", home.filePath(QStringLiteral("other-config")).toUtf8());

    Licasa::BackgroundModeManager manager;
    const QString path =
        home.filePath(QStringLiteral(".config/autostart/licasa-background.desktop"));
    manager.syncAutostart();
    QVERIFY(!manager.enabled());
    QVERIFY(!QFileInfo::exists(path));

    manager.setEnabled(true);
    QVERIFY(manager.enabled());
    QFile entry(path);
    QVERIFY(entry.open(QIODevice::ReadOnly | QIODevice::Text));
    QVERIFY(entry.readAll().contains("Exec=licasa --background\n"));
    entry.close();

    manager.setEnabled(false);
    QVERIFY(!manager.enabled());
    QVERIFY(!entry.exists());
}

void BackgroundModeTests::extractedAppImageUsesAppRun()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    qunsetenv("SNAP_NAME");
    qunsetenv("APPIMAGE");
    qputenv("XDG_CONFIG_HOME", home.filePath(QStringLiteral("config")).toUtf8());
    const QString appDir = home.filePath(QStringLiteral("installed-appimage"));
    QVERIFY(QDir().mkpath(appDir));
    QFile appRun(QDir(appDir).filePath(QStringLiteral("AppRun")));
    QVERIFY(appRun.open(QIODevice::WriteOnly));
    appRun.close();
    qputenv("APPDIR", appDir.toUtf8());

    Licasa::BackgroundModeManager manager;
    manager.setEnabled(true);
    QFile entry(home.filePath(QStringLiteral("config/autostart/licasa-background.desktop")));
    QVERIFY(entry.open(QIODevice::ReadOnly | QIODevice::Text));
    QVERIFY(entry.readAll().contains(
        (QStringLiteral("Exec=\"") + appRun.fileName() + QStringLiteral("\" --background\n"))
            .toUtf8()));
    qunsetenv("APPDIR");
}

void BackgroundModeTests::unrelatedAutostartIsPreserved()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    qputenv("XDG_CONFIG_HOME", home.path().toUtf8());
    qunsetenv("SNAP_NAME");
    QVERIFY(QDir().mkpath(home.filePath(QStringLiteral("autostart"))));
    QFile entry(home.filePath(QStringLiteral("autostart/licasa.desktop")));
    QVERIFY(entry.open(QIODevice::WriteOnly | QIODevice::Text));
    QVERIFY(entry.write("[Desktop Entry]\nName=Other program\n") > 0);
    entry.close();

    Licasa::BackgroundModeManager manager;
    manager.syncAutostart();
    QVERIFY(entry.exists());
    manager.setEnabled(false);
    QVERIFY(entry.exists());

    QFile reserved(home.filePath(QStringLiteral("autostart/licasa-background.desktop")));
    QVERIFY(reserved.open(QIODevice::WriteOnly | QIODevice::Text));
    QVERIFY(reserved.write("[Desktop Entry]\nName=Other program\n") > 0);
    reserved.close();
    QSignalSpy errors(&manager, &Licasa::BackgroundModeManager::errorOccurred);
    manager.setEnabled(true);
    QVERIFY(!manager.enabled());
    QVERIFY(reserved.exists());
    QCOMPARE(errors.size(), 1);
}

QTEST_GUILESS_MAIN(BackgroundModeTests)
#include "tst_background_mode.moc"
