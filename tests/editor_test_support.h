#pragma once

#include "export/image_export_contract.h"
#include "export/image_save_operations.h"
#include "imaging/image_edit_pipeline.h"
#include <QAccessible>
#include <QClipboard>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>

// Test-only bridge: real window input, accessible actions and isolated files.
// Production editor state and the production save service remain under test.
class EditorTestSupport final : public QObject {
    Q_OBJECT
  public:
    Q_INVOKABLE QUrl destination(const QString& name) const
    {
        return QUrl::fromLocalFile(directory_.filePath(name));
    }
    Q_INVOKABLE QUrl image(const QString& name, int width = 97, int height = 61,
                           bool alpha = false) const
    {
        QImage pixels(width, height, QImage::Format_ARGB32);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                pixels.setPixel(x, y,
                                qRgba(30 + x % 100, 60 + y % 100, 90 + (x + y) % 100,
                                      alpha ? (x * 29 + y * 13) % 256 : 255));
            }
        }
        pixels.setColorSpace(QColorSpace::SRgb);
        pixels.setDotsPerMeterX(11811);
        pixels.setDotsPerMeterY(11811);
        const auto url = destination(name);
        return pixels.save(url.toLocalFile()) ? url : QUrl();
    }
    Q_INVOKABLE QVariantMap inspect(const QUrl& url) const
    {
        QImageReader reader(url.toLocalFile());
        const QSize size = reader.size();
        return {{"size", size},
                {"withinBudget", size.isValid()},
                {"format", QString::fromLatin1(reader.format())},
                {"width", size.width()},
                {"height", size.height()}};
    }
    Q_INVOKABLE QVariantMap decodedInfo(const QUrl& url) const
    {
        const QImage image(url.toLocalFile());
        return {{"width", image.width()}, {"height", image.height()}, {"valid", !image.isNull()}};
    }
    Q_INVOKABLE QByteArray digest(const QUrl& url) const
    {
        QFile file(url.toLocalFile());
        return file.open(QIODevice::ReadOnly)
                   ? QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex()
                   : QByteArray();
    }
    Q_INVOKABLE bool exists(const QUrl& url) const { return QFile::exists(url.toLocalFile()); }
    Q_INVOKABLE QUrl copy(const QUrl& source, const QString& name) const
    {
        const auto target = destination(name);
        QFile::remove(target.toLocalFile());
        return QFile::copy(source.toLocalFile(), target.toLocalFile()) ? target : QUrl();
    }
    Q_INVOKABLE QUrl formatImage(const QString& name, const QString& format) const
    {
        const auto source = image(name + "-base.png");
        const auto result = Licasa::ImageSaveOperations::writeEditedImage(
            source.toLocalFile(), destination(name + "." + format).toLocalFile(), {},
            Licasa::exportOptionsFromMap({{"format", format}, {"iconSize", 64}}), 25);
        return result.destinationUrl;
    }
    Q_INVOKABLE QVariantList pixel(const QUrl& url, int x, int y) const
    {
        const QImage image(url.toLocalFile());
        if (!image.rect().contains(x, y)) {
            return {};
        }
        const QColor color = image.pixelColor(x, y);
        return {color.red(), color.green(), color.blue(), color.alpha()};
    }
    Q_INVOKABLE bool matchesEdits(const QUrl& source, const QUrl& output,
                                  const QVariantMap& edits) const
    {
        QImage expected(source.toLocalFile());
        return Licasa::applyImageEdits(expected, Licasa::editParametersFromMap(edits)) &&
               expected.convertToFormat(QImage::Format_RGBA8888) ==
                   QImage(output.toLocalFile()).convertToFormat(QImage::Format_RGBA8888);
    }
    Q_INVOKABLE QVariantList formats() const { return Licasa::ImageExport::formats(); }
    Q_INVOKABLE void clipboard(const QString& value) const
    {
        QGuiApplication::clipboard()->setText(value);
    }
    Q_INVOKABLE bool screenshot(QQuickWindow* window, const QString& name) const
    {
        const auto folder = qEnvironmentVariable("LICASA_TEST_SCREENSHOTS");
        if (folder.isEmpty()) {
            return true;
        }
        return QDir().mkpath(folder) && window->grabWindow().save(folder + '/' + name + ".png");
    }
    Q_INVOKABLE void key(QQuickWindow* window, int code, int modifiers = 0) const
    {
        QTest::keyClick(window, Qt::Key(code), Qt::KeyboardModifiers(modifiers));
    }
    Q_INVOKABLE void text(QQuickWindow* window, const QString& value) const
    {
        for (const auto character : value.toLatin1()) {
            QTest::keyClick(window, character);
        }
    }
    Q_INVOKABLE bool press(QObject* object) const
    {
        auto* accessible = QAccessible::queryAccessibleInterface(object);
        if (!accessible || !accessible->actionInterface()) {
            return false;
        }
        accessible->actionInterface()->doAction(QAccessibleActionInterface::pressAction());
        return true;
    }
    Q_INVOKABLE QVariantMap accessible(QObject* object) const
    {
        auto* item = QAccessible::queryAccessibleInterface(object);
        if (!item) {
            return {};
        }
        return {{"name", item->text(QAccessible::Name)},
                {"value", item->text(QAccessible::Value)},
                {"role", int(item->role())},
                {"description", item->text(QAccessible::Description)},
                {"editable", item->state().editable},
                {"disabled", item->state().disabled},
                {"invisible", item->state().invisible}};
    }

  private:
    QTemporaryDir directory_;
};
