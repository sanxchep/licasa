#include "app/language_manager.h"
#include "imaging/format_support.h"

#include <QCoreApplication>
#include <QDir>
#include <QFont>
#include <QImage>
#include <QLocale>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using Licasa::LanguageManager;

class LanguageManagerTest final : public QObject {
    Q_OBJECT
  private slots:
    void systemLanguage_data()
    {
        QTest::addColumn<QStringList>("preferences");
        QTest::addColumn<QString>("expected");
        QTest::newRow("regional-swedish") << QStringList{"sv-SE"} << QString("sv");
        QTest::newRow("regional-underscore") << QStringList{"sv_FI"} << QString("sv");
        QTest::newRow("supported-fallback")
            << QStringList{"de-DE", "sv-SE", "en-US"} << QString("sv");
        QTest::newRow("english-precedes-swedish")
            << QStringList{"de-DE", "en-US", "sv-SE"} << QString("en");
        QTest::newRow("unsupported") << QStringList{"de-DE"} << QString("en");
        QTest::newRow("empty") << QStringList{} << QString("en");
    }

    void systemLanguage()
    {
        QFETCH(QStringList, preferences);
        QFETCH(QString, expected);
        QTemporaryDir directory;
        LanguageManager manager(directory.filePath("settings.ini"), ":/i18n", preferences);
        QCOMPARE(manager.selectedLanguage(), "system");
        QCOMPARE(manager.effectiveLanguage(), expected);
        QCOMPARE(manager.languages().size(), 3);
        QCOMPARE(manager.languages().at(1).toMap().value("text").toString(), "English");
        QCOMPARE(manager.languages().at(2).toMap().value("text").toString(),
                 QString::fromUtf8("Svenska"));
    }

    void explicitSelectionPersistsAndInvalidStateRecovers()
    {
        QTemporaryDir directory;
        const QString file = directory.filePath("settings.ini");
        {
            LanguageManager manager(file, ":/i18n", {"en-US"});
            QVERIFY(manager.selectLanguage("sv"));
            QCOMPARE(manager.selectedLanguage(), "sv");
            QCOMPARE(QSettings(file, QSettings::IniFormat).value("viewer/language").toString(),
                     "sv");
        }
        {
            LanguageManager restarted(file, ":/i18n", {"en-US"});
            QCOMPARE(restarted.effectiveLanguage(), "sv");
            QVERIFY(restarted.selectLanguage("system"));
            QCOMPARE(restarted.effectiveLanguage(), "en");
        }
        {
            QSettings settings(file, QSettings::IniFormat);
            settings.setValue("viewer/language", "../../untrusted-catalog");
            settings.sync();
        }
        LanguageManager recovered(file, ":/i18n", {"sv-SE"});
        QCOMPARE(recovered.selectedLanguage(), "system");
        QCOMPARE(recovered.effectiveLanguage(), "sv");
        QVERIFY(recovered.selectLanguage("system"));
        QCOMPARE(QSettings(file, QSettings::IniFormat).value("viewer/language").toString(),
                 "system");
    }

    void compiledCatalogSwitchesBothWindowsWithoutChangingTheirState()
    {
        QTemporaryDir directory;
        LanguageManager manager(directory.filePath("settings.ini"), ":/i18n", {"en-US"});
        QQmlEngine engine;
        connect(&manager, &LanguageManager::languageChanged, &engine, &QQmlEngine::retranslate);
        QQmlComponent component(&engine);
        component.setData("import QtQml\nQtObject { property string label: "
                          "qsTranslate(\"FullscreenSettingsContent\", \"Language\"); property int "
                          "editValue: 42 }",
                          QUrl());
        std::unique_ptr<QObject> first(component.create());
        std::unique_ptr<QObject> second(component.create());
        QVERIFY2(first && second, qPrintable(component.errorString()));
        QCOMPARE(first->property("label").toString(), "Language");
        first->setProperty("editValue", 73);
        QVERIFY(manager.selectLanguage("sv"));
        QCOMPARE(first->property("label").toString(), QString::fromUtf8("Språk"));
        QCOMPARE(second->property("label").toString(), QString::fromUtf8("Språk"));
        QCOMPARE(first->property("editValue").toInt(), 73);
        QCOMPARE(second->property("editValue").toInt(), 42);
        QVERIFY(manager.selectLanguage("en"));
        QCOMPARE(first->property("label").toString(), "Language");
        QCOMPARE(second->property("label").toString(), "Language");
        QCOMPARE(first->property("editValue").toInt(), 73);
    }

    void translatedFiltersAndDateDoNotChangeNumbersOrFormatKeys()
    {
        QTemporaryDir directory;
        LanguageManager manager(directory.filePath("settings.ini"), ":/i18n", {"en-US"});
        manager.setReleaseDate(QDate(2026, 11, 1));
        Licasa::FormatSupport formats;
        const QStringList english = formats.nameFilters();
        const QVariantList exportKeys = formats.exportFormats();
        const QLocale numericLocale;
        QSignalSpy changed(&formats, &Licasa::FormatSupport::filtersChanged);
        connect(&manager, &LanguageManager::languageChanged, &formats,
                &Licasa::FormatSupport::retranslate);
        QCOMPARE(manager.releaseDate(), "1 November 2026");
        QVERIFY(manager.selectLanguage("sv"));
        QCOMPARE(manager.releaseDate(), "1 november 2026");
        QVERIFY(formats.nameFilters() != english);
        QCOMPARE(formats.nameFilters().last(), QString::fromUtf8("Alla filer (*)"));
        QCOMPARE(formats.nameFilters().first().mid(formats.nameFilters().first().indexOf('(')),
                 english.first().mid(english.first().indexOf('(')));
        QCOMPARE(formats.exportFormats(), exportKeys);
        QCOMPARE(QLocale().name(), numericLocale.name());
        QCOMPARE(QLocale().decimalPoint(), numericLocale.decimalPoint());
        QCOMPARE(changed.count(), 1);
        QVERIFY(manager.selectLanguage("en"));
        QCOMPARE(formats.nameFilters(), english);
    }

    void failedLoadOrSaveRetainsTheAppliedLanguage()
    {
        QTemporaryDir directory;
        LanguageManager missing(directory.filePath("settings.ini"), directory.filePath("missing"),
                                {"en-US"});
        QVERIFY(!missing.selectLanguage("sv"));
        QCOMPARE(missing.selectedLanguage(), "system");
        QCOMPARE(missing.effectiveLanguage(), "en");
        QVERIFY(!missing.errorMessage().isEmpty());
        QVERIFY(!missing.selectLanguage("unavailable"));
        QVERIFY(missing.selectLanguage("en"));
        QVERIFY(missing.errorMessage().isEmpty());

        QVERIFY(QDir(directory.path()).mkdir("not-a-settings-file"));
        LanguageManager unwritable(directory.filePath("not-a-settings-file"), ":/i18n", {"en-US"});
        QVERIFY(!unwritable.selectLanguage("sv"));
        QCOMPARE(unwritable.selectedLanguage(), "system");
        QCOMPARE(unwritable.effectiveLanguage(), "en");
        QVERIFY(!unwritable.errorMessage().isEmpty());
    }

    void missingStartupCatalogReportsTheEnglishFallback()
    {
        QTemporaryDir directory;
        LanguageManager manager(directory.filePath("settings.ini"), directory.filePath("missing"),
                                {"sv-SE"});
        QCOMPARE(manager.selectedLanguage(), "en");
        QCOMPARE(manager.effectiveLanguage(), "en");
        QVERIFY(!manager.errorMessage().isEmpty());
    }

    void settingsLayoutAndOptionalSnapshots()
    {
        QTemporaryDir directory;
        LanguageManager manager(directory.filePath("settings.ini"), ":/i18n", {"en-US"});
        QQmlEngine engine;
        connect(&manager, &LanguageManager::languageChanged, &engine, &QQmlEngine::retranslate);
        const QString output = qEnvironmentVariable("LICASA_LOCALIZATION_SCREENSHOTS");
        if (!output.isEmpty()) {
            QVERIFY(QDir().mkpath(output));
        }
        for (int width : {640, 320}) {
            QQuickWindow window;
            window.resize(width, 600);
            window.setColor(QColor(QStringLiteral("#171717")));
            QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(
                                                 LICASA_SOURCE_DIR "/qml/FullscreenSideMenu.qml")));
            QVERIFY2(component.isReady(), qPrintable(component.errorString()));
            std::unique_ptr<QObject> object(component.createWithInitialProperties(
                {{"fullscreen", false},
                 {"smoothMotionEnabled", false},
                 {"width", width},
                 {"height", 600},
                 {"languageManager", QVariant::fromValue(static_cast<QObject*>(&manager))}}));
            auto* item = qobject_cast<QQuickItem*>(object.get());
            QVERIFY2(item, qPrintable(component.errorString()));
            item->setParentItem(window.contentItem());
            item->setProperty("panelOpen", true);
            window.show();
            QTRY_VERIFY(window.isExposed());
            auto* choice = item->findChild<QQuickItem*>("languageChoice");
            auto* title = item->findChild<QQuickItem*>("languageSettingTitle");
            auto* error = item->findChild<QQuickItem*>("languageSettingError");
            QVERIFY(choice && title && error);
            QVERIFY(!item->findChild<QQuickItem*>("languageSettingAnnotation"));
            for (const QString& language : {QStringLiteral("en"), QStringLiteral("sv")}) {
                QVERIFY(manager.selectLanguage(language));
                QTRY_COMPARE(choice->property("currentValue").toString(), language);
                QTRY_VERIFY(choice->y() >= title->y() + title->height());
                QVERIFY(!error->isVisible());
                QImage image = window.grabWindow();
                QVERIFY(!image.isNull());
                if (!output.isEmpty()) {
                    QVERIFY(image.save(
                        output + QStringLiteral("/settings-%1-%2.png").arg(language).arg(width)));
                }
            }
            if (width == 320) {
                QFont font = title->property("font").value<QFont>();
                font.setPixelSize(18);
                title->setProperty("font", font);
                QTRY_VERIFY(choice->y() >= title->y() + title->height());
                QImage image = window.grabWindow();
                QVERIFY(!image.isNull());
                if (!output.isEmpty()) {
                    QVERIFY(
                        image.save(output + QStringLiteral("/settings-sv-320-larger-title.png")));
                }
            }
        }
    }
};

QTEST_MAIN(LanguageManagerTest)
#include "tst_language_manager.moc"
