#include "diagnostics_common.h"

#include "app/app_constants.h"
#include "app/application_settings.h"
#include "app/window_manager.h"
#include "export/image_save_service.h"
#include "imaging/async_image_provider.h"
#include "imaging/codec_plugins.h"
#include "imaging/format_support.h"
#include "imaging/image_animation.h"
#include "imaging/image_decode_contract.h"
#include "imaging/image_probe.h"
#include "imaging/image_resource_policy.h"
#include "platform/graphics_backend.h"
#include "platform/native_window_ops.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonObject>
#include <QPluginLoader>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QUrl>

#include <memory>

namespace {
QJsonArray strings(const QList<QByteArray>& values)
{
    QJsonArray result;
    for (const auto& value : values) {
        result.append(QString::fromLatin1(value));
    }
    return result;
}

int inspectFormats(const QStringList& paths, Licasa::ImageResourcePolicy& policy)
{
    QJsonObject report{
        {"qt_version", QString::fromLatin1(qVersion())},
        {"read_formats", strings(QImageReader::supportedImageFormats())},
        {"read_mime_types", strings(QImageReader::supportedMimeTypes())},
        {"write_formats", strings(QImageWriter::supportedImageFormats())},
        {"maximum_megapixels", policy.maximumImageMegapixels()},
        {"policy_allocation_mib", policy.decoderAllocationLimitMiB()},
        {"qt_allocation_mib", QImageReader::allocationLimit()},
        {"qt_imageio_maxalloc", QString::fromLocal8Bit(qgetenv("QT_IMAGEIO_MAXALLOC"))},
        {"plugin_paths", QJsonArray::fromStringList(QCoreApplication::libraryPaths())}};
    QJsonArray plugins;
    for (const auto& directory : QCoreApplication::libraryPaths()) {
        QDir imagePlugins(directory + QStringLiteral("/imageformats"));
        for (const auto& file : imagePlugins.entryList(QDir::Files)) {
            const QString path = imagePlugins.absoluteFilePath(file);
            const QPluginLoader loader(path);
            plugins.append(QJsonObject{{"path", path}, {"metadata", loader.metaData()}});
        }
    }
    report["plugins"] = plugins;
    report["before_decode"] = LicasaDiagnostics::processMetrics();
    QJsonArray samples;
    for (const auto& path : paths) {
        QImageReader reader(path);
        Licasa::ImageDecodeContract::configure(reader, policy.maximumImagePixels());
        reader.setAutoTransform(true);
        const qint64 probeStart = LicasaDiagnostics::monotonicNs();
        const bool canRead = reader.canRead();
        const QSize declared = reader.size();
        const qint64 probeNs = LicasaDiagnostics::monotonicNs() - probeStart;
        const QByteArray format = reader.format();
        QSize requested = declared.isValid()
                              ? declared.scaled(QSize(1200, 900), Qt::KeepAspectRatio)
                              : QSize(1200, 900);
        reader.setScaledSize(requested);
        const qint64 start = LicasaDiagnostics::monotonicNs();
        // This is a decode smoke test, so failed/unsupported samples are reported,
        // not promoted to advertised support based on canRead alone.
        const QImage decoded = reader.read();
        samples.append(QJsonObject{
            {"path", path},
            {"can_read", canRead},
            {"format", QString::fromLatin1(format)},
            {"probe_ms", probeNs / 1e6},
            {"declared_width", declared.width()},
            {"declared_height", declared.height()},
            {"native_scaled_option", reader.supportsOption(QImageIOHandler::ScaledSize)},
            {"animation", reader.supportsAnimation()},
            {"frames", reader.imageCount()},
            {"decoded", !decoded.isNull()},
            {"decoded_width", decoded.width()},
            {"decoded_height", decoded.height()},
            {"decode_ms", (LicasaDiagnostics::monotonicNs() - start) / 1e6},
            {"error", decoded.isNull() ? reader.errorString() : QString()},
            {"codec_error",
             reader.device()->property(Licasa::ImageDecodeContract::errorProperty).toString()},
            {"decoder_backend", reader.text(QStringLiteral("Backend"))},
            {"preview_path", reader.text(QStringLiteral("PreviewPath"))},
            {"raw_development", reader.device()->property("_licasaRawDemosaic").toBool()},
            {"largest_native_raster_pixels",
             qint64(reader.device()->property("_licasaNativeRasterPixels").toULongLong())},
            {"native_peak_tracked_bytes",
             qint64(reader.device()->property("_licasaNativePeakBytes").toULongLong())},
            {"process", LicasaDiagnostics::processMetrics()}});
    }
    report["samples"] = samples;
    LicasaDiagnostics::printJson(report);
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    bool validStart = false;
    const qint64 parentStart = qgetenv("LICASA_BENCHMARK_START_NS").toLongLong(&validStart);
    const qint64 started = validStart ? parentStart : LicasaDiagnostics::monotonicNs();
    Licasa::ImageResourcePolicy::initializeDecoderEnvironment();
    Licasa::configureSurfaceFormatForRequestedOpenGL();
    QGuiApplication application(argc, argv);
    Licasa::initializeImagePlugins();
    application.setOrganizationName(QStringLiteral("LicasaDiagnostics"));
    application.setApplicationName(QStringLiteral("LicasaDiagnostics"));
    application.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"formats", "Report runtime formats and decode the supplied samples."});
    parser.addOption(
        {"viewer", "Measure the production QML viewer (use an isolated XDG_CONFIG_HOME)."});
    parser.addOption({"navigation", "Check the visible image while browsing to a second image."});
    parser.addOption({"raw-behavior", "Verify progressive RAW preview and automatic full "
                                      "development in the production viewer."});
    parser.addOption(
        {"raw-preview-only", "Stop the RAW behavior check after the embedded preview."});
    parser.addOption(
        {"raw-edit-only", "Stop the RAW behavior check after two automatic full-detail edits."});
    parser.addOption(
        {"motion-photo-controls",
         "Drive the production TemporalControls lifecycle against one real Motion Photo."});
    parser.addOption({"megapixels", "Set the test image policy.", "mp", "100"});
    parser.addOption({"cycles", "Number of viewer close/reopen cycles to verify.", "count", "1"});
    parser.addPositionalArgument("images", "Local sample paths.", "[images...]");
    parser.process(application);
    Licasa::ViewerPreferences preferences;
    bool validMp = false;
    const int megapixels = parser.value("megapixels").toInt(&validMp);
    bool validCycles = false;
    const int cycles = parser.value("cycles").toInt(&validCycles);
    if (!validMp || megapixels < 25 || megapixels > 1000 || !validCycles || cycles < 1 ||
        cycles > 10) {
        parser.showHelp(2);
    }
    preferences.setMaximumImageMegapixels(megapixels);
    Licasa::ImageResourcePolicy policy(preferences);
    if (parser.isSet("formats")) {
        return inspectFormats(parser.positionalArguments(), policy);
    }
    const bool navigation = parser.isSet("navigation");
    if (!parser.isSet("viewer") || parser.positionalArguments().size() > (navigation ? 2 : 1) ||
        (navigation && parser.positionalArguments().size() != 2) ||
        (parser.isSet("raw-preview-only") && parser.isSet("raw-edit-only"))) {
        parser.showHelp(2);
    }

    const QUrl imageUrl =
        parser.positionalArguments().isEmpty()
            ? QUrl()
            : QUrl::fromLocalFile(
                  QFileInfo(parser.positionalArguments().first()).absoluteFilePath());
    if (parser.isSet("motion-photo-controls") && imageUrl.isEmpty()) {
        parser.showHelp(2);
    }
    Licasa::BackgroundModeManager background;
    Licasa::ImageProbe imageProbe(policy);
    Licasa::FormatSupport formats;
    Licasa::ImageSaveService saves(policy);
    NativeWindowOps nativeOps;
    auto* decodeProvider = new Licasa::AsyncImageProvider(policy);
    Licasa::ImageAnimationService animations(policy, *decodeProvider);
    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("animation"), animations.createFrameProvider());
    const QVariantMap properties{
        {"imageProbe", QVariant::fromValue(static_cast<QObject*>(&imageProbe))},
        {"formatSupport", QVariant::fromValue(static_cast<QObject*>(&formats))},
        {"imageSaveService", QVariant::fromValue(static_cast<QObject*>(&saves))},
        {"imageAnimationService", QVariant::fromValue(static_cast<QObject*>(&animations))},
        {"nativeWindowOps", QVariant::fromValue(static_cast<QObject*>(&nativeOps))},
        {"backgroundModeManager", QVariant::fromValue(static_cast<QObject*>(&background))},
        {"viewerPreferences", QVariant::fromValue(static_cast<QObject*>(&preferences))}};
    Licasa::WindowManager windows(&engine, properties);
    windows.setMotionPhotoResourcePolicy(&policy);
    windows.setImageResourcePolicy(&policy);
    windows.setImageProvider(decodeProvider);
    windows.setImageSaveService(&saves);
    engine.addImageProvider(QString::fromLatin1(Licasa::Constants::imageProviderName),
                            decodeProvider);
    std::unique_ptr<LicasaDiagnostics::DiagnosticCheck> check;
    if (navigation) {
        const QUrl nextUrl =
            QUrl::fromLocalFile(QFileInfo(parser.positionalArguments().at(1)).absoluteFilePath());
        check = LicasaDiagnostics::makeNavigationTransitionCheck(nextUrl);
    } else if (parser.isSet("raw-behavior")) {
        check = LicasaDiagnostics::makeRawBehaviorCheck(parser.isSet("raw-preview-only"),
                                                        parser.isSet("raw-edit-only"));
    } else if (parser.isSet("motion-photo-controls")) {
        check = LicasaDiagnostics::makeMotionPhotoControlsCheck();
    } else {
        check = LicasaDiagnostics::makeViewerMeasurement(started, imageUrl, cycles);
    }
    if (!windows.createWindow(imageUrl, false, true)) {
        return 2;
    }
    for (auto* window : QGuiApplication::topLevelWindows()) {
        if (auto* quick = qobject_cast<QQuickWindow*>(window)) {
            const bool attached = check->attach(quick);
            if (!attached) {
                return 2;
            }
            const int exitCode = application.exec();
            decodeProvider->waitForPendingWork();
            return exitCode;
        }
    }
    return 2;
}
