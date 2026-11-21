#pragma once

#include <QHash>
#include <QObject>
#include <QRect>
#include <QVariantMap>

#include <optional>

class QQuickWindow;
class QScreen;

class NativeWindowOps final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool nativeWayland READ nativeWayland CONSTANT)
    Q_PROPERTY(bool gnomeWayland READ gnomeWayland CONSTANT)

  public:
    explicit NativeWindowOps(QObject* parent = nullptr);

    enum class FullscreenPlatform { Other, NativeWayland, GnomeWayland };
    static FullscreenPlatform fullscreenPlatform(const QString& qpa, const QString& desktop);
    bool nativeWayland() const;
    bool gnomeWayland() const;

    Q_INVOKABLE bool startSystemMove(QObject* windowObject) const;
    Q_INVOKABLE bool showWindow(QObject* windowObject, bool fullScreen, bool showGnomeTopBar);
    Q_INVOKABLE QVariantMap currentWindowGeometry(QObject* windowObject) const;
    Q_INVOKABLE QVariantMap currentScreenGeometry(QObject* windowObject) const;
    Q_INVOKABLE QVariantMap globalCursorPosition() const;
    Q_INVOKABLE bool leftMouseButtonPressed() const;
    Q_INVOKABLE qreal windowDevicePixelRatio(QObject* windowObject) const;
    Q_INVOKABLE bool setWindowGeometry(QObject* windowObject, int x, int y, int width,
                                       int height) const;
    Q_INVOKABLE bool setWindowPosition(QObject* windowObject, int x, int y) const;
    Q_INVOKABLE bool gestureWindowSupported() const;
    Q_INVOKABLE bool setWindowInputRectangle(QObject* windowObject, int x, int y, int width,
                                             int height) const;
    Q_INVOKABLE bool beginGestureWindows(QObject* exactObject, QObject* gestureObject) const;
    Q_INVOKABLE bool endGestureWindows(QObject* gestureObject, QObject* exactObject) const;
    Q_INVOKABLE bool requestWindowFrame(QObject* windowObject) const;
    Q_INVOKABLE void traceZoom(const QString& phase, qreal scale) const;
    Q_INVOKABLE bool sameImageSource(const QString& actual, const QString& requested) const;
    Q_INVOKABLE bool releaseWindowResources(QObject* windowObject) const;
    Q_INVOKABLE bool trimProcessMemory() const;

  signals:
    void screensChanged();
    void fullscreenMaximizationChanged(QObject* window, bool maximized);

  private:
    struct GnomeWindowState {
        Qt::WindowState target = Qt::WindowNoState;
        Qt::WindowState effective = Qt::WindowNoState;
        std::optional<Qt::WindowState> requested;
        bool leavingOccupiedState = false;
        bool replacingPendingMaximization = false;
    };

    bool eventFilter(QObject* watched, QEvent* event) override;
    void applyGnomeWindowState(QQuickWindow* window);
    const FullscreenPlatform fullscreenPlatform_;
    QHash<QQuickWindow*, GnomeWindowState> gnomeWindowStates_;
    void watchScreen(QScreen* screen);
    static QVariantMap rectToMap(const QRect& rect);
};
