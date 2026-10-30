#include "plugin/PluginHost.hpp"

#include "client/mods/WasmRuntime.hpp"
#include "core/Json.hpp"
#include "core/Log.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <sstream>

namespace perlin::server {
    namespace fs = std::filesystem;
    using mods::WasmInstance;

    struct PluginHost::Plugin {
        Info info;
        std::vector<uint8_t> wasm;
        std::unique_ptr<WasmInstance> instance;
        uint32_t subscriptions = 0;
        bool hasAlloc = false, hasFree = false;
        std::set<std::string> commands;
        // set while a call into the plugin is running so a trap can be pinned on it
        bool inCall = false;
    };

    namespace {
        constexpr uint32_t kWasiNoSys = 52, kWasiBadf = 8;

        uint64_t NowNs() {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::system_clock::now().time_since_epoch())
                                             .count());
        }

        template <class T> bool Poke(WasmInstance& w, uint64_t at, T value) {
            uint8_t* p = w.Memory(at, sizeof(T));
            if (!p)
                return false;
            std::memcpy(p, &value, sizeof(T));
            return true;
        }

        // just enough wasi for a freestanding reactor: no files, no sockets, output goes to the log
        WasmInstance::HostFn WasiFunction(const std::string& name, const std::string& pluginName) {
            using Ctx = WasmInstance::HostContext;
            const auto status = [](uint32_t code) {
                return [code](Ctx&, const uint64_t*, uint32_t, uint64_t* r, uint32_t nr) {
                    if (nr)
                        r[0] = code;
                    return true;
                };
            };
            if (name == "args_sizes_get" || name == "environ_sizes_get")
                return [](Ctx& c, const uint64_t* a, uint32_t, uint64_t* r, uint32_t nr) {
                    Poke<uint32_t>(c.instance, a[0], 0);
                    Poke<uint32_t>(c.instance, a[1], 0);
                    if (nr)
                        r[0] = 0;
                    return true;
                };
            if (name == "args_get" || name == "environ_get")
                return status(0);
            if (name == "clock_time_get")
                return [](Ctx& c, const uint64_t* a, uint32_t, uint64_t* r, uint32_t nr) {
                    Poke<uint64_t>(c.instance, a[2], NowNs());
                    if (nr)
                        r[0] = 0;
                    return true;
                };
            if (name == "random_get")
                return [](Ctx& c, const uint64_t* a, uint32_t, uint64_t* r, uint32_t nr) {
                    if (uint8_t* p = c.instance.Memory(a[0], a[1])) {
                        static thread_local std::mt19937_64 rng{std::random_device{}()};
                        for (uint64_t i = 0; i < a[1]; i++)
                            p[i] = static_cast<uint8_t>(rng());
                    }
                    if (nr)
                        r[0] = 0;
                    return true;
                };
            if (name == "fd_write")
                return [pluginName](Ctx& c, const uint64_t* a, uint32_t, uint64_t* r, uint32_t nr) {
                    // iovec: u32 pointer, u32 length (wasm32 layout)
                    uint32_t total = 0;
                    std::string text;
                    for (uint32_t i = 0; i < a[2]; i++) {
                        const uint8_t* iov = c.instance.Memory(a[1] + i * 8ull, 8);
                        if (!iov)
                            break;
                        uint32_t ptr, len;
                        std::memcpy(&ptr, iov, 4);
                        std::memcpy(&len, iov + 4, 4);
                        if (const uint8_t* s = c.instance.Memory(ptr, len))
                            text.append(reinterpret_cast<const char*>(s), len);
                        total += len;
                    }
                    if (!text.empty()) {
                        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
                            text.pop_back();
                        if (!text.empty())
                            LogInfo("[" + pluginName + "] " + text);
                    }
                    Poke<uint32_t>(c.instance, a[3], total);
                    if (nr)
                        r[0] = 0;
                    return true;
                };
            if (name == "proc_exit")
                return [](Ctx& c, const uint64_t*, uint32_t, uint64_t*, uint32_t) {
                    c.error = "plugin called exit";
                    return false;
                };
            if (name == "sched_yield" || name == "fd_close" || name == "fd_sync")
                return status(0);
            if (name.rfind("fd_", 0) == 0)
                return status(kWasiBadf);
            return status(kWasiNoSys);
        }

        std::string ReadManifestField(const json::Value& m, const char* key) { return m[key].String(); }
    }

    namespace {
        bool CallEvent(WasmInstance& w, bool hasAlloc, bool hasFree, uint32_t id, const std::vector<uint8_t>& payload, uint32_t& result,
                       std::string& error);
    }

    PluginHost::PluginHost(PluginApi& api) : api(api) {}
    PluginHost::~PluginHost() { UnloadAll(); }

    void PluginHost::LoadDirectory(const std::string& dir) {
        std::error_code ec;
        if (dir.empty() || !fs::is_directory(dir, ec))
            return;
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(dir, ec))
            if (entry.is_regular_file() && entry.path().extension() == ".wasm")
                files.push_back(entry.path());
        std::sort(files.begin(), files.end());
        for (const fs::path& file : files) {
            std::string error;
            if (!LoadFile(file.string(), error))
                LogWarn("plugin " + file.filename().string() + " not loaded: " + error);
        }
    }

    bool PluginHost::LoadFile(const std::string& path, std::string& error) {
        auto plugin = std::make_unique<Plugin>();
        plugin->info.file = path;
        plugin->info.name = fs::path(path).stem().string();
        plugin->info.engine = mods::WasmEngineName();

        std::ifstream in(path, std::ios::binary);
        if (!in) {
            error = "cannot read the file";
            return false;
        }
        plugin->wasm.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());

        // optional name.json beside the module
        {
            std::ifstream mf(fs::path(path).replace_extension(".json"), std::ios::binary);
            if (mf) {
                std::stringstream buf;
                buf << mf.rdbuf();
                json::Value manifest;
                if (json::Parse(buf.str(), manifest)) {
                    const std::string name = ReadManifestField(manifest, "name");
                    if (!name.empty())
                        plugin->info.name = name;
                    plugin->info.version = ReadManifestField(manifest, "version");
                    plugin->info.author = ReadManifestField(manifest, "author");
                    plugin->info.description = ReadManifestField(manifest, "description");
                }
            }
        }

        Plugin* raw = plugin.get();
        PluginApi* host = &api;
        const std::string pluginName = plugin->info.name;
        raw->instance = std::make_unique<WasmInstance>();
        const auto resolver = [raw, host, pluginName](const std::string& module, const std::string& name) -> WasmInstance::HostFn {
            if (module == "perlin" && name == "call")
                return [raw, host](WasmInstance::HostContext& c, const uint64_t* a, uint32_t n, uint64_t* r, uint32_t nr) {
                    if (n != 5 || nr != 1) {
                        c.error = "perlin.call has the wrong signature";
                        return false;
                    }
                    const uint32_t op = static_cast<uint32_t>(a[0]);
                    const uint8_t* in = c.instance.Memory(a[1], a[2]);
                    uint8_t* out = c.instance.Memory(a[3], a[4]);
                    if (!in || !out) {
                        r[0] = static_cast<uint32_t>(PS_ERR_BAD_ARGUMENTS);
                        return true;
                    }
                    PayloadReader rd(in, static_cast<size_t>(a[2]));
                    PayloadWriter wr;
                    int32_t rc = PS_OK;
                    switch (op) {
                    case PS_OP_LOG: {
                        const uint8_t level = rd.U8();
                        const std::string text = rd.Str();
                        if (!rd.ok) { rc = PS_ERR_BAD_ARGUMENTS; break; }
                        Log(static_cast<LogLevel>(std::min<int>(level, 3)), "[" + raw->info.name + "] " + text);
                        break;
                    }
                    case PS_OP_BROADCAST: {
                        const std::string text = rd.Str();
                        if (!rd.ok) { rc = PS_ERR_BAD_ARGUMENTS; break; }
                        host->Broadcast(text);
                        break;
                    }
                    case PS_OP_MESSAGE: {
                        const uint64_t id = rd.U64();
                        const std::string text = rd.Str();
                        if (!rd.ok) { rc = PS_ERR_BAD_ARGUMENTS; break; }
                        if (!host->Message(id, text)) rc = PS_ERR_NO_SUCH_PLAYER;
                        break;
                    }
                    case PS_OP_KICK: {
                        const uint64_t id = rd.U64();
                        const std::string text = rd.Str();
                        if (!rd.ok) { rc = PS_ERR_BAD_ARGUMENTS; break; }
                        if (!host->Kick(id, text)) rc = PS_ERR_NO_SUCH_PLAYER;
                        break;
                    }
                    case PS_OP_REGISTER_COMMAND: {
                        const std::string name = rd.Str();
                        const std::string description = rd.Str();
                        if (!rd.ok || name.empty()) { rc = PS_ERR_BAD_ARGUMENTS; break; }
                        raw->commands.insert(name);
                        host->RegisterCommand(raw->info.name, name, description);
                        break;
                    }
                    case PS_OP_PLAYERS: {
                        const auto players = host->Players();
                        wr.U32(static_cast<uint32_t>(players.size()));
                        for (const auto& p : players) {
                            wr.U64(p.id);
                            wr.Str(p.name);
                            wr.F64(p.x);
                            wr.F64(p.y);
                            wr.F64(p.z);
                        }
                        break;
                    }
                    case PS_OP_TELEPORT: {
                        const uint64_t id = rd.U64();
                        const double x = rd.F64(), y = rd.F64(), z = rd.F64();
                        if (!rd.ok) { rc = PS_ERR_BAD_ARGUMENTS; break; }
                        if (!host->Teleport(id, x, y, z)) rc = PS_ERR_NO_SUCH_PLAYER;
                        break;
                    }
                    case PS_OP_SET_BLOCK: {
                        const int32_t x = rd.I32(), y = rd.I32(), z = rd.I32();
                        const std::string block = rd.Str();
                        if (!rd.ok) { rc = PS_ERR_BAD_ARGUMENTS; break; }
                        rc = host->SetBlock(x, y, z, block);
                        break;
                    }
                    case PS_OP_GET_BLOCK: {
                        const int32_t x = rd.I32(), y = rd.I32(), z = rd.I32();
                        if (!rd.ok) { rc = PS_ERR_BAD_ARGUMENTS; break; }
                        std::string block;
                        rc = host->GetBlock(x, y, z, block);
                        if (rc == PS_OK) wr.Str(block);
                        break;
                    }
                    case PS_OP_SERVER_INFO: {
                        const auto s = host->GetStatus();
                        wr.Str(s.name);
                        wr.U32(s.players);
                        wr.U32(s.maxPlayers);
                        wr.U64(s.tick);
                        wr.Str(s.backend);
                        break;
                    }
                    case PS_OP_GIVE_ITEM: {
                        const uint64_t id = rd.U64();
                        const std::string item = rd.Str();
                        const uint32_t count = rd.U32();
                        if (!rd.ok || count == 0 || count > 4096) { rc = PS_ERR_BAD_ARGUMENTS; break; }
                        const int left = host->GiveItem(id, item, static_cast<int>(count));
                        if (left < 0) { rc = left; break; }
                        wr.U32(static_cast<uint32_t>(left));
                        break;
                    }
                    case PS_OP_TIME_MS:
                        wr.I64(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
                        break;
                    default: rc = PS_ERR_UNKNOWN_OP; break;
                    }
                    if (rc >= 0 && !wr.bytes.empty()) {
                        if (wr.bytes.size() > a[4])
                            rc = PS_ERR_BUFFER_TOO_SMALL;
                        else {
                            std::memcpy(out, wr.bytes.data(), wr.bytes.size());
                            rc = static_cast<int32_t>(wr.bytes.size());
                        }
                    }
                    r[0] = static_cast<uint32_t>(rc);
                    return true;
                };
            if (module == "wasi_snapshot_preview1")
                return WasiFunction(name, pluginName);
            if (module == "env" && name == "emscripten_notify_memory_growth")
                return [](WasmInstance::HostContext&, const uint64_t*, uint32_t, uint64_t* r, uint32_t nr) {
                    if (nr) r[0] = 0;
                    return true;
                };
            return {};
        };

        if (!raw->instance->Load(raw->wasm.data(), raw->wasm.size(), resolver, error))
            return false;
        if (!raw->instance->HasExport("perlin_event")) {
            error = "not a Perlin server plugin (no perlin_event export)";
            return false;
        }
        raw->hasAlloc = raw->instance->HasExport("perlin_alloc");
        raw->hasFree = raw->instance->HasExport("perlin_free");
        std::string ignored;
        if (raw->instance->HasExport("_initialize"))
            raw->instance->Call("_initialize", {}, nullptr, ignored);
        uint64_t subs = 0;
        if (raw->instance->HasExport("perlin_subscriptions") && raw->instance->Call("perlin_subscriptions", {}, &subs, ignored))
            raw->subscriptions = static_cast<uint32_t>(subs);
        raw->info.enabled = true;
        LogInfo("plugin " + raw->info.name + (raw->info.version.empty() ? "" : " " + raw->info.version) + " loaded (" +
                (raw->instance->Is64() ? "wasm64" : "wasm32") + " on " + raw->info.engine + ")");
        plugins.push_back(std::move(plugin));
        uint32_t ignoredResult = 0;
        CallPlugin(*raw, PS_EVENT_ENABLE, {}, ignoredResult);
        return true;
    }

    void PluginHost::UnloadAll() {
        for (auto& p : plugins) {
            uint32_t ignoredResult = 0;
            if (p->info.enabled)
                CallPlugin(*p, PS_EVENT_DISABLE, {}, ignoredResult);
        }
        plugins.clear();
    }

    bool PluginHost::CallPlugin(Plugin& p, uint32_t id, const std::vector<uint8_t>& payload, uint32_t& result) {
        std::string error;
        if (CallEvent(*p.instance, p.hasAlloc, p.hasFree, id, payload, result, error))
            return true;
        p.info.enabled = false;
        p.info.error = error;
        LogError("plugin " + p.info.name + " disabled after an error: " + error);
        return false;
    }

    bool PluginHost::AnyoneListens(PsEvent event) const {
        for (const auto& p : plugins)
            if (p->info.enabled && (p->subscriptions & (1u << event)))
                return true;
        return false;
    }

    namespace {
        // calls perlin_event on one plugin; false when it trapped
        bool CallEvent(WasmInstance& w, bool hasAlloc, bool hasFree, uint32_t id, const std::vector<uint8_t>& payload, uint32_t& result,
                       std::string& error) {
            uint64_t ptr = 0;
            const uint64_t len = payload.size();
            if (len) {
                if (!hasAlloc) {
                    error = "plugin has no perlin_alloc export";
                    return false;
                }
                if (!w.Call("perlin_alloc", {len}, &ptr, error))
                    return false;
                uint8_t* dst = ptr ? w.Memory(ptr, len) : nullptr;
                if (!dst) {
                    error = "perlin_alloc returned a bad pointer";
                    return false;
                }
                std::memcpy(dst, payload.data(), payload.size());
            }
            uint64_t rc = 0;
            if (!w.Call("perlin_event", {id, ptr, len}, &rc, error))
                return false;
            result = static_cast<uint32_t>(rc);
            std::string ignored;
            if (len && hasFree)
                w.Call("perlin_free", {ptr}, nullptr, ignored);
            return true;
        }
    }

    uint32_t PluginHost::Dispatch(PsEvent event, const std::vector<uint8_t>& payload) {
        uint32_t combined = 0;
        for (auto& p : plugins) {
            if (!p->info.enabled || !(p->subscriptions & (1u << event)) || event == PS_EVENT_COUNT)
                continue;
            uint32_t result = 0;
            if (!CallPlugin(*p, static_cast<uint32_t>(event), payload, result))
                continue;
            combined |= result;
            if (result & 1u)
                break;
        }
        return combined;
    }

    bool PluginHost::DispatchCommand(const std::string& plugin, uint64_t player, const std::string& command, const std::string& args) {
        for (auto& p : plugins) {
            if (p->info.name != plugin || !p->info.enabled)
                continue;
            PayloadWriter w;
            w.U64(player);
            w.Str(command);
            w.Str(args);
            uint32_t result = 0;
            return CallPlugin(*p, PS_EVENT_COMMAND, w.bytes, result);
        }
        return false;
    }

    std::vector<PluginHost::Info> PluginHost::List() const {
        std::vector<Info> out;
        for (const auto& p : plugins)
            out.push_back(p->info);
        return out;
    }
}
