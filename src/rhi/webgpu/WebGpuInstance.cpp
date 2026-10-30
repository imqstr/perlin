#include "rhi/webgpu/WebGpuInternal.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
extern "C" {
// html5_webgpu.h still names WGPUSwapChain, which emdawnwebgpu no longer has, so the one function needed is declared here
WGPUDevice emscripten_webgpu_get_device(void);
}
#else
#include <webgpu/wgpu.h>
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace rhi::webgpu {
    namespace {
        void OnUncapturedError(WGPUDevice const*, WGPUErrorType type, WGPUStringView message, void*, void*) {
            std::fprintf(stderr, "[webgpu] validation error (%d): %s\n", static_cast<int>(type), Str(message).c_str());
        }

        void OnDeviceLost(WGPUDevice const*, WGPUDeviceLostReason reason, WGPUStringView message, void*, void*) {
            // destroying the device on shutdown reports a loss too; only the unexpected ones matter
            if (reason != WGPUDeviceLostReason_Destroyed)
                std::fprintf(stderr, "[webgpu] device lost (%d): %s\n", static_cast<int>(reason), Str(message).c_str());
        }

        AdapterType ToAdapterType(WGPUAdapterType type) {
            switch (type) {
            case WGPUAdapterType_DiscreteGPU: return AdapterType::Discrete;
            case WGPUAdapterType_IntegratedGPU: return AdapterType::Integrated;
            case WGPUAdapterType_CPU: return AdapterType::Cpu;
            default: return AdapterType::Unknown;
            }
        }

        WGPUPresentMode ToPresent(PresentMode mode) {
            switch (mode) {
            case PresentMode::Immediate: return WGPUPresentMode_Immediate;
            case PresentMode::Mailbox: return WGPUPresentMode_Mailbox;
            default: return WGPUPresentMode_Fifo;
            }
        }

        struct RequestState {
            bool done = false;
            bool ok = false;
            WGPUDevice device = nullptr;
            std::string message;
        };

        void OnDevice(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void* a, void*) {
            auto* s = static_cast<RequestState*>(a);
            s->ok = status == WGPURequestDeviceStatus_Success;
            s->device = device;
            s->message = Str(message);
            s->done = true;
        }

        bool WaitFor(WGPUInstance instance, const bool& done, uint64_t timeoutMs) {
            const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            while (!done) {
                wgpuInstanceProcessEvents(instance);
                if (std::chrono::steady_clock::now() > end)
                    return false;
#if !defined(__EMSCRIPTEN__)
                std::this_thread::sleep_for(std::chrono::microseconds(200));
#endif
            }
            return true;
        }

        constexpr WGPUFeatureName kWantedFeatures[] = {
            WGPUFeatureName_DepthClipControl,       WGPUFeatureName_Depth32FloatStencil8, WGPUFeatureName_TextureCompressionBC,
            WGPUFeatureName_IndirectFirstInstance,  WGPUFeatureName_RG11B10UfloatRenderable,
            WGPUFeatureName_Float32Filterable,      WGPUFeatureName_Float32Blendable,
        };

        class WebGpuSwapchain final : public Swapchain {
        public:
            WebGpuSwapchain(WebGpuDevice& device, Owned<WGPUSurface> surface, const SwapchainDesc& desc)
                : device(device), surface(std::move(surface)), width(desc.width), height(desc.height), present(desc.present),
                  allowSampling(desc.allowSampling) {}

            ~WebGpuSwapchain() override {
                if (handle)
                    device.Destroy(handle);
                if (surface)
                    wgpuSurfaceUnconfigure(surface.Get());
            }

            bool Init() {
                WGPUSurfaceCapabilities caps{};
                if (wgpuSurfaceGetCapabilities(surface.Get(), device.Adapter(), &caps) != WGPUStatus_Success || !caps.formatCount) {
                    std::fprintf(stderr, "[webgpu] surface reports no capabilities\n");
                    return false;
                }
                // the renderer applies its own tone curve, so a non-srgb format
                WGPUTextureFormat chosen = caps.formats[0];
                for (WGPUTextureFormat want : {WGPUTextureFormat_BGRA8Unorm, WGPUTextureFormat_RGBA8Unorm}) {
                    bool found = false;
                    for (size_t i = 0; i < caps.formatCount; i++)
                        found |= caps.formats[i] == want;
                    if (found) {
                        chosen = want;
                        break;
                    }
                }
                usage = WGPUTextureUsage_RenderAttachment;
                if (caps.usages & WGPUTextureUsage_CopySrc)
                    usage |= WGPUTextureUsage_CopySrc;
                sampled = allowSampling && (caps.usages & WGPUTextureUsage_TextureBinding);
                if (sampled)
                    usage |= WGPUTextureUsage_TextureBinding;
                presentModes.assign(caps.presentModes, caps.presentModes + caps.presentModeCount);
                wgpuSurfaceCapabilitiesFreeMembers(caps);

                native = chosen;
                format = FromTextureFormat(chosen);
                if (format == Format::Unknown) {
                    std::fprintf(stderr, "[webgpu] surface format has no rhi equivalent\n");
                    return false;
                }
                handle = device.CreateSurfaceTexture(format, width, height, sampled);
                Configure();
                return true;
            }

            bool Acquire(Image& out) override {
                if (!width || !height)
                    return false;
                if (needsConfigure)
                    Configure();
                WGPUSurfaceTexture st{};
                wgpuSurfaceGetCurrentTexture(surface.Get(), &st);
                switch (st.status) {
                case WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal: break;
                case WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal: needsConfigure = true; break;
                case WGPUSurfaceGetCurrentTextureStatus_Outdated:
                case WGPUSurfaceGetCurrentTextureStatus_Lost:
                    if (st.texture)
                        wgpuTextureRelease(st.texture);
                    needsConfigure = true;
                    return false;
                default:
                    if (st.texture)
                        wgpuTextureRelease(st.texture);
                    return false;
                }
                device.SetSurfaceTexture(handle, st.texture);
                out.texture = handle;
                out.index = 0;
                acquired = true;
                return true;
            }

            void Present() override {
                if (!acquired)
                    return;
                acquired = false;
#if !defined(__EMSCRIPTEN__)
                // the browser presents on its own when the frame callback returns
                wgpuSurfacePresent(surface.Get());
#endif
                device.SetSurfaceTexture(handle, nullptr);
            }

            bool Resize(uint32_t w, uint32_t h) override {
                if (w == width && h == height)
                    return true;
                width = w;
                height = h;
                device.ResizeSurfaceTexture(handle, w, h);
                needsConfigure = true;
                return w && h;
            }

            void SetPresentMode(PresentMode mode) override {
                if (mode == present)
                    return;
                present = mode;
                needsConfigure = true;
            }

            Format GetFormat() const override { return format; }
            uint32_t Width() const override { return width; }
            uint32_t Height() const override { return height; }
            PresentMode GetPresentMode() const override { return effectivePresent; }
            uint32_t ImageCount() const override { return 2; }

        private:
            void Configure() {
                needsConfigure = false;
                if (!width || !height)
                    return;
                WGPUPresentMode mode = ToPresent(present);
                bool supported = false;
                for (WGPUPresentMode m : presentModes)
                    supported |= m == mode;
                if (!supported)
                    mode = WGPUPresentMode_Fifo;
                effectivePresent = mode == WGPUPresentMode_Immediate ? PresentMode::Immediate
                                   : mode == WGPUPresentMode_Mailbox ? PresentMode::Mailbox
                                                                     : PresentMode::Fifo;
                WGPUSurfaceConfiguration cfg{};
                cfg.device = device.Dev();
                cfg.format = native;
                cfg.usage = usage;
                cfg.width = width;
                cfg.height = height;
                cfg.alphaMode = WGPUCompositeAlphaMode_Auto;
                cfg.presentMode = mode;
                wgpuSurfaceConfigure(surface.Get(), &cfg);
            }

            WebGpuDevice& device;
            Owned<WGPUSurface> surface;
            TextureHandle handle;
            uint32_t width, height;
            PresentMode present;
            PresentMode effectivePresent = PresentMode::Fifo;
            bool allowSampling;
            bool sampled = false;
            bool needsConfigure = false;
            bool acquired = false;
            Format format = Format::Unknown;
            WGPUTextureFormat native = WGPUTextureFormat_Undefined;
            WGPUTextureUsage usage = 0;
            std::vector<WGPUPresentMode> presentModes;
        };

        Owned<WGPUSurface> CreateSurface(WGPUInstance instance, const NativeWindow& w) {
            if (!w.window)
                return {};
            WGPUSurfaceDescriptor sd{};
            sd.label = View("perlin surface");
#if defined(__EMSCRIPTEN__)
            WGPUEmscriptenSurfaceSourceCanvasHTMLSelector src{};
            src.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
            src.selector = View(static_cast<const char*>(w.window));
            sd.nextInChain = &src.chain;
            return Owned<WGPUSurface>(wgpuInstanceCreateSurface(instance, &sd));
#elif defined(_WIN32)
            WGPUSurfaceSourceWindowsHWND src{};
            src.chain.sType = WGPUSType_SurfaceSourceWindowsHWND;
            src.hinstance = GetModuleHandleW(nullptr);
            src.hwnd = w.window;
            sd.nextInChain = &src.chain;
            return Owned<WGPUSurface>(wgpuInstanceCreateSurface(instance, &sd));
#elif defined(__ANDROID__)
            WGPUSurfaceSourceAndroidNativeWindow src{};
            src.chain.sType = WGPUSType_SurfaceSourceAndroidNativeWindow;
            src.window = w.window;
            sd.nextInChain = &src.chain;
            return Owned<WGPUSurface>(wgpuInstanceCreateSurface(instance, &sd));
#elif defined(__APPLE__)
            // the window is a CAMetalLayer, the same object the metal backend takes
            WGPUSurfaceSourceMetalLayer src{};
            src.chain.sType = WGPUSType_SurfaceSourceMetalLayer;
            src.layer = w.window;
            sd.nextInChain = &src.chain;
            return Owned<WGPUSurface>(wgpuInstanceCreateSurface(instance, &sd));
#else
            if (w.wayland) {
                WGPUSurfaceSourceWaylandSurface src{};
                src.chain.sType = WGPUSType_SurfaceSourceWaylandSurface;
                src.display = w.display;
                src.surface = w.window;
                sd.nextInChain = &src.chain;
                return Owned<WGPUSurface>(wgpuInstanceCreateSurface(instance, &sd));
            }
            WGPUSurfaceSourceXlibWindow src{};
            src.chain.sType = WGPUSType_SurfaceSourceXlibWindow;
            src.display = w.display;
            src.window = reinterpret_cast<uintptr_t>(w.window);
            sd.nextInChain = &src.chain;
            return Owned<WGPUSurface>(wgpuInstanceCreateSurface(instance, &sd));
#endif
        }

        class WebGpuInstance final : public Instance {
        public:
            explicit WebGpuInstance(const InstanceOptions& options) : options(options) {}

            bool Init() {
#if defined(__EMSCRIPTEN__)
                instance = Owned<WGPUInstance>(wgpuCreateInstance(nullptr));
                if (!instance)
                    return false;
                AdapterInfo info;
                info.api = Api::WebGPU;
                info.name = "Browser WebGPU";
                infos.push_back(info);
                return true;
#else
                WGPUInstanceExtras extras{};
                extras.chain.sType = static_cast<WGPUSType>(WGPUSType_InstanceExtras);
                extras.backends = WGPUInstanceBackend_Primary;
                extras.flags = options.validation ? (WGPUInstanceFlag_Debug | WGPUInstanceFlag_Validation) : WGPUInstanceFlag_Default;
                WGPUInstanceDescriptor id{};
                id.nextInChain = &extras.chain;
                instance = Owned<WGPUInstance>(wgpuCreateInstance(&id));
                if (!instance)
                    return false;

                WGPUInstanceEnumerateAdapterOptions eo{};
                eo.backends = WGPUInstanceBackend_Primary;
                const size_t count = wgpuInstanceEnumerateAdapters(instance.Get(), &eo, nullptr);
                std::vector<WGPUAdapter> raw(count);
                if (count)
                    wgpuInstanceEnumerateAdapters(instance.Get(), &eo, raw.data());
                for (WGPUAdapter a : raw) {
                    WGPUAdapterInfo ai{};
                    wgpuAdapterGetInfo(a, &ai);
                    AdapterInfo info;
                    info.api = Api::WebGPU;
                    info.index = static_cast<uint32_t>(infos.size());
                    info.name = Str(ai.device);
                    if (info.name.empty())
                        info.name = Str(ai.description);
                    info.vendorId = ai.vendorID;
                    info.deviceId = ai.deviceID;
                    info.type = ToAdapterType(ai.adapterType);
                    info.driverName = Str(ai.description);
                    wgpuAdapterInfoFreeMembers(ai);
                    adapters.emplace_back(a);
                    infos.push_back(std::move(info));
                }
                return !infos.empty();
#endif
            }

            Api GetApi() const override { return Api::WebGPU; }
            const std::vector<AdapterInfo>& Adapters() const override { return infos; }

            std::unique_ptr<Device> CreateDevice(const DeviceOptions& opts) override {
                if (opts.adapterIndex >= infos.size())
                    return nullptr;
#if defined(__EMSCRIPTEN__)
                // the page's loader script acquired the device before main(): blocking on a promise is not possible here
                WGPUDevice raw = emscripten_webgpu_get_device();
                if (!raw)
                    return nullptr;
                wgpuInstanceAddRef(instance.Get());
                auto device = std::make_unique<WebGpuDevice>(Owned<WGPUInstance>(instance.Get()), Owned<WGPUAdapter>(), Owned<WGPUDevice>(raw), opts,
                                                             infos[0], false);
#else
                WGPUAdapter adapter = adapters[opts.adapterIndex].Get();
                std::vector<WGPUFeatureName> features;
                for (WGPUFeatureName f : kWantedFeatures)
                    if (wgpuAdapterHasFeature(adapter, f))
                        features.push_back(f);
                WGPULimits limits{};
                wgpuAdapterGetLimits(adapter, &limits);

                WGPUDeviceDescriptor dd{};
                dd.label = View("perlin device");
                dd.requiredFeatureCount = features.size();
                dd.requiredFeatures = features.data();
                dd.requiredLimits = &limits;
                dd.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
                dd.deviceLostCallbackInfo.callback = OnDeviceLost;
                dd.uncapturedErrorCallbackInfo.callback = OnUncapturedError;

                RequestState state;
                WGPURequestDeviceCallbackInfo cb{};
                cb.mode = WGPUCallbackMode_AllowProcessEvents;
                cb.callback = OnDevice;
                cb.userdata1 = &state;
                wgpuAdapterRequestDevice(adapter, &dd, cb);
                if (!WaitFor(instance.Get(), state.done, 15000) || !state.ok || !state.device) {
                    std::fprintf(stderr, "[webgpu] device request failed: %s\n", state.message.c_str());
                    return nullptr;
                }
                wgpuAdapterAddRef(adapter);
                wgpuInstanceAddRef(instance.Get());
                auto device = std::make_unique<WebGpuDevice>(Owned<WGPUInstance>(instance.Get()), Owned<WGPUAdapter>(adapter),
                                                             Owned<WGPUDevice>(state.device), opts, infos[opts.adapterIndex], true);
#endif
                if (!device->Init())
                    return nullptr;
                return device;
            }

        private:
            InstanceOptions options;
            Owned<WGPUInstance> instance;
            std::vector<Owned<WGPUAdapter>> adapters;
            std::vector<AdapterInfo> infos;
        };
    }

    std::unique_ptr<Instance> MakeInstance(const InstanceOptions& options) {
        auto instance = std::make_unique<WebGpuInstance>(options);
        if (!instance->Init())
            return nullptr;
        return instance;
    }

    std::unique_ptr<Swapchain> WebGpuDevice::CreateSwapchain(const SwapchainDesc& desc) {
        Owned<WGPUSurface> surface = CreateSurface(Inst(), desc.window);
        if (!surface) {
            ReportError("CreateSwapchain: could not create a surface for this window");
            return nullptr;
        }
        auto swapchain = std::make_unique<WebGpuSwapchain>(*this, std::move(surface), desc);
        if (!swapchain->Init())
            return nullptr;
        return swapchain;
    }
}

namespace rhi::backends {
    std::unique_ptr<Instance> CreateWebGpuInstance(const InstanceOptions& options) { return webgpu::MakeInstance(options); }
}
