#pragma once
#include "rhi/Device.hpp"

// defaults keep a bare include working; the build sets these per target
#ifndef RHI_HAS_VULKAN
#define RHI_HAS_VULKAN 0
#endif
#ifndef RHI_HAS_D3D12
#define RHI_HAS_D3D12 0
#endif
#ifndef RHI_HAS_D3D11
#define RHI_HAS_D3D11 0
#endif
#ifndef RHI_HAS_METAL
#define RHI_HAS_METAL 0
#endif
#ifndef RHI_HAS_WEBGPU
#define RHI_HAS_WEBGPU 0
#endif

namespace rhi::backends {
    std::unique_ptr<Instance> CreateNullInstance(const InstanceOptions& options);
    std::unique_ptr<Instance> CreateVulkanInstance(const InstanceOptions& options);
    std::unique_ptr<Instance> CreateD3D12Instance(const InstanceOptions& options);
    std::unique_ptr<Instance> CreateD3D11Instance(const InstanceOptions& options);
    std::unique_ptr<Instance> CreateMetalInstance(const InstanceOptions& options);
    std::unique_ptr<Instance> CreateWebGpuInstance(const InstanceOptions& options);
}
