#pragma once
#include "rhi/Types.hpp"

#include <d3d11_1.h>
#include <dxgiformat.h>

namespace rhi::d3d11 {
    struct FormatMap {
        // resource format: typeless where the same bits are viewed as several formats (srgb, depth + sampled)
        DXGI_FORMAT resource = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT dsv = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT stencilView = DXGI_FORMAT_UNKNOWN;
    };

    inline FormatMap MapFormat(Format f) {
        FormatMap m;
        const auto simple = [&](DXGI_FORMAT x) { m.resource = m.view = x; };
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
            break;
        case Format::D32FloatS8Uint:
            m.resource = DXGI_FORMAT_R32G8X24_TYPELESS;
            m.view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            m.stencilView = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
            m.dsv = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            break;
        case Format::BC1Unorm: simple(DXGI_FORMAT_BC1_UNORM); break;
        case Format::BC3Unorm: simple(DXGI_FORMAT_BC3_UNORM); break;
        case Format::BC7Unorm: simple(DXGI_FORMAT_BC7_UNORM); break;
        default: break;
        }
        return m;
    }

    inline D3D11_COMPARISON_FUNC ToD3D(CompareOp op) {
        switch (op) {
        case CompareOp::Never: return D3D11_COMPARISON_NEVER;
        case CompareOp::Less: return D3D11_COMPARISON_LESS;
        case CompareOp::Equal: return D3D11_COMPARISON_EQUAL;
        case CompareOp::LessEqual: return D3D11_COMPARISON_LESS_EQUAL;
        case CompareOp::Greater: return D3D11_COMPARISON_GREATER;
        case CompareOp::NotEqual: return D3D11_COMPARISON_NOT_EQUAL;
        case CompareOp::GreaterEqual: return D3D11_COMPARISON_GREATER_EQUAL;
        default: return D3D11_COMPARISON_ALWAYS;
        }
    }

    inline D3D11_STENCIL_OP ToD3D(StencilOp op) {
        switch (op) {
        case StencilOp::Keep: return D3D11_STENCIL_OP_KEEP;
        case StencilOp::Zero: return D3D11_STENCIL_OP_ZERO;
        case StencilOp::Replace: return D3D11_STENCIL_OP_REPLACE;
        case StencilOp::IncrementClamp: return D3D11_STENCIL_OP_INCR_SAT;
        case StencilOp::DecrementClamp: return D3D11_STENCIL_OP_DECR_SAT;
        case StencilOp::Invert: return D3D11_STENCIL_OP_INVERT;
        case StencilOp::IncrementWrap: return D3D11_STENCIL_OP_INCR;
        default: return D3D11_STENCIL_OP_DECR;
        }
    }

    // alpha factors may not read colour channels
    inline D3D11_BLEND ToD3D(BlendFactor f, bool alpha) {
        switch (f) {
        case BlendFactor::Zero: return D3D11_BLEND_ZERO;
        case BlendFactor::One: return D3D11_BLEND_ONE;
        case BlendFactor::SrcColor: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
        case BlendFactor::InvSrcColor: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
        case BlendFactor::SrcAlpha: return D3D11_BLEND_SRC_ALPHA;
        case BlendFactor::InvSrcAlpha: return D3D11_BLEND_INV_SRC_ALPHA;
        case BlendFactor::DstColor: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
        case BlendFactor::InvDstColor: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
        case BlendFactor::DstAlpha: return D3D11_BLEND_DEST_ALPHA;
        case BlendFactor::InvDstAlpha: return D3D11_BLEND_INV_DEST_ALPHA;
        default: return D3D11_BLEND_SRC_ALPHA_SAT;
        }
    }

    inline D3D11_BLEND_OP ToD3D(BlendOp op) {
        switch (op) {
        case BlendOp::Add: return D3D11_BLEND_OP_ADD;
        case BlendOp::Subtract: return D3D11_BLEND_OP_SUBTRACT;
        case BlendOp::ReverseSubtract: return D3D11_BLEND_OP_REV_SUBTRACT;
        case BlendOp::Min: return D3D11_BLEND_OP_MIN;
        default: return D3D11_BLEND_OP_MAX;
        }
    }

    inline D3D11_TEXTURE_ADDRESS_MODE ToD3D(AddressMode m) {
        switch (m) {
        case AddressMode::Repeat: return D3D11_TEXTURE_ADDRESS_WRAP;
        case AddressMode::Mirror: return D3D11_TEXTURE_ADDRESS_MIRROR;
        case AddressMode::Clamp: return D3D11_TEXTURE_ADDRESS_CLAMP;
        default: return D3D11_TEXTURE_ADDRESS_BORDER;
        }
    }

    inline D3D11_FILTER ToD3D(const SamplerDesc& s) {
        if (s.maxAnisotropy > 1.0f)
            return s.compare ? D3D11_FILTER_COMPARISON_ANISOTROPIC : D3D11_FILTER_ANISOTROPIC;
        UINT bits = (s.min == Filter::Linear ? 0x10 : 0) | (s.mag == Filter::Linear ? 0x04 : 0) | (s.mip == Filter::Linear ? 0x01 : 0);
        if (s.compare)
            bits |= 0x80;
        return static_cast<D3D11_FILTER>(bits);
    }

    inline D3D11_PRIMITIVE_TOPOLOGY ToD3D(Topology t) {
        switch (t) {
        case Topology::TriangleList: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        case Topology::TriangleStrip: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        case Topology::LineList: return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        default: return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
        }
    }
}
