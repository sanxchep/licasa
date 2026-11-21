#include "platform/native_window_ops.h"

#include <QCursor>
#include <QEvent>
#include <QGuiApplication>
#include <QPointer>
#include <QQuickWindow>
#include <QScreen>
#include <QStringList>
#include <QUrl>

#include <algorithm>
#include <chrono>
#include <cstdio>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#if defined(LICASA_HAVE_X11_POINTER_QUERY) && QT_CONFIG(xcb)
#define LICASA_USE_X11_POINTER_QUERY 1
#endif

#ifdef LICASA_USE_X11_POINTER_QUERY
#include <QtGui/qguiapplication_platform.h>

#include <X11/Xlib.h>
#ifdef LICASA_HAVE_X11_SHAPE
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#endif
#endif

namespace {

bool zoomTraceEnabled()
{
    static const bool enabled = qEnvironmentVariableIsSet("LICASA_ZOOM_TRACE");
    return enabled;
}

long long zoomTraceUs()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

#ifdef LICASA_USE_X11_POINTER_QUERY
Display* x11Display()
{
    if (QGuiApplication::platformName() != QStringLiteral("xcb")) {
        return nullptr;
    }

    auto* nativeInterface = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    Display* display = nativeInterface ? nativeInterface->display() : nullptr;
    return display;
}
#ifdef LICASA_HAVE_X11_SHAPE
void setX11WindowOpacity(Display* display, Atom property, ::Window window, unsigned long opacity)
{
    XChangeProperty(display, window, property, XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&opacity), 1);
}
#endif
#endif

} // namespace

NativeWindowOps::NativeWindowOps(QObject* parent)
    : QObject(parent),
      fullscreenPlatform_(fullscreenPlatform(QGuiApplication::platformName(),
                                             qEnvironmentVariable("XDG_CURRENT_DESKTOP")))
{
    for (QScreen* screen : QGuiApplication::screens()) {
        watchScreen(screen);
    }
    QObject::connect(qGuiApp, &QGuiApplication::screenAdded, this, [this](QScreen* screen) {
        watchScreen(screen);
        emit screensChanged();
    });
    QObject::connect(qGuiApp, &QGuiApplication::screenRemoved, this,
                     [this](QScreen*) { emit screensChanged(); });
    QObject::connect(qGuiApp, &QGuiApplication::primaryScreenChanged, this,
                     [this](QScreen*) { emit screensChanged(); });
}

void NativeWindowOps::watchScreen(QScreen* screen)
{
    if (!screen) {
        return;
    }
    QObject::connect(screen, &QScreen::geometryChanged, this, [this] { emit screensChanged(); });
}

NativeWindowOps::FullscreenPlatform NativeWindowOps::fullscreenPlatform(const QString& qpa,
                                                                        const QString& desktop)
{
    if (qpa != QStringLiteral("wayland") && !qpa.startsWith(QStringLiteral("wayland-"))) {
        return FullscreenPlatform::Other;
    }
    return desktop.split(QLatin1Char(':')).contains(QStringLiteral("GNOME"), Qt::CaseInsensitive)
               ? FullscreenPlatform::GnomeWayland
               : FullscreenPlatform::NativeWayland;
}

bool NativeWindowOps::nativeWayland() const
{
    return fullscreenPlatform_ != FullscreenPlatform::Other;
}

bool NativeWindowOps::gnomeWayland() const
{
    return fullscreenPlatform_ == FullscreenPlatform::GnomeWayland;
}

bool NativeWindowOps::startSystemMove(QObject* windowObject) const
{
    auto* window = qobject_cast<QQuickWindow*>(windowObject);
    return window && window->startSystemMove();
}

bool NativeWindowOps::showWindow(QObject* windowObject, bool fullScreen, bool showGnomeTopBar)
{
    auto* window = qobject_cast<QQuickWindow*>(windowObject);
    if (!window) {
        return false;
    }

    if (gnomeWayland()) {
        if (!gnomeWindowStates_.contains(window)) {
            window->installEventFilter(this);
            connect(window, &QObject::destroyed, this,
                    [this, window] { gnomeWindowStates_.remove(window); });
        }
        auto& state = gnomeWindowStates_[window];
        state.target = !fullScreen       ? Qt::WindowNoState
                       : showGnomeTopBar ? Qt::WindowMaximized
                                         : Qt::WindowFullScreen;
        applyGnomeWindowState(window);
    } else if (fullScreen) {
        window->showFullScreen();
    } else {
        window->showNormal();
    }
    return true;
}

void NativeWindowOps::applyGnomeWindowState(QQuickWindow* window)
{
    auto& state = gnomeWindowStates_[window];
    if (state.leavingOccupiedState || state.requested == state.target) {
        return;
    }

    // Qt 6.4 sends set_maximized before unset_fullscreen. Mutter can then
    // configure a maximized window with its old floating bounds. Leave the
    // occupied state first and send the latest request after the normal configure.
    state.leavingOccupiedState = state.target != Qt::WindowNoState &&
                                 state.effective != Qt::WindowNoState &&
                                 state.effective != state.target;
    state.replacingPendingMaximization = state.target == Qt::WindowFullScreen &&
                                         state.requested == Qt::WindowMaximized &&
                                         state.effective == Qt::WindowNoState;
    state.requested = state.leavingOccupiedState ? Qt::WindowNoState : state.target;
    switch (*state.requested) {
    case Qt::WindowMaximized:
        window->showMaximized();
        break;
    case Qt::WindowFullScreen:
        window->showFullScreen();
        break;
    default:
        window->showNormal();
        break;
    }
}

bool NativeWindowOps::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() != QEvent::Hide && event->type() != QEvent::WindowStateChange) {
        return QObject::eventFilter(watched, event);
    }
    auto* window = static_cast<QQuickWindow*>(watched);
    if (event->type() == QEvent::Hide) {
        gnomeWindowStates_[window] = {};
    }
    // QWindow reports requested states immediately. A spontaneous state event
    // carries the compositor's configure.
    if (event->type() == QEvent::WindowStateChange && event->spontaneous() && window->isVisible()) {
        auto& state = gnomeWindowStates_[window];
        state.effective = window->windowState();
        emit fullscreenMaximizationChanged(window, state.effective == Qt::WindowMaximized);
        const bool normalConfirmed =
            state.leavingOccupiedState && state.effective == Qt::WindowNoState;
        const bool supersededMaximization = state.replacingPendingMaximization &&
                                            state.target == Qt::WindowFullScreen &&
                                            window->windowStates().testFlag(Qt::WindowMaximized);
        if (supersededMaximization) {
            // The fullscreen request sent while maximization was pending did
            // not unset it. Reconcile the latest target once Qt sees the old state.
            state.replacingPendingMaximization = false;
            state.requested.reset();
        }
        if (normalConfirmed || supersededMaximization) {
            if (normalConfirmed) {
                state.leavingOccupiedState = false;
            }
            QMetaObject::invokeMethod(
                this,
                [this, window = QPointer<QQuickWindow>(window)] {
                    if (window && window->isVisible()) {
                        applyGnomeWindowState(window);
                    }
                },
                Qt::QueuedConnection);
        }
    }
    return QObject::eventFilter(watched, event);
}

QVariantMap NativeWindowOps::currentWindowGeometry(QObject* windowObject) const
{
    auto* window = qobject_cast<QQuickWindow*>(windowObject);
    return window ? rectToMap(window->geometry()) : QVariantMap{};
}

QVariantMap NativeWindowOps::currentScreenGeometry(QObject* windowObject) const
{
    auto* window = qobject_cast<QQuickWindow*>(windowObject);
    QScreen* screen = window ? window->screen() : QGuiApplication::primaryScreen();
    return screen ? rectToMap(screen->geometry()) : QVariantMap{};
}

QVariantMap NativeWindowOps::globalCursorPosition() const
{
    const QPoint position = QCursor::pos();
    return {
        {QStringLiteral("x"), position.x()},
        {QStringLiteral("y"), position.y()},
    };
}

bool NativeWindowOps::leftMouseButtonPressed() const
{
#ifdef LICASA_USE_X11_POINTER_QUERY
    if (Display* display = x11Display()) {
        ::Window root = DefaultRootWindow(display);
        ::Window rootReturn = None;
        ::Window childReturn = None;
        int rootX = 0;
        int rootY = 0;
        int windowX = 0;
        int windowY = 0;
        unsigned int buttonMask = 0;
        if (XQueryPointer(display, root, &rootReturn, &childReturn, &rootX, &rootY, &windowX,
                          &windowY, &buttonMask)) {
            return (buttonMask & Button1Mask) != 0;
        }
    }
#endif

    return QGuiApplication::mouseButtons().testFlag(Qt::LeftButton);
}

qreal NativeWindowOps::windowDevicePixelRatio(QObject* windowObject) const
{
    auto* window = qobject_cast<QQuickWindow*>(windowObject);
    return window ? std::max<qreal>(1.0, window->devicePixelRatio()) : 1.0;
}

bool NativeWindowOps::setWindowGeometry(QObject* windowObject, int x, int y, int width,
                                        int height) const
{
    auto* window = qobject_cast<QQuickWindow*>(windowObject);
    if (!window || width <= 0 || height <= 0) {
        return false;
    }

    if (zoomTraceEnabled()) {
        if (!window->property("_licasa_zoom_trace_connected").toBool()) {
            window->setProperty("_licasa_zoom_trace_connected", true);
            QObject::connect(
                window, &QQuickWindow::frameSwapped, window,
                [] { std::fprintf(stderr, "ZOOMTRACE %lld frame-swap\n", zoomTraceUs()); },
                Qt::DirectConnection);
            QObject::connect(window, &QWindow::xChanged, window, [window] {
                const QRect g = window->geometry();
                std::fprintf(stderr, "ZOOMTRACE %lld geometry %d %d %d %d\n", zoomTraceUs(), g.x(),
                             g.y(), g.width(), g.height());
            });
            QObject::connect(window, &QWindow::widthChanged, window, [window] {
                const QRect g = window->geometry();
                std::fprintf(stderr, "ZOOMTRACE %lld geometry %d %d %d %d\n", zoomTraceUs(), g.x(),
                             g.y(), g.width(), g.height());
            });
        }
        std::fprintf(stderr, "ZOOMTRACE %lld request %d %d %d %d\n", zoomTraceUs(), x, y, width,
                     height);
    }

    window->setGeometry(x, y, width, height);
    return true;
}

void NativeWindowOps::traceZoom(const QString& phase, qreal scale) const
{
    if (zoomTraceEnabled()) {
        const QByteArray label = phase.toUtf8();
        std::fprintf(stderr, "ZOOMTRACE %lld %s %.6f\n", zoomTraceUs(), label.constData(), scale);
    }
}

bool NativeWindowOps::sameImageSource(const QString& actual, const QString& requested) const
{
    // QML Image.source exposes QUrl's display form, which decodes spaces in
    // filenames. Compare parsed URLs rather than their string spellings.
    return !actual.isEmpty() && !requested.isEmpty() && QUrl(actual) == QUrl(requested);
}

bool NativeWindowOps::setWindowPosition(QObject* windowObject, int x, int y) const
{
    auto* window = qobject_cast<QQuickWindow*>(windowObject);
    if (!window) {
        return false;
    }

    window->setPosition(x, y);
    return true;
}

bool NativeWindowOps::gestureWindowSupported() const
{
#if defined(LICASA_USE_X11_POINTER_QUERY) && defined(LICASA_HAVE_X11_SHAPE)
    if (qEnvironmentVariableIsSet("LICASA_DISABLE_FLOATING_GESTURE")) {
        return false;
    }
    Display* display = x11Display();
    int eventBase = 0;
    int errorBase = 0;
    return display && XShapeQueryExtension(display, &eventBase, &errorBase);
#else
    return false;
#endif
}

bool NativeWindowOps::setWindowInputRectangle(QObject* windowObject, int x, int y, int width,
                                              int height) const
{
#if defined(LICASA_USE_X11_POINTER_QUERY) && defined(LICASA_HAVE_X11_SHAPE)
    auto* window = qobject_cast<QQuickWindow*>(windowObject);
    Display* display = x11Display();
    if (!window || !display) {
        return false;
    }

    // Only the input region changes. Bounding-region updates created stale
    // compositor pixels in the isolated desktop experiment.
    XRectangle rectangle{static_cast<short>(x), static_cast<short>(y),
                         static_cast<unsigned short>(std::max(0, width)),
                         static_cast<unsigned short>(std::max(0, height))};
    XShapeCombineRectangles(display, static_cast<::Window>(window->winId()), ShapeInput, 0, 0,
                            width > 0 && height > 0 ? &rectangle : nullptr,
                            width > 0 && height > 0 ? 1 : 0, ShapeSet, Unsorted);
    XFlush(display);
    return true;
#else
    Q_UNUSED(windowObject)
    Q_UNUSED(x)
    Q_UNUSED(y)
    Q_UNUSED(width)
    Q_UNUSED(height)
    return false;
#endif
}

bool NativeWindowOps::beginGestureWindows(QObject* exactObject, QObject* gestureObject) const
{
#if defined(LICASA_USE_X11_POINTER_QUERY) && defined(LICASA_HAVE_X11_SHAPE)
    auto* exact = qobject_cast<QQuickWindow*>(exactObject);
    auto* gesture = qobject_cast<QQuickWindow*>(gestureObject);
    Display* display = x11Display();
    if (!exact || !gesture || !display) {
        return false;
    }
    const ::Window exactId = static_cast<::Window>(exact->winId());
    const ::Window gestureId = static_cast<::Window>(gesture->winId());
    const Atom opacity = XInternAtom(display, "_NET_WM_WINDOW_OPACITY", False);

    // Both surfaces remain on screen and keep painting. Native moves of the
    // exact window let GNOME discard its texture, causing a blank return.
    XGrabServer(display);
    XShapeCombineRectangles(display, exactId, ShapeInput, 0, 0, nullptr, 0, ShapeSet, Unsorted);
    setX11WindowOpacity(display, opacity, exactId, 0);
    setX11WindowOpacity(display, opacity, gestureId, 0xffffffffUL);
    XRaiseWindow(display, gestureId);
    XUngrabServer(display);
    XFlush(display);
    return true;
#else
    Q_UNUSED(exactObject)
    Q_UNUSED(gestureObject)
    return false;
#endif
}

bool NativeWindowOps::endGestureWindows(QObject* gestureObject, QObject* exactObject) const
{
#if defined(LICASA_USE_X11_POINTER_QUERY) && defined(LICASA_HAVE_X11_SHAPE)
    auto* gesture = qobject_cast<QQuickWindow*>(gestureObject);
    auto* exact = qobject_cast<QQuickWindow*>(exactObject);
    Display* display = x11Display();
    if (!gesture || !exact || !display) {
        return false;
    }
    const ::Window gestureId = static_cast<::Window>(gesture->winId());
    const ::Window exactId = static_cast<::Window>(exact->winId());
    const Atom opacity = XInternAtom(display, "_NET_WM_WINDOW_OPACITY", False);

    XGrabServer(display);
    XShapeCombineRectangles(display, gestureId, ShapeInput, 0, 0, nullptr, 0, ShapeSet, Unsorted);
    // None removes the exact window's custom input region, so later normal
    // resizes retain input over their entire image-sized native bounds.
    XShapeCombineMask(display, exactId, ShapeInput, 0, 0, None, ShapeSet);
    setX11WindowOpacity(display, opacity, exactId, 0xffffffffUL);
    setX11WindowOpacity(display, opacity, gestureId, 0);
    XRaiseWindow(display, exactId);
    XUngrabServer(display);
    XFlush(display);
    return true;
#else
    Q_UNUSED(gestureObject)
    Q_UNUSED(exactObject)
    return false;
#endif
}

bool NativeWindowOps::requestWindowFrame(QObject* windowObject) const
{
    auto* window = qobject_cast<QQuickWindow*>(windowObject);
    if (!window) {
        return false;
    }
    window->update();
    return true;
}

bool NativeWindowOps::releaseWindowResources(QObject* windowObject) const
{
    auto* window = qobject_cast<QQuickWindow*>(windowObject);
    if (!window) {
        return false;
    }

    window->releaseResources();
    return true;
}

bool NativeWindowOps::trimProcessMemory() const
{
#if defined(__GLIBC__)
    return malloc_trim(0) != 0;
#else
    return false;
#endif
}

QVariantMap NativeWindowOps::rectToMap(const QRect& rect)
{
    return {{QStringLiteral("x"), rect.x()},
            {QStringLiteral("y"), rect.y()},
            {QStringLiteral("width"), rect.width()},
            {QStringLiteral("height"), rect.height()}};
}
