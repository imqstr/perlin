#pragma once
#include "plugin/Payload.hpp"
#include "plugin/PluginApi.hpp"
#include "../sdk/perlin_server.h"

#include <memory>
#include <string>
#include <vector>

namespace perlin::server {

    class PluginHost {
    public:
        struct Info {
            std::string name;
            std::string file;
            std::string version;
            std::string author;
            std::string description;
            std::string engine;
            bool enabled = false;
            std::string error;
        };

        explicit PluginHost(PluginApi& api);
        ~PluginHost();

        // loads every *.wasm in `dir`; a plugin that fails to load is reported and skipped
        void LoadDirectory(const std::string& dir);
        bool LoadFile(const std::string& path, std::string& error);
        void UnloadAll();

        // result bit 0 set when a plugin cancelled / handled; stops at the first plugin that does
        uint32_t Dispatch(PsEvent event, const std::vector<uint8_t>& payload);
        // events tick-rate hot paths can skip building when nobody listens
        bool AnyoneListens(PsEvent event) const;
        // plugin that registered a command, empty if none
        bool DispatchCommand(const std::string& plugin, uint64_t player, const std::string& command, const std::string& args);

        std::vector<Info> List() const;

    private:
        struct Plugin;
        // false (and the plugin disabled) when it trapped
        bool CallPlugin(Plugin& plugin, uint32_t id, const std::vector<uint8_t>& payload, uint32_t& result);
        std::vector<std::unique_ptr<Plugin>> plugins;
        PluginApi& api;
    };
}
