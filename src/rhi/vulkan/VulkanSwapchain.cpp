#include "rhi/vulkan/VulkanDevice.hpp"

#include "rhi/vulkan/VulkanConvert.hpp"

#include <algorithm>
#include <cstdint>

namespace rhi::vk {
    namespace {
        // declared here instead of pulling in each window system's headers: the structs are plain data and the
        // entry points are fetched by name
        struct XlibSurfaceInfo {
            VkStructureType sType;
            const void* pNext;
            VkFlags flags;
            void* dpy;
            unsigned long window;
        };
        struct WaylandSurfaceInfo {
            VkStructureType sType;
            const void* pNext;
            VkFlags flags;
            void* display;
            void* surface;
        };
        struct Win32SurfaceInfo {
            VkStructureType sType;
            const void* pNext;
            VkFlags flags;
            void* hinstance;
            void* hwnd;
        };
        struct MetalSurfaceInfo {
            VkStructureType sType;
            const void* pNext;
            VkFlags flags;
            const void* layer;
        };
        struct AndroidSurfaceInfo {
            VkStructureType sType;
            const void* pNext;
            VkFlags flags;
            void* window;
        };
        constexpr VkStructureType kXlibType = static_cast<VkStructureType>(1000004000);
        constexpr VkStructureType kWaylandType = static_cast<VkStructureType>(1000006000);
        constexpr VkStructureType kWin32Type = static_cast<VkStructureType>(1000009000);
        constexpr VkStructureType kAndroidType = static_cast<VkStructureType>(1000008000);
        constexpr VkStructureType kMetalType = static_cast<VkStructureType>(1000217000);

        template <class Info>
        VkSurfaceKHR CreateWith(VkInstance instance, const char* fnName, const Info& info) {
            using Fn = VkResult(VKAPI_PTR*)(VkInstance, const Info*, const VkAllocationCallbacks*, VkSurfaceKHR*);
            auto create = reinterpret_cast<Fn>(vkGetInstanceProcAddr(instance, fnName));
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            if (!create || create(instance, &info, nullptr, &surface) != VK_SUCCESS)
                return VK_NULL_HANDLE;
            return surface;
        }

        VkSurfaceKHR CreateSurface(VkInstance instance, const NativeWindow& w) {
#if defined(_WIN32)
            Win32SurfaceInfo info{kWin32Type, nullptr, 0, w.display, w.window};
            return CreateWith(instance, "vkCreateWin32SurfaceKHR", info);
#elif defined(__APPLE__)
            MetalSurfaceInfo info{kMetalType, nullptr, 0, w.window};
            return CreateWith(instance, "vkCreateMetalSurfaceEXT", info);
#elif defined(__ANDROID__)
            AndroidSurfaceInfo info{kAndroidType, nullptr, 0, w.window};
            return CreateWith(instance, "vkCreateAndroidSurfaceKHR", info);
#else
            if (w.wayland) {
                WaylandSurfaceInfo info{kWaylandType, nullptr, 0, w.display, w.window};
                return CreateWith(instance, "vkCreateWaylandSurfaceKHR", info);
            }
            XlibSurfaceInfo info{kXlibType, nullptr, 0, w.display, static_cast<unsigned long>(reinterpret_cast<uintptr_t>(w.window))};
            return CreateWith(instance, "vkCreateXlibSurfaceKHR", info);
#endif
        }

        Format FromVk(VkFormat f) {
            switch (f) {
            case VK_FORMAT_B8G8R8A8_UNORM: return Format::BGRA8Unorm;
            case VK_FORMAT_B8G8R8A8_SRGB: return Format::BGRA8Srgb;
            case VK_FORMAT_R8G8B8A8_UNORM: return Format::RGBA8Unorm;
            case VK_FORMAT_R8G8B8A8_SRGB: return Format::RGBA8Srgb;
            case VK_FORMAT_R16G16B16A16_SFLOAT: return Format::RGBA16Float;
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return Format::RGB10A2Unorm;
            default: return Format::Unknown;
            }
        }
    }

    class Swapchain final : public rhi::Swapchain {
    public:
        Swapchain(Device& device, const SwapchainDesc& desc) : dev(device), desc(desc) {}

        ~Swapchain() override {
            dev.fn.vkDeviceWaitIdle(dev.device);
            Destroy();
            for (VkSemaphore s : acquireSems)
                dev.fn.vkDestroySemaphore(dev.device, s, nullptr);
            if (surface)
                vkDestroySurfaceKHR(dev.inst->instance, surface, nullptr);
        }

        bool Init() {
            if (!dev.swapchainExt || !dev.inst->surface)
                return false;
            surface = CreateSurface(dev.inst->instance, desc.window);
            if (!surface)
                return false;
            VkBool32 supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev.physical, dev.queues[0].family, surface, &supported);
            if (!supported)
                return false;
            // a zero-sized window has nothing to build yet; Resize or Acquire retries
            Build(desc.width, desc.height);
            return true;
        }

        bool Acquire(Image& out) override {
            if (needsRebuild) {
                needsRebuild = false;
                Build(desc.width, desc.height);
            }
            if (!swapchain)
                return false;
            for (int attempt = 0; attempt < 2; attempt++) {
                VkSemaphore sem = acquireSems[ring];
                const VkResult r = dev.fn.vkAcquireNextImageKHR(dev.device, swapchain, UINT64_MAX, sem, VK_NULL_HANDLE, &index);
                if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
                    if (r == VK_SUBOPTIMAL_KHR)
                        needsRebuild = true;
                    ring = (ring + 1) % acquireSems.size();
                    dev.SetPendingAcquire(sem);
                    out.texture = handles[index];
                    out.index = index;
                    return true;
                }
                if (r != VK_ERROR_OUT_OF_DATE_KHR || !Build(desc.width, desc.height))
                    return false;
            }
            return false;
        }

        void Present() override {
            if (!swapchain)
                return;
            VkSemaphore done = renderDone[index];
            dev.SubmitPresent(done);
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &done;
            pi.swapchainCount = 1;
            pi.pSwapchains = &swapchain;
            pi.pImageIndices = &index;
            std::lock_guard lock(*dev.PresentMutex());
            const VkResult r = dev.fn.vkQueuePresentKHR(dev.PresentQueue(), &pi);
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
                needsRebuild = true;
            else if (r != VK_SUCCESS)
                dev.ReportError("vkQueuePresentKHR failed (" + std::to_string(r) + ")");
        }

        bool Resize(uint32_t width, uint32_t height) override {
            desc.width = width;
            desc.height = height;
            if (!width || !height) {
                dev.fn.vkDeviceWaitIdle(dev.device);
                Destroy();
                return false;
            }
            if (swapchain && width == extent.width && height == extent.height && !needsRebuild)
                return true;
            needsRebuild = false;
            return Build(width, height);
        }

        void SetPresentMode(PresentMode mode) override {
            if (mode == desc.present)
                return;
            desc.present = mode;
            needsRebuild = true;
        }

        Format GetFormat() const override { return rhiFormat; }
        uint32_t Width() const override { return swapchain ? extent.width : 0; }
        uint32_t Height() const override { return swapchain ? extent.height : 0; }
        PresentMode GetPresentMode() const override { return activeMode; }
        uint32_t ImageCount() const override { return static_cast<uint32_t>(handles.size()); }
        bool IsHdr() const override { return hdrActive; }

    private:
        void Destroy() {
            for (TextureHandle h : handles)
                dev.DestroyWrapped(h);
            handles.clear();
            images.clear();
            for (VkSemaphore s : renderDone)
                dev.fn.vkDestroySemaphore(dev.device, s, nullptr);
            renderDone.clear();
            if (swapchain) {
                dev.fn.vkDestroySwapchainKHR(dev.device, swapchain, nullptr);
                swapchain = VK_NULL_HANDLE;
            }
        }

        bool Build(uint32_t width, uint32_t height) {
            VkSurfaceCapabilitiesKHR caps{};
            if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev.physical, surface, &caps) != VK_SUCCESS)
                return false;
            VkExtent2D ext = caps.currentExtent;
            if (ext.width == 0xFFFFFFFFu) {
                ext.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
                ext.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
            }
            if (!ext.width || !ext.height) {
                dev.fn.vkDeviceWaitIdle(dev.device);
                Destroy();
                return false;
            }

            uint32_t count = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(dev.physical, surface, &count, nullptr);
            std::vector<VkSurfaceFormatKHR> formats(count);
            vkGetPhysicalDeviceSurfaceFormatsKHR(dev.physical, surface, &count, formats.data());
            VkSurfaceFormatKHR chosen{};
            bool found = false;
            hdrActive = false;
            if (desc.hdr)
                for (const auto& f : formats)
                    if (f.format == VK_FORMAT_R16G16B16A16_SFLOAT && f.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) {
                        chosen = f;
                        found = hdrActive = true;
                    }
            // unorm, not srgb: the game's shaders encode gamma themselves
            for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM})
                for (const auto& f : formats)
                    if (!found && f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                        chosen = f;
                        found = true;
                    }
            if (!found && !formats.empty() && FromVk(formats[0].format) != Format::Unknown) {
                chosen = formats[0];
                found = true;
            }
            if (!found) {
                dev.ReportError("swapchain: no usable surface format");
                return false;
            }

            vkGetPhysicalDeviceSurfacePresentModesKHR(dev.physical, surface, &count, nullptr);
            std::vector<VkPresentModeKHR> modes(count);
            vkGetPhysicalDeviceSurfacePresentModesKHR(dev.physical, surface, &count, modes.data());
            auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
            VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
            activeMode = PresentMode::Fifo;
            if (desc.present == PresentMode::Mailbox && has(VK_PRESENT_MODE_MAILBOX_KHR)) {
                mode = VK_PRESENT_MODE_MAILBOX_KHR;
                activeMode = PresentMode::Mailbox;
            } else if (desc.present == PresentMode::Immediate) {
                if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
                    mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
                    activeMode = PresentMode::Immediate;
                } else if (has(VK_PRESENT_MODE_MAILBOX_KHR)) {
                    mode = VK_PRESENT_MODE_MAILBOX_KHR;
                    activeMode = PresentMode::Mailbox;
                }
            }

            uint32_t imageCount = (std::max)(desc.imageCount, caps.minImageCount);
            if (caps.maxImageCount)
                imageCount = (std::min)(imageCount, caps.maxImageCount);

            VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            usage |= caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            usage |= caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (desc.allowSampling)
                usage |= caps.supportedUsageFlags & VK_IMAGE_USAGE_SAMPLED_BIT;

            VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
            ci.surface = surface;
            ci.minImageCount = imageCount;
            ci.imageFormat = chosen.format;
            ci.imageColorSpace = chosen.colorSpace;
            ci.imageExtent = ext;
            ci.imageArrayLayers = 1;
            ci.imageUsage = usage;
            ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
            ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                                                                                     : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
            ci.presentMode = mode;
            ci.clipped = VK_TRUE;
            ci.oldSwapchain = swapchain;

            // old images and semaphores may still be in flight; resizing is rare enough to wait
            dev.fn.vkDeviceWaitIdle(dev.device);
            VkSwapchainKHR fresh = VK_NULL_HANDLE;
            const VkResult r = dev.fn.vkCreateSwapchainKHR(dev.device, &ci, nullptr, &fresh);
            for (TextureHandle h : handles)
                dev.DestroyWrapped(h);
            handles.clear();
            for (VkSemaphore s : renderDone)
                dev.fn.vkDestroySemaphore(dev.device, s, nullptr);
            renderDone.clear();
            if (swapchain)
                dev.fn.vkDestroySwapchainKHR(dev.device, swapchain, nullptr);
            swapchain = VK_NULL_HANDLE;
            if (r != VK_SUCCESS) {
                dev.ReportError("vkCreateSwapchainKHR failed (" + std::to_string(r) + ")");
                return false;
            }
            swapchain = fresh;
            extent = ext;
            rhiFormat = FromVk(chosen.format);

            vkGetSwapchainImagesKHR_(imageCount);
            TextureDesc td;
            td.format = rhiFormat;
            td.width = ext.width;
            td.height = ext.height;
            td.usage = TextureUsage::ColorTarget | ((usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ? TextureUsage::CopySrc : TextureUsage::None) |
                       ((usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ? TextureUsage::CopyDst : TextureUsage::None) |
                       ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) ? TextureUsage::Sampled : TextureUsage::None);
            td.name = "backbuffer";
            for (VkImage image : images)
                handles.push_back(dev.WrapSwapchainImage(image, td, chosen.format));

            VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            for (size_t i = 0; i < images.size(); i++) {
                VkSemaphore s = VK_NULL_HANDLE;
                dev.fn.vkCreateSemaphore(dev.device, &si, nullptr, &s);
                renderDone.push_back(s);
            }
            // acquire semaphores outlive rebuilds; one spare so a pending wait never meets a reused semaphore
            while (acquireSems.size() < images.size() + 1) {
                VkSemaphore s = VK_NULL_HANDLE;
                dev.fn.vkCreateSemaphore(dev.device, &si, nullptr, &s);
                acquireSems.push_back(s);
            }
            index = 0;
            return true;
        }

        void vkGetSwapchainImagesKHR_(uint32_t) {
            uint32_t n = 0;
            dev.fn.vkGetSwapchainImagesKHR(dev.device, swapchain, &n, nullptr);
            images.resize(n);
            dev.fn.vkGetSwapchainImagesKHR(dev.device, swapchain, &n, images.data());
        }

        Device& dev;
        SwapchainDesc desc;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkExtent2D extent{};
        Format rhiFormat = Format::BGRA8Unorm;
        PresentMode activeMode = PresentMode::Fifo;
        bool hdrActive = false;
        bool needsRebuild = false;
        std::vector<VkImage> images;
        std::vector<TextureHandle> handles;
        std::vector<VkSemaphore> renderDone;
        std::vector<VkSemaphore> acquireSems;
        size_t ring = 0;
        uint32_t index = 0;
    };

    std::unique_ptr<rhi::Swapchain> MakeSwapchain(Device& device, const SwapchainDesc& desc) {
        auto sc = std::make_unique<Swapchain>(device, desc);
        if (!sc->Init())
            return nullptr;
        return sc;
    }
}
