#pragma once
#include "viewer_resource_budget.h"
namespace Licasa {
// GUI-owner state. Rebind target admission only after old scene graph/native
// teardown; upload leases retain their own adapter and retirement ownership.
class HdrTargetHandoff {
public:
    enum class Phase { Active, Quiescing, Detached, Sdr };
    HdrTargetHandoff(std::shared_ptr<GainmapLab::ResourceBudget> pool, quint64 bytes,
                     GainmapLab::ResourceBudget::Adapter adapter, GainmapLab::ResourceBudget::Lease lease)
        : pool_(std::move(pool)), bytes_(bytes), adapter_(std::move(adapter)), lease_(std::move(lease)) {}
    quint64 request(GainmapLab::ResourceBudget::Adapter destination) {
        destination_ = std::move(destination); phase_ = Phase::Quiescing; return ++token_;
    }
    bool detached(quint64 token) {
        if (token != token_ || phase_ != Phase::Quiescing) return false;
        lease_.reset(); adapter_.clear(); phase_ = Phase::Detached; return true;
    }
    bool admit(quint64 token) {
        if (token != token_ || phase_ != Phase::Detached) return false;
        lease_ = pool_->acquire({0,0,0,bytes_}, destination_);
        if (lease_) { adapter_ = destination_; phase_ = Phase::Active; }
        else phase_ = Phase::Sdr;
        return bool(lease_);
    }
    quint64 token() const { return token_; }
    Phase phase() const { return phase_; }
    const GainmapLab::ResourceBudget::Adapter& adapter() const { return adapter_; }
private:
    std::shared_ptr<GainmapLab::ResourceBudget> pool_;
    quint64 bytes_, token_ = 1;
    GainmapLab::ResourceBudget::Adapter adapter_, destination_;
    GainmapLab::ResourceBudget::Lease lease_;
    Phase phase_ = Phase::Active;
};
}
