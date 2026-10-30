#pragma once
#include "rhi/Device.hpp"
#include "rhi/common/HandlePool.hpp"
#include "rhi/common/RetireQueue.hpp"
#include "rhi/common/StateValidator.hpp"
#include "rhi/common/Util.hpp"

#include <volk.h>
#include <vk_mem_alloc.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rhi::vk {
    // keeps the VkInstance alive for devices created from it
    struct InstanceState {
        VkInstance instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
        uint32_t apiVersion = VK_API_VERSION_1_1;
        bool validation = false;
        bool debugUtils = false;
        bool surface = false;
        bool colorSpaceExt = false;
        bool surfaceCaps2 = false;
        std::vector<std::string> enabledExtensions;
        std::mutex messageMutex;
        std::vector<std::string> messages;
        size_t errorCount = 0;
        ~InstanceState();
    };

    struct PhysicalInfo {
        VkPhysicalDevice device = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties props{};
        std::vector<std::string> extensions;
        bool Has(const char* name) const;
    };

    struct BufferRec {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation alloc = nullptr;
        BufferDesc desc;
        std::string name;
        uint8_t* mapped = nullptr;
        VkDeviceAddress address = 0;
        bool coherent = true;
    };

    struct TextureRec {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation alloc = nullptr;
        TextureDesc desc;
        std::string name;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        VkImageUsageFlags usage = 0;
        VkImageCreateFlags createFlags = 0;
        uint32_t layers = 1;
        bool external = false;
        std::unordered_map<uint64_t, VkImageView> views;
    };

    struct SamplerRec {
        VkSampler sampler = VK_NULL_HANDLE;
        SamplerDesc desc;
    };

    struct ShaderRec {
        VkShaderModule module = VK_NULL_HANDLE;
        ShaderStage stage = ShaderStage::Vertex;
        std::string entry;
    };

    struct BindLayoutRec {
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        std::vector<BindLayoutEntry> entries;
        uint32_t dynamicCount = 0;
    };

    struct BindGroupRec {
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        BindLayoutHandle layout;
        bool perFrame = false;
        uint32_t dynamicCount = 0;
        // only filled while state validation is on
        std::vector<StateValidator::BoundResource> bound;
    };

    struct PipelineLayoutRec {
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkShaderStageFlags pushStages = 0;
        uint32_t pushBytes = 0;
        std::array<BindLayoutHandle, kMaxBindGroups> sets{};
    };

    struct PipelineRec {
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        PipelineLayoutHandle layoutHandle;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkShaderStageFlags pushStages = 0;
        uint32_t pushBytes = 0;
        uint32_t colorCount = 0;
        std::array<Format, kMaxColorTargets> colorFormats{};
        Format depthFormat = Format::Unknown;
        uint32_t streamCount = 0;
        bool compute = false;
    };

    struct QueryRec {
        VkQueryPool pool = VK_NULL_HANDLE;
        uint32_t count = 0;
    };

    struct FenceRec {
        VkSemaphore semaphore = VK_NULL_HANDLE;
    };

    struct QueueInfo {
        VkQueue queue = VK_NULL_HANDLE;
        uint32_t family = 0;
        std::mutex* mutex = nullptr;
        VkSemaphore timeline = VK_NULL_HANDLE;
        std::atomic<uint64_t> serial{0};
        bool shared = false;
    };

    class Device;

    class CmdList final : public CommandList {
    public:
        CmdList(Device& device, QueueType queue, VkCommandBuffer cmd) : device(device), queue(queue), cmd(cmd) {}

        QueueType Queue() const override { return queue; }
        void Begin();
        void End();
        VkCommandBuffer Cmd() const { return cmd; }

        void Barrier(std::span<const TextureBarrier> textures, std::span<const BufferBarrier> buffers) override;
        void UavBarrier(BufferHandle buffer) override;
        void UavBarrier(TextureHandle texture) override;
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
        void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset,
                         uint32_t firstInstance) override;
        void DrawIndexedIndirect(BufferHandle args, uint64_t offset, uint32_t maxDraws, uint32_t stride) override;
        void DrawIndexedIndirectCount(BufferHandle args, uint64_t offset, BufferHandle countBuffer, uint64_t countOffset,
                                      uint32_t maxDraws, uint32_t stride) override;
        void Dispatch(uint32_t x, uint32_t y, uint32_t z) override;
        void DispatchIndirect(BufferHandle args, uint64_t offset) override;
        void WriteTimestamp(QuerySetHandle set, uint32_t index) override;
        void ResolveQueries(QuerySetHandle set, uint32_t first, uint32_t count, BufferHandle dst, uint64_t dstOffset) override;
        void BeginDebugLabel(const char* name, uint32_t rgba) override;
        void EndDebugLabel() override;
        void* NativeHandle() override { return cmd; }
        void InvalidateBoundState() override;

        int openLabels = 0;

    private:
        void FlushBindings();
        void Fail(const std::string& message);

        Device& device;
        QueueType queue;
        VkCommandBuffer cmd;

        PipelineRec pipeline;
        bool havePipeline = false;
        bool inRendering = false;
        std::array<BindGroupHandle, kMaxBindGroups> groups{};
        std::array<std::array<uint32_t, kMaxDynamicOffsets>, kMaxBindGroups> dynamicOffsets{};
        std::array<uint32_t, kMaxBindGroups> dynamicCounts{};
        uint32_t dirtySets = 0;
        std::array<uint8_t, kMaxPushConstantBytes> push{};
        bool pushDirty = false;
        uint32_t pushUsed = 0;
        std::array<Format, kMaxColorTargets> colorFormats{};
        uint32_t colorCount = 0;
        Format depthFormat = Format::Unknown;
        PipelineHandle pipelineHandle;
        bool haveIndex = false;
        uint32_t vertexMask = 0;
        uint32_t targetWidth = 0, targetHeight = 0;
        // kept so InvalidateBoundState can restore what vendor code may have overwritten
        bool invalid = false;
        VkViewport viewport{};
        VkRect2D scissor{};
        bool haveViewport = false, haveScissor = false;
        std::array<VkBuffer, kMaxVertexStreams> vbuffers{};
        std::array<VkDeviceSize, kMaxVertexStreams> voffsets{};
        VkBuffer ibuffer = VK_NULL_HANDLE;
        VkDeviceSize ioffset = 0;
        VkIndexType itype = VK_INDEX_TYPE_UINT32;
        uint32_t stencilRef = 0;
    };

    struct ThreadPools {
        struct PerQueue {
            VkCommandPool pool = VK_NULL_HANDLE;
            std::vector<std::unique_ptr<CmdList>> lists;
            size_t used = 0;
        };
        // [slot][queue]
        std::vector<std::array<PerQueue, kQueueCount>> slots;
    };

    class Swapchain;

    class Device final : public rhi::Device {
    public:
        Device(std::shared_ptr<InstanceState> inst, const PhysicalInfo& phys, const AdapterInfo& adapter, const DeviceOptions& options);
        ~Device() override;
        bool Init(std::string& error);

        Api GetApi() const override { return Api::Vulkan; }
        const DeviceCaps& Caps() const override { return caps; }
        NativeHandles Native() const override;
        NativeTexture NativeOf(TextureHandle texture, State state) override;

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
        uint64_t GpuAddress(BufferHandle buffer) const override;

        FenceHandle CreateFence(uint64_t initialValue) override;
        uint64_t FenceValue(FenceHandle fence) override;
        bool WaitFence(FenceHandle fence, uint64_t value, uint64_t timeoutNs) override;
        void SignalFence(FenceHandle fence, uint64_t value) override;

        CommandList* AcquireCommandList(QueueType queue) override;
        void Submit(QueueType queue, const SubmitDesc& desc) override;
        std::unique_ptr<rhi::Swapchain> CreateSwapchain(const SwapchainDesc& desc) override;

        void BeginFrame() override;
        void EndFrame() override;
        uint64_t FrameNumber() const override { return frame; }
        uint32_t FrameSlot() const override { return slot; }
        void WaitIdle() override;

        MemoryBudget QueryMemoryBudget() override;
        DeviceStats Stats() const override;
        std::vector<uint8_t> SerializePipelineCache() override;
        std::string DebugReport() override;
        float TimestampPeriodNs() const override { return caps.limits.timestampPeriodNs; }

        // ---- internals shared with CmdList and Swapchain
        VkFormat FormatToVk(Format f) const;
        VkImageView View(TextureRec& tex, const SubresourceRange& range, TextureDim dim, Format format, bool stencil);
        VkImageView AttachmentView(TextureHandle h, uint32_t mip, uint32_t layer);
        TextureHandle WrapSwapchainImage(VkImage image, const TextureDesc& desc, VkFormat format);
        void DestroyWrapped(TextureHandle h);
        void NameObject(VkObjectType type, uint64_t handle, const char* name);
        void Retire(std::function<void()> fn);
        void ReportError(const std::string& message);
        QueueSerials Completed();
        void SetPendingAcquire(VkSemaphore s) { pendingAcquire = s; }
        void SubmitPresent(VkSemaphore renderDone);
        VkQueue PresentQueue() const { return queues[0].queue; }
        std::mutex* PresentMutex() const { return queues[0].mutex; }

        VolkDeviceTable fn{};
        VkDevice device = VK_NULL_HANDLE;
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        VmaAllocator allocator = nullptr;
        std::shared_ptr<InstanceState> inst;
        PhysicalInfo phys;
        DeviceCaps caps;
        DeviceOptions options;
        StateValidator validator;
        bool useValidator = false;

        bool dynamicRenderingExt = false;
        bool hostQueryReset = false;
        bool depthClipEnable = false;
        bool swapchainExt = false;
        bool d24Supported = true;
        VkPhysicalDeviceMemoryProperties memProps{};

        HandlePool<BufferTag, BufferRec> buffers;
        HandlePool<TextureTag, TextureRec> textures;
        HandlePool<SamplerTag, SamplerRec> samplers;
        HandlePool<ShaderTag, ShaderRec> shaders;
        HandlePool<BindLayoutTag, BindLayoutRec> bindLayouts;
        HandlePool<BindGroupTag, BindGroupRec> bindGroups;
        HandlePool<PipelineLayoutTag, PipelineLayoutRec> pipelineLayouts;
        HandlePool<PipelineTag, PipelineRec> pipelines;
        HandlePool<QuerySetTag, QueryRec> queries;
        HandlePool<FenceTag, FenceRec> fences;

        std::array<QueueInfo, kQueueCount> queues;
        mutable DeviceStats stats;
        std::atomic<uint64_t> drawCount{0}, dispatchCount{0}, indirectCount{0}, barrierCount{0}, pipelineBindCount{0},
            groupBindCount{0}, submitCount{0};
        std::atomic<int64_t> bufferBytes{0}, textureBytes{0};
        std::atomic<int32_t> liveBuffers{0}, liveTextures{0}, livePipelines{0}, liveGroups{0};

    private:
        friend class CmdList;
        friend class Swapchain;

        bool PickQueues(std::string& error);
        ThreadPools& PoolsForThisThread();
        void ResetSlot(uint32_t s);
        VkDescriptorPool NewDescriptorPool(bool freeable);
        VkDescriptorSet AllocateSet(VkDescriptorSetLayout layout, bool perFrame, VkDescriptorPool& poolOut);
        void FillCaps(const AdapterInfo& adapter);

        std::mutex viewMutex;
        std::mutex samplerMutex;
        std::vector<std::pair<SamplerDesc, SamplerHandle>> samplerCache;
        std::array<std::mutex, 3> queueMutexes;
        VkPipelineCache pipelineCache = VK_NULL_HANDLE;
        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        std::array<float, 8> queuePriorities{};
        std::array<uint32_t, kQueueCount> queueIndex{};
        VkDescriptorSetLayout emptySetLayout = VK_NULL_HANDLE;
        VkImageView MakeView(TextureRec& tex, const SubresourceRange& range, VkImageViewType type, VkFormat format, VkImageAspectFlags aspect);

        std::mutex descriptorMutex;
        std::vector<VkDescriptorPool> persistentPools;
        struct FrameDescriptors {
            std::vector<VkDescriptorPool> pools;
            size_t used = 0;
            std::vector<BindGroupHandle> groups;
        };
        std::vector<FrameDescriptors> frameDescriptors;

        std::mutex poolsMutex;
        std::unordered_map<std::thread::id, std::unique_ptr<ThreadPools>> threadPools;

        RetireQueue retire;
        uint64_t frame = 0;
        uint32_t slot = 0;
        // per frame slot: serial each queue reached while the slot was current
        std::vector<QueueSerials> slotSerials;
        std::atomic<VkSemaphore> pendingAcquire{VK_NULL_HANDLE};
        std::atomic<bool> lost{false};
    };

    std::unique_ptr<Instance> CreateInstance(const InstanceOptions& options);
    std::unique_ptr<rhi::Swapchain> MakeSwapchain(Device& device, const SwapchainDesc& desc);
}
