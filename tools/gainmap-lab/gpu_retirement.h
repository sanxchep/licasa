#pragma once

#include <QtGlobal>
#include <memory>
#include <vector>

namespace Licasa::GainmapLab {
// Backend fences are scoped to one device/queue generation. Poll is nonblocking;
// wait must establish completion (or legal device-loss teardown) before true.
class GpuFence {
public:
    enum class Status { Pending, Complete, Failed, DeviceLost };
    virtual ~GpuFence() = default;
    virtual Status poll() noexcept = 0;
    virtual bool wait() noexcept = 0;
};

// Render-thread-owned bounded retirement list. Retire after the LAST submission
// using a resource, including later draws. Completion of its upload alone is
// insufficient. Enqueue before submission so queue exhaustion cannot strand work.
class GpuRetirement {
public:
    explicit GpuRetirement(size_t capacity = 64) : capacity_(capacity) {}
    ~GpuRetirement()
    {
        if (!shutdown()) qFatal("GPU retirement could not establish safe teardown");
    }
    GpuRetirement(const GpuRetirement&) = delete;
    GpuRetirement& operator=(const GpuRetirement&) = delete;
    bool retire(const std::shared_ptr<void>& resource, const std::shared_ptr<GpuFence>& fence)
    {
        if (!resource || !fence || entries_.size() >= capacity_) return false;
        entries_.push_back(std::make_unique<Entry>(Entry{fence, resource}));
        return true;
    }
    // A failed poll retains ownership and reports failure; it is never completion.
    bool collect()
    {
        bool healthy = true;
        for (auto i = entries_.begin(); i != entries_.end();) {
            const auto status = (*i)->fence->poll();
            deviceLost_ |= status == GpuFence::Status::DeviceLost;
            if (status == GpuFence::Status::Complete) i = entries_.erase(i);
            else { healthy &= status == GpuFence::Status::Pending; ++i; }
        }
        return healthy;
    }
    bool shutdown() noexcept
    {
        for (const auto& entry : entries_) if (!entry->fence->wait()) return false;
        entries_.clear(); // Resource destroyed before its fence and lease.
        return true;
    }
    size_t pending() const { return entries_.size(); }
    bool deviceLost() const { return deviceLost_; }
private:
    struct Entry { std::shared_ptr<GpuFence> fence; std::shared_ptr<void> resource; };
    bool deviceLost_ = false;
    const size_t capacity_;
    std::vector<std::unique_ptr<Entry>> entries_;
};
} // namespace Licasa::GainmapLab
