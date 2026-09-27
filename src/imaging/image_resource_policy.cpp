#include "imaging/image_resource_policy.h"
#include "app/app_constants.h"
#include "app/application_settings.h"
#include "imaging/image_decode_contract.h"
#include "imaging/image_processing.h"
#include <QImageReader>
#include <QThread>
#include <algorithm>

namespace Licasa {
namespace {
constexpr quint64 kBytesPerMebibyte = 1024 * 1024;

int boundedMaximumImageMegapixels(int value)
{
    return std::clamp(value, Constants::minimumImageLimitMegapixels,
                      Constants::maximumImageLimitMegapixels);
}

int allocationLimitMiBForMegapixels(int megapixels)
{
    const quint64 bytes = ImageProcessing::pixelsForMegapixels(megapixels) *
                          static_cast<quint64>(Constants::decoderBudgetBytesPerPixel);
    return static_cast<int>((bytes + kBytesPerMebibyte - 1) / kBytesPerMebibyte);
}

} // namespace

void ImageResourcePolicy::initializeDecoderEnvironment()
{
    // This is deliberately done before Qt and worker initialization: changing
    // process environment variables once other threads run is not safe.
    qunsetenv("QT_IMAGEIO_MAXALLOC");

    // LibRaw's unpack step is format-dependent and can remain serial, but its
    // expensive demosaic/post-processing path can use OpenMP. Respect an
    // explicit user override; otherwise let RAW development use every logical
    // CPU available while the Qt GUI remains on its own event-loop thread.
    if (qEnvironmentVariableIsEmpty("OMP_NUM_THREADS")) {
        qputenv("OMP_NUM_THREADS", QByteArray::number(std::max(1, QThread::idealThreadCount())));
    }
    if (qEnvironmentVariableIsEmpty("OMP_DYNAMIC")) {
        qputenv("OMP_DYNAMIC", QByteArrayLiteral("FALSE"));
    }
}

ImageResourcePolicy::ImageResourcePolicy(ViewerPreferences& preferences, QObject* parent)
    : QObject(parent),
      maximumImageMegapixels_(boundedMaximumImageMegapixels(preferences.maximumImageMegapixels()))
{
    allocationUpdateTimer_.setInterval(10);
    connect(&allocationUpdateTimer_, &QTimer::timeout, this,
            &ImageResourcePolicy::tryUpdateDecoderAllocationLimit);
    applyMaximumImageMegapixels(preferences.maximumImageMegapixels());
    connect(&preferences, &ViewerPreferences::maximumImageMegapixelsChanged, this,
            [this, &preferences]() {
                applyMaximumImageMegapixels(preferences.maximumImageMegapixels());
            });
}

int ImageResourcePolicy::maximumImageMegapixels() const noexcept
{
    return maximumImageMegapixels_.load(std::memory_order_acquire);
}

quint64 ImageResourcePolicy::maximumImagePixels() const noexcept
{
    return ImageProcessing::pixelsForMegapixels(maximumImageMegapixels());
}

int ImageResourcePolicy::decoderAllocationLimitMiB() const noexcept
{
    return allocationLimitMiBForMegapixels(maximumImageMegapixels());
}

bool ImageResourcePolicy::allows(const QSize& size) const noexcept
{
    return ImageDecodeContract::allows(size, maximumImagePixels());
}

QMutex& ImageResourcePolicy::processingMutex() noexcept { return processingMutex_; }

void ImageResourcePolicy::applyMaximumImageMegapixels(int value)
{
    const int boundedValue = boundedMaximumImageMegapixels(value);
    maximumImageMegapixels_.store(boundedValue, std::memory_order_release);
    tryUpdateDecoderAllocationLimit();
    qInfo().noquote()
        << QStringLiteral(
               "[Licasa] Full-resolution image limit: %1 MP; next-operation decoder cap: %2 MiB")
               .arg(boundedValue)
               .arg(allocationLimitMiBForMegapixels(boundedValue));
}

int ImageResourcePolicy::prepareForProcessing()
{
    const int megapixels = maximumImageMegapixels();
    const int cap = allocationLimitMiBForMegapixels(megapixels);
    if (QImageReader::allocationLimit() != cap) {
        QImageReader::setAllocationLimit(cap);
    }
    return megapixels;
}

void ImageResourcePolicy::tryUpdateDecoderAllocationLimit()
{
    // Never wait on a non-interruptible codec from the settings/UI thread.
    // Workers synchronize the cap themselves before admitting their next job;
    // this timer also updates it if no further work arrives.
    if (!processingMutex_.tryLock()) {
        if (!allocationUpdateTimer_.isActive()) {
            allocationUpdateTimer_.start();
        }
        return;
    }
    prepareForProcessing();
    processingMutex_.unlock();
    allocationUpdateTimer_.stop();
}

} // namespace Licasa
