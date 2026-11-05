#include "app/application_settings.h"
#include "platform/application_identity.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <array>
#include <memory>

namespace {
constexpr std::array<const char*, 9> environmentNames{
    "HOME",           "XDG_CONFIG_HOME",  "SNAP_NAME",     "SNAP_INSTANCE_NAME",
    "SNAP_USER_DATA", "SNAP_USER_COMMON", "SNAP_REVISION", "APPIMAGE",
    "APPDIR"};

QString preferenceKey()
{
    return QStringLiteral("background/installations/") +
           Licasa::applicationIdentity().installationId + QStringLiteral("/enabled");
}

QString entryPath()
{
    if (qEnvironmentVariableIsSet("SNAP_NAME")) {
        return QDir(qEnvironmentVariable("SNAP_USER_DATA"))
            .filePath(QStringLiteral(".config/autostart/licasa-background.desktop"));
    }
    return QDir(qEnvironmentVariable("XDG_CONFIG_HOME"))
        .filePath(QStringLiteral("autostart/licasa-background-") +
                  Licasa::applicationIdentity().installationId + QStringLiteral(".desktop"));
}

QString quoteExec(QString value)
{
    value.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    value.replace(QStringLiteral("\""), QStringLiteral("\\\""));
    value.replace(QStringLiteral("`"), QStringLiteral("\\`"));
    value.replace(QStringLiteral("$"), QStringLiteral("\\$"));
    value.replace(QStringLiteral("%"), QStringLiteral("%%"));
    return QStringLiteral("\"") + value + QStringLiteral("\"");
}

QByteArray legacyEntry(bool marked)
{
    QByteArray entry("[Desktop Entry]\nType=Application\nName=Licasa\n"
                     "Comment=Start Licasa in background at login\n");
    entry +=
        "Exec=" + quoteExec(QCoreApplication::applicationFilePath()).toUtf8() + " --background\n";
    if (marked) {
        entry += "X-Licasa-Background=1\n";
    }
    return entry;
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
} // namespace

class BackgroundModeTests final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase()
    {
        previousOrganization_ = QCoreApplication::organizationName();
        previousApplication_ = QCoreApplication::applicationName();
        QCoreApplication::setOrganizationName(QStringLiteral("LicasaBackgroundTests"));
        QCoreApplication::setApplicationName(QStringLiteral("LicasaBackgroundTests"));
    }

    void cleanupTestCase()
    {
        QCoreApplication::setOrganizationName(previousOrganization_);
        QCoreApplication::setApplicationName(previousApplication_);
    }

    void init()
    {
        for (std::size_t index = 0; index < environmentNames.size(); ++index) {
            savedPresent_[index] = qEnvironmentVariableIsSet(environmentNames[index]);
            savedEnvironment_[index] = qgetenv(environmentNames[index]);
            qunsetenv(environmentNames[index]);
        }
        home_ = std::make_unique<QTemporaryDir>();
        QVERIFY(home_->isValid());
        qputenv("HOME", home_->path().toUtf8());
        qputenv("XDG_CONFIG_HOME", home_->filePath(QStringLiteral("config")).toUtf8());
        // QSettings caches its default search path. Reset it explicitly when
        // changing XDG roots within one process rather than sharing consent.
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                           home_->filePath(QStringLiteral("config")));
        QSettings::setPath(QSettings::NativeFormat, QSettings::SystemScope,
                           home_->filePath(QStringLiteral("system-config")));
    }

    void cleanup()
    {
        for (std::size_t index = 0; index < environmentNames.size(); ++index) {
            if (savedPresent_[index]) {
                qputenv(environmentNames[index], savedEnvironment_[index]);
            } else {
                qunsetenv(environmentNames[index]);
            }
        }
        home_.reset();
    }

    void freshNativeInstallIsOffUntilEnabled()
    {
        Licasa::BackgroundModeManager manager;
        QSignalSpy changed(&manager, &Licasa::BackgroundModeManager::enabledChanged);
        const QString path = entryPath();
        manager.syncAutostart();
        QVERIFY(!manager.enabled());
        QVERIFY(!QFileInfo::exists(path));
        QCOMPARE(QSettings().value(preferenceKey()).toBool(), false);

        manager.setEnabled(true);
        QVERIFY(manager.enabled());
        QCOMPARE(changed.size(), 1);
        const QByteArray content = readFile(path);
        QVERIFY(content.contains("X-Licasa-Background=1\n"));
        QVERIFY(content.contains("X-Licasa-Schema=1\n"));
        QVERIFY(content.contains("X-Licasa-Installation=" +
                                 Licasa::applicationIdentity().installationId.toUtf8() + '\n'));
        QVERIFY(content.contains("Exec=/usr/bin/env " +
                                 quoteExec(QCoreApplication::applicationFilePath()).toUtf8() +
                                 " --background\n"));
        manager.syncAutostart();
        QCOMPARE(readFile(path), content);
        manager.setEnabled(false);
        QVERIFY(!manager.enabled());
        QVERIFY(!QFileInfo::exists(path));
        QCOMPARE(changed.size(), 2);
    }

    void unaskedLegacyEntryOnlyRetiredForExactInstallation()
    {
        const QString path = home_->filePath(QStringLiteral("config/autostart/licasa.desktop"));
        QVERIFY(writeFile(path, legacyEntry(false)));
        Licasa::BackgroundModeManager manager;
        manager.syncAutostart();
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!manager.enabled());

        const QByteArray foreign("[Desktop Entry]\nName=Licasa\n"
                                 "Comment=Start Licasa in background at login\n"
                                 "Exec=/usr/bin/licasa --background\n");
        QVERIFY(writeFile(path, foreign));
        manager.syncAutostart();
        QCOMPARE(readFile(path), foreign);
    }

    void unaskedLegacyEntryWithForeignMetadataIsPreserved_data()
    {
        QTest::addColumn<QByteArray>("suffix");
        QTest::newRow("foreign-owner") << QByteArray("X-Licasa-Installation=another-install\n");
        QTest::newRow("unsupported-schema") << QByteArray("X-Licasa-Schema=99\n");
        QTest::newRow("duplicate-exec") << QByteArray("Exec=/bin/true\n");
        QTest::newRow("oversized-entry") << QByteArray(4096, '#');
    }

    void unaskedLegacyEntryWithForeignMetadataIsPreserved()
    {
        QFETCH(QByteArray, suffix);
        const QString path = home_->filePath(QStringLiteral("config/autostart/licasa.desktop"));
        const QByteArray content = legacyEntry(false) + suffix;
        QVERIFY(writeFile(path, content));

        Licasa::BackgroundModeManager manager;
        manager.syncAutostart();
        QVERIFY(!manager.enabled());
        QCOMPARE(readFile(path), content);
    }

    void legacyMarkedOptInMigratesToScopedEntry()
    {
        const QString path =
            home_->filePath(QStringLiteral("config/autostart/licasa-background.desktop"));
        QVERIFY(writeFile(path, legacyEntry(true)));
        Licasa::BackgroundModeManager manager;
        QVERIFY(manager.enabled());
        manager.syncAutostart();
        QVERIFY(manager.enabled());
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(QFileInfo::exists(entryPath()));
        QCOMPARE(QSettings().value(preferenceKey()).toBool(), true);
    }

    void explicitDisabledPreferenceWinsOverLegacyOptIn()
    {
        const QString path =
            home_->filePath(QStringLiteral("config/autostart/licasa-background.desktop"));
        QVERIFY(writeFile(path, legacyEntry(true)));
        QSettings settings;
        settings.setValue(preferenceKey(), false);
        settings.sync();
        Licasa::BackgroundModeManager manager;
        manager.syncAutostart();
        QVERIFY(!manager.enabled());
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!QFileInfo::exists(entryPath()));
    }

    void snapPreferenceAndIdentitySurviveRevisionChange()
    {
        qputenv("SNAP_USER_COMMON", home_->filePath(QStringLiteral("snap/common")).toUtf8());
        qputenv("SNAP_NAME", "licasa");
        qputenv("SNAP_INSTANCE_NAME", "licasa_review");
        qputenv("SNAP_REVISION", "100");
        qputenv("SNAP_USER_DATA", home_->filePath(QStringLiteral("snap/100")).toUtf8());
        const QString id = Licasa::applicationIdentity().installationId;
        Licasa::BackgroundModeManager manager;
        manager.syncAutostart();
        QVERIFY(!manager.enabled());
        manager.setEnabled(true);
        const QByteArray first = readFile(entryPath());
        QVERIFY(first.contains("Exec=licasa --background\n"));

        qputenv("SNAP_REVISION", "200");
        qputenv("SNAP_USER_DATA", home_->filePath(QStringLiteral("snap/200")).toUtf8());
        QCOMPARE(Licasa::applicationIdentity().installationId, id);
        Licasa::BackgroundModeManager replacement;
        replacement.syncAutostart();
        QVERIFY(replacement.enabled());
        QCOMPARE(readFile(entryPath()), first);
        replacement.setEnabled(false);
        QVERIFY(!replacement.enabled());
        QVERIFY(!QFileInfo::exists(entryPath()));
        qputenv("SNAP_REVISION", "100");
        qputenv("SNAP_USER_DATA", home_->filePath(QStringLiteral("snap/100")).toUtf8());
        Licasa::BackgroundModeManager reverted;
        reverted.syncAutostart();
        QVERIFY(!reverted.enabled());
        QVERIFY(!QFileInfo::exists(entryPath()));
    }

    void coinstalledPortableCopiesKeepSeparateConsent()
    {
        const QString firstImage = home_->filePath(QStringLiteral("first.AppImage"));
        const QString secondImage = home_->filePath(QStringLiteral("second.AppImage"));
        QVERIFY(writeFile(firstImage, "first"));
        QVERIFY(writeFile(secondImage, "second"));
        qputenv("APPIMAGE", firstImage.toUtf8());
        const QString firstId = Licasa::applicationIdentity().installationId;
        const QString firstEntry = entryPath();
        {
            Licasa::BackgroundModeManager first;
            first.setEnabled(true);
            QVERIFY(first.enabled());
        }
        qputenv("APPIMAGE", secondImage.toUtf8());
        QVERIFY(Licasa::applicationIdentity().installationId != firstId);
        const QString secondEntry = entryPath();
        {
            Licasa::BackgroundModeManager second;
            QVERIFY(!second.enabled());
            second.setEnabled(true);
        }
        qputenv("APPIMAGE", firstImage.toUtf8());
        {
            Licasa::BackgroundModeManager first;
            first.setEnabled(false);
            QVERIFY(!first.enabled());
        }
        QVERIFY(!QFileInfo::exists(firstEntry));
        QVERIFY(QFileInfo::exists(secondEntry));
        qputenv("APPIMAGE", secondImage.toUtf8());
        Licasa::BackgroundModeManager second;
        QVERIFY(second.enabled());
    }

    void extractedAppImageUsesQuotedStableAppRun()
    {
        const QString gio = QStandardPaths::findExecutable(QStringLiteral("gio"));
        if (gio.isEmpty()) {
            QSKIP("GIO desktop launcher is required for the real startup parser check");
        }
        const QString appDir = home_->filePath(QStringLiteral("installed space $`\"%\\ image"));
        const QString appRun = QDir(appDir).filePath(QStringLiteral("AppRun"));
        // GIO can return before AppRun finishes. Publish the complete capture in one rename.
        QVERIFY(writeFile(appRun, "#!/bin/sh\n"
                                  "set -e\n"
                                  "capture=$LICASA_BACKGROUND_CAPTURE\n"
                                  "printf '%s\\0' \"$0\" \"$@\" > \"$capture.tmp\"\n"
                                  "mv -- \"$capture.tmp\" \"$capture\"\n"));
        QVERIFY(QFile::setPermissions(appRun, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                  QFileDevice::ExeOwner));
        qputenv("APPDIR", appDir.toUtf8());
        const QString legacy =
            home_->filePath(QStringLiteral("config/autostart/licasa-background.desktop"));
        const QByteArray oldEntry = "[Desktop Entry]\nType=Application\nName=Licasa\n"
                                    "Comment=Start Licasa in background at login\nExec=" +
                                    quoteExec(appRun).toUtf8() +
                                    " --background\nX-Licasa-Background=1\n";
        QVERIFY(writeFile(legacy, oldEntry));
        Licasa::BackgroundModeManager manager;
        QVERIFY(manager.enabled());
        manager.syncAutostart();
        QVERIFY(manager.enabled());
        QVERIFY(!QFileInfo::exists(legacy));
        const QString capture = home_->filePath(QStringLiteral("captured-startup-argv"));
        QProcess launcher;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("LICASA_BACKGROUND_CAPTURE"), capture);
        launcher.setProcessEnvironment(environment);
        launcher.start(gio, {QStringLiteral("launch"), entryPath()});
        QVERIFY(launcher.waitForFinished(5000));
        QCOMPARE(launcher.exitStatus(), QProcess::NormalExit);
        QVERIFY2(launcher.exitCode() == 0, launcher.readAllStandardError().constData());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(capture), 5000);
        QByteArray expected = QFile::encodeName(appRun);
        expected.append('\0');
        expected.append("--background");
        expected.append('\0');
        QCOMPARE(readFile(capture), expected);
        manager.setEnabled(false);
        QVERIFY(!manager.enabled());
        QVERIFY(!QFileInfo::exists(entryPath()));
    }

    void newlineLaunchPathIsRejected()
    {
        const QString image = home_->filePath(QStringLiteral("image\nHidden=true.AppImage"));
        QVERIFY(writeFile(image, "test"));
        qputenv("APPIMAGE", image.toUtf8());
        Licasa::BackgroundModeManager manager;
        QSignalSpy errors(&manager, &Licasa::BackgroundModeManager::errorOccurred);
        manager.setEnabled(true);
        QVERIFY(!manager.enabled());
        QVERIFY(!QFileInfo::exists(entryPath()));
        QCOMPARE(errors.size(), 1);
    }

    void foreignScopedEntryAndGenericEntryArePreserved()
    {
        const QByteArray foreign("[Desktop Entry]\nName=Other program\nExec=/bin/true\n");
        const QString generic = home_->filePath(QStringLiteral("config/autostart/licasa.desktop"));
        QVERIFY(writeFile(generic, foreign));
        const QString scoped = entryPath();
        QVERIFY(writeFile(scoped, foreign));
        Licasa::BackgroundModeManager manager;
        QSignalSpy errors(&manager, &Licasa::BackgroundModeManager::errorOccurred);
        manager.syncAutostart();
        manager.setEnabled(true);
        QVERIFY(!manager.enabled());
        QCOMPARE(readFile(scoped), foreign);
        QCOMPARE(readFile(generic), foreign);
        QCOMPARE(errors.size(), 1);
    }

    void symlinkedEntryIsNeverReplaced()
    {
        const QString target = home_->filePath(QStringLiteral("foreign.desktop"));
        const QByteArray content("[Desktop Entry]\nName=Other program\n");
        QVERIFY(writeFile(target, content));
        QVERIFY(QDir().mkpath(QFileInfo(entryPath()).absolutePath()));
        QVERIFY(QFile::link(target, entryPath()));
        Licasa::BackgroundModeManager manager;
        QSignalSpy errors(&manager, &Licasa::BackgroundModeManager::errorOccurred);
        manager.setEnabled(true);
        QVERIFY(!manager.enabled());
        QVERIFY(QFileInfo(entryPath()).isSymLink());
        QCOMPARE(readFile(target), content);
        QCOMPARE(errors.size(), 1);
    }

    void disabledLegacyOverrideIsNotAdopted()
    {
        const QString path =
            home_->filePath(QStringLiteral("config/autostart/licasa-background.desktop"));
        const QByteArray content = legacyEntry(true) + "Hidden=true\n";
        QVERIFY(writeFile(path, content));
        Licasa::BackgroundModeManager manager;
        manager.syncAutostart();
        QVERIFY(!manager.enabled());
        QCOMPARE(readFile(path), content);
        QVERIFY(!QFileInfo::exists(entryPath()));
    }

    void explicitDisablePreservesUserOverrideAndConsentStaysOff()
    {
        Licasa::BackgroundModeManager manager;
        manager.setEnabled(true);
        QVERIFY(manager.enabled());
        const QByteArray override = readFile(entryPath()) + "X-GNOME-Autostart-enabled=false\n";
        QVERIFY(writeFile(entryPath(), override));
        manager.setEnabled(false);
        QVERIFY(!manager.enabled());
        QCOMPARE(readFile(entryPath()), override);
        manager.syncAutostart();
        QVERIFY(!manager.enabled());
        QCOMPARE(readFile(entryPath()), override);
    }

    void ambiguousOwnershipIsRejected_data()
    {
        QTest::addColumn<QByteArray>("suffix");
        QTest::newRow("duplicate-exec") << QByteArray("Exec=/bin/true\n");
        QTest::newRow("duplicate-owner") << QByteArray("X-Licasa-Installation=another-install\n");
        QTest::newRow("unsupported-schema") << QByteArray("X-Licasa-Schema=99\n");
    }

    void ambiguousOwnershipIsRejected()
    {
        QFETCH(QByteArray, suffix);
        const QByteArray content = legacyEntry(true) + suffix;
        QVERIFY(writeFile(entryPath(), content));
        Licasa::BackgroundModeManager manager;
        QSignalSpy errors(&manager, &Licasa::BackgroundModeManager::errorOccurred);
        manager.setEnabled(true);
        QVERIFY(!manager.enabled());
        QCOMPARE(readFile(entryPath()), content);
        QCOMPARE(errors.size(), 1);
    }

  private:
    std::unique_ptr<QTemporaryDir> home_;
    QString previousOrganization_;
    QString previousApplication_;
    std::array<QByteArray, environmentNames.size()> savedEnvironment_;
    std::array<bool, environmentNames.size()> savedPresent_{};
};

QTEST_GUILESS_MAIN(BackgroundModeTests)
#include "tst_background_mode.moc"
