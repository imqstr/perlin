#pragma once
// metal backend: one queue, implicit hazard tracking, so barriers are statistics only. Buffers the cpu touches are shared storage
// and need no flush; commands record straight into encoders and the encoder kind is switched lazily. Objective-C++ with ARC.
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "rhi/Device.hpp"
#include "rhi/common/HandlePool.hpp"
#include "rhi/common/ShaderReflect.hpp"
#include "rhi/common/Util.hpp"
#include "rhi/Backends.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rhi::metal {
    // d24s8 only exists on some macs: the device decides once and every conversion agrees
    extern bool gDepth24Stencil8;

    MTLPixelFormat ToPixelFormat(Format f);
    MTLVertexFormat ToVertexFormat(Format f);
    MTLCompareFunction ToCompare(CompareOp op);
    MTLStencilOperation ToStencilOp(StencilOp op);
    MTLBlendFactor ToBlendFactor(BlendFactor f);
    MTLBlendOperation ToBlendOp(BlendOp op);
    MTLSamplerAddressMode ToAddress(AddressMode m);
    MTLSamplerMinMagFilter ToMinMag(Filter f);
    MTLSamplerMipFilter ToMip(Filter f);
    MTLPrimitiveType ToPrimitive(Topology t);
    MTLTextureType ToTextureType(TextureDim dim, bool multisample, bool array);

    struct Buffer {
        id<MTLBuffer> buffer = nil;
        BufferDesc desc;
        std::string name;
        uint64_t size = 0;
        // null for private storage
        uint8_t* contents = nullptr;
    };

    struct ViewKey {
        uint8_t dim = 0;
        uint8_t stencil = 0;
        uint16_t format = 0;
        uint32_t baseMip = 0, mipCount = 0, baseLayer = 0, layerCount = 0;
        auto operator<=>(const ViewKey&) const = default;
    };

    struct Texture {
        id<MTLTexture> texture = nil;
        TextureDesc desc;
        std::string name;
        // swapchain images are replaced every frame
        bool external = false;
        std::map<ViewKey, id<MTLTexture>> views;
    };

    struct Sampler {
        id<MTLSamplerState> state = nil;
        SamplerDesc desc;
    };

    struct Shader {
        ShaderStage stage = ShaderStage::Vertex;
        std::string source;
        std::string entry;
        ShaderInterface iface;
        // workgroup size a compute shader declared, carried in a comment by the shader packer
        uint32_t threads[3] = {1, 1, 1};
    };

    struct BindLayout {
        std::vector<BindLayoutEntry> entries;
    };

    struct BindGroup {
        BindLayoutHandle layout;
        std::vector<BindGroupEntry> entries;
    };

    struct PipelineLayout {
        PipelineLayoutDesc desc;
    };

    struct StageSlot {
        uint32_t set = 0;
        uint32_t binding = 0;
        ResourceKind kind = ResourceKind::UniformBuffer;
        TextureDim dim = TextureDim::Tex2D;
        // argument table index in the msl
        uint32_t index = 0;
        uint32_t dynamicIndex = ~0u;
    };

    enum StageIndex : uint32_t { kStageVertex = 0, kStageFragment = 1, kStageCompute = 2 };

    // vertex streams live at the top of the buffer table so uniforms keep their low indices
    constexpr uint32_t kVertexBufferBase = 30;

    struct Pipeline {
        bool compute = false;
        id<MTLRenderPipelineState> render = nil;
        id<MTLComputePipelineState> comp = nil;
        id<MTLDepthStencilState> depthStencil = nil;
        MTLCullMode cull = MTLCullModeNone;
        MTLWinding winding = MTLWindingCounterClockwise;
        MTLTriangleFillMode fill = MTLTriangleFillModeFill;
        MTLDepthClipMode clip = MTLDepthClipModeClip;
        float biasConstant = 0, biasSlope = 0, biasClamp = 0;
        MTLPrimitiveType primitive = MTLPrimitiveTypeTriangle;
        uint32_t streamCount = 0;
        MTLSize threads = {1, 1, 1};
        std::array<std::vector<StageSlot>, 3> stages;
    };

    struct Query {
        uint32_t count = 0;
    };

    // completion handlers outlive the handle, so the counter is shared
    struct Fence {
        std::shared_ptr<std::atomic<uint64_t>> value = std::make_shared<std::atomic<uint64_t>>(0);
    };

    class Device;

    class CmdList final : public rhi::CommandList {
    public:
        CmdList(Device& device, QueueType queue) : device(device), queue(queue) {}
        ~CmdList() override;

        QueueType Queue() const override { return queue; }
        void Begin();
        // ends the open encoder and hands the command buffer over
        id<MTLCommandBuffer> Finish();

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
        void* NativeHandle() override { return (__bridge void*)commandBuffer; }
        void InvalidateBoundState() override { dirtySets = 0xF; pipelineDirty = true; vertexDirty = 0xFF; }

    private:
        void EndEncoder();
        id<MTLBlitCommandEncoder> Blit();
        id<MTLComputeCommandEncoder> Compute();
        bool PrepareDraw();
        bool BindSets(const Pipeline& p);
        void ApplyPipelineState(const Pipeline& p);

        Device& device;
        QueueType queue;
        id<MTLCommandBuffer> commandBuffer = nil;
        id<MTLRenderCommandEncoder> render = nil;
        id<MTLComputeCommandEncoder> compute = nil;
        id<MTLBlitCommandEncoder> blit = nil;

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
        Viewport viewport;
        Rect scissor;
        bool viewportSet = false, scissorSet = false;
        uint32_t stencilRef = 0;
        uint32_t passWidth = 0, passHeight = 0;
    };

    class Device final : public rhi::Device {
    public:
        Device(id<MTLDevice> dev, const DeviceOptions& options, const AdapterInfo& info);
        ~Device() override;

        bool Init();

        Api GetApi() const override { return Api::Metal; }
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
        void Flush(BufferHandle, uint64_t, uint64_t) override {}
        void Invalidate(BufferHandle, uint64_t, uint64_t) override {}
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

        // ---- internals shared with the command list and the swapchain
        id<MTLDevice> Dev() const { return device; }
        id<MTLCommandQueue> Queue() const { return queue; }
        const DeviceOptions& Options() const { return options; }
        DeviceStats& MutableStats() { return stats; }
        void ReportError(const std::string& message);
        id<MTLTexture> ViewOf(Texture& texture, const SubresourceRange& range, TextureDim dim, Format format, bool stencil);
        // swapchain images: a handle that points at a different drawable every frame
        TextureHandle CreateSurfaceTexture(Format format, uint32_t width, uint32_t height, bool sampled);
        void SetSurfaceTexture(TextureHandle handle, id<MTLTexture> texture);
        void ResizeSurfaceTexture(TextureHandle handle, uint32_t width, uint32_t height);
        // completion handlers raise a fence once everything committed before them has finished
        void SignalWhenDone(id<MTLCommandBuffer> after, FenceHandle fence, uint64_t value);

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

    public:
        std::mutex viewMutex;

    private:
        void FillCaps();
        id<MTLFunction> CompileStage(const Shader& shader, const PipelineLayoutDesc& layout, std::vector<StageSlot>& slots, std::string& error);

        id<MTLDevice> device = nil;
        id<MTLCommandQueue> queue = nil;
        DeviceOptions options;
        DeviceCaps caps;
        DeviceStats stats;
        uint64_t frame = 0;
        FenceHandle frameFence;
        std::mutex listMutex;
        std::vector<std::unique_ptr<CmdList>> lists;
        std::vector<CmdList*> freeLists;
        std::mutex libraryMutex;
        std::unordered_map<std::string, id<MTLLibrary>> libraries;
        std::mutex errorMutex;
    };

    std::unique_ptr<rhi::Swapchain> MakeSwapchain(Device& device, const SwapchainDesc& desc);
}
