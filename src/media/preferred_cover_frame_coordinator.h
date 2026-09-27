#pragma once

#include "media/preferred_cover_frame_store.h"

#include <QObject>
#include <QUrl>

#include <functional>
#include <optional>

namespace Licasa {

// Per-window native authority for the optional preferred temporal cover frame.
// QML observes only capability/state and emits intent through the invokable
// actions below; persisted timestamps/frame indexes never cross the meta-object
// boundary.
class PreferredCoverFrameCoordinator final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool canSetCurrent READ canSetCurrent NOTIFY stateChanged)
    Q_PROPERTY(bool hasPreferred READ hasPreferred NOTIFY stateChanged)
    Q_PROPERTY(bool currentIsPreferred READ currentIsPreferred NOTIFY stateChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorChanged)

  public:
    explicit PreferredCoverFrameCoordinator(QObject* parent = nullptr);

    bool canSetCurrent() const noexcept { return canSetCurrent_; }
    bool hasPreferred() const noexcept { return preferredValue_.has_value(); }
    bool currentIsPreferred() const noexcept;
    QString errorString() const { return errorString_; }

    // Native-only authority updates. These deliberately are not slots or
    // invokables, so QML cannot inject source identities or raw temporal values.
    void setSource(const QUrl& sourceUrl, PreferredCoverFrameKind kind);
    void clearSource();
    void setCurrentState(qint64 value, bool canSetCurrent, bool canApplyPreferred);
    void setApplyHandler(std::function<bool(qint64)> handler);

    Q_INVOKABLE void makeCurrentPreferred();
    Q_INVOKABLE void clearPreferred();

  signals:
    void stateChanged();
    void errorChanged();

  private:
    void setErrorString(const QString& value);
    void loadPreference();
    void maybeApplyPreferred();
    bool storeCurrent(QString* error);

    PreferredCoverFrameStore store_;
    QUrl sourceUrl_;
    std::optional<PreferredCoverFrameKind> kind_;
    std::optional<qint64> preferredValue_;
    qint64 currentValue_ = 0;
    bool hasCurrentValue_ = false;
    bool canSetCurrent_ = false;
    bool canApplyPreferred_ = false;
    bool pendingApply_ = false;
    std::function<bool(qint64)> applyHandler_;
    QString errorString_;
};

} // namespace Licasa
