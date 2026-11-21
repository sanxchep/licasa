#include "platform/native_window_ops.h"

#include <QAccessible>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QtTest>
#include <memory>

class AccessibilityControlsTest final : public QObject {
    Q_OBJECT
  private slots:
    void sliders_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QVariantMap>("initial");
        QTest::addColumn<QByteArray>("signal");
        QTest::newRow("editor adjustment") << QString("AdjustRow")
                                           << QVariantMap{{"label", "Exposure"},
                                                          {"value", 0.25},
                                                          {"from", -1.0},
                                                          {"to", 1.0},
                                                          {"keyboardStep", 0.1}}
                                           << QByteArray(SIGNAL(valueRequested(double)));
        QTest::newRow("settings value") << QString("ValueRow")
                                        << QVariantMap{{"title", "Opacity"},
                                                       {"value", 0.25},
                                                       {"from", -1.0},
                                                       {"to", 1.0},
                                                       {"step", 0.1}}
                                        << QByteArray(SIGNAL(valueChangedByUser(double)));
    }
    void sliders()
    {
        QFETCH(QString, file);
        QFETCH(QVariantMap, initial);
        QFETCH(QByteArray, signal);
        QQmlEngine engine;
        QQmlComponent component(
            &engine, QUrl::fromLocalFile(QStringLiteral(LICASA_SOURCE_DIR "/qml/components/") +
                                         file + ".qml"));
        std::unique_ptr<QObject> control(component.createWithInitialProperties(initial));
        QVERIFY2(control, qPrintable(component.errorString()));
        auto* slider = control->findChild<QObject*>(
            file == "AdjustRow" ? "adjustmentAccessibleSlider" : "settingsAccessibleSlider");
        // Before the fix the raw model value was exposed on the control itself.
        auto* accessible = QAccessible::queryAccessibleInterface(slider ? slider : control.get());
        QVERIFY(accessible);
        QCOMPARE(accessible->role(), QAccessible::Slider);
        QVERIFY(!accessible->text(QAccessible::Name).isEmpty());
        QVERIFY(accessible->state().focusable);
        auto* values = accessible->valueInterface();
        QVERIFY(values);
        QCOMPARE(values->currentValue().toDouble(), 25.0);
        QCOMPARE(values->minimumValue().toDouble(), -100.0);
        QCOMPARE(values->maximumValue().toDouble(), 100.0);
        QCOMPARE(values->minimumStepSize().toDouble(), 10.0);
        QSignalSpy requested(control.get(), signal.constData());
        QVERIFY(requested.isValid());
        auto* actions = accessible->actionInterface();
        QVERIFY(actions);
        actions->doAction(QAccessibleActionInterface::increaseAction());
        QCOMPARE(requested.count(), 1);
        QCOMPARE(requested.last().first().toDouble(), 0.35);
        actions->doAction(QAccessibleActionInterface::decreaseAction());
        QCOMPARE(requested.count(), 2);
        QCOMPARE(requested.last().first().toDouble(), 0.15);
        // Screen-reader actions must go through the same production request
        // signal as keyboard/pointer input, preserving the parent's binding.
        QCOMPARE(control->property("value").toDouble(), 0.25);
        // A direct AT-SPI value write must use the production request signal,
        // rather than replacing the model binding on the public edit value.
        values->setCurrentValue(40);
        QCOMPARE(requested.count(), 3);
        QCOMPARE(requested.last().first().toDouble(), 0.4);
        QCOMPARE(control->property("value").toDouble(), 0.25);
        QCOMPARE(values->currentValue().toDouble(), 25.0);
        QVERIFY(control->setProperty("value", 0.65));
        QCOMPARE(values->currentValue().toDouble(), 65.0);
        QCOMPARE(requested.count(), 3);
        if (file == "ValueRow") {
            QVERIFY(control->setProperty("subtitle", "Dark backdrop behind the image"));
            QCOMPARE(accessible->text(QAccessible::Description),
                     QString("Dark backdrop behind the image. Value in percent"));
        }
    }
    void fullscreenDesktop_data()
    {
        using Platform = NativeWindowOps::FullscreenPlatform;
        QTest::addColumn<QString>("qpa");
        QTest::addColumn<QString>("desktop");
        QTest::addColumn<int>("platform");
        QTest::newRow("GNOME") << QString("wayland") << QString("GNOME")
                               << int(Platform::GnomeWayland);
        QTest::newRow("Ubuntu GNOME")
            << QString("wayland-egl") << QString("ubuntu:GNOME") << int(Platform::GnomeWayland);
        QTest::newRow("KDE") << QString("wayland") << QString("KDE")
                             << int(Platform::NativeWayland);
        QTest::newRow("missing") << QString("wayland") << QString() << int(Platform::NativeWayland);
        QTest::newRow("partial token")
            << QString("wayland") << QString("notGNOME:ubuntu") << int(Platform::NativeWayland);
        QTest::newRow("XWayland") << QString("xcb") << QString("GNOME") << int(Platform::Other);
    }

    void fullscreenDesktop()
    {
        QFETCH(QString, qpa);
        QFETCH(QString, desktop);
        QFETCH(int, platform);
        QCOMPARE(int(NativeWindowOps::fullscreenPlatform(qpa, desktop)), platform);
    }

    void disabledSettingsSliderRejectsAccessibleEdits()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(
                                             LICASA_SOURCE_DIR "/qml/components/ValueRow.qml")));
        std::unique_ptr<QObject> control(
            component.createWithInitialProperties({{"title", "Background opacity"},
                                                   {"subtitle", "Enable the top bar to adjust"},
                                                   {"value", 0.35},
                                                   {"enabled", false}}));
        QVERIFY2(control, qPrintable(component.errorString()));
        auto* slider = control->findChild<QObject*>("settingsAccessibleSlider");
        auto* accessible = QAccessible::queryAccessibleInterface(slider);
        QVERIFY(accessible);
        QVERIFY(accessible->state().readOnly);
        QVERIFY(!accessible->state().focusable);
        QCOMPARE(accessible->text(QAccessible::Description),
                 QString("Enable the top bar to adjust. Value in percent"));
        auto* actions = accessible->actionInterface();
        auto* values = accessible->valueInterface();
        QVERIFY(actions && values);
        QSignalSpy requested(control.get(), SIGNAL(valueChangedByUser(double)));
        actions->doAction(QAccessibleActionInterface::increaseAction());
        actions->doAction(QAccessibleActionInterface::decreaseAction());
        values->setCurrentValue(50);
        QCOMPARE(requested.size(), 0);
        QCOMPARE(values->currentValue().toInt(), 35);

        QVERIFY(control->setProperty("enabled", true));
        values->setCurrentValue(50);
        QCOMPARE(requested.size(), 1);
        QCOMPARE(requested.first().first().toDouble(), 0.5);
    }

    void readableValues_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<double>("modelValue");
        QTest::addColumn<int>("spokenValue");
        for (const auto& file : {QString("AdjustRow"), QString("ValueRow")}) {
            for (const auto& value :
                 {std::pair(0.09999999999999999, 10), std::pair(-0.09999999999999999, -10),
                  std::pair(0.19999999999999998, 20), std::pair(-0.19999999999999998, -20),
                  std::pair(6.938893903907228e-18, 0), std::pair(-6.938893903907228e-18, 0),
                  std::pair(0.123456789, 12), std::pair(-1.0, -100), std::pair(1.0, 100)}) {
                const auto name = file + '-' + QString::number(value.first, 'g', 17);
                QTest::newRow(qPrintable(name)) << file << value.first << value.second;
            }
        }
    }
    void readableValues()
    {
        QFETCH(QString, file);
        QFETCH(double, modelValue);
        QFETCH(int, spokenValue);
        QQmlEngine engine;
        QQmlComponent component(
            &engine, QUrl::fromLocalFile(QStringLiteral(LICASA_SOURCE_DIR "/qml/components/") +
                                         file + ".qml"));
        const auto nameProperty = file == "AdjustRow" ? "label" : "title";
        std::unique_ptr<QObject> control(component.createWithInitialProperties(
            {{nameProperty, "Brightness"}, {"value", modelValue}, {"from", -1.0}, {"to", 1.0}}));
        QVERIFY2(control, qPrintable(component.errorString()));
        auto* slider = control->findChild<QObject*>(
            file == "AdjustRow" ? "adjustmentAccessibleSlider" : "settingsAccessibleSlider");
        QVERIFY(slider);
        auto* accessible = QAccessible::queryAccessibleInterface(slider);
        QVERIFY(accessible);
        QCOMPARE(accessible->valueInterface()->currentValue().toInt(), spokenValue);
        QCOMPARE(accessible->valueInterface()->currentValue().metaType().id(), QMetaType::Int);
        QCOMPARE(accessible->text(QAccessible::Description), QString("Value in percent"));
        // Pointer precision belongs to the edit model, independently of speech.
        QCOMPARE(control->property("value").toDouble(), modelValue);
    }
    void fractionalDisplayUsesWholeNamedUnits()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(
                                             LICASA_SOURCE_DIR "/qml/components/ValueRow.qml")));
        std::unique_ptr<QObject> control(
            component.createWithInitialProperties({{"title", "Memory cache"},
                                                   {"subtitle", "Budget for decoded images"},
                                                   {"value", 8704.0},
                                                   {"from", 512.0},
                                                   {"to", 32768.0},
                                                   {"step", 128.0},
                                                   {"displayMultiplier", 1.0 / 1024},
                                                   {"displayDecimals", 2},
                                                   {"displaySuffix", " GiB"},
                                                   {"accessibleUnit", "MiB"}}));
        QVERIFY2(control, qPrintable(component.errorString()));
        auto* slider = control->findChild<QObject*>("settingsAccessibleSlider");
        QVERIFY(slider);
        auto* accessible = QAccessible::queryAccessibleInterface(slider);
        QCOMPARE(accessible->valueInterface()->currentValue().toDouble(), 8704.0);
        QCOMPARE(accessible->valueInterface()->minimumStepSize().toDouble(), 128.0);
        QCOMPARE(accessible->text(QAccessible::Description),
                 QString("Budget for decoded images. Value in MiB"));
        auto* valueText = control->findChild<QObject*>("valueRowValue");
        QVERIFY(valueText);
        QCOMPARE(valueText->property("text").toString(), QString("8.50 GiB"));
    }
    void adjustmentResetIsReachable()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(
                                             LICASA_SOURCE_DIR "/qml/components/AdjustRow.qml")));
        std::unique_ptr<QObject> control(
            component.createWithInitialProperties({{"label", "Exposure"}, {"value", 0.25}}));
        QVERIFY2(control, qPrintable(component.errorString()));
        auto* reset = control->findChild<QQuickItem*>("adjustmentResetButton");
        QVERIFY(reset);
        QVERIFY(reset->activeFocusOnTab());
        auto* accessible = QAccessible::queryAccessibleInterface(reset);
        QCOMPARE(accessible->role(), QAccessible::Button);
        QCOMPARE(accessible->text(QAccessible::Name), QString("Reset Exposure"));
        QSignalSpy requested(control.get(), SIGNAL(valueRequested(double)));
        accessible->actionInterface()->doAction(QAccessibleActionInterface::pressAction());
        QCOMPARE(requested.count(), 1);
        QCOMPARE(requested.last().first().toDouble(), 0.0);
        QVERIFY(control->setProperty("value", 0.0));
        QVERIFY(!reset->isVisible());
    }
    void timelineUsesWholePercentWithoutChangingSeekPrecision()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(
                                             LICASA_SOURCE_DIR "/qml/TemporalControls.qml")));
        std::unique_ptr<QObject> control(component.createWithInitialProperties(
            {{"hasTimeline", true}, {"canScrub", true}, {"position", 0.19999999999999998}}));
        QVERIFY2(control, qPrintable(component.errorString()));
        QAccessibleInterface* slider = nullptr;
        for (auto* child : control->findChildren<QQuickItem*>()) {
            auto* accessible = QAccessible::queryAccessibleInterface(child);
            if (accessible && accessible->role() == QAccessible::Slider) {
                slider = accessible;
                break;
            }
        }
        QVERIFY(slider);
        QCOMPARE(slider->valueInterface()->currentValue().toInt(), 20);
        QCOMPARE(slider->valueInterface()->minimumValue().toInt(), 0);
        QCOMPARE(slider->valueInterface()->maximumValue().toInt(), 100);
        QCOMPARE(slider->valueInterface()->minimumStepSize().toInt(), 2);
        QCOMPARE(slider->text(QAccessible::Description), QString("Position in percent"));
        QSignalSpy requested(control.get(), SIGNAL(seekRequested(double)));
        slider->actionInterface()->doAction(QAccessibleActionInterface::increaseAction());
        QCOMPARE(requested.count(), 1);
        QCOMPARE(requested.first().first().toDouble(), 0.19999999999999998 + 0.02);
        QCOMPARE(control->property("position").toDouble(), 0.19999999999999998);
    }
    void toggleStateAndAction()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(
                                             LICASA_SOURCE_DIR "/qml/components/ToggleRow.qml")));
        std::unique_ptr<QObject> control(
            component.createWithInitialProperties({{"title", "Smooth motion"}, {"checked", true}}));
        QVERIFY2(control, qPrintable(component.errorString()));
        auto* accessible = QAccessible::queryAccessibleInterface(control.get());
        QVERIFY(accessible);
        QCOMPARE(accessible->role(), QAccessible::CheckBox);
        QVERIFY(accessible->state().checkable);
        QVERIFY(accessible->state().checked);
        QSignalSpy toggled(control.get(), SIGNAL(toggled(bool)));
        accessible->actionInterface()->doAction(QAccessibleActionInterface::toggleAction());
        QCOMPARE(toggled.count(), 1);
        QCOMPARE(toggled.first().first().toBool(), false);
        QVERIFY(control->property("checked").toBool());
    }
    void selectedButtonsExposeState()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(
                                             LICASA_SOURCE_DIR "/qml/components/ActionChip.qml")));
        std::unique_ptr<QObject> control(component.createWithInitialProperties(
            {{"text", "PNG"}, {"checkable", true}, {"checked", true}}));
        QVERIFY2(control, qPrintable(component.errorString()));
        auto* accessible = QAccessible::queryAccessibleInterface(control.get());
        QVERIFY(accessible);
        QCOMPARE(accessible->text(QAccessible::Name), QString("PNG"));
        QVERIFY(accessible->state().checkable);
        QVERIFY(accessible->state().checked);
        QSignalSpy pressed(control.get(), SIGNAL(clicked()));
        accessible->actionInterface()->doAction(QAccessibleActionInterface::pressAction());
        QCOMPARE(pressed.count(), 1);
    }
};
int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    QAccessible::setActive(true);
    AccessibilityControlsTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_accessibility_controls.moc"
