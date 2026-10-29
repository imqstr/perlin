#pragma once
#include "rhi/Types.hpp"

#include <functional>
#include <memory>
#include <optional>

namespace rhi {
    enum class AdapterType : uint8_t { Unknown, Discrete, Integrated, Virtual, Cpu };

    namespace vendor_id {
        constexpr uint32_t kNvidia = 0x10DE;
        constexpr uint32_t kAmd = 0x1002;
        constexpr uint32_t kIntel = 0x8086;
        constexpr uint32_t kApple = 0x106B;
        constexpr uint32_t kQualcomm = 0x5143;
        constexpr uint32_t kArm = 0x13B5;
        constexpr uint32_t kMicrosoft = 0x1414;
    }

    const char* VendorName(uint32_t vendorId);

    struct AdapterInfo {
        Api api = Api::None;
        uint32_t index = 0;
        std::string name;
        uint32_t vendorId = 0;
        uint32_t deviceId = 0;
        AdapterType type = AdapterType::Unknown;
        uint64_t dedicatedVram = 0;
        uint64_t sharedMemory = 0;
        uint64_t driverVersion = 0;
        std::string driverName;
        // vulkan: packed VK_API_VERSION; d3d12: feature level (0xc100 = 12_1); metal: gpu family
        uint32_t apiVersion = 0;
        uint64_t luid = 0;
        // false when the adapter lacks something the renderer requires; reason says what
        bool usable = true;
        std::string unusableReason;
    };

    enum class FormatUse : uint32_t {
        None = 0,
        Sampled = 1u << 0,
        SampledLinear = 1u << 1,
        ColorTarget = 1u << 2,
        Blendable = 1u << 3,
        DepthTarget = 1u << 4,
        Storage = 1u << 5,
        Msaa = 1u << 6,
    };
    constexpr FormatUse operator|(FormatUse a, FormatUse b) {
        return static_cast<FormatUse>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
    constexpr bool Has(FormatUse v, FormatUse bit) {
        return (static_cast<uint32_t>(v) & static_cast<uint32_t>(bit)) != 0;
    }

    struct DeviceFeatures {
        bool multiDrawIndirect = false;
        // count read from a buffer: lets a culling shader decide how many draws run
        bool drawIndirectCount = false;
        bool drawIndirectFirstInstance = false;
        bool asyncCompute = false;
        bool dedicatedCopyQueue = false;
        bool timestampQueries = false;
        bool pipelineCacheBlob = false;
        bool resizableBar = false;
        bool unifiedMemory = false;
        bool textureCompressionBC = false;
        bool samplerAnisotropy = false;
        bool shaderFloat16 = false;
        bool shaderInt64 = false;
        bool subgroupOps = false;
        bool meshShaders = false;
        bool rayTracing = false;
        bool variableRateShading = false;
        // matrix-vector hardware for neural shading (vk_nv_cooperative_vector, d3d12 sm6.9)
        bool cooperativeVector = false;
        bool cooperativeMatrix = false;
        bool tearing = false;
        bool hdrOutput = false;
        bool enhancedBarriers = false;
        bool bindless = false;
        bool conservativeRaster = false;
        bool depthClipControl = false;
        bool bufferDeviceAddress = false;
    };

    struct DeviceLimits {
        uint32_t maxTexture2D = 4096;
        uint32_t maxTextureLayers = 256;
        uint32_t maxBindGroups = 4;
        uint32_t maxPushConstantBytes = 128;
        uint32_t maxSampledTexturesPerStage = 16;
        uint32_t maxColorTargets = 8;
        uint32_t maxMsaaSamples = 1;
        uint32_t minSubgroupSize = 0;
        uint32_t maxSubgroupSize = 0;
        uint32_t uniformBufferAlignment = 256;
        uint32_t storageBufferAlignment = 256;
        uint32_t copyRowPitchAlignment = 256;
        uint32_t copyOffsetAlignment = 512;
        uint64_t maxBufferSize = 1ull << 30;
        float maxAnisotropy = 1.0f;
        float timestampPeriodNs = 1.0f;
    };

    struct DeviceCaps {
        AdapterInfo adapter;
        DeviceFeatures features;
        DeviceLimits limits;
        std::array<FormatUse, static_cast<size_t>(Format::Count)> formats{};
        FormatUse FormatSupport(Format f) const { return formats[static_cast<size_t>(f)]; }
    };

    // optional hooks for vendor sdks that wrap or extend api creation (frame generation layers, ngx)
    struct VendorHooks {
        void* vkGetInstanceProcAddr = nullptr;
        void* d3d12CreateDevice = nullptr;
        void* dxgiCreateFactory = nullptr;
        std::vector<const char*> instanceExtensions;
        std::vector<const char*> deviceExtensions;
        // chained into VkDeviceCreateInfo after the renderer's own features
        void* deviceFeatureChain = nullptr;
    };

    struct InstanceOptions {
        bool validation = false;
        const char* appName = "Perlin";
        VendorHooks hooks;
    };

    struct DeviceOptions {
        uint32_t adapterIndex = 0;
        bool validation = false;
        // frames the cpu may run ahead of the gpu
        uint32_t framesInFlight = 2;
        // 0 = off; rhi state tracking checks barriers against what the previous use left
        bool stateValidation = false;
        // serialised pipeline cache from a previous run
        std::span<const uint8_t> pipelineCacheData;
        bool allowAsyncCompute = true;
        VendorHooks hooks;
    };

    class Swapchain {
    public:
        virtual ~Swapchain() = default;

        struct Image {
            TextureHandle texture;
            uint32_t index = 0;
        };

        // false while minimised or out of date: skip the frame
        virtual bool Acquire(Image& out) = 0;
        virtual void Present() = 0;
        virtual bool Resize(uint32_t width, uint32_t height) = 0;
        virtual void SetPresentMode(PresentMode mode) = 0;
        virtual Format GetFormat() const = 0;
        virtual uint32_t Width() const = 0;
        virtual uint32_t Height() const = 0;
        virtual PresentMode GetPresentMode() const = 0;
        virtual uint32_t ImageCount() const = 0;
        virtual bool IsHdr() const { return false; }
    };

    class CommandList {
    public:
        virtual ~CommandList() = default;

        virtual QueueType Queue() const = 0;

        virtual void Barrier(std::span<const TextureBarrier> textures, std::span<const BufferBarrier> buffers) = 0;
        void Barrier(const TextureBarrier& t) { Barrier(std::span<const TextureBarrier>(&t, 1), {}); }
        void Barrier(const BufferBarrier& b) { Barrier({}, std::span<const BufferBarrier>(&b, 1)); }
        // make writes visible to later uses of the same resource in the same state (uav / storage hazards)
        virtual void UavBarrier(BufferHandle buffer) = 0;
        virtual void UavBarrier(TextureHandle texture) = 0;

        virtual void CopyBuffer(BufferHandle dst, uint64_t dstOffset, BufferHandle src, uint64_t srcOffset, uint64_t size) = 0;
        virtual void CopyBufferToTexture(TextureHandle dst, BufferHandle src, std::span<const BufferTextureCopy> regions) = 0;
        virtual void CopyTextureToBuffer(BufferHandle dst, TextureHandle src, std::span<const BufferTextureCopy> regions) = 0;
        virtual void CopyTexture(TextureHandle dst, TextureHandle src, std::span<const TextureCopy> regions) = 0;
        virtual void FillBuffer(BufferHandle dst, uint64_t offset, uint64_t size, uint32_t value) = 0;
        virtual void ResolveTexture(TextureHandle dst, TextureHandle src) = 0;

        virtual void BeginRendering(const RenderingDesc& desc) = 0;
        virtual void EndRendering() = 0;

        virtual void SetPipeline(PipelineHandle pipeline) = 0;
        virtual void SetBindGroup(uint32_t set, BindGroupHandle group, std::span<const uint32_t> dynamicOffsets = {}) = 0;
        virtual void PushConstants(const void* data, uint32_t bytes, uint32_t offset = 0) = 0;
        virtual void SetViewport(const Viewport& viewport) = 0;
        virtual void SetScissor(const Rect& rect) = 0;
        virtual void SetVertexBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset = 0) = 0;
        virtual void SetIndexBuffer(BufferHandle buffer, IndexType type, uint64_t offset = 0) = 0;
        virtual void SetStencilReference(uint32_t value) = 0;

        virtual void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) = 0;
        virtual void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset,
                                 uint32_t firstInstance) = 0;
        // maxDraws commands of DrawIndexedIndirectCommand at stride bytes apart
        virtual void DrawIndexedIndirect(BufferHandle args, uint64_t offset, uint32_t maxDraws, uint32_t stride) = 0;
        // draw count read from countBuffer at countOffset, clamped to maxDraws
        virtual void DrawIndexedIndirectCount(BufferHandle args, uint64_t offset, BufferHandle countBuffer,
                                              uint64_t countOffset, uint32_t maxDraws, uint32_t stride) = 0;

        virtual void Dispatch(uint32_t x, uint32_t y, uint32_t z) = 0;
        virtual void DispatchIndirect(BufferHandle args, uint64_t offset) = 0;

        virtual void WriteTimestamp(QuerySetHandle set, uint32_t index) = 0;
        virtual void ResolveQueries(QuerySetHandle set, uint32_t first, uint32_t count, BufferHandle dst, uint64_t dstOffset) = 0;

        virtual void BeginDebugLabel(const char* name, uint32_t rgba = 0xFFFFFFFFu) = 0;
        virtual void EndDebugLabel() = 0;

        // vulkan: VkCommandBuffer, d3d12: ID3D12GraphicsCommandList*, d3d11: ID3D11DeviceContext*
        virtual void* NativeHandle() = 0;
        // call after vendor code recorded into the native list: it may have changed any bound state
        virtual void InvalidateBoundState() = 0;
    };

    struct FenceOp {
        FenceHandle fence;
        uint64_t value = 0;
    };

    struct SubmitDesc {
        std::span<CommandList* const> lists;
        std::span<const FenceOp> waits;
        std::span<const FenceOp> signals;
    };

    class Device {
    public:
        virtual ~Device() = default;

        virtual Api GetApi() const = 0;
        virtual const DeviceCaps& Caps() const = 0;
        virtual NativeHandles Native() const = 0;
        // native object of a texture plus its api state when it is in `state`; empty for a stale handle or a backend without one
        virtual NativeTexture NativeOf(TextureHandle texture, State state) { (void)texture; (void)state; return {}; }

        virtual BufferHandle CreateBuffer(const BufferDesc& desc) = 0;
        virtual TextureHandle CreateTexture(const TextureDesc& desc) = 0;
        virtual SamplerHandle CreateSampler(const SamplerDesc& desc) = 0;
        virtual ShaderHandle CreateShader(const ShaderDesc& desc) = 0;
        virtual BindLayoutHandle CreateBindLayout(const BindLayoutDesc& desc) = 0;
        virtual BindGroupHandle CreateBindGroup(const BindGroupDesc& desc) = 0;
        virtual PipelineLayoutHandle CreatePipelineLayout(const PipelineLayoutDesc& desc) = 0;
        virtual PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
        virtual PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) = 0;
        virtual QuerySetHandle CreateQuerySet(const QuerySetDesc& desc) = 0;

        // destruction waits for every queue's work submitted so far; safe to call with live references in flight
        virtual void Destroy(BufferHandle h) = 0;
        virtual void Destroy(TextureHandle h) = 0;
        virtual void Destroy(SamplerHandle h) = 0;
        virtual void Destroy(ShaderHandle h) = 0;
        virtual void Destroy(BindLayoutHandle h) = 0;
        virtual void Destroy(BindGroupHandle h) = 0;
        virtual void Destroy(PipelineLayoutHandle h) = 0;
        virtual void Destroy(PipelineHandle h) = 0;
        virtual void Destroy(QuerySetHandle h) = 0;

        virtual const BufferDesc* GetDesc(BufferHandle h) const = 0;
        virtual const TextureDesc* GetDesc(TextureHandle h) const = 0;

        // persistent mapping; null for GpuOnly buffers
        virtual uint8_t* Map(BufferHandle buffer) = 0;
        virtual void Flush(BufferHandle buffer, uint64_t offset, uint64_t size) = 0;
        virtual void Invalidate(BufferHandle buffer, uint64_t offset, uint64_t size) = 0;
        virtual uint64_t GpuAddress(BufferHandle buffer) const = 0;

        virtual FenceHandle CreateFence(uint64_t initialValue = 0) = 0;
        virtual void Destroy(FenceHandle h) = 0;
        virtual uint64_t FenceValue(FenceHandle fence) = 0;
        virtual bool WaitFence(FenceHandle fence, uint64_t value, uint64_t timeoutNs = ~0ull) = 0;
        virtual void SignalFence(FenceHandle fence, uint64_t value) = 0;

        // pooled: valid until passed to Submit, then recycled when the work completes
        virtual CommandList* AcquireCommandList(QueueType queue) = 0;
        virtual void Submit(QueueType queue, const SubmitDesc& desc) = 0;
        void Submit(QueueType queue, CommandList* list) { Submit(queue, SubmitDesc{std::span<CommandList* const>(&list, 1), {}, {}}); }

        virtual std::unique_ptr<Swapchain> CreateSwapchain(const SwapchainDesc& desc) = 0;

        // pacing: blocks until the frame that used these slots framesInFlight ago has finished
        virtual void BeginFrame() = 0;
        virtual void EndFrame() = 0;
        virtual uint64_t FrameNumber() const = 0;
        virtual uint32_t FrameSlot() const = 0;
        virtual void WaitIdle() = 0;

        virtual MemoryBudget QueryMemoryBudget() = 0;
        virtual DeviceStats Stats() const = 0;
        virtual std::vector<uint8_t> SerializePipelineCache() { return {}; }
        virtual std::string DebugReport() { return {}; }

        // total gpu time between two timestamp indices, nanoseconds; valid once their frame retired
        virtual float TimestampPeriodNs() const = 0;
    };

    class Instance {
    public:
        virtual ~Instance() = default;
        virtual Api GetApi() const = 0;
        virtual const std::vector<AdapterInfo>& Adapters() const = 0;
        virtual std::unique_ptr<Device> CreateDevice(const DeviceOptions& options) = 0;
    };

    // null when the api is not compiled in or not present on this machine (no loader, no driver)
    std::unique_ptr<Instance> CreateInstance(Api api, const InstanceOptions& options = {});
    bool ApiCompiledIn(Api api);
    std::vector<Api> CompiledApis();
}
