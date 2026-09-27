#pragma once

#include <QMutex>

namespace Licasa {

// Narrow runtime contract shared across the demand-loaded Motion Photo backend.
// The backend never needs the full ImageResourcePolicy implementation or any
// QML-facing services; it only borrows the heavy-operation gate and its current
// admitted megapixel budget.
class MotionPhotoResourcePolicy {
  public:
    virtual ~MotionPhotoResourcePolicy() = default;
    virtual QMutex& processingMutex() noexcept = 0;
    virtual int prepareForProcessing() = 0;
};

} // namespace Licasa
