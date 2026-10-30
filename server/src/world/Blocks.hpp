#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace perlin::server {

    using StateValue = std::variant<bool, int32_t, std::string>;
    using StateList = std::vector<std::pair<std::string, StateValue>>;

    // network block id: fnv-1a over the little endian nbt of {name, states}; states sorted by key
    int32_t BlockNetworkHash(const std::string& name, const StateList& sortedStates);

    // server-side block ids are small and local; the hash is what goes on the wire
    class BlockRegistry {
    public:
        static constexpr uint16_t kAir = 0;

        static BlockRegistry& Instance();

        uint16_t Intern(const std::string& name, StateList states = {});
        // 0 (air) when the name is unknown and `create` is off
        uint16_t Find(const std::string& name) const;
        int32_t Hash(uint16_t id) const;
        std::string Name(uint16_t id) const;
        size_t Size() const;

    private:
        BlockRegistry();
        mutable std::mutex mutex;
        struct Entry {
            std::string name;
            StateList states;
            int32_t hash = 0;
        };
        std::vector<Entry> entries;
        std::unordered_map<std::string, uint16_t> byKey;
    };
}
