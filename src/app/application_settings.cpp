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

BackgroundModeManager::BackgroundModeManager(QObject* parent) : QObject(parent) {}

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
    const QFileInfo info(autostartEntryPath());
    if (!info.isFile() || info.isSymLink() || info.size() > 4096) {
        return false;
    }
    QFile entry(info.filePath());
    if (!entry.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    const QByteArray content = entry.readAll();
    const QString launchCommand = qEnvironmentVariableIsSet("SNAP_NAME")
                                      ? QStringLiteral("licasa")
                                      : quoteDesktopExecArgument(executablePath());
    return content.contains("X-Licasa-Background=1\n") &&
           content.contains((QStringLiteral("Exec=%1 --background\n").arg(launchCommand)).toUtf8());
}

QString BackgroundModeManager::autostartEntryPath() const
{
    if (!supported()) {
        return {};
    }

    if (qEnvironmentVariableIsSet("SNAP_NAME")) {
        const QString snapUserData = qEnvironmentVariable("SNAP_USER_DATA");
        if (snapUserData.isEmpty()) {
            return {};
        }
        return QDir(snapUserData)
            .filePath(QStringLiteral(".config/autostart/licasa-background.desktop"));
    }

    QString configRoot = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (configRoot.isEmpty()) {
        configRoot = QDir::homePath() + QStringLiteral("/.config");
    }

    return QDir(configRoot).filePath(QStringLiteral("autostart/licasa-background.desktop"));
}

void BackgroundModeManager::syncAutostart()
{
    const QString currentPath = autostartEntryPath();
    if (currentPath.isEmpty()) {
        return;
    }
    // Old releases created login entries without asking. Retire only entries
    // with the known legacy signature; the new entry is created by the toggle.
    const QString legacyPath =
        QFileInfo(currentPath).dir().filePath(QStringLiteral("licasa.desktop"));
    removeAutostart(legacyPath, false);
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
    const QString appImage = qEnvironmentVariable("APPIMAGE");
    if (QFileInfo(appImage).isFile()) {
        return QFileInfo(appImage).absoluteFilePath();
    }
    const QString appDir = qEnvironmentVariable("APPDIR");
    const QFileInfo appRun(QDir(appDir).filePath(QStringLiteral("AppRun")));
    if (!appDir.isEmpty() && appRun.isFile()) {
        return appRun.absoluteFilePath();
    }
    const QFileInfo fileInfo(QCoreApplication::applicationFilePath());
    const QString canonicalPath = fileInfo.canonicalFilePath();
    return canonicalPath.isEmpty() ? fileInfo.absoluteFilePath() : canonicalPath;
}

void BackgroundModeManager::reportError(const QString& message)
{
    qWarning().noquote() << message;
    emit errorOccurred(message);
}

void BackgroundModeManager::setEnabled(bool value)
{
    if (!supported() || autostartEntryPath().isEmpty() || value == enabled()) {
        return;
    }
    const bool success = value ? writeAutostart() : removeAutostart(autostartEntryPath(), true);
    if (success) {
        emit enabledChanged();
    }
}

bool BackgroundModeManager::removeAutostart(const QString& path, bool includeCurrent)
{
    if (path.isEmpty()) {
        return false;
    }
    const QFileInfo entryInfo(path);
    if (!entryInfo.exists()) {
        return true;
    }
    if (!entryInfo.isFile() || entryInfo.isSymLink() || entryInfo.size() > 4096) {
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    const QByteArray entry = file.read(4097);
    const bool current = entry.contains("X-Licasa-Background=1\n");
    const bool legacy = entry.contains("Name=Licasa\n") &&
                        entry.contains("Comment=Start Licasa in background at login\n") &&
                        entry.contains("--background\n");
    const bool managed = (includeCurrent && current) || (!current && legacy);
    if (entry.size() > 4096 || !managed) {
        return false;
    }
    file.close();
    if (!file.remove()) {
        reportError(QStringLiteral("Failed to remove Licasa autostart file: %1").arg(path));
        return false;
    }
    return true;
}

bool BackgroundModeManager::writeAutostart()
{
    const QString path = autostartEntryPath();
    const QString directoryPath = QFileInfo(path).absolutePath();

    const QString content = QStringLiteral("[Desktop Entry]\n"
                                           "Type=Application\n"
                                           "Version=1.0\n"
                                           "Name=Licasa\n"
                                           "Comment=Start Licasa in background at login\n"
                                           "Exec=%1 --background\n"
                                           "X-Licasa-Background=1\n"
                                           "Terminal=false\n"
                                           "NoDisplay=true\n"
                                           "StartupNotify=false\n")
                                .arg(qEnvironmentVariableIsSet("SNAP_NAME")
                                         ? QStringLiteral("licasa")
                                         : quoteDesktopExecArgument(executablePath()));

    const QByteArray bytes = content.toUtf8();
    const QFileInfo existingInfo(path);
    if (existingInfo.isSymLink()) {
        reportError(QStringLiteral("Refusing to replace symlinked autostart file: %1").arg(path));
        return false;
    }
    if (existingInfo.exists() && !existingInfo.isFile()) {
        reportError(QStringLiteral("Refusing to replace non-file autostart entry: %1").arg(path));
        return false;
    }
    if (existingInfo.isFile()) {
        QFile existing(path);
        if (!existing.open(QIODevice::ReadOnly) || existingInfo.size() > 4096) {
            reportError(QStringLiteral("Cannot inspect existing autostart file: %1").arg(path));
            return false;
        }
        const QByteArray previous = existing.read(4097);
        if (previous == bytes) {
            return true;
        }
        if (!previous.contains("X-Licasa-Background=1\n")) {
            reportError(QStringLiteral("Refusing to replace another autostart file: %1").arg(path));
            return false;
        }
    }

    QDir directory;
    if (!directory.mkpath(directoryPath)) {
        reportError(QStringLiteral("Failed to create autostart directory: %1").arg(directoryPath));
        return false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        reportError(QStringLiteral("Failed to open autostart file for writing: %1").arg(path));
        return false;
    }
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
