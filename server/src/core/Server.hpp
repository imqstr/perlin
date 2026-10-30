#pragma once
#include "core/Config.hpp"
#include "net/Connection.hpp"
#include "net/LoopbackListener.hpp"
#include "plugin/PluginApi.hpp"
#include "plugin/PluginHost.hpp"
#include "session/Session.hpp"
#include "world/ChunkManager.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace perlin::server {

    struct CommandDef {
        std::string name;
        std::string description;
        // plugin that owns it, empty for built-ins
        std::string plugin;
        bool operatorOnly = false;
        // built-ins run here; plugin commands are forwarded to the plugin
        std::function<void(Session* source, const std::string& args)> run;
    };

    class Server final : public PluginApi {
    public:
        explicit Server(ServerConfig config);
        ~Server() override;

        bool Start(std::string& error);
        // blocking loop at the configured tick rate until RequestStop
        void Run();
        // one tick; hosts that drive the loop themselves (embedded) call this
        void Tick();
        void Stop();
        void RequestStop() { stopRequested = true; }
        bool StopRequested() const { return stopRequested; }

        // console / embedding entry point for a command line without the leading slash
        void RunConsoleCommand(const std::string& line);

        LoopbackListener& Loopback() { return *loopback; }

        const ServerConfig& Config() const { return config; }
        ChunkManager& Chunks() { return *chunks; }
        PluginHost& Plugins() { return *plugins; }
        uint64_t TickCount() const { return tick; }
        const std::map<std::string, CommandDef>& Commands() const { return commands; }

        // session callbacks
        void OnPlayerJoined(Session& session);
        void OnPlayerLeft(Session& session);
        void HandleChat(Session& from, const std::string& message);
        void RunPlayerCommand(Session& from, const std::string& line);
        std::vector<Session*> InGamePlayers();
        Session* FindSession(uint64_t id);
        Session* FindByName(const std::string& name);
        bool IsOperatorName(const std::string& name) const;
        // operators may be listed by name or by xuid; with xbox_auth only an xuid is a safe key
        bool IsOperator(const std::string& name, const std::string& xuid) const;
        bool Full() const;
        double SpawnY() const;

        // players editing the world: the server validates, runs plugin events, applies and tells everyone who has the chunk
        bool PlayerBreak(Session& by, int x, int y, int z);
        bool PlayerPlace(Session& by, int x, int y, int z, int face);
        void SendBlockTo(Session& to, int x, int y, int z);
        const std::vector<uint8_t>& ItemRegistryBody() const { return itemRegistryBody; }
        const std::vector<uint8_t>& CreativeContentBody() const { return creativeBody; }

        // PluginApi
        void Broadcast(const std::string& text) override;
        bool Message(uint64_t player, const std::string& text) override;
        bool Kick(uint64_t player, const std::string& reason) override;
        void RegisterCommand(const std::string& plugin, const std::string& name, const std::string& description) override;
        std::vector<PlayerInfo> Players() override;
        bool Teleport(uint64_t player, double x, double y, double z) override;
        int SetBlock(int x, int y, int z, const std::string& block) override;
        int GetBlock(int x, int y, int z, std::string& block) override;
        Status GetStatus() override;
        int GiveItem(uint64_t player, const std::string& item, int count) override;

        std::string Describe() const;

    private:
        void RegisterBuiltins();
        void BuildItemPackets();
        // writes a block into the world and sends the change to every player who has that column
        void ApplyBlock(int x, int y, int z, uint16_t block);
        void Pump();
        void ReapClosed();

        ServerConfig config;
        std::unique_ptr<ChunkManager> chunks;
        std::unique_ptr<PluginHost> plugins;
        std::unique_ptr<LoopbackListener> loopback;
        std::unique_ptr<Listener> nethernet;
        EventQueue events;

        std::unordered_map<uint64_t, std::unique_ptr<Session>> sessions;
        uint64_t nextRuntimeId = 1;
        std::map<std::string, CommandDef> commands;
        std::vector<uint8_t> itemRegistryBody, creativeBody;
        uint64_t tick = 0;
        std::atomic<bool> stopRequested{false};
        bool started = false;
    };

    // implemented in net/NetherNetListener.cpp when built with nethernet support
    std::unique_ptr<Listener> CreateNetherNetListener(const ServerConfig& config, std::function<std::string()> infoJson);
}
