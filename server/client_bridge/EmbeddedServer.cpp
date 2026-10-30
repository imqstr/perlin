#include "EmbeddedServer.hpp"

#include "perlin/server.h"

#include <atomic>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace perlin {
    namespace {
        struct Api {
            void* lib = nullptr;
            uint32_t (*abi)() = nullptr;
            ps_server* (*create)(const ps_options*) = nullptr;
            const char* (*lastError)() = nullptr;
            int (*start)(ps_server*) = nullptr;
            int (*runAsync)(ps_server*) = nullptr;
            void (*tick)(ps_server*) = nullptr;
            void (*stop)(ps_server*) = nullptr;
            void (*destroy)(ps_server*) = nullptr;
            void (*command)(ps_server*, const char*) = nullptr;
            ps_conn* (*connect)(ps_server*) = nullptr;
            int (*send)(ps_conn*, const uint8_t*, size_t) = nullptr;
            int (*receive)(ps_conn*, uint8_t*, size_t, size_t*) = nullptr;
            int (*alive)(ps_conn*) = nullptr;
            void (*close)(ps_conn*) = nullptr;
        };

        void* Open(const std::string& path) {
#ifdef _WIN32
            return LoadLibraryA(path.c_str());
#else
            return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        }
        void* Sym(void* lib, const char* name) {
#ifdef _WIN32
            return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
            return dlsym(lib, name);
#endif
        }
        void CloseLib(void* lib) {
#ifdef _WIN32
            FreeLibrary(static_cast<HMODULE>(lib));
#else
            dlclose(lib);
#endif
        }

        class LoopbackTransport final : public Transport {
        public:
            LoopbackTransport(std::shared_ptr<Api> api, ps_conn* conn) : api(std::move(api)), conn(conn) {
                info.name = "Singleplayer";
                info.protocol = kBedrockProtocol;
                info.version = kBedrockVersion;
            }
            ~LoopbackTransport() override { Disconnect(); }

            void Disconnect() override {
                if (conn) {
                    api->close(conn);
                    conn = nullptr;
                }
            }
            bool SendReliable(const std::vector<uint8_t>& data) override { return conn && api->send(conn, data.data(), data.size()) == 1; }
            bool SendUnreliable(const std::vector<uint8_t>& data) override { return SendReliable(data); }
            bool TryReceive(std::vector<uint8_t>& out) override {
                if (!conn)
                    return false;
                size_t size = 0;
                int r = api->receive(conn, nullptr, 0, &size);
                if (r == 0)
                    return false;
                out.resize(size);
                r = api->receive(conn, out.data(), out.size(), &size);
                out.resize(size);
                return r == 1;
            }
            bool IsConnected() const override { return conn && api->alive(conn); }
            std::string LastError() const override { return {}; }
            const NetherNetServerInfo& Info() const override { return info; }

        private:
            std::shared_ptr<Api> api;
            ps_conn* conn;
            NetherNetServerInfo info;
        };
    }

    struct EmbeddedServer::Impl {
        std::shared_ptr<Api> api;
        ps_server* server = nullptr;
        std::atomic<bool> running{false};
    };

    EmbeddedServer::EmbeddedServer() : impl(std::make_unique<Impl>()) {}
    EmbeddedServer::~EmbeddedServer() { Stop(); }

    bool EmbeddedServer::Start(const std::string& libraryPath, const std::string& dataDir, const std::string& configJson, std::string& error) {
        Stop();
        auto api = std::make_shared<Api>();
        api->lib = Open(libraryPath);
        if (!api->lib) {
            error = "cannot load " + libraryPath;
            return false;
        }
#define BIND(field, name)                                                          \
    api->field = reinterpret_cast<decltype(api->field)>(Sym(api->lib, name));      \
    if (!api->field) {                                                             \
        error = std::string("perlin_server library lacks ") + name;                \
        CloseLib(api->lib);                                                        \
        return false;                                                              \
    }
        BIND(abi, "ps_abi_version")
        BIND(create, "ps_create")
        BIND(lastError, "ps_last_error")
        BIND(start, "ps_start")
        BIND(runAsync, "ps_run_async")
        BIND(tick, "ps_tick")
        BIND(stop, "ps_stop")
        BIND(destroy, "ps_destroy")
        BIND(command, "ps_command")
        BIND(connect, "ps_connect_loopback")
        BIND(send, "ps_conn_send")
        BIND(receive, "ps_conn_receive")
        BIND(alive, "ps_conn_alive")
        BIND(close, "ps_conn_close")
#undef BIND
        if (api->abi() != 1) {
            error = "perlin_server library has a different abi version";
            CloseLib(api->lib);
            return false;
        }
        ps_options options{};
        options.data_dir = dataDir.c_str();
        options.config_json = configJson.empty() ? nullptr : configJson.c_str();
        options.loopback_only = 1;
        impl->server = api->create(&options);
        if (!impl->server || api->start(impl->server) != 0) {
            error = api->lastError();
            if (impl->server)
                api->destroy(impl->server);
            impl->server = nullptr;
            CloseLib(api->lib);
            return false;
        }
        api->runAsync(impl->server);
        impl->api = std::move(api);
        impl->running = true;
        return true;
    }

    void EmbeddedServer::Stop() {
        if (!impl->running.exchange(false))
            return;
        impl->api->destroy(impl->server);
        impl->server = nullptr;
        // the library stays mapped for any transport still alive; the shared Api keeps it until the last user is gone
    }

    bool EmbeddedServer::Running() const { return impl->running; }
    void EmbeddedServer::Tick() {
        if (impl->running)
            impl->api->tick(impl->server);
    }
    void EmbeddedServer::Command(const std::string& line) {
        if (impl->running)
            impl->api->command(impl->server, line.c_str());
    }

    std::unique_ptr<Transport> EmbeddedServer::Connect() {
        if (!impl->running)
            return nullptr;
        ps_conn* conn = impl->api->connect(impl->server);
        return conn ? std::make_unique<LoopbackTransport>(impl->api, conn) : nullptr;
    }
}
