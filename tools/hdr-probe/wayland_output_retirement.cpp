// Exercise queued object arguments across native output/receiver retirement.
// This uses public Wayland APIs and no compositor, QPA plugin or HDR renderer.
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server.h>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

class Peer {
public:
    explicit Peer(bool duplicate) : duplicate_(duplicate)
    {
        int sockets[2];
        require(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0, "socketpair failed");
        server_ = wl_display_create();
        if (!server_) { close(sockets[0]); close(sockets[1]); throw std::runtime_error("server failed"); }
        if (!wl_client_create(server_, sockets[0])) {
            close(sockets[0]); close(sockets[1]); wl_display_destroy(server_);
            throw std::runtime_error("server client failed");
        }
        // A private fixture may advertise any interface as a global. Keeping
        // the real wl_surface.enter signature exercises its object references.
        auto* surfaceGlobal = wl_global_create(server_, &wl_surface_interface, 1, this, bindSurface);
        auto* outputGlobal = wl_global_create(server_, &wl_output_interface, 4, this, bindOutput);
        client = wl_display_connect_to_fd(sockets[1]);
        if (!surfaceGlobal || !outputGlobal || !client) {
            if (client) wl_display_disconnect(client); else close(sockets[1]);
            wl_display_destroy_clients(server_); wl_display_destroy(server_);
            throw std::runtime_error("fixture globals or client failed");
        }
        thread_ = std::thread([this] {
            while (!stop_.load()) {
                if (wl_event_loop_dispatch(wl_display_get_event_loop(server_), 10) < 0) error_ = true;
                wl_display_flush_clients(server_);
            }
        });
    }
    ~Peer()
    {
        wl_display_disconnect(client);
        stop_ = true;
        thread_.join();
        wl_display_destroy_clients(server_);
        wl_display_destroy(server_);
    }
    bool healthy() const { return !error_.load(); }
    wl_display* client = nullptr;
private:
    static void destroyResource(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
    static void bindSurface(wl_client* client, void* data, uint32_t, uint32_t id)
    {
        auto* self = static_cast<Peer*>(data);
        static const struct wl_surface_interface methods = [] {
            struct wl_surface_interface value{}; value.destroy = destroyResource; return value;
        }();
        self->surface_ = wl_resource_create(client, &wl_surface_interface, 1, id);
        if (!self->surface_) { self->error_ = true; wl_client_post_no_memory(client); return; }
        wl_resource_set_implementation(self->surface_, &methods, self, nullptr);
        self->send();
    }
    static void bindOutput(wl_client* client, void* data, uint32_t, uint32_t id)
    {
        auto* self = static_cast<Peer*>(data);
        static const struct wl_output_interface methods{destroyResource};
        self->output_ = wl_resource_create(client, &wl_output_interface, 4, id);
        if (!self->output_) { self->error_ = true; wl_client_post_no_memory(client); return; }
        wl_resource_set_implementation(self->output_, &methods, self, nullptr);
        self->send();
    }
    void send()
    {
        if (!surface_ || !output_ || sent_) return;
        wl_surface_send_enter(surface_, output_);
        if (duplicate_) wl_surface_send_enter(surface_, output_);
        sent_ = true;
    }
    wl_display* server_ = nullptr;
    wl_resource* surface_ = nullptr;
    wl_resource* output_ = nullptr;
    std::thread thread_;
    std::atomic_bool stop_{false}, error_{false};
    bool duplicate_ = false, sent_ = false;
};

struct Globals {
    uint32_t surface = 0, output = 0;
    static void added(void* data, wl_registry*, uint32_t name, const char* interface, uint32_t)
    {
        auto* self = static_cast<Globals*>(data);
        if (!strcmp(interface, "wl_surface")) self->surface = name;
        if (!strcmp(interface, "wl_output")) self->output = name;
    }
    static void removed(void*, wl_registry*, uint32_t) {}
};

struct Observations {
    int callbacks = 0, nullOutputs = 0, liveOutputs = 0;
    wl_output* expected = nullptr;
    bool foreign = false;
    static void enter(void* data, wl_surface*, wl_output* output)
    {
        auto* self = static_cast<Observations*>(data);
        ++self->callbacks;
        if (!output) ++self->nullOutputs;
        else { ++self->liveOutputs; self->foreign |= output != self->expected; }
    }
    static void leave(void*, wl_surface*, wl_output*) {}
};

Observations run(const QString& scenario)
{
    Peer peer(scenario == "duplicate");
    auto* registry = wl_display_get_registry(peer.client);
    Globals globals;
    const wl_registry_listener registryListener{Globals::added, Globals::removed};
    wl_registry_add_listener(registry, &registryListener, &globals);
    require(wl_display_roundtrip(peer.client) >= 0 && globals.surface && globals.output, "registry failed");
    auto* queue = wl_display_create_queue(peer.client);
    require(queue, "queue failed");
    auto* surface = static_cast<wl_surface*>(wl_registry_bind(registry, globals.surface, &wl_surface_interface, 1));
    auto* output = static_cast<wl_output*>(wl_registry_bind(registry, globals.output, &wl_output_interface, 4));
    require(surface && output, "binding failed");
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(surface), queue);
    Observations observed;
    observed.expected = output;
    const wl_surface_listener listener{Observations::enter, Observations::leave, nullptr, nullptr};
    wl_surface_add_listener(surface, &listener, &observed);
    // The default-queue sync is sent after the binds. The server queues enter
    // before sync.done, so this reads enter without dispatching its private queue.
    require(wl_display_roundtrip(peer.client) >= 0 && observed.callbacks == 0, "queue barrier failed");
    const bool live = scenario == "live";
    if (!live) { wl_output_release(output); output = nullptr; }
    const bool destroyReceiver = scenario == "receiver" || scenario == "discard";
    if (destroyReceiver) { wl_surface_destroy(surface); surface = nullptr; }
    if (scenario != "discard")
        require(wl_display_dispatch_queue_pending(peer.client, queue) >= 0, "dispatch failed");
    const int count = scenario == "duplicate" ? 2 : 1;
    require(observed.callbacks == (destroyReceiver ? 0 : count) && !observed.foreign
            && observed.liveOutputs == (live ? count : 0)
            && observed.nullOutputs == (destroyReceiver || live ? 0 : count), "wrong object delivery");
    if (output) wl_output_release(output);
    if (surface) wl_surface_destroy(surface);
    wl_event_queue_destroy(queue);
    wl_registry_destroy(registry);
    require(wl_display_roundtrip(peer.client) >= 0 && peer.healthy(), "fixture teardown failed");
    // Do not return a stale address into the conservative leak scanner's roots.
    observed.expected = nullptr;
    return observed;
}
}

int main(int argc, char** argv)
{
#if !defined(__SANITIZE_ADDRESS__)
#  if !defined(__has_feature)
    std::fprintf(stderr, "Leak-enabled AddressSanitizer instrumentation is required\n");
    return 77;
#  elif !__has_feature(address_sanitizer)
    std::fprintf(stderr, "Leak-enabled AddressSanitizer instrumentation is required\n");
    return 77;
#  endif
#endif
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    const QString scenario = arguments.value(1);
    bool valid = false;
    const int repeats = arguments.value(2, "32").toInt(&valid);
    if (!QStringList{"live", "retired", "receiver", "discard", "duplicate"}.contains(scenario)
        || !valid || repeats < 1 || repeats > 256) return 2;
    try {
        int callbacks = 0, nullOutputs = 0;
        for (int i = 0; i < repeats; ++i) {
            const auto result = run(scenario);
            callbacks += result.callbacks; nullOutputs += result.nullOutputs;
        }
        Dl_info library{};
        require(dladdr(reinterpret_cast<void*>(&wl_display_connect_to_fd), &library) && library.dli_fname,
                "runtime identity unavailable");
        QFile file(QString::fromLocal8Bit(library.dli_fname));
        require(file.open(QIODevice::ReadOnly), "runtime fingerprint unavailable");
        const QJsonObject report{{"status", "passed"}, {"case", scenario}, {"repeats", repeats},
            {"callbacks", callbacks}, {"null_outputs", nullOutputs},
            {"client_library", file.fileName()},
            {"client_library_sha256", QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex())}};
        const auto json = QJsonDocument(report).toJson(QJsonDocument::Compact);
        std::puts(json.constData()); std::fflush(stdout);
        return 0; // LeakSanitizer's unsuppressed process-exit check is authoritative.
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
