#include "core/Config.hpp"

#include "core/Json.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace perlin::server {

    ServerConfig ServerConfig::Load(const std::string& path, std::string* error) {
        ServerConfig c;
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return c;
        std::stringstream buf;
        buf << in.rdbuf();
        json::Value root;
        std::string err;
        if (!json::Parse(buf.str(), root, &err)) {
            if (error)
                *error = path + ": " + err;
            return c;
        }
        c.name = root["name"].String(c.name);
        c.motd = root["motd"].String(c.motd);
        c.levelName = root["level_name"].String(c.levelName);
        c.xboxAuth = root["xbox_auth"].Bool(c.xboxAuth);
        for (const json::Value& k : root["auth_root_keys"].items)
            if (k.IsString())
                c.authRootKeys.push_back(k.text);
        c.pluginDir = root["plugin_dir"].String(c.pluginDir);
        c.blockDataDir = root["block_data_dir"].String(c.blockDataDir);
        c.maxPlayers = static_cast<uint32_t>(std::clamp<int64_t>(root["max_players"].Int(c.maxPlayers), 1, 1000));
        c.viewDistance = static_cast<uint32_t>(std::clamp<int64_t>(root["view_distance"].Int(c.viewDistance), 2, 64));
        c.tickRate = static_cast<uint32_t>(std::clamp<int64_t>(root["tick_rate"].Int(c.tickRate), 1, 200));
        c.gameMode = static_cast<int>(std::clamp<int64_t>(root["gamemode"].Int(c.gameMode), 0, 2));
        c.seed = static_cast<uint64_t>(root["seed"].Int(static_cast<int64_t>(c.seed)));
        c.nethernet = root["nethernet"].Bool(c.nethernet);
        c.port = static_cast<uint16_t>(std::clamp<int64_t>(root["port"].Int(c.port), 1, 65535));
        c.bindAddress = root["bind_address"].String(c.bindAddress);
        for (const json::Value& s : root["ice_servers"].items)
            if (s.IsString())
                c.iceServers.push_back(s.text);
        for (const json::Value& o : root["operators"].items)
            if (o.IsString())
                c.operators.push_back(o.text);
        c.iceUdpMux = root["ice_udp_mux"].Bool(c.iceUdpMux);
        c.iceBindAddress = root["ice_bind_address"].String(c.iceBindAddress);
        c.generator = root["generator"].String(c.generator);
        c.terrainBackend = root["terrain_backend"].String(c.terrainBackend);
        if (root["flat_layers"].IsArray()) {
            c.flatLayers.clear();
            for (const json::Value& l : root["flat_layers"].items) {
                FlatLayer layer;
                layer.block = l["block"].String();
                layer.count = static_cast<int>(std::clamp<int64_t>(l["count"].Int(1), 1, 384));
                if (!layer.block.empty())
                    c.flatLayers.push_back(std::move(layer));
            }
        }
        return c;
    }

    bool ServerConfig::Save(const std::string& path) const {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out << "{\n"
            << "  \"name\": \"" << json::Escape(name) << "\",\n"
            << "  \"motd\": \"" << json::Escape(motd) << "\",\n"
            << "  \"level_name\": \"" << json::Escape(levelName) << "\",\n"
            << "  \"max_players\": " << maxPlayers << ",\n"
            << "  \"view_distance\": " << viewDistance << ",\n"
            << "  \"tick_rate\": " << tickRate << ",\n"
            << "  \"gamemode\": " << gameMode << ",\n"
            << "  \"seed\": " << static_cast<int64_t>(seed) << ",\n"
            << "  \"nethernet\": " << (nethernet ? "true" : "false") << ",\n"
            << "  \"port\": " << port << ",\n"
            << "  \"bind_address\": \"" << json::Escape(bindAddress) << "\",\n"
            << "  \"ice_udp_mux\": " << (iceUdpMux ? "true" : "false") << ",\n"
            << "  \"ice_bind_address\": \"" << json::Escape(iceBindAddress) << "\",\n"
            << "  \"generator\": \"" << json::Escape(generator) << "\",\n"
            << "  \"terrain_backend\": \"" << json::Escape(terrainBackend) << "\",\n"
            << "  \"operators\": [";
        for (size_t i = 0; i < operators.size(); i++)
            out << (i ? ", " : "") << "\"" << json::Escape(operators[i]) << "\"";
        out << "],\n"
            << "  \"xbox_auth\": " << (xboxAuth ? "true" : "false") << ",\n"
            << "  \"plugin_dir\": \"" << json::Escape(pluginDir) << "\",\n"
            << "  \"block_data_dir\": \"" << json::Escape(blockDataDir) << "\",\n"
            << "  \"flat_layers\": [";
        for (size_t i = 0; i < flatLayers.size(); i++)
            out << (i ? ", " : "") << "{\"block\": \"" << json::Escape(flatLayers[i].block) << "\", \"count\": " << flatLayers[i].count << "}";
        out << "]\n}\n";
        return out.good();
    }
}
