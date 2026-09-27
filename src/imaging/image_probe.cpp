#include "imaging/image_probe.h"
#include "imaging/image_decode_contract.h"
#include "imaging/image_resource_policy.h"
#include <QDateTime>
#include <QFileInfo>
#include <QImageReader>

namespace Licasa {
ImageProbe::ImageProbe(ImageResourcePolicy& resourcePolicy, QObject* parent)
    : QObject(parent), resourcePolicy_(resourcePolicy)
{}

QVariantMap ImageProbe::inspect(const QUrl& url) const
{
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
        return lastProbe_;
    }
    lastProbePath_ = fileInfo.absoluteFilePath();
    lastProbeBytes_ = fileInfo.size();
    lastProbeModified_ = modified;
    lastProbeBudget_ = budget;
    lastProbe_ = {{QStringLiteral("fileSize"), fileInfo.size()}};

    QImageReader reader(fileInfo.absoluteFilePath());
    ImageDecodeContract::configure(reader, budget);
    reader.setAutoTransform(true);
    if (!reader.canRead()) {
        return lastProbe_;
    }

    QSize size = reader.size();
    if (!size.isValid()) {
        return lastProbe_;
    }

    const auto transformation = reader.transformation();
    if (transformation.testFlag(QImageIOHandler::TransformationRotate90) ||
        transformation.testFlag(QImageIOHandler::TransformationRotate270)) {
        size.transpose();
    }
    const QSize sensor =
        reader.device()->property(ImageDecodeContract::sensorSizeProperty).toSize();
    lastProbe_ = {
        {QStringLiteral("fileSize"), fileInfo.size()},
        {QStringLiteral("size"), size},
        {QStringLiteral("withinBudget"),
         resourcePolicy_.allows(size) && (!sensor.isValid() || resourcePolicy_.allows(sensor))},
        {QStringLiteral("animated"), reader.supportsAnimation() && reader.imageCount() != 1},
        {QStringLiteral("previewFirst"),
         reader.device()->property(ImageDecodeContract::previewFirstProperty).toBool()},
        {QStringLiteral("previewSize"),
         reader.device()->property(ImageDecodeContract::embeddedPreviewSizeProperty).toSize()}};
    return lastProbe_;
}

} // namespace Licasa
