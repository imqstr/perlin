#pragma once
#include "rhi/Types.hpp"

#include <directx/d3d12.h>
#include <dxgiformat.h>

namespace rhi::d3d12 {
    struct FormatMap {
        // resource format: typeless where the same bits are viewed as several formats (srgb, depth + sampled)
        DXGI_FORMAT resource = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT dsv = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT stencilView = DXGI_FORMAT_UNKNOWN;
        uint32_t planes = 1;
    };

    inline FormatMap MapFormat(Format f) {
        FormatMap m;
        auto simple = [&](DXGI_FORMAT x) {
            m.resource = m.view = x;
        };
        switch (f) {
        case Format::R8Unorm: simple(DXGI_FORMAT_R8_UNORM); break;
        case Format::RG8Unorm: simple(DXGI_FORMAT_R8G8_UNORM); break;
        case Format::RGBA8Unorm:
            m.resource = DXGI_FORMAT_R8G8B8A8_TYPELESS;
            m.view = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;
        case Format::RGBA8Srgb:
            m.resource = DXGI_FORMAT_R8G8B8A8_TYPELESS;
            m.view = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            break;
        case Format::BGRA8Unorm:
            m.resource = DXGI_FORMAT_B8G8R8A8_TYPELESS;
            m.view = DXGI_FORMAT_B8G8R8A8_UNORM;
            break;
        case Format::BGRA8Srgb:
            m.resource = DXGI_FORMAT_B8G8R8A8_TYPELESS;
            m.view = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            break;
        case Format::R8Uint: simple(DXGI_FORMAT_R8_UINT); break;
        case Format::R16Uint: simple(DXGI_FORMAT_R16_UINT); break;
        case Format::R32Uint: simple(DXGI_FORMAT_R32_UINT); break;
        case Format::RG32Uint: simple(DXGI_FORMAT_R32G32_UINT); break;
        case Format::RGBA32Uint: simple(DXGI_FORMAT_R32G32B32A32_UINT); break;
        case Format::R16Float: simple(DXGI_FORMAT_R16_FLOAT); break;
        case Format::RG16Float: simple(DXGI_FORMAT_R16G16_FLOAT); break;
        case Format::RGBA16Float: simple(DXGI_FORMAT_R16G16B16A16_FLOAT); break;
        case Format::R32Float: simple(DXGI_FORMAT_R32_FLOAT); break;
        case Format::RG32Float: simple(DXGI_FORMAT_R32G32_FLOAT); break;
        case Format::RGB32Float: simple(DXGI_FORMAT_R32G32B32_FLOAT); break;
        case Format::RGBA32Float: simple(DXGI_FORMAT_R32G32B32A32_FLOAT); break;
        case Format::RGB10A2Unorm: simple(DXGI_FORMAT_R10G10B10A2_UNORM); break;
        case Format::R11G11B10Float: simple(DXGI_FORMAT_R11G11B10_FLOAT); break;
        case Format::RG16Unorm: simple(DXGI_FORMAT_R16G16_UNORM); break;
        case Format::RGBA16Unorm: simple(DXGI_FORMAT_R16G16B16A16_UNORM); break;
        case Format::D16Unorm:
            m.resource = DXGI_FORMAT_R16_TYPELESS;
            m.view = DXGI_FORMAT_R16_UNORM;
            m.dsv = DXGI_FORMAT_D16_UNORM;
            break;
        case Format::D32Float:
            m.resource = DXGI_FORMAT_R32_TYPELESS;
            m.view = DXGI_FORMAT_R32_FLOAT;
            m.dsv = DXGI_FORMAT_D32_FLOAT;
            break;
        case Format::D24UnormS8Uint:
            m.resource = DXGI_FORMAT_R24G8_TYPELESS;
            m.view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            m.stencilView = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
            m.dsv = DXGI_FORMAT_D24_UNORM_S8_UINT;
            m.planes = 2;
            break;
        case Format::D32FloatS8Uint:
            m.resource = DXGI_FORMAT_R32G8X24_TYPELESS;
            m.view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            m.stencilView = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
            m.dsv = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            m.planes = 2;
            break;
        case Format::BC1Unorm: simple(DXGI_FORMAT_BC1_UNORM); break;
        case Format::BC3Unorm: simple(DXGI_FORMAT_BC3_UNORM); break;
        case Format::BC7Unorm: simple(DXGI_FORMAT_BC7_UNORM); break;
        default: break;
        }
        return m;
    }

    constexpr D3D12_RESOURCE_STATES kReadStates = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER |
                                                  D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE |
                                                  D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_RESOLVE_SOURCE;

    // d3d12 states: write states stand alone, read states combine. Present is the common state
    inline D3D12_RESOURCE_STATES ToStates(State s, QueueType queue) {
        if (s == State::Undefined || s == State::Present)
            return D3D12_RESOURCE_STATE_COMMON;
        if (Has(s, State::ShaderWrite))
            return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        if (Has(s, State::ColorTarget))
            return D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (Has(s, State::DepthWrite))
            return D3D12_RESOURCE_STATE_DEPTH_WRITE;
        if (Has(s, State::CopyDst))
            return D3D12_RESOURCE_STATE_COPY_DEST;
        if (Has(s, State::ResolveDst))
            return D3D12_RESOURCE_STATE_RESOLVE_DEST;

        D3D12_RESOURCE_STATES out = D3D12_RESOURCE_STATE_COMMON;
        if (Has(s, State::CopySrc))
            out |= D3D12_RESOURCE_STATE_COPY_SOURCE;
        if (Has(s, State::VertexBuffer) || Has(s, State::UniformBuffer))
            out |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        if (Has(s, State::IndexBuffer))
            out |= D3D12_RESOURCE_STATE_INDEX_BUFFER;
        if (Has(s, State::IndirectArgument))
            out |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        if (Has(s, State::ShaderReadFragment))
            out |= queue == QueueType::Graphics ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        if (Has(s, State::ShaderReadOther))
            out |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        if (Has(s, State::DepthRead))
            out |= D3D12_RESOURCE_STATE_DEPTH_READ;
        if (Has(s, State::ResolveSrc))
            out |= D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        return out;
    }

    inline D3D12_COMPARISON_FUNC ToD3D(CompareOp op) {
        switch (op) {
        case CompareOp::Never: return D3D12_COMPARISON_FUNC_NEVER;
        case CompareOp::Less: return D3D12_COMPARISON_FUNC_LESS;
        case CompareOp::Equal: return D3D12_COMPARISON_FUNC_EQUAL;
        case CompareOp::LessEqual: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case CompareOp::Greater: return D3D12_COMPARISON_FUNC_GREATER;
        case CompareOp::NotEqual: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
        case CompareOp::GreaterEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        default: return D3D12_COMPARISON_FUNC_ALWAYS;
        }
    }

    inline D3D12_STENCIL_OP ToD3D(StencilOp op) {
        switch (op) {
        case StencilOp::Keep: return D3D12_STENCIL_OP_KEEP;
        case StencilOp::Zero: return D3D12_STENCIL_OP_ZERO;
        case StencilOp::Replace: return D3D12_STENCIL_OP_REPLACE;
        case StencilOp::IncrementClamp: return D3D12_STENCIL_OP_INCR_SAT;
        case StencilOp::DecrementClamp: return D3D12_STENCIL_OP_DECR_SAT;
        case StencilOp::Invert: return D3D12_STENCIL_OP_INVERT;
        case StencilOp::IncrementWrap: return D3D12_STENCIL_OP_INCR;
        default: return D3D12_STENCIL_OP_DECR;
        }
    }

    inline D3D12_BLEND ToD3D(BlendFactor f, bool alpha) {
        switch (f) {
        case BlendFactor::Zero: return D3D12_BLEND_ZERO;
        case BlendFactor::One: return D3D12_BLEND_ONE;
        case BlendFactor::SrcColor: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
        case BlendFactor::InvSrcColor: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
        case BlendFactor::SrcAlpha: return D3D12_BLEND_SRC_ALPHA;
        case BlendFactor::InvSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
        case BlendFactor::DstColor: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
        case BlendFactor::InvDstColor: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
        case BlendFactor::DstAlpha: return D3D12_BLEND_DEST_ALPHA;
        case BlendFactor::InvDstAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
        default: return D3D12_BLEND_SRC_ALPHA_SAT;
        }
    }

    inline D3D12_BLEND_OP ToD3D(BlendOp op) {
        switch (op) {
        case BlendOp::Add: return D3D12_BLEND_OP_ADD;
        case BlendOp::Subtract: return D3D12_BLEND_OP_SUBTRACT;
        case BlendOp::ReverseSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
        case BlendOp::Min: return D3D12_BLEND_OP_MIN;
        default: return D3D12_BLEND_OP_MAX;
        }
    }

    inline D3D12_TEXTURE_ADDRESS_MODE ToD3D(AddressMode m) {
        switch (m) {
        case AddressMode::Repeat: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        case AddressMode::Mirror: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case AddressMode::Clamp: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        default: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        }
    }

    inline D3D12_FILTER ToD3D(const SamplerDesc& s) {
        if (s.maxAnisotropy > 1.0f)
            return s.compare ? D3D12_FILTER_COMPARISON_ANISOTROPIC : D3D12_FILTER_ANISOTROPIC;
        UINT bits = (s.min == Filter::Linear ? 0x10 : 0) | (s.mag == Filter::Linear ? 0x04 : 0) | (s.mip == Filter::Linear ? 0x01 : 0);
        if (s.compare)
            bits |= 0x80;
        return static_cast<D3D12_FILTER>(bits);
    }

    inline D3D12_PRIMITIVE_TOPOLOGY ToD3D(Topology t) {
        switch (t) {
        case Topology::TriangleList: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        case Topology::TriangleStrip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        case Topology::LineList: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        default: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        }
    }

    inline D3D12_PRIMITIVE_TOPOLOGY_TYPE ToD3DType(Topology t) {
        switch (t) {
        case Topology::TriangleList:
        case Topology::TriangleStrip: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        case Topology::LineList: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        default: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        }
    }

    inline UINT SubresourceIndex(uint32_t mip, uint32_t layer, uint32_t plane, uint32_t mips, uint32_t layers) {
        return mip + layer * mips + plane * mips * layers;
    }
}
