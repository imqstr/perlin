#include "rhi/ShaderLibrary.hpp"

#include <cstring>

namespace rhi {
    namespace {
        constexpr uint32_t kMagic = 0x48535250;
        constexpr uint32_t kVersion = 1;

        struct Reader {
            std::span<const uint8_t> bytes;
            size_t at = 0;
            bool ok = true;

            template <class T>
            T Read() {
                T value{};
                if (at + sizeof(T) > bytes.size()) {
                    ok = false;
                    return value;
                }
                std::memcpy(&value, bytes.data() + at, sizeof(T));
                at += sizeof(T);
                return value;
            }

            std::string String() {
                const uint16_t length = Read<uint16_t>();
                if (!ok || at + length > bytes.size()) {
                    ok = false;
                    return {};
                }
                std::string out(reinterpret_cast<const char*>(bytes.data() + at), length);
                at += length;
                return out;
            }
        };

        uint8_t StageId(ShaderStage stage) {
            switch (stage) {
            case ShaderStage::Vertex: return 0;
            case ShaderStage::Fragment: return 1;
            default: return 2;
            }
        }
    }

    bool ShaderLibrary::Load(std::span<const uint8_t> pack, std::string* error) {
        auto fail = [&](const char* message) {
            if (error)
                *error = message;
            programs.clear();
            slots.clear();
            blobs.clear();
            data = {};
            return false;
        };
        Reader header{pack};
        if (header.Read<uint32_t>() != kMagic)
            return fail("not a shader pack");
        if (header.Read<uint32_t>() != kVersion)
            return fail("unsupported shader pack version");
        const uint32_t indexBytes = header.Read<uint32_t>();
        header.Read<uint32_t>();
        if (!header.ok || header.at + indexBytes > pack.size())
            return fail("truncated shader pack");

        Reader r{pack.subspan(header.at, indexBytes)};
        const std::span<const uint8_t> area = pack.subspan(header.at + indexBytes);
        slots.clear();
        const uint16_t slotCount = r.Read<uint16_t>();
        for (uint16_t i = 0; i < slotCount && r.ok; i++) {
            TextureSlot slot;
            slot.name = r.String();
            slot.slot = r.Read<uint8_t>();
            const uint8_t flags = r.Read<uint8_t>();
            slot.array = (flags & 1) != 0;
            slot.unfilterable = (flags & 2) != 0;
            slots.push_back(std::move(slot));
        }
        const uint32_t count = r.Read<uint32_t>();
        programs.clear();
        blobs.clear();
        for (uint32_t p = 0; p < count && r.ok; p++) {
            ProgramInfo info;
            info.name = r.String();
            info.uniformBytes = r.Read<uint32_t>();
            const uint16_t uniformCount = r.Read<uint16_t>();
            for (uint16_t u = 0; u < uniformCount && r.ok; u++) {
                UniformMember m;
                m.name = r.String();
                m.offset = r.Read<uint32_t>();
                m.bytes = r.Read<uint32_t>();
                m.elements = r.Read<uint16_t>();
                info.uniforms.push_back(std::move(m));
            }
            std::vector<Blob> list;
            const uint8_t stageCount = r.Read<uint8_t>();
            for (uint8_t s = 0; s < stageCount && r.ok; s++) {
                const uint8_t stage = r.Read<uint8_t>();
                const uint8_t targetCount = r.Read<uint8_t>();
                for (uint8_t t = 0; t < targetCount && r.ok; t++) {
                    Blob b;
                    b.stage = stage;
                    b.target = r.Read<uint8_t>();
                    b.offset = r.Read<uint32_t>();
                    b.bytes = r.Read<uint32_t>();
                    if (static_cast<uint64_t>(b.offset) + b.bytes > area.size())
                        return fail("shader blob outside the pack");
                    list.push_back(b);
                }
            }
            programs.push_back(std::move(info));
            blobs.push_back(std::move(list));
        }
        if (!r.ok)
            return fail("truncated shader pack index");
        data = area;
        return true;
    }

    const TextureSlot* ShaderLibrary::FindTexture(std::string_view name) const {
        for (const TextureSlot& s : slots)
            if (s.name == name)
                return &s;
        return nullptr;
    }

    const ProgramInfo* ShaderLibrary::Find(std::string_view name) const {
        for (const ProgramInfo& p : programs)
            if (p.name == name)
                return &p;
        return nullptr;
    }

    std::span<const uint8_t> ShaderLibrary::Code(const ProgramInfo& program, ShaderStage stage, ShaderTarget target) const {
        const size_t index = static_cast<size_t>(&program - programs.data());
        if (index >= programs.size())
            return {};
        for (const Blob& b : blobs[index])
            if (b.stage == StageId(stage) && b.target == static_cast<uint8_t>(target))
                return data.subspan(b.offset, b.bytes);
        return {};
    }

    ShaderTarget ShaderLibrary::TargetFor(Api api) {
        switch (api) {
        case Api::Vulkan: return ShaderTarget::SpirV;
        case Api::D3D12: return ShaderTarget::Dxil;
        case Api::D3D11: return ShaderTarget::Hlsl;
        case Api::Metal: return ShaderTarget::Msl;
        case Api::WebGPU: return ShaderTarget::Wgsl;
        default: return ShaderTarget::SpirV;
        }
    }

    ShaderFormat ShaderLibrary::FormatFor(ShaderTarget target) {
        switch (target) {
        case ShaderTarget::Dxil: return ShaderFormat::Dxil;
        case ShaderTarget::Hlsl: return ShaderFormat::Hlsl;
        case ShaderTarget::Msl: return ShaderFormat::Msl;
        case ShaderTarget::Wgsl: return ShaderFormat::Wgsl;
        default: return ShaderFormat::SpirV;
        }
    }

    const char* ShaderLibrary::EntryFor(ShaderStage stage, ShaderTarget target) {
        // spir-v and dxil carry their own entry; source targets keep the slang names
        if (target == ShaderTarget::SpirV || target == ShaderTarget::Dxil)
            return "main";
        switch (stage) {
        case ShaderStage::Vertex: return "vsMain";
        case ShaderStage::Fragment: return "fsMain";
        default: return "csMain";
        }
    }
}
