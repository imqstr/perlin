#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace perlin::server {

    // what plugins may do to the server; implemented by Server so the plugin host does not depend on it
    class PluginApi {
    public:
        struct PlayerInfo {
            uint64_t id = 0;
            std::string name;
            double x = 0, y = 0, z = 0;
        };
        struct Status {
            std::string name;
            uint32_t players = 0;
            uint32_t maxPlayers = 0;
            uint64_t tick = 0;
            std::string backend;
        };

        virtual ~PluginApi() = default;
        virtual void Broadcast(const std::string& text) = 0;
        virtual bool Message(uint64_t player, const std::string& text) = 0;
        virtual bool Kick(uint64_t player, const std::string& reason) = 0;
        virtual void RegisterCommand(const std::string& plugin, const std::string& name, const std::string& description) = 0;
        virtual std::vector<PlayerInfo> Players() = 0;
        virtual bool Teleport(uint64_t player, double x, double y, double z) = 0;
        // 0 ok, PsStatus otherwise
        virtual int SetBlock(int x, int y, int z, const std::string& block) = 0;
        virtual int GetBlock(int x, int y, int z, std::string& block) = 0;
        virtual Status GetStatus() = 0;
        // count that did not fit, or a negative PsStatus
        virtual int GiveItem(uint64_t player, const std::string& item, int count) = 0;
    };
}
