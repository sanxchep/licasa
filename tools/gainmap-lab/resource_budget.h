#pragma once

#include <QtGlobal>
#include <QByteArray>
#include <memory>
#include <limits>
#include <mutex>
#include <map>
#include <utility>

namespace Licasa::GainmapLab {
// Admission estimates, not allocator/RSS enforcement. CPU categories and total
// usage are shared across adapters; GPU usage additionally has an adapter cap.
class ResourceBudget : public std::enable_shared_from_this<ResourceBudget> {
public:
    using Adapter = QByteArray;
    struct Bytes {
        quint64 decode = 0;
        quint64 retained = 0;
        quint64 upload = 0;
        quint64 targets = 0;
    };
    class Reservation {
    public:
        ~Reservation() { clear(); }
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        std::shared_ptr<Reservation> split(Bytes bytes)
        {
            auto part = std::shared_ptr<Reservation>(new Reservation(pool_, {}));
            std::lock_guard<std::mutex> lock(pool_->mutex_);
            if (!fits(bytes, bytes_)) return {};
            part->adapter_ = adapter_;
            part->bytes_ = bytes;
            subtract(bytes_, bytes);
            return part;
        }
        bool growCpu(quint64 decode, quint64 retained)
        {
            std::lock_guard<std::mutex> lock(pool_->mutex_);
            if (!bytes_.decode || !pool_->fitsLocked({decode, retained, 0}, adapter_)) return false;
            bytes_.decode += decode;
            bytes_.retained += retained;
            pool_->used_.decode += decode;
            pool_->used_.retained += retained;
            return true;
        }
        quint64 decodeBytes() const
        {
            std::lock_guard<std::mutex> lock(pool_->mutex_);
            return bytes_.decode;
        }
        // Only for a not-yet-submitted upload admitted without a device in an
        // offline probe. Never transfers a live GPU resource between adapters.
        bool bindAdapter(const Adapter& adapter)
        {
            std::lock_guard<std::mutex> lock(pool_->mutex_);
            if (!adapter_.isEmpty()) return adapter_ == adapter;
            const quint64 gpu = bytes_.upload + bytes_.targets;
            if (!gpu || adapter.isEmpty() || !pool_->adapterFits(adapter, gpu)) return false;
            pool_->adapters_.at(adapter).used += gpu;
            adapter_ = adapter;
            return true;
        }
        void clear()
        {
            std::lock_guard<std::mutex> lock(pool_->mutex_);
            if (!adapter_.isEmpty()) pool_->adapters_.at(adapter_).used -= bytes_.upload + bytes_.targets;
            subtract(pool_->used_, bytes_);
            bytes_ = {};
        }
    private:
        friend class ResourceBudget;
        Reservation(std::shared_ptr<ResourceBudget> pool, Bytes bytes, Adapter adapter = {})
            : pool_(std::move(pool)), bytes_(bytes), adapter_(std::move(adapter)) {}
        std::shared_ptr<ResourceBudget> pool_;
        Bytes bytes_;
        Adapter adapter_;
    };
    using Lease = std::shared_ptr<Reservation>;
    explicit ResourceBudget(Bytes limits, quint64 gpuLimit = std::numeric_limits<quint64>::max(),
                            quint64 totalLimit = std::numeric_limits<quint64>::max(), bool requireAdapter = false)
        : limits_(limits), gpuLimit_(gpuLimit), totalLimit_(totalLimit), requireAdapter_(requireAdapter) {}
    Lease acquire(Bytes bytes, const Adapter& adapter = {})
    {
        auto lease = Lease(new Reservation(shared_from_this(), {}, adapter));
        std::lock_guard<std::mutex> lock(mutex_);
        if (!fitsLocked(bytes, adapter)) {
            lease->adapter_.clear(); // A rejected/unknown adapter has no debit.
            return {};
        }
        lease->bytes_ = bytes;
        used_.decode += bytes.decode;
        used_.retained += bytes.retained;
        used_.upload += bytes.upload;
        used_.targets += bytes.targets;
        if (!adapter.isEmpty()) adapters_.at(adapter).used += bytes.upload + bytes.targets;
        return lease;
    }
    bool canAcquire(Bytes bytes, const Adapter& adapter = {}) const
    { std::lock_guard<std::mutex> lock(mutex_); return fitsLocked(bytes, adapter); }
    // Decreases never erase existing debits. New work waits until usage fits;
    // reservations admitted before the change can still split/retire safely.
    void setLimits(Bytes limits, quint64 gpuLimit, quint64 totalLimit)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fits(limits, limits_) && fits(limits_, limits) && gpuLimit_ == gpuLimit && totalLimit_ == totalLimit) return;
        limits_ = limits; gpuLimit_ = gpuLimit; totalLimit_ = totalLimit; ++revision_;
    }
    bool setAdapterLimit(const Adapter& adapter, quint64 limit)
    {
        if (adapter.isEmpty()) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = adapters_.find(adapter);
        if (found == adapters_.end()) { adapters_.emplace(adapter, AdapterUsage{limit, 0}); ++revision_; }
        else if (found->second.limit != limit) { found->second.limit = limit; ++revision_; }
        return true;
    }
    quint64 adapterUsed(const Adapter& adapter) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = adapters_.find(adapter);
        return found == adapters_.end() ? 0 : found->second.used;
    }
    quint64 adapterLimit(const Adapter& adapter) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = adapters_.find(adapter);
        return found == adapters_.end() ? 0 : found->second.limit;
    }
    Bytes used() const { std::lock_guard<std::mutex> lock(mutex_); return used_; }
    Bytes limits() const { std::lock_guard<std::mutex> lock(mutex_); return limits_; }
    quint64 gpuLimit() const { std::lock_guard<std::mutex> lock(mutex_); return gpuLimit_; }
    quint64 totalLimit() const { std::lock_guard<std::mutex> lock(mutex_); return totalLimit_; }
    quint64 revision() const { std::lock_guard<std::mutex> lock(mutex_); return revision_; }
private:
    static quint64 available(quint64 limit, quint64 used) { return used < limit ? limit - used : 0; }
    static bool consume(quint64& left, quint64 amount) {
        if (amount > left) return false;
        left -= amount; return true;
    }
    bool adapterFits(const Adapter& adapter, quint64 gpu) const {
        if (adapter.isEmpty()) return !requireAdapter_ || gpu == 0;
        const auto found = adapters_.find(adapter);
        return found != adapters_.end() && gpu <= available(found->second.limit, found->second.used);
    }
    bool fitsLocked(Bytes bytes, const Adapter& adapter) const {
        const Bytes free{available(limits_.decode, used_.decode), available(limits_.retained, used_.retained),
                         available(limits_.upload, used_.upload), available(limits_.targets, used_.targets)};
        if (!fits(bytes, free)) return false;
        quint64 total = available(totalLimit_, used_.decode + used_.retained + used_.upload + used_.targets);
        quint64 gpu = available(gpuLimit_, used_.upload + used_.targets);
        return consume(total, bytes.decode) && consume(total, bytes.retained)
            && consume(total, bytes.upload) && consume(total, bytes.targets)
            && consume(gpu, bytes.upload) && consume(gpu, bytes.targets)
            && adapterFits(adapter, bytes.upload + bytes.targets);
    }
    static bool fits(Bytes a, Bytes b)
    { return a.decode <= b.decode && a.retained <= b.retained && a.upload <= b.upload && a.targets <= b.targets; }
    static void subtract(Bytes& a, Bytes b)
    { a.decode -= b.decode; a.retained -= b.retained; a.upload -= b.upload; a.targets -= b.targets; }
    struct AdapterUsage { quint64 limit; quint64 used; };
    Bytes limits_, used_;
    quint64 gpuLimit_, totalLimit_, revision_ = 1;
    const bool requireAdapter_;
    std::map<Adapter, AdapterUsage> adapters_;
    mutable std::mutex mutex_;
};

// Destroy/reset on the owning thread after the final GPU use has completed.
template<class Resource> class LeasedResource {
public:
    LeasedResource(std::shared_ptr<void> lease, std::unique_ptr<Resource> resource)
        : lease_(std::move(lease)), resource_(std::move(resource))
    { Q_ASSERT(lease_ && resource_); }
    LeasedResource(const LeasedResource&) = delete;
    LeasedResource& operator=(const LeasedResource&) = delete;
    Resource* get() const { return resource_.get(); }
    void reset() { resource_.reset(); lease_.reset(); }
private:
    std::shared_ptr<void> lease_;
    std::unique_ptr<Resource> resource_;
};
} // namespace Licasa::GainmapLab
