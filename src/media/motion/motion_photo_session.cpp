#include "media/motion/motion_photo_session.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>

#include <utility>

#ifndef LICASA_MOTION_BACKEND_FILE_NAME
#define LICASA_MOTION_BACKEND_FILE_NAME "liblicasa_motion_photo_backend.so"
#endif

namespace Licasa {
namespace {

QStringList backendCandidates()
{
    QStringList paths;
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty()) {
        paths.append(QDir(appDir).filePath(QStringLiteral("plugins/motion/") +
                                           QStringLiteral(LICASA_MOTION_BACKEND_FILE_NAME)));
#ifdef LICASA_MOTION_BACKEND_INSTALL_RELATIVE
        paths.append(
            QDir(appDir).filePath(QStringLiteral(LICASA_MOTION_BACKEND_INSTALL_RELATIVE "/") +
                                  QStringLiteral(LICASA_MOTION_BACKEND_FILE_NAME)));
#endif
    }

    paths.removeDuplicates();
    return paths;
}

} // namespace

MotionPhotoSession::MotionPhotoSession(MotionPhotoResourcePolicy* resourcePolicy, QObject* parent)
    : QObject(parent), resourcePolicy_(resourcePolicy)
{}

MotionPhotoSession::~MotionPhotoSession() { releaseBackend(); }

void MotionPhotoSession::setAsset(const PhotoAssetInfo& asset)
{
    // Replacement is a hard authority/lifecycle boundary. A backend created for
    // an old source is destroyed before any new validated asset is cached.
    releaseBackend();
    resetPlaybackState();
    clearPresentationFrame();
    asset_.reset();

    const bool motion = asset.kind == PhotoAssetKind::MotionPhoto && asset.motion.has_value();
    const bool embedded = motion && asset.motion->hasEmbeddedVideo();
    const bool external = motion && asset.motion->hasExternalVideo();
    const bool validAuthority = motion && asset.sourceUrl.isLocalFile() && embedded != external;

    if (validAuthority) {
        asset_ = asset;
    }
    setAvailable(validAuthority);
}

void MotionPhotoSession::clearAsset()
{
    releaseBackend();
    resetPlaybackState();
    clearPresentationFrame();
    asset_.reset();
    setAvailable(false);
}

std::optional<MotionPhotoFrameRaster> MotionPhotoSession::captureCurrentFrame() const
{
    if (!backend_ || !active_) {
        return std::nullopt;
    }
    return backend_->captureCurrentFrame();
}

std::optional<MotionPhotoFrameRaster> MotionPhotoSession::presentationFrame() const
{
    return presentationFrame_;
}

void MotionPhotoSession::play()
{
    if (!available_ || !asset_ || !resourcePolicy_) {
        return;
    }

    if (backend_) {
        if (active_ && !playing_) {
            backend_->resume();
            return;
        }
        if (active_) {
            return;
        }
    }

    if (!ensureBackend()) {
        return;
    }

    setErrorString(QString());
    backend_->play(*asset_);
}

void MotionPhotoSession::pause()
{
    if (backend_ && active_) {
        backend_->pause();
    }
}

void MotionPhotoSession::togglePlayback()
{
    if (playing_) {
        pause();
    } else {
        play();
    }
}

void MotionPhotoSession::seek(qint64 positionMs)
{
    if (!backend_ || !active_ || !seekable_) {
        return;
    }
    backend_->seek(positionMs);
}

void MotionPhotoSession::stop()
{
    // Stop is a hard playback lifecycle boundary. MotionPhotoPlayback::stop()
    // is queued, so merely resetting the mirrored state can expose a false
    // "stopped" window while the backend still owns media and the policy lock.
    // Destroy the private backend synchronously; keep asset_/available_ intact
    // so a later play() creates a clean backend for the validated asset.
    releaseBackend();
    resetPlaybackState();
    clearPresentationFrame();
}

bool MotionPhotoSession::ensureBackend()
{
    if (backend_) {
        return true;
    }
    if (!resourcePolicy_) {
        setErrorString(QStringLiteral("Motion Photo playback has no resource policy."));
        return false;
    }

    QString lastError;
    for (const QString& path : backendCandidates()) {
        if (!QFileInfo(path).isFile()) {
            continue;
        }

        auto library = std::make_unique<QLibrary>(path);
        if (!library->load()) {
            lastError = library->errorString();
            continue;
        }

        const auto abi = reinterpret_cast<MotionPhotoBackendAbiFn>(
            library->resolve("licasa_motion_photo_backend_abi"));
        const auto create = reinterpret_cast<CreateMotionPhotoBackendFn>(
            library->resolve("licasa_create_motion_photo_backend"));
        if (!abi || !create || abi() != kMotionPhotoBackendAbi) {
            lastError = QStringLiteral("backend ABI mismatch");
            library->unload();
            continue;
        }

        std::unique_ptr<MotionPhotoBackend> backend(create(resourcePolicy_));
        if (!backend) {
            lastError = QStringLiteral("backend factory rejected the resource policy");
            library->unload();
            continue;
        }

        backend->setStateChangedCallback(
            [this](const MotionPhotoBackendState& state) { applyBackendState(state); });
        backend->setFrameChangedCallback(
            [this](MotionPhotoFrameRaster frame) { acceptPresentationFrame(std::move(frame)); });
        backendLibrary_ = std::move(library);
        backend_ = std::move(backend);
        return true;
    }

    if (!lastError.isEmpty()) {
        qWarning() << "Motion Photo backend load failed:" << lastError;
    } else {
        qWarning() << "Motion Photo backend module was not found";
    }
    setErrorString(QStringLiteral("Motion Photo playback backend is unavailable."));
    return false;
}

void MotionPhotoSession::releaseBackend()
{
    if (backend_) {
        backend_->setFrameChangedCallback({});
        backend_->setStateChangedCallback({});
        backend_.reset();
    }
    if (backendLibrary_) {
        if (!backendLibrary_->unload()) {
            qWarning() << "Motion Photo backend module remained mapped:"
                       << backendLibrary_->errorString();
        }
        backendLibrary_.reset();
    }
}

void MotionPhotoSession::applyBackendState(const MotionPhotoBackendState& state)
{
    const bool becameInactive = active_ && !state.active;
    if (active_ != state.active) {
        active_ = state.active;
        emit activeChanged();
    }
    if (playing_ != state.playing) {
        playing_ = state.playing;
        emit playingChanged();
    }
    if (positionMs_ != state.positionMs) {
        positionMs_ = state.positionMs;
        emit positionChanged();
    }
    if (durationMs_ != state.durationMs) {
        durationMs_ = state.durationMs;
        emit durationChanged();
    }
    if (seekable_ != state.seekable) {
        seekable_ = state.seekable;
        emit seekableChanged();
    }
    setErrorString(state.errorString);
    if (becameInactive) {
        clearPresentationFrame();
    }
}

void MotionPhotoSession::resetPlaybackState() { applyBackendState({}); }

void MotionPhotoSession::setAvailable(bool value)
{
    if (available_ == value) {
        return;
    }
    available_ = value;
    emit availableChanged();
}

void MotionPhotoSession::setErrorString(const QString& value)
{
    if (errorString_ == value) {
        return;
    }
    errorString_ = value;
    emit errorChanged();
}

void MotionPhotoSession::acceptPresentationFrame(MotionPhotoFrameRaster frame)
{
    if (!frame.isValid()) {
        return;
    }
    presentationFrame_ = std::move(frame);
    emit frameChanged();
}

void MotionPhotoSession::clearPresentationFrame()
{
    if (!presentationFrame_) {
        return;
    }
    presentationFrame_.reset();
    emit frameChanged();
}

} // namespace Licasa
