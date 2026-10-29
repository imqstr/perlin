#include "rhi/Backends.hpp"
#include "rhi/d3d11/D3D11Internal.hpp"

#include <algorithm>
#include <cstdio>

namespace rhi::d3d11 {
    namespace {
        std::string Narrow(const wchar_t* w) {
            std::string out;
            for (; *w; w++)
                out.push_back(*w < 0x80 ? static_cast<char>(*w) : '?');
            return out;
        }

        class SwapchainImpl final : public Swapchain {
        public:
            SwapchainImpl(Device& device, const SwapchainDesc& desc)
                : device(device), desc(desc), width(desc.width), height(desc.height), presentMode(desc.present), hdr(desc.hdr) {}

            ~SwapchainImpl() override {
                device.WaitIdle();
                if (handle)
                    device.ReleaseBackbuffer(handle);
                chain = nullptr;
            }

            bool Init() {
                hwnd = static_cast<HWND>(desc.window.window);
                if (!hwnd)
                    return false;
                if (!width || !height) {
                    RECT rc{};
                    GetClientRect(hwnd, &rc);
                    width = static_cast<uint32_t>(std::max<LONG>(rc.right - rc.left, 1));
                    height = static_cast<uint32_t>(std::max<LONG>(rc.bottom - rc.top, 1));
                }
                format = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
                // flip models need windows 8 / 10; older systems fall back to the blit model
                const DXGI_SWAP_EFFECT effects[] = {DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, DXGI_SWAP_EFFECT_DISCARD};
                for (const DXGI_SWAP_EFFECT effect : effects) {
                    const bool flip = effect != DXGI_SWAP_EFFECT_DISCARD;
                    DXGI_SWAP_CHAIN_DESC1 sd{};
                    sd.Width = width;
                    sd.Height = height;
                    sd.Format = format;
                    sd.SampleDesc.Count = 1;
                    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | (desc.allowSampling ? DXGI_USAGE_SHADER_INPUT : 0);
                    sd.BufferCount = flip ? std::clamp(desc.imageCount, 2u, 4u) : 1;
                    sd.SwapEffect = effect;
                    sd.Scaling = DXGI_SCALING_STRETCH;
                    tearing = flip && device.Inst().tearing;
                    flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
                    sd.Flags = flags;
                    if (SUCCEEDED(device.Inst().factory->CreateSwapChainForHwnd(device.Dev(), hwnd, &sd, nullptr, nullptr, chain.put())))
                        break;
                    chain = nullptr;
                }
                if (!chain)
                    return false;
                device.Inst().factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
                if (hdr)
                    if (auto c3 = chain.try_as<IDXGISwapChain3>())
                        c3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
                return Wrap();
            }

            bool Acquire(Image& out) override {
                if (!width || !height || !handle)
                    return false;
                out.texture = handle;
                out.index = 0;
                return true;
            }

            void Present() override {
                const UINT interval = presentMode == PresentMode::Fifo ? 1 : 0;
                const UINT presentFlags = interval == 0 && tearing && presentMode == PresentMode::Immediate ? DXGI_PRESENT_ALLOW_TEARING : 0;
                auto lock = device.LockContext();
                chain->Present(interval, presentFlags);
            }

            bool Resize(uint32_t w, uint32_t h) override {
                if (w == 0 || h == 0) {
                    width = w;
                    height = h;
                    return false;
                }
                if (w == width && h == height && handle)
                    return true;
                width = w;
                height = h;
                // every reference to the buffers must be gone before ResizeBuffers
                device.WaitIdle();
                {
                    auto lock = device.LockContext();
                    device.Imm()->ClearState();
                    device.Imm()->Flush();
                }
                if (handle) {
                    device.ReleaseBackbuffer(handle);
                    handle = {};
                }
                if (FAILED(chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, flags)))
                    return false;
                return Wrap();
            }

            void SetPresentMode(PresentMode mode) override { presentMode = mode; }
            Format GetFormat() const override { return hdr ? Format::RGBA16Float : Format::BGRA8Unorm; }
            uint32_t Width() const override { return width; }
            uint32_t Height() const override { return height; }
            PresentMode GetPresentMode() const override { return presentMode; }
            uint32_t ImageCount() const override { return 2; }
            bool IsHdr() const override { return hdr; }

        private:
            // flip model: the application always renders to buffer 0 and dxgi rotates behind it
            bool Wrap() {
                winrt::com_ptr<ID3D11Texture2D> buffer;
                if (FAILED(chain->GetBuffer(0, winrt::guid_of<ID3D11Texture2D>(), buffer.put_void())))
                    return false;
                TextureDesc td;
                td.width = width;
                td.height = height;
                td.format = GetFormat();
                td.usage = TextureUsage::ColorTarget | (desc.allowSampling ? TextureUsage::Sampled : TextureUsage::None) | TextureUsage::CopySrc;
                handle = device.WrapBackbuffer(buffer.get(), td);
                return static_cast<bool>(handle);
            }

            Device& device;
            SwapchainDesc desc;
            HWND hwnd = nullptr;
            winrt::com_ptr<IDXGISwapChain1> chain;
            TextureHandle handle;
            DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
            UINT flags = 0;
            uint32_t width = 0, height = 0;
            PresentMode presentMode = PresentMode::Fifo;
            bool tearing = false, hdr = false;
        };

        class D3D11Instance final : public Instance {
        public:
            explicit D3D11Instance(const InstanceOptions& options) : options(options) {}

            bool Init() {
                state = std::make_shared<InstanceState>();
                Library& lib = state->lib;
                lib.d3d11 = LoadLibraryW(L"d3d11.dll");
                lib.dxgi = LoadLibraryW(L"dxgi.dll");
                if (!lib.d3d11 || !lib.dxgi)
                    return false;
                lib.createDevice = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(lib.d3d11, "D3D11CreateDevice"));
                lib.createFactory1 = reinterpret_cast<decltype(lib.createFactory1)>(GetProcAddress(lib.dxgi, "CreateDXGIFactory1"));
                if (!lib.createDevice || !lib.createFactory1)
                    return false;
                // shaders are hlsl source: without the compiler no pipeline can be built
                lib.compiler = LoadLibraryW(L"d3dcompiler_47.dll");
                if (lib.compiler)
                    lib.compile = reinterpret_cast<CompileFn>(GetProcAddress(lib.compiler, "D3DCompile"));
                state->validation = options.validation;
                if (FAILED(lib.createFactory1(winrt::guid_of<IDXGIFactory2>(), state->factory.put_void())))
                    return false;

                BOOL tearing = FALSE;
                if (auto f5 = state->factory.try_as<IDXGIFactory5>())
                    if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))))
                        state->tearing = tearing != FALSE;
                EnumerateAdapters();
                return true;
            }

            Api GetApi() const override { return Api::D3D11; }
            const std::vector<AdapterInfo>& Adapters() const override { return adapters; }

            std::unique_ptr<rhi::Device> CreateDevice(const DeviceOptions& deviceOptions) override {
                if (deviceOptions.adapterIndex >= adapters.size() || !adapters[deviceOptions.adapterIndex].usable)
                    return nullptr;
                auto device = std::make_unique<Device>(state, native[deviceOptions.adapterIndex], adapters[deviceOptions.adapterIndex], deviceOptions);
                std::string error;
                if (!device->Init(error)) {
                    std::fprintf(stderr, "[d3d11] device creation failed: %s\n", error.c_str());
                    return nullptr;
                }
                return device;
            }

        private:
            void Consider(winrt::com_ptr<IDXGIAdapter1> adapter) {
                DXGI_ADAPTER_DESC1 d{};
                adapter->GetDesc1(&d);
                AdapterInfo a;
                a.api = Api::D3D11;
                a.index = static_cast<uint32_t>(adapters.size());
                a.name = Narrow(d.Description);
                a.vendorId = d.VendorId;
                a.deviceId = d.DeviceId;
                a.dedicatedVram = d.DedicatedVideoMemory;
                a.sharedMemory = d.SharedSystemMemory;
                a.luid = (static_cast<uint64_t>(d.AdapterLuid.HighPart) << 32) | d.AdapterLuid.LowPart;
                const bool software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
                a.type = software ? AdapterType::Cpu : (d.DedicatedVideoMemory > (512ull << 20) ? AdapterType::Discrete : AdapterType::Integrated);
                LARGE_INTEGER umd{};
                if (SUCCEEDED(adapter->CheckInterfaceSupport(winrt::guid_of<IDXGIDevice>(), &umd)))
                    a.driverVersion = static_cast<uint64_t>(umd.QuadPart);

                const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
                D3D_FEATURE_LEVEL got{};
                HRESULT hr = state->lib.createDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                                     D3D11_SDK_VERSION, nullptr, &got, nullptr);
                if (FAILED(hr))
                    hr = state->lib.createDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 1,
                                                 D3D11_SDK_VERSION, nullptr, &got, nullptr);
                if (FAILED(hr)) {
                    a.usable = false;
                    a.unusableReason = "no Direct3D 11 feature level 11_0";
                } else {
                    a.apiVersion = got;
                    if (!state->lib.compile) {
                        a.usable = false;
                        a.unusableReason = "d3dcompiler_47.dll is missing";
                    }
                }
                adapters.push_back(std::move(a));
                native.push_back(std::move(adapter));
            }

            void EnumerateAdapters() {
                winrt::com_ptr<IDXGIAdapter1> warp;
                const auto handle = [&](winrt::com_ptr<IDXGIAdapter1> a) {
                    DXGI_ADAPTER_DESC1 d{};
                    a->GetDesc1(&d);
                    if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                        if (!warp)
                            warp = a;
                        return;
                    }
                    Consider(a);
                };
                if (auto f6 = state->factory.try_as<IDXGIFactory6>()) {
                    for (UINT i = 0;; i++) {
                        winrt::com_ptr<IDXGIAdapter1> a;
                        if (FAILED(f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, winrt::guid_of<IDXGIAdapter1>(), a.put_void())))
                            break;
                        handle(a);
                    }
                } else {
                    for (UINT i = 0;; i++) {
                        winrt::com_ptr<IDXGIAdapter1> a;
                        if (FAILED(state->factory->EnumAdapters1(i, a.put())))
                            break;
                        handle(a);
                    }
                }
                if (warp)
                    Consider(warp);
            }

            InstanceOptions options;
            std::shared_ptr<InstanceState> state;
            std::vector<AdapterInfo> adapters;
            std::vector<winrt::com_ptr<IDXGIAdapter1>> native;
        };
    }

    std::unique_ptr<rhi::Swapchain> MakeSwapchain(Device& device, const SwapchainDesc& desc) {
        auto sc = std::make_unique<SwapchainImpl>(device, desc);
        if (!sc->Init())
            return nullptr;
        return sc;
    }
}

namespace rhi::backends {
    std::unique_ptr<Instance> CreateD3D11Instance(const InstanceOptions& options) {
        auto instance = std::make_unique<d3d11::D3D11Instance>(options);
        if (!instance->Init())
            return nullptr;
        return instance;
    }
}
