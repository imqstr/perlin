#pragma once
#include "client/javashaders/Transform.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace perlin::jshader {
    // what the running rhi device takes: spir-v (vulkan), hlsl source (d3d11) or dxbc (d3d12)
    enum class RhiShaderKind { SpirV, Hlsl, Dxbc };

    using DxbcCompiler = std::function<bool(const std::string& hlsl, bool fragment, std::vector<uint8_t>& dxbc, std::string& error)>;

    struct RhiSampler {
        std::string name;
        int slot = 0;
        bool shadow = false;
    };

    struct RhiProgram {
        bool ok = false;
        std::string error;
        RhiShaderKind kind = RhiShaderKind::SpirV;
        std::vector<uint8_t> vertex, fragment;
        std::vector<RhiSampler> samplers;
        int uniformRegs = 0;
        std::string vertexSource, fragmentSource;
    };

    // `program` must come from TransformProgram with splitSamplers set
    RhiProgram BuildRhiProgram(const ProgramResult& program, RhiShaderKind kind, const DxbcCompiler& dxbc);

    // d3dcompiler_47 on windows, empty elsewhere
    DxbcCompiler SystemDxbcCompiler();
}
