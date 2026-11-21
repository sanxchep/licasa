#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
#include "imaging/hdr/viewer_memory_policy.h"
#include "imaging/presented_still_sources.h"
#include "gainmap-lab/jpeg_gainmap.h"
#endif
#include "diagnostics_common.h"
#include "diagnostics_dialogs.h"

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

#include <QColorSpace>
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
#include <QTimer>
#include <QUrl>

#include <memory>

namespace {
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
int inspectGainmapCodec(const QString& path)
{
    const auto before = LicasaDiagnostics::processMetrics();
    QImageReader reader(path, "jpeg");
    reader.setAutoTransform(true);
    const auto base = reader.read();
    const auto identity = Licasa::externalFileIdentityAtPath(path);
    QString error;
    const auto hdr = identity && !base.isNull()
        ? Licasa::GainmapLab::decode(path, *identity, 4, error)
        : std::nullopt;
    QJsonObject report{{"qt", QString::fromLatin1(qVersion())},
                       {"before_decode", before},
                       {"sdr_decoded", !base.isNull()},
                       {"sdr_width", base.width()}, {"sdr_height", base.height()},
                       {"hdr_decoded", hdr.has_value()}, {"hdr_error", error},
                       {"after_decode", LicasaDiagnostics::processMetrics()}};
    if (hdr) {
        const auto& image = hdr->linear;
        const auto pixel = image.pixelColor(image.width() / 2, image.height() / 2);
        report["hdr_width"] = image.width();
        report["hdr_height"] = image.height();
        report["hdr_fp16"] = image.format() == QImage::Format_RGBA16FPx4;
        report["hdr_center"] = QJsonArray{pixel.redF(), pixel.greenF(), pixel.blueF(), pixel.alphaF()};
    }
    LicasaDiagnostics::printJson(report);
    return base.isNull() ? 1 : 0;
}
#endif

QJsonArray strings(const QList<QByteArray>& values)
{
    QJsonArray result;
    for (const auto& value : values) {
        result.append(QString::fromLatin1(value));
    }
    return result;
}

int inspectFormats(const QStringList& paths, Licasa::ImageResourcePolicy& policy,
                   bool nativeSdrPixels)
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
        if (!nativeSdrPixels) {
            const QSize requested = declared.isValid()
                ? declared.scaled(QSize(1200, 900), Qt::KeepAspectRatio) : QSize(1200, 900);
            reader.setScaledSize(requested);
        }
        const qint64 start = LicasaDiagnostics::monotonicNs();
        // This is a decode smoke test, so failed/unsupported samples are reported,
        // not promoted to advertised support based on canRead alone.
        const QImage decoded = reader.read();
        QJsonObject sample{
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
            {"process", LicasaDiagnostics::processMetrics()}};
        if (nativeSdrPixels && !decoded.isNull()) {
            const auto rgba = [&decoded](int x, int y) {
                const auto color = decoded.pixelColor(x, y);
                return QJsonArray{color.red(), color.green(), color.blue(), color.alpha()};
            };
            QJsonArray top;
            for (int x = 0; x < qMin(9, decoded.width()); ++x) top.append(rgba(x, 0));
            sample["native_sdr_pixels"] = QJsonObject{
                {"srgb", decoded.colorSpace() == QColorSpace(QColorSpace::SRgb)},
                {"depth", decoded.depth()},
                {"top", top},
                {"center", rgba(decoded.width() / 2, decoded.height() / 2)}};
        }
        samples.append(sample);
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
    for (int argument = 1; argument < argc; ++argument) {
        if (QString::fromLocal8Bit(argv[argument]) == QStringLiteral("--dialog-interactions")) {
            QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        }
    }
    QGuiApplication application(argc, argv);
    Licasa::configureSurfaceFormatForPlatform(QGuiApplication::platformName());
    Licasa::initializeImagePlugins();
    application.setOrganizationName(QStringLiteral("LicasaDiagnostics"));
    application.setApplicationName(QStringLiteral("LicasaDiagnostics"));
    application.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption(
        {"dialog-interactions", "Exercise the production file-picker UI and save flow."});
    parser.addOption({"dialog-sample", "Known image to select in the picker.", "path"});
    parser.addOption({"dialog-output-dir", "Isolated output directory for dialog tests.", "path"});
    parser.addOption(
        {"dialog-expect-unavailable", "Verify recoverable missing file-picker dependencies."});
    parser.addOption({"formats", "Report runtime formats and decode the supplied samples."});
    parser.addOption({"native-sdr-pixels",
                      "With --formats, report unscaled SDR color samples for package checks."});
    parser.addOption(
        {"viewer", "Measure the production QML viewer (use an isolated XDG_CONFIG_HOME)."});
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
    parser.addOption({"wayland-hdr-check", "Verify native HDR or SDR with the production viewer.",
                      "hdr|hdr-calibration|sdr|unsupported"});
    parser.addOption({"wayland-window-churn",
                      "With --wayland-hdr-check, verify floating, zoom and hide/restore."});
    parser.addOption({"wayland-window-seed",
                      "With --wayland-window-churn, replay 100 window actions.", "seed"});
    parser.addOption({"wayland-multiwindow-seed",
                      "With --wayland-hdr-check hdr, churn two production HDR windows.", "seed"});
    parser.addOption({"multiwindow-actions",
                      "With --wayland-multiwindow-seed, the number of actions (a multiple of 50; default 100).",
                      "actions"});
    parser.addOption({"wayland-output-seed",
                      "With --wayland-hdr-check hdr, exercise virtual HDR/SDR migration and hotplug.", "seed"});
    parser.addOption({"output-cycles",
                      "With --wayland-output-seed, the number of 12-action cycles (8 to 400; default 8).", "cycles"});
    parser.addOption({"wayland-replacement-seed",
                      "With --wayland-hdr-check hdr, replace one photo under two HDR windows.", "seed"});
    parser.addOption({"wayland-motion-journey",
                      "With --wayland-hdr-check hdr, play one Motion Photo through viewer actions.", "seed"});
    parser.addOption({"motion-peer", "With --wayland-motion-journey, a second Motion Photo to browse to.", "path"});
    parser.addOption({"motion-soak-rounds",
                      "With --wayland-motion-journey, repeat the playback lifecycle and measure each round.", "rounds"});
    parser.addOption({"motion-journey-output",
                      "With --wayland-motion-journey, a new directory for copies and exports.", "path"});
    parser.addOption({"wayland-camera-navigation",
                      "With --wayland-hdr-check hdr, browse between two camera-sized photos.", "seed"});
    parser.addOption({"camera-second", "With --wayland-camera-navigation, the second photo.", "path"});
    parser.addOption({"camera-output", "With --wayland-camera-navigation, a new scratch directory.", "path"});
    parser.addOption({"gainmap-codec-check", "Exercise the private gain-map codec and SDR JPEG fallback."});
#endif
    parser.addOption({"navigation", "Check the visible image while browsing to a second image."});
    parser.addOption({"hdr-navigation-seed",
                      "Exercise seeded SDR-fallback navigation and source replacement with a JPEG "
                      "gain-map fixture.",
                      "seed"});
    parser.addOption(
        {"navigation-during-full",
         "Browse while full-detail development is still running (with --navigation)."});
    parser.addOption(
        {"navigation-sequence", "Measure repeated arrow presses in the production viewer."});
    parser.addOption({"sequence-presses", "Number of arrow presses.", "count", "5"});
    parser.addOption({"sequence-interval-ms", "Time between presses.", "ms", "180"});
    parser.addOption(
        {"sequence-warmup-ms", "Time after first pixels before pressing.", "ms", "250"});
    parser.addOption({"sequence-idle-ms", "Time to observe after the last frame.", "ms", "0"});
    parser.addOption({"sequence-direction",
                      "Arrow direction: right, left, left-right, or right-left.", "direction",
                      "right"});
    parser.addOption({"sequence-release-last", "Release the final arrow key after its press."});
    parser.addOption({"raw-behavior", "Verify progressive RAW preview and automatic full "
                                      "development in the production viewer."});
    parser.addOption(
        {"raw-preview-only", "Stop the RAW behavior check after the embedded preview."});
    parser.addOption(
        {"raw-edit-only", "Stop the RAW behavior check after two automatic full-detail edits."});
    parser.addOption(
        {"motion-photo-controls",
         "Drive the production TemporalControls lifecycle against one real Motion Photo."});
    parser.addOption(
        {"motion-frame-output",
         "Save and decode a real captured frame during the Motion Photo control check.", "path"});
    parser.addOption({"megapixels", "Set the test image policy.", "mp", "100"});
    parser.addOption({"cycles", "Number of viewer close/reopen cycles to verify.", "count", "1"});
    parser.addPositionalArgument("images", "Local sample paths.", "[images...]");
    parser.process(application);
    if (parser.isSet("native-sdr-pixels") && !parser.isSet("formats")) {
        parser.showHelp(2);
    }
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
    if (parser.isSet("gainmap-codec-check")) {
        if (parser.positionalArguments().size() != 1) parser.showHelp(2);
        return inspectGainmapCodec(QFileInfo(parser.positionalArguments().first()).absoluteFilePath());
    }
#endif
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
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
    Licasa::bindHdrMemoryPolicy(preferences);
    Licasa::PresentedStillSources::instance().setEnabled(true);
#endif
    Licasa::ImageResourcePolicy policy(preferences);
    if (parser.isSet("formats")) {
        return inspectFormats(parser.positionalArguments(), policy, parser.isSet("native-sdr-pixels"));
    }
    const bool navigation = parser.isSet("navigation");
    const bool navigationSequence = parser.isSet("navigation-sequence");
    const bool hdrNavigation = parser.isSet("hdr-navigation-seed");
    bool validSeed = false;
    const quint32 hdrNavigationSeed = parser.value("hdr-navigation-seed").toUInt(&validSeed);
    bool validPresses = false;
    bool validInterval = false;
    bool validWarmup = false;
    bool validIdle = false;
    const int sequencePresses = parser.value("sequence-presses").toInt(&validPresses);
    const int sequenceIntervalMs = parser.value("sequence-interval-ms").toInt(&validInterval);
    const int sequenceWarmupMs = parser.value("sequence-warmup-ms").toInt(&validWarmup);
    const int sequenceIdleMs = parser.value("sequence-idle-ms").toInt(&validIdle);
    const QString sequenceDirection = parser.value("sequence-direction");
    if (!parser.isSet("viewer") || (parser.isSet("navigation-during-full") && !navigation) ||
        (navigation && navigationSequence) ||
        (hdrNavigation &&
         (!validSeed || navigation || navigationSequence || parser.isSet("raw-behavior") ||
          parser.isSet("motion-photo-controls") || parser.isSet("dialog-interactions") ||
          QGuiApplication::platformName() != QStringLiteral("offscreen") ||
          parser.positionalArguments().size() != 1)) ||
        parser.positionalArguments().size() > (navigation ? 2 : 1) ||
        (navigation && parser.positionalArguments().size() != 2) ||
        (navigationSequence && parser.positionalArguments().size() != 1) ||
        (navigationSequence &&
         (!validPresses || sequencePresses < 1 || sequencePresses > 100 || !validInterval ||
          sequenceIntervalMs < 1 || sequenceIntervalMs > 10000 || !validWarmup ||
          sequenceWarmupMs < 0 || sequenceWarmupMs > 10000 || !validIdle || sequenceIdleMs < 0 ||
          sequenceIdleMs > 10000 ||
          (sequenceDirection != QStringLiteral("right") &&
           sequenceDirection != QStringLiteral("left") &&
           sequenceDirection != QStringLiteral("left-right") &&
           sequenceDirection != QStringLiteral("right-left")))) ||
        (parser.isSet("raw-preview-only") && parser.isSet("raw-edit-only"))) {
        parser.showHelp(2);
    }

    QUrl imageUrl =
        parser.positionalArguments().isEmpty()
            ? QUrl()
            : QUrl::fromLocalFile(
                  QFileInfo(parser.positionalArguments().first()).absoluteFilePath());
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
    const bool hdrCheck = parser.isSet("wayland-hdr-check");
    const QString expectedOutput = parser.value("wayland-hdr-check");
    bool windowSeedValid = false;
    parser.value("wayland-window-seed").toUInt(&windowSeedValid);
    bool multiwindowSeedValid = false;
    const auto multiwindowSeed = parser.value("wayland-multiwindow-seed").toUInt(&multiwindowSeedValid);
    const bool multiwindow = parser.isSet("wayland-multiwindow-seed");
    bool outputSeedValid = false;
    const auto outputSeed = parser.value("wayland-output-seed").toUInt(&outputSeedValid);
    const bool outputMigration = parser.isSet("wayland-output-seed");
    if ((parser.isSet("wayland-window-churn") && !hdrCheck) ||
        (parser.isSet("wayland-window-seed") &&
         (!parser.isSet("wayland-window-churn") || !windowSeedValid)) ||
        (multiwindow && (!hdrCheck || expectedOutput != "hdr" || !multiwindowSeedValid ||
                        parser.isSet("wayland-window-churn")))) {
        parser.showHelp(2);
    }
    bool multiwindowActionsValid = !parser.isSet("multiwindow-actions");
    const int multiwindowActions = multiwindowActionsValid ? 100
        : parser.value("multiwindow-actions").toInt(&multiwindowActionsValid);
    if (!multiwindowActionsValid || multiwindowActions < 100 || multiwindowActions > 5000 ||
        multiwindowActions % 50 != 0 || (!multiwindow && parser.isSet("multiwindow-actions"))) {
        parser.showHelp(2);
    }
    if (outputMigration && (!hdrCheck || expectedOutput != "hdr" || !outputSeedValid ||
                            multiwindow || parser.isSet("wayland-window-churn"))) parser.showHelp(2);
    bool outputCyclesValid = !parser.isSet("output-cycles");
    const int outputCycles = outputCyclesValid ? 8 : parser.value("output-cycles").toInt(&outputCyclesValid);
    if (!outputCyclesValid || outputCycles < 8 || outputCycles > 400 ||
        (!outputMigration && parser.isSet("output-cycles"))) {
        parser.showHelp(2);
    }
    bool replacementSeedValid = false;
    const auto replacementSeed = parser.value("wayland-replacement-seed").toUInt(&replacementSeedValid);
    const bool replacement = parser.isSet("wayland-replacement-seed");
    if (replacement && (!hdrCheck || expectedOutput != "hdr" || !replacementSeedValid || multiwindow ||
                        outputMigration || parser.isSet("wayland-window-churn"))) parser.showHelp(2);
    bool motionJourneySeedValid = false;
    const auto motionJourneySeed = parser.value("wayland-motion-journey").toUInt(&motionJourneySeedValid);
    const bool motionJourney = parser.isSet("wayland-motion-journey");
    if ((motionJourney && (!hdrCheck || expectedOutput != "hdr" || !motionJourneySeedValid || multiwindow ||
                           outputMigration || replacement || parser.isSet("wayland-window-churn") ||
                           !parser.isSet("motion-journey-output"))) ||
        (!motionJourney && (parser.isSet("motion-peer") || parser.isSet("motion-journey-output") ||
                            parser.isSet("motion-soak-rounds")))) {
        parser.showHelp(2);
    }
    bool soakRoundsValid = false;
    const int soakRounds = parser.value("motion-soak-rounds").toInt(&soakRoundsValid);
    if (parser.isSet("motion-soak-rounds") && (!soakRoundsValid || soakRounds < 1 || soakRounds > 1000 ||
                                               parser.isSet("motion-peer"))) {
        parser.showHelp(2);
    }
    bool cameraSeedValid = false;
    const auto cameraSeed = parser.value("wayland-camera-navigation").toUInt(&cameraSeedValid);
    const bool cameraNavigation = parser.isSet("wayland-camera-navigation");
    if ((cameraNavigation && (!hdrCheck || expectedOutput != "hdr" || !cameraSeedValid || motionJourney ||
                              multiwindow || outputMigration || replacement || parser.isSet("wayland-window-churn") ||
                              !parser.isSet("camera-second") || !parser.isSet("camera-output"))) ||
        (!cameraNavigation && (parser.isSet("camera-second") || parser.isSet("camera-output")))) {
        parser.showHelp(2);
    }
    if (hdrCheck && (imageUrl.isEmpty() || navigation || navigationSequence || hdrNavigation ||
                     parser.isSet("raw-behavior") || parser.isSet("motion-photo-controls") ||
                     parser.isSet("dialog-interactions") ||
                     (expectedOutput != "hdr" && expectedOutput != "hdr-calibration" &&
                      expectedOutput != "sdr" && expectedOutput != "unsupported") ||
                     !QGuiApplication::platformName().startsWith("wayland"))) {
        parser.showHelp(2);
    }
#endif
    if ((parser.isSet("motion-photo-controls") && imageUrl.isEmpty()) ||
        (parser.isSet("motion-frame-output") && !parser.isSet("motion-photo-controls"))) {
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
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
        {"appInfo",QVariantMap{{"version",application.applicationVersion()},{"releaseDate",QString{}},{"hdrViewerBridgeEnabled",true}}},
#endif
        {"imageProbe", QVariant::fromValue(static_cast<QObject*>(&imageProbe))},
        {"formatSupport", QVariant::fromValue(static_cast<QObject*>(&formats))},
        {"imageSaveService", QVariant::fromValue(static_cast<QObject*>(&saves))},
        {"imageAnimationService", QVariant::fromValue(static_cast<QObject*>(&animations))},
        {"nativeWindowOps", QVariant::fromValue(static_cast<QObject*>(&nativeOps))},
        {"backgroundModeManager", QVariant::fromValue(static_cast<QObject*>(&background))},
        {"viewerPreferences", QVariant::fromValue(static_cast<QObject*>(&preferences))}};
    Licasa::WindowManager windows(&engine, properties);
    QObject::connect(&windows, &Licasa::WindowManager::initialGraphicsFailed, &application,
                     [&application](const QString& message) {
                         qWarning().noquote()
                             << "[Licasa] Diagnostic graphics initialization failed:" << message;
                         QTimer::singleShot(0, &application, []() { QCoreApplication::exit(43); });
                     });
    windows.setMotionPhotoResourcePolicy(&policy);
    windows.setImageResourcePolicy(&policy);
    windows.setImageProvider(decodeProvider);
    windows.setImageSaveService(&saves);
    engine.addImageProvider(QString::fromLatin1(Licasa::Constants::imageProviderName),
                            decodeProvider);
    std::unique_ptr<LicasaDiagnostics::DiagnosticCheck> check;
    if (parser.isSet("dialog-interactions")) {
        const QString sample = QFileInfo(parser.value("dialog-sample")).absoluteFilePath();
        const QString output = QFileInfo(parser.value("dialog-output-dir")).absoluteFilePath();
        if (!QFileInfo::exists(sample) || !QDir().mkpath(output) || !imageUrl.isEmpty()) {
            parser.showHelp(2);
        }
        check = std::make_unique<LicasaDiagnostics::DialogInteractionCheck>(
            QUrl::fromLocalFile(sample), output, parser.isSet("dialog-expect-unavailable"));
    } else if (hdrNavigation) {
        check = LicasaDiagnostics::makeHdrNavigationCheck(imageUrl, hdrNavigationSeed);
    } else if (navigationSequence) {
        check = LicasaDiagnostics::makeNavigationSequenceCheck(
            decodeProvider, sequencePresses, sequenceIntervalMs, sequenceWarmupMs, sequenceIdleMs,
            sequenceDirection == QStringLiteral("left")         ? -1
            : sequenceDirection == QStringLiteral("left-right") ? -2
            : sequenceDirection == QStringLiteral("right-left") ? 2
                                                                : 1,
            parser.isSet("sequence-release-last"));
    } else if (navigation) {
        const QUrl nextUrl =
            QUrl::fromLocalFile(QFileInfo(parser.positionalArguments().at(1)).absoluteFilePath());
        check = LicasaDiagnostics::makeNavigationTransitionCheck(
            nextUrl, parser.isSet("navigation-during-full"));
    } else if (parser.isSet("raw-behavior")) {
        check = LicasaDiagnostics::makeRawBehaviorCheck(parser.isSet("raw-preview-only"),
                                                        parser.isSet("raw-edit-only"));
    } else if (parser.isSet("motion-photo-controls")) {
        check =
            LicasaDiagnostics::makeMotionPhotoControlsCheck(parser.value("motion-frame-output"));
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
    } else if (hdrCheck) {
        if (cameraNavigation) {
            check = LicasaDiagnostics::makeCameraNavigationCheck(
                imageUrl, QUrl::fromLocalFile(QFileInfo(parser.value("camera-second")).absoluteFilePath()),
                &windows, cameraSeed, QFileInfo(parser.value("camera-output")).absoluteFilePath());
        } else if (motionJourney) {
            const QString peer = parser.value("motion-peer");
            check = LicasaDiagnostics::makeMotionHdrJourneyCheck(
                imageUrl, peer.isEmpty() ? QUrl() : QUrl::fromLocalFile(QFileInfo(peer).absoluteFilePath()),
                &windows, &policy.processingMutex(), motionJourneySeed,
                QFileInfo(parser.value("motion-journey-output")).absoluteFilePath(),
                parser.isSet("motion-soak-rounds") ? soakRounds : 0);
        } else if (replacement) {
            // The windows view a scratch copy that the check replaces.
            QUrl scratch;
            check = LicasaDiagnostics::makeReplacementHdrCheck(imageUrl, &windows, replacementSeed, &scratch);
            imageUrl = scratch;
        } else if (outputMigration) {
            check = LicasaDiagnostics::makeOutputMigrationCheck(imageUrl, &windows, outputSeed, outputCycles);
        } else if (multiwindow) {
            check = LicasaDiagnostics::makeMultiwindowHdrCheck(imageUrl, &windows, multiwindowSeed,
                                                               multiwindowActions);
        } else {
            check = LicasaDiagnostics::makeWaylandHdrCheck(imageUrl, expectedOutput,
                                                       parser.isSet("wayland-window-churn"),
                                                       parser.value("wayland-window-seed"));
        }
#endif
    } else {
        check = LicasaDiagnostics::makeViewerMeasurement(started, imageUrl, cycles);
    }
    if (!windows.createWindow(imageUrl, false, true)) {
        return 2;
    }
    for (auto* window : QGuiApplication::topLevelWindows()) {
        if (auto* quick = qobject_cast<QQuickWindow*>(window);
            quick && quick->property("windowSession").isValid()) {
            const bool attached = check->attach(quick);
            if (!attached) {
                return 2;
            }
            const int exitCode = application.exec();
            decodeProvider->waitForPendingWork();
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
            if (hdrCheck) {
                // Stop rendering, then deliver queued QML signal proxies while
                // their targets still exist. Qt's proxy retains itself if its
                // target is deleted before the queued MetaCall is dispatched.
                for (auto* ownedWindow : QGuiApplication::topLevelWindows()) {
                    if (auto* ownedQuick = qobject_cast<QQuickWindow*>(ownedWindow)) {
                        ownedQuick->hide();
                        ownedQuick->releaseResources();
                    }
                }
                QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
                windows.destroyAllWindows();
                engine.collectGarbage();
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            }
#endif
            return exitCode;
        }
    }
    return 2;
}
