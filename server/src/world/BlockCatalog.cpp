#include "world/BlockCatalog.hpp"

#include "core/Json.hpp"
#include "core/Log.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#ifndef PERLIN_SERVER_RESOURCE_DIR
#define PERLIN_SERVER_RESOURCE_DIR ""
#endif

namespace perlin::server {
    namespace fs = std::filesystem;

    BlockCatalog& BlockCatalog::Instance() {
        static BlockCatalog catalog;
        return catalog;
    }

    std::string BlockCatalog::FindFile(const std::string& dir, const std::string& name) {
        std::error_code ec;
        for (const std::string& base : {dir, dir.empty() ? std::string() : dir + "/resources", std::string("resources"),
                                        std::string(PERLIN_SERVER_RESOURCE_DIR), std::string("../resources")}) {
            if (base.empty())
                continue;
            const fs::path p = fs::path(base) / name;
            if (fs::is_regular_file(p, ec))
                return p.string();
        }
        return {};
    }

    const CatalogItem* BlockCatalog::ItemById(int16_t id) const {
        const auto it = byId.find(id);
        return it == byId.end() ? nullptr : &items[it->second];
    }
    const CatalogItem* BlockCatalog::ItemByBlockHash(int32_t hash) const {
        const auto it = byHash.find(hash);
        return it == byHash.end() ? nullptr : &items[it->second];
    }
    const CatalogItem* BlockCatalog::ItemByName(const std::string& name) const {
        const auto it = byName.find(name);
        return it == byName.end() ? nullptr : &items[it->second];
    }

    uint16_t BlockCatalog::RegistryId(int32_t hash) const {
        const auto it = states.find(hash);
        if (it == states.end())
            return BlockRegistry::kAir;
        return BlockRegistry::Instance().Intern(it->second.name, it->second.list);
    }

    void BlockCatalog::BuildItems(const std::vector<std::string>& names, const std::unordered_map<std::string, int32_t>& defaultHash) {
        items.clear();
        byId.clear();
        byHash.clear();
        byName.clear();
        std::vector<std::string> sorted = names;
        std::sort(sorted.begin(), sorted.end());
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
        for (const std::string& name : sorted) {
            const auto it = defaultHash.find("minecraft:" + name);
            if (it == defaultHash.end() || name == "air")
                continue;
            CatalogItem item;
            item.name = "minecraft:" + name;
            item.networkId = static_cast<int16_t>(items.size() + 1);
            item.blockHash = it->second;
            byId[item.networkId] = items.size();
            byHash[item.blockHash] = items.size();
            byName[item.name] = items.size();
            items.push_back(std::move(item));
        }
    }

    bool BlockCatalog::Load(const std::string& blockStatesPath, const std::string& itemNamesPath, std::string& error) {
        std::ifstream states_in(blockStatesPath, std::ios::binary);
        if (!states_in) {
            error = "cannot read " + blockStatesPath;
            return false;
        }
        std::stringstream text;
        text << states_in.rdbuf();
        json::Value root;
        std::string parseError;
        if (!json::Parse(text.str(), root, &parseError) || !root.IsArray()) {
            error = blockStatesPath + ": " + (parseError.empty() ? "not an array" : parseError);
            return false;
        }
        states.clear();
        std::unordered_map<std::string, int32_t> defaultHash;
        states.reserve(root.items.size());
        for (const json::Value& entry : root.items) {
            const std::string name = entry[1].String();
            if (name.empty())
                continue;
            const int32_t hash = static_cast<int32_t>(entry[0].Int());
            State s;
            s.name = name;
            // keys arrive sorted in the file, the registry sorts again anyway
            for (const auto& [key, v] : entry[2].members) {
                const std::string type = v[0].String();
                if (type == "b")
                    s.list.emplace_back(key, StateValue(v[1].Int() != 0));
                else if (type == "i")
                    s.list.emplace_back(key, StateValue(static_cast<int32_t>(v[1].Int())));
                else
                    s.list.emplace_back(key, StateValue(v[1].String()));
            }
            // the first permutation listed is the default one
            defaultHash.emplace(name, hash);
            states.emplace(hash, std::move(s));
        }
        std::vector<std::string> names;
        std::ifstream map(itemNamesPath);
        std::string line;
        while (std::getline(map, line)) {
            if (line.empty() || line[0] == '#')
                continue;
            // block_destroy.tsv: name, hardness, tool, level; hardness -1 marks technical blocks that never become items
            // (bedrock and barrier stay: creative players build with them)
            const size_t tab = line.find('\t');
            if (tab == std::string::npos)
                continue;
            const std::string name = line.substr(0, tab);
            const size_t next = line.find('\t', tab + 1);
            const std::string hardness = line.substr(tab + 1, next == std::string::npos ? std::string::npos : next - tab - 1);
            if (hardness == "-1" && name != "bedrock" && name != "barrier")
                continue;
            names.push_back(name);
        }
        if (names.empty()) {
            // no list of item names: every block that has a single state
            for (const auto& [n, h] : defaultHash)
                names.push_back(n.substr(10));
        }
        BuildItems(names, defaultHash);
        fromFiles = true;
        return !items.empty();
    }

    void BlockCatalog::LoadBuiltin() {
        static const char* const kNames[] = {
            "stone", "granite", "diorite", "andesite", "cobblestone", "mossy_cobblestone", "dirt", "grass_block", "sand", "gravel",
            "oak_planks", "spruce_planks", "birch_planks", "jungle_planks", "acacia_planks", "dark_oak_planks", "glass", "bricks",
            "stone_bricks", "sandstone", "obsidian", "bedrock", "white_wool", "orange_wool", "red_wool", "blue_wool", "black_wool",
            "gold_block", "iron_block", "diamond_block", "emerald_block", "coal_block", "bookshelf", "glowstone", "netherrack",
            "snow", "clay", "pumpkin", "melon_block", "sea_lantern", "quartz_block", "prismarine"};
        states.clear();
        std::unordered_map<std::string, int32_t> defaultHash;
        std::vector<std::string> names;
        for (const char* n : kNames) {
            const std::string full = std::string("minecraft:") + n;
            const int32_t hash = BlockNetworkHash(full, {});
            State s;
            s.name = full;
            states.emplace(hash, std::move(s));
            defaultHash.emplace(full, hash);
            names.push_back(n);
        }
        BuildItems(names, defaultHash);
        fromFiles = false;
    }
}
