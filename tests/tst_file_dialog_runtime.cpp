#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

// Use the production wrappers and Qt's actual fallback picker. The negative
// run has its own process and import tree, so global QML plugin caches cannot
// turn a missing-module test into a false positive.
class FileDialogRuntimeTest final : public QObject {
    Q_OBJECT
  private slots:
    void dialogs_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QString>("typedSuffix");
        const QDir directory(QStringLiteral(LICASA_SOURCE_DIR "/qml"));
        int wrappers = 0;
        for (const auto& name : directory.entryList({QStringLiteral("*.qml")}, QDir::Files)) {
            QFile source(directory.filePath(name));
            QVERIFY(source.open(QIODevice::ReadOnly));
            // Discovery only: each row below instantiates the production type
            // and invokes its actual recovery API. New wrappers participate
            // automatically, and unknown required properties fail creation.
            if (!QRegularExpression(QStringLiteral(R"(\bFileDialog\s*\{)"))
                     .match(QString::fromUtf8(source.readAll()))
                     .hasMatch()) {
                continue;
            }
            const QString base = QFileInfo(name).completeBaseName();
            QTest::newRow(qPrintable(base)) << base << QString();
            ++wrappers;
        }
        QVERIFY2(wrappers > 0, "No production FileDialog wrappers were discovered");
        if (!qEnvironmentVariableIsSet("LICASA_TEST_MISSING_DIALOG_MODULE")) {
            QTest::newRow("typed-frame")
                << QStringLiteral("FrameSaveDialog") << QStringLiteral("png");
            QTest::newRow("typed-motion")
                << QStringLiteral("MotionExportDialog") << QStringLiteral("mov");
            QTest::newRow("typed-animation")
                << QStringLiteral("AnimationExportDialog") << QStringLiteral("apng");
        }
    }

    void dialogs()
    {
        QFETCH(QString, file);
        QFETCH(QString, typedSuffix);
        QQmlEngine engine;
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        const bool missing = qEnvironmentVariableIsSet("LICASA_TEST_MISSING_DIALOG_MODULE");
        if (missing) {
            // Preserve the real QtQuick/QtQml imports and all labs modules
            // except FolderListModel. No standard/host import path remains.
            const QString qtRoot = QString::fromLocal8Bit(LICASA_TEST_QML_ROOT);
            QVERIFY(QDir(qtRoot).exists());
            const QDir source(qtRoot);
            QDir target(isolated.path());
            for (const auto& entry : source.entryList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
                if (entry != QStringLiteral("Qt")) {
                    QVERIFY(QFile::link(source.filePath(entry), target.filePath(entry)));
                }
            }
            QVERIFY(target.mkpath(QStringLiteral("Qt/labs")));
            const QDir qt(source.filePath(QStringLiteral("Qt")));
            for (const auto& entry : qt.entryList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
                if (entry != QStringLiteral("labs")) {
                    QVERIFY(QFile::link(qt.filePath(entry), target.filePath("Qt/" + entry)));
                }
            }
            const QDir labs(source.filePath(QStringLiteral("Qt/labs")));
            for (const auto& entry : labs.entryList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
                if (entry != QStringLiteral("folderlistmodel")) {
                    QVERIFY(QFile::link(labs.filePath(entry), target.filePath("Qt/labs/" + entry)));
                }
            }
            engine.setImportPathList(
                {isolated.path(), QStringLiteral("qrc:/qt-project.org/imports")});
        }
        QQuickWindow window;
        window.resize(800, 600);
        window.show();
        QQmlComponent component(&engine,
                                QUrl::fromLocalFile(QStringLiteral(LICASA_SOURCE_DIR "/qml/") +
                                                    file + QStringLiteral(".qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        const QVariantMap formats{{"nameFilters", QStringList{"Images (*.png)"}},
                                  {"saveNameFilters", QStringList{"PNG (*.png)"}}};
        const QString suffix = typedSuffix.isEmpty() ? QStringLiteral("png") : typedSuffix;
        QVariantMap initial{
            {"formatSupport", formats},
            {"suggestedFile", QUrl::fromLocalFile(isolated.filePath("copy." + suffix))},
            {"suggestedSuffix", suffix}};
        std::unique_ptr<QObject> dialog(component.beginCreate(engine.rootContext()));
        QVERIFY2(dialog, qPrintable(component.errorString()));
        QVERIFY2(dialog->inherits("QQuickFileDialog"),
                 "Discovered wrapper must instantiate a production Qt FileDialog");
        for (auto it = initial.begin(); it != initial.end();) {
            if (dialog->metaObject()->indexOfProperty(it.key().toLatin1().constData()) < 0) {
                it = initial.erase(it);
            } else {
                ++it;
            }
        }
        component.setInitialProperties(dialog.get(), initial);
        component.completeCreate();
        QVERIFY2(!component.isError(), qPrintable(component.errorString()));
        QVERIFY2(dialog, qPrintable(component.errorString()));
        dialog->setParent(&window);
        QVERIFY(dialog->setProperty("parentWindow", QVariant::fromValue(&window)));
        QSignalSpy failed(dialog.get(), SIGNAL(openFailed(QString)));
        QSignalSpy rejected(dialog.get(), SIGNAL(rejected()));
        for (int cycle = 0; cycle < 2; ++cycle) {
            QVariant opened;
            QVERIFY(QMetaObject::invokeMethod(dialog.get(), "openSafely",
                                              Q_RETURN_ARG(QVariant, opened)));
            QCOMPARE(opened.toBool(), !missing);
            if (missing) {
                QCOMPARE(failed.count(), cycle + 1);
                QVERIFY(failed.last().first().toString().contains("Your image and edits are safe"));
                QVERIFY(!dialog->property("visible").toBool());
            } else {
                QTRY_VERIFY(dialog->property("visible").toBool());
                QObject* picker = nullptr;
                for (auto* object : window.findChildren<QObject*>()) {
                    if (object->inherits("QQuickFileDialogImpl") &&
                        object->property("visible").toBool()) {
                        picker = object;
                        break;
                    }
                }
                QVERIFY2(picker, "Qt fallback picker content did not become visible");
                auto* content =
                    qobject_cast<QQuickItem*>(picker->property("contentItem").value<QObject*>());
                QVERIFY(content);
                QTRY_VERIFY(content->window() && content->window()->isExposed());
                if (!typedSuffix.isEmpty()) {
                    checkTypedDestination(dialog.get(), picker, content->window(),
                                          isolated.filePath("typed." + typedSuffix));
                    return;
                }
                QTest::keyClick(content->window(), Qt::Key_Escape);
                QTRY_VERIFY(!dialog->property("visible").toBool());
                QCOMPARE(rejected.count(), cycle + 1);
            }
        }
    }

  private:
    void checkTypedDestination(QObject* dialog, QObject* picker, QQuickWindow* window,
                               const QString& expectedPath)
    {
        QSignalSpy chosen(dialog, SIGNAL(fileChosen(QUrl)));
        QVERIFY(chosen.isValid());
        auto* field = picker->findChild<QQuickItem*>(QStringLiteral("fileNameTextField"));
        QVERIFY(field);
        field->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        for (const char key : QByteArrayLiteral("typed")) {
            QTest::keyClick(window, key);
        }
        QCOMPARE(field->property("text").toString(), QStringLiteral("typed"));

        QQuickItem* box = nullptr;
        for (auto* item : picker->findChildren<QQuickItem*>()) {
            if (item->inherits("QQuickDialogButtonBox") && item->isVisible()) {
                box = item;
                break;
            }
        }
        QVERIFY(box);
        QQmlExpression accept(qmlContext(box), box,
                              QStringLiteral("standardButton(DialogButtonBox.Save) || "
                                             "standardButton(DialogButtonBox.Open)"));
        auto* button = qobject_cast<QQuickItem*>(accept.evaluate().value<QObject*>());
        QVERIFY2(!accept.hasError(), qPrintable(accept.error().toString()));
        QVERIFY(button && button->isVisible() && button->isEnabled());
        QTest::mouseClick(
            window, Qt::LeftButton, Qt::NoModifier,
            button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint());
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(chosen.count(), 1);
        const QUrl emitted = chosen.first().first().toUrl();
        qInfo().noquote() << "fileChosen:" << emitted.toString()
                          << "local:" << emitted.isLocalFile() << "path:" << emitted.toLocalFile();
        QVERIFY(emitted.isLocalFile());
        QCOMPARE(emitted.toLocalFile(), expectedPath);
    }
};

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QGuiApplication application(argc, argv);
    FileDialogRuntimeTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_file_dialog_runtime.moc"
