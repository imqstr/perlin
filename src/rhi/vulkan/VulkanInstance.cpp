#include "rhi/Backends.hpp"
#include "rhi/vulkan/VulkanDevice.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

namespace rhi::vk {
    namespace {
        VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
            auto* state = static_cast<InstanceState*>(user);
            if (severity < VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
                return VK_FALSE;
            std::lock_guard lock(state->messageMutex);
            if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                state->errorCount++;
            if (state->messages.size() < 256)
                state->messages.push_back(data && data->pMessage ? data->pMessage : "");
            std::fprintf(stderr, "[vulkan] %s\n", data && data->pMessage ? data->pMessage : "");
            return VK_FALSE;
        }

        bool HasExt(const std::vector<VkExtensionProperties>& list, const char* name) {
            for (const auto& e : list)
                if (std::strcmp(e.extensionName, name) == 0)
                    return true;
            return false;
        }

        std::vector<std::string> DeviceExtensionNames(VkPhysicalDevice device) {
            uint32_t count = 0;
            vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
            std::vector<VkExtensionProperties> props(count);
            vkEnumerateDeviceExtensionProperties(device, nullptr, &count, props.data());
            std::vector<std::string> out;
            out.reserve(count);
            for (const auto& p : props)
                out.emplace_back(p.extensionName);
            return out;
        }
    }

    InstanceState::~InstanceState() {
        if (messenger && vkDestroyDebugUtilsMessengerEXT)
            vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
        if (instance && vkDestroyInstance)
            vkDestroyInstance(instance, nullptr);
    }

    bool PhysicalInfo::Has(const char* name) const {
        for (const auto& e : extensions)
            if (e == name)
                return true;
        return false;
    }

    namespace {
        class VulkanInstance final : public Instance {
        public:
            explicit VulkanInstance(const InstanceOptions& options) : options(options) {}

            bool Init() {
                if (options.hooks.vkGetInstanceProcAddr) {
                    volkInitializeCustom(reinterpret_cast<PFN_vkGetInstanceProcAddr>(options.hooks.vkGetInstanceProcAddr));
                } else if (volkInitialize() != VK_SUCCESS) {
                    return false;
                }

                uint32_t apiVersion = VK_API_VERSION_1_0;
                if (vkEnumerateInstanceVersion)
                    vkEnumerateInstanceVersion(&apiVersion);
                if (apiVersion < VK_API_VERSION_1_1)
                    return false;

                state = std::make_shared<InstanceState>();
                state->apiVersion = std::min(apiVersion, VK_MAKE_API_VERSION(0, 1, 3, 0));

                uint32_t extCount = 0;
                vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr);
                std::vector<VkExtensionProperties> available(extCount);
                vkEnumerateInstanceExtensionProperties(nullptr, &extCount, available.data());

                std::vector<const char*> extensions;
                auto enable = [&](const char* name) {
                    if (!HasExt(available, name))
                        return false;
                    extensions.push_back(name);
                    state->enabledExtensions.push_back(name);
                    return true;
                };

                if (enable(VK_KHR_SURFACE_EXTENSION_NAME)) {
                    state->surface = true;
#if defined(_WIN32)
                    enable("VK_KHR_win32_surface");
#elif defined(__APPLE__)
                    enable("VK_EXT_metal_surface");
#elif defined(__ANDROID__)
                    enable("VK_KHR_android_surface");
#else
                    enable("VK_KHR_xlib_surface");
                    enable("VK_KHR_xcb_surface");
                    enable("VK_KHR_wayland_surface");
#endif
                    state->colorSpaceExt = enable(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
                }
#if defined(__APPLE__)
                const bool portability = enable(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
                enable(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#else
                const bool portability = false;
#endif

                std::vector<const char*> layers;
                if (options.validation) {
                    uint32_t layerCount = 0;
                    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
                    std::vector<VkLayerProperties> layerProps(layerCount);
                    vkEnumerateInstanceLayerProperties(&layerCount, layerProps.data());
                    for (const auto& l : layerProps)
                        if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                            layers.push_back("VK_LAYER_KHRONOS_validation");
                            state->validation = true;
                        }
                }
                // debug utils also names objects in captures, so it is requested whenever validation is
                if (options.validation)
                    state->debugUtils = enable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

                for (const char* e : options.hooks.instanceExtensions)
                    if (std::find(extensions.begin(), extensions.end(), e) == extensions.end() && HasExt(available, e))
                        extensions.push_back(e);

                VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
                app.pApplicationName = options.appName;
                app.pEngineName = "Perlin RHI";
                app.apiVersion = state->apiVersion;

                VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
                ci.pApplicationInfo = &app;
                ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
                ci.ppEnabledExtensionNames = extensions.data();
                ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
                ci.ppEnabledLayerNames = layers.data();
                if (portability)
                    ci.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;

                if (vkCreateInstance(&ci, nullptr, &state->instance) != VK_SUCCESS) {
                    state->instance = VK_NULL_HANDLE;
                    return false;
                }
                volkLoadInstanceOnly(state->instance);

                if (state->debugUtils) {
                    VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
                    mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
                    mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
                    mi.pfnUserCallback = DebugCallback;
                    mi.pUserData = state.get();
                    vkCreateDebugUtilsMessengerEXT(state->instance, &mi, nullptr, &state->messenger);
                }

                EnumerateAdapters();
                return true;
            }

            Api GetApi() const override { return Api::Vulkan; }
            const std::vector<AdapterInfo>& Adapters() const override { return adapters; }

            std::unique_ptr<rhi::Device> CreateDevice(const DeviceOptions& deviceOptions) override {
                if (deviceOptions.adapterIndex >= adapters.size() || !adapters[deviceOptions.adapterIndex].usable)
                    return nullptr;
                auto device = std::make_unique<Device>(state, infos[deviceOptions.adapterIndex], adapters[deviceOptions.adapterIndex],
                                                       deviceOptions);
                std::string error;
                if (!device->Init(error)) {
                    std::fprintf(stderr, "[vulkan] device creation failed: %s\n", error.c_str());
                    return nullptr;
                }
                return device;
            }

        private:
            void EnumerateAdapters() {
                uint32_t count = 0;
                vkEnumeratePhysicalDevices(state->instance, &count, nullptr);
                std::vector<VkPhysicalDevice> devices(count);
                vkEnumeratePhysicalDevices(state->instance, &count, devices.data());

                for (VkPhysicalDevice dev : devices) {
                    PhysicalInfo info;
                    info.device = dev;
                    vkGetPhysicalDeviceProperties(dev, &info.props);
                    info.extensions = DeviceExtensionNames(dev);

                    AdapterInfo a;
                    a.api = Api::Vulkan;
                    a.index = static_cast<uint32_t>(adapters.size());
                    a.name = info.props.deviceName;
                    a.vendorId = info.props.vendorID;
                    a.deviceId = info.props.deviceID;
                    a.apiVersion = info.props.apiVersion;
                    a.driverVersion = info.props.driverVersion;
                    switch (info.props.deviceType) {
                    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: a.type = AdapterType::Discrete; break;
                    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: a.type = AdapterType::Integrated; break;
                    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: a.type = AdapterType::Virtual; break;
                    case VK_PHYSICAL_DEVICE_TYPE_CPU: a.type = AdapterType::Cpu; break;
                    default: a.type = AdapterType::Unknown; break;
                    }

                    VkPhysicalDeviceMemoryProperties mem;
                    vkGetPhysicalDeviceMemoryProperties(dev, &mem);
                    for (uint32_t i = 0; i < mem.memoryHeapCount; i++) {
                        if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                            a.dedicatedVram = std::max<uint64_t>(a.dedicatedVram, mem.memoryHeaps[i].size);
                        else
                            a.sharedMemory = std::max<uint64_t>(a.sharedMemory, mem.memoryHeaps[i].size);
                    }
                    if (a.type == AdapterType::Integrated) {
                        a.sharedMemory = std::max(a.sharedMemory, a.dedicatedVram);
                        a.dedicatedVram = 0;
                    }

                    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
                    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
                    if (info.props.apiVersion >= VK_API_VERSION_1_2) {
                        props2.pNext = &driver;
                        vkGetPhysicalDeviceProperties2(dev, &props2);
                        a.driverName = driver.driverName;
                    }

                    a.usable = true;
                    if (info.props.apiVersion < VK_API_VERSION_1_2) {
                        a.usable = false;
                        a.unusableReason = "needs Vulkan 1.2";
                    } else {
                        const bool api13 = info.props.apiVersion >= VK_API_VERSION_1_3;
                        VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
                        VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
                        VkPhysicalDeviceDynamicRenderingFeaturesKHR dr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
                        VkPhysicalDeviceSynchronization2FeaturesKHR s2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR};
                        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
                        f2.pNext = &v12;
                        if (api13) {
                            v12.pNext = &v13;
                        } else if (info.Has(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) && info.Has(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) {
                            v12.pNext = &dr;
                            dr.pNext = &s2;
                        }
                        vkGetPhysicalDeviceFeatures2(dev, &f2);
                        const bool dynamicRendering = api13 ? v13.dynamicRendering : dr.dynamicRendering;
                        const bool sync2 = api13 ? v13.synchronization2 : s2.synchronization2;
                        if (!v12.timelineSemaphore) {
                            a.usable = false;
                            a.unusableReason = "no timeline semaphores";
                        } else if (!dynamicRendering) {
                            a.usable = false;
                            a.unusableReason = "no dynamic rendering";
                        } else if (!sync2) {
                            a.usable = false;
                            a.unusableReason = "no synchronization2";
                        }
                    }

                    adapters.push_back(std::move(a));
                    physical.push_back(dev);
                    infos.push_back(std::move(info));
                }
            }

            InstanceOptions options;
            std::shared_ptr<InstanceState> state;
            std::vector<AdapterInfo> adapters;
            std::vector<VkPhysicalDevice> physical;
            std::vector<PhysicalInfo> infos;
        };
    }
}

namespace rhi::backends {
    std::unique_ptr<Instance> CreateVulkanInstance(const InstanceOptions& options) {
        auto instance = std::make_unique<vk::VulkanInstance>(options);
        if (!instance->Init())
            return nullptr;
        return instance;
    }
}
