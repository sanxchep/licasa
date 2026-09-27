#pragma once

#include "imaging/image_edit_pipeline.h"

#include <array>
#include <cstddef>
#include <memory>

namespace Licasa {

// Private boundary: drivers receive an admitted, contiguous ARGB32 raster.
// Backends may retain up to two 256 MiB staging rasters for interactive edits.
// A stable source identity lets repeated single-stage edits keep the immutable
// base raster resident and skip host-to-device upload. Combined color+sharpen
// invalidates that residency because the second pass reuses the base buffer.
using EditUniforms = std::array<double, 10>;

class EditComputeDevice {
  public:
    virtual ~EditComputeDevice() = default;
    virtual bool run(const QImage& source, QImage& destination, quint64 sourceIdentity,
                     const EditUniforms& values, bool color, bool sharpen,
                     const std::atomic_bool* cancelled) = 0;
    virtual void releaseImageBuffers() = 0;
    virtual size_t retainedImageBytes() const = 0;
    QString description;
};

std::unique_ptr<EditComputeDevice> createOpenClEditDevice(QString& error);
std::unique_ptr<EditComputeDevice> createCudaEditDevice(QString& error);

bool warmGpuImageEdits(ImageEditExecution& execution);
void releaseGpuImageEditBuffers();
size_t retainedGpuImageEditBytes();

bool tryGpuImageEdits(QImage& image, const ImageEditParameters& parameters, bool color,
                      bool sharpen, const std::atomic_bool* cancelled,
                      ImageEditExecution& execution);

} // namespace Licasa
