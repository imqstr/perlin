#pragma once
#include "client/javashaders/Transform.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace perlin::jshader {
    enum class BackendKind { OpenGL, D3D11, Vulkan };

    using HlslCompiler = std::function<bool(const std::string& hlsl, bool fragment, std::vector<uint8_t>& dxbc, std::string& error)>;

    struct BackendOptions {
        BackendKind kind = BackendKind::OpenGL;
        int glslVersion = 430;
        HlslCompiler hlslCompiler;
        bool explicitLod0 = false;
    };

    struct SamplerInfo {
        std::string name;
        int stage = 0;
        bool shadow = false;
        int dimension = 2;
        int component = 0;
    };

    struct BackendProgram {
        bool ok = false;
        std::string error;
        std::vector<uint8_t> vertex, fragment;
        std::vector<SamplerInfo> samplers;
        int uniformRegs = 0;
        std::string vertexSource, fragmentSource;
    };

    BackendProgram BuildBackendProgram(const ProgramResult& program, const BackendOptions& options);

    struct ContainerUniform {
        std::string name;
        uint8_t type = 0;
        uint8_t num = 0;
        uint16_t regIndex = 0, regCount = 0;
        uint8_t texComponent = 0, texDimension = 0;
        uint16_t texFormat = 0;
    };
    std::vector<uint8_t> WriteBgfxShader(bool fragment, const std::vector<ContainerUniform>& uniforms, const void* code,
                                         size_t codeSize, const std::vector<uint16_t>& attributes, uint16_t constantBytes,
                                         uint32_t interfaceHash);
}
