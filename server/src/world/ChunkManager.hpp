#pragma once
#include "world/Chunk.hpp"
#include "world/Generator.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace perlin::server {

    struct ChunkEntry {
        ChunkColumn column;
        // encoded LevelChunk payload, rebuilt when the column changes
        std::vector<uint8_t> payload;
        uint64_t payloadVersion = ~0ull;
    };

    // owns loaded columns; generation runs on a worker so the tick never waits for terrain
    class ChunkManager {
    public:
        explicit ChunkManager(std::unique_ptr<TerrainBackend> backend);
        ~ChunkManager();

        const char* BackendName() const { return backend->Name(); }

        // null while the column is still being generated; asks for it on the first miss
        std::shared_ptr<ChunkEntry> Get(ChunkPos pos);
        std::shared_ptr<ChunkEntry> Peek(ChunkPos pos) const;
        // moves finished columns into the map; call once per tick
        void Pump();

        const std::vector<uint8_t>& Payload(ChunkEntry& entry) const;

        uint16_t GetBlock(int x, int y, int z) const;
        // false when the column is not loaded
        bool SetBlock(int x, int y, int z, uint16_t block);

        size_t Loaded() const { return chunks.size(); }
        size_t Pending() const;

    private:
        void Worker();

        std::unique_ptr<TerrainBackend> backend;
        std::unordered_map<ChunkPos, std::shared_ptr<ChunkEntry>, ChunkPosHash> chunks;
        std::unordered_set<ChunkPos, ChunkPosHash> requested;

        mutable std::mutex mutex;
        std::condition_variable wake;
        std::deque<ChunkPos> queue;
        std::vector<std::pair<ChunkPos, std::shared_ptr<ChunkEntry>>> done;
        bool stopping = false;
        std::thread worker;
    };
}
