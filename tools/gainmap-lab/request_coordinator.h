#pragma once

#include "jpeg_gainmap.h"
#include "resource_budget.h"
#include <memory>

namespace Licasa::GainmapLab {
// Owner-thread state machine; only decode() and the cancellation flag cross to
// a worker. One synchronous codec call may drain after cancellation.
class RequestCoordinator {
public:
    struct Budgets {
        quint64 decode = 320 * 1024 * 1024;
        quint64 retained = 32 * 1024 * 1024;
        quint64 upload = 32 * 1024 * 1024;
    };
    struct Raster {
        ResourceBudget::Lease lease;
        QImage image;
    };
    struct Upload {
        std::shared_ptr<Raster> raster;
        ResourceBudget::Lease lease;
    };
    struct Request {
        quint64 generation;
        QString path;
        ExternalFileIdentity identity;
        float headroom;
        Limits limits;
        std::shared_ptr<std::atomic_bool> cancelled;
        ResourceBudget::Lease reservation;
        quint64 reservedRasterPixels;
        // Called once by the built-in worker after the codec verifies dimensions.
        bool reserveRaster(quint64 pixels) const;
    };
    RequestCoordinator();
    explicit RequestCoordinator(Budgets budgets);
    explicit RequestCoordinator(std::shared_ptr<ResourceBudget> pool);
    ~RequestCoordinator() { cancel(); }
    RequestCoordinator(const RequestCoordinator&) = delete;
    RequestCoordinator& operator=(const RequestCoordinator&) = delete;
    // Every request replaces the previous intent, including rejected requests.
    // No hidden queue: a busy caller stays on SDR and may retry after completion.
    std::optional<Request> request(const QString& path, const ExternalFileIdentity& identity,
                                   float headroom, Limits limits = {}, bool deferRasterGrowth = false);
    static constexpr ResourceBudget::Bytes decodeAllowance(Limits limits)
    {
        return {limits.rasterPixels * 64 + limits.compressedBytes * 2,
                limits.rasterPixels * 8, 0};
    }
    void cancel();
    bool complete(const Request& request, std::optional<Result> result);
    const QImage& image() const { return raster_ ? raster_->image : empty_; }
    quint64 decodeBytes() const { return active_ ? active_->reservation->decodeBytes() : 0; }
    quint64 retainedBytes() const { return quint64(image().sizeInBytes()); }
    quint64 uploadBytes() const { return upload_ ? retainedBytes() : 0; }
    // Keep the returned Upload with queued transfers/persistent resources.
    // It retains both the raster and source-texture allowance after cancel/close.
    bool beginUpload(const ResourceBudget::Adapter& adapter = {});
    QImage* uploadImage() { return upload_ ? &raster_->image : nullptr; }
    std::shared_ptr<Upload> uploadLease() const { return upload_; }
    void endUpload() { upload_.reset(); }
    std::shared_ptr<ResourceBudget> budget() const { return pool_; }
private:
    std::shared_ptr<ResourceBudget> pool_;
    quint64 generation_ = 0;
    std::shared_ptr<Upload> upload_;
    std::optional<Request> active_;
    std::shared_ptr<Raster> raster_;
    QImage empty_;
    std::optional<Request> retainedSource_;
};
} // namespace Licasa::GainmapLab
