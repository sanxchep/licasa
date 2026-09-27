#include "imaging/image_processing.h"
#include "app/app_constants.h"
#include "imaging/image_decode_contract.h"
#include <QImageReader>
#include <algorithm>

namespace Licasa {
namespace ImageProcessing {
quint64 pixelsForMegapixels(int megapixels)
{
    return quint64(std::clamp(megapixels, Constants::minimumImageLimitMegapixels,
                              Constants::maximumImageLimitMegapixels)) *
           1'000'000;
}

QString imageLimitError(int maximumMegapixels)
{
    return QStringLiteral(
               "The image exceeds the effective %1 MP full-resolution limit of the per-image "
               "memory budget. Increase Maximum working memory per image in Advanced Settings "
               "to process it at full detail.")
        .arg(maximumMegapixels);
}

QString readerErrorOrFallback(const QImageReader& reader)
{
    if (reader.device()) {
        const QString backendError =
            reader.device()->property(ImageDecodeContract::errorProperty).toString();
        if (!backendError.isEmpty()) {
            return backendError;
        }
    }
    const QString error = reader.errorString().trimmed();
    return error.isEmpty() ? QStringLiteral("The image could not be decoded.") : error;
}

} // namespace ImageProcessing

} // namespace Licasa
