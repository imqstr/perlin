#pragma once
// webgpu backend: one queue, implicit synchronisation. Barriers are no-ops (the api orders everything), fences are
// emulated from queue completion callbacks, and cpu-visible buffers are shadow copies pushed with queue writes.
#include "rhi/Backends.hpp"
#include "rhi/common/HandlePool.hpp"
#include "rhi/common/ShaderReflect.hpp"
#include "rhi/common/Util.hpp"

#include <webgpu/webgpu.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rhi::webgpu {
    inline WGPUStringView View(const char* s) { return WGPUStringView{s, s ? WGPU_STRLEN : 0}; }
    inline WGPUStringView View(const std::string& s) { return WGPUStringView{s.data(), s.size()}; }
    inline std::string Str(WGPUStringView v) {
        if (!v.data)
            return {};
        return std::string(v.data, v.length == WGPU_STRLEN ? std::char_traits<char>::length(v.data) : v.length);
    }

    inline void Release(WGPUBuffer v) { wgpuBufferRelease(v); }
    inline void Release(WGPUTexture v) { wgpuTextureRelease(v); }
    inline void Release(WGPUTextureView v) { wgpuTextureViewRelease(v); }
    inline void Release(WGPUSampler v) { wgpuSamplerRelease(v); }
    inline void Release(WGPUShaderModule v) { wgpuShaderModuleRelease(v); }
    inline void Release(WGPUBindGroupLayout v) { wgpuBindGroupLayoutRelease(v); }
    inline void Release(WGPUBindGroup v) { wgpuBindGroupRelease(v); }
    inline void Release(WGPUPipelineLayout v) { wgpuPipelineLayoutRelease(v); }
    inline void Release(WGPURenderPipeline v) { wgpuRenderPipelineRelease(v); }
    inline void Release(WGPUComputePipeline v) { wgpuComputePipelineRelease(v); }
    inline void Release(WGPUQuerySet v) { wgpuQuerySetRelease(v); }
    inline void Release(WGPUCommandEncoder v) { wgpuCommandEncoderRelease(v); }
    inline void Release(WGPUCommandBuffer v) { wgpuCommandBufferRelease(v); }
    inline void Release(WGPURenderPassEncoder v) { wgpuRenderPassEncoderRelease(v); }
    inline void Release(WGPUComputePassEncoder v) { wgpuComputePassEncoderRelease(v); }
    inline void Release(WGPUQueue v) { wgpuQueueRelease(v); }
    inline void Release(WGPUDevice v) { wgpuDeviceRelease(v); }
    inline void Release(WGPUAdapter v) { wgpuAdapterRelease(v); }
    inline void Release(WGPUInstance v) { wgpuInstanceRelease(v); }
    inline void Release(WGPUSurface v) { wgpuSurfaceRelease(v); }

    // owns one reference to a native object
    template <class T>
    class Owned {
    public:
        Owned() = default;
        explicit Owned(T object) : object(object) {}
        Owned(Owned&& o) noexcept : object(o.object) { o.object = nullptr; }
        Owned& operator=(Owned&& o) noexcept {
            if (this != &o) {
                Reset();
                object = o.object;
                o.object = nullptr;
            }
            return *this;
        }
        Owned(const Owned&) = delete;
        Owned& operator=(const Owned&) = delete;
        ~Owned() { Reset(); }

        T Get() const { return object; }
        explicit operator bool() const { return object != nullptr; }
        void Reset() {
            if (object) {
                Release(object);
                object = nullptr;
            }
        }
        T Detach() {
            T t = object;
            object = nullptr;
            return t;
        }

    private:
        T object = nullptr;
    };

    // ---- conversions (WebGpuConvert.cpp)
    WGPUTextureFormat ToTextureFormat(Format format);
    Format FromTextureFormat(WGPUTextureFormat format);
    WGPUVertexFormat ToVertexFormat(Format format);
    WGPUBlendFactor ToBlendFactor(BlendFactor f);
    WGPUBlendOperation ToBlendOp(BlendOp op);
    WGPUCompareFunction ToCompare(CompareOp op);
    WGPUStencilOperation ToStencilOp(StencilOp op);
    WGPUAddressMode ToAddress(AddressMode mode);
    WGPUFilterMode ToFilter(Filter f);
    WGPUMipmapFilterMode ToMipFilter(Filter f);
    WGPUTextureViewDimension ToViewDimension(TextureDim dim);

    struct Buffer {
        Owned<WGPUBuffer> buffer;
        BufferDesc desc;
        std::string name;
        // cpu copy of upload / readback / mappable buffers: Map() hands this out, Flush() pushes it with a queue write
        std::vector<uint8_t> shadow;
        uint64_t gpuSize = 0;
    };

    struct ViewKey {
        uint32_t baseMip = 0, mipCount = 0, baseLayer = 0, layerCount = 0;
        uint16_t dim = 0;
        uint16_t format = 0;
        uint8_t aspect = 0;
        auto operator<=>(const ViewKey&) const = default;
    };

    struct Texture {
        Owned<WGPUTexture> texture;
        // swapchain images are borrowed from the surface and replaced every frame
        bool external = false;
        TextureDesc desc;
        std::string name;
        std::map<ViewKey, Owned<WGPUTextureView>> views;
        std::mutex viewMutex;

        Texture() = default;
        Texture(Texture&& o) noexcept
            : texture(std::move(o.texture)), external(o.external), desc(o.desc), name(std::move(o.name)), views(std::move(o.views)) {
            desc.name = nullptr;
        }
        Texture& operator=(Texture&& o) noexcept {
            texture = std::move(o.texture);
            external = o.external;
            desc = o.desc;
            name = std::move(o.name);
            views = std::move(o.views);
            return *this;
        }
    };

    struct Sampler {
        Owned<WGPUSampler> sampler;
        SamplerDesc desc;
        bool linear = false;
    };

    struct Shader {
        ShaderDesc desc;
        std::string source;
        std::string entry;
        Owned<WGPUShaderModule> module;
        ShaderInterface iface;
    };

    struct BindLayout {
        std::vector<BindLayoutEntry> entries;
    };

    struct BindGroup {
        BindLayoutHandle layout;
        std::vector<BindGroupEntry> entries;
        // native groups, one per native layout this group has been bound with
        std::map<WGPUBindGroupLayout, Owned<WGPUBindGroup>> native;
    };

    struct PipelineLayout {
        PipelineLayoutDesc desc;
    };

    // the layout a pipeline really uses for one set: the rhi layout cut down to what its shaders reference
    struct NativeSet {
        Owned<WGPUBindGroupLayout> layout;
        struct Entry {
            BindLayoutEntry rhi;
            bool depth = false;
            bool comparison = false;
        };
        std::vector<Entry> entries;
        // positions in the rhi layout's dynamic offset list that survive
        std::vector<uint32_t> dynamicPick;
        uint32_t rhiDynamicCount = 0;
        // bound where the caller supplied no group but the pipeline still needs the slot filled
        Owned<WGPUBindGroup> blank;
    };

    struct Pipeline {
        bool compute = false;
        Owned<WGPURenderPipeline> render;
        Owned<WGPUComputePipeline> comp;
        PipelineLayoutHandle layoutHandle;
        Owned<WGPUPipelineLayout> layout;
        std::array<std::shared_ptr<NativeSet>, kMaxBindGroups> sets;
        uint32_t streamCount = 0;
        uint32_t setCount = 0;
    };

    struct Query {
        Owned<WGPUQuerySet> set;
        uint32_t count = 0;
    };

    // shared with in-flight completion callbacks, which can outlive the handle
    struct Fence {
        std::shared_ptr<std::atomic<uint64_t>> value = std::make_shared<std::atomic<uint64_t>>(0);
    };

    class WebGpuDevice;

    class WebGpuCommandList final : public CommandList {
    public:
        WebGpuCommandList(WebGpuDevice& device, QueueType queue) : device(device), queue(queue) {}
        ~WebGpuCommandList() override;

        QueueType Queue() const override { return queue; }
        // starts recording; the previous contents are dropped
        void Begin();
        // closes the encoder; null when nothing was recorded
        WGPUCommandBuffer Finish();

        void Barrier(std::span<const TextureBarrier> textures, std::span<const BufferBarrier> buffers) override;
        void UavBarrier(BufferHandle) override;
        void UavBarrier(TextureHandle) override;
        void CopyBuffer(BufferHandle dst, uint64_t dstOffset, BufferHandle src, uint64_t srcOffset, uint64_t size) override;
        void CopyBufferToTexture(TextureHandle dst, BufferHandle src, std::span<const BufferTextureCopy> regions) override;
        void CopyTextureToBuffer(BufferHandle dst, TextureHandle src, std::span<const BufferTextureCopy> regions) override;
        void CopyTexture(TextureHandle dst, TextureHandle src, std::span<const TextureCopy> regions) override;
        void FillBuffer(BufferHandle dst, uint64_t offset, uint64_t size, uint32_t value) override;
        void ResolveTexture(TextureHandle dst, TextureHandle src) override;
        void BeginRendering(const RenderingDesc& desc) override;
        void EndRendering() override;
        void SetPipeline(PipelineHandle pipeline) override;
        void SetBindGroup(uint32_t set, BindGroupHandle group, std::span<const uint32_t> dynamicOffsets) override;
        void PushConstants(const void* data, uint32_t bytes, uint32_t offset) override;
        void SetViewport(const Viewport& viewport) override;
        void SetScissor(const Rect& rect) override;
        void SetVertexBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset) override;
        void SetIndexBuffer(BufferHandle buffer, IndexType type, uint64_t offset) override;
        void SetStencilReference(uint32_t value) override;
        void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override;
        void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) override;
        void DrawIndexedIndirect(BufferHandle args, uint64_t offset, uint32_t maxDraws, uint32_t stride) override;
        void DrawIndexedIndirectCount(BufferHandle args, uint64_t offset, BufferHandle countBuffer, uint64_t countOffset, uint32_t maxDraws,
                                      uint32_t stride) override;
        void Dispatch(uint32_t x, uint32_t y, uint32_t z) override;
        void DispatchIndirect(BufferHandle args, uint64_t offset) override;
        void WriteTimestamp(QuerySetHandle set, uint32_t index) override;
        void ResolveQueries(QuerySetHandle set, uint32_t first, uint32_t count, BufferHandle dst, uint64_t dstOffset) override;
        void BeginDebugLabel(const char* name, uint32_t rgba) override;
        void EndDebugLabel() override;
        void* NativeHandle() override { return encoder.Get(); }
        void InvalidateBoundState() override { dirtySets = 0xF; }

    private:
        void EndPass();
        void FlushBindings();
        bool PrepareDraw();

        WebGpuDevice& device;
        QueueType queue;
        Owned<WGPUCommandEncoder> encoder;
        Owned<WGPURenderPassEncoder> render;
        Owned<WGPUComputePassEncoder> compute;
        PipelineHandle pipeline;
        struct BoundSet {
            BindGroupHandle group;
            std::array<uint32_t, kMaxDynamicOffsets> offsets{};
            uint32_t offsetCount = 0;
        };
        std::array<BoundSet, kMaxBindGroups> sets{};
        uint32_t dirtySets = 0;
        bool pipelineDirty = true;
        struct VertexSlot {
            BufferHandle buffer;
            uint64_t offset = 0;
        };
        std::array<VertexSlot, kMaxVertexStreams> vertex{};
        uint32_t vertexDirty = 0;
        BufferHandle indexBuffer;
        IndexType indexType = IndexType::U32;
        uint64_t indexOffset = 0;
        bool indexDirty = false;
        Viewport viewport;
        bool viewportSet = false, viewportDirty = false;
        Rect scissor;
        bool scissorSet = false, scissorDirty = false;
        uint32_t stencilRef = 0;
        bool stencilDirty = false;
        uint32_t passWidth = 0, passHeight = 0;
        bool warnedPush = false;
    };

    class WebGpuDevice final : public Device {
    public:
        WebGpuDevice(Owned<WGPUInstance> instance, Owned<WGPUAdapter> adapter, Owned<WGPUDevice> device, const DeviceOptions& options,
                     const AdapterInfo& info, bool mapSync);
        ~WebGpuDevice() override;

        bool Init();

        Api GetApi() const override { return Api::WebGPU; }
        const DeviceCaps& Caps() const override { return caps; }
        NativeHandles Native() const override;

        BufferHandle CreateBuffer(const BufferDesc& desc) override;
        TextureHandle CreateTexture(const TextureDesc& desc) override;
        SamplerHandle CreateSampler(const SamplerDesc& desc) override;
        ShaderHandle CreateShader(const ShaderDesc& desc) override;
        BindLayoutHandle CreateBindLayout(const BindLayoutDesc& desc) override;
        BindGroupHandle CreateBindGroup(const BindGroupDesc& desc) override;
        PipelineLayoutHandle CreatePipelineLayout(const PipelineLayoutDesc& desc) override;
        PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
        PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) override;
        QuerySetHandle CreateQuerySet(const QuerySetDesc& desc) override;

        void Destroy(BufferHandle h) override;
        void Destroy(TextureHandle h) override;
        void Destroy(SamplerHandle h) override;
        void Destroy(ShaderHandle h) override;
        void Destroy(BindLayoutHandle h) override;
        void Destroy(BindGroupHandle h) override;
        void Destroy(PipelineLayoutHandle h) override;
        void Destroy(PipelineHandle h) override;
        void Destroy(QuerySetHandle h) override;
        void Destroy(FenceHandle h) override;

        const BufferDesc* GetDesc(BufferHandle h) const override;
        const TextureDesc* GetDesc(TextureHandle h) const override;

        uint8_t* Map(BufferHandle buffer) override;
        void Flush(BufferHandle buffer, uint64_t offset, uint64_t size) override;
        void Invalidate(BufferHandle buffer, uint64_t offset, uint64_t size) override;
        uint64_t GpuAddress(BufferHandle) const override { return 0; }

        FenceHandle CreateFence(uint64_t initialValue) override;
        uint64_t FenceValue(FenceHandle fence) override;
        bool WaitFence(FenceHandle fence, uint64_t value, uint64_t timeoutNs) override;
        void SignalFence(FenceHandle fence, uint64_t value) override;

        CommandList* AcquireCommandList(QueueType queue) override;
        void Submit(QueueType queue, const SubmitDesc& desc) override;
        std::unique_ptr<Swapchain> CreateSwapchain(const SwapchainDesc& desc) override;

        void BeginFrame() override;
        void EndFrame() override;
        uint64_t FrameNumber() const override { return frame; }
        uint32_t FrameSlot() const override { return static_cast<uint32_t>(frame % (std::max)(1u, options.framesInFlight)); }
        void WaitIdle() override;

        MemoryBudget QueryMemoryBudget() override;
        DeviceStats Stats() const override { return stats; }
        float TimestampPeriodNs() const override { return 1.0f; }

        // ---- internals shared with the command list and swapchain
        WGPUDevice Dev() const { return device.Get(); }
        WGPUQueue Queue() const { return queue.Get(); }
        WGPUInstance Inst() const { return instance.Get(); }
        WGPUAdapter Adapter() const { return adapter.Get(); }
        const DeviceOptions& Options() const { return options; }
        DeviceStats& MutableStats() { return stats; }
        // blocks (native) or just services callbacks (web) until `done` is true or the timeout passes
        bool Pump(const std::function<bool()>& done, uint64_t timeoutNs);
        void ReportError(const std::string& message);

        // attachment views must cover every aspect of a depth-stencil format; sampled views pick one
        WGPUTextureView ViewOf(Texture& texture, const SubresourceRange& range, TextureDim dim, Format format, bool stencil, bool attachment);
        WGPUBindGroup NativeGroupFor(BindGroupHandle group, const NativeSet& set);
        std::shared_ptr<NativeSet> BuildNativeSet(const PipelineLayoutDesc& layout, uint32_t set, std::span<const Shader* const> shaders,
                                                  ShaderStage stagesPresent);
        WGPUSampler NearestSampler() const { return nearestSampler.Get(); }
        WGPUBindGroup EmptyGroup() const { return emptyGroup.Get(); }
        void SetSurfaceTexture(TextureHandle handle, WGPUTexture texture);
        TextureHandle CreateSurfaceTexture(Format format, uint32_t width, uint32_t height, bool sampled);
        void ResizeSurfaceTexture(TextureHandle handle, uint32_t width, uint32_t height);

        HandlePool<BufferTag, Buffer> buffers;
        HandlePool<TextureTag, Texture> textures;
        HandlePool<SamplerTag, Sampler> samplers;
        HandlePool<ShaderTag, Shader> shaders;
        HandlePool<BindLayoutTag, BindLayout> bindLayouts;
        HandlePool<BindGroupTag, BindGroup> bindGroups;
        HandlePool<PipelineLayoutTag, PipelineLayout> pipelineLayouts;
        HandlePool<PipelineTag, Pipeline> pipelines;
        HandlePool<QuerySetTag, Query> queries;
        HandlePool<FenceTag, Fence> fences;

    private:
        friend class WebGpuCommandList;
        void FillCaps();
        std::shared_ptr<NativeSet> CachedLayout(const std::string& key, const std::function<std::shared_ptr<NativeSet>()>& make);

        Owned<WGPUInstance> instance;
        Owned<WGPUAdapter> adapter;
        Owned<WGPUDevice> device;
        Owned<WGPUQueue> queue;
        Owned<WGPUSampler> nearestSampler;
        Owned<WGPUBindGroupLayout> emptyLayout;
        Owned<WGPUBindGroup> emptyGroup;
        DeviceOptions options;
        DeviceCaps caps;
        DeviceStats stats;
        bool mapSync = true;
        uint64_t frame = 0;
        FenceHandle frameFence;
        std::atomic<uint64_t> submittedFrames{0};
        std::vector<std::unique_ptr<WebGpuCommandList>> lists;
        std::vector<WebGpuCommandList*> freeLists;
        std::mutex listMutex;
        std::mutex layoutMutex;
        std::unordered_map<std::string, std::shared_ptr<NativeSet>> layoutCache;
        std::mutex errorMutex;
        std::string firstError;
        bool hasFeatureTimestamp = false;
    };

    // creates the instance for the platform: native adapters through wgpu-native, the browser's device on the web
    std::unique_ptr<Instance> MakeInstance(const InstanceOptions& options);
}
