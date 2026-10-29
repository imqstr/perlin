#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rhi {
    enum class Api : uint8_t { None, Vulkan, D3D12, D3D11, Metal, WebGPU };

    constexpr const char* ApiName(Api api) {
        switch (api) {
        case Api::Vulkan: return "Vulkan";
        case Api::D3D12: return "Direct3D 12";
        case Api::D3D11: return "Direct3D 11";
        case Api::Metal: return "Metal";
        case Api::WebGPU: return "WebGPU";
        default: return "None";
        }
    }

    // stable ids for settings files and the command line
    constexpr const char* ApiId(Api api) {
        switch (api) {
        case Api::Vulkan: return "vulkan";
        case Api::D3D12: return "d3d12";
        case Api::D3D11: return "d3d11";
        case Api::Metal: return "metal";
        case Api::WebGPU: return "webgpu";
        default: return "none";
        }
    }

    Api ApiFromId(std::string_view id);

    // index in the low 24 bits, generation above: a stale handle never aliases a recycled slot
    template <class Tag>
    struct Handle {
        uint32_t value = 0;
        constexpr explicit operator bool() const { return value != 0; }
        constexpr bool operator==(const Handle&) const = default;
        constexpr uint32_t Index() const { return value & 0xFFFFFFu; }
        constexpr uint32_t Generation() const { return value >> 24; }
    };

    using BufferHandle = Handle<struct BufferTag>;
    using TextureHandle = Handle<struct TextureTag>;
    using SamplerHandle = Handle<struct SamplerTag>;
    using ShaderHandle = Handle<struct ShaderTag>;
    using BindLayoutHandle = Handle<struct BindLayoutTag>;
    using BindGroupHandle = Handle<struct BindGroupTag>;
    using PipelineLayoutHandle = Handle<struct PipelineLayoutTag>;
    using PipelineHandle = Handle<struct PipelineTag>;
    using FenceHandle = Handle<struct FenceTag>;
    using QuerySetHandle = Handle<struct QuerySetTag>;

    enum class QueueType : uint8_t { Graphics, Compute, Copy, Count };
    constexpr size_t kQueueCount = static_cast<size_t>(QueueType::Count);

    enum class Format : uint16_t {
        Unknown,
        R8Unorm,
        RG8Unorm,
        RGBA8Unorm,
        RGBA8Srgb,
        BGRA8Unorm,
        BGRA8Srgb,
        R8Uint,
        R16Uint,
        R32Uint,
        RG32Uint,
        RGBA32Uint,
        R16Float,
        RG16Float,
        RGBA16Float,
        R32Float,
        RG32Float,
        RGB32Float,
        RGBA32Float,
        RGB10A2Unorm,
        R11G11B10Float,
        RG16Unorm,
        RGBA16Unorm,
        D16Unorm,
        D32Float,
        D24UnormS8Uint,
        D32FloatS8Uint,
        BC1Unorm,
        BC3Unorm,
        BC7Unorm,
        Count
    };

    struct FormatInfo {
        uint8_t bytesPerBlock;
        uint8_t blockWidth;
        uint8_t blockHeight;
        uint8_t channels;
        bool depth;
        bool stencil;
        bool srgb;
        bool compressed;
        bool floating;
    };
    const FormatInfo& GetFormatInfo(Format format);
    inline bool IsDepthFormat(Format f) { return GetFormatInfo(f).depth; }
    inline bool HasStencil(Format f) { return GetFormatInfo(f).stencil; }

    enum class ShaderStage : uint8_t {
        None = 0,
        Vertex = 1,
        Fragment = 2,
        Compute = 4,
        AllGraphics = Vertex | Fragment,
        All = Vertex | Fragment | Compute
    };
    constexpr ShaderStage operator|(ShaderStage a, ShaderStage b) {
        return static_cast<ShaderStage>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
    }
    constexpr bool operator&(ShaderStage a, ShaderStage b) {
        return (static_cast<uint8_t>(a) & static_cast<uint8_t>(b)) != 0;
    }

    enum class BufferUsage : uint32_t {
        None = 0,
        Vertex = 1u << 0,
        Index = 1u << 1,
        Uniform = 1u << 2,
        Storage = 1u << 3,
        Indirect = 1u << 4,
        CopySrc = 1u << 5,
        CopyDst = 1u << 6,
    };
    constexpr BufferUsage operator|(BufferUsage a, BufferUsage b) {
        return static_cast<BufferUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
    constexpr bool Has(BufferUsage v, BufferUsage bit) {
        return (static_cast<uint32_t>(v) & static_cast<uint32_t>(bit)) != 0;
    }

    // Upload and Readback are persistently mapped; GpuOnly is never CPU visible
    enum class MemoryClass : uint8_t {
        GpuOnly,
        Upload,
        Readback,
        // device-local and host-visible when the adapter has it (resizable bar / unified), else Upload
        GpuMappable
    };

    struct BufferDesc {
        uint64_t size = 0;
        BufferUsage usage = BufferUsage::None;
        MemoryClass memory = MemoryClass::GpuOnly;
        // element size for structured storage buffers; 0 = raw
        uint32_t stride = 0;
        // shared between queues: avoids ownership transfers at the cost of some driver fast paths
        bool crossQueue = false;
        const char* name = nullptr;
    };

    enum class TextureDim : uint8_t { Tex2D, Tex2DArray, Cube, Tex3D };

    enum class TextureUsage : uint32_t {
        None = 0,
        Sampled = 1u << 0,
        Storage = 1u << 1,
        ColorTarget = 1u << 2,
        DepthTarget = 1u << 3,
        CopySrc = 1u << 4,
        CopyDst = 1u << 5,
    };
    constexpr TextureUsage operator|(TextureUsage a, TextureUsage b) {
        return static_cast<TextureUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
    constexpr bool Has(TextureUsage v, TextureUsage bit) {
        return (static_cast<uint32_t>(v) & static_cast<uint32_t>(bit)) != 0;
    }

    struct TextureDesc {
        TextureDim dim = TextureDim::Tex2D;
        Format format = Format::Unknown;
        uint32_t width = 1;
        uint32_t height = 1;
        // array layers for Tex2DArray, depth for Tex3D, ignored for Cube (6) and Tex2D (1)
        uint32_t depthOrLayers = 1;
        uint32_t mips = 1;
        uint32_t samples = 1;
        TextureUsage usage = TextureUsage::Sampled;
        bool crossQueue = false;
        const char* name = nullptr;
        // clear colour the driver can fast-clear to; zero for anything else
        std::array<float, 4> optimizedClear{};
    };

    struct SubresourceRange {
        uint32_t baseMip = 0;
        uint32_t mipCount = 0;       // 0 = all remaining
        uint32_t baseLayer = 0;
        uint32_t layerCount = 0;     // 0 = all remaining
    };

    enum class Filter : uint8_t { Nearest, Linear };
    enum class AddressMode : uint8_t { Repeat, Mirror, Clamp, Border };
    enum class CompareOp : uint8_t { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };

    struct SamplerDesc {
        Filter min = Filter::Linear;
        Filter mag = Filter::Linear;
        Filter mip = Filter::Linear;
        AddressMode u = AddressMode::Clamp;
        AddressMode v = AddressMode::Clamp;
        AddressMode w = AddressMode::Clamp;
        float maxAnisotropy = 1.0f;
        float lodBias = 0.0f;
        float minLod = 0.0f;
        float maxLod = 1000.0f;
        bool compare = false;
        CompareOp compareOp = CompareOp::LessEqual;
        std::array<float, 4> border{};
        constexpr bool operator==(const SamplerDesc&) const = default;
    };

    // Hlsl is source text, compiled by the driver layer at run time (d3d11)
    enum class ShaderFormat : uint8_t { SpirV, Dxil, Dxbc, Msl, Wgsl, Hlsl };

    struct ShaderDesc {
        ShaderStage stage = ShaderStage::Vertex;
        ShaderFormat format = ShaderFormat::SpirV;
        std::span<const uint8_t> code;
        const char* entry = "main";
        const char* name = nullptr;
    };

    enum class BindingType : uint8_t {
        UniformBuffer,
        // offset supplied at SetBindGroup; maps to a root CBV on D3D12
        UniformBufferDynamic,
        StorageBuffer,
        StorageBufferRW,
        SampledTexture,
        StorageTextureRW,
        Sampler
    };

    struct BindLayoutEntry {
        uint32_t binding = 0;
        BindingType type = BindingType::UniformBuffer;
        ShaderStage stages = ShaderStage::AllGraphics;
        uint32_t count = 1;
        // texture views seen by the shader; ignored for other types
        TextureDim viewDim = TextureDim::Tex2D;
        // false: the texture holds depth or 32 bit float data and the paired sampler is nearest-only. only webgpu cares
        bool filterable = true;
    };

    struct BindLayoutDesc {
        std::span<const BindLayoutEntry> entries;
        const char* name = nullptr;
    };

    struct BufferBinding {
        BufferHandle buffer;
        uint64_t offset = 0;
        // 0 = to the end of the buffer
        uint64_t size = 0;
    };

    struct TextureBinding {
        TextureHandle texture;
        SubresourceRange range;
        TextureDim viewDim = TextureDim::Tex2D;
        // reinterpret as another format of the same size (srgb views, typeless depth); Unknown = native
        Format format = Format::Unknown;
        // sample the stencil plane of a depth-stencil texture instead of depth
        bool stencil = false;
    };

    struct BindGroupEntry {
        uint32_t binding = 0;
        uint32_t arrayIndex = 0;
        BufferBinding buffer;
        TextureBinding texture;
        SamplerHandle sampler;
    };

    struct BindGroupDesc {
        BindLayoutHandle layout;
        std::span<const BindGroupEntry> entries;
        // Frame groups are recycled when the frame that created them retires
        bool perFrame = false;
        const char* name = nullptr;
    };

    constexpr uint32_t kMaxBindGroups = 4;
    constexpr uint32_t kMaxPushConstantBytes = 128;
    constexpr uint32_t kMaxColorTargets = 8;
    constexpr uint32_t kMaxVertexStreams = 8;
    constexpr uint32_t kMaxVertexAttributes = 16;
    constexpr uint32_t kMaxDynamicOffsets = 4;

    struct PipelineLayoutDesc {
        std::array<BindLayoutHandle, kMaxBindGroups> sets{};
        uint32_t pushConstantBytes = 0;
        ShaderStage pushConstantStages = ShaderStage::AllGraphics;
        const char* name = nullptr;
    };

    enum class VertexStep : uint8_t { Vertex, Instance };

    struct VertexStreamDesc {
        uint32_t stride = 0;
        VertexStep step = VertexStep::Vertex;
    };

    // shaders declare every vertex input as TEXCOORD<location> so all backends agree on the binding
    struct VertexAttributeDesc {
        uint32_t location = 0;
        uint32_t stream = 0;
        Format format = Format::Unknown;
        uint32_t offset = 0;
    };

    enum class Topology : uint8_t { TriangleList, TriangleStrip, LineList, PointList };
    enum class CullMode : uint8_t { None, Back, Front };
    enum class FrontFace : uint8_t { CounterClockwise, Clockwise };

    struct RasterState {
        CullMode cull = CullMode::None;
        FrontFace frontFace = FrontFace::CounterClockwise;
        bool wireframe = false;
        bool depthClip = true;
        float depthBiasConstant = 0.0f;
        float depthBiasSlope = 0.0f;
        float depthBiasClamp = 0.0f;
    };

    enum class StencilOp : uint8_t { Keep, Zero, Replace, IncrementClamp, DecrementClamp, Invert, IncrementWrap, DecrementWrap };

    struct StencilFace {
        StencilOp fail = StencilOp::Keep;
        StencilOp depthFail = StencilOp::Keep;
        StencilOp pass = StencilOp::Keep;
        CompareOp compare = CompareOp::Always;
    };

    struct DepthStencilState {
        bool depthTest = false;
        bool depthWrite = false;
        CompareOp depthCompare = CompareOp::LessEqual;
        bool stencilTest = false;
        uint8_t stencilReadMask = 0xFF;
        uint8_t stencilWriteMask = 0xFF;
        StencilFace front;
        StencilFace back;
    };

    enum class BlendFactor : uint8_t {
        Zero, One, SrcColor, InvSrcColor, SrcAlpha, InvSrcAlpha, DstColor, InvDstColor, DstAlpha, InvDstAlpha, SrcAlphaSaturate
    };
    enum class BlendOp : uint8_t { Add, Subtract, ReverseSubtract, Min, Max };

    enum class ColorMask : uint8_t { None = 0, R = 1, G = 2, B = 4, A = 8, RGB = 7, All = 15 };
    constexpr ColorMask operator|(ColorMask a, ColorMask b) {
        return static_cast<ColorMask>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
    }
    constexpr bool Has(ColorMask v, ColorMask bit) {
        return (static_cast<uint8_t>(v) & static_cast<uint8_t>(bit)) != 0;
    }

    struct BlendTarget {
        bool enable = false;
        BlendFactor srcColor = BlendFactor::One;
        BlendFactor dstColor = BlendFactor::Zero;
        BlendOp colorOp = BlendOp::Add;
        BlendFactor srcAlpha = BlendFactor::One;
        BlendFactor dstAlpha = BlendFactor::Zero;
        BlendOp alphaOp = BlendOp::Add;
        ColorMask writeMask = ColorMask::All;
    };

    struct BlendState {
        bool alphaToCoverage = false;
        std::array<BlendTarget, kMaxColorTargets> targets{};
    };

    struct GraphicsPipelineDesc {
        ShaderHandle vertex;
        ShaderHandle fragment;
        PipelineLayoutHandle layout;
        std::array<VertexStreamDesc, kMaxVertexStreams> streams{};
        uint32_t streamCount = 0;
        std::array<VertexAttributeDesc, kMaxVertexAttributes> attributes{};
        uint32_t attributeCount = 0;
        Topology topology = Topology::TriangleList;
        RasterState raster;
        DepthStencilState depthStencil;
        BlendState blend;
        std::array<Format, kMaxColorTargets> colorFormats{};
        uint32_t colorCount = 0;
        Format depthFormat = Format::Unknown;
        uint32_t samples = 1;
        const char* name = nullptr;
    };

    struct ComputePipelineDesc {
        ShaderHandle compute;
        PipelineLayoutHandle layout;
        const char* name = nullptr;
    };

    // pipelines compile on worker threads in the render layer; the device only sees finished descriptions
    enum class IndexType : uint8_t { U16, U32 };

    // what a resource is being used for between two barriers
    enum class State : uint32_t {
        Undefined = 0,
        CopySrc = 1u << 0,
        CopyDst = 1u << 1,
        VertexBuffer = 1u << 2,
        IndexBuffer = 1u << 3,
        UniformBuffer = 1u << 4,
        IndirectArgument = 1u << 5,
        // sampled or read-only storage; stage split matters on D3D12 and for async compute
        ShaderReadFragment = 1u << 6,
        ShaderReadOther = 1u << 7,
        ShaderWrite = 1u << 8,
        ColorTarget = 1u << 9,
        DepthWrite = 1u << 10,
        DepthRead = 1u << 11,
        Present = 1u << 12,
        ResolveSrc = 1u << 13,
        ResolveDst = 1u << 14,
        ShaderRead = ShaderReadFragment | ShaderReadOther,
        // depth buffer bound for testing and sampled at the same time
        DepthReadSampled = DepthRead | ShaderRead
    };
    constexpr State operator|(State a, State b) {
        return static_cast<State>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
    constexpr bool Has(State v, State bit) {
        return (static_cast<uint32_t>(v) & static_cast<uint32_t>(bit)) != 0;
    }
    constexpr bool IsWriteState(State s) {
        return Has(s, State::CopyDst) || Has(s, State::ShaderWrite) || Has(s, State::ColorTarget) ||
               Has(s, State::DepthWrite) || Has(s, State::ResolveDst);
    }

    struct TextureBarrier {
        TextureHandle texture;
        State before = State::Undefined;
        State after = State::Undefined;
        SubresourceRange range;
        // queue ownership transfer; leave equal for none
        QueueType srcQueue = QueueType::Graphics;
        QueueType dstQueue = QueueType::Graphics;
    };

    struct BufferBarrier {
        BufferHandle buffer;
        State before = State::Undefined;
        State after = State::Undefined;
        QueueType srcQueue = QueueType::Graphics;
        QueueType dstQueue = QueueType::Graphics;
    };

    enum class LoadOp : uint8_t { Load, Clear, DontCare };
    enum class StoreOp : uint8_t { Store, DontCare };

    struct ColorAttachment {
        TextureHandle texture;
        uint32_t mip = 0;
        uint32_t layer = 0;
        LoadOp load = LoadOp::Clear;
        StoreOp store = StoreOp::Store;
        std::array<float, 4> clear{};
        // msaa resolve target; null for none
        TextureHandle resolve;
    };

    struct DepthAttachment {
        TextureHandle texture;
        uint32_t mip = 0;
        uint32_t layer = 0;
        LoadOp load = LoadOp::Clear;
        StoreOp store = StoreOp::Store;
        float clearDepth = 1.0f;
        uint8_t clearStencil = 0;
        // depth is read-only for the pass (sampled while tested)
        bool readOnly = false;
    };

    struct RenderingDesc {
        std::array<ColorAttachment, kMaxColorTargets> colors{};
        uint32_t colorCount = 0;
        DepthAttachment depth;
        const char* name = nullptr;
    };

    struct Viewport {
        float x = 0, y = 0, width = 0, height = 0, minDepth = 0.0f, maxDepth = 1.0f;
    };

    struct Rect {
        int32_t x = 0, y = 0;
        uint32_t width = 0, height = 0;
    };

    struct BufferTextureCopy {
        uint64_t bufferOffset = 0;
        // bytes between rows in the buffer; 0 = tightly packed
        uint32_t bufferRowPitch = 0;
        uint32_t mip = 0;
        uint32_t layer = 0;
        uint32_t x = 0, y = 0, z = 0;
        uint32_t width = 0, height = 0, depth = 1;
    };

    struct TextureCopy {
        uint32_t srcMip = 0, srcLayer = 0, srcX = 0, srcY = 0;
        uint32_t dstMip = 0, dstLayer = 0, dstX = 0, dstY = 0;
        uint32_t width = 0, height = 0;
    };

    struct DrawIndexedIndirectCommand {
        uint32_t indexCount;
        uint32_t instanceCount;
        uint32_t firstIndex;
        int32_t vertexOffset;
        uint32_t firstInstance;
    };
    static_assert(sizeof(DrawIndexedIndirectCommand) == 20);

    struct DrawIndirectCommand {
        uint32_t vertexCount;
        uint32_t instanceCount;
        uint32_t firstVertex;
        uint32_t firstInstance;
    };
    static_assert(sizeof(DrawIndirectCommand) == 16);

    struct DispatchIndirectCommand {
        uint32_t x, y, z;
    };

    struct QuerySetDesc {
        uint32_t count = 0;
        const char* name = nullptr;
    };

    enum class PresentMode : uint8_t {
        // wait for vblank
        Fifo,
        // newest frame wins, no tearing, extra latency-free buffering
        Mailbox,
        // tear
        Immediate
    };

    struct NativeWindow {
        void* window = nullptr;
        void* display = nullptr;
        bool wayland = false;
    };

    struct SwapchainDesc {
        NativeWindow window;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t imageCount = 3;
        PresentMode present = PresentMode::Fifo;
        bool hdr = false;
        // sampled/copied from after rendering (screenshots, frame generation inputs)
        bool allowSampling = false;
    };

    // vulkan / d3d handles for vendor sdks (streamline, ngx, fidelityfx); null when the api has no such object
    struct NativeHandles {
        Api api = Api::None;
        void* instance = nullptr;
        void* physicalDevice = nullptr;
        void* device = nullptr;
        void* graphicsQueue = nullptr;
        void* computeQueue = nullptr;
        uint32_t graphicsFamily = 0;
        uint32_t computeFamily = 0;
        void* adapter = nullptr;
        void* factory = nullptr;
        void* getInstanceProcAddr = nullptr;
        void* getDeviceProcAddr = nullptr;
    };

    // what a vendor sdk needs to adopt a texture: the api object and the state it is in at the point of the call
    struct NativeTexture {
        // VkImage / ID3D12Resource*
        void* resource = nullptr;
        // VkImageView over the whole texture (vulkan only)
        void* view = nullptr;
        // VkDeviceMemory (vulkan only)
        void* memory = nullptr;
        // VkImageLayout / D3D12_RESOURCE_STATES for the rhi state asked about
        uint32_t state = 0;
        // VkFormat / DXGI_FORMAT
        uint32_t nativeFormat = 0;
        uint32_t width = 0, height = 0, mips = 1, layers = 1;
        // VkImageUsageFlags / VkImageCreateFlags (vulkan only)
        uint32_t usage = 0, flags = 0;
        // VkImageAspectFlags (vulkan only)
        uint32_t aspect = 0;
    };

    struct MemoryBudget {
        uint64_t deviceBudget = 0;
        uint64_t deviceUsage = 0;
        uint64_t hostBudget = 0;
        uint64_t hostUsage = 0;
        bool valid = false;
    };

    struct DeviceStats {
        uint64_t draws = 0;
        uint64_t dispatches = 0;
        uint64_t indirectDraws = 0;
        uint64_t barriers = 0;
        uint64_t pipelineBinds = 0;
        uint64_t bindGroupBinds = 0;
        uint64_t submits = 0;
        uint64_t bufferBytes = 0;
        uint64_t textureBytes = 0;
        uint32_t liveBuffers = 0;
        uint32_t liveTextures = 0;
        uint32_t livePipelines = 0;
        uint32_t liveBindGroups = 0;
    };
}
