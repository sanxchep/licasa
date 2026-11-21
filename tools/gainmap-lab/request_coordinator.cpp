#include "request_coordinator.h"
#include <cmath>

namespace Licasa::GainmapLab {
RequestCoordinator::RequestCoordinator() : RequestCoordinator(Budgets{}) {}
RequestCoordinator::RequestCoordinator(Budgets budgets)
    : RequestCoordinator(std::make_shared<ResourceBudget>(ResourceBudget::Bytes{
          budgets.decode, budgets.retained, budgets.upload})) {}
RequestCoordinator::RequestCoordinator(std::shared_ptr<ResourceBudget> pool) : pool_(std::move(pool))
{ Q_ASSERT(pool_); }

void RequestCoordinator::cancel()
{
    ++generation_;
    if (active_) active_->cancelled->store(true);
    raster_.reset();
    retainedSource_.reset();
    upload_.reset();
    // A codec call cannot be interrupted inside libultrahdr. Keep its allowance.
}

std::optional<RequestCoordinator::Request> RequestCoordinator::request(
    const QString& path, const ExternalFileIdentity& identity, float headroom, Limits limits,
    bool deferRasterGrowth)
{
    cancel();
    // Match the decoder's hard ceilings before doing arithmetic. The decode
    // allowance includes two compressed copies and a conservative 64 bytes per
    // combined pixel for codec/intermediate/output rasters; it is not an RSS cap.
    if (active_ || !std::isfinite(headroom) || headroom < 1 ||
        !limits.rasterPixels || limits.rasterPixels > maximumRasterPixels ||
        !limits.compressedBytes || limits.compressedBytes > Limits{}.compressedBytes)
        return std::nullopt;
    Limits initial = limits;
    if (deferRasterGrowth) initial.rasterPixels = qMin(initial.rasterPixels, Limits{}.rasterPixels);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || externalFileIdentity(file) != identity)
        return std::nullopt;
    auto reservation = pool_->acquire(decodeAllowance(initial));
    if (!reservation) return std::nullopt;
    active_ = Request{generation_, path, identity, headroom, limits,
                      std::make_shared<std::atomic_bool>(false), std::move(reservation),
                      initial.rasterPixels};
    return active_;
}

bool RequestCoordinator::Request::reserveRaster(quint64 pixels) const
{
    if (cancelled->load() || pixels > limits.rasterPixels) return false;
    if (pixels <= reservedRasterPixels) return true;
    const auto extra = decodeAllowance({0, pixels - reservedRasterPixels});
    return reservation->growCpu(extra.decode, extra.retained);
}

bool RequestCoordinator::complete(const Request& request, std::optional<Result> result)
{
    // A duplicate or unrelated callback must not release another job's budget.
    if (!active_ || request.generation != active_->generation ||
        request.cancelled != active_->cancelled) return false;
    const Request admitted = *active_;
    active_.reset();
    QFile file(admitted.path);
    if (admitted.generation != generation_ || admitted.cancelled->load() || !result ||
        !file.open(QIODevice::ReadOnly) || externalFileIdentity(file) != admitted.identity ||
        result->linear.isNull() || result->linear.format() != QImage::Format_RGBA16FPx4 ||
        quint64(result->linear.sizeInBytes()) > admitted.limits.rasterPixels * 8) {
        result.reset(); // Free output before releasing its work reservation.
        admitted.reservation->clear();
        return false;
    }
    auto retained = admitted.reservation->split({0, quint64(result->linear.sizeInBytes()), 0});
    if (!retained) {
        result.reset();
        admitted.reservation->clear();
        return false;
    }
    raster_ = std::make_shared<Raster>(Raster{std::move(retained), std::move(result->linear)});
    retainedSource_ = admitted;
    admitted.reservation->clear();
    return true;
}

bool RequestCoordinator::beginUpload(const ResourceBudget::Adapter& adapter)
{
    if (!raster_ || upload_) return false;
    QFile file(retainedSource_->path);
    if (!file.open(QIODevice::ReadOnly) || externalFileIdentity(file) != retainedSource_->identity) {
        cancel();
        return false;
    }
    auto lease = pool_->acquire({0, 0, retainedBytes()}, adapter);
    if (!lease) return false;
    upload_ = std::make_shared<Upload>(Upload{raster_, std::move(lease)});
    return true;
}
} // namespace Licasa::GainmapLab
