#pragma once

#include "../../src/imaging/hdr/wayland_hdr.h"
#include "color-management-client.h"
#include "color-management-server.h"
#include <QSocketNotifier>
#include <wayland-server.h>
#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>

namespace Licasa::WaylandTest {
struct Config {
    bool omitTargetPrimaries=false;
    bool allowSurfaceTag=false;
    bool advertised=true, complete=true, linear=true, accept=true, malformed=false, hang=false, delayInfo=false;
    bool params=true,extended=true,primary=true,intent=true,hangAcceptance=false,zeroIdentity=false;
    uint32_t maximum=1000;
};
class Server {
public:
    Config config;
    wl_display* display=nullptr;
    wl_global* color=nullptr;
    std::vector<wl_resource*> feedbacks;
    struct Pending;
    std::vector<Pending*> pendingInfos;
    std::atomic<bool> stop{false},removeGlobal{false},releaseLate{false},reuseNextIdentity{false},addGlobal{false},disconnectPeer{false};
    std::atomic<int> nextMaximum{-1}, colorSurfaceRequests{0}, parametricRequests{0},infoRequests{0};
    std::atomic<int> surfaceTags{0}, retiredColorSurfaces{0}, heldInfos{0};
    std::atomic<bool> holdNextInfo{false};
    std::atomic<uint32_t> requestedTf{0},requestedPrimary{0},requestedMaximum{0},nextIdentity{0};
    uint32_t identity=10;
    std::thread thread;
    int clientFd=-1;
    explicit Server(Config c):config(c) {
        display=wl_display_create();
        wl_global_create(display,&wl_compositor_interface,4,this,bindCompositor);
        if (config.advertised) color=wl_global_create(display,&wp_color_manager_v1_interface,1,this,bindColor);
        int sockets[2]; if (socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,sockets)) qFatal("socketpair failed");
        wl_client_create(display,sockets[0]); clientFd=sockets[1];
        thread=std::thread([this] {
            while (!stop.load()) {
                wl_event_loop_dispatch(wl_display_get_event_loop(display),5);
                const int maximum=nextMaximum.exchange(-1);
                if (maximum>=0) {
                    config.maximum=uint32_t(maximum);
                    const auto forcedIdentity=nextIdentity.exchange(0);
                    const bool reuse=reuseNextIdentity.exchange(false);
                    if (forcedIdentity) identity=forcedIdentity;
                    else if (!reuse) ++identity;
                    for (auto* feedback:feedbacks) wp_color_management_surface_feedback_v1_send_preferred_changed(feedback,identity);
                }
                if (removeGlobal.exchange(false) && color) { wl_global_destroy(color); color=nullptr; }
                if (addGlobal.exchange(false) && !color) color=wl_global_create(display,&wp_color_manager_v1_interface,1,this,bindColor);
                if (releaseLate.exchange(false)) flushPending();
                if (disconnectPeer.exchange(false)) wl_display_destroy_clients(display);
                wl_display_flush_clients(display);
            }
        });
    }
    ~Server() {
        stop=true; thread.join();
        wl_display_destroy_clients(display); wl_display_destroy(display);
        if (clientFd>=0) close(clientFd);
    }
    static void destroy(wl_client*,wl_resource* resource) { wl_resource_destroy(resource); }
    static void bindCompositor(wl_client* client,void* data,uint32_t version,uint32_t id) {
        auto* resource=wl_resource_create(client,&wl_compositor_interface,std::min(version,4u),id);
        static const struct wl_compositor_interface implementation{
            [](wl_client* c,wl_resource* r,uint32_t id) {
                auto* surface=wl_resource_create(c,&wl_surface_interface,wl_resource_get_version(r),id);
                static const auto implementation=[] { struct wl_surface_interface i{}; i.destroy=destroy; return i; }();
                wl_resource_set_implementation(surface,&implementation,wl_resource_get_user_data(r),nullptr);
            },nullptr};
        wl_resource_set_implementation(resource,&implementation,data,nullptr);
    }
    struct Description { Server* server; bool acceptance; uint32_t identity,maximum; };
    struct Pending { wl_resource* resource; Description description; };
    static void sendInformation(wl_resource* info,const Description& state) {
        wp_image_description_info_v1_send_primaries(info,640000,330000,300000,600000,150000,60000,312700,329000);
        wp_image_description_info_v1_send_primaries_named(info,WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
        wp_image_description_info_v1_send_tf_named(info,WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR);
        if (!state.server->config.malformed) wp_image_description_info_v1_send_luminances(info,0,80,80);
        if (!state.server->config.omitTargetPrimaries) wp_image_description_info_v1_send_target_primaries(info,640000,330000,300000,600000,150000,60000,312700,329000);
        wp_image_description_info_v1_send_target_luminance(info,0,state.maximum);
        wp_image_description_info_v1_send_done(info); wl_resource_destroy(info);
    }
    void flushPending() {
        const auto pending=pendingInfos;
        for (auto* i:pending) sendInformation(i->resource,i->description);
    }
    static void description(wl_client* client,wl_resource* parent,uint32_t id,bool acceptance) {
        auto& s=*static_cast<Server*>(wl_resource_get_user_data(parent));
        auto* resource=wl_resource_create(client,&wp_image_description_v1_interface,1,id);
        auto* state=new Description{&s,acceptance,s.identity,s.config.maximum};
        static const struct wp_image_description_v1_interface implementation{destroy,
            [](wl_client* client,wl_resource* r,uint32_t id) {
                auto& state=*static_cast<Description*>(wl_resource_get_user_data(r));
                if (state.acceptance) { wl_resource_post_error(r,0,"Information requested on client-created description"); return; }
                auto* info=wl_resource_create(client,&wp_image_description_info_v1_interface,1,id);
                auto& s=*state.server;
                const auto sequence=s.infoRequests.fetch_add(1);
                if ((s.config.delayInfo && sequence==0) || s.holdNextInfo.exchange(false)) {
                    auto* pending=new Pending{info,state}; s.pendingInfos.push_back(pending);
                    ++s.heldInfos;
                    wl_resource_set_implementation(info,nullptr,pending,[](wl_resource* r) {
                        auto* pending=static_cast<Pending*>(wl_resource_get_user_data(r));
                        auto& list=pending->description.server->pendingInfos;
                        list.erase(std::remove(list.begin(),list.end(),pending),list.end());
                        --pending->description.server->heldInfos;
                        delete pending;
                    });
                } else sendInformation(info,state);
            }};
        wl_resource_set_implementation(resource,&implementation,state,[](wl_resource* r) {
            delete static_cast<Description*>(wl_resource_get_user_data(r));
        });
        if (acceptance && !s.config.accept) wp_image_description_v1_send_failed(resource,0,"Synthetic rejection");
        else if ((acceptance && !s.config.hangAcceptance) || (!acceptance && !s.config.hang))
            wp_image_description_v1_send_ready(resource,acceptance && s.config.zeroIdentity ? 0 : state->identity);
    }
    static void bindColor(wl_client* client,void* data,uint32_t,uint32_t id) {
        auto& s=*static_cast<Server*>(data);
        auto* resource=wl_resource_create(client,&wp_color_manager_v1_interface,1,id);
        static const auto implementation=[] {
            struct wp_color_manager_v1_interface i{};
            i.destroy=destroy;
            i.get_surface=[](wl_client* client,wl_resource* r,uint32_t id,wl_resource*) {
                auto& s=*static_cast<Server*>(wl_resource_get_user_data(r));
                ++s.colorSurfaceRequests;
                if (!s.config.allowSurfaceTag || s.colorSurfaceRequests.load()-s.retiredColorSurfaces.load()!=1) {
                    wl_resource_post_error(r,0,"Unexpected color surface owner");
                    return;
                }
                auto* surface=wl_resource_create(client,&wp_color_management_surface_v1_interface,1,id);
                static const struct wp_color_management_surface_v1_interface impl{destroy,
                    [](wl_client*,wl_resource* r,wl_resource* description,uint32_t intent) {
                        const auto* d=static_cast<Description*>(wl_resource_get_user_data(description));
                        if (!d->acceptance || intent!=WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL) {
                            wl_resource_post_error(r,0,"Wrong description or intent");
                            return;
                        }
                        ++static_cast<Server*>(wl_resource_get_user_data(r))->surfaceTags;
                    },nullptr};
                wl_resource_set_implementation(surface,&impl,&s,[](wl_resource* r) {
                    ++static_cast<Server*>(wl_resource_get_user_data(r))->retiredColorSurfaces;
                });
            };
            i.get_surface_feedback=[](wl_client* client,wl_resource* r,uint32_t id,wl_resource*) {
                auto& s=*static_cast<Server*>(wl_resource_get_user_data(r));
                auto* f=wl_resource_create(client,&wp_color_management_surface_feedback_v1_interface,1,id);
                static const struct wp_color_management_surface_feedback_v1_interface impl{destroy,
                    [](wl_client* c,wl_resource* r,uint32_t id) { description(c,r,id,false); },
                    [](wl_client* c,wl_resource* r,uint32_t id) { description(c,r,id,false); }};
                wl_resource_set_implementation(f,&impl,&s,[](wl_resource* r) {
                    auto& s=*static_cast<Server*>(wl_resource_get_user_data(r));
                    s.feedbacks.erase(std::remove(s.feedbacks.begin(),s.feedbacks.end(),r),s.feedbacks.end());
                });
                s.feedbacks.push_back(f);
            };
            i.create_parametric_creator=[](wl_client* client,wl_resource* r,uint32_t id) {
                auto& s=*static_cast<Server*>(wl_resource_get_user_data(r)); ++s.parametricRequests;
                auto* creator=wl_resource_create(client,&wp_image_description_creator_params_v1_interface,1,id);
                static const auto impl=[] {
                    struct wp_image_description_creator_params_v1_interface i{};
                    i.create=[](wl_client* c,wl_resource* r,uint32_t id) { description(c,r,id,true); wl_resource_destroy(r); };
                    i.set_tf_named=[](wl_client*,wl_resource* r,uint32_t v) { static_cast<Server*>(wl_resource_get_user_data(r))->requestedTf=v; };
                    i.set_primaries_named=[](wl_client*,wl_resource* r,uint32_t v) { static_cast<Server*>(wl_resource_get_user_data(r))->requestedPrimary=v; };
                    i.set_mastering_luminance=[](wl_client*,wl_resource* r,uint32_t,uint32_t v) { static_cast<Server*>(wl_resource_get_user_data(r))->requestedMaximum=v; };
                    return i;
                }();
                wl_resource_set_implementation(creator,&impl,&s,nullptr);
            };
            return i;
        }();
        wl_resource_set_implementation(resource,&implementation,&s,nullptr);
        if (s.config.intent) wp_color_manager_v1_send_supported_intent(resource,WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
        if (s.config.params) wp_color_manager_v1_send_supported_feature(resource,WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC);
        wp_color_manager_v1_send_supported_feature(resource,WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES);
        if (s.config.extended) wp_color_manager_v1_send_supported_feature(resource,WP_COLOR_MANAGER_V1_FEATURE_EXTENDED_TARGET_VOLUME);
        if (s.config.primary) wp_color_manager_v1_send_supported_primaries_named(resource,WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
        if (s.config.linear) wp_color_manager_v1_send_supported_tf_named(resource,WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR);
        if (s.config.complete) wp_color_manager_v1_send_done(resource);
    }
};
struct Client {
    Server server;
    wl_display* display=nullptr;
    wl_registry* registry=nullptr;
    wl_compositor* compositor=nullptr;
    wl_surface* surface=nullptr;
    std::unique_ptr<QSocketNotifier> reader;
    std::unique_ptr<Licasa::WaylandHdrObserver> observer;
    int notifications=0;
    explicit Client(Config config={}, bool observe=true):server(config) {
        display=wl_display_connect_to_fd(server.clientFd); server.clientFd=-1;
        registry=wl_display_get_registry(display);
        static const wl_registry_listener listener{
            [](void* p,wl_registry* r,uint32_t name,const char* type,uint32_t version) {
                auto& c=*static_cast<Client*>(p);
                if (QByteArray(type)=="wl_compositor") c.compositor=static_cast<wl_compositor*>(wl_registry_bind(r,name,&wl_compositor_interface,std::min(version,4u)));
            },[](void*,wl_registry*,uint32_t) {}};
        wl_registry_add_listener(registry,&listener,this);
        if (wl_display_roundtrip(display)<0 || !compositor) qFatal("Synthetic compositor bootstrap failed");
        surface=wl_compositor_create_surface(compositor);
        reader=std::make_unique<QSocketNotifier>(wl_display_get_fd(display),QSocketNotifier::Read);
        QObject::connect(reader.get(),&QSocketNotifier::activated,reader.get(),[this] {
            // Only this test transport reads its private connection. Production
            // observation never reads Qt's borrowed connection.
            if (wl_display_prepare_read(display)==0) {
                if (wl_display_read_events(display)<0) reader->setEnabled(false);
            }
            if (wl_display_dispatch_pending(display)<0) reader->setEnabled(false);
        });
        if (observe) {
            observer=std::make_unique<Licasa::WaylandHdrObserver>(display,[this] { ++notifications; });
            observer->setSurface(surface);
        }
        wl_display_flush(display);
    }
    ~Client() {
        observer.reset(); reader.reset();
        if (surface) wl_surface_destroy(surface);
        wl_compositor_destroy(compositor); wl_registry_destroy(registry);
        wl_display_flush(display); wl_display_disconnect(display);
    }
};
}
