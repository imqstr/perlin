#pragma once
#include "core/Config.hpp"
#include "world/Chunk.hpp"

#include <memory>
#include <span>
#include <string>

namespace perlin::server {

    // one batch in, one column per position out; implementations are free to run the whole batch at once
    // (the gpu backend does) and must give identical blocks for the same seed and position
    class TerrainBackend {
    public:
        virtual ~TerrainBackend() = default;
        virtual const char* Name() const = 0;
        virtual void Generate(std::span<const ChunkPos> positions, std::span<ChunkColumn> out) = 0;
        // largest batch worth submitting at once
        virtual size_t PreferredBatch() const { return 1; }
    };

    // flat layers on the cpu: also the reference the gpu backend is checked against
    class FlatCpuBackend final : public TerrainBackend {
    public:
        explicit FlatCpuBackend(const ServerConfig& config);
        const char* Name() const override { return "flat/cpu"; }
        void Generate(std::span<const ChunkPos> positions, std::span<ChunkColumn> out) override;
        size_t PreferredBatch() const override { return 8; }

        // block id per world y from the floor up; air above the last layer
        const std::vector<uint16_t>& Profile() const { return profile; }

    private:
        std::vector<uint16_t> profile;
    };

    struct BackendChoice {
        std::unique_ptr<TerrainBackend> backend;
        std::string note;
    };

    // "auto": gpu when it starts and matches the cpu reference on a probe batch, otherwise cpu
    BackendChoice CreateTerrainBackend(const ServerConfig& config);

    // cells of a flat world, also used for the spawn height
    int FlatSurfaceY(const ServerConfig& config);
}
