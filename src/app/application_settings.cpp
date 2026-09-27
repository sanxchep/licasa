#include "app/application_settings.h"

#include "app/app_constants.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

namespace Licasa {
namespace {

const QString backgroundEnabledKey = QStringLiteral("backgroundProcess/enabled");
const QString startInFullscreenKey = QStringLiteral("viewer/startInFullscreen");
const QString fullscreenBackgroundOpacityKey = QStringLiteral("viewer/fullscreenBackgroundOpacity");
const QString transparencyCheckerboardEnabledKey =
    QStringLiteral("viewer/transparencyCheckerboardEnabled");
const QString transparencyCheckerboardOpacityKey =
    QStringLiteral("viewer/transparencyCheckerboardOpacity");
const QString windowedTransparencyCheckerboardEnabledKey =
    QStringLiteral("viewer/windowedTransparencyCheckerboardEnabled");
const QString windowedTransparencyOpacityKey = QStringLiteral("viewer/windowedTransparencyOpacity");
const QString animatedImagePlaybackEnabledKey =
    QStringLiteral("viewer/advanced/animatedImagePlaybackEnabled");
const QString animationSpeedKey = QStringLiteral("viewer/advanced/animationSpeed");
const QString smoothImageScalingEnabledKey =
    QStringLiteral("viewer/advanced/smoothImageScalingEnabled");
const QString mipmapImageScalingEnabledKey =
    QStringLiteral("viewer/advanced/mipmapImageScalingEnabled");
const QString pixelAlignedRenderingEnabledKey =
    QStringLiteral("viewer/advanced/pixelAlignedRenderingEnabled");
const QString fullResolutionRenderingEnabledKey =
    QStringLiteral("viewer/advanced/fullResolutionRenderingEnabled");
const QString colorManagedRenderingEnabledKey =
    QStringLiteral("viewer/advanced/colorManagedRenderingEnabled");
const QString maximumImageMemoryMiBKey = QStringLiteral("viewer/advanced/maximumImageMemoryMiB");
const QString cropShieldOpacityKey = QStringLiteral("viewer/advanced/cropShieldOpacity");

qreal boundedFinite(qreal value, qreal minimum, qreal maximum, qreal fallback)
{
    return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
}

qreal clampUnit(qreal value, qreal fallback) { return boundedFinite(value, 0.0, 1.0, fallback); }

qreal clampAnimationSpeed(qreal value) { return boundedFinite(value, 0.25, 2.0, 1.0); }

int clampMaximumImageMegapixels(int value)
{
    return std::clamp(value, Constants::minimumImageLimitMegapixels,
                      Constants::maximumImageLimitMegapixels);
}

} // namespace

BackgroundModeManager::BackgroundModeManager(QObject* parent) : QObject(parent)
{
    syncAutostartToState();
}

bool BackgroundModeManager::supported() const
{
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD) || defined(Q_OS_OPENBSD) || defined(Q_OS_NETBSD)
    return true;
#else
    return false;
#endif
}

bool BackgroundModeManager::enabled() const
{
    return settings_.value(backgroundEnabledKey, false).toBool();
}

QString BackgroundModeManager::autostartEntryPath() const
{
    if (!supported()) {
        return {};
    }

    QString configRoot = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (configRoot.isEmpty()) {
        configRoot = QDir::homePath() + QStringLiteral("/.config");
    }

    return QDir(configRoot).filePath(QStringLiteral("autostart/licasa.desktop"));
}

void BackgroundModeManager::setEnabled(bool value)
{
    if (!supported()) {
        reportError(QStringLiteral("Background mode is not supported on this platform."));
        return;
    }

    if (enabled() == value && autostartStateMatches(value)) {
        return;
    }
    if (!applyAutostart(value)) {
        return;
    }

    settings_.setValue(backgroundEnabledKey, value);
    settings_.sync();
    if (settings_.status() != QSettings::NoError) {
        reportError(QStringLiteral("Failed to save the background-mode setting."));
    }
    emit enabledChanged();
}

QString BackgroundModeManager::quoteDesktopExecArgument(QString argument)
{
    argument.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    argument.replace(QStringLiteral("\""), QStringLiteral("\\\""));
    argument.replace(QStringLiteral("`"), QStringLiteral("\\`"));
    argument.replace(QStringLiteral("$"), QStringLiteral("\\$"));
    argument.replace(QStringLiteral("%"), QStringLiteral("%%"));
    return QStringLiteral("\"") + argument + QStringLiteral("\"");
}

QString BackgroundModeManager::executablePath() const
{
    const QFileInfo fileInfo(QCoreApplication::applicationFilePath());
    const QString canonicalPath = fileInfo.canonicalFilePath();
    return canonicalPath.isEmpty() ? fileInfo.absoluteFilePath() : canonicalPath;
}

void BackgroundModeManager::reportError(const QString& message)
{
    qWarning().noquote() << message;
    emit errorOccurred(message);
}

bool BackgroundModeManager::autostartStateMatches(bool shouldBeEnabled) const
{
    const QFileInfo fileInfo(autostartEntryPath());
    return shouldBeEnabled ? fileInfo.exists() && fileInfo.isFile() : !fileInfo.exists();
}

void BackgroundModeManager::syncAutostartToState()
{
    if (supported()) {
        applyAutostart(enabled());
    }
}

bool BackgroundModeManager::applyAutostart(bool enable)
{
    const QString path = autostartEntryPath();
    const QString directoryPath = QFileInfo(path).absolutePath();

    if (enable) {
        QDir directory;
        if (!directory.mkpath(directoryPath)) {
            reportError(
                QStringLiteral("Failed to create autostart directory: %1").arg(directoryPath));
            return false;
        }

        const QString content = QStringLiteral("[Desktop Entry]\n"
                                               "Type=Application\n"
                                               "Version=1.0\n"
                                               "Name=Licasa\n"
                                               "Comment=Start Licasa in background at login\n"
                                               "Exec=%1 --background\n"
                                               "Terminal=false\n"
                                               "NoDisplay=true\n"
                                               "StartupNotify=false\n")
                                    .arg(quoteDesktopExecArgument(executablePath()));

        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            reportError(QStringLiteral("Failed to open autostart file for writing: %1").arg(path));
            return false;
        }

        const QByteArray bytes = content.toUtf8();
        if (file.write(bytes) != bytes.size()) {
            file.cancelWriting();
            reportError(QStringLiteral("Failed to write autostart file: %1").arg(path));
            return false;
        }
        if (!file.commit()) {
            reportError(QStringLiteral("Failed to commit autostart file: %1").arg(path));
            return false;
        }
        return true;
    }

    QFile file(path);
    if (!file.exists()) {
        return true;
    }
    if (!file.remove()) {
        reportError(QStringLiteral("Failed to remove autostart file: %1").arg(path));
        return false;
    }
    return true;
}

ViewerPreferences::ViewerPreferences(QObject* parent) : QObject(parent) {}

bool ViewerPreferences::startInFullscreen() const
{
    return settings_.value(startInFullscreenKey, true).toBool();
}

qreal ViewerPreferences::fullscreenBackgroundOpacity() const
{
    constexpr qreal defaultValue = 179.0 / 255.0;
    return clampUnit(settings_.value(fullscreenBackgroundOpacityKey, defaultValue).toReal(),
                     defaultValue);
}

bool ViewerPreferences::transparencyCheckerboardEnabled() const
{
    return settings_.value(transparencyCheckerboardEnabledKey, false).toBool();
}

qreal ViewerPreferences::transparencyCheckerboardOpacity() const
{
    constexpr qreal defaultValue = 179.0 / 255.0;
    return clampUnit(settings_.value(transparencyCheckerboardOpacityKey, defaultValue).toReal(),
                     defaultValue);
}

bool ViewerPreferences::windowedTransparencyCheckerboardEnabled() const
{
    return settings_.value(windowedTransparencyCheckerboardEnabledKey, false).toBool();
}

qreal ViewerPreferences::windowedTransparencyOpacity() const
{
    constexpr qreal defaultValue = 179.0 / 255.0;
    return clampUnit(settings_.value(windowedTransparencyOpacityKey, defaultValue).toReal(),
                     defaultValue);
}

bool ViewerPreferences::animatedImagePlaybackEnabled() const
{
    return settings_.value(animatedImagePlaybackEnabledKey, true).toBool();
}

qreal ViewerPreferences::animationSpeed() const
{
    return clampAnimationSpeed(settings_.value(animationSpeedKey, 1.0).toReal());
}

bool ViewerPreferences::smoothImageScalingEnabled() const
{
    return settings_.value(smoothImageScalingEnabledKey, true).toBool();
}

bool ViewerPreferences::mipmapImageScalingEnabled() const
{
    return settings_.value(mipmapImageScalingEnabledKey, true).toBool();
}

bool ViewerPreferences::pixelAlignedRenderingEnabled() const
{
    return settings_.value(pixelAlignedRenderingEnabledKey, true).toBool();
}

bool ViewerPreferences::fullResolutionRenderingEnabled() const
{
    return settings_.value(fullResolutionRenderingEnabledKey, true).toBool();
}

bool ViewerPreferences::colorManagedRenderingEnabled() const
{
    return settings_.value(colorManagedRenderingEnabledKey, true).toBool();
}

int ViewerPreferences::maximumImageMegapixels() const
{
    const qint64 pixels =
        qint64(maximumImageMemoryMiB()) * 1024 * 1024 / Constants::estimatedWorkingBytesPerPixel;
    return clampMaximumImageMegapixels(int(pixels / 1'000'000));
}

int ViewerPreferences::maximumImageMemoryMiB() const
{
    return std::clamp(
        settings_.value(maximumImageMemoryMiBKey, Constants::defaultImageMemoryMiB).toInt(),
        Constants::minimumImageMemoryMiB, Constants::maximumImageMemoryMiB);
}

int ViewerPreferences::minimumImageMemoryMiB() const { return Constants::minimumImageMemoryMiB; }

int ViewerPreferences::maximumImageMemoryLimitMiB() const
{
    return Constants::maximumImageMemoryMiB;
}

int ViewerPreferences::minimumImageLimitMegapixels() const
{
    return Constants::minimumImageLimitMegapixels;
}

int ViewerPreferences::maximumImageLimitMegapixels() const
{
    return Constants::maximumImageLimitMegapixels;
}

qreal ViewerPreferences::cropShieldOpacity() const
{
    return clampUnit(settings_.value(cropShieldOpacityKey, 0.70).toReal(), 0.70);
}

void ViewerPreferences::storeValue(const QString& key, const QVariant& value)
{
    settings_.setValue(key, value);
    settings_.sync();
    if (settings_.status() == QSettings::NoError) {
        return;
    }

    const QString message = QStringLiteral("Failed to save viewer setting: %1").arg(key);
    qWarning().noquote() << message;
    emit errorOccurred(message);
}

void ViewerPreferences::setStartInFullscreen(bool value)
{
    if (startInFullscreen() == value) {
        return;
    }

    storeValue(startInFullscreenKey, value);
    emit startInFullscreenChanged();
}

void ViewerPreferences::setFullscreenBackgroundOpacity(qreal value)
{
    const qreal boundedValue = clampUnit(value, fullscreenBackgroundOpacity());
    if (qFuzzyCompare(fullscreenBackgroundOpacity(), boundedValue)) {
        return;
    }

    storeValue(fullscreenBackgroundOpacityKey, boundedValue);
    emit fullscreenBackgroundOpacityChanged();
}

void ViewerPreferences::setTransparencyCheckerboardEnabled(bool value)
{
    if (transparencyCheckerboardEnabled() == value) {
        return;
    }

    storeValue(transparencyCheckerboardEnabledKey, value);
    emit transparencyCheckerboardEnabledChanged();
}

void ViewerPreferences::setTransparencyCheckerboardOpacity(qreal value)
{
    const qreal boundedValue = clampUnit(value, transparencyCheckerboardOpacity());
    if (qFuzzyCompare(transparencyCheckerboardOpacity(), boundedValue)) {
        return;
    }

    storeValue(transparencyCheckerboardOpacityKey, boundedValue);
    emit transparencyCheckerboardOpacityChanged();
}

void ViewerPreferences::setWindowedTransparencyCheckerboardEnabled(bool value)
{
    if (windowedTransparencyCheckerboardEnabled() == value) {
        return;
    }

    storeValue(windowedTransparencyCheckerboardEnabledKey, value);
    emit windowedTransparencyCheckerboardEnabledChanged();
}

void ViewerPreferences::setWindowedTransparencyOpacity(qreal value)
{
    const qreal boundedValue = clampUnit(value, windowedTransparencyOpacity());
    if (qFuzzyCompare(windowedTransparencyOpacity(), boundedValue)) {
        return;
    }

    storeValue(windowedTransparencyOpacityKey, boundedValue);
    emit windowedTransparencyOpacityChanged();
}

void ViewerPreferences::setAnimatedImagePlaybackEnabled(bool value)
{
    if (animatedImagePlaybackEnabled() == value) {
        return;
    }

    storeValue(animatedImagePlaybackEnabledKey, value);
    emit animatedImagePlaybackEnabledChanged();
}

void ViewerPreferences::setAnimationSpeed(qreal value)
{
    const qreal boundedValue = clampAnimationSpeed(value);
    if (qFuzzyCompare(animationSpeed(), boundedValue)) {
        return;
    }

    storeValue(animationSpeedKey, boundedValue);
    emit animationSpeedChanged();
}

void ViewerPreferences::setSmoothImageScalingEnabled(bool value)
{
    if (smoothImageScalingEnabled() == value) {
        return;
    }

    storeValue(smoothImageScalingEnabledKey, value);
    emit smoothImageScalingEnabledChanged();
}

void ViewerPreferences::setMipmapImageScalingEnabled(bool value)
{
    if (mipmapImageScalingEnabled() == value) {
        return;
    }

    storeValue(mipmapImageScalingEnabledKey, value);
    emit mipmapImageScalingEnabledChanged();
}

void ViewerPreferences::setPixelAlignedRenderingEnabled(bool value)
{
    if (pixelAlignedRenderingEnabled() == value) {
        return;
    }

    storeValue(pixelAlignedRenderingEnabledKey, value);
    emit pixelAlignedRenderingEnabledChanged();
}

void ViewerPreferences::setFullResolutionRenderingEnabled(bool value)
{
    if (fullResolutionRenderingEnabled() == value) {
        return;
    }

    storeValue(fullResolutionRenderingEnabledKey, value);
    emit fullResolutionRenderingEnabledChanged();
}

void ViewerPreferences::setColorManagedRenderingEnabled(bool value)
{
    if (colorManagedRenderingEnabled() == value) {
        return;
    }

    storeValue(colorManagedRenderingEnabledKey, value);
    emit colorManagedRenderingEnabledChanged();
}

void ViewerPreferences::setMaximumImageMegapixels(int value)
{
    const int boundedValue = clampMaximumImageMegapixels(value);
    const qint64 bytes =
        qint64(boundedValue) * 1'000'000 * Constants::estimatedWorkingBytesPerPixel;
    const int mebibytes = int((bytes + 1024 * 1024 - 1) / (1024 * 1024));
    setMaximumImageMemoryMiB(mebibytes);
}

void ViewerPreferences::setMaximumImageMemoryMiB(int value)
{
    const int boundedValue =
        std::clamp(value, Constants::minimumImageMemoryMiB, Constants::maximumImageMemoryMiB);
    if (maximumImageMemoryMiB() == boundedValue) {
        return;
    }
    storeValue(maximumImageMemoryMiBKey, boundedValue);
    emit maximumImageMemoryMiBChanged();
    emit maximumImageMegapixelsChanged();
}

void ViewerPreferences::setCropShieldOpacity(qreal value)
{
    const qreal boundedValue = clampUnit(value, cropShieldOpacity());
    if (qFuzzyCompare(cropShieldOpacity(), boundedValue)) {
        return;
    }

    storeValue(cropShieldOpacityKey, boundedValue);
    emit cropShieldOpacityChanged();
}

} // namespace Licasa
