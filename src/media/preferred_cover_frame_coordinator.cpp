#include "media/preferred_cover_frame_coordinator.h"

#include <limits>
#include <utility>

namespace Licasa {

PreferredCoverFrameCoordinator::PreferredCoverFrameCoordinator(QObject* parent) : QObject(parent) {}

bool PreferredCoverFrameCoordinator::currentIsPreferred() const noexcept
{
    return hasCurrentValue_ && preferredValue_ && currentValue_ == *preferredValue_;
}

void PreferredCoverFrameCoordinator::setSource(const QUrl& sourceUrl, PreferredCoverFrameKind kind)
{
    if (sourceUrl_ == sourceUrl && kind_ && *kind_ == kind) {
        return;
    }

    sourceUrl_ = sourceUrl;
    kind_ = kind;
    preferredValue_.reset();
    pendingApply_ = false;
    setErrorString({});
    loadPreference();
    emit stateChanged();
    maybeApplyPreferred();
}

void PreferredCoverFrameCoordinator::clearSource()
{
    const bool changed = !sourceUrl_.isEmpty() || kind_.has_value() ||
                         preferredValue_.has_value() || hasCurrentValue_ || canSetCurrent_ ||
                         canApplyPreferred_;

    sourceUrl_ = QUrl{};
    kind_.reset();
    preferredValue_.reset();
    currentValue_ = 0;
    hasCurrentValue_ = false;
    canSetCurrent_ = false;
    canApplyPreferred_ = false;
    pendingApply_ = false;
    applyHandler_ = {};
    setErrorString({});

    if (changed) {
        emit stateChanged();
    }
}

void PreferredCoverFrameCoordinator::setCurrentState(qint64 value, bool canSetCurrent,
                                                     bool canApplyPreferred)
{
    const bool oldIsPreferred = currentIsPreferred();
    const bool changed = currentValue_ != value || !hasCurrentValue_ ||
                         canSetCurrent_ != canSetCurrent || canApplyPreferred_ != canApplyPreferred;

    currentValue_ = value;
    hasCurrentValue_ = true;
    canSetCurrent_ = canSetCurrent;
    canApplyPreferred_ = canApplyPreferred;

    if (changed || oldIsPreferred != currentIsPreferred()) {
        emit stateChanged();
    }

    maybeApplyPreferred();
}

void PreferredCoverFrameCoordinator::setApplyHandler(std::function<bool(qint64)> handler)
{
    applyHandler_ = std::move(handler);
    maybeApplyPreferred();
}

void PreferredCoverFrameCoordinator::makeCurrentPreferred()
{
    if (!kind_ || sourceUrl_.isEmpty() || !canSetCurrent_ || !hasCurrentValue_) {
        return;
    }

    QString error;
    if (!storeCurrent(&error)) {
        setErrorString(error);
        return;
    }

    preferredValue_ = currentValue_;
    pendingApply_ = false;
    setErrorString({});
    emit stateChanged();
}

void PreferredCoverFrameCoordinator::clearPreferred()
{
    if (!kind_ || sourceUrl_.isEmpty()) {
        return;
    }

    QString error;
    if (!store_.clear(sourceUrl_, &error)) {
        setErrorString(error);
        return;
    }

    const bool hadPreferred = preferredValue_.has_value();
    preferredValue_.reset();
    pendingApply_ = false;
    setErrorString({});
    if (hadPreferred) {
        emit stateChanged();
    }
}

void PreferredCoverFrameCoordinator::setErrorString(const QString& value)
{
    if (errorString_ == value) {
        return;
    }
    errorString_ = value;
    emit errorChanged();
}

void PreferredCoverFrameCoordinator::loadPreference()
{
    if (!kind_ || sourceUrl_.isEmpty()) {
        return;
    }

    QString error;
    const std::optional<PreferredCoverFrame> preferred = store_.load(sourceUrl_, &error);
    if (!error.isEmpty()) {
        setErrorString(error);
        return;
    }

    if (!preferred || preferred->kind != *kind_) {
        return;
    }

    preferredValue_ = preferred->value;
    pendingApply_ = true;
}

void PreferredCoverFrameCoordinator::maybeApplyPreferred()
{
    if (!pendingApply_ || !preferredValue_ || !canApplyPreferred_ || !applyHandler_) {
        return;
    }

    if (applyHandler_(*preferredValue_)) {
        pendingApply_ = false;
    }
}

bool PreferredCoverFrameCoordinator::storeCurrent(QString* error)
{
    switch (*kind_) {
    case PreferredCoverFrameKind::MotionTimestampUs:
        return store_.storeMotionTimestampUs(sourceUrl_, currentValue_, error);
    case PreferredCoverFrameKind::AnimationFrameIndex:
        if (currentValue_ < 0 || currentValue_ > std::numeric_limits<int>::max()) {
            if (error) {
                *error = QStringLiteral("Preferred animation frame index is out of range.");
            }
            return false;
        }
        return store_.storeAnimationFrameIndex(sourceUrl_, static_cast<int>(currentValue_), error);
    }
    return false;
}

} // namespace Licasa
