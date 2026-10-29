#pragma once
#include "rhi/Types.hpp"

#include <volk.h>

namespace rhi::vk {
    inline VkFormat BaseFormat(Format f) {
        switch (f) {
        case Format::R8Unorm: return VK_FORMAT_R8_UNORM;
        case Format::RG8Unorm: return VK_FORMAT_R8G8_UNORM;
        case Format::RGBA8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
        case Format::RGBA8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
        case Format::BGRA8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
        case Format::BGRA8Srgb: return VK_FORMAT_B8G8R8A8_SRGB;
        case Format::R8Uint: return VK_FORMAT_R8_UINT;
        case Format::R16Uint: return VK_FORMAT_R16_UINT;
        case Format::R32Uint: return VK_FORMAT_R32_UINT;
        case Format::RG32Uint: return VK_FORMAT_R32G32_UINT;
        case Format::RGBA32Uint: return VK_FORMAT_R32G32B32A32_UINT;
        case Format::R16Float: return VK_FORMAT_R16_SFLOAT;
        case Format::RG16Float: return VK_FORMAT_R16G16_SFLOAT;
        case Format::RGBA16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case Format::R32Float: return VK_FORMAT_R32_SFLOAT;
        case Format::RG32Float: return VK_FORMAT_R32G32_SFLOAT;
        case Format::RGB32Float: return VK_FORMAT_R32G32B32_SFLOAT;
        case Format::RGBA32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case Format::RGB10A2Unorm: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case Format::R11G11B10Float: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case Format::RG16Unorm: return VK_FORMAT_R16G16_UNORM;
        case Format::RGBA16Unorm: return VK_FORMAT_R16G16B16A16_UNORM;
        case Format::D16Unorm: return VK_FORMAT_D16_UNORM;
        case Format::D32Float: return VK_FORMAT_D32_SFLOAT;
        case Format::D24UnormS8Uint: return VK_FORMAT_D24_UNORM_S8_UINT;
        case Format::D32FloatS8Uint: return VK_FORMAT_D32_SFLOAT_S8_UINT;
        case Format::BC1Unorm: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case Format::BC3Unorm: return VK_FORMAT_BC3_UNORM_BLOCK;
        case Format::BC7Unorm: return VK_FORMAT_BC7_UNORM_BLOCK;
        default: return VK_FORMAT_UNDEFINED;
        }
    }

    inline VkImageAspectFlags AspectOf(Format f) {
        const FormatInfo& info = GetFormatInfo(f);
        if (info.depth)
            return info.stencil ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT;
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }

    struct StateUse {
        VkPipelineStageFlags2 stages = 0;
        VkAccessFlags2 access = 0;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    constexpr VkPipelineStageFlags2 kGraphicsOnlyStages =
        VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    constexpr VkAccessFlags2 kGraphicsOnlyAccess =
        VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    // stage/access/layout for a rhi state. queue restricts the masks to what that queue family can name
    inline StateUse UseOf(State s, QueueType queue, bool depthFormat) {
        StateUse use;
        if (s == State::Undefined)
            return use;
        VkImageLayout layouts[16];
        int layoutCount = 0;
        auto add = [&](VkPipelineStageFlags2 st, VkAccessFlags2 ac, VkImageLayout lay) {
            use.stages |= st;
            use.access |= ac;
            layouts[layoutCount++] = lay;
        };
        const VkImageLayout shaderRead = depthFormat ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        constexpr auto kShaders = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        if (Has(s, State::CopySrc))
            add(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        if (Has(s, State::CopyDst))
            add(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (Has(s, State::VertexBuffer))
            add(VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT, VK_IMAGE_LAYOUT_GENERAL);
        if (Has(s, State::IndexBuffer))
            add(VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT, VK_IMAGE_LAYOUT_GENERAL);
        if (Has(s, State::UniformBuffer))
            add(kShaders, VK_ACCESS_2_UNIFORM_READ_BIT, VK_IMAGE_LAYOUT_GENERAL);
        if (Has(s, State::IndirectArgument))
            add(VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, VK_IMAGE_LAYOUT_GENERAL);
        if (Has(s, State::ShaderReadFragment))
            add(VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, shaderRead);
        if (Has(s, State::ShaderReadOther))
            add(VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, shaderRead);
        if (Has(s, State::ShaderWrite))
            add(kShaders, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL);
        if (Has(s, State::ColorTarget))
            add(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        if (Has(s, State::DepthWrite))
            add(VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        if (Has(s, State::DepthRead))
            add(VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        if (Has(s, State::Present))
            add(0, 0, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        if (Has(s, State::ResolveSrc))
            add(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        if (Has(s, State::ResolveDst))
            add(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        use.layout = layouts[0];
        for (int i = 1; i < layoutCount; i++)
            if (layouts[i] != use.layout) {
                use.layout = VK_IMAGE_LAYOUT_GENERAL;
                break;
            }
        // sampled depth that is also tested stays in the read-only depth layout
        if (Has(s, State::DepthRead) && (Has(s, State::ShaderReadFragment) || Has(s, State::ShaderReadOther)))
            use.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        if (queue == QueueType::Compute) {
            if (use.stages & kGraphicsOnlyStages)
                use.stages = (use.stages & ~kGraphicsOnlyStages) | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            use.access &= ~kGraphicsOnlyAccess;
        } else if (queue == QueueType::Copy) {
            use.stages = use.stages ? VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT : 0;
            use.access &= VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        }
        return use;
    }

    inline VkCompareOp ToVk(CompareOp op) {
        switch (op) {
        case CompareOp::Never: return VK_COMPARE_OP_NEVER;
        case CompareOp::Less: return VK_COMPARE_OP_LESS;
        case CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
        case CompareOp::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case CompareOp::Greater: return VK_COMPARE_OP_GREATER;
        case CompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
        case CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        default: return VK_COMPARE_OP_ALWAYS;
        }
    }

    inline VkStencilOp ToVk(StencilOp op) {
        switch (op) {
        case StencilOp::Keep: return VK_STENCIL_OP_KEEP;
        case StencilOp::Zero: return VK_STENCIL_OP_ZERO;
        case StencilOp::Replace: return VK_STENCIL_OP_REPLACE;
        case StencilOp::IncrementClamp: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case StencilOp::DecrementClamp: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case StencilOp::Invert: return VK_STENCIL_OP_INVERT;
        case StencilOp::IncrementWrap: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        }
    }

    inline VkBlendFactor ToVk(BlendFactor f) {
        switch (f) {
        case BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
        case BlendFactor::One: return VK_BLEND_FACTOR_ONE;
        case BlendFactor::SrcColor: return VK_BLEND_FACTOR_SRC_COLOR;
        case BlendFactor::InvSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case BlendFactor::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
        case BlendFactor::InvSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DstColor: return VK_BLEND_FACTOR_DST_COLOR;
        case BlendFactor::InvDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case BlendFactor::DstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
        case BlendFactor::InvDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        default: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        }
    }

    inline VkBlendOp ToVk(BlendOp op) {
        switch (op) {
        case BlendOp::Add: return VK_BLEND_OP_ADD;
        case BlendOp::Subtract: return VK_BLEND_OP_SUBTRACT;
        case BlendOp::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case BlendOp::Min: return VK_BLEND_OP_MIN;
        default: return VK_BLEND_OP_MAX;
        }
    }

    inline VkSamplerAddressMode ToVk(AddressMode m) {
        switch (m) {
        case AddressMode::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case AddressMode::Mirror: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case AddressMode::Clamp: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        }
    }

    inline VkFilter ToVk(Filter f) { return f == Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR; }

    inline VkShaderStageFlags ToVk(ShaderStage s) {
        VkShaderStageFlags out = 0;
        if (s & ShaderStage::Vertex)
            out |= VK_SHADER_STAGE_VERTEX_BIT;
        if (s & ShaderStage::Fragment)
            out |= VK_SHADER_STAGE_FRAGMENT_BIT;
        if (s & ShaderStage::Compute)
            out |= VK_SHADER_STAGE_COMPUTE_BIT;
        return out;
    }

    inline VkImageViewType ViewTypeOf(TextureDim dim) {
        switch (dim) {
        case TextureDim::Tex2D: return VK_IMAGE_VIEW_TYPE_2D;
        case TextureDim::Tex2DArray: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case TextureDim::Cube: return VK_IMAGE_VIEW_TYPE_CUBE;
        default: return VK_IMAGE_VIEW_TYPE_3D;
        }
    }

    inline VkSampleCountFlagBits SamplesOf(uint32_t n) {
        switch (n) {
        case 2: return VK_SAMPLE_COUNT_2_BIT;
        case 4: return VK_SAMPLE_COUNT_4_BIT;
        case 8: return VK_SAMPLE_COUNT_8_BIT;
        case 16: return VK_SAMPLE_COUNT_16_BIT;
        default: return VK_SAMPLE_COUNT_1_BIT;
        }
    }
}
