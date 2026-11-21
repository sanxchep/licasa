#include "wayland_hdr.h"
#include "color-management-client.h"
#include <QAbstractEventDispatcher>
#include <QElapsedTimer>
#include <QCryptographicHash>
#include <array>
#include <atomic>
#include <QGuiApplication>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QQuickWindow>
#include <QTimer>
#include <qpa/qplatformwindow_p.h>
#include <vector>
#include <wayland-client.h>
#include <algorithm>
#include <mutex>
#include <unistd.h>

namespace Licasa {
struct WaylandHdrObserver::Impl {
    using State = WaylandHdrReport::State;
    WaylandHdrObserver* owner;
    wl_display* display;
    wl_event_queue* queue = nullptr;
    wl_registry* registry = nullptr;
    wl_callback* sync = nullptr;
    wp_color_manager_v1* manager = nullptr;
    wp_image_description_v1* acceptance = nullptr;
    wp_color_management_surface_v1* colorSurface = nullptr;
    std::atomic_bool linearTagged{false};
    wp_color_management_surface_feedback_v1* feedback = nullptr;
    wp_image_description_v1* preferred = nullptr;
    wl_surface* surface = nullptr; // Borrowed; never destroyed or re-queued.
    QPointer<QQuickWindow> window;
    std::vector<QMetaObject::Connection> surfaceConnections;
    quint64 nativeEpoch=0, surfaceEpoch=0;
    uint32_t managerName = 0;
    bool params=false, extended=false, mastering=false, linear=false, srgb=false, intent=false, done=false;
    bool accepted=false, terminal=false, notified=false, changing=false;
    QTimer pump;
    QElapsedTimer deadline;
    std::function<void()> changed;
    mutable std::mutex mutex;
    WaylandHdrReport value;
    struct Info {
        Impl* self;
        wp_image_description_info_v1* proxy = nullptr;
        uint32_t identity=0, reference=0, maximum=0;
        bool luminances=false, target=false, tf=false, primaries=false, targetPrimaries=false, invalid=false;
        std::array<qint64,26> fields{};
        ~Info() { if (proxy) wp_image_description_info_v1_destroy(proxy); }
    };
    std::unique_ptr<Info> info;
    explicit Impl(WaylandHdrObserver* o,wl_display* d,std::function<void()> cb)
        : owner(o),display(d),changed(std::move(cb))
    {
        if (!display) { publish(State::Unavailable,"no_native_display"); return; }
        queue=wl_display_create_queue(display);
        if (!queue) { publish(State::Unavailable,"queue_failed"); return; }
        auto* wrapper=static_cast<wl_display*>(wl_proxy_create_wrapper(display));
        if (!wrapper) { publish(State::Unavailable,"queue_failed"); return; }
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper),queue);
        registry=wl_display_get_registry(wrapper);
        sync=wl_display_sync(wrapper);
        wl_proxy_wrapper_destroy(wrapper);
        if (!registry || !sync) { publish(State::Unavailable,"connection_failed"); return; }
        static const wl_registry_listener registryListener{global,removed};
        wl_registry_add_listener(registry,&registryListener,this);
        static const wl_callback_listener syncListener{registryDone};
        wl_callback_add_listener(sync,&syncListener,this);
        deadline.start();
        pump.setInterval(250);
        QObject::connect(&pump,&QTimer::timeout,owner,[this] { dispatch(); });
        if (auto* dispatcher=QAbstractEventDispatcher::instance(owner->thread()))
            QObject::connect(dispatcher,&QAbstractEventDispatcher::aboutToBlock,owner,[this] { dispatch(); });
        pump.start(); wl_display_flush(display);
    }
    ~Impl() {
        pump.stop();
        for (const auto& c:surfaceConnections) QObject::disconnect(c);
        clearColorSurface(); clearFeedback(); clearManager();
        if (sync) wl_callback_destroy(sync);
        if (registry) wl_registry_destroy(registry);
        if (queue) wl_event_queue_destroy(queue);
        // Qt owns the connection, event reader and wl_surface.
    }
    WaylandHdrReport snapshot() const { std::lock_guard lock(mutex); return value; }
    void publish(State state,const QString& reason,uint32_t identity=0,double reference=0,double maximum=0,const QByteArray& fingerprint={}) {
        {
            std::lock_guard lock(mutex);
            if (value.state==state && value.reason==reason && value.protocol==bool(manager)
                && value.descriptionAccepted==accepted && value.preferredIdentity==identity
                && value.referenceNits==reference && value.targetNits==maximum && value.preferredChanged==changing
                && value.preferredFingerprint==fingerprint && value.surfaceEpoch==surfaceEpoch) return;
            value={state,reason,value.revision+1,identity,reference,maximum,bool(manager),accepted,changing,fingerprint,bool(surface),surfaceEpoch};
        }
        if (!notified) {
            notified=true;
            QTimer::singleShot(0,owner,[this] { notified=false; if (changed) changed(); });
        }
    }
    void dispatch() {
        if (!queue || (terminal && wl_display_get_error(display))) return;
        if (wl_display_get_error(display) || wl_display_dispatch_queue_pending(display,queue)<0) {
            terminal=true; publish(State::Unavailable,"connection_failed"); return;
        }
        wl_display_flush(display); // EAGAIN is left to Qt's normal socket writer.
        if (snapshot().state==State::Pending && deadline.isValid() && deadline.elapsed()>2000) {
            terminal=true; publish(State::Unavailable,"protocol_timeout");
        }
    }
    void clearFeedback() {
        info.reset();
        if (preferred) { wp_image_description_v1_destroy(preferred); preferred=nullptr; }
        if (feedback) { wp_color_management_surface_feedback_v1_destroy(feedback); feedback=nullptr; }
    }
    void clearColorSurface() {
        linearTagged = false;
        if (colorSurface) { wp_color_management_surface_v1_destroy(colorSurface); colorSurface=nullptr; }
    }
    bool tagLinearSurface() {
        if (colorSurface) return linearTagged.load();
        if (!surface || !manager || !acceptance || snapshot().state!=State::Ready
            || wl_display_get_error(display)) return false;
        colorSurface=wp_color_manager_v1_get_surface(manager,surface);
        if (!colorSurface) return false;
        wp_color_management_surface_v1_set_image_description(
            colorSurface,acceptance,WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
        // The description is ready and its assignment precedes the next
        // rendered commit on this connection. The observer never commits.
        linearTagged = true;
        return true;
    }
    void clearManager() {
        if (acceptance) { wp_image_description_v1_destroy(acceptance); acceptance=nullptr; }
        if (manager) { wp_color_manager_v1_destroy(manager); manager=nullptr; }
        accepted=done=params=extended=mastering=linear=srgb=intent=false;
    }
    void setSurface(wl_surface* next) {
        if (surface==next) return;
        clearColorSurface(); clearFeedback(); surface=next; ++surfaceEpoch; changing=false;
        if (next && done && accepted && !wl_display_get_error(display)) terminal=false;
        if (terminal || !done) {
            const auto old=snapshot();
            publish(old.state,old.reason,0,0,0);
            return;
        }
        publish(State::Pending,"surface_pending");
        if (next) deadline.start(); else deadline.invalidate();
        requestFeedback();
    }
    void requestFeedback() {
        if (!manager || !done || terminal || !surface || feedback) return;
        feedback=wp_color_manager_v1_get_surface_feedback(manager,surface);
        if (!feedback) { terminal=true; publish(State::Unavailable,"connection_failed"); return; }
        static const wp_color_management_surface_feedback_v1_listener listener{preferredChanged};
        wp_color_management_surface_feedback_v1_add_listener(feedback,&listener,this);
        requestPreferred();
    }
    void requestPreferred() {
        info.reset();
        if (preferred) wp_image_description_v1_destroy(preferred);
        preferred=wp_color_management_surface_feedback_v1_get_preferred_parametric(feedback);
        if (!preferred) { terminal=true; publish(State::Unavailable,"connection_failed"); return; }
        static const wp_image_description_v1_listener listener{preferredFailed,preferredReady};
        wp_image_description_v1_add_listener(preferred,&listener,this);
        publish(State::Pending,"preferred_pending"); deadline.start(); wl_display_flush(display);
    }
    static void global(void* data,wl_registry* registry,uint32_t name,const char* interface,uint32_t version) {
        auto& s=*static_cast<Impl*>(data);
        if (QByteArray(interface)!="wp_color_manager_v1" || !version || s.manager) return;
        s.managerName=name; s.terminal=false;
        s.manager=static_cast<wp_color_manager_v1*>(wl_registry_bind(registry,name,&wp_color_manager_v1_interface,1));
        static const wp_color_manager_v1_listener listener{
            [](void* p,wp_color_manager_v1*,uint32_t intent) { if (intent==WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL) static_cast<Impl*>(p)->intent=true; },
            [](void* p,wp_color_manager_v1*,uint32_t f) {
                auto& s=*static_cast<Impl*>(p);
                if (f==WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC) s.params=true;
                if (f==WP_COLOR_MANAGER_V1_FEATURE_EXTENDED_TARGET_VOLUME) s.extended=true;
                if (f==WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES) s.mastering=true;
            },
            [](void* p,wp_color_manager_v1*,uint32_t tf) { if (tf==WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR) static_cast<Impl*>(p)->linear=true; },
            [](void* p,wp_color_manager_v1*,uint32_t primary) { if (primary==WP_COLOR_MANAGER_V1_PRIMARIES_SRGB) static_cast<Impl*>(p)->srgb=true; },
            managerDone};
        wp_color_manager_v1_add_listener(s.manager,&listener,&s);
        s.publish(State::Pending,"capabilities_pending"); s.deadline.start();
    }
    static void removed(void* data,wl_registry*,uint32_t name) {
        auto& s=*static_cast<Impl*>(data);
        if (name!=s.managerName) return;
        s.clearFeedback(); s.clearManager(); s.changing=true;
        s.publish(State::Unavailable,"color_manager_removed");
    }
    static void registryDone(void* data,wl_callback* callback,uint32_t) {
        auto& s=*static_cast<Impl*>(data); wl_callback_destroy(callback); s.sync=nullptr;
        if (!s.manager) s.publish(State::Unavailable,"no_color_manager");
    }
    static void managerDone(void* data,wp_color_manager_v1*) {
        auto& s=*static_cast<Impl*>(data);
        if (s.done || s.terminal) return;
        if (!(s.params && s.extended && s.mastering && s.linear && s.srgb && s.intent)) {
            s.terminal=true; s.publish(State::Unavailable,"linear_hdr_not_advertised"); return;
        }
        s.done=true;
        // Admit the 80-nit extended sRGB description with a 10000-nit
        // target before any decision to use it on a pass-through surface.
        auto* creator=wp_color_manager_v1_create_parametric_creator(s.manager);
        if (!creator) { s.terminal=true; s.publish(State::Unavailable,"connection_failed"); return; }
        wp_image_description_creator_params_v1_set_primaries_named(creator,WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
        wp_image_description_creator_params_v1_set_tf_named(creator,WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR);
        wp_image_description_creator_params_v1_set_mastering_luminance(creator,0,10000);
        s.acceptance=wp_image_description_creator_params_v1_create(creator); // Consumes creator.
        if (!s.acceptance) { s.terminal=true; s.publish(State::Unavailable,"connection_failed"); return; }
        static const wp_image_description_v1_listener listener{
            [](void* p,wp_image_description_v1*,uint32_t,const char*) {
                auto& s=*static_cast<Impl*>(p); s.terminal=true; s.publish(State::Unavailable,"description_rejected");
            },
            [](void* p,wp_image_description_v1*,uint32_t identity) {
                auto& s=*static_cast<Impl*>(p);
                if (s.terminal) return;
                if (!identity) { s.terminal=true; s.publish(State::Unavailable,"invalid_description_identity"); return; }
                s.accepted=true;
                if (s.snapshot().preferredIdentity) s.complete();
                else {
                    s.publish(State::Pending,s.surface?"preferred_pending":"surface_pending");
                    if (!s.surface) s.deadline.invalidate();
                }
            }};
        wp_image_description_v1_add_listener(s.acceptance,&listener,&s);
        s.deadline.start(); s.requestFeedback(); wl_display_flush(s.display);
    }
    static void preferredChanged(void* data,wp_color_management_surface_feedback_v1*,uint32_t identity) {
        auto& s=*static_cast<Impl*>(data);
        const auto old=s.snapshot().preferredIdentity;
        if (old && old==identity) return;
        s.changing |= old!=0;
        if (s.done && s.accepted && !wl_display_get_error(s.display)) s.terminal=false;
        s.requestPreferred();
    }
    static void preferredFailed(void* data,wp_image_description_v1*,uint32_t,const char*) {
        auto& s=*static_cast<Impl*>(data); s.terminal=true; s.publish(State::Unavailable,"preferred_rejected");
    }
    static void preferredReady(void* data,wp_image_description_v1*,uint32_t identity) {
        auto& s=*static_cast<Impl*>(data);
        if (s.terminal) return;
        if (!identity) { s.terminal=true; s.publish(State::Unavailable,"invalid_preferred_information"); return; }
        s.info=std::make_unique<Info>(); s.info->self=&s; s.info->identity=identity;
        s.info->proxy=wp_image_description_v1_get_information(s.preferred);
        if (!s.info->proxy) { s.info.reset(); s.terminal=true; s.publish(State::Unavailable,"connection_failed"); return; }
        static const auto listener=[] {
            wp_image_description_info_v1_listener l{};
            l.done=[](void* p,wp_image_description_info_v1*) {
                auto& i=*static_cast<Info*>(p); auto& s=*i.self;
                if (!(i.luminances && i.target && i.tf && i.primaries) || !i.reference || !i.maximum || i.invalid) {
                    s.terminal=true; s.publish(State::Unavailable,"invalid_preferred_information");
                } else {
                    // The event-specific v1 rule permits omitted target primaries
                    // when they equal the primary color volume. Normalize that
                    // representation before comparing metadata across surfaces.
                    if (!i.targetPrimaries) std::copy_n(i.fields.begin(),8,i.fields.begin()+13);
                    const auto fields=QByteArray::fromRawData(reinterpret_cast<const char*>(i.fields.data()),sizeof(i.fields));
                    s.publish(State::Pending,"description_pending",i.identity,i.reference,i.maximum,
                              QCryptographicHash::hash(fields,QCryptographicHash::Sha256));
                    s.complete();
                }
                s.info.reset(); // done destroys server resource; discard local proxy.
            };
            l.icc_file=[](void* p,wp_image_description_info_v1*,int32_t fd,uint32_t) { close(fd); static_cast<Info*>(p)->invalid=true; };
            l.primaries=[](void* p,wp_image_description_info_v1*,int32_t rx,int32_t ry,int32_t gx,int32_t gy,int32_t bx,int32_t by,int32_t wx,int32_t wy) {
                auto& i=*static_cast<Info*>(p); i.primaries=true;
                const qint64 values[]{rx,ry,gx,gy,bx,by,wx,wy}; std::copy(std::begin(values),std::end(values),i.fields.begin());
            };
            l.primaries_named=[](void* p,wp_image_description_info_v1*,uint32_t v) { static_cast<Info*>(p)->fields[25]=v; };
            l.tf_power=[](void* p,wp_image_description_info_v1*,uint32_t v) { auto& i=*static_cast<Info*>(p); i.tf=true; i.invalid|=!v; i.fields[9]=v; };
            l.tf_named=[](void* p,wp_image_description_info_v1*,uint32_t v) { auto& i=*static_cast<Info*>(p); i.tf=true; i.invalid|=!v; i.fields[8]=v; };
            l.luminances=[](void* p,wp_image_description_info_v1*,uint32_t min,uint32_t max,uint32_t ref) {
                auto& i=*static_cast<Info*>(p); i.luminances=true; i.reference=ref; i.fields[10]=min; i.fields[11]=max; i.fields[12]=ref;
            };
            l.target_primaries=[](void* p,wp_image_description_info_v1*,int32_t rx,int32_t ry,int32_t gx,int32_t gy,int32_t bx,int32_t by,int32_t wx,int32_t wy) {
                auto& i=*static_cast<Info*>(p); i.targetPrimaries=true;
                const qint64 values[]{rx,ry,gx,gy,bx,by,wx,wy}; std::copy(std::begin(values),std::end(values),i.fields.begin()+13);
            };
            l.target_luminance=[](void* p,wp_image_description_info_v1*,uint32_t min,uint32_t max) { auto& i=*static_cast<Info*>(p); i.target=true; i.maximum=max; i.fields[21]=min; i.fields[22]=max; };
            l.target_max_cll=[](void* p,wp_image_description_info_v1*,uint32_t v) { static_cast<Info*>(p)->fields[23]=v; };
            l.target_max_fall=[](void* p,wp_image_description_info_v1*,uint32_t v) { static_cast<Info*>(p)->fields[24]=v; };
            return l;
        }();
        wp_image_description_info_v1_add_listener(s.info->proxy,&listener,s.info.get());
    }
    void complete() {
        const auto report=snapshot();
        if (terminal || !accepted || !report.preferredIdentity) return;
        publish(report.targetNits>report.referenceNits?State::Ready:State::Sdr,
                report.targetNits>report.referenceNits?"hdr_preferred":"sdr_preferred",
                report.preferredIdentity,report.referenceNits,report.targetNits,report.preferredFingerprint);
    }
};
WaylandHdrObserver::WaylandHdrObserver(wl_display* display,std::function<void()> changed,QObject* parent)
    : QObject(parent),d_(std::make_unique<Impl>(this,display,std::move(changed))) {}
WaylandHdrObserver::~WaylandHdrObserver() {
    if (d_->window) d_->window->removeEventFilter(this);
}
void WaylandHdrObserver::setSurface(wl_surface* surface) { d_->setSurface(surface); }
WaylandHdrReport WaylandHdrObserver::report() const { return d_->snapshot(); }
bool WaylandHdrObserver::tagLinearSurface() { return d_->tagLinearSurface(); }
bool WaylandHdrObserver::linearSurfaceTagged() const { return d_->linearTagged.load(); }
void WaylandHdrObserver::watchWindow(QQuickWindow* window) {
    if (d_->window) d_->window->removeEventFilter(this);
    d_->window=window;
    if (window) { window->installEventFilter(this); if (window->handle()) attachNativeSurface(); }
}
void WaylandHdrObserver::attachNativeSurface() {
    const auto epoch=++d_->nativeEpoch;
    for (const auto& c:d_->surfaceConnections) QObject::disconnect(c);
    d_->surfaceConnections.clear();
#if QT_CONFIG(wayland)
    auto* native=d_->window ? d_->window->nativeInterface<QNativeInterface::Private::QWaylandWindow>() : nullptr;
    if (!native) { setSurface(nullptr); return; }
    QPointer<QNativeInterface::Private::QWaylandWindow> guarded(native);
    d_->surfaceConnections.push_back(connect(native,&QNativeInterface::Private::QWaylandWindow::surfaceCreated,this,
        [this,guarded,epoch] { if (d_->nativeEpoch==epoch && guarded) setSurface(guarded->surface()); }));
    d_->surfaceConnections.push_back(connect(native,&QNativeInterface::Private::QWaylandWindow::surfaceDestroyed,this,
        [this,epoch] { if (d_->nativeEpoch==epoch) setSurface(nullptr); }));
    setSurface(native->surface());
#endif
}
bool WaylandHdrObserver::eventFilter(QObject* watched,QEvent* event) {
    if (watched==d_->window && event->type()==QEvent::PlatformSurface) {
        const auto kind=static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType();
        if (kind==QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
            ++d_->nativeEpoch;
            for (const auto& c:d_->surfaceConnections) QObject::disconnect(c);
            d_->surfaceConnections.clear(); setSurface(nullptr);
        } else attachNativeSurface();
    }
    return QObject::eventFilter(watched,event);
}
std::unique_ptr<WaylandHdrObserver> observeWaylandHdr(QQuickWindow* window,std::function<void()> changed)
{
#if QT_CONFIG(wayland)
    auto* native=qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    if (!native) return {};
    auto observer=std::make_unique<WaylandHdrObserver>(native->display(),std::move(changed));
    // Surface lifetime belongs to Qt. Feedback and any app-owned tag follow
    // that lifetime through the observer's event filter.
    observer->watchWindow(window);
    return observer;
#else
    Q_UNUSED(window); Q_UNUSED(changed); return {};
#endif
}
}
