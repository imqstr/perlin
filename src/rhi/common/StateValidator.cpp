#include "rhi/common/StateValidator.hpp"

#include <algorithm>

namespace rhi {
    uint32_t StateValidator::LayerCount(const TextureDesc& desc) {
        switch (desc.dim) {
        case TextureDim::Tex2DArray: return (std::max)(1u, desc.depthOrLayers);
        case TextureDim::Cube: return 6;
        default: return 1;
        }
    }

    std::string StateValidator::StateName(State state) {
        if (state == State::Undefined)
            return "Undefined";
        static const struct {
            State bit;
            const char* name;
        } kNames[] = {
            {State::CopySrc, "CopySrc"},
            {State::CopyDst, "CopyDst"},
            {State::VertexBuffer, "VertexBuffer"},
            {State::IndexBuffer, "IndexBuffer"},
            {State::UniformBuffer, "UniformBuffer"},
            {State::IndirectArgument, "IndirectArgument"},
            {State::ShaderReadFragment, "ShaderReadFragment"},
            {State::ShaderReadOther, "ShaderReadOther"},
            {State::ShaderWrite, "ShaderWrite"},
            {State::ColorTarget, "ColorTarget"},
            {State::DepthWrite, "DepthWrite"},
            {State::DepthRead, "DepthRead"},
            {State::Present, "Present"},
            {State::ResolveSrc, "ResolveSrc"},
            {State::ResolveDst, "ResolveDst"},
        };
        std::string out;
        for (const auto& entry : kNames)
            if (Has(state, entry.bit)) {
                if (!out.empty())
                    out += '|';
                out += entry.name;
            }
        return out;
    }

    void StateValidator::Report(std::string message) {
        total++;
        if (errors.size() < 64)
            errors.push_back(std::move(message));
    }

    void StateValidator::OnCreate(TextureHandle handle, const TextureDesc& desc, State initial) {
        std::lock_guard lock(mutex);
        TextureInfo info;
        info.desc = desc;
        info.layers = LayerCount(desc);
        info.states.assign(static_cast<size_t>(std::max(1u, desc.mips)) * info.layers, initial);
        textures[handle.value] = std::move(info);
    }

    void StateValidator::OnCreate(BufferHandle handle, const BufferDesc& desc) {
        std::lock_guard lock(mutex);
        buffers[handle.value] = {desc, State::Undefined};
    }

    void StateValidator::OnDestroy(TextureHandle handle) {
        std::lock_guard lock(mutex);
        textures.erase(handle.value);
    }

    void StateValidator::OnDestroy(BufferHandle handle) {
        std::lock_guard lock(mutex);
        buffers.erase(handle.value);
    }

    void StateValidator::Barrier(const TextureBarrier& barrier) {
        std::lock_guard lock(mutex);
        auto it = textures.find(barrier.texture.value);
        if (it == textures.end()) {
            Report("barrier on a destroyed or unknown texture");
            return;
        }
        TextureInfo& info = it->second;
        const uint32_t mips = (std::max)(1u, info.desc.mips);
        const uint32_t m0 = barrier.range.baseMip;
        const uint32_t m1 = barrier.range.mipCount ? (std::min)(mips, m0 + barrier.range.mipCount) : mips;
        const uint32_t l0 = barrier.range.baseLayer;
        const uint32_t l1 = barrier.range.layerCount ? (std::min)(info.layers, l0 + barrier.range.layerCount) : info.layers;
        for (uint32_t m = m0; m < m1; m++)
            for (uint32_t l = l0; l < l1; l++) {
                State& state = info.states[static_cast<size_t>(l) * mips + m];
                // Undefined as `before` means "contents may be discarded": always legal
                if (barrier.before != State::Undefined && barrier.before != state) {
                    Report(std::string("texture '") + (info.desc.name ? info.desc.name : "?") + "' mip " + std::to_string(m) +
                           " layer " + std::to_string(l) + ": barrier says it was " + StateName(barrier.before) + " but it was left " +
                           StateName(state));
                    state = barrier.after;
                    continue;
                }
                state = barrier.after;
            }
    }

    void StateValidator::Barrier(const BufferBarrier& barrier) {
        std::lock_guard lock(mutex);
        auto it = buffers.find(barrier.buffer.value);
        if (it == buffers.end()) {
            Report("barrier on a destroyed or unknown buffer");
            return;
        }
        BufferInfo& info = it->second;
        // cpu-visible classes sit in a fixed state on every api; barriers on them are ignored
        if (info.desc.memory == MemoryClass::Upload || info.desc.memory == MemoryClass::Readback)
            return;
        if (barrier.before != State::Undefined && info.state != State::Undefined && barrier.before != info.state)
            Report(std::string("buffer '") + (info.desc.name ? info.desc.name : "?") + "': barrier says it was " +
                   StateName(barrier.before) + " but it was left " + StateName(info.state));
        info.state = barrier.after;
    }

    void StateValidator::ExpectTexture(TextureHandle handle, const SubresourceRange& range, State want, const char* what) {
        std::lock_guard lock(mutex);
        auto it = textures.find(handle.value);
        if (it == textures.end()) {
            Report(std::string(what) + ": unknown texture");
            return;
        }
        TextureInfo& info = it->second;
        const uint32_t mips = (std::max)(1u, info.desc.mips);
        const uint32_t m0 = range.baseMip;
        const uint32_t m1 = range.mipCount ? (std::min)(mips, m0 + range.mipCount) : mips;
        const uint32_t l0 = range.baseLayer;
        const uint32_t l1 = range.layerCount ? (std::min)(info.layers, l0 + range.layerCount) : info.layers;
        for (uint32_t m = m0; m < m1; m++)
            for (uint32_t l = l0; l < l1; l++) {
                const State state = info.states[static_cast<size_t>(l) * mips + m];
                if ((static_cast<uint32_t>(state) & static_cast<uint32_t>(want)) == 0) {
                    Report(std::string(what) + ": texture '" + (info.desc.name ? info.desc.name : "?") + "' mip " + std::to_string(m) +
                           " layer " + std::to_string(l) + " is " + StateName(state) + ", needs " + StateName(want));
                    return;
                }
            }
    }

    void StateValidator::ExpectBuffer(BufferHandle handle, State want, const char* what) {
        std::lock_guard lock(mutex);
        auto it = buffers.find(handle.value);
        if (it == buffers.end()) {
            Report(std::string(what) + ": unknown buffer");
            return;
        }
        const BufferInfo& info = it->second;
        if (info.desc.memory == MemoryClass::Upload || info.desc.memory == MemoryClass::Readback)
            return;
        // a buffer nobody transitioned yet is treated as usable for anything
        if (info.state == State::Undefined)
            return;
        if ((static_cast<uint32_t>(info.state) & static_cast<uint32_t>(want)) == 0)
            Report(std::string(what) + ": buffer '" + (info.desc.name ? info.desc.name : "?") + "' is " + StateName(info.state) +
                   ", needs " + StateName(want));
    }

    void StateValidator::ExpectAll(const std::vector<BoundResource>& resources, const char* what) {
        for (const BoundResource& r : resources) {
            if (r.texture)
                ExpectTexture(r.texture, r.range, r.want, what);
            else if (r.buffer)
                ExpectBuffer(r.buffer, r.want, what);
        }
    }

    size_t StateValidator::ErrorCount() const {
        std::lock_guard lock(mutex);
        return total;
    }

    std::vector<std::string> StateValidator::TakeErrors() {
        std::lock_guard lock(mutex);
        std::vector<std::string> out;
        out.swap(errors);
        total = 0;
        return out;
    }
}
