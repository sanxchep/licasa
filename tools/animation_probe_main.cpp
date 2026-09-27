// AVIF_STAGE4_PACKAGE_ANIMATION_V1
// APNG_STAGE3_PACKAGE_AUTOMATION_V1
// HEIF_SEQUENCE_STAGE3_PACKAGE_AUTOMATION_V1
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringList>

namespace {

constexpr int maximumProbeFrames = 100000;

struct StreamDiscovery {
    bool ok = false;
    int count = 0;
    bool canReadAfterEos = false;
    QString error;
};

StreamDiscovery discoverStreamingFrameCount(QImageReader& reader)
{
    StreamDiscovery result;
    if (!reader.jumpToImage(0)) {
        result.error = reader.errorString();
        return result;
    }

    for (int decoded = 0; decoded < maximumProbeFrames; ++decoded) {
        const QImage image = reader.read();
        if (image.isNull()) {
            result.count = reader.imageCount();
            result.canReadAfterEos = reader.canRead();
            result.ok = result.count >= 2 && result.count == decoded;
            if (!result.ok) {
                result.error = reader.errorString();
            }
            return result;
        }
        if (reader.currentImageNumber() != decoded) {
            result.error =
                QStringLiteral("Streaming animation frame numbering changed unexpectedly.");
            return result;
        }

        if (!reader.jumpToNextImage()) {
            result.count = reader.imageCount();
            result.canReadAfterEos = reader.canRead();
            result.ok = result.count >= 2 && result.count == decoded + 1;
            if (!result.ok) {
                result.error = reader.errorString();
            }
            return result;
        }
    }

    result.error = QStringLiteral("Streaming animation exceeded the 100000-frame probe limit.");
    return result;
}

QJsonObject frameResult(QImageReader& reader, int requested)
{
    QJsonObject result;
    result["requested"] = requested;

    if (!reader.jumpToImage(requested)) {
        result["jumped"] = false;
        result["decoded"] = false;
        result["error"] = reader.errorString();
        return result;
    }

    result["jumped"] = true;
    const QImage image = reader.read();
    result["decoded"] = !image.isNull();
    result["current"] = reader.currentImageNumber();
    result["width"] = image.width();
    result["height"] = image.height();
    result["delay_ms"] = reader.nextImageDelay();
    result["error"] = reader.errorString();
    return result;
}

QJsonArray mappedCodecLibraries()
{
    QJsonArray output;
#ifdef Q_OS_LINUX
    QFile maps(QStringLiteral("/proc/self/maps"));
    if (!maps.open(QIODevice::ReadOnly)) {
        return output;
    }

    QSet<QString> found;
    for (;;) {
        const QByteArray line = maps.readLine();
        if (line.isEmpty()) {
            break;
        }

        const int slash = line.indexOf('/');
        if (slash < 0) {
            continue;
        }

        QString path = QString::fromUtf8(line.mid(slash)).trimmed();
        if (!(path.contains(QStringLiteral("libavif")) ||
              path.contains(QStringLiteral("libdav1d")) ||
              path.contains(QStringLiteral("libyuv")) ||
              path.contains(QStringLiteral("licasa_avif")) ||
              path.contains(QStringLiteral("licasa_apng")) ||
              path.contains(QStringLiteral("libheif")) ||
              path.contains(QStringLiteral("libde265")) ||
              path.contains(QStringLiteral("licasa_heif")))) {
            continue;
        }

        const QString deleted = QStringLiteral(" (deleted)");
        if (path.endsWith(deleted)) {
            path.chop(deleted.size());
        }

        const QFileInfo info(path);
        const QString canonical = info.canonicalFilePath();
        found.insert(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
    }

    QStringList sorted = found.values();
    sorted.sort();
    for (const QString& path : sorted) {
        output.append(path);
    }
#endif
    return output;
}

void printJson(const QJsonObject& object)
{
    const QByteArray json = QJsonDocument(object).toJson(QJsonDocument::Compact);
    fwrite(json.constData(), 1, size_t(json.size()), stdout);
    fwrite("\n", 1, 1, stdout);
    fflush(stdout);
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);

    const QString appDir = QCoreApplication::applicationDirPath();
    QCoreApplication::addLibraryPath(QDir(appDir).absoluteFilePath(QStringLiteral("plugins")));
    QCoreApplication::addLibraryPath(
        QDir(appDir).absoluteFilePath(QStringLiteral("../lib/licasa/plugins")));

    QJsonObject report;
    report["probe"] = QStringLiteral("licasa_animation_probe");
    report["qt_version"] = QString::fromLatin1(qVersion());

    if (application.arguments().size() < 2 || application.arguments().size() > 3) {
        report["ok"] = false;
        report["error"] =
            QStringLiteral("Usage: licasa_animation_probe <animated-image> [forced-format]");
        printJson(report);
        return 2;
    }

    const QString path = QFileInfo(application.arguments().at(1)).absoluteFilePath();
    report["path"] = path;
    const QByteArray forcedFormat = application.arguments().size() == 3
                                        ? application.arguments().at(2).toLatin1()
                                        : QByteArrayLiteral("avif");
    report["forced_format"] = QString::fromLatin1(forcedFormat);

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        report["ok"] = false;
        report["error"] = file.errorString();
        printJson(report);
        return 3;
    }

    QImageReader reader(&file, forcedFormat);
    report["can_read"] = reader.canRead();
    report["format"] = QString::fromLatin1(reader.format());
    report["animation"] = reader.supportsAnimation();
    const int initialCount = reader.imageCount();
    report["frames_initial"] = initialCount;
    report["frames"] = initialCount;
    report["streaming_count_discovery"] = false;
    report["loop_count"] = reader.loopCount();

    const QSize declared = reader.size();
    report["declared_width"] = declared.width();
    report["declared_height"] = declared.height();

    QJsonArray readFormats;
    for (const QByteArray& format : QImageReader::supportedImageFormats()) {
        readFormats.append(QString::fromLatin1(format));
    }
    report["read_formats"] = readFormats;

    QJsonArray readMimes;
    for (const QByteArray& mime : QImageReader::supportedMimeTypes()) {
        readMimes.append(QString::fromLatin1(mime));
    }
    report["read_mime_types"] = readMimes;

    if (!reader.canRead() || !reader.supportsAnimation()) {
        report["ok"] = false;
        report["error"] = reader.errorString();
        printJson(report);
        return 4;
    }

    int count = initialCount;
    if (count < 2) {
        const StreamDiscovery discovery = discoverStreamingFrameCount(reader);
        report["streaming_count_discovery"] = true;
        report["can_read_after_eos"] = discovery.canReadAfterEos;
        report["frames"] = discovery.count;
        if (!discovery.ok) {
            report["ok"] = false;
            report["error"] = discovery.error;
            printJson(report);
            return 4;
        }
        count = discovery.count;
    }

    const int middle = count / 2;

    QJsonArray frames;
    frames.append(frameResult(reader, 0));
    frames.append(frameResult(reader, middle));
    frames.append(frameResult(reader, count - 1));
    report["frame_checks"] = frames;

    report["can_read_after_last"] = reader.canRead();
    const QJsonObject backToZero = frameResult(reader, 0);
    report["seek_back_to_zero"] = backToZero;

    report["native_raster_pixels"] =
        QString::number(file.property("_licasaNativeRasterPixels").toULongLong());
    report["decoder_backend"] = reader.text(QStringLiteral("Backend"));
    report["preview_path"] = reader.text(QStringLiteral("PreviewPath"));
    report["mapped_codec_libraries"] = mappedCodecLibraries();

    bool framesOk = true;
    for (const QJsonValue& value : frames) {
        const QJsonObject frame = value.toObject();
        framesOk = framesOk && frame.value("jumped").toBool() && frame.value("decoded").toBool() &&
                   frame.value("current").toInt(-1) == frame.value("requested").toInt(-2) &&
                   frame.value("width").toInt() > 0 && frame.value("height").toInt() > 0 &&
                   frame.value("delay_ms").toInt() >= 10 &&
                   frame.value("delay_ms").toInt() <= 60000;
    }

    const bool backOk = backToZero.value("jumped").toBool() &&
                        backToZero.value("decoded").toBool() &&
                        backToZero.value("current").toInt(-1) == 0;

    report["ok"] = framesOk && backOk && report.value("animation").toBool() &&
                   report.value("frames").toInt() >= 2;

    printJson(report);
    return report.value("ok").toBool() ? 0 : 5;
}
