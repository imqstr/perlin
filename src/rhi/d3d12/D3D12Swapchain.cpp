#include "rhi/d3d12/D3D12Device.hpp"

#include "rhi/d3d12/D3D12Convert.hpp"

#include <algorithm>

namespace rhi::d3d12 {
    namespace {
        class SwapchainImpl final : public rhi::Swapchain {
        public:
            SwapchainImpl(Device& device, const SwapchainDesc& desc) : device(device), desc(desc) {}

            ~SwapchainImpl() override {
                device.WaitIdle();
                ReleaseBuffers();
            }

            bool Init() {
                hwnd = static_cast<HWND>(desc.window.window);
                if (!hwnd)
                    return false;
                width = desc.width;
                height = desc.height;
                presentMode = desc.present;
                imageCount = std::clamp(desc.imageCount, 2u, 8u);
                tearing = device.inst->tearing;
                hdr = false;

                DXGI_SWAP_CHAIN_DESC1 sd{};
                sd.Width = std::max(1u, width);
                sd.Height = std::max(1u, height);
                sd.Format = desc.hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
                sd.SampleDesc.Count = 1;
                sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | (desc.allowSampling ? DXGI_USAGE_SHADER_INPUT : 0);
                sd.BufferCount = imageCount;
                sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                sd.Scaling = DXGI_SCALING_STRETCH;
                sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
                sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
                flags = sd.Flags;

                winrt::com_ptr<IDXGISwapChain1> sc1;
                if (FAILED(device.inst->factory->CreateSwapChainForHwnd(device.queues[0].queue.get(), hwnd, &sd, nullptr, nullptr, sc1.put()))) {
                    if (!desc.hdr)
                        return false;
                    // hdr format refused: fall back to the 8-bit chain
                    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    if (FAILED(device.inst->factory->CreateSwapChainForHwnd(device.queues[0].queue.get(), hwnd, &sd, nullptr, nullptr, sc1.put())))
                        return false;
                }
                format = sd.Format;
                if (!(chain = sc1.try_as<IDXGISwapChain3>()))
                    return false;
                // alt+enter fullscreen switching is the game's job, not dxgi's
                device.inst->factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
                chain->SetMaximumFrameLatency(std::min(imageCount - 1, std::max(1u, device.options.framesInFlight)));
                waitable.attach(chain->GetFrameLatencyWaitableObject());

                if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                    UINT support = 0;
                    if (SUCCEEDED(chain->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &support)) &&
                        (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
                        hdr = SUCCEEDED(chain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709));
                    }
                }
                return WrapBuffers();
            }

            bool Acquire(Image& out) override {
                if (width == 0 || height == 0 || IsIconic(hwnd))
                    return false;
                if (pendingResize && !DoResize())
                    return false;
                // the chain holds back the cpu until a buffer is free: one wait per frame
                if (!acquired && waitable)
                    WaitForSingleObjectEx(waitable.get(), 1000, TRUE);
                acquired = true;
                const UINT index = chain->GetCurrentBackBufferIndex();
                out.index = index;
                out.texture = textures[index];
                return true;
            }

            void Present() override {
                if (!acquired)
                    return;
                acquired = false;
                UINT interval = 1;
                UINT flagsOut = 0;
                if (presentMode != PresentMode::Fifo) {
                    interval = 0;
                    // only an immediate present may tear; mailbox keeps the flip model's newest-wins behaviour
                    if (presentMode == PresentMode::Immediate && tearing)
                        flagsOut = DXGI_PRESENT_ALLOW_TEARING;
                }
                HRESULT hr;
                {
                    // dxgi presents on the queue it was created with: keep it ordered with other submits
                    std::lock_guard lock(device.queues[0].mutex);
                    hr = chain->Present(interval, flagsOut);
                }
                device.CheckRemoved(hr, "Present");
            }

            bool Resize(uint32_t w, uint32_t h) override {
                if (w == 0 || h == 0) {
                    width = w;
                    height = h;
                    return false;
                }
                if (w == width && h == height && !pendingResize)
                    return true;
                width = w;
                height = h;
                pendingResize = true;
                return DoResize();
            }

            void SetPresentMode(PresentMode mode) override { presentMode = mode; }
            Format GetFormat() const override { return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? Format::RGBA16Float : Format::BGRA8Unorm; }
            uint32_t Width() const override { return width; }
            uint32_t Height() const override { return height; }
            PresentMode GetPresentMode() const override { return presentMode; }
            uint32_t ImageCount() const override { return imageCount; }
            bool IsHdr() const override { return hdr; }

        private:
            bool DoResize() {
                // buffers may still be in flight and every reference must be gone before ResizeBuffers
                device.WaitIdle();
                ReleaseBuffers();
                if (FAILED(chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, flags)))
                    return false;
                pendingResize = false;
                acquired = false;
                return WrapBuffers();
            }

            bool WrapBuffers() {
                textures.resize(imageCount);
                for (uint32_t i = 0; i < imageCount; i++) {
                    winrt::com_ptr<ID3D12Resource> res;
                    if (FAILED(chain->GetBuffer(i, winrt::guid_of<ID3D12Resource>(), res.put_void())))
                        return false;
                    TextureDesc td;
                    td.width = width;
                    td.height = height;
                    td.format = GetFormat();
                    td.usage = TextureUsage::ColorTarget | (desc.allowSampling ? TextureUsage::Sampled : TextureUsage::None) | TextureUsage::CopyDst;
                    textures[i] = device.WrapSwapchainImage(res.get(), td);
                }
                return true;
            }

            void ReleaseBuffers() {
                for (TextureHandle& t : textures)
                    if (t)
                        device.DestroyWrapped(t);
                textures.clear();
            }

            Device& device;
            SwapchainDesc desc;
            HWND hwnd = nullptr;
            winrt::com_ptr<IDXGISwapChain3> chain;
            winrt::handle waitable;
            std::vector<TextureHandle> textures;
            DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
            UINT flags = 0;
            uint32_t width = 0, height = 0, imageCount = 3;
            PresentMode presentMode = PresentMode::Fifo;
            bool tearing = false, hdr = false, acquired = false, pendingResize = false;
        };
    }

    std::unique_ptr<rhi::Swapchain> MakeSwapchain(Device& device, const SwapchainDesc& desc) {
        auto sc = std::make_unique<SwapchainImpl>(device, desc);
        if (!sc->Init())
            return nullptr;
        return sc;
    }
}
