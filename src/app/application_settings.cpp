#include "app/application_settings.h"

#include "app/app_constants.h"
#include "platform/application_identity.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

namespace Licasa {
namespace {

constexpr qsizetype maximumAutostartBytes = 4096;

QString backgroundPreferenceFile()
{
    if (qEnvironmentVariableIsSet("SNAP_NAME")) {
        const QString common = qEnvironmentVariable("SNAP_USER_COMMON");
        if (!common.isEmpty() && QDir::isAbsolutePath(common)) {
            return QDir(common).filePath(QStringLiteral("licasa-background.ini"));
        }
    }
    return QSettings().fileName();
}

QString legacyQuoteDesktopExecArgument(QString argument)
{
    argument.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    argument.replace(QStringLiteral("\""), QStringLiteral("\\\""));
    argument.replace(QStringLiteral("`"), QStringLiteral("\\`"));
    argument.replace(QStringLiteral("$"), QStringLiteral("\\$"));
    argument.replace(QStringLiteral("%"), QStringLiteral("%%"));
    return QStringLiteral("\"") + argument + QStringLiteral("\"");
}

QMap<QByteArray, QByteArray> desktopEntryFields(const QByteArray& bytes)
{
    if (bytes.size() > maximumAutostartBytes) {
        return {};
    }
    QMap<QByteArray, QByteArray> fields;
    bool inEntry = false;
    const QList<QByteArray> allowed{"Type",
                                    "Version",
                                    "Name",
                                    "Comment",
                                    "Exec",
                                    "X-Licasa-Background",
                                    "X-Licasa-Installation",
                                    "X-Licasa-Schema",
                                    "Terminal",
                                    "NoDisplay",
                                    "StartupNotify"};
    for (QByteArray line : bytes.split('\n')) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith('#')) {
            continue;
        }
        if (line == "[Desktop Entry]" && !inEntry && fields.isEmpty()) {
            inEntry = true;
            continue;
        }
        const qsizetype separator = line.indexOf('=');
        if (!inEntry || separator <= 0) {
            return {};
        }
        const QByteArray key = line.left(separator);
        if (!allowed.contains(key) || fields.contains(key)) {
            return {};
        }
        fields.insert(key, line.mid(separator + 1));
    }
    return fields;
}

bool hasCompatibleAutostartOwner(const QMap<QByteArray, QByteArray>& fields,
                                 const QByteArray& owner)
{
    return (!fields.contains("X-Licasa-Installation") ||
            fields.value("X-Licasa-Installation") == owner) &&
           (!fields.contains("X-Licasa-Schema") || fields.value("X-Licasa-Schema") == "1");
}

const QString startInFullscreenKey = QStringLiteral("viewer/startInFullscreen");
const QString showGnomeTopBarKey = QStringLiteral("viewer/showGnomeTopBar");
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

BackgroundModeManager::BackgroundModeManager(QObject* parent)
    : QObject(parent), backgroundSettings_(backgroundPreferenceFile(), QSettings::IniFormat)
{}

bool BackgroundModeManager::supported() const
{
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD) || defined(Q_OS_OPENBSD) || defined(Q_OS_NETBSD)
    if (qEnvironmentVariableIsSet("SNAP_NAME")) {
        return !qEnvironmentVariableIsEmpty("SNAP_USER_DATA") &&
               QDir::isAbsolutePath(qEnvironmentVariable("SNAP_USER_DATA")) &&
               !qEnvironmentVariableIsEmpty("SNAP_USER_COMMON") &&
               QDir::isAbsolutePath(qEnvironmentVariable("SNAP_USER_COMMON"));
    }
    return true;
#else
    return false;
#endif
}

QString BackgroundModeManager::preferenceKey() const
{
    return QStringLiteral("background/installations/") + applicationIdentity(false).installationId +
           QStringLiteral("/enabled");
}

QString BackgroundModeManager::autostartCommand() const
{
    const QString launcher =
        qEnvironmentVariableIsSet("SNAP_NAME")
            ? QStringLiteral("licasa")
            : QStringLiteral("/usr/bin/env ") + quoteDesktopExecArgument(applicationLaunchPath());
    return launcher + QStringLiteral(" --background");
}

bool BackgroundModeManager::matchesAutostartCommand(const QByteArray& command) const
{
    if (command == autostartCommand().toUtf8()) {
        return true;
    }
    const QByteArray previous =
        (legacyQuoteDesktopExecArgument(applicationLaunchPath()) + QStringLiteral(" --background"))
            .toUtf8();
    const QByteArray former =
        (legacyQuoteDesktopExecArgument(applicationIdentity(false).executablePath) +
         QStringLiteral(" --background"))
            .toUtf8();
    return command == previous || command == former;
}

bool BackgroundModeManager::ownsAutostart(const QByteArray& content) const
{
    const auto fields = desktopEntryFields(content);
    const QByteArray owner = applicationIdentity(false).installationId.toUtf8();
    return hasCompatibleAutostartOwner(fields, owner) && fields.value("Type") == "Application" &&
           fields.value("Name") == "Licasa" && fields.value("X-Licasa-Background") == "1" &&
           matchesAutostartCommand(fields.value("Exec"));
}

bool BackgroundModeManager::enabled() const
{
    if (autostartEntryPath().isEmpty()) {
        return false;
    }
    if (backgroundSettings_.contains(preferenceKey())) {
        return backgroundSettings_.value(preferenceKey(), false).toBool();
    }
    // Adopt only an exact, marked opt-in from this installation's old generator.
    for (const QString& path : {autostartEntryPath(), legacyAutostartEntryPath()}) {
        const QFileInfo info(path);
        if (!info.isFile() || info.isSymLink() || info.size() > maximumAutostartBytes) {
            continue;
        }
        QFile entry(path);
        if (entry.open(QIODevice::ReadOnly) &&
            ownsAutostart(entry.read(maximumAutostartBytes + 1))) {
            return true;
        }
    }
    return false;
}

QString BackgroundModeManager::legacyAutostartEntryPath() const
{
    if (!supported()) {
        return {};
    }
    if (qEnvironmentVariableIsSet("SNAP_NAME")) {
        const QString data = qEnvironmentVariable("SNAP_USER_DATA");
        const QString common = qEnvironmentVariable("SNAP_USER_COMMON");
        if (common.isEmpty() || !QDir::isAbsolutePath(common)) {
            return {};
        }
        return data.isEmpty() ? QString{}
                              : QDir(data).filePath(
                                    QStringLiteral(".config/autostart/licasa-background.desktop"));
    }
    QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (config.isEmpty()) {
        config = QDir::homePath() + QStringLiteral("/.config");
    }
    return QDir(config).filePath(QStringLiteral("autostart/licasa-background.desktop"));
}

QString BackgroundModeManager::autostartEntryPath() const
{
    const QString legacy = legacyAutostartEntryPath();
    if (legacy.isEmpty() || qEnvironmentVariableIsSet("SNAP_NAME")) {
        return legacy;
    }
    return QFileInfo(legacy).dir().filePath(QStringLiteral("licasa-background-") +
                                            applicationIdentity(false).installationId +
                                            QStringLiteral(".desktop"));
}

void BackgroundModeManager::syncAutostart()
{
    if (autostartEntryPath().isEmpty()) {
        return;
    }
    const bool consent = enabled();
    // Existing consent is already durable. Only adoption from the legacy
    // desktop entry needs a new preference write before removing that entry.
    if (!backgroundSettings_.contains(preferenceKey())) {
        backgroundSettings_.setValue(preferenceKey(), consent);
        backgroundSettings_.sync();
    }
    if (backgroundSettings_.status() != QSettings::NoError) {
        reportError(QStringLiteral("Could not persist the background preference."));
        return;
    }
    if (consent) {
        if (!writeAutostart()) {
            return;
        }
    } else {
        removeAutostart(autostartEntryPath(), true);
    }
    if (legacyAutostartEntryPath() != autostartEntryPath()) {
        removeAutostart(legacyAutostartEntryPath(), true);
    }
    const QString old =
        QFileInfo(autostartEntryPath()).dir().filePath(QStringLiteral("licasa.desktop"));
    removeAutostart(old, false);
}

QString BackgroundModeManager::quoteDesktopExecArgument(QString argument)
{
    // Desktop string decoding happens before command-line parsing. Preserve
    // the argument escapes through both layers; the executable token itself
    // remains the stable /usr/bin/env rather than an unusually named path.
    argument = legacyQuoteDesktopExecArgument(argument);
    argument.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    return argument;
}

void BackgroundModeManager::reportError(const QString& message)
{
    qWarning().noquote() << message;
    emit errorOccurred(message);
}

void BackgroundModeManager::setEnabled(bool value)
{
    if (!supported() || autostartEntryPath().isEmpty()) {
        return;
    }
    const bool previous = enabled();
    if (value == previous && backgroundSettings_.contains(preferenceKey())) {
        return;
    }
    if (value && !writeAutostart()) {
        return;
    }
    if (!value && !removeAutostart(autostartEntryPath(), true)) {
        reportError(QStringLiteral("Background preference disabled; a modified startup entry was "
                                   "preserved for your review."));
    }
    backgroundSettings_.setValue(preferenceKey(), value);
    backgroundSettings_.sync();
    if (backgroundSettings_.status() != QSettings::NoError) {
        reportError(QStringLiteral("Could not persist the background preference."));
        return;
    }
    if (legacyAutostartEntryPath() != autostartEntryPath()) {
        removeAutostart(legacyAutostartEntryPath(), true);
    }
    if (value != previous) {
        emit enabledChanged();
    }
}

bool BackgroundModeManager::removeAutostart(const QString& path, bool includeCurrent)
{
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink()) {
        return true;
    }
    if (!info.isFile() || info.isSymLink() || info.size() > maximumAutostartBytes) {
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray entry = file.read(maximumAutostartBytes + 1);
    const auto fields = desktopEntryFields(entry);
    const bool unaskedLegacy =
        hasCompatibleAutostartOwner(fields, applicationIdentity(false).installationId.toUtf8()) &&
        !fields.contains("X-Licasa-Background") && fields.value("Type") == "Application" &&
        fields.value("Name") == "Licasa" &&
        fields.value("Comment") == "Start Licasa in background at login" &&
        matchesAutostartCommand(fields.value("Exec"));
    if (!(includeCurrent && ownsAutostart(entry)) && !unaskedLegacy) {
        return false;
    }
    // Preserve user overrides, even on our generated filename.
    if (entry.contains("Hidden=true") || entry.contains("X-GNOME-Autostart-enabled=false")) {
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
    const QString launchPath = applicationLaunchPath();
    for (const QChar character : launchPath) {
        if (character.unicode() < 32 || character.unicode() == 127) {
            reportError(QStringLiteral(
                "The application path cannot be represented in a desktop startup entry."));
            return false;
        }
    }
    const QString directoryPath = QFileInfo(path).absolutePath();

    const QString content = QStringLiteral("[Desktop Entry]\n"
                                           "Type=Application\n"
                                           "Version=1.0\n"
                                           "Name=Licasa\n"
                                           "Comment=Start Licasa in background at login\n"
                                           "Exec=%1\n"
                                           "X-Licasa-Background=1\n"
                                           "X-Licasa-Installation=%2\n"
                                           "X-Licasa-Schema=1\n"
                                           "Terminal=false\n"
                                           "NoDisplay=true\n"
                                           "StartupNotify=false\n")
                                .arg(autostartCommand(), applicationIdentity(false).installationId);

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
        if (!existing.open(QIODevice::ReadOnly) || existingInfo.size() > maximumAutostartBytes) {
            reportError(QStringLiteral("Cannot inspect existing autostart file: %1").arg(path));
            return false;
        }
        const QByteArray previous = existing.read(maximumAutostartBytes + 1);
        if (previous == bytes) {
            return true;
        }
        if (!ownsAutostart(previous) || previous.contains("Hidden=true") ||
            previous.contains("X-GNOME-Autostart-enabled=false")) {
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

bool ViewerPreferences::showGnomeTopBar() const
{
    return settings_.value(showGnomeTopBarKey).toString() == QStringLiteral("true");
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

void ViewerPreferences::setShowGnomeTopBar(bool value)
{
    if (showGnomeTopBar() == value) {
        return;
    }

    storeValue(showGnomeTopBarKey, value);
    emit showGnomeTopBarChanged();
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
