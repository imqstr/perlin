#pragma once
#include "world/Generator.hpp"

namespace perlin::server {

    // null with `note` saying why when the gpu path is not built, no device starts, or its output differs from the cpu reference
    std::unique_ptr<TerrainBackend> CreateGpuTerrainBackend(const ServerConfig& config, std::string& note);

    // the cpu/gpu agreement check, exposed so tools and tests can run it on any backend
    bool BackendsAgree(TerrainBackend& candidate, TerrainBackend& reference, std::string& detail);
}
