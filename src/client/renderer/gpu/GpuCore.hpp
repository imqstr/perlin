#pragma once
#include "rhi/Device.hpp"
#include "rhi/ShaderLibrary.hpp"
#include "client/renderer/RangeAllocator.hpp"

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace perlin::gpu {
    enum class VertexLayout : uint8_t { Terrain, Position, Color, Texture, Ui, Count };

    struct VertexLayoutInfo {
        uint32_t stride;
        std::array<rhi::VertexAttributeDesc, 4> attributes;
        uint32_t attributeCount;
    };
    const VertexLayoutInfo& LayoutInfo(VertexLayout layout);

    // everything that selects a pipeline; hashed and compared as raw fields
    struct PipelineKey {
        uint16_t program = 0;
        VertexLayout layout = VertexLayout::Terrain;
        rhi::CullMode cull = rhi::CullMode::None;
        rhi::FrontFace front = rhi::FrontFace::Clockwise;
        bool depthTest = false;
        bool depthWrite = false;
        rhi::CompareOp depthOp = rhi::CompareOp::LessEqual;
        bool blend = false;
        rhi::BlendFactor srcColor = rhi::BlendFactor::One, dstColor = rhi::BlendFactor::Zero;
        rhi::BlendFactor srcAlpha = rhi::BlendFactor::One, dstAlpha = rhi::BlendFactor::Zero;
        rhi::ColorMask mask = rhi::ColorMask::All;
        // min on alpha keeps the nearest depth when several layers write into one target
        rhi::BlendOp alphaOp = rhi::BlendOp::Add;
        bool alphaToCoverage = false;
        rhi::Format color = rhi::Format::RGBA8Unorm;
        rhi::Format depth = rhi::Format::D32Float;
        // fixed point (x100) so the key stays hashable
        int16_t biasConstant = 0;
        int16_t biasSlope = 0;
        // padding makes a raw compare unreliable, so keys are compared through their packed form
        std::array<uint64_t, 3> Pack() const;
        bool operator==(const PipelineKey& o) const { return Pack() == o.Pack(); }
    };

    struct PipelineKeyHash {
        size_t operator()(const PipelineKey& k) const;
    };

    enum class SamplerKind : uint8_t { PointClamp, PointRepeat, LinearClamp, LinearRepeat, LinearMipRepeat, PointMipClamp, Count };

    constexpr uint32_t kTextureSlots = 24;

    // what is bound at set 1; one entry per global texture slot
    struct TextureSet {
        std::array<rhi::TextureHandle, kTextureSlots> texture{};
        std::array<SamplerKind, kTextureSlots> sampler{};
        bool operator==(const TextureSet& o) const { return texture == o.texture && sampler == o.sampler; }
    };

    struct TextureSetHash {
        size_t operator()(const TextureSet& s) const;
    };

    // a view into the current frame's cpu-written gpu memory
    struct RingSlice {
        rhi::BufferHandle buffer;
        uint32_t chunk = 0;
        uint64_t offset = 0;
        uint8_t* data = nullptr;
    };

    // gpu-only buffer carved up by a range allocator; contents change through Gpu::Write
    class Arena {
    public:
        bool Create(rhi::Device& device, uint32_t elements, uint32_t elementBytes, rhi::BufferUsage usage, const char* name);
        void Destroy(rhi::Device& device);
        bool Alloc(uint32_t count, uint32_t& first) { return ranges.Alloc(count, first); }
        void Free(uint32_t first, uint32_t count) { ranges.Free(first, count); }
        bool Unused() const { return ranges.Unused(); }
        void Reset() { ranges.Reset(ranges.Capacity()); }
        uint32_t Capacity() const { return ranges.Capacity(); }
        uint32_t FreeTotal() const { return ranges.FreeTotal(); }
        rhi::BufferHandle Buffer() const { return buffer; }
        uint32_t ElementBytes() const { return elementBytes; }
        rhi::State BarrierState() const { return state; }
        uint64_t Bytes() const { return static_cast<uint64_t>(ranges.Capacity()) * elementBytes; }
        // forgets the buffer without destroying it (its device is already gone); ranges stay
        void Detach() { buffer = {}; }
        // new empty buffer of the same size on the current device; ranges stay
        bool Rebuild(rhi::Device& device);

    private:
        rhi::BufferUsage usage = rhi::BufferUsage::None;
        const char* name = nullptr;
        rhi::BufferHandle buffer;
        RangeAllocator ranges;
        uint32_t elementBytes = 0;
        rhi::State state = rhi::State::VertexBuffer;
    };

    struct GpuSettings {
        rhi::Api api = rhi::Api::Vulkan;
        uint32_t adapterIndex = 0;
        bool validation = false;
        rhi::NativeWindow window;
        uint32_t width = 0, height = 0;
        rhi::PresentMode present = rhi::PresentMode::Fifo;
        uint32_t framesInFlight = 2;
        bool hdr = false;
        // headless runs (tests, capture) render to an owned image instead of a window
        bool offscreen = false;
        rhi::VendorHooks hooks;
        std::span<const uint8_t> pipelineCache;
    };

    class Gpu {
    public:
        ~Gpu();

        bool Init(const GpuSettings& settings, std::span<const uint8_t> shaderPack, std::string* error);
        void Shutdown();

        rhi::Device& Dev() { return *device; }
        const rhi::DeviceCaps& Caps() const { return device->Caps(); }
        rhi::Api Api() const { return settings.api; }
        const GpuSettings& Settings() const { return settings; }
        rhi::Swapchain* Chain() { return swapchain.get(); }
        const rhi::ShaderLibrary& Library() const { return library; }
        uint32_t Frames() const { return settings.framesInFlight; }

        // -1 when the pack has no such program
        int Program(std::string_view name) const;
        const rhi::ProgramInfo& ProgramInfo(int program) const { return library.Programs()[static_cast<size_t>(program)]; }
        bool PipelineReady(const PipelineKey& key);
        rhi::PipelineHandle Pipeline(const PipelineKey& key);
        // drops compiled pipelines (device reset, pack reload)
        void ClearPipelines();

        // frame pacing; call once per frame before any ring use
        void BeginFrame();
        // false while minimised or out of date: skip the frame
        bool AcquireImage(rhi::Swapchain::Image& out);
        void PresentImage();
        // true when the window changed size or the surface was lost since the last call
        bool Resize(uint32_t width, uint32_t height);
        // the window surface was replaced (mobile resume, display change): rebuild the swapchain
        bool RecreateSurface(uint32_t width, uint32_t height);
        void SetWindow(const rhi::NativeWindow& window) { settings.window = window; }
        void SetPresentMode(rhi::PresentMode mode);
        rhi::Format TargetFormat() const { return targetFormat; }
        uint32_t TargetWidth() const { return targetWidth; }
        // frames in a row that found no image to draw to
        uint32_t AcquireFailures() const { return acquireFailures; }
        void ClearAcquireFailures() { acquireFailures = 0; }
        uint32_t TargetHeight() const { return targetHeight; }
        rhi::TextureHandle OffscreenTexture() const { return offscreen; }

        uint32_t FrameSlot() const { return device->FrameSlot(); }

        // per-frame upload memory (lives until the frame that wrote it retires)
        RingSlice Ring(uint64_t bytes, uint64_t align);
        rhi::BindGroupHandle UniformGroup(uint32_t chunk) const { return ringGroups[FrameSlot()][chunk]; }
        rhi::BufferHandle RingBuffer(uint32_t chunk) const { return rings[FrameSlot()][chunk].buffer; }
        uint32_t uniformRange() const { return uniformRangeBytes; }

        rhi::BindGroupHandle TextureGroup(const TextureSet& set);
        void ForgetTexture(rhi::TextureHandle texture);
        rhi::SamplerHandle Sampler(SamplerKind kind) const { return samplers[static_cast<size_t>(kind)]; }
        rhi::PipelineLayoutHandle Layout() const { return pipelineLayout; }
        rhi::TextureHandle White2D() const { return white2d; }
        rhi::TextureHandle WhiteArray() const { return whiteArray; }

        // deferred data movement; applied by Flush at the start of the frame's command list
        void WriteBuffer(const Arena& arena, uint64_t byteOffset, const void* data, uint64_t bytes) {
            WriteBuffer(arena.Buffer(), arena.BarrierState(), byteOffset, data, bytes);
        }
        void WriteBuffer(rhi::BufferHandle buffer, rhi::State resting, uint64_t byteOffset, const void* data, uint64_t bytes);
        // immutable vertex or index data uploaded at the next flush
        void ForgetBuffer(rhi::BufferHandle buffer) { bufferStates.erase(buffer.value); }
        rhi::BufferHandle CreateStatic(const void* data, uint64_t bytes, rhi::BufferUsage usage, const char* name);
        struct ImageWrite {
            rhi::TextureHandle texture;
            uint32_t mip = 0, layer = 0, x = 0, y = 0, width = 0, height = 0;
            uint64_t source = 0;
            uint32_t rowBytes = 0;
        };
        // data is copied now; rows are tightly packed unless rowBytes says otherwise
        void WriteTexture(rhi::TextureHandle texture, uint32_t layer, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                          const void* data, uint32_t rowBytes = 0, uint32_t mip = 0);
        // texture was created by the caller in this state after the first upload
        rhi::TextureHandle CreateSampled(rhi::TextureDesc desc);
        void DestroyTexture(rhi::TextureHandle texture);
        void Flush(rhi::CommandList& list);
        // makes this frame's ring writes visible to the device; call before every submit that reads ring memory
        void FlushRings();
        bool HasPendingWrites() const { return !bufferWrites.empty() || !imageWrites.empty(); }

        // state tracking for textures the context transitions by hand
        rhi::State StateOf(rhi::TextureHandle texture) const;
        void Transition(rhi::CommandList& list, rhi::TextureHandle texture, rhi::State to);
        void SetState(rhi::TextureHandle texture, rhi::State state);

        uint64_t FrameNumber() const { return device->FrameNumber(); }

        struct BufferSource {
            rhi::BufferHandle buffer;
            rhi::State resting = rhi::State::VertexBuffer;
            uint64_t bytes = 0;
        };
        // blocking copy of gpu-only buffers to the cpu, for when the whole device is about to be replaced
        std::vector<std::vector<uint8_t>> ReadBuffers(std::span<const BufferSource> sources);

    private:
        struct RingChunk {
            rhi::BufferHandle buffer;
            uint8_t* data = nullptr;
            uint64_t size = 0;
            uint64_t used = 0;
        };
        struct BufferWrite {
            rhi::BufferHandle buffer;
            rhi::State state;
            uint64_t offset, bytes, source;
        };

        bool CreateRing(uint32_t slot, uint64_t minimum);
        void ReleaseRings();
        void BuildSamplers();
        bool BuildLayouts();

        GpuSettings settings;
        std::unique_ptr<rhi::Instance> instance;
        std::unique_ptr<rhi::Device> device;
        std::unique_ptr<rhi::Swapchain> swapchain;
        rhi::ShaderLibrary library;
        std::vector<uint8_t> packBytes;
        std::vector<rhi::ShaderHandle> vertexShaders, fragmentShaders;
        rhi::ShaderTarget target = rhi::ShaderTarget::SpirV;

        rhi::BindLayoutHandle uniformLayout, textureLayout;
        rhi::PipelineLayoutHandle pipelineLayout;
        std::array<rhi::SamplerHandle, static_cast<size_t>(SamplerKind::Count)> samplers{};
        uint32_t uniformRangeBytes = 4096;

        std::unordered_map<PipelineKey, rhi::PipelineHandle, PipelineKeyHash> pipelines;
        std::unordered_map<TextureSet, rhi::BindGroupHandle, TextureSetHash> textureGroups;

        std::vector<std::vector<RingChunk>> rings;
        std::vector<std::vector<rhi::BindGroupHandle>> ringGroups;
        std::vector<uint32_t> ringCursor;

        std::vector<uint8_t> pendingBytes;
        std::vector<BufferWrite> bufferWrites;
        std::vector<ImageWrite> imageWrites;
        std::unordered_map<uint32_t, rhi::State> textureStates;
        std::unordered_map<uint32_t, rhi::State> bufferStates;

        rhi::TextureHandle white2d, whiteArray;
        rhi::TextureHandle offscreen;
        rhi::Format targetFormat = rhi::Format::BGRA8Unorm;
        uint32_t targetWidth = 0, targetHeight = 0;
        uint32_t acquireFailures = 0;
    };

    uint32_t FormatBytes(rhi::Format format);
}
