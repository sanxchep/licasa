#pragma once
#include "../../../tools/gainmap-lab/request_coordinator.h"
#include "../../app/app_constants.h"

namespace Licasa {
inline GainmapLab::ResourceBudget::Bytes viewerHdrResourceLimits(quint64 total)
{
    auto limits = GainmapLab::RequestCoordinator::decodeAllowance(
        {GainmapLab::Limits{}.compressedBytes, GainmapLab::maximumRasterPixels});
    limits.upload = limits.retained;
    limits.targets = qMin(total, 960ull * 1024 * 1024);
    return limits;
}

// One process-wide admission pool across windows/adapters. Qt allocator/driver
// overhead and unrelated scene-graph caches are outside this accounting policy.
inline std::shared_ptr<GainmapLab::ResourceBudget> viewerResourceBudget()
{
    static auto pool = std::make_shared<GainmapLab::ResourceBudget>(
        viewerHdrResourceLimits(0),
        1024ull * 1024 * 1024, 0, true); // Fail closed until the user policy is bound.
    return pool;
}
inline void configureHdrMemoryBudget(int mebibytes)
{
    const quint64 total = quint64(qBound(Constants::minimumImageMemoryMiB, mebibytes,
                                        Constants::maximumImageMemoryMiB)) * 1024 * 1024;
    viewerResourceBudget()->setLimits(viewerHdrResourceLimits(total),
                                     qMin(total, 1024ull * 1024 * 1024), total);
}
}
