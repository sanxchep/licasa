#include "media/preferred_cover_frame_store.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <limits>

namespace Licasa {
namespace {

constexpr int schemaVersion = 1;
const QString settingsRoot = QStringLiteral("temporalCover/v1/");
const QString motionKind = QStringLiteral("motion-timestamp-us");
const QString animationKind = QStringLiteral("animation-frame-index");

QString decimal(quint64 value) { return QString::number(value); }

QString decimal(qint64 value) { return QString::number(value); }

bool readUnsigned(const QSettings& settings, const QString& key, quint64* value)
{
    bool ok = false;
    const quint64 parsed = settings.value(key).toString().toULongLong(&ok, 10);
    if (ok && value) {
        *value = parsed;
    }
    return ok;
}

bool readSigned(const QSettings& settings, const QString& key, qint64* value)
{
    bool ok = false;
    const qint64 parsed = settings.value(key).toString().toLongLong(&ok, 10);
    if (ok && value) {
        *value = parsed;
    }
    return ok;
}

QString kindString(PreferredCoverFrameKind kind)
{
    switch (kind) {
    case PreferredCoverFrameKind::MotionTimestampUs:
        return motionKind;
    case PreferredCoverFrameKind::AnimationFrameIndex:
        return animationKind;
    }
    return {};
}

} // namespace

PreferredCoverFrameStore::PreferredCoverFrameStore() = default;

QString PreferredCoverFrameStore::settingsGroupKey(const QUrl& sourceUrl, QString* error)
{
    if (!sourceUrl.isLocalFile()) {
        if (error) {
            *error = QStringLiteral("Preferred cover frames require a local source file.");
        }
        return {};
    }

    const QString localPath = sourceUrl.toLocalFile();
    if (localPath.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Preferred cover frame source path is empty.");
        }
        return {};
    }

    const QString normalizedPath = QDir::cleanPath(QFileInfo(localPath).absoluteFilePath());
    if (normalizedPath.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Preferred cover frame source path is invalid.");
        }
        return {};
    }

    const QByteArray digest =
        QCryptographicHash::hash(normalizedPath.toUtf8(), QCryptographicHash::Sha256).toHex();
    return settingsRoot + QString::fromLatin1(digest);
}

std::optional<PreferredCoverFrameStore::InspectedAsset>
PreferredCoverFrameStore::inspectAsset(const QUrl& sourceUrl, QString* error)
{
    const QString groupKey = settingsGroupKey(sourceUrl, error);
    if (groupKey.isEmpty()) {
        return std::nullopt;
    }

    QFile file(sourceUrl.toLocalFile());
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("Unable to open preferred cover frame source: %1")
                         .arg(file.errorString());
        }
        return std::nullopt;
    }

    QString identityError;
    const std::optional<ExternalFileIdentity> identity = externalFileIdentity(file, &identityError);
    if (!identity) {
        if (error) {
            *error = identityError;
        }
        return std::nullopt;
    }

    return InspectedAsset{groupKey, *identity};
}

bool PreferredCoverFrameStore::persistedIdentityMatches(const QSettings& settings,
                                                        const ExternalFileIdentity& identity)
{
    ExternalFileIdentity persisted;
    // Existing preferences predate link-count pinning. Their other identity
    // fields still guard the stored choice, so keep them readable.
    const QString linkCountKey = QStringLiteral("identity/linkCount");
    if (settings.contains(linkCountKey)) {
        if (!readUnsigned(settings, linkCountKey, &persisted.linkCount)) {
            return false;
        }
    } else {
        persisted.linkCount = identity.linkCount;
    }
    if (!readUnsigned(settings, QStringLiteral("identity/device"), &persisted.device) ||
        !readUnsigned(settings, QStringLiteral("identity/inode"), &persisted.inode) ||
        !readUnsigned(settings, QStringLiteral("identity/size"), &persisted.size) ||
        !readSigned(settings, QStringLiteral("identity/modifiedSeconds"),
                    &persisted.modifiedSeconds) ||
        !readSigned(settings, QStringLiteral("identity/modifiedNanoseconds"),
                    &persisted.modifiedNanoseconds) ||
        !readSigned(settings, QStringLiteral("identity/changedSeconds"),
                    &persisted.changedSeconds) ||
        !readSigned(settings, QStringLiteral("identity/changedNanoseconds"),
                    &persisted.changedNanoseconds)) {
        return false;
    }

    return persisted == identity;
}

void PreferredCoverFrameStore::writeIdentity(QSettings& settings,
                                             const ExternalFileIdentity& identity)
{
    settings.setValue(QStringLiteral("identity/device"), decimal(identity.device));
    settings.setValue(QStringLiteral("identity/inode"), decimal(identity.inode));
    settings.setValue(QStringLiteral("identity/linkCount"), decimal(identity.linkCount));
    settings.setValue(QStringLiteral("identity/size"), decimal(identity.size));
    settings.setValue(QStringLiteral("identity/modifiedSeconds"),
                      decimal(identity.modifiedSeconds));
    settings.setValue(QStringLiteral("identity/modifiedNanoseconds"),
                      decimal(identity.modifiedNanoseconds));
    settings.setValue(QStringLiteral("identity/changedSeconds"), decimal(identity.changedSeconds));
    settings.setValue(QStringLiteral("identity/changedNanoseconds"),
                      decimal(identity.changedNanoseconds));
}

std::optional<PreferredCoverFrame> PreferredCoverFrameStore::load(const QUrl& sourceUrl,
                                                                  QString* error) const
{
    const std::optional<InspectedAsset> asset = inspectAsset(sourceUrl, error);
    if (!asset) {
        return std::nullopt;
    }

    settings_.beginGroup(asset->groupKey);

    const int schema = settings_.value(QStringLiteral("schema"), 0).toInt();
    if (schema != schemaVersion || !persistedIdentityMatches(settings_, asset->identity)) {
        settings_.endGroup();
        return std::nullopt;
    }

    const QString kind = settings_.value(QStringLiteral("kind")).toString();
    qint64 value = -1;
    const bool valueOk = readSigned(settings_, QStringLiteral("value"), &value);
    settings_.endGroup();

    if (!valueOk || value < 0) {
        return std::nullopt;
    }

    if (kind == motionKind) {
        return PreferredCoverFrame{PreferredCoverFrameKind::MotionTimestampUs, value};
    }

    if (kind == animationKind && value <= std::numeric_limits<int>::max()) {
        return PreferredCoverFrame{PreferredCoverFrameKind::AnimationFrameIndex, value};
    }

    return std::nullopt;
}

bool PreferredCoverFrameStore::storeMotionTimestampUs(const QUrl& sourceUrl, qint64 timestampUs,
                                                      QString* error)
{
    if (timestampUs < 0) {
        if (error) {
            *error = QStringLiteral("Preferred Motion Photo timestamp cannot be negative.");
        }
        return false;
    }
    return store(sourceUrl, PreferredCoverFrameKind::MotionTimestampUs, timestampUs, error);
}

bool PreferredCoverFrameStore::storeAnimationFrameIndex(const QUrl& sourceUrl, int frameIndex,
                                                        QString* error)
{
    if (frameIndex < 0) {
        if (error) {
            *error = QStringLiteral("Preferred animation frame index cannot be negative.");
        }
        return false;
    }
    return store(sourceUrl, PreferredCoverFrameKind::AnimationFrameIndex, qint64(frameIndex),
                 error);
}

bool PreferredCoverFrameStore::store(const QUrl& sourceUrl, PreferredCoverFrameKind kind,
                                     qint64 value, QString* error)
{
    const std::optional<InspectedAsset> asset = inspectAsset(sourceUrl, error);
    if (!asset) {
        return false;
    }

    settings_.beginGroup(asset->groupKey);
    settings_.remove(QString());
    settings_.setValue(QStringLiteral("schema"), schemaVersion);
    settings_.setValue(QStringLiteral("kind"), kindString(kind));
    settings_.setValue(QStringLiteral("value"), decimal(value));
    writeIdentity(settings_, asset->identity);
    settings_.endGroup();

    return sync(error);
}

bool PreferredCoverFrameStore::clear(const QUrl& sourceUrl, QString* error)
{
    const QString groupKey = settingsGroupKey(sourceUrl, error);
    if (groupKey.isEmpty()) {
        return false;
    }

    settings_.remove(groupKey);
    return sync(error);
}

bool PreferredCoverFrameStore::sync(QString* error)
{
    settings_.sync();
    if (settings_.status() == QSettings::NoError) {
        return true;
    }

    if (error) {
        *error = QStringLiteral("Unable to persist preferred cover frame state.");
    }
    return false;
}

} // namespace Licasa
