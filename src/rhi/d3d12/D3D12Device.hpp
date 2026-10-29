#pragma once
#include "rhi/Device.hpp"
#include "rhi/common/HandlePool.hpp"
#include "rhi/common/RetireQueue.hpp"
#include "rhi/common/StateValidator.hpp"
#include "rhi/common/Util.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_6.h>
#include <directx/d3d12.h>

#if __has_include(<winrt/base.h>)
#define WINRT_LEAN_AND_MEAN
#include <winrt/base.h>
#else
#include "rhi/d3d12/WinrtShim.hpp"
#endif

#include "D3D12MemAlloc.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rhi::d3d12 {
    struct Library {
        HMODULE d3d12 = nullptr;
        HMODULE dxgi = nullptr;
        PFN_D3D12_CREATE_DEVICE createDevice = nullptr;
        PFN_D3D12_GET_DEBUG_INTERFACE getDebugInterface = nullptr;
        PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE serializeRootSignature = nullptr;
        HRESULT(WINAPI* createFactory2)(UINT, REFIID, void**) = nullptr;
    };

    struct InstanceState {
        Library lib;
        winrt::com_ptr<IDXGIFactory4> factory;
        bool validation = false;
        bool tearing = false;
        std::mutex messageMutex;
        std::vector<std::string> messages;
        size_t errorCount = 0;
    };

    // cpu-only heaps for render and depth views
    class CpuDescriptorPool {
    public:
        bool Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity);
        uint32_t Alloc();
        void Free(uint32_t index);
        D3D12_CPU_DESCRIPTOR_HANDLE Handle(uint32_t index) const { return {cpuStart.ptr + static_cast<SIZE_T>(index) * increment}; }

    private:
        winrt::com_ptr<ID3D12DescriptorHeap> heap;
        D3D12_CPU_DESCRIPTOR_HANDLE cpuStart{};
        uint32_t increment = 0;
        uint32_t capacity = 0;
        uint32_t next = 0;
        std::vector<uint32_t> freeList;
        std::mutex mutex;
    };

    // shader-visible heap handing out contiguous ranges; freed ranges merge with their neighbours
    class RangeHeap {
    public:
        bool Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity);
        // returns false when no range of that size is free
        bool Alloc(uint32_t count, uint32_t& offset);
        void Free(uint32_t offset, uint32_t count);
        D3D12_CPU_DESCRIPTOR_HANDLE Cpu(uint32_t index) const { return {cpuStart.ptr + static_cast<SIZE_T>(index) * increment}; }
        D3D12_GPU_DESCRIPTOR_HANDLE Gpu(uint32_t index) const { return {gpuStart.ptr + static_cast<UINT64>(index) * increment}; }
        ID3D12DescriptorHeap* Heap() const { return heap.get(); }
        uint32_t Used() const { return used; }

    private:
        winrt::com_ptr<ID3D12DescriptorHeap> heap;
        D3D12_CPU_DESCRIPTOR_HANDLE cpuStart{};
        D3D12_GPU_DESCRIPTOR_HANDLE gpuStart{};
        uint32_t increment = 0;
        uint32_t used = 0;
        std::vector<std::pair<uint32_t, uint32_t>> freeRanges;
        std::mutex mutex;
    };

    struct BufferRec {
        winrt::com_ptr<ID3D12Resource> resource;
        winrt::com_ptr<D3D12MA::Allocation> alloc;
        BufferDesc desc;
        std::string name;
        uint8_t* mapped = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS address = 0;
        // state as of the last recorded barrier (recording order is submission order)
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    };

    struct ViewSet {
        std::unordered_map<uint64_t, uint32_t> rtv;
        std::unordered_map<uint64_t, uint32_t> dsv;
    };

    struct TextureRec {
        winrt::com_ptr<ID3D12Resource> resource;
        winrt::com_ptr<D3D12MA::Allocation> alloc;
        TextureDesc desc;
        std::string name;
        DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
        uint32_t planes = 1;
        uint32_t mips = 1;
        uint32_t layers = 1;
        bool external = false;
        // shared across queues without barriers (crossQueue): stays in common
        bool simultaneous = false;
        std::vector<D3D12_RESOURCE_STATES> states;
        ViewSet views;
    };

    struct SamplerRec {
        SamplerDesc desc;
    };

    struct ShaderRec {
        std::vector<uint8_t> code;
        ShaderStage stage = ShaderStage::Vertex;
    };

    struct BindLayoutRec {
        std::vector<BindLayoutEntry> entries;
        // offset of each entry inside the group's cbv/srv/uav or sampler table (parallel to entries)
        std::vector<uint32_t> tableOffset;
        uint32_t tableCount = 0;
        uint32_t samplerCount = 0;
        uint32_t dynamicCount = 0;
        // binding numbers of the dynamic uniform buffers, ascending: the order SetBindGroup offsets arrive in
        std::vector<uint32_t> dynamicBindings;
    };

    struct SamplerTable {
        uint32_t offset = 0;
        uint32_t count = 0;
        uint32_t refs = 0;
        std::vector<uint32_t> key;
    };

    struct BindGroupRec {
        BindLayoutHandle layout;
        uint32_t tableOffset = 0;
        uint32_t tableCount = 0;
        int samplerTable = -1;
        uint32_t samplerOffset = 0;
        bool perFrame = false;
        uint32_t dynamicCount = 0;
        // dynamic uniform buffers in binding order: root cbv address base
        std::array<D3D12_GPU_VIRTUAL_ADDRESS, kMaxDynamicOffsets> dynamicBase{};
        std::vector<StateValidator::BoundResource> bound;
    };

    struct PipelineLayoutRec {
        winrt::com_ptr<ID3D12RootSignature> rootSignature;
        uint64_t hash = 0;
        int pushParam = -1;
        uint32_t pushBytes = 0;
        std::array<BindLayoutHandle, kMaxBindGroups> sets{};
        std::array<int, kMaxBindGroups> tableParam{};
        std::array<int, kMaxBindGroups> samplerParam{};
        std::array<std::array<int, kMaxDynamicOffsets>, kMaxBindGroups> dynamicParam{};
    };

    struct PipelineRec {
        winrt::com_ptr<ID3D12PipelineState> pso;
        winrt::com_ptr<ID3D12RootSignature> rootSignature;
        PipelineLayoutHandle layoutHandle;
        bool compute = false;
        D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        std::array<uint32_t, kMaxVertexStreams> strides{};
        uint32_t streamCount = 0;
        uint32_t colorCount = 0;
        std::array<Format, kMaxColorTargets> colorFormats{};
        Format depthFormat = Format::Unknown;
        uint32_t pushBytes = 0;
    };

    struct QueryRec {
        winrt::com_ptr<ID3D12QueryHeap> heap;
        uint32_t count = 0;
    };

    struct FenceRec {
        winrt::com_ptr<ID3D12Fence> fence;
    };

    struct QueueInfo {
        winrt::com_ptr<ID3D12CommandQueue> queue;
        winrt::com_ptr<ID3D12Fence> fence;
        std::atomic<uint64_t> serial{0};
        std::mutex mutex;
    };

    class Device;

    class CmdList final : public CommandList {
    public:
        CmdList(Device& device, QueueType queue, winrt::com_ptr<ID3D12CommandAllocator> allocator, winrt::com_ptr<ID3D12GraphicsCommandList> list)
            : device(device), queue(queue), allocator(std::move(allocator)), list(std::move(list)) {}

        QueueType Queue() const override { return queue; }
        void Begin();
        void End();
        ID3D12GraphicsCommandList* Native() const { return list.get(); }
        ID3D12CommandAllocator* Allocator() const { return allocator.get(); }

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
        void* NativeHandle() override { return list.get(); }
        void InvalidateBoundState() override;

        int openLabels = 0;

    private:
        void FlushBindings();
        void Fail(const std::string& message);
        void Transition(ID3D12Resource* resource, UINT subresource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

        Device& device;
        QueueType queue;
        winrt::com_ptr<ID3D12CommandAllocator> allocator;
        winrt::com_ptr<ID3D12GraphicsCommandList> list;

        PipelineRec pipeline;
        PipelineLayoutHandle layoutHandle;
        bool havePipeline = false;
        bool inRendering = false;
        std::array<BindGroupHandle, kMaxBindGroups> groups{};
        std::array<std::array<uint32_t, kMaxDynamicOffsets>, kMaxBindGroups> dynamicOffsets{};
        std::array<uint32_t, kMaxBindGroups> dynamicCounts{};
        uint32_t dirtySets = 0;
        std::array<uint32_t, kMaxPushConstantBytes / 4> push{};
        uint32_t pushUsed = 0;
        bool pushDirty = false;
        std::array<D3D12_VERTEX_BUFFER_VIEW, kMaxVertexStreams> vbuffers{};
        uint32_t vertexMask = 0;
        uint32_t vertexDirty = 0;
        bool haveIndex = false;
        D3D12_INDEX_BUFFER_VIEW indexView{};
        std::array<Format, kMaxColorTargets> colorFormats{};
        uint32_t colorCount = 0;
        Format depthFormat = Format::Unknown;
        struct ResolvePair {
            TextureHandle src, dst;
        };
        std::vector<ResolvePair> pendingResolves;
        bool heapsSet = false;
        bool recording = false;
        bool invalid = false;
        uint32_t targetWidth = 0, targetHeight = 0;
        uint32_t stencilRef = 0;
        ID3D12RootSignature* boundRoot = nullptr;
        bool boundCompute = false;
        D3D12_VIEWPORT viewport{};
        D3D12_RECT scissor{};
        bool haveViewport = false, haveScissor = false;
        // fill sources and similar scratch: held until the list is reused, after its frame slot completed
        std::vector<std::pair<winrt::com_ptr<ID3D12Resource>, winrt::com_ptr<D3D12MA::Allocation>>> temps;
    };

    struct ThreadPools {
        struct PerQueue {
            std::vector<std::unique_ptr<CmdList>> lists;
            size_t used = 0;
        };
        std::vector<std::array<PerQueue, kQueueCount>> slots;
    };

    class Device final : public rhi::Device {
    public:
        Device(std::shared_ptr<InstanceState> inst, winrt::com_ptr<IDXGIAdapter1> adapter, const AdapterInfo& info, const DeviceOptions& options);
        ~Device() override;
        bool Init(std::string& error);

        Api GetApi() const override { return Api::D3D12; }
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
        void Flush(BufferHandle, uint64_t, uint64_t) override {}
        void Invalidate(BufferHandle, uint64_t, uint64_t) override {}
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
        TextureHandle WrapSwapchainImage(ID3D12Resource* resource, const TextureDesc& desc);
        void DestroyWrapped(TextureHandle h);
        D3D12_CPU_DESCRIPTOR_HANDLE RenderView(TextureRec& tex, uint32_t mip, uint32_t layer, bool depth, bool readOnly = false);
        ID3D12CommandSignature* Signature(bool dispatch, uint32_t stride);
        void Retire(std::function<void()> fn);
        void ReportError(const std::string& message);
        QueueSerials Completed();
        void CheckRemoved(HRESULT hr, const char* what);
        void SignalSerial(QueueType q);

        winrt::com_ptr<ID3D12Device> device;
        winrt::com_ptr<ID3D12Device4> device4;
        winrt::com_ptr<IDXGIAdapter1> adapter;
        winrt::com_ptr<D3D12MA::Allocator> allocator;
        std::shared_ptr<InstanceState> inst;
        DeviceCaps caps;
        DeviceOptions options;
        StateValidator validator;
        bool useValidator = false;
        bool uma = false;

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
        std::atomic<uint64_t> drawCount{0}, dispatchCount{0}, indirectCount{0}, barrierCount{0}, pipelineBindCount{0},
            groupBindCount{0}, submitCount{0};
        std::atomic<int64_t> bufferBytes{0}, textureBytes{0};
        std::atomic<int32_t> liveBuffers{0}, liveTextures{0}, livePipelines{0}, liveGroups{0};

        CpuDescriptorPool rtvPool, dsvPool;
        RangeHeap resourceHeap, samplerHeap;
        std::mutex viewMutex;

    private:
        friend class CmdList;

        ThreadPools& PoolsForThisThread();
        void ResetSlot(uint32_t s);
        void FillCaps();
        void WriteSamplerDesc(D3D12_CPU_DESCRIPTOR_HANDLE dst, const SamplerDesc& d);
        int AcquireSamplerTable(const std::vector<uint32_t>& key, const std::vector<SamplerDesc>& descs);
        void ReleaseSamplerTable(int index);

        std::mutex samplerMutex;
        std::vector<std::pair<SamplerDesc, SamplerHandle>> samplerCache;
        std::vector<SamplerTable> samplerTables;

        std::mutex signatureMutex;
        std::unordered_map<uint64_t, winrt::com_ptr<ID3D12CommandSignature>> signatures;

        winrt::com_ptr<ID3D12PipelineLibrary> pipelineLibrary;
        std::mutex libraryMutex;

        std::mutex poolsMutex;
        std::unordered_map<std::thread::id, std::unique_ptr<ThreadPools>> threadPools;

        struct FrameGroups {
            std::vector<BindGroupHandle> groups;
        };
        std::vector<FrameGroups> frameGroups;
        std::mutex frameGroupMutex;

        RetireQueue retire;
        uint64_t frame = 0;
        uint32_t slot = 0;
        std::vector<QueueSerials> slotSerials;
        winrt::com_ptr<ID3D12InfoQueue> infoQueue;
        winrt::handle waitEvent;
        std::mutex waitMutex;
    };

    std::unique_ptr<rhi::Swapchain> MakeSwapchain(Device& device, const SwapchainDesc& desc);
}
