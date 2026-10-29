#include "rhi/metal/MetalInternal.hpp"

namespace rhi::metal {
    bool gDepth24Stencil8 = false;

    MTLPixelFormat ToPixelFormat(Format f) {
        switch (f) {
        case Format::R8Unorm: return MTLPixelFormatR8Unorm;
        case Format::RG8Unorm: return MTLPixelFormatRG8Unorm;
        case Format::RGBA8Unorm: return MTLPixelFormatRGBA8Unorm;
        case Format::RGBA8Srgb: return MTLPixelFormatRGBA8Unorm_sRGB;
        case Format::BGRA8Unorm: return MTLPixelFormatBGRA8Unorm;
        case Format::BGRA8Srgb: return MTLPixelFormatBGRA8Unorm_sRGB;
        case Format::R8Uint: return MTLPixelFormatR8Uint;
        case Format::R16Uint: return MTLPixelFormatR16Uint;
        case Format::R32Uint: return MTLPixelFormatR32Uint;
        case Format::RG32Uint: return MTLPixelFormatRG32Uint;
        case Format::RGBA32Uint: return MTLPixelFormatRGBA32Uint;
        case Format::R16Float: return MTLPixelFormatR16Float;
        case Format::RG16Float: return MTLPixelFormatRG16Float;
        case Format::RGBA16Float: return MTLPixelFormatRGBA16Float;
        case Format::R32Float: return MTLPixelFormatR32Float;
        case Format::RG32Float: return MTLPixelFormatRG32Float;
        case Format::RGBA32Float: return MTLPixelFormatRGBA32Float;
        case Format::RGB10A2Unorm: return MTLPixelFormatRGB10A2Unorm;
        case Format::R11G11B10Float: return MTLPixelFormatRG11B10Float;
        case Format::RG16Unorm: return MTLPixelFormatRG16Unorm;
        case Format::RGBA16Unorm: return MTLPixelFormatRGBA16Unorm;
        case Format::D16Unorm: return MTLPixelFormatDepth16Unorm;
        case Format::D32Float: return MTLPixelFormatDepth32Float;
        // the macs without a 24 bit depth format get 32 bit depth; nothing depends on the exact precision
        case Format::D24UnormS8Uint:
#if TARGET_OS_OSX
            if (gDepth24Stencil8)
                return MTLPixelFormatDepth24Unorm_Stencil8;
#endif
            return MTLPixelFormatDepth32Float_Stencil8;
        case Format::D32FloatS8Uint: return MTLPixelFormatDepth32Float_Stencil8;
#if TARGET_OS_OSX || (defined(__IPHONE_16_4) && __IPHONE_OS_VERSION_MAX_ALLOWED >= 160400)
        case Format::BC1Unorm: return MTLPixelFormatBC1_RGBA;
        case Format::BC3Unorm: return MTLPixelFormatBC3_RGBA;
        case Format::BC7Unorm: return MTLPixelFormatBC7_RGBAUnorm;
#endif
        default: return MTLPixelFormatInvalid;
        }
    }

    // RGB32Float has no texture format but is a fine vertex format
    MTLVertexFormat ToVertexFormat(Format f) {
        switch (f) {
        case Format::R8Unorm: return MTLVertexFormatUCharNormalized;
        case Format::RG8Unorm: return MTLVertexFormatUChar2Normalized;
        case Format::RGBA8Unorm: return MTLVertexFormatUChar4Normalized;
        case Format::BGRA8Unorm: return MTLVertexFormatUChar4Normalized_BGRA;
        case Format::R8Uint: return MTLVertexFormatUChar;
        case Format::R16Uint: return MTLVertexFormatUShort;
        case Format::R32Uint: return MTLVertexFormatUInt;
        case Format::RG32Uint: return MTLVertexFormatUInt2;
        case Format::RGBA32Uint: return MTLVertexFormatUInt4;
        case Format::R16Float: return MTLVertexFormatHalf;
        case Format::RG16Float: return MTLVertexFormatHalf2;
        case Format::RGBA16Float: return MTLVertexFormatHalf4;
        case Format::R32Float: return MTLVertexFormatFloat;
        case Format::RG32Float: return MTLVertexFormatFloat2;
        case Format::RGB32Float: return MTLVertexFormatFloat3;
        case Format::RGBA32Float: return MTLVertexFormatFloat4;
        case Format::RGB10A2Unorm: return MTLVertexFormatUInt1010102Normalized;
        case Format::RG16Unorm: return MTLVertexFormatUShort2Normalized;
        case Format::RGBA16Unorm: return MTLVertexFormatUShort4Normalized;
        default: return MTLVertexFormatInvalid;
        }
    }

    MTLCompareFunction ToCompare(CompareOp op) {
        switch (op) {
        case CompareOp::Never: return MTLCompareFunctionNever;
        case CompareOp::Less: return MTLCompareFunctionLess;
        case CompareOp::Equal: return MTLCompareFunctionEqual;
        case CompareOp::LessEqual: return MTLCompareFunctionLessEqual;
        case CompareOp::Greater: return MTLCompareFunctionGreater;
        case CompareOp::NotEqual: return MTLCompareFunctionNotEqual;
        case CompareOp::GreaterEqual: return MTLCompareFunctionGreaterEqual;
        default: return MTLCompareFunctionAlways;
        }
    }

    MTLStencilOperation ToStencilOp(StencilOp op) {
        switch (op) {
        case StencilOp::Zero: return MTLStencilOperationZero;
        case StencilOp::Replace: return MTLStencilOperationReplace;
        case StencilOp::IncrementClamp: return MTLStencilOperationIncrementClamp;
        case StencilOp::DecrementClamp: return MTLStencilOperationDecrementClamp;
        case StencilOp::Invert: return MTLStencilOperationInvert;
        case StencilOp::IncrementWrap: return MTLStencilOperationIncrementWrap;
        case StencilOp::DecrementWrap: return MTLStencilOperationDecrementWrap;
        default: return MTLStencilOperationKeep;
        }
    }

    MTLBlendFactor ToBlendFactor(BlendFactor f) {
        switch (f) {
        case BlendFactor::Zero: return MTLBlendFactorZero;
        case BlendFactor::One: return MTLBlendFactorOne;
        case BlendFactor::SrcColor: return MTLBlendFactorSourceColor;
        case BlendFactor::InvSrcColor: return MTLBlendFactorOneMinusSourceColor;
        case BlendFactor::SrcAlpha: return MTLBlendFactorSourceAlpha;
        case BlendFactor::InvSrcAlpha: return MTLBlendFactorOneMinusSourceAlpha;
        case BlendFactor::DstColor: return MTLBlendFactorDestinationColor;
        case BlendFactor::InvDstColor: return MTLBlendFactorOneMinusDestinationColor;
        case BlendFactor::DstAlpha: return MTLBlendFactorDestinationAlpha;
        case BlendFactor::InvDstAlpha: return MTLBlendFactorOneMinusDestinationAlpha;
        default: return MTLBlendFactorSourceAlphaSaturated;
        }
    }

    MTLBlendOperation ToBlendOp(BlendOp op) {
        switch (op) {
        case BlendOp::Subtract: return MTLBlendOperationSubtract;
        case BlendOp::ReverseSubtract: return MTLBlendOperationReverseSubtract;
        case BlendOp::Min: return MTLBlendOperationMin;
        case BlendOp::Max: return MTLBlendOperationMax;
        default: return MTLBlendOperationAdd;
        }
    }

    MTLSamplerAddressMode ToAddress(AddressMode m) {
        switch (m) {
        case AddressMode::Repeat: return MTLSamplerAddressModeRepeat;
        case AddressMode::Mirror: return MTLSamplerAddressModeMirrorRepeat;
        case AddressMode::Border:
#if TARGET_OS_OSX
            return MTLSamplerAddressModeClampToBorderColor;
#else
            return MTLSamplerAddressModeClampToEdge;
#endif
        default: return MTLSamplerAddressModeClampToEdge;
        }
    }

    MTLSamplerMinMagFilter ToMinMag(Filter f) { return f == Filter::Nearest ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear; }
    MTLSamplerMipFilter ToMip(Filter f) { return f == Filter::Nearest ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear; }

    MTLPrimitiveType ToPrimitive(Topology t) {
        switch (t) {
        case Topology::TriangleStrip: return MTLPrimitiveTypeTriangleStrip;
        case Topology::LineList: return MTLPrimitiveTypeLine;
        case Topology::PointList: return MTLPrimitiveTypePoint;
        default: return MTLPrimitiveTypeTriangle;
        }
    }

    MTLTextureType ToTextureType(TextureDim dim, bool multisample, bool array) {
        switch (dim) {
        case TextureDim::Tex2D:
            if (multisample)
                return array ? MTLTextureType2DMultisampleArray : MTLTextureType2DMultisample;
            return array ? MTLTextureType2DArray : MTLTextureType2D;
        case TextureDim::Tex2DArray: return multisample ? MTLTextureType2DMultisampleArray : MTLTextureType2DArray;
        case TextureDim::Cube: return MTLTextureTypeCube;
        default: return MTLTextureType3D;
        }
    }
}
