#pragma once
// direct3d 11 backend: the fallback on windows. One immediate context orders everything, so barriers are statistics only.
// Recording goes to deferred contexts and replays at Submit; cpu visible buffers keep a shadow that Flush pushes with
// UpdateSubresource. Shaders arrive as slang's hlsl and are compiled with d3dcompiler after their registers are remapped.
#include "rhi/Device.hpp"
#include "rhi/common/HandlePool.hpp"
#include "rhi/common/ShaderReflect.hpp"
#include "rhi/common/Util.hpp"
#include "rhi/d3d11/D3D11Convert.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcommon.h>
#include <dxgi1_6.h>

#if __has_include(<winrt/base.h>)
#define WINRT_LEAN_AND_MEAN
#include <winrt/base.h>
#else
#include "rhi/d3d12/WinrtShim.hpp"
#endif

#include <array>
#include <deque>
#include <map>
#include <memory>
#include <tuple>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rhi::d3d11 {
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

    struct Library {
        HMODULE d3d11 = nullptr;
        HMODULE dxgi = nullptr;
        HMODULE compiler = nullptr;
        PFN_D3D11_CREATE_DEVICE createDevice = nullptr;
        HRESULT(WINAPI* createFactory1)(REFIID, void**) = nullptr;
        CompileFn compile = nullptr;
    };

    struct InstanceState {
        Library lib;
        winrt::com_ptr<IDXGIFactory2> factory;
        bool validation = false;
        bool tearing = false;
    };

    // a texture readback waiting for the data to be copied into a readback buffer's shadow
    struct PendingReadback {
        winrt::com_ptr<ID3D11Texture2D> staging;
        uint64_t bufferOffset = 0;
        uint32_t bufferRowPitch = 0;
        uint32_t rowBytes = 0;
        uint32_t rows = 0;
    };

    struct Buffer {
        // everything but constant binding; the target of copies
        winrt::com_ptr<ID3D11Buffer> main;
        // d3d11 refuses constant buffers that are also vertex / index / storage: uniform data is mirrored here
        winrt::com_ptr<ID3D11Buffer> constant;
        BufferDesc desc;
        std::string name;
        std::vector<uint8_t> shadow;
        uint64_t gpuSize = 0;
        bool structured = false;
        std::map<std::tuple<uint64_t, uint64_t, uint8_t>, winrt::com_ptr<ID3D11View>> views;
        std::vector<PendingReadback> pending;
    };

    struct ViewKey {
        uint8_t kind = 0;
        uint8_t dim = 0;
        uint16_t format = 0;
        uint32_t baseMip = 0, mipCount = 0, baseLayer = 0, layerCount = 0;
        auto operator<=>(const ViewKey&) const = default;
    };

    struct Texture {
        winrt::com_ptr<ID3D11Resource> resource;
        TextureDesc desc;
        std::string name;
        FormatMap formats;
        // swapchain images are re-wrapped on resize
        bool external = false;
        std::map<ViewKey, winrt::com_ptr<ID3D11View>> views;
    };

    struct Sampler {
        winrt::com_ptr<ID3D11SamplerState> state;
        SamplerDesc desc;
    };

    struct Shader {
        ShaderStage stage = ShaderStage::Vertex;
        std::string source;
        std::string entry;
        ShaderInterface iface;
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

    // one resource of a stage: where it lives in the rhi layout and which register it was remapped to
    struct StageSlot {
        uint32_t set = 0;
        uint32_t binding = 0;
        ResourceKind kind = ResourceKind::UniformBuffer;
        TextureDim dim = TextureDim::Tex2D;
        char registerClass = 't';
        uint32_t slot = 0;
        // index into the dynamic offsets of SetBindGroup, ~0 for a static binding
        uint32_t dynamicIndex = ~0u;
    };

    enum StageIndex : uint32_t { kStageVertex = 0, kStagePixel = 1, kStageCompute = 2 };

    struct Pipeline {
        bool compute = false;
        winrt::com_ptr<ID3D11VertexShader> vs;
        winrt::com_ptr<ID3D11PixelShader> ps;
        winrt::com_ptr<ID3D11ComputeShader> cs;
        winrt::com_ptr<ID3D11InputLayout> inputLayout;
        winrt::com_ptr<ID3D11RasterizerState> raster;
        winrt::com_ptr<ID3D11BlendState> blend;
        winrt::com_ptr<ID3D11DepthStencilState> depthStencil;
        D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        std::array<UINT, kMaxVertexStreams> strides{};
        uint32_t streamCount = 0;
        std::array<std::vector<StageSlot>, 3> stages;
    };

    struct Query {
        uint32_t count = 0;
    };

    struct Fence {
        uint64_t value = 0;
        std::deque<std::pair<uint64_t, winrt::com_ptr<ID3D11Query>>> pending;
    };

    class Device;

    class CmdList final : public rhi::CommandList {
    public:
        CmdList(Device& device, QueueType queue);
        ~CmdList() override;

        bool Init();
        QueueType Queue() const override { return queue; }
        void Begin();
        // null when nothing was recorded
        ID3D11CommandList* Finish();
        // a recorded list keeps its resources alive; swapchain buffers cannot be resized while one does
        void ReleaseFinished() { finished = nullptr; }

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
        void* NativeHandle() override { return ctx.get(); }
        void InvalidateBoundState() override { dirtySets = 0xF; pipelineDirty = true; vertexDirty = 0xFF; indexDirty = true; }

    private:
        bool PrepareDraw();
        bool BindSets(const Pipeline& p);
        void ClearBindings();
        void UnbindTargets();
        void CopyToConstant(Buffer& b, uint64_t offset, uint64_t size);

        Device& device;
        QueueType queue;
        winrt::com_ptr<ID3D11DeviceContext> ctx;
        winrt::com_ptr<ID3D11DeviceContext1> ctx1;
        winrt::com_ptr<ID3D11CommandList> finished;
        bool recorded = false;

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
        Rect scissor;
        bool viewportSet = false, scissorSet = false;
        uint32_t stencilRef = 0;
        uint32_t passWidth = 0, passHeight = 0;
        bool inPass = false;
        // slots the last draw bound, cleared before targets change so the runtime never has to force them to null
        std::array<uint32_t, 3> boundSrvs{};
        uint32_t boundUavs = 0;
        struct Resolve {
            TextureHandle dst, src;
        };
        std::vector<Resolve> resolves;
        // per stage and register: the range a dynamic constant buffer binding selected, copied out when the runtime cannot offset
        winrt::com_ptr<ID3D11Buffer> mirrors[3][14];
    };

    class Device final : public rhi::Device {
    public:
        Device(std::shared_ptr<InstanceState> instance, winrt::com_ptr<IDXGIAdapter1> adapter, const AdapterInfo& info, const DeviceOptions& options);
        ~Device() override;

        bool Init(std::string& error);

        Api GetApi() const override { return Api::D3D11; }
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
        uint32_t FrameSlot() const override { return static_cast<uint32_t>(frame % std::max(1u, options.framesInFlight)); }
        void WaitIdle() override;

        MemoryBudget QueryMemoryBudget() override;
        DeviceStats Stats() const override { return stats; }
        float TimestampPeriodNs() const override { return 1.0f; }

        // ---- internals shared with the command list and the swapchain
        ID3D11Device* Dev() const { return device.get(); }
        ID3D11Device1* Dev1() const { return device1.get(); }
        ID3D11DeviceContext* Imm() const { return immediate.get(); }
        InstanceState& Inst() const { return *inst; }
        const DeviceOptions& Options() const { return options; }
        DeviceStats& MutableStats() { return stats; }
        bool ConstantOffsetting() const { return constantOffsetting; }
        void ReportError(const std::string& message);
        std::unique_lock<std::mutex> LockContext() { return std::unique_lock<std::mutex>(ctxMutex); }

        ID3D11ShaderResourceView* SrvOf(Texture& texture, const SubresourceRange& range, TextureDim dim, Format format, bool stencil);
        ID3D11UnorderedAccessView* UavOf(Texture& texture, const SubresourceRange& range, TextureDim dim);
        ID3D11RenderTargetView* RtvOf(Texture& texture, uint32_t mip, uint32_t layer);
        ID3D11DepthStencilView* DsvOf(Texture& texture, uint32_t mip, uint32_t layer, bool readOnly);
        ID3D11ShaderResourceView* BufferSrv(Buffer& buffer, uint64_t offset, uint64_t size);
        ID3D11UnorderedAccessView* BufferUav(Buffer& buffer, uint64_t offset, uint64_t size);
        // wraps a swapchain buffer; SetBackbuffer swaps the resource after a resize
        TextureHandle WrapBackbuffer(ID3D11Texture2D* texture, const TextureDesc& desc);
        void SetBackbuffer(TextureHandle handle, ID3D11Texture2D* texture);
        void ReleaseBackbuffer(TextureHandle handle);

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
        friend class CmdList;
        void FillCaps();
        bool CompileStage(const Shader& shader, const PipelineLayoutDesc& layout, uint32_t stageIndex, std::vector<StageSlot>& slots,
                          winrt::com_ptr<ID3DBlob>& blob, std::string& error);
        void PollFence(Fence& fence);
        void MarkSubmitted(FenceHandle fence, uint64_t value);

        std::shared_ptr<InstanceState> inst;
        winrt::com_ptr<IDXGIAdapter1> adapter;
        winrt::com_ptr<ID3D11Device> device;
        winrt::com_ptr<ID3D11Device1> device1;
        winrt::com_ptr<ID3D11DeviceContext> immediate;
        DeviceOptions options;
        DeviceCaps caps;
        DeviceStats stats;
        bool constantOffsetting = false;
        uint64_t frame = 0;
        FenceHandle frameFence;

        // the immediate context is not thread safe: Flush, Submit, queries and Present all go through this
        std::mutex ctxMutex;
        std::mutex viewMutex;
        std::mutex fenceMutex;
        std::mutex listMutex;
        std::mutex shaderCacheMutex;
        std::vector<std::unique_ptr<CmdList>> lists;
        std::vector<CmdList*> freeLists;
        std::vector<winrt::com_ptr<ID3D11Query>> freeQueries;
        std::unordered_map<std::string, winrt::com_ptr<ID3DBlob>> blobCache;
        std::mutex errorMutex;
        std::vector<uint8_t> zeros;
    };

    std::unique_ptr<rhi::Swapchain> MakeSwapchain(Device& device, const SwapchainDesc& desc);
}
