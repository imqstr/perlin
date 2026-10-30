#pragma once
#include "world/Blocks.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace perlin::server {

    // an item the server hands out: one per placeable block, in the default state
    struct CatalogItem {
        std::string name;
        int16_t networkId = 0;
        int32_t blockHash = 0;
    };

    // every block state the game knows (the client's own block_states.json), plus the item list built from it
    class BlockCatalog {
    public:
        static BlockCatalog& Instance();

        // block_states.json: [[hash, name, {state: [type, value]}, bool], ...]; block_destroy.tsv picks which blocks become items
        bool Load(const std::string& blockStatesPath, const std::string& itemNamesPath, std::string& error);
        // no data files: a short list of blocks that have no states, enough to build with
        void LoadBuiltin();
        bool Loaded() const { return !items.empty(); }
        bool FromFiles() const { return fromFiles; }

        const std::vector<CatalogItem>& Items() const { return items; }
        const CatalogItem* ItemById(int16_t id) const;
        const CatalogItem* ItemByBlockHash(int32_t hash) const;
        const CatalogItem* ItemByName(const std::string& name) const;

        // registry id of the block with this network hash; 0 (air) when the hash is not a known block
        uint16_t RegistryId(int32_t hash) const;
        bool KnownHash(int32_t hash) const { return states.count(hash) != 0; }
        size_t StateCount() const { return states.size(); }

        // search order for the data files: explicit dir, then next to the executable, then the source tree
        static std::string FindFile(const std::string& dir, const std::string& name);

    private:
        struct State {
            std::string name;
            StateList list;
        };
        void BuildItems(const std::vector<std::string>& names, const std::unordered_map<std::string, int32_t>& defaultHash);
        std::unordered_map<int32_t, State> states;
        std::vector<CatalogItem> items;
        std::unordered_map<int16_t, size_t> byId;
        std::unordered_map<int32_t, size_t> byHash;
        std::unordered_map<std::string, size_t> byName;
        bool fromFiles = false;
    };
}
