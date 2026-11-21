#pragma once
#include <QImage>
#include <QString>

namespace GainmapPhotoProbe {
// Mutate the exclusively owned decoded raster without allocating another image.
bool prepareUpload(QImage& upload, QString& sourcePrimaries, QString& error);
} // namespace GainmapPhotoProbe
