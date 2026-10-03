#pragma once

#include <QObject>
#include <QProcess>
#include <QSettings>
#include <QTimer>
#include <QVariant>

namespace Licasa {

class BackgroundModeManager final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled CONSTANT)
    Q_PROPERTY(bool snapService READ snapService CONSTANT)
    Q_PROPERTY(QString snapServiceStatus READ snapServiceStatus NOTIFY snapServiceStatusChanged)

  public:
    explicit BackgroundModeManager(QObject* parent = nullptr);

    bool supported() const;
    bool enabled() const;
    bool snapService() const;
    QString snapServiceStatus() const;
    void syncAutostart();
    Q_INVOKABLE void refreshSnapServiceStatus();

  signals:
    void errorOccurred(const QString& message);
    void snapServiceStatusChanged();

  private:
    static QString quoteDesktopExecArgument(QString argument);

    QString autostartEntryPath() const;
    QString executablePath() const;
    void reportError(const QString& message);
    bool writeAutostart();
    void removeLegacySnapAutostart();
    void setSnapServiceStatus(const QString& status);

    QProcess serviceCheck_;
    QTimer serviceCheckTimeout_;
    QString snapServiceStatus_ = QStringLiteral("checking");
};

class ViewerPreferences final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool startInFullscreen READ startInFullscreen WRITE setStartInFullscreen NOTIFY
                   startInFullscreenChanged)
    Q_PROPERTY(qreal fullscreenBackgroundOpacity READ fullscreenBackgroundOpacity WRITE
                   setFullscreenBackgroundOpacity NOTIFY fullscreenBackgroundOpacityChanged)
    Q_PROPERTY(bool transparencyCheckerboardEnabled READ transparencyCheckerboardEnabled WRITE
                   setTransparencyCheckerboardEnabled NOTIFY transparencyCheckerboardEnabledChanged)
    Q_PROPERTY(qreal transparencyCheckerboardOpacity READ transparencyCheckerboardOpacity WRITE
                   setTransparencyCheckerboardOpacity NOTIFY transparencyCheckerboardOpacityChanged)
    Q_PROPERTY(
        bool windowedTransparencyCheckerboardEnabled READ windowedTransparencyCheckerboardEnabled
            WRITE setWindowedTransparencyCheckerboardEnabled NOTIFY
                windowedTransparencyCheckerboardEnabledChanged)
    Q_PROPERTY(qreal windowedTransparencyOpacity READ windowedTransparencyOpacity WRITE
                   setWindowedTransparencyOpacity NOTIFY windowedTransparencyOpacityChanged)
    Q_PROPERTY(bool animatedImagePlaybackEnabled READ animatedImagePlaybackEnabled WRITE
                   setAnimatedImagePlaybackEnabled NOTIFY animatedImagePlaybackEnabledChanged)
    Q_PROPERTY(qreal animationSpeed READ animationSpeed WRITE setAnimationSpeed NOTIFY
                   animationSpeedChanged)
    Q_PROPERTY(bool smoothImageScalingEnabled READ smoothImageScalingEnabled WRITE
                   setSmoothImageScalingEnabled NOTIFY smoothImageScalingEnabledChanged)
    Q_PROPERTY(bool mipmapImageScalingEnabled READ mipmapImageScalingEnabled WRITE
                   setMipmapImageScalingEnabled NOTIFY mipmapImageScalingEnabledChanged)
    Q_PROPERTY(bool pixelAlignedRenderingEnabled READ pixelAlignedRenderingEnabled WRITE
                   setPixelAlignedRenderingEnabled NOTIFY pixelAlignedRenderingEnabledChanged)
    Q_PROPERTY(bool fullResolutionRenderingEnabled READ fullResolutionRenderingEnabled WRITE
                   setFullResolutionRenderingEnabled NOTIFY fullResolutionRenderingEnabledChanged)
    Q_PROPERTY(bool colorManagedRenderingEnabled READ colorManagedRenderingEnabled WRITE
                   setColorManagedRenderingEnabled NOTIFY colorManagedRenderingEnabledChanged)
    Q_PROPERTY(int maximumImageMegapixels READ maximumImageMegapixels WRITE
                   setMaximumImageMegapixels NOTIFY maximumImageMegapixelsChanged)
    Q_PROPERTY(int maximumImageMemoryMiB READ maximumImageMemoryMiB WRITE setMaximumImageMemoryMiB
                   NOTIFY maximumImageMemoryMiBChanged)
    Q_PROPERTY(int minimumImageMemoryMiB READ minimumImageMemoryMiB CONSTANT)
    Q_PROPERTY(int maximumImageMemoryLimitMiB READ maximumImageMemoryLimitMiB CONSTANT)
    Q_PROPERTY(int minimumImageLimitMegapixels READ minimumImageLimitMegapixels CONSTANT)
    Q_PROPERTY(int maximumImageLimitMegapixels READ maximumImageLimitMegapixels CONSTANT)
    Q_PROPERTY(qreal cropShieldOpacity READ cropShieldOpacity WRITE setCropShieldOpacity NOTIFY
                   cropShieldOpacityChanged)

  public:
    explicit ViewerPreferences(QObject* parent = nullptr);

    bool startInFullscreen() const;
    qreal fullscreenBackgroundOpacity() const;
    bool transparencyCheckerboardEnabled() const;
    qreal transparencyCheckerboardOpacity() const;
    bool windowedTransparencyCheckerboardEnabled() const;
    qreal windowedTransparencyOpacity() const;
    bool animatedImagePlaybackEnabled() const;
    qreal animationSpeed() const;
    bool smoothImageScalingEnabled() const;
    bool mipmapImageScalingEnabled() const;
    bool pixelAlignedRenderingEnabled() const;
    bool fullResolutionRenderingEnabled() const;
    bool colorManagedRenderingEnabled() const;
    int maximumImageMegapixels() const;
    int maximumImageMemoryMiB() const;
    int minimumImageMemoryMiB() const;
    int maximumImageMemoryLimitMiB() const;
    int minimumImageLimitMegapixels() const;
    int maximumImageLimitMegapixels() const;
    qreal cropShieldOpacity() const;

  public slots:
    void setStartInFullscreen(bool value);
    void setFullscreenBackgroundOpacity(qreal value);
    void setTransparencyCheckerboardEnabled(bool value);
    void setTransparencyCheckerboardOpacity(qreal value);
    void setWindowedTransparencyCheckerboardEnabled(bool value);
    void setWindowedTransparencyOpacity(qreal value);
    void setAnimatedImagePlaybackEnabled(bool value);
    void setAnimationSpeed(qreal value);
    void setSmoothImageScalingEnabled(bool value);
    void setMipmapImageScalingEnabled(bool value);
    void setPixelAlignedRenderingEnabled(bool value);
    void setFullResolutionRenderingEnabled(bool value);
    void setColorManagedRenderingEnabled(bool value);
    void setMaximumImageMegapixels(int value);
    void setMaximumImageMemoryMiB(int value);
    void setCropShieldOpacity(qreal value);

  signals:
    void startInFullscreenChanged();
    void fullscreenBackgroundOpacityChanged();
    void transparencyCheckerboardEnabledChanged();
    void transparencyCheckerboardOpacityChanged();
    void windowedTransparencyCheckerboardEnabledChanged();
    void windowedTransparencyOpacityChanged();
    void animatedImagePlaybackEnabledChanged();
    void animationSpeedChanged();
    void smoothImageScalingEnabledChanged();
    void mipmapImageScalingEnabledChanged();
    void pixelAlignedRenderingEnabledChanged();
    void fullResolutionRenderingEnabledChanged();
    void colorManagedRenderingEnabledChanged();
    void maximumImageMegapixelsChanged();
    void maximumImageMemoryMiBChanged();
    void cropShieldOpacityChanged();
    void errorOccurred(const QString& message);

  private:
    void storeValue(const QString& key, const QVariant& value);

    QSettings settings_;
};

} // namespace Licasa
