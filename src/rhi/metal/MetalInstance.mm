#include "rhi/Backends.hpp"
#include "rhi/metal/MetalInternal.hpp"

#import <CoreGraphics/CoreGraphics.h>
#include <cstdio>

namespace rhi::metal {
    namespace {
        class SwapchainImpl final : public Swapchain {
        public:
            SwapchainImpl(Device& device, const SwapchainDesc& desc)
                : device(device), desc(desc), width(desc.width), height(desc.height), presentMode(desc.present), hdr(desc.hdr) {}

            ~SwapchainImpl() override {
                device.WaitIdle();
                drawable = nil;
                if (handle)
                    device.Destroy(handle);
            }

            bool Init() {
                id object = (__bridge id)desc.window.window;
                if ([object isKindOfClass:[CAMetalLayer class]]) {
                    layer = object;
                } else if ([object respondsToSelector:NSSelectorFromString([NSString stringWithUTF8String:"layer"])]) {
                    // an NSView / UIView whose backing layer is the metal layer
                    id backing = [object valueForKey:[NSString stringWithUTF8String:"layer"]];
                    if ([backing isKindOfClass:[CAMetalLayer class]])
                        layer = backing;
                }
                if (!layer)
                    return false;
                format = hdr ? Format::RGBA16Float : Format::BGRA8Unorm;
                layer.device = device.Dev();
                layer.pixelFormat = hdr ? MTLPixelFormatRGBA16Float : MTLPixelFormatBGRA8Unorm;
                layer.framebufferOnly = !desc.allowSampling;
                layer.maximumDrawableCount = std::clamp(desc.imageCount, 2u, 3u);
#if TARGET_OS_OSX
                if (hdr) {
                    layer.wantsExtendedDynamicRangeContent = YES;
                    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearDisplayP3);
                    layer.colorspace = space;
                    CGColorSpaceRelease(space);
                }
#endif
                if (!width || !height) {
                    const CGSize size = layer.drawableSize;
                    width = std::max(1u, static_cast<uint32_t>(size.width));
                    height = std::max(1u, static_cast<uint32_t>(size.height));
                }
                layer.drawableSize = CGSizeMake(width, height);
                SetPresentMode(presentMode);
                handle = device.CreateSurfaceTexture(format, width, height, desc.allowSampling);
                return handle.value != 0;
            }

            bool Acquire(Image& out) override {
                if (!width || !height || !handle)
                    return false;
                drawable = [layer nextDrawable];
                if (!drawable)
                    return false;
                device.SetSurfaceTexture(handle, drawable.texture);
                out.texture = handle;
                out.index = counter++ % ImageCount();
                return true;
            }

            // presented from a buffer queued behind the frame: the queue is in order, so the image is finished by then
            void Present() override {
                if (!drawable)
                    return;
                id<MTLCommandBuffer> cb = [device.Queue() commandBuffer];
                [cb presentDrawable:drawable];
                [cb commit];
                drawable = nil;
            }

            bool Resize(uint32_t w, uint32_t h) override {
                width = w;
                height = h;
                if (!w || !h)
                    return true;
                drawable = nil;
                layer.drawableSize = CGSizeMake(w, h);
                device.ResizeSurfaceTexture(handle, w, h);
                return true;
            }

            void SetPresentMode(PresentMode mode) override {
                presentMode = mode;
#if TARGET_OS_OSX
                layer.displaySyncEnabled = mode != PresentMode::Immediate;
#endif
            }

            Format GetFormat() const override { return format; }
            uint32_t Width() const override { return width; }
            uint32_t Height() const override { return height; }
            PresentMode GetPresentMode() const override { return presentMode; }
            uint32_t ImageCount() const override { return static_cast<uint32_t>(layer.maximumDrawableCount); }
            bool IsHdr() const override { return hdr; }

        private:
            Device& device;
            SwapchainDesc desc;
            CAMetalLayer* layer = nil;
            id<CAMetalDrawable> drawable = nil;
            TextureHandle handle;
            uint32_t width, height;
            PresentMode presentMode;
            bool hdr;
            Format format = Format::BGRA8Unorm;
            uint32_t counter = 0;
        };

        uint32_t VendorOf(NSString* name) {
            NSString* n = [name lowercaseString];
            if ([n containsString:@"apple"]) return vendor_id::kApple;
            if ([n containsString:@"amd"] || [n containsString:@"radeon"]) return vendor_id::kAmd;
            if ([n containsString:@"nvidia"] || [n containsString:@"geforce"]) return vendor_id::kNvidia;
            if ([n containsString:@"intel"]) return vendor_id::kIntel;
            return 0;
        }

        class MetalInstance final : public Instance {
        public:
            explicit MetalInstance(const InstanceOptions& options) : options(options) {}

            bool Init() {
                NSArray<id<MTLDevice>>* all = nil;
#if TARGET_OS_OSX
                all = MTLCopyAllDevices();
#else
                id<MTLDevice> d = MTLCreateSystemDefaultDevice();
                all = d ? [NSArray arrayWithObjects:&d count:1] : [NSArray array];
#endif
                // the system default (the gpu driving the display) goes first
                id<MTLDevice> preferred = MTLCreateSystemDefaultDevice();
                NSMutableArray<id<MTLDevice>>* ordered = [NSMutableArray array];
                if (preferred)
                    [ordered addObject:preferred];
                for (id<MTLDevice> d in all)
                    if (d != preferred)
                        [ordered addObject:d];
                for (id<MTLDevice> d in ordered) {
                    AdapterInfo a;
                    a.api = Api::Metal;
                    a.index = static_cast<uint32_t>(adapters.size());
                    a.name = [d.name UTF8String];
                    a.vendorId = VendorOf(d.name);
                    a.dedicatedVram = d.hasUnifiedMemory ? 0 : d.recommendedMaxWorkingSetSize;
                    a.sharedMemory = d.hasUnifiedMemory ? d.recommendedMaxWorkingSetSize : 0;
#if TARGET_OS_OSX
                    a.type = d.hasUnifiedMemory || d.lowPower ? AdapterType::Integrated : AdapterType::Discrete;
                    if (d.isRemovable)
                        a.type = AdapterType::Discrete;
#else
                    a.type = AdapterType::Integrated;
#endif
                    a.apiVersion = 0;
                    for (const MTLGPUFamily fam : {MTLGPUFamilyApple9, MTLGPUFamilyApple8, MTLGPUFamilyApple7, MTLGPUFamilyApple6, MTLGPUFamilyApple5,
                                                   MTLGPUFamilyApple4, MTLGPUFamilyApple3})
                        if ([d supportsFamily:fam]) {
                            a.apiVersion = static_cast<uint32_t>(fam);
                            break;
                        }
                    // base instance and the indirect draws the renderer uses start at these families
                    if (![d supportsFamily:MTLGPUFamilyApple3] && ![d supportsFamily:MTLGPUFamilyMac2]) {
                        a.usable = false;
                        a.unusableReason = "needs an Apple3 or Mac2 class gpu";
                    }
                    adapters.push_back(std::move(a));
                    native.push_back(d);
                }
                return !adapters.empty();
            }

            Api GetApi() const override { return Api::Metal; }
            const std::vector<AdapterInfo>& Adapters() const override { return adapters; }

            std::unique_ptr<rhi::Device> CreateDevice(const DeviceOptions& deviceOptions) override {
                if (deviceOptions.adapterIndex >= adapters.size() || !adapters[deviceOptions.adapterIndex].usable)
                    return nullptr;
                auto device = std::make_unique<Device>(native[deviceOptions.adapterIndex], deviceOptions, adapters[deviceOptions.adapterIndex]);
                if (!device->Init()) {
                    std::fprintf(stderr, "[metal] device creation failed\n");
                    return nullptr;
                }
                return device;
            }

        private:
            InstanceOptions options;
            std::vector<AdapterInfo> adapters;
            std::vector<id<MTLDevice>> native;
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
    std::unique_ptr<Instance> CreateMetalInstance(const InstanceOptions& options) {
        auto instance = std::make_unique<metal::MetalInstance>(options);
        if (!instance->Init())
            return nullptr;
        return instance;
    }
}
