#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace perlin::server {

    struct ChunkPos {
        int32_t x = 0, z = 0;
        bool operator==(const ChunkPos&) const = default;
    };

    struct ChunkPosHash {
        size_t operator()(const ChunkPos& p) const {
            return static_cast<size_t>((static_cast<uint64_t>(static_cast<uint32_t>(p.x)) << 32) | static_cast<uint32_t>(p.z)) * 0x9E3779B97F4A7C15ull;
        }
    };

    // overworld height: y -64 .. 319
    constexpr int kMinY = -64;
    constexpr int kHeight = 384;
    constexpr int kSections = kHeight / 16;
    constexpr int kCells = 4096;
    constexpr uint8_t kDefaultBiome = 1;

    // cell index inside a section, the order the wire uses: x outermost, y innermost
    constexpr int CellIndex(int x, int y, int z) { return (x << 8) | (z << 4) | (y & 15); }

    class ChunkColumn {
    public:
        struct Section {
            uint16_t uniform = 0;
            std::unique_ptr<std::array<uint16_t, kCells>> cells;
        };

        uint16_t Get(int x, int y, int z) const;
        void Set(int x, int y, int z, uint16_t block);

        // bulk fill from a generator: `cells` is in wire order; collapses to uniform when every cell matches
        void SetSection(int section, const uint16_t* cells);
        void FillSection(int section, uint16_t block);

        const Section& SectionAt(int section) const { return sections[static_cast<size_t>(section)]; }
        bool IsUniform(int section) const { return !sections[static_cast<size_t>(section)].cells; }

        uint64_t version = 0;

        // full LevelChunk payload for protocol 2225: sub chunks, biomes, border byte
        void EncodeNetwork(std::vector<uint8_t>& out) const;

    private:
        std::array<Section, kSections> sections;
    };
}
