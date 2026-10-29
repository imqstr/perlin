#include "rhi/Device.hpp"
#include "rhi/Backends.hpp"

#include <algorithm>

namespace rhi {
    Api ApiFromId(std::string_view id) {
        for (Api api : {Api::Vulkan, Api::D3D12, Api::D3D11, Api::Metal, Api::WebGPU})
            if (id == ApiId(api))
                return api;
        // names players type into settings files
        if (id == "dx12" || id == "directx12")
            return Api::D3D12;
        if (id == "dx11" || id == "directx11")
            return Api::D3D11;
        if (id == "vk")
            return Api::Vulkan;
        if (id == "mtl")
            return Api::Metal;
        return Api::None;
    }

    const char* VendorName(uint32_t vendorId) {
        switch (vendorId) {
        case vendor_id::kNvidia: return "NVIDIA";
        case vendor_id::kAmd: return "AMD";
        case vendor_id::kIntel: return "Intel";
        case vendor_id::kApple: return "Apple";
        case vendor_id::kQualcomm: return "Qualcomm";
        case vendor_id::kArm: return "ARM";
        case vendor_id::kMicrosoft: return "Microsoft";
        default: return "Unknown";
        }
    }

    const FormatInfo& GetFormatInfo(Format format) {
        //                                 bytes bw bh ch  depth stencil srgb  comp  float
        static const FormatInfo kTable[] = {
            /* Unknown        */ {0, 1, 1, 0, false, false, false, false, false},
            /* R8Unorm        */ {1, 1, 1, 1, false, false, false, false, false},
            /* RG8Unorm       */ {2, 1, 1, 2, false, false, false, false, false},
            /* RGBA8Unorm     */ {4, 1, 1, 4, false, false, false, false, false},
            /* RGBA8Srgb      */ {4, 1, 1, 4, false, false, true, false, false},
            /* BGRA8Unorm     */ {4, 1, 1, 4, false, false, false, false, false},
            /* BGRA8Srgb      */ {4, 1, 1, 4, false, false, true, false, false},
            /* R8Uint         */ {1, 1, 1, 1, false, false, false, false, false},
            /* R16Uint        */ {2, 1, 1, 1, false, false, false, false, false},
            /* R32Uint        */ {4, 1, 1, 1, false, false, false, false, false},
            /* RG32Uint       */ {8, 1, 1, 2, false, false, false, false, false},
            /* RGBA32Uint     */ {16, 1, 1, 4, false, false, false, false, false},
            /* R16Float       */ {2, 1, 1, 1, false, false, false, false, true},
            /* RG16Float      */ {4, 1, 1, 2, false, false, false, false, true},
            /* RGBA16Float    */ {8, 1, 1, 4, false, false, false, false, true},
            /* R32Float       */ {4, 1, 1, 1, false, false, false, false, true},
            /* RG32Float      */ {8, 1, 1, 2, false, false, false, false, true},
            /* RGB32Float     */ {12, 1, 1, 3, false, false, false, false, true},
            /* RGBA32Float    */ {16, 1, 1, 4, false, false, false, false, true},
            /* RGB10A2Unorm   */ {4, 1, 1, 4, false, false, false, false, false},
            /* R11G11B10Float */ {4, 1, 1, 3, false, false, false, false, true},
            /* RG16Unorm      */ {4, 1, 1, 2, false, false, false, false, false},
            /* RGBA16Unorm    */ {8, 1, 1, 4, false, false, false, false, false},
            /* D16Unorm       */ {2, 1, 1, 1, true, false, false, false, false},
            /* D32Float       */ {4, 1, 1, 1, true, false, false, false, true},
            /* D24UnormS8Uint */ {4, 1, 1, 2, true, true, false, false, false},
            /* D32FloatS8Uint */ {8, 1, 1, 2, true, true, false, false, true},
            /* BC1Unorm       */ {8, 4, 4, 4, false, false, false, true, false},
            /* BC3Unorm       */ {16, 4, 4, 4, false, false, false, true, false},
            /* BC7Unorm       */ {16, 4, 4, 4, false, false, false, true, false},
        };
        static_assert(std::size(kTable) == static_cast<size_t>(Format::Count));
        return kTable[static_cast<size_t>(format)];
    }

    bool ApiCompiledIn(Api api) {
        switch (api) {
#if RHI_HAS_VULKAN
        case Api::Vulkan: return true;
#endif
#if RHI_HAS_D3D12
        case Api::D3D12: return true;
#endif
#if RHI_HAS_D3D11
        case Api::D3D11: return true;
#endif
#if RHI_HAS_METAL
        case Api::Metal: return true;
#endif
#if RHI_HAS_WEBGPU
        case Api::WebGPU: return true;
#endif
        case Api::None: return true;
        default: return false;
        }
    }

    std::vector<Api> CompiledApis() {
        std::vector<Api> out;
        for (Api api : {Api::Vulkan, Api::D3D12, Api::D3D11, Api::Metal, Api::WebGPU})
            if (ApiCompiledIn(api))
                out.push_back(api);
        return out;
    }

    std::unique_ptr<Instance> CreateInstance(Api api, const InstanceOptions& options) {
        switch (api) {
        case Api::None: return backends::CreateNullInstance(options);
#if RHI_HAS_VULKAN
        case Api::Vulkan: return backends::CreateVulkanInstance(options);
#endif
#if RHI_HAS_D3D12
        case Api::D3D12: return backends::CreateD3D12Instance(options);
#endif
#if RHI_HAS_D3D11
        case Api::D3D11: return backends::CreateD3D11Instance(options);
#endif
#if RHI_HAS_METAL
        case Api::Metal: return backends::CreateMetalInstance(options);
#endif
#if RHI_HAS_WEBGPU
        case Api::WebGPU: return backends::CreateWebGpuInstance(options);
#endif
        default: return nullptr;
        }
    }
}
