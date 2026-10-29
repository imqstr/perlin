#pragma once
#include "rhi/Types.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rhi {
    // pack target ids; written by tools/pack_shaders.py
    enum class ShaderTarget : uint8_t { SpirV, Dxil, Hlsl, Msl, Wgsl, Count };

    struct UniformMember {
        std::string name;
        uint32_t offset = 0;
        uint32_t bytes = 0;
        // array length, 1 for plain values
        uint32_t elements = 1;
    };

    // textures are bound by name at fixed slots shared by every program: texture n sits at set 1 binding 2n, its sampler at 2n+1
    struct TextureSlot {
        std::string name;
        uint32_t slot = 0;
        bool array = false;
        // holds depth or 32 bit float data that webgpu can only sample with a nearest sampler
        bool unfilterable = false;
    };

    struct ProgramInfo {
        std::string name;
        // size of the `Uniforms` block, 0 when the program has none
        uint32_t uniformBytes = 0;
        std::vector<UniformMember> uniforms;

        const UniformMember* FindUniform(std::string_view member) const {
            for (const UniformMember& u : uniforms)
                if (u.name == member)
                    return &u;
            return nullptr;
        }
    };

    // read-only view over a shader pack; the bytes must outlive it
    class ShaderLibrary {
    public:
        bool Load(std::span<const uint8_t> pack, std::string* error = nullptr);

        const std::vector<ProgramInfo>& Programs() const { return programs; }
        const std::vector<TextureSlot>& TextureSlots() const { return slots; }
        const TextureSlot* FindTexture(std::string_view name) const;
        const ProgramInfo* Find(std::string_view name) const;
        // empty when the program lacks that stage or the pack was built without the target
        std::span<const uint8_t> Code(const ProgramInfo& program, ShaderStage stage, ShaderTarget target) const;

        static ShaderTarget TargetFor(Api api);
        static ShaderFormat FormatFor(ShaderTarget target);
        static const char* EntryFor(ShaderStage stage, ShaderTarget target);

    private:
        struct Blob {
            uint8_t stage = 0;
            uint8_t target = 0;
            uint32_t offset = 0;
            uint32_t bytes = 0;
        };
        std::span<const uint8_t> data;
        std::vector<ProgramInfo> programs;
        std::vector<TextureSlot> slots;
        std::vector<std::vector<Blob>> blobs;
    };
}
