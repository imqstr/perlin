#include "rhi/webgpu/WebGpuInternal.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <type_traits>

#if !defined(__EMSCRIPTEN__)
#include <webgpu/wgpu.h>
#endif

namespace rhi::webgpu {
    namespace {
        constexpr const char* kShaderPrefix = "diagnostic(off, derivative_uniformity);\n";

        // the completion callback lost its message argument in older headers
        template <class Fn>
        constexpr auto QueueDoneFor() {
            if constexpr (std::is_convertible_v<void (*)(WGPUQueueWorkDoneStatus, void*, void*), Fn>) {
                return +[](WGPUQueueWorkDoneStatus, void* a, void* b) {
                    auto* shared = static_cast<std::shared_ptr<std::atomic<uint64_t>>*>(a);
                    const uint64_t value = reinterpret_cast<uintptr_t>(b);
                    uint64_t seen = shared->get()->load();
                    while (seen < value && !shared->get()->compare_exchange_weak(seen, value)) {
                    }
                    delete shared;
                };
            } else {
                return +[](WGPUQueueWorkDoneStatus, WGPUStringView, void* a, void* b) {
                    auto* shared = static_cast<std::shared_ptr<std::atomic<uint64_t>>*>(a);
                    const uint64_t value = reinterpret_cast<uintptr_t>(b);
                    uint64_t seen = shared->get()->load();
                    while (seen < value && !shared->get()->compare_exchange_weak(seen, value)) {
                    }
                    delete shared;
                };
            }
        }

        struct MapState {
            bool done = false;
            bool ok = false;
        };

        void OnMapped(WGPUMapAsyncStatus status, WGPUStringView, void* a, void*) {
            auto* state = static_cast<MapState*>(a);
            state->ok = status == WGPUMapAsyncStatus_Success;
            state->done = true;
        }

        uint64_t StageBits(ShaderStage s) {
            uint64_t bits = 0;
            if (s & ShaderStage::Vertex)
                bits |= WGPUShaderStage_Vertex;
            if (s & ShaderStage::Fragment)
                bits |= WGPUShaderStage_Fragment;
            if (s & ShaderStage::Compute)
                bits |= WGPUShaderStage_Compute;
            return bits;
        }

        bool KindMatches(BindingType type, ResourceKind kind) {
            switch (type) {
            case BindingType::UniformBuffer:
            case BindingType::UniformBufferDynamic: return kind == ResourceKind::UniformBuffer;
            case BindingType::StorageBuffer: return kind == ResourceKind::StorageBuffer || kind == ResourceKind::StorageBufferRW;
            case BindingType::StorageBufferRW: return kind == ResourceKind::StorageBufferRW;
            case BindingType::SampledTexture: return kind == ResourceKind::SampledTexture;
            case BindingType::StorageTextureRW: return kind == ResourceKind::StorageTexture;
            case BindingType::Sampler: return kind == ResourceKind::Sampler;
            }
            return false;
        }
    }

    WebGpuDevice::WebGpuDevice(Owned<WGPUInstance> instance, Owned<WGPUAdapter> adapter, Owned<WGPUDevice> device,
                               const DeviceOptions& options, const AdapterInfo& info, bool mapSync)
        : instance(std::move(instance)), adapter(std::move(adapter)), device(std::move(device)), options(options), mapSync(mapSync) {
        caps.adapter = info;
    }

    WebGpuDevice::~WebGpuDevice() {
        WaitIdle();
        lists.clear();
    }

    void WebGpuDevice::ReportError(const std::string& message) {
        std::lock_guard lock(errorMutex);
        if (firstError.empty())
            firstError = message;
        std::fprintf(stderr, "[webgpu] %s\n", message.c_str());
    }

    NativeHandles WebGpuDevice::Native() const {
        NativeHandles n;
        n.api = Api::WebGPU;
        n.instance = instance.Get();
        n.device = device.Get();
        n.adapter = adapter.Get();
        return n;
    }

    bool WebGpuDevice::Init() {
        queue = Owned<WGPUQueue>(wgpuDeviceGetQueue(device.Get()));
        if (!queue)
            return false;
        FillCaps();

        WGPUSamplerDescriptor sd{};
        sd.label = View("nearest");
        sd.addressModeU = sd.addressModeV = sd.addressModeW = WGPUAddressMode_ClampToEdge;
        sd.magFilter = sd.minFilter = WGPUFilterMode_Nearest;
        sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
        sd.lodMaxClamp = 32.0f;
        sd.maxAnisotropy = 1;
        nearestSampler = Owned<WGPUSampler>(wgpuDeviceCreateSampler(device.Get(), &sd));

        WGPUBindGroupLayoutDescriptor ld{};
        ld.label = View("empty");
        emptyLayout = Owned<WGPUBindGroupLayout>(wgpuDeviceCreateBindGroupLayout(device.Get(), &ld));

        WGPUBindGroupDescriptor gd{};
        gd.label = View("empty group");
        gd.layout = emptyLayout.Get();
        emptyGroup = Owned<WGPUBindGroup>(wgpuDeviceCreateBindGroup(device.Get(), &gd));

        frameFence = CreateFence(0);
        return nearestSampler && emptyLayout && emptyGroup;
    }

    void WebGpuDevice::FillCaps() {
        WGPULimits lim{};
        wgpuDeviceGetLimits(device.Get(), &lim);
        DeviceLimits& l = caps.limits;
        l.maxTexture2D = lim.maxTextureDimension2D;
        l.maxTextureLayers = lim.maxTextureArrayLayers;
        l.maxBindGroups = std::min<uint32_t>(kMaxBindGroups, lim.maxBindGroups);
        // webgpu has no push constants; the engine puts per-draw data in uniforms
        l.maxPushConstantBytes = 0;
        l.maxSampledTexturesPerStage = lim.maxSampledTexturesPerShaderStage;
        l.maxColorTargets = std::min<uint32_t>(kMaxColorTargets, lim.maxColorAttachments);
        l.maxMsaaSamples = 4;
        l.uniformBufferAlignment = lim.minUniformBufferOffsetAlignment;
        l.storageBufferAlignment = lim.minStorageBufferOffsetAlignment;
        l.copyRowPitchAlignment = 256;
        l.copyOffsetAlignment = 256;
        l.maxBufferSize = lim.maxBufferSize;
        l.maxAnisotropy = 16.0f;

        const auto has = [&](WGPUFeatureName f) { return wgpuDeviceHasFeature(device.Get(), f) != 0; };
        DeviceFeatures& f = caps.features;
        f.samplerAnisotropy = true;
        f.textureCompressionBC = has(WGPUFeatureName_TextureCompressionBC);
        f.timestampQueries = false;
        f.depthClipControl = has(WGPUFeatureName_DepthClipControl);
        f.drawIndirectFirstInstance = has(WGPUFeatureName_IndirectFirstInstance);
        f.shaderFloat16 = has(WGPUFeatureName_ShaderF16);
        f.unifiedMemory = caps.adapter.type == AdapterType::Integrated || caps.adapter.type == AdapterType::Cpu;
        // one in-order queue: no async compute, no separate copy queue, no multi draw, no count buffers
        f.asyncCompute = false;
        f.dedicatedCopyQueue = false;
        f.multiDrawIndirect = false;
        f.drawIndirectCount = false;

        const bool f32Filter = has(WGPUFeatureName_Float32Filterable);
        const bool f32Blend = has(WGPUFeatureName_Float32Blendable);
        const bool rg11 = has(WGPUFeatureName_RG11B10UfloatRenderable);
        const bool d32s8 = has(WGPUFeatureName_Depth32FloatStencil8);
        const FormatUse S = FormatUse::Sampled, L = FormatUse::SampledLinear, C = FormatUse::ColorTarget, B = FormatUse::Blendable,
                        D = FormatUse::DepthTarget, T = FormatUse::Storage, M = FormatUse::Msaa;
        const auto set = [&](Format fmt, FormatUse use) { caps.formats[static_cast<size_t>(fmt)] = use; };
        for (Format fmt : {Format::R8Unorm, Format::RG8Unorm, Format::RGBA8Unorm, Format::RGBA8Srgb, Format::BGRA8Unorm, Format::BGRA8Srgb,
                           Format::R16Float, Format::RG16Float, Format::RGBA16Float, Format::RGB10A2Unorm})
            set(fmt, S | L | C | B | M);
        set(Format::RGBA8Unorm, S | L | C | B | M | T);
        set(Format::RGBA16Float, S | L | C | B | M | T);
        set(Format::R8Uint, S | C | M);
        set(Format::R16Uint, S | C | M);
        set(Format::R32Uint, S | C | T);
        set(Format::RG32Uint, S | C | T);
        set(Format::RGBA32Uint, S | C | T);
        FormatUse f32 = S | C | T;
        if (f32Filter)
            f32 = f32 | L;
        if (f32Blend)
            f32 = f32 | B;
        set(Format::R32Float, f32);
        set(Format::RG32Float, f32);
        set(Format::RGBA32Float, f32);
        set(Format::R11G11B10Float, rg11 ? (S | L | C | B | M) : (S | L));
        set(Format::D16Unorm, S | D | M);
        set(Format::D32Float, S | D | M);
        set(Format::D24UnormS8Uint, S | D | M);
        if (d32s8)
            set(Format::D32FloatS8Uint, S | D | M);
        if (f.textureCompressionBC)
            for (Format fmt : {Format::BC1Unorm, Format::BC3Unorm, Format::BC7Unorm})
                set(fmt, S | L);
    }

    // ---- resources ----------------------------------------------------------------------------------------------------

    BufferHandle WebGpuDevice::CreateBuffer(const BufferDesc& desc) {
        Buffer b;
        b.desc = desc;
        b.name = desc.name ? desc.name : "";
        b.gpuSize = AlignUp(std::max<uint64_t>(desc.size, 4), 4);
        WGPUBufferUsage usage = 0;
        if (desc.memory == MemoryClass::Readback) {
            // a mappable buffer may only carry copy-dst
            usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
        } else {
            if (Has(desc.usage, BufferUsage::Vertex))
                usage |= WGPUBufferUsage_Vertex;
            if (Has(desc.usage, BufferUsage::Index))
                usage |= WGPUBufferUsage_Index;
            if (Has(desc.usage, BufferUsage::Uniform))
                usage |= WGPUBufferUsage_Uniform;
            if (Has(desc.usage, BufferUsage::Storage))
                usage |= WGPUBufferUsage_Storage;
            if (Has(desc.usage, BufferUsage::Indirect))
                usage |= WGPUBufferUsage_Indirect;
            if (Has(desc.usage, BufferUsage::CopySrc))
                usage |= WGPUBufferUsage_CopySrc;
            usage |= WGPUBufferUsage_CopyDst;
        }
        WGPUBufferDescriptor bd{};
        bd.label = View(b.name);
        bd.usage = usage;
        bd.size = b.gpuSize;
        b.buffer = Owned<WGPUBuffer>(wgpuDeviceCreateBuffer(device.Get(), &bd));
        if (!b.buffer)
            return {};
        if (desc.memory != MemoryClass::GpuOnly)
            b.shadow.assign(b.gpuSize, 0);
        stats.bufferBytes += b.gpuSize;
        stats.liveBuffers++;
        const BufferHandle h = buffers.Alloc(std::move(b));
        buffers.Get(h)->desc.name = buffers.Get(h)->name.c_str();
        return h;
    }

    TextureHandle WebGpuDevice::CreateTexture(const TextureDesc& desc) {
        const WGPUTextureFormat format = ToTextureFormat(desc.format);
        if (format == WGPUTextureFormat_Undefined) {
            ReportError(std::string("CreateTexture: format not available on webgpu: ") + std::to_string(static_cast<int>(desc.format)));
            return {};
        }
        Texture t;
        t.desc = desc;
        t.desc.mips = std::max(1u, desc.mips);
        t.desc.samples = std::max(1u, desc.samples);
        t.name = desc.name ? desc.name : "";
        WGPUTextureUsage usage = 0;
        if (Has(desc.usage, TextureUsage::Sampled))
            usage |= WGPUTextureUsage_TextureBinding;
        if (Has(desc.usage, TextureUsage::Storage))
            usage |= WGPUTextureUsage_StorageBinding;
        if (Has(desc.usage, TextureUsage::ColorTarget) || Has(desc.usage, TextureUsage::DepthTarget))
            usage |= WGPUTextureUsage_RenderAttachment;
        if (Has(desc.usage, TextureUsage::CopySrc))
            usage |= WGPUTextureUsage_CopySrc;
        if (Has(desc.usage, TextureUsage::CopyDst))
            usage |= WGPUTextureUsage_CopyDst;
        WGPUTextureDescriptor td{};
        td.label = View(t.name);
        td.usage = usage;
        td.dimension = desc.dim == TextureDim::Tex3D ? WGPUTextureDimension_3D : WGPUTextureDimension_2D;
        td.size = {desc.width, desc.height, desc.dim == TextureDim::Tex3D ? std::max(1u, desc.depthOrLayers) : LayerCountOf(t.desc)};
        td.format = format;
        td.mipLevelCount = t.desc.mips;
        td.sampleCount = t.desc.samples;
        t.texture = Owned<WGPUTexture>(wgpuDeviceCreateTexture(device.Get(), &td));
        if (!t.texture)
            return {};
        uint64_t bytes = 0;
        for (uint32_t m = 0; m < t.desc.mips; m++)
            bytes += static_cast<uint64_t>(RowBytes(desc.format, MipExtent(desc.width, m))) * RowBlocks(desc.format, MipExtent(desc.height, m)) *
                     LayerCountOf(t.desc) * DepthOf(t.desc);
        stats.textureBytes += bytes * t.desc.samples;
        stats.liveTextures++;
        const TextureHandle h = textures.Alloc(std::move(t));
        textures.Get(h)->desc.name = textures.Get(h)->name.c_str();
        return h;
    }

    SamplerHandle WebGpuDevice::CreateSampler(const SamplerDesc& desc) {
        Sampler s;
        s.desc = desc;
        s.linear = desc.min == Filter::Linear || desc.mag == Filter::Linear || desc.mip == Filter::Linear;
        WGPUSamplerDescriptor sd{};
        sd.label = View("sampler");
        sd.addressModeU = ToAddress(desc.u);
        sd.addressModeV = ToAddress(desc.v);
        sd.addressModeW = ToAddress(desc.w);
        sd.magFilter = ToFilter(desc.mag);
        sd.minFilter = ToFilter(desc.min);
        sd.mipmapFilter = ToMipFilter(desc.mip);
        sd.lodMinClamp = desc.minLod;
        sd.lodMaxClamp = std::min(desc.maxLod, 32.0f);
        sd.compare = desc.compare ? ToCompare(desc.compareOp) : WGPUCompareFunction_Undefined;
        const bool allLinear = desc.min == Filter::Linear && desc.mag == Filter::Linear && desc.mip == Filter::Linear;
        sd.maxAnisotropy = allLinear ? static_cast<uint16_t>(std::clamp(desc.maxAnisotropy, 1.0f, 16.0f)) : 1;
        s.sampler = Owned<WGPUSampler>(wgpuDeviceCreateSampler(device.Get(), &sd));
        if (!s.sampler)
            return {};
        return samplers.Alloc(std::move(s));
    }

    ShaderHandle WebGpuDevice::CreateShader(const ShaderDesc& desc) {
        if (desc.format != ShaderFormat::Wgsl) {
            ReportError("CreateShader: webgpu takes wgsl");
            return {};
        }
        Shader s;
        s.desc = desc;
        s.desc.code = {};
        s.desc.entry = nullptr;
        s.desc.name = nullptr;
        s.entry = desc.entry ? desc.entry : "main";
        s.source = std::string(kShaderPrefix) + std::string(reinterpret_cast<const char*>(desc.code.data()), desc.code.size());
        WGPUShaderSourceWGSL wgsl{};
        wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
        wgsl.code = View(s.source);
        WGPUShaderModuleDescriptor md{};
        md.nextInChain = &wgsl.chain;
        md.label = View(desc.name);
        s.module = Owned<WGPUShaderModule>(wgpuDeviceCreateShaderModule(device.Get(), &md));
        if (!s.module)
            return {};
        s.iface = ReflectShader(ShaderFormat::Wgsl, s.source);
        return shaders.Alloc(std::move(s));
    }

    BindLayoutHandle WebGpuDevice::CreateBindLayout(const BindLayoutDesc& desc) {
        BindLayout l;
        l.entries.assign(desc.entries.begin(), desc.entries.end());
        std::sort(l.entries.begin(), l.entries.end(), [](const BindLayoutEntry& a, const BindLayoutEntry& b) { return a.binding < b.binding; });
        return bindLayouts.Alloc(std::move(l));
    }

    BindGroupHandle WebGpuDevice::CreateBindGroup(const BindGroupDesc& desc) {
        if (!bindLayouts.Get(desc.layout)) {
            ReportError("CreateBindGroup: unknown layout");
            return {};
        }
        BindGroup g;
        g.layout = desc.layout;
        g.entries.assign(desc.entries.begin(), desc.entries.end());
        stats.liveBindGroups++;
        return bindGroups.Alloc(std::move(g));
    }

    PipelineLayoutHandle WebGpuDevice::CreatePipelineLayout(const PipelineLayoutDesc& desc) {
        if (desc.pushConstantBytes)
            ReportError("CreatePipelineLayout: webgpu has no push constants");
        PipelineLayout l;
        l.desc = desc;
        l.desc.name = nullptr;
        return pipelineLayouts.Alloc(std::move(l));
    }

    QuerySetHandle WebGpuDevice::CreateQuerySet(const QuerySetDesc& desc) {
        Query q;
        q.count = desc.count;
        return queries.Alloc(std::move(q));
    }

    void WebGpuDevice::Destroy(BufferHandle h) {
        Buffer b;
        if (!buffers.Free(h, &b))
            return;
        stats.bufferBytes -= b.gpuSize;
        stats.liveBuffers--;
    }

    void WebGpuDevice::Destroy(TextureHandle h) {
        Texture t;
        if (!textures.Free(h, &t))
            return;
        stats.liveTextures--;
    }

    void WebGpuDevice::Destroy(SamplerHandle h) { samplers.Free(h); }
    void WebGpuDevice::Destroy(ShaderHandle h) { shaders.Free(h); }
    void WebGpuDevice::Destroy(BindLayoutHandle h) { bindLayouts.Free(h); }
    void WebGpuDevice::Destroy(BindGroupHandle h) {
        if (bindGroups.Free(h))
            stats.liveBindGroups--;
    }
    void WebGpuDevice::Destroy(PipelineLayoutHandle h) { pipelineLayouts.Free(h); }
    void WebGpuDevice::Destroy(PipelineHandle h) {
        if (pipelines.Free(h))
            stats.livePipelines--;
    }
    void WebGpuDevice::Destroy(QuerySetHandle h) { queries.Free(h); }
    void WebGpuDevice::Destroy(FenceHandle h) { fences.Free(h); }

    const BufferDesc* WebGpuDevice::GetDesc(BufferHandle h) const {
        const Buffer* b = buffers.Get(h);
        return b ? &b->desc : nullptr;
    }

    const TextureDesc* WebGpuDevice::GetDesc(TextureHandle h) const {
        const Texture* t = textures.Get(h);
        return t ? &t->desc : nullptr;
    }

    uint8_t* WebGpuDevice::Map(BufferHandle handle) {
        Buffer* b = buffers.Get(handle);
        return b && !b->shadow.empty() ? b->shadow.data() : nullptr;
    }

    void WebGpuDevice::Flush(BufferHandle handle, uint64_t offset, uint64_t size) {
        Buffer* b = buffers.Get(handle);
        if (!b || b->shadow.empty() || b->desc.memory == MemoryClass::Readback)
            return;
        const uint64_t lo = offset & ~3ull;
        const uint64_t hi = std::min<uint64_t>(b->gpuSize, AlignUp(offset + size, 4));
        if (hi <= lo)
            return;
        wgpuQueueWriteBuffer(queue.Get(), b->buffer.Get(), lo, b->shadow.data() + lo, static_cast<size_t>(hi - lo));
    }

    void WebGpuDevice::Invalidate(BufferHandle handle, uint64_t, uint64_t) {
        Buffer* b = buffers.Get(handle);
        if (!b || b->desc.memory != MemoryClass::Readback || !mapSync)
            return;
        MapState state;
        WGPUBufferMapCallbackInfo info{};
        info.mode = WGPUCallbackMode_AllowProcessEvents;
        info.callback = OnMapped;
        info.userdata1 = &state;
        wgpuBufferMapAsync(b->buffer.Get(), WGPUMapMode_Read, 0, static_cast<size_t>(b->gpuSize), info);
        if (!Pump([&] { return state.done; }, 10'000'000'000ull) || !state.ok) {
            ReportError("Invalidate: readback map failed");
            return;
        }
        if (const void* p = wgpuBufferGetConstMappedRange(b->buffer.Get(), 0, static_cast<size_t>(b->gpuSize)))
            std::memcpy(b->shadow.data(), p, b->shadow.size());
        wgpuBufferUnmap(b->buffer.Get());
    }

    // ---- fences, frames ----------------------------------------------------------------------------------------------

    FenceHandle WebGpuDevice::CreateFence(uint64_t initialValue) {
        Fence f;
        f.value->store(initialValue);
        return fences.Alloc(std::move(f));
    }

    uint64_t WebGpuDevice::FenceValue(FenceHandle fence) {
        Fence* f = fences.Get(fence);
        return f ? f->value->load() : 0;
    }

    void WebGpuDevice::SignalFence(FenceHandle fence, uint64_t value) {
        if (Fence* f = fences.Get(fence)) {
            uint64_t seen = f->value->load();
            while (seen < value && !f->value->compare_exchange_weak(seen, value)) {
            }
        }
    }

    bool WebGpuDevice::WaitFence(FenceHandle fence, uint64_t value, uint64_t timeoutNs) {
        Fence* f = fences.Get(fence);
        if (!f)
            return true;
        const auto counter = f->value;
        return Pump([&] { return counter->load() >= value; }, timeoutNs);
    }

    bool WebGpuDevice::Pump(const std::function<bool()>& done, uint64_t timeoutNs) {
        if (done())
            return true;
#if defined(__EMSCRIPTEN__)
        // the browser runs callbacks between frames; blocking here would starve them
        (void)timeoutNs;
        wgpuInstanceProcessEvents(instance.Get());
        return done();
#else
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            wgpuDevicePoll(device.Get(), false, nullptr);
            wgpuInstanceProcessEvents(instance.Get());
            if (done())
                return true;
            if (timeoutNs != ~0ull &&
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()) >= timeoutNs)
                return false;
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
#endif
    }

    void WebGpuDevice::BeginFrame() {
        frame++;
        const uint64_t behind = std::max(1u, options.framesInFlight);
        if (frame > behind)
            WaitFence(frameFence, frame - behind, 2'000'000'000ull);
        else
            Pump([] { return true; }, 0);
#if !defined(__EMSCRIPTEN__)
        wgpuInstanceProcessEvents(instance.Get());
#endif
    }

    void WebGpuDevice::EndFrame() {
        Fence* f = fences.Get(frameFence);
        if (!f)
            return;
        using Fn = decltype(WGPUQueueWorkDoneCallbackInfo::callback);
        WGPUQueueWorkDoneCallbackInfo info{};
        info.mode = WGPUCallbackMode_AllowProcessEvents;
        info.callback = QueueDoneFor<Fn>();
        info.userdata1 = new std::shared_ptr<std::atomic<uint64_t>>(f->value);
        info.userdata2 = reinterpret_cast<void*>(static_cast<uintptr_t>(frame));
        wgpuQueueOnSubmittedWorkDone(queue.Get(), info);
    }

    void WebGpuDevice::WaitIdle() {
        if (!device)
            return;
#if defined(__EMSCRIPTEN__)
        wgpuInstanceProcessEvents(instance.Get());
#else
        wgpuDevicePoll(device.Get(), true, nullptr);
        wgpuInstanceProcessEvents(instance.Get());
#endif
    }

    MemoryBudget WebGpuDevice::QueryMemoryBudget() {
        MemoryBudget m;
        m.deviceUsage = stats.bufferBytes + stats.textureBytes;
        return m;
    }

    // ---- command lists, submission -----------------------------------------------------------------------------------

    CommandList* WebGpuDevice::AcquireCommandList(QueueType queueType) {
        std::lock_guard lock(listMutex);
        WebGpuCommandList* list;
        if (!freeLists.empty()) {
            list = freeLists.back();
            freeLists.pop_back();
        } else {
            lists.push_back(std::make_unique<WebGpuCommandList>(*this, queueType));
            list = lists.back().get();
        }
        list->Begin();
        return list;
    }

    void WebGpuDevice::Submit(QueueType, const SubmitDesc& desc) {
        std::vector<WGPUCommandBuffer> commands;
        for (CommandList* base : desc.lists) {
            auto* list = static_cast<WebGpuCommandList*>(base);
            if (WGPUCommandBuffer cb = list->Finish())
                commands.push_back(cb);
        }
        if (!commands.empty())
            wgpuQueueSubmit(queue.Get(), commands.size(), commands.data());
        for (WGPUCommandBuffer cb : commands)
            wgpuCommandBufferRelease(cb);
        {
            std::lock_guard lock(listMutex);
            for (CommandList* base : desc.lists)
                freeLists.push_back(static_cast<WebGpuCommandList*>(base));
        }
        stats.submits++;

        for (const FenceOp& op : desc.signals) {
            Fence* f = fences.Get(op.fence);
            if (!f)
                continue;
            using Fn = decltype(WGPUQueueWorkDoneCallbackInfo::callback);
            WGPUQueueWorkDoneCallbackInfo info{};
            info.mode = WGPUCallbackMode_AllowProcessEvents;
            info.callback = QueueDoneFor<Fn>();
            info.userdata1 = new std::shared_ptr<std::atomic<uint64_t>>(f->value);
            info.userdata2 = reinterpret_cast<void*>(static_cast<uintptr_t>(op.value));
            wgpuQueueOnSubmittedWorkDone(queue.Get(), info);
        }
    }

    // ---- texture views, native bind layouts and groups ---------------------------------------------------------------

    WGPUTextureView WebGpuDevice::ViewOf(Texture& texture, const SubresourceRange& range, TextureDim dim, Format format, bool stencil,
                                         bool attachment) {
        const SubresourceRange r = ResolveRange(range, texture.desc);
        const bool depthFormat = IsDepthFormat(texture.desc.format);
        ViewKey key;
        key.baseMip = r.baseMip;
        key.mipCount = r.mipCount;
        key.baseLayer = r.baseLayer;
        key.layerCount = dim == TextureDim::Tex2D ? 1 : dim == TextureDim::Cube ? 6 : r.layerCount;
        key.dim = static_cast<uint16_t>(dim);
        key.format = static_cast<uint16_t>(format);
        key.aspect = attachment || !depthFormat ? 0 : (stencil ? 2 : 1);
        std::lock_guard lock(texture.viewMutex);
        if (const auto it = texture.views.find(key); it != texture.views.end())
            return it->second.Get();
        WGPUTextureViewDescriptor vd{};
        vd.label = View("view");
        vd.format = format == Format::Unknown ? WGPUTextureFormat_Undefined : ToTextureFormat(format);
        vd.dimension = ToViewDimension(dim);
        vd.baseMipLevel = key.baseMip;
        vd.mipLevelCount = key.mipCount;
        vd.baseArrayLayer = key.baseLayer;
        vd.arrayLayerCount = key.layerCount;
        vd.aspect = key.aspect == 0 ? WGPUTextureAspect_All : (key.aspect == 1 ? WGPUTextureAspect_DepthOnly : WGPUTextureAspect_StencilOnly);
        vd.usage = 0;
        WGPUTextureView view = wgpuTextureCreateView(texture.texture.Get(), &vd);
        if (view)
            texture.views.emplace(key, Owned<WGPUTextureView>(view));
        return view;
    }

    std::shared_ptr<NativeSet> WebGpuDevice::CachedLayout(const std::string& key, const std::function<std::shared_ptr<NativeSet>()>& make) {
        std::lock_guard lock(layoutMutex);
        if (const auto it = layoutCache.find(key); it != layoutCache.end())
            return it->second;
        std::shared_ptr<NativeSet> made = make();
        if (made && made->layout)
            layoutCache[key] = made;
        return made;
    }

    std::shared_ptr<NativeSet> WebGpuDevice::BuildNativeSet(const PipelineLayoutDesc& pl, uint32_t set, std::span<const Shader* const> stages,
                                                            ShaderStage) {
        const BindLayout* layout = bindLayouts.Get(pl.sets[set]);
        std::vector<NativeSet::Entry> chosen;
        std::vector<uint64_t> visibility;
        uint32_t rhiDynamic = 0;
        std::vector<uint32_t> pick;
        if (layout) {
            for (const BindLayoutEntry& e : layout->entries) {
                uint64_t vis = 0;
                NativeSet::Entry entry;
                entry.rhi = e;
                bool used = false;
                for (const Shader* sh : stages) {
                    if (!sh)
                        continue;
                    for (const ShaderResource& r : sh->iface.resources) {
                        if (r.set != set || r.binding != e.binding || !KindMatches(e.type, r.kind))
                            continue;
                        used = true;
                        vis |= StageBits(sh->desc.stage);
                        entry.depth |= r.depth;
                        entry.comparison |= r.comparison;
                    }
                }
                const bool dynamic = e.type == BindingType::UniformBufferDynamic;
                if (used) {
                    if (dynamic)
                        pick.push_back(rhiDynamic);
                    chosen.push_back(entry);
                    visibility.push_back(vis);
                }
                if (dynamic)
                    rhiDynamic++;
            }
        }

        std::string key;
        for (size_t i = 0; i < chosen.size(); i++) {
            const NativeSet::Entry& e = chosen[i];
            key += std::to_string(e.rhi.binding) + ':' + std::to_string(static_cast<int>(e.rhi.type)) + ':' + std::to_string(visibility[i]) + ':' +
                   std::to_string(static_cast<int>(e.rhi.viewDim)) + (e.rhi.filterable ? 'f' : 'n') + (e.depth ? 'd' : '-') + (e.comparison ? 'c' : '-') + ';';
        }
        auto made = CachedLayout(key, [&]() -> std::shared_ptr<NativeSet> {
            auto result = std::make_shared<NativeSet>();
            result->entries = chosen;
            std::vector<WGPUBindGroupLayoutEntry> native(chosen.size());
            for (size_t i = 0; i < chosen.size(); i++) {
                const BindLayoutEntry& e = chosen[i].rhi;
                WGPUBindGroupLayoutEntry& n = native[i];
                n = {};
                n.binding = e.binding;
                n.visibility = visibility[i];
                switch (e.type) {
                case BindingType::UniformBuffer:
                case BindingType::UniformBufferDynamic:
                    n.buffer.type = WGPUBufferBindingType_Uniform;
                    n.buffer.hasDynamicOffset = e.type == BindingType::UniformBufferDynamic;
                    break;
                case BindingType::StorageBuffer: n.buffer.type = WGPUBufferBindingType_ReadOnlyStorage; break;
                case BindingType::StorageBufferRW: n.buffer.type = WGPUBufferBindingType_Storage; break;
                case BindingType::SampledTexture:
                    n.texture.sampleType = chosen[i].depth ? WGPUTextureSampleType_Depth
                                           : e.filterable ? WGPUTextureSampleType_Float
                                                          : WGPUTextureSampleType_UnfilterableFloat;
                    n.texture.viewDimension = ToViewDimension(e.viewDim);
                    break;
                case BindingType::Sampler:
                    n.sampler.type = chosen[i].comparison ? WGPUSamplerBindingType_Comparison
                                     : e.filterable       ? WGPUSamplerBindingType_Filtering
                                                          : WGPUSamplerBindingType_NonFiltering;
                    break;
                case BindingType::StorageTextureRW:
                    n.storageTexture.access = WGPUStorageTextureAccess_ReadWrite;
                    n.storageTexture.format = WGPUTextureFormat_RGBA8Unorm;
                    n.storageTexture.viewDimension = ToViewDimension(e.viewDim);
                    break;
                }
            }
            WGPUBindGroupLayoutDescriptor ld{};
            ld.label = View("pipeline set");
            ld.entryCount = native.size();
            ld.entries = native.data();
            result->layout = Owned<WGPUBindGroupLayout>(wgpuDeviceCreateBindGroupLayout(device.Get(), &ld));
            return result;
        });
        if (!made || !made->layout)
            return nullptr;
        // the cached object is shared by key; the dynamic bookkeeping belongs to this pipeline's rhi layout, so copy it out
        auto mine = std::make_shared<NativeSet>();
        mine->entries = made->entries;
        mine->dynamicPick = pick;
        mine->rhiDynamicCount = rhiDynamic;
        WGPUBindGroupLayout raw = made->layout.Get();
        wgpuBindGroupLayoutAddRef(raw);
        mine->layout = Owned<WGPUBindGroupLayout>(raw);
        return mine;
    }

    WGPUBindGroup WebGpuDevice::NativeGroupFor(BindGroupHandle handle, const NativeSet& set) {
        BindGroup* g = bindGroups.Get(handle);
        if (!g)
            return nullptr;
        if (const auto it = g->native.find(set.layout.Get()); it != g->native.end())
            return it->second.Get();
        std::vector<WGPUBindGroupEntry> entries(set.entries.size());
        for (size_t i = 0; i < set.entries.size(); i++) {
            const BindLayoutEntry& le = set.entries[i].rhi;
            WGPUBindGroupEntry& n = entries[i];
            n = {};
            n.binding = le.binding;
            const BindGroupEntry* src = nullptr;
            for (const BindGroupEntry& e : g->entries)
                if (e.binding == le.binding && e.arrayIndex == 0) {
                    src = &e;
                    break;
                }
            if (!src) {
                ReportError("bind group lacks binding " + std::to_string(le.binding));
                return nullptr;
            }
            switch (le.type) {
            case BindingType::UniformBuffer:
            case BindingType::UniformBufferDynamic:
            case BindingType::StorageBuffer:
            case BindingType::StorageBufferRW: {
                Buffer* b = buffers.Get(src->buffer.buffer);
                if (!b) {
                    ReportError("bind group buffer at binding " + std::to_string(le.binding) + " is gone");
                    return nullptr;
                }
                n.buffer = b->buffer.Get();
                n.offset = src->buffer.offset;
                n.size = src->buffer.size ? src->buffer.size : b->gpuSize - src->buffer.offset;
                break;
            }
            case BindingType::SampledTexture:
            case BindingType::StorageTextureRW: {
                Texture* t = textures.Get(src->texture.texture);
                if (!t) {
                    ReportError("bind group texture at binding " + std::to_string(le.binding) + " is gone");
                    return nullptr;
                }
                n.textureView = ViewOf(*t, src->texture.range, le.viewDim, src->texture.format, src->texture.stencil, false);
                break;
            }
            case BindingType::Sampler: {
                Sampler* s = samplers.Get(src->sampler);
                if (!s) {
                    ReportError("bind group sampler at binding " + std::to_string(le.binding) + " is gone");
                    return nullptr;
                }
                // a layout that promises a nearest sampler cannot take a filtering one
                n.sampler = (!le.filterable && s->linear && !set.entries[i].comparison) ? nearestSampler.Get() : s->sampler.Get();
                break;
            }
            }
        }
        WGPUBindGroupDescriptor bd{};
        bd.label = View("bind group");
        bd.layout = set.layout.Get();
        bd.entryCount = entries.size();
        bd.entries = entries.data();
        WGPUBindGroup made = wgpuDeviceCreateBindGroup(device.Get(), &bd);
        if (!made)
            return nullptr;
        g->native.emplace(set.layout.Get(), Owned<WGPUBindGroup>(made));
        return made;
    }

    // ---- surface textures ---------------------------------------------------------------------------------------------

    TextureHandle WebGpuDevice::CreateSurfaceTexture(Format format, uint32_t width, uint32_t height, bool sampled) {
        Texture t;
        t.external = true;
        t.desc.format = format;
        t.desc.width = width;
        t.desc.height = height;
        t.desc.usage = TextureUsage::ColorTarget | TextureUsage::CopySrc | (sampled ? TextureUsage::Sampled : TextureUsage::None);
        t.name = "swapchain image";
        return textures.Alloc(std::move(t));
    }

    void WebGpuDevice::SetSurfaceTexture(TextureHandle handle, WGPUTexture texture) {
        Texture* t = textures.Get(handle);
        if (!t)
            return;
        {
            std::lock_guard lock(t->viewMutex);
            t->views.clear();
        }
        t->texture = Owned<WGPUTexture>(texture);
    }

    void WebGpuDevice::ResizeSurfaceTexture(TextureHandle handle, uint32_t width, uint32_t height) {
        if (Texture* t = textures.Get(handle)) {
            t->desc.width = width;
            t->desc.height = height;
        }
    }
}
