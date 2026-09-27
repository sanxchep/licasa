#pragma once

#include <QObject>
#include <QRect>
#include <QVariantMap>

class QScreen;

class NativeWindowOps final : public QObject {
    Q_OBJECT

  public:
    explicit NativeWindowOps(QObject* parent = nullptr);

    Q_INVOKABLE bool startSystemMove(QObject* windowObject) const;
    Q_INVOKABLE bool showWindow(QObject* windowObject, bool fullScreen) const;
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
    Q_INVOKABLE bool releaseWindowResources(QObject* windowObject) const;
    Q_INVOKABLE bool trimProcessMemory() const;

  signals:
    void screensChanged();

  private:
    void watchScreen(QScreen* screen);
    static QVariantMap rectToMap(const QRect& rect);
};
