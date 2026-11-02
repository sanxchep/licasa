#include "imaging/image_probe.h"
#include "imaging/image_decode_contract.h"
#include "imaging/image_resource_policy.h"
#ifdef LICASA_HAVE_FAST_PNG_PREVIEW
#include "imaging/fast_png_preview.h"
#include <zlib.h>
#endif
#ifdef LICASA_HAVE_FAST_WEBP_PREVIEW
#include "imaging/fast_webp_preview.h"
#endif
#include <QBuffer>
#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QXmlStreamReader>
#include <algorithm>

namespace {
QSize svgViewportFromHeader(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    // Size discovery must not parse/decode a large embedded raster. This is
    // only a viewport hint; the normal reader still validates the whole image.
    constexpr int headerLimit = 64 * 1024;
    QByteArray header;
    if (file.peek(2) == QByteArray::fromHex("1f8b")) {
#ifdef LICASA_HAVE_FAST_PNG_PREVIEW
        const QByteArray compressed = file.read(headerLimit);
        z_stream stream{};
        if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
            return {};
        }
        header.resize(headerLimit);
        stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.constData()));
        stream.avail_in = uInt(compressed.size());
        stream.next_out = reinterpret_cast<Bytef*>(header.data());
        stream.avail_out = uInt(header.size());
        const int result = inflate(&stream, Z_NO_FLUSH);
        const int bytes = int(stream.total_out);
        inflateEnd(&stream);
        if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR) {
            return {};
        }
        header.truncate(bytes);
#else
        return {};
#endif
    } else {
        header = file.read(headerLimit);
    }
    QXmlStreamReader xml(header);
    if (!xml.readNextStartElement() || xml.name() != QStringLiteral("svg") ||
        xml.namespaceUri() != QStringLiteral("http://www.w3.org/2000/svg") ||
        xml.attributes().hasAttribute(QStringLiteral("style"))) {
        return {};
    }
    const auto dimension = [&](const char* name) {
        const QString value = xml.attributes().value(QLatin1String(name)).toString();
        if (value.isEmpty() || value.size() > 10 ||
            std::any_of(value.cbegin(), value.cend(),
                        [](QChar c) { return c < QLatin1Char('0') || c > QLatin1Char('9'); })) {
            return 0;
        }
        bool valid = false;
        const int number = value.toInt(&valid);
        return valid && number > 0 ? number : 0;
    };
    const QSize viewport(dimension("width"), dimension("height"));
    if (viewport.isEmpty()) {
        return {};
    }
    // Let the installed Qt plugin determine its size convention and retain
    // normal unavailable-plugin behavior, using a tiny body-free document.
    QByteArray minimal = "<svg xmlns='http://www.w3.org/2000/svg' width='" +
                         QByteArray::number(viewport.width()) + "' height='" +
                         QByteArray::number(viewport.height()) + "'/>";
    QBuffer buffer(&minimal);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "svg");
    return reader.size();
}
} // namespace

namespace Licasa {
ImageProbe::ImageProbe(ImageResourcePolicy& resourcePolicy, QObject* parent)
    : QObject(parent), resourcePolicy_(resourcePolicy)
{}

QVariantMap ImageProbe::inspect(const QUrl& url) const
{
    const bool trace = qEnvironmentVariableIsSet("LICASA_TRACE_IMAGE_PROBE");
    QElapsedTimer elapsed;
    if (trace) {
        elapsed.start();
    }
    if (!url.isValid() || !url.isLocalFile()) {
        return {};
    }

    const QFileInfo fileInfo(url.toLocalFile());
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return {};
    }

    const quint64 budget = resourcePolicy_.maximumImagePixels();
    const qint64 modified = fileInfo.lastModified().toMSecsSinceEpoch();
    if (lastProbePath_ == fileInfo.absoluteFilePath() && lastProbeBytes_ == fileInfo.size() &&
        lastProbeModified_ == modified && lastProbeBudget_ == budget) {
        if (trace) {
            qInfo().noquote() << "[Licasa] Image metadata cache hit:"
                              << elapsed.nsecsElapsed() / 1000000.0 << "ms";
        }
        return lastProbe_;
    }
    lastProbePath_ = fileInfo.absoluteFilePath();
    lastProbeBytes_ = fileInfo.size();
    lastProbeModified_ = modified;
    lastProbeBudget_ = budget;
    lastProbe_ = {{QStringLiteral("fileSize"), fileInfo.size()}};

    const QString suffix = fileInfo.suffix().toLower();
    if (suffix == QStringLiteral("svg") || suffix == QStringLiteral("svgz")) {
        const QSize viewport = svgViewportFromHeader(fileInfo.absoluteFilePath());
        if (viewport.isValid() && !viewport.isEmpty()) {
            lastProbe_.insert(QStringLiteral("size"), viewport);
            lastProbe_.insert(QStringLiteral("withinBudget"), resourcePolicy_.allows(viewport));
            lastProbe_.insert(QStringLiteral("animated"), false);
            lastProbe_.insert(QStringLiteral("previewFirst"), false);
            lastProbe_.insert(QStringLiteral("previewSize"), QSize());
            lastProbe_.insert(QStringLiteral("reusablePngBase"), false);
            if (trace) {
                qInfo().noquote() << "[Licasa] Bounded SVG viewport probe:"
                                  << elapsed.nsecsElapsed() / 1000000.0 << "ms";
            }
            return lastProbe_;
        }
    }

    QImageReader reader(fileInfo.absoluteFilePath());
    ImageDecodeContract::configure(reader, budget);
    reader.setAutoTransform(true);
    if (!reader.canRead()) {
        if (trace) {
            qInfo().noquote() << "[Licasa] Image metadata probe (unreadable):"
                              << elapsed.nsecsElapsed() / 1000000.0 << "ms";
        }
        return lastProbe_;
    }

    QSize size = reader.size();
    if (!size.isValid()) {
        if (trace) {
            qInfo().noquote() << "[Licasa] Image metadata probe (unknown dimensions):"
                              << elapsed.nsecsElapsed() / 1000000.0 << "ms";
        }
        return lastProbe_;
    }

    const auto transformation = reader.transformation();
    if (transformation.testFlag(QImageIOHandler::TransformationRotate90) ||
        transformation.testFlag(QImageIOHandler::TransformationRotate270)) {
        size.transpose();
    }
    const QSize sensor =
        reader.device()->property(ImageDecodeContract::sensorSizeProperty).toSize();
    const bool plainPng =
        fileInfo.suffix().compare(QStringLiteral("png"), Qt::CaseInsensitive) == 0 &&
        reader.transformation() == QImageIOHandler::TransformationNone &&
        (!reader.supportsAnimation() || reader.imageCount() == 1);
    bool reusablePngBase = plainPng && ImageDecodeContract::reusablePngFullBase(
                                           reader.size(), reader.imageFormat(), budget);
#ifdef LICASA_HAVE_FAST_PNG_PREVIEW
    if (plainPng && !reusablePngBase &&
        ImageDecodeContract::boundedRasterFullBase(reader.size(), budget)) {
        reusablePngBase = fastPngBaseEligible(fileInfo.absoluteFilePath(), budget);
    }
#endif
    bool reusableWebpBase = false;
#ifdef LICASA_HAVE_FAST_WEBP_PREVIEW
    reusableWebpBase =
        fileInfo.suffix().compare(QStringLiteral("webp"), Qt::CaseInsensitive) == 0 &&
        reader.transformation() == QImageIOHandler::TransformationNone &&
        ImageDecodeContract::boundedRasterFullBase(reader.size(), budget, 400ULL * 1024 * 1024) &&
        fastWebpBaseEligible(fileInfo.absoluteFilePath());
#endif
    lastProbe_ = {
        {QStringLiteral("reusableWebpBase"), reusableWebpBase},
        {QStringLiteral("fileSize"), fileInfo.size()},
        {QStringLiteral("size"), size},
        {QStringLiteral("withinBudget"),
         resourcePolicy_.allows(size) && (!sensor.isValid() || resourcePolicy_.allows(sensor))},
        {QStringLiteral("animated"), reader.supportsAnimation() && reader.imageCount() != 1},
        {QStringLiteral("previewFirst"),
         reader.device()->property(ImageDecodeContract::previewFirstProperty).toBool()},
        {QStringLiteral("previewSize"),
         reader.device()->property(ImageDecodeContract::embeddedPreviewSizeProperty).toSize()},
        {QStringLiteral("reusablePngBase"), reusablePngBase}};
    if (trace) {
        qInfo().noquote() << "[Licasa] Image metadata probe:" << elapsed.nsecsElapsed() / 1000000.0
                          << "ms";
    }
    return lastProbe_;
}

} // namespace Licasa
