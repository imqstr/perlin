#include "rhi/webgpu/WebGpuInternal.hpp"

namespace rhi::webgpu {
    WGPUTextureFormat ToTextureFormat(Format format) {
        switch (format) {
        case Format::R8Unorm: return WGPUTextureFormat_R8Unorm;
        case Format::RG8Unorm: return WGPUTextureFormat_RG8Unorm;
        case Format::RGBA8Unorm: return WGPUTextureFormat_RGBA8Unorm;
        case Format::RGBA8Srgb: return WGPUTextureFormat_RGBA8UnormSrgb;
        case Format::BGRA8Unorm: return WGPUTextureFormat_BGRA8Unorm;
        case Format::BGRA8Srgb: return WGPUTextureFormat_BGRA8UnormSrgb;
        case Format::R8Uint: return WGPUTextureFormat_R8Uint;
        case Format::R16Uint: return WGPUTextureFormat_R16Uint;
        case Format::R32Uint: return WGPUTextureFormat_R32Uint;
        case Format::RG32Uint: return WGPUTextureFormat_RG32Uint;
        case Format::RGBA32Uint: return WGPUTextureFormat_RGBA32Uint;
        case Format::R16Float: return WGPUTextureFormat_R16Float;
        case Format::RG16Float: return WGPUTextureFormat_RG16Float;
        case Format::RGBA16Float: return WGPUTextureFormat_RGBA16Float;
        case Format::R32Float: return WGPUTextureFormat_R32Float;
        case Format::RG32Float: return WGPUTextureFormat_RG32Float;
        case Format::RGBA32Float: return WGPUTextureFormat_RGBA32Float;
        case Format::RGB10A2Unorm: return WGPUTextureFormat_RGB10A2Unorm;
        case Format::R11G11B10Float: return WGPUTextureFormat_RG11B10Ufloat;
        case Format::D16Unorm: return WGPUTextureFormat_Depth16Unorm;
        case Format::D32Float: return WGPUTextureFormat_Depth32Float;
        case Format::D24UnormS8Uint: return WGPUTextureFormat_Depth24PlusStencil8;
        case Format::D32FloatS8Uint: return WGPUTextureFormat_Depth32FloatStencil8;
        case Format::BC1Unorm: return WGPUTextureFormat_BC1RGBAUnorm;
        case Format::BC3Unorm: return WGPUTextureFormat_BC3RGBAUnorm;
        case Format::BC7Unorm: return WGPUTextureFormat_BC7RGBAUnorm;
        // 16 bit normalised, 96 bit float and the integer-only odd sizes have no texture format here
        default: return WGPUTextureFormat_Undefined;
        }
    }

    Format FromTextureFormat(WGPUTextureFormat format) {
        switch (format) {
        case WGPUTextureFormat_BGRA8Unorm: return Format::BGRA8Unorm;
        case WGPUTextureFormat_BGRA8UnormSrgb: return Format::BGRA8Srgb;
        case WGPUTextureFormat_RGBA8Unorm: return Format::RGBA8Unorm;
        case WGPUTextureFormat_RGBA8UnormSrgb: return Format::RGBA8Srgb;
        case WGPUTextureFormat_RGBA16Float: return Format::RGBA16Float;
        case WGPUTextureFormat_RGB10A2Unorm: return Format::RGB10A2Unorm;
        default: return Format::Unknown;
        }
    }

    WGPUVertexFormat ToVertexFormat(Format format) {
        switch (format) {
        case Format::R8Unorm: return WGPUVertexFormat_Unorm8;
        case Format::RG8Unorm: return WGPUVertexFormat_Unorm8x2;
        case Format::RGBA8Unorm: return WGPUVertexFormat_Unorm8x4;
        case Format::BGRA8Unorm: return WGPUVertexFormat_Unorm8x4BGRA;
        case Format::R8Uint: return WGPUVertexFormat_Uint8;
        case Format::R16Uint: return WGPUVertexFormat_Uint16;
        case Format::R32Uint: return WGPUVertexFormat_Uint32;
        case Format::RG32Uint: return WGPUVertexFormat_Uint32x2;
        case Format::RGBA32Uint: return WGPUVertexFormat_Uint32x4;
        case Format::R16Float: return WGPUVertexFormat_Float16;
        case Format::RG16Float: return WGPUVertexFormat_Float16x2;
        case Format::RGBA16Float: return WGPUVertexFormat_Float16x4;
        case Format::R32Float: return WGPUVertexFormat_Float32;
        case Format::RG32Float: return WGPUVertexFormat_Float32x2;
        case Format::RGB32Float: return WGPUVertexFormat_Float32x3;
        case Format::RGBA32Float: return WGPUVertexFormat_Float32x4;
        case Format::RGB10A2Unorm: return WGPUVertexFormat_Unorm10_10_10_2;
        case Format::RG16Unorm: return WGPUVertexFormat_Unorm16x2;
        case Format::RGBA16Unorm: return WGPUVertexFormat_Unorm16x4;
        default: return static_cast<WGPUVertexFormat>(0);
        }
    }

    WGPUBlendFactor ToBlendFactor(BlendFactor f) {
        switch (f) {
        case BlendFactor::Zero: return WGPUBlendFactor_Zero;
        case BlendFactor::One: return WGPUBlendFactor_One;
        case BlendFactor::SrcColor: return WGPUBlendFactor_Src;
        case BlendFactor::InvSrcColor: return WGPUBlendFactor_OneMinusSrc;
        case BlendFactor::SrcAlpha: return WGPUBlendFactor_SrcAlpha;
        case BlendFactor::InvSrcAlpha: return WGPUBlendFactor_OneMinusSrcAlpha;
        case BlendFactor::DstColor: return WGPUBlendFactor_Dst;
        case BlendFactor::InvDstColor: return WGPUBlendFactor_OneMinusDst;
        case BlendFactor::DstAlpha: return WGPUBlendFactor_DstAlpha;
        case BlendFactor::InvDstAlpha: return WGPUBlendFactor_OneMinusDstAlpha;
        case BlendFactor::SrcAlphaSaturate: return WGPUBlendFactor_SrcAlphaSaturated;
        }
        return WGPUBlendFactor_One;
    }

    WGPUBlendOperation ToBlendOp(BlendOp op) {
        switch (op) {
        case BlendOp::Add: return WGPUBlendOperation_Add;
        case BlendOp::Subtract: return WGPUBlendOperation_Subtract;
        case BlendOp::ReverseSubtract: return WGPUBlendOperation_ReverseSubtract;
        case BlendOp::Min: return WGPUBlendOperation_Min;
        case BlendOp::Max: return WGPUBlendOperation_Max;
        }
        return WGPUBlendOperation_Add;
    }

    WGPUCompareFunction ToCompare(CompareOp op) {
        switch (op) {
        case CompareOp::Never: return WGPUCompareFunction_Never;
        case CompareOp::Less: return WGPUCompareFunction_Less;
        case CompareOp::Equal: return WGPUCompareFunction_Equal;
        case CompareOp::LessEqual: return WGPUCompareFunction_LessEqual;
        case CompareOp::Greater: return WGPUCompareFunction_Greater;
        case CompareOp::NotEqual: return WGPUCompareFunction_NotEqual;
        case CompareOp::GreaterEqual: return WGPUCompareFunction_GreaterEqual;
        case CompareOp::Always: return WGPUCompareFunction_Always;
        }
        return WGPUCompareFunction_Always;
    }

    WGPUStencilOperation ToStencilOp(StencilOp op) {
        switch (op) {
        case StencilOp::Keep: return WGPUStencilOperation_Keep;
        case StencilOp::Zero: return WGPUStencilOperation_Zero;
        case StencilOp::Replace: return WGPUStencilOperation_Replace;
        case StencilOp::IncrementClamp: return WGPUStencilOperation_IncrementClamp;
        case StencilOp::DecrementClamp: return WGPUStencilOperation_DecrementClamp;
        case StencilOp::Invert: return WGPUStencilOperation_Invert;
        case StencilOp::IncrementWrap: return WGPUStencilOperation_IncrementWrap;
        case StencilOp::DecrementWrap: return WGPUStencilOperation_DecrementWrap;
        }
        return WGPUStencilOperation_Keep;
    }

    WGPUAddressMode ToAddress(AddressMode mode) {
        switch (mode) {
        case AddressMode::Repeat: return WGPUAddressMode_Repeat;
        case AddressMode::Mirror: return WGPUAddressMode_MirrorRepeat;
        // no border colour in webgpu
        default: return WGPUAddressMode_ClampToEdge;
        }
    }

    WGPUFilterMode ToFilter(Filter f) { return f == Filter::Nearest ? WGPUFilterMode_Nearest : WGPUFilterMode_Linear; }
    WGPUMipmapFilterMode ToMipFilter(Filter f) { return f == Filter::Nearest ? WGPUMipmapFilterMode_Nearest : WGPUMipmapFilterMode_Linear; }

    WGPUTextureViewDimension ToViewDimension(TextureDim dim) {
        switch (dim) {
        case TextureDim::Tex2D: return WGPUTextureViewDimension_2D;
        case TextureDim::Tex2DArray: return WGPUTextureViewDimension_2DArray;
        case TextureDim::Cube: return WGPUTextureViewDimension_Cube;
        case TextureDim::Tex3D: return WGPUTextureViewDimension_3D;
        }
        return WGPUTextureViewDimension_2D;
    }
}
