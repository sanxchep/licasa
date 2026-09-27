#pragma once

#include "io/external_file_identity.h"

#include <QSettings>
#include <QUrl>

#include <optional>

namespace Licasa {

enum class PreferredCoverFrameKind {
    MotionTimestampUs,
    AnimationFrameIndex,
};

struct PreferredCoverFrame {
    PreferredCoverFrameKind kind = PreferredCoverFrameKind::MotionTimestampUs;
    qint64 value = 0;
};

inline bool operator==(const PreferredCoverFrame& left, const PreferredCoverFrame& right) noexcept
{
    return left.kind == right.kind && left.value == right.value;
}

inline bool operator!=(const PreferredCoverFrame& left, const PreferredCoverFrame& right) noexcept
{
    return !(left == right);
}

// Licasa-side persistence for an optional user-selected temporal cover frame.
//
// This class intentionally owns no decoder, image data, or QML surface. Records
// are keyed by a SHA-256 digest of the normalized local path, and the persisted
// preference is accepted only when the currently opened regular file has the
// same immutable file identity that was pinned when the preference was saved.
// Replacing or mutating the source therefore invalidates stale state without
// ever rewriting the imported asset.
class PreferredCoverFrameStore final {
  public:
    PreferredCoverFrameStore();

    std::optional<PreferredCoverFrame> load(const QUrl& sourceUrl, QString* error = nullptr) const;

    bool storeMotionTimestampUs(const QUrl& sourceUrl, qint64 timestampUs,
                                QString* error = nullptr);
    bool storeAnimationFrameIndex(const QUrl& sourceUrl, int frameIndex, QString* error = nullptr);
    bool clear(const QUrl& sourceUrl, QString* error = nullptr);

  private:
    struct InspectedAsset {
        QString groupKey;
        ExternalFileIdentity identity;
    };

    static QString settingsGroupKey(const QUrl& sourceUrl, QString* error);
    static std::optional<InspectedAsset> inspectAsset(const QUrl& sourceUrl, QString* error);
    static bool persistedIdentityMatches(const QSettings& settings,
                                         const ExternalFileIdentity& identity);
    static void writeIdentity(QSettings& settings, const ExternalFileIdentity& identity);

    bool store(const QUrl& sourceUrl, PreferredCoverFrameKind kind, qint64 value, QString* error);
    bool sync(QString* error);

    mutable QSettings settings_;
};

} // namespace Licasa
