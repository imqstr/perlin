#pragma once
#include "rhi/Types.hpp"

#include <algorithm>

namespace rhi {
    constexpr uint64_t AlignUp(uint64_t value, uint64_t alignment) {
        return (value + alignment - 1) / alignment * alignment;
    }

    constexpr uint32_t MipExtent(uint32_t extent, uint32_t mip) { return (std::max)(1u, extent >> mip); }

    inline uint32_t LayerCountOf(const TextureDesc& desc) {
        switch (desc.dim) {
        case TextureDim::Tex2DArray: return (std::max)(1u, desc.depthOrLayers);
        case TextureDim::Cube: return 6;
        default: return 1;
        }
    }

    inline uint32_t DepthOf(const TextureDesc& desc) {
        return desc.dim == TextureDim::Tex3D ? (std::max)(1u, desc.depthOrLayers) : 1u;
    }

    inline SubresourceRange ResolveRange(const SubresourceRange& range, const TextureDesc& desc) {
        SubresourceRange out = range;
        const uint32_t mips = (std::max)(1u, desc.mips);
        const uint32_t layers = LayerCountOf(desc);
        out.baseMip = (std::min)(out.baseMip, mips - 1);
        out.baseLayer = (std::min)(out.baseLayer, layers - 1);
        out.mipCount = out.mipCount ? (std::min)(out.mipCount, mips - out.baseMip) : mips - out.baseMip;
        out.layerCount = out.layerCount ? (std::min)(out.layerCount, layers - out.baseLayer) : layers - out.baseLayer;
        return out;
    }

    inline uint32_t RowBytes(Format format, uint32_t width) {
        const FormatInfo& info = GetFormatInfo(format);
        return (width + info.blockWidth - 1) / info.blockWidth * info.bytesPerBlock;
    }

    inline uint32_t RowBlocks(Format format, uint32_t height) {
        const FormatInfo& info = GetFormatInfo(format);
        return (height + info.blockHeight - 1) / info.blockHeight;
    }

    inline uint32_t MipLevelsFor(uint32_t width, uint32_t height) {
        uint32_t n = 1;
        while ((std::max(width, height) >> n) > 0)
            n++;
        return n;
    }
}
