#include "imaging/compute/edit_compute.h"

#include <QColorSpace>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <cmath>
#include <utility>

namespace Licasa {
namespace {

struct DeviceSlot {
    bool attempted = false;
    bool warmed = false;
    std::unique_ptr<EditComputeDevice> device;
    QString error;
};

struct ComputeState {
    QMutex mutex;
    DeviceSlot opencl;
    DeviceSlot cuda;
};

ComputeState& computeState()
{
    static ComputeState state;
    return state;
}

void prepareDevice(DeviceSlot& slot, bool cuda)
{
    if (!slot.attempted) {
        slot.attempted = true;
        slot.device = cuda ? createCudaEditDevice(slot.error) : createOpenClEditDevice(slot.error);
    }
}

ImageEditBackend configuredBackend(ImageEditBackend requested)
{
    if (requested != ImageEditBackend::Automatic) {
        return requested;
    }

    const QByteArray backend = qgetenv("LICASA_EDIT_BACKEND");
    if (backend == "cpu") {
        return ImageEditBackend::Cpu;
    }
    if (backend == "opencl") {
        return ImageEditBackend::OpenCl;
    }
    if (backend == "cuda") {
        return ImageEditBackend::Cuda;
    }
    return ImageEditBackend::Automatic;
}

EditComputeDevice* selectDevice(ComputeState& state, ImageEditExecution& execution, bool& cuda)
{
    execution.requested = configuredBackend(execution.requested);
    if (execution.requested == ImageEditBackend::Cpu) {
        return nullptr;
    }

    cuda = execution.requested == ImageEditBackend::Cuda;
    if (execution.requested == ImageEditBackend::Automatic) {
        prepareDevice(state.cuda, true);
        cuda = bool(state.cuda.device);
    }

    DeviceSlot& slot = cuda ? state.cuda : state.opencl;
    prepareDevice(slot, cuda);
    if (!slot.device) {
        execution.detail = slot.error;
        return nullptr;
    }
    return slot.device.get();
}

} // namespace

#ifndef LICASA_HAVE_OPENCL_EDITS
std::unique_ptr<EditComputeDevice> createOpenClEditDevice(QString& error)
{
    error = QStringLiteral("OpenCL support was not built (headers unavailable)");
    return {};
}
#endif

#ifndef LICASA_HAVE_CUDA_EDITS
std::unique_ptr<EditComputeDevice> createCudaEditDevice(QString& error)
{
    error = QStringLiteral("CUDA support was not built (headers unavailable)");
    return {};
}
#endif

bool warmGpuImageEdits(ImageEditExecution& execution)
{
    execution.used = ImageEditBackend::Cpu;
    execution.detail = QStringLiteral("CPU");
    execution.requested = configuredBackend(execution.requested);
    if (execution.requested == ImageEditBackend::Cpu) {
        return false;
    }

    ComputeState& state = computeState();
    QMutexLocker lock(&state.mutex);
    bool cuda = false;
    EditComputeDevice* device = selectDevice(state, execution, cuda);
    if (!device) {
        return false;
    }
    DeviceSlot& slot = cuda ? state.cuda : state.opencl;
    if (slot.warmed) {
        execution.used = cuda ? ImageEditBackend::Cuda : ImageEditBackend::OpenCl;
        execution.detail = device->description;
        return true;
    }

    // Run one tiny identity color pass so context creation, PTX/OpenCL program
    // compilation, the first kernel launch, and driver-side JIT all happen when
    // the editor opens instead of on the user's first slider movement.
    QImage source(16, 16, QImage::Format_ARGB32);
    source.fill(qRgba(64, 96, 128, 255));
    QImage result = source.copy();
    const EditUniforms identity{1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    if (!device->run(source, result, quint64(source.cacheKey()), identity, true, false, nullptr)) {
        execution.detail = QStringLiteral("GPU warm-up failed; CPU fallback remains available");
        return false;
    }

    // Warm the driver and kernels, not a reusable raster containing the tiny
    // warm-up image. Real edits allocate their own appropriately sized buffers.
    device->releaseImageBuffers();
    slot.warmed = true;
    execution.used = cuda ? ImageEditBackend::Cuda : ImageEditBackend::OpenCl;
    execution.detail = device->description;
    return true;
}

void releaseGpuImageEditBuffers()
{
    ComputeState& state = computeState();
    QMutexLocker lock(&state.mutex);
    if (state.cuda.device) {
        state.cuda.device->releaseImageBuffers();
    }
    if (state.opencl.device) {
        state.opencl.device->releaseImageBuffers();
    }
}

size_t retainedGpuImageEditBytes()
{
    ComputeState& state = computeState();
    QMutexLocker lock(&state.mutex);
    const size_t cudaBytes = state.cuda.device ? state.cuda.device->retainedImageBytes() : 0;
    const size_t openclBytes = state.opencl.device ? state.opencl.device->retainedImageBytes() : 0;
    return cudaBytes + openclBytes;
}

bool tryGpuImageEdits(QImage& image, const ImageEditParameters& parameters, bool color,
                      bool sharpen, const std::atomic_bool* cancelled,
                      ImageEditExecution& execution)
{
    execution.requested = configuredBackend(execution.requested);
    if ((!color && !sharpen) || execution.requested == ImageEditBackend::Cpu) {
        return false;
    }
    // Bound temporary device memory to two 256 MiB rasters, independent of the
    // decoder's larger user-configurable admission limit. Oversize work stays CPU.
    const qint64 pixels = qint64(image.width()) * image.height();
    if (pixels > 64 * 1024 * 1024) {
        execution.detail = QStringLiteral("CPU: edit exceeds the 512 MiB device-buffer budget");
        return false;
    }
    // Interactive RAW previews are commonly around 0.5-1 MP. On the reference
    // RTX 4070, warmed end-to-end GPU requests at that size are 8-12x faster
    // than the scalar CPU path, including upload and readback.
    if (execution.requested == ImageEditBackend::Automatic && pixels < 256 * 1024) {
        execution.detail = QStringLiteral("CPU: small image avoids GPU setup and transfers");
        return false;
    }
    ComputeState& state = computeState();
    QMutexLocker lock(&state.mutex);
    if (cancelled && cancelled->load(std::memory_order_relaxed)) {
        return false;
    }
    bool cuda = false;
    EditComputeDevice* device = selectDevice(state, execution, cuda);
    if (!device) {
        return false;
    }

    // Capture the immutable decoded-base identity before format conversion.
    // RAW bases are commonly RGB888, and each ARGB32 conversion gets a fresh
    // QImage identity even when the underlying decoded source is unchanged.
    const quint64 sourceIdentity = quint64(image.cacheKey());
    QImage source = image.convertToFormat(QImage::Format_ARGB32);
    if (source.bytesPerLine() != source.width() * qsizetype(sizeof(QRgb))) {
        source = source.copy();
    }
    // The GPU overwrites every output pixel, so copying the full ARGB32 source
    // into the destination only burns host memory bandwidth. Allocate an empty
    // raster and preserve the small QImage metadata instead. Keep the old path
    // available for qualification A/B runs.
    QImage result;
    if (qEnvironmentVariableIsSet("LICASA_GPU_RESULT_COPY_LEGACY")) {
        result = source.copy();
    } else {
        result = QImage(source.size(), source.format());
        if (!result.isNull()) {
            result.setColorSpace(source.colorSpace());
            result.setDevicePixelRatio(source.devicePixelRatio());
            result.setDotsPerMeterX(source.dotsPerMeterX());
            result.setDotsPerMeterY(source.dotsPerMeterY());
            result.setOffset(source.offset());
            for (const QString& key : source.textKeys()) {
                result.setText(key, source.text(key));
            }
        }
    }
    if (source.isNull() || result.isNull()) {
        execution.detail = QStringLiteral("GPU staging allocation failed; retrying on CPU");
        return false;
    }
    const EditUniforms values{std::pow(2.0, parameters.exposure),
                              std::max(0.0, 1.0 + parameters.contrast),
                              parameters.highlights,
                              parameters.shadows,
                              parameters.saturation,
                              parameters.vibrance,
                              parameters.warmth * 42.0,
                              parameters.tint * 36.0,
                              parameters.vignette,
                              parameters.sharpen * 0.48};
    if (!device->run(source, result, sourceIdentity, values, color, sharpen, cancelled)) {
        execution.detail = QStringLiteral("GPU request failed or was cancelled; CPU fallback");
        return false;
    }
    image = std::move(result);
    execution.used = cuda ? ImageEditBackend::Cuda : ImageEditBackend::OpenCl;
    execution.detail = device->description;
    return true;
}

} // namespace Licasa
