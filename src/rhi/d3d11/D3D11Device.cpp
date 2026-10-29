#include "rhi/d3d11/D3D11Internal.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace rhi::d3d11 {
    namespace {
        // D3DCOMPILE_OPTIMIZATION_LEVEL3
        constexpr UINT kOptimizeFull = 1u << 15;

        bool IsIntegerFormat(Format f) {
            return f == Format::R8Uint || f == Format::R16Uint || f == Format::R32Uint || f == Format::RG32Uint || f == Format::RGBA32Uint;
        }

        int ClassIndex(char c) { return c == 'b' ? 0 : c == 't' ? 1 : c == 's' ? 2 : 3; }
    }

    Device::Device(std::shared_ptr<InstanceState> instance, winrt::com_ptr<IDXGIAdapter1> adapterIn, const AdapterInfo& info,
                   const DeviceOptions& opts)
        : inst(std::move(instance)), adapter(std::move(adapterIn)), options(opts) {
        caps.adapter = info;
        options.framesInFlight = std::clamp(options.framesInFlight, 1u, 4u);
    }

    Device::~Device() {
        if (!device)
            return;
        WaitIdle();
        lists.clear();
        freeQueries.clear();
        if (immediate) {
            immediate->ClearState();
            immediate->Flush();
        }
    }

    void Device::ReportError(const std::string& message) {
        std::lock_guard lock(errorMutex);
        std::fprintf(stderr, "[d3d11] %s\n", message.c_str());
    }

    NativeHandles Device::Native() const {
        NativeHandles n;
        n.api = Api::D3D11;
        n.device = device.get();
        n.adapter = adapter.get();
        n.factory = inst->factory.get();
        return n;
    }

    bool Device::Init(std::string& error) {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        if (inst->validation)
            flags |= D3D11_CREATE_DEVICE_DEBUG;
        const D3D_FEATURE_LEVEL both[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        const D3D_FEATURE_LEVEL only11[] = {D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL got{};
        HRESULT hr = E_FAIL;
        // the debug layer is an optional windows feature, and runtimes before 11.1 reject 11_1 in the list
        for (const bool debug : {true, false}) {
            if (debug && !(flags & D3D11_CREATE_DEVICE_DEBUG))
                continue;
            const UINT f = debug ? flags : (flags & ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG));
            for (const auto& levels : {std::span<const D3D_FEATURE_LEVEL>(both), std::span<const D3D_FEATURE_LEVEL>(only11)}) {
                device = nullptr;
                immediate = nullptr;
                hr = inst->lib.createDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, f, levels.data(), static_cast<UINT>(levels.size()),
                                            D3D11_SDK_VERSION, device.put(), &got, immediate.put());
                if (SUCCEEDED(hr))
                    break;
            }
            if (SUCCEEDED(hr))
                break;
        }
        if (FAILED(hr)) {
            error = "D3D11CreateDevice failed";
            return false;
        }
        device1 = device.try_as<ID3D11Device1>();
        if (device1) {
            D3D11_FEATURE_DATA_D3D11_OPTIONS options11{};
            if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options11, sizeof(options11))))
                constantOffsetting = options11.ConstantBufferOffsetting != FALSE;
        }
        FillCaps();
        frameFence = CreateFence(0);
        return true;
    }

    void Device::FillCaps() {
        DeviceLimits& l = caps.limits;
        l.maxTexture2D = 16384;
        l.maxTextureLayers = 2048;
        l.maxBindGroups = kMaxBindGroups;
        l.maxPushConstantBytes = 0;
        l.maxSampledTexturesPerStage = 128;
        l.maxColorTargets = 8;
        l.uniformBufferAlignment = 256;
        l.storageBufferAlignment = 16;
        // UpdateSubresource takes any pitch and offset
        l.copyRowPitchAlignment = 4;
        l.copyOffsetAlignment = 4;
        l.maxBufferSize = 1ull << 30;
        l.maxAnisotropy = 16.0f;
        l.maxMsaaSamples = 1;
        for (const UINT samples : {8u, 4u, 2u}) {
            UINT levels = 0;
            if (SUCCEEDED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, samples, &levels)) && levels > 0) {
                l.maxMsaaSamples = samples;
                break;
            }
        }

        DeviceFeatures& f = caps.features;
        f.samplerAnisotropy = true;
        f.textureCompressionBC = true;
        f.depthClipControl = true;
        f.drawIndirectFirstInstance = true;
        f.unifiedMemory = caps.adapter.type == AdapterType::Integrated || caps.adapter.type == AdapterType::Cpu;
        f.tearing = inst->tearing;
        // one immediate context: no async compute, no multi draw, no count buffers
        f.multiDrawIndirect = false;
        f.drawIndirectCount = false;
        f.asyncCompute = false;
        f.dedicatedCopyQueue = false;
        f.timestampQueries = false;

        for (uint32_t i = 1; i < static_cast<uint32_t>(Format::Count); i++) {
            const Format fmt = static_cast<Format>(i);
            const FormatMap fm = MapFormat(fmt);
            if (fm.view == DXGI_FORMAT_UNKNOWN)
                continue;
            UINT s = 0;
            device->CheckFormatSupport(fm.view, &s);
            FormatUse use = FormatUse::None;
            if (s & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)
                use = use | FormatUse::Sampled | (IsIntegerFormat(fmt) ? FormatUse::None : FormatUse::SampledLinear);
            if (s & D3D11_FORMAT_SUPPORT_RENDER_TARGET)
                use = use | FormatUse::ColorTarget;
            if (s & D3D11_FORMAT_SUPPORT_BLENDABLE)
                use = use | FormatUse::Blendable;
            if (s & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)
                use = use | FormatUse::Storage;
            if (s & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET)
                use = use | FormatUse::Msaa;
            if (fm.dsv != DXGI_FORMAT_UNKNOWN) {
                UINT d = 0;
                device->CheckFormatSupport(fm.dsv, &d);
                if (d & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL)
                    use = use | FormatUse::DepthTarget;
                if (d & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET)
                    use = use | FormatUse::Msaa;
            }
            caps.formats[i] = use;
        }
    }

    // ---- resources ----------------------------------------------------------------------------------------------------

    BufferHandle Device::CreateBuffer(const BufferDesc& desc) {
        Buffer b;
        b.desc = desc;
        b.name = desc.name ? desc.name : "";
        const bool storage = Has(desc.usage, BufferUsage::Storage);
        const bool indirect = Has(desc.usage, BufferUsage::Indirect);
        b.structured = storage && desc.stride > 0 && !indirect;
        uint64_t size = AlignUp(std::max<uint64_t>(desc.size, 16), 16);
        if (Has(desc.usage, BufferUsage::Uniform))
            size = AlignUp(size, 256);
        if (b.structured)
            size = AlignUp(size, desc.stride);
        b.gpuSize = size;

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = static_cast<UINT>(size);
        if (desc.memory == MemoryClass::Readback) {
            bd.Usage = D3D11_USAGE_STAGING;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        } else {
            bd.Usage = D3D11_USAGE_DEFAULT;
            if (Has(desc.usage, BufferUsage::Vertex))
                bd.BindFlags |= D3D11_BIND_VERTEX_BUFFER;
            if (Has(desc.usage, BufferUsage::Index))
                bd.BindFlags |= D3D11_BIND_INDEX_BUFFER;
            if (storage)
                bd.BindFlags |= D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            if (indirect)
                bd.MiscFlags |= D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
            if (b.structured) {
                bd.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
                bd.StructureByteStride = desc.stride;
            } else if (storage) {
                bd.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            }
        }
        if (FAILED(device->CreateBuffer(&bd, nullptr, b.main.put())))
            return {};

        if (Has(desc.usage, BufferUsage::Uniform) && constantOffsetting && desc.memory != MemoryClass::Readback) {
            D3D11_BUFFER_DESC cd{};
            cd.ByteWidth = static_cast<UINT>(size);
            cd.Usage = D3D11_USAGE_DEFAULT;
            cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            if (FAILED(device->CreateBuffer(&cd, nullptr, b.constant.put())))
                return {};
        }
        if (desc.memory != MemoryClass::GpuOnly)
            b.shadow.assign(size, 0);
        stats.bufferBytes += size * (b.constant ? 2 : 1);
        stats.liveBuffers++;
        const BufferHandle h = buffers.Alloc(std::move(b));
        buffers.Get(h)->desc.name = buffers.Get(h)->name.c_str();
        return h;
    }

    TextureHandle Device::CreateTexture(const TextureDesc& desc) {
        const FormatMap fm = MapFormat(desc.format);
        if (fm.resource == DXGI_FORMAT_UNKNOWN) {
            ReportError("CreateTexture: unsupported format " + std::to_string(static_cast<int>(desc.format)));
            return {};
        }
        Texture t;
        t.desc = desc;
        t.desc.mips = std::max(1u, desc.mips);
        t.desc.samples = std::max(1u, desc.samples);
        t.formats = fm;
        t.name = desc.name ? desc.name : "";

        UINT bind = 0;
        if (Has(desc.usage, TextureUsage::Sampled))
            bind |= D3D11_BIND_SHADER_RESOURCE;
        if (Has(desc.usage, TextureUsage::ColorTarget))
            bind |= D3D11_BIND_RENDER_TARGET;
        if (Has(desc.usage, TextureUsage::DepthTarget))
            bind |= D3D11_BIND_DEPTH_STENCIL;
        if (Has(desc.usage, TextureUsage::Storage))
            bind |= D3D11_BIND_UNORDERED_ACCESS;

        if (desc.dim == TextureDim::Tex3D) {
            D3D11_TEXTURE3D_DESC td{};
            td.Width = desc.width;
            td.Height = desc.height;
            td.Depth = std::max(1u, desc.depthOrLayers);
            td.MipLevels = t.desc.mips;
            td.Format = fm.resource;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = bind;
            winrt::com_ptr<ID3D11Texture3D> tex;
            if (FAILED(device->CreateTexture3D(&td, nullptr, tex.put())))
                return {};
            t.resource.copy_from(tex.get());
        } else {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = desc.width;
            td.Height = desc.height;
            td.MipLevels = t.desc.samples > 1 ? 1 : t.desc.mips;
            td.ArraySize = LayerCountOf(t.desc);
            td.Format = fm.resource;
            td.SampleDesc.Count = t.desc.samples;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = bind;
            td.MiscFlags = desc.dim == TextureDim::Cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
            winrt::com_ptr<ID3D11Texture2D> tex;
            if (FAILED(device->CreateTexture2D(&td, nullptr, tex.put())))
                return {};
            t.resource.copy_from(tex.get());
        }
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

    SamplerHandle Device::CreateSampler(const SamplerDesc& desc) {
        Sampler s;
        s.desc = desc;
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = ToD3D(desc);
        sd.AddressU = ToD3D(desc.u);
        sd.AddressV = ToD3D(desc.v);
        sd.AddressW = ToD3D(desc.w);
        sd.MipLODBias = desc.lodBias;
        sd.MaxAnisotropy = static_cast<UINT>(std::clamp(desc.maxAnisotropy, 1.0f, 16.0f));
        sd.ComparisonFunc = desc.compare ? ToD3D(desc.compareOp) : D3D11_COMPARISON_NEVER;
        for (int i = 0; i < 4; i++)
            sd.BorderColor[i] = desc.border[i];
        sd.MinLOD = desc.minLod;
        sd.MaxLOD = std::min(desc.maxLod, 1000.0f);
        if (FAILED(device->CreateSamplerState(&sd, s.state.put())))
            return {};
        return samplers.Alloc(std::move(s));
    }

    ShaderHandle Device::CreateShader(const ShaderDesc& desc) {
        if (desc.format != ShaderFormat::Hlsl) {
            ReportError("CreateShader: d3d11 takes hlsl source");
            return {};
        }
        Shader s;
        s.stage = desc.stage;
        s.source.assign(reinterpret_cast<const char*>(desc.code.data()), desc.code.size());
        s.entry = desc.entry ? desc.entry : "main";
        s.iface = ReflectShader(ShaderFormat::Hlsl, s.source);
        return shaders.Alloc(std::move(s));
    }

    BindLayoutHandle Device::CreateBindLayout(const BindLayoutDesc& desc) {
        BindLayout l;
        l.entries.assign(desc.entries.begin(), desc.entries.end());
        std::sort(l.entries.begin(), l.entries.end(), [](const BindLayoutEntry& a, const BindLayoutEntry& b) { return a.binding < b.binding; });
        return bindLayouts.Alloc(std::move(l));
    }

    BindGroupHandle Device::CreateBindGroup(const BindGroupDesc& desc) {
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

    PipelineLayoutHandle Device::CreatePipelineLayout(const PipelineLayoutDesc& desc) {
        if (desc.pushConstantBytes)
            ReportError("CreatePipelineLayout: d3d11 has no push constants");
        PipelineLayout l;
        l.desc = desc;
        l.desc.name = nullptr;
        return pipelineLayouts.Alloc(std::move(l));
    }

    QuerySetHandle Device::CreateQuerySet(const QuerySetDesc& desc) {
        Query q;
        q.count = desc.count;
        return queries.Alloc(std::move(q));
    }

    void Device::Destroy(BufferHandle h) {
        Buffer b;
        if (!buffers.Free(h, &b))
            return;
        stats.bufferBytes -= b.gpuSize * (b.constant ? 2 : 1);
        stats.liveBuffers--;
    }

    void Device::Destroy(TextureHandle h) {
        Texture t;
        if (!textures.Free(h, &t))
            return;
        if (!t.external)
            stats.liveTextures--;
    }

    void Device::Destroy(SamplerHandle h) { samplers.Free(h); }
    void Device::Destroy(ShaderHandle h) { shaders.Free(h); }
    void Device::Destroy(BindLayoutHandle h) { bindLayouts.Free(h); }
    void Device::Destroy(BindGroupHandle h) {
        if (bindGroups.Free(h))
            stats.liveBindGroups--;
    }
    void Device::Destroy(PipelineLayoutHandle h) { pipelineLayouts.Free(h); }
    void Device::Destroy(PipelineHandle h) {
        if (pipelines.Free(h))
            stats.livePipelines--;
    }
    void Device::Destroy(QuerySetHandle h) { queries.Free(h); }
    void Device::Destroy(FenceHandle h) { fences.Free(h); }

    const BufferDesc* Device::GetDesc(BufferHandle h) const {
        const Buffer* b = buffers.Get(h);
        return b ? &b->desc : nullptr;
    }

    const TextureDesc* Device::GetDesc(TextureHandle h) const {
        const Texture* t = textures.Get(h);
        return t ? &t->desc : nullptr;
    }

    uint8_t* Device::Map(BufferHandle handle) {
        Buffer* b = buffers.Get(handle);
        return b && !b->shadow.empty() ? b->shadow.data() : nullptr;
    }

    void Device::Flush(BufferHandle handle, uint64_t offset, uint64_t size) {
        Buffer* b = buffers.Get(handle);
        if (!b || b->shadow.empty() || b->desc.memory == MemoryClass::Readback)
            return;
        const uint64_t lo = offset & ~3ull;
        const uint64_t hi = std::min<uint64_t>(b->gpuSize, AlignUp(offset + size, 4));
        if (hi <= lo)
            return;
        D3D11_BOX box{static_cast<UINT>(lo), 0, 0, static_cast<UINT>(hi), 1, 1};
        auto lock = LockContext();
        immediate->UpdateSubresource(b->main.get(), 0, &box, b->shadow.data() + lo, 0, 0);
        if (b->constant)
            immediate->CopySubresourceRegion(b->constant.get(), 0, static_cast<UINT>(lo), 0, 0, b->main.get(), 0, &box);
    }

    void Device::Invalidate(BufferHandle handle, uint64_t, uint64_t) {
        Buffer* b = buffers.Get(handle);
        if (!b || b->desc.memory != MemoryClass::Readback)
            return;
        auto lock = LockContext();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(immediate->Map(b->main.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            std::memcpy(b->shadow.data(), mapped.pData, b->shadow.size());
            immediate->Unmap(b->main.get(), 0);
        }
        std::vector<PendingReadback> pending;
        {
            std::lock_guard views(viewMutex);
            pending.swap(b->pending);
        }
        for (PendingReadback& p : pending) {
            if (FAILED(immediate->Map(p.staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
                continue;
            for (uint32_t y = 0; y < p.rows; y++) {
                const uint64_t at = p.bufferOffset + static_cast<uint64_t>(y) * p.bufferRowPitch;
                if (at + p.rowBytes <= b->shadow.size())
                    std::memcpy(b->shadow.data() + at, static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch, p.rowBytes);
            }
            immediate->Unmap(p.staging.get(), 0);
        }
    }

    // ---- views --------------------------------------------------------------------------------------------------------

    ID3D11ShaderResourceView* Device::SrvOf(Texture& t, const SubresourceRange& range, TextureDim dim, Format format, bool stencil) {
        const SubresourceRange r = ResolveRange(range, t.desc);
        ViewKey key;
        key.kind = 0;
        key.dim = static_cast<uint8_t>(dim);
        key.format = static_cast<uint16_t>(format) | (stencil ? 0x8000 : 0);
        key.baseMip = r.baseMip;
        key.mipCount = r.mipCount;
        key.baseLayer = r.baseLayer;
        key.layerCount = r.layerCount;
        std::lock_guard lock(viewMutex);
        if (const auto it = t.views.find(key); it != t.views.end())
            return static_cast<ID3D11ShaderResourceView*>(it->second.get());

        D3D11_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = format == Format::Unknown ? (stencil ? t.formats.stencilView : t.formats.view) : MapFormat(format).view;
        const bool ms = t.desc.samples > 1;
        switch (dim) {
        case TextureDim::Tex2D:
            if (ms) {
                d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
            } else if (r.baseLayer > 0) {
                d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                d.Texture2DArray = {r.baseMip, r.mipCount, r.baseLayer, 1};
            } else {
                d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                d.Texture2D = {r.baseMip, r.mipCount};
            }
            break;
        case TextureDim::Tex2DArray:
            if (ms) {
                d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
                d.Texture2DMSArray = {r.baseLayer, r.layerCount};
            } else {
                d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                d.Texture2DArray = {r.baseMip, r.mipCount, r.baseLayer, r.layerCount};
            }
            break;
        case TextureDim::Cube:
            d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            d.TextureCube = {r.baseMip, r.mipCount};
            break;
        case TextureDim::Tex3D:
            d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
            d.Texture3D = {r.baseMip, r.mipCount};
            break;
        }
        winrt::com_ptr<ID3D11ShaderResourceView> view;
        if (FAILED(device->CreateShaderResourceView(t.resource.get(), &d, view.put())))
            return nullptr;
        t.views[key].copy_from(view.get());
        return view.get();
    }

    ID3D11UnorderedAccessView* Device::UavOf(Texture& t, const SubresourceRange& range, TextureDim dim) {
        const SubresourceRange r = ResolveRange(range, t.desc);
        ViewKey key;
        key.kind = 3;
        key.dim = static_cast<uint8_t>(dim);
        key.baseMip = r.baseMip;
        key.baseLayer = r.baseLayer;
        key.layerCount = r.layerCount;
        std::lock_guard lock(viewMutex);
        if (const auto it = t.views.find(key); it != t.views.end())
            return static_cast<ID3D11UnorderedAccessView*>(it->second.get());
        D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = t.formats.view;
        if (t.desc.dim == TextureDim::Tex3D) {
            d.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
            d.Texture3D = {r.baseMip, 0, std::max(1u, DepthOf(t.desc) >> r.baseMip)};
        } else if (dim == TextureDim::Tex2D && r.baseLayer == 0 && LayerCountOf(t.desc) == 1) {
            d.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            d.Texture2D = {r.baseMip};
        } else {
            d.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray = {r.baseMip, r.baseLayer, dim == TextureDim::Tex2D ? 1u : r.layerCount};
        }
        winrt::com_ptr<ID3D11UnorderedAccessView> view;
        if (FAILED(device->CreateUnorderedAccessView(t.resource.get(), &d, view.put())))
            return nullptr;
        t.views[key].copy_from(view.get());
        return view.get();
    }

    ID3D11RenderTargetView* Device::RtvOf(Texture& t, uint32_t mip, uint32_t layer) {
        ViewKey key;
        key.kind = 1;
        key.baseMip = mip;
        key.baseLayer = layer;
        std::lock_guard lock(viewMutex);
        if (const auto it = t.views.find(key); it != t.views.end())
            return static_cast<ID3D11RenderTargetView*>(it->second.get());
        D3D11_RENDER_TARGET_VIEW_DESC d{};
        d.Format = t.formats.view;
        const bool ms = t.desc.samples > 1;
        const bool array = LayerCountOf(t.desc) > 1 || layer > 0;
        if (t.desc.dim == TextureDim::Tex3D) {
            d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
            d.Texture3D = {mip, layer, ~0u};
        } else if (ms) {
            d.ViewDimension = array ? D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY : D3D11_RTV_DIMENSION_TEXTURE2DMS;
            if (array)
                d.Texture2DMSArray = {layer, 1};
        } else if (array) {
            d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray = {mip, layer, 1};
        } else {
            d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            d.Texture2D = {mip};
        }
        winrt::com_ptr<ID3D11RenderTargetView> view;
        if (FAILED(device->CreateRenderTargetView(t.resource.get(), &d, view.put())))
            return nullptr;
        t.views[key].copy_from(view.get());
        return view.get();
    }

    ID3D11DepthStencilView* Device::DsvOf(Texture& t, uint32_t mip, uint32_t layer, bool readOnly) {
        ViewKey key;
        key.kind = readOnly ? 5 : 2;
        key.baseMip = mip;
        key.baseLayer = layer;
        std::lock_guard lock(viewMutex);
        if (const auto it = t.views.find(key); it != t.views.end())
            return static_cast<ID3D11DepthStencilView*>(it->second.get());
        D3D11_DEPTH_STENCIL_VIEW_DESC d{};
        d.Format = t.formats.dsv;
        if (readOnly)
            d.Flags = D3D11_DSV_READ_ONLY_DEPTH | (HasStencil(t.desc.format) ? D3D11_DSV_READ_ONLY_STENCIL : 0);
        const bool ms = t.desc.samples > 1;
        const bool array = LayerCountOf(t.desc) > 1 || layer > 0;
        if (ms) {
            d.ViewDimension = array ? D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY : D3D11_DSV_DIMENSION_TEXTURE2DMS;
            if (array)
                d.Texture2DMSArray = {layer, 1};
        } else if (array) {
            d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray = {mip, layer, 1};
        } else {
            d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            d.Texture2D = {mip};
        }
        winrt::com_ptr<ID3D11DepthStencilView> view;
        if (FAILED(device->CreateDepthStencilView(t.resource.get(), &d, view.put())))
            return nullptr;
        t.views[key].copy_from(view.get());
        return view.get();
    }

    ID3D11ShaderResourceView* Device::BufferSrv(Buffer& b, uint64_t offset, uint64_t size) {
        if (!size || offset + size > b.gpuSize)
            size = b.gpuSize - offset;
        const auto key = std::make_tuple(offset, size, uint8_t{0});
        std::lock_guard lock(viewMutex);
        if (const auto it = b.views.find(key); it != b.views.end())
            return static_cast<ID3D11ShaderResourceView*>(it->second.get());
        D3D11_SHADER_RESOURCE_VIEW_DESC d{};
        if (b.structured) {
            d.Format = DXGI_FORMAT_UNKNOWN;
            d.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            d.Buffer.FirstElement = static_cast<UINT>(offset / b.desc.stride);
            d.Buffer.NumElements = static_cast<UINT>(size / b.desc.stride);
        } else {
            d.Format = DXGI_FORMAT_R32_TYPELESS;
            d.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
            d.BufferEx.FirstElement = static_cast<UINT>(offset / 4);
            d.BufferEx.NumElements = static_cast<UINT>(size / 4);
            d.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        }
        winrt::com_ptr<ID3D11ShaderResourceView> view;
        if (FAILED(device->CreateShaderResourceView(b.main.get(), &d, view.put())))
            return nullptr;
        b.views[key].copy_from(view.get());
        return view.get();
    }

    ID3D11UnorderedAccessView* Device::BufferUav(Buffer& b, uint64_t offset, uint64_t size) {
        if (!size || offset + size > b.gpuSize)
            size = b.gpuSize - offset;
        const auto key = std::make_tuple(offset, size, uint8_t{1});
        std::lock_guard lock(viewMutex);
        if (const auto it = b.views.find(key); it != b.views.end())
            return static_cast<ID3D11UnorderedAccessView*>(it->second.get());
        D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
        d.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        if (b.structured) {
            d.Format = DXGI_FORMAT_UNKNOWN;
            d.Buffer.FirstElement = static_cast<UINT>(offset / b.desc.stride);
            d.Buffer.NumElements = static_cast<UINT>(size / b.desc.stride);
        } else {
            d.Format = DXGI_FORMAT_R32_TYPELESS;
            d.Buffer.FirstElement = static_cast<UINT>(offset / 4);
            d.Buffer.NumElements = static_cast<UINT>(size / 4);
            d.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        }
        winrt::com_ptr<ID3D11UnorderedAccessView> view;
        if (FAILED(device->CreateUnorderedAccessView(b.main.get(), &d, view.put())))
            return nullptr;
        b.views[key].copy_from(view.get());
        return view.get();
    }

    TextureHandle Device::WrapBackbuffer(ID3D11Texture2D* texture, const TextureDesc& desc) {
        Texture t;
        t.resource.copy_from(texture);
        t.desc = desc;
        t.formats = MapFormat(desc.format);
        t.external = true;
        t.name = "swapchain image";
        return textures.Alloc(std::move(t));
    }

    void Device::SetBackbuffer(TextureHandle handle, ID3D11Texture2D* texture) {
        Texture* t = textures.Get(handle);
        if (!t)
            return;
        std::lock_guard lock(viewMutex);
        t->views.clear();
        t->resource.copy_from(texture);
    }

    void Device::ReleaseBackbuffer(TextureHandle handle) { Destroy(handle); }

    // ---- shaders and pipelines ----------------------------------------------------------------------------------------

    // slang numbers registers by rhi binding and uses spaces for sets; d3d11 has neither, and only 16 sampler slots
    bool Device::CompileStage(const Shader& sh, const PipelineLayoutDesc& layout, uint32_t stageIndex, std::vector<StageSlot>& slots,
                              winrt::com_ptr<ID3DBlob>& blob, std::string& error) {
        std::vector<const ShaderResource*> resources;
        for (const ShaderResource& r : sh.iface.resources)
            resources.push_back(&r);
        std::sort(resources.begin(), resources.end(), [](const ShaderResource* a, const ShaderResource* b) {
            return std::tie(a->set, a->binding, a->registerClass) < std::tie(b->set, b->binding, b->registerClass);
        });

        const uint32_t limits[4] = {14, 128, 16, stageIndex == kStageCompute ? 64u : 8u};
        uint32_t next[4] = {0, 0, 0, 0};
        struct Edit {
            size_t begin, end;
            std::string text;
        };
        std::vector<Edit> edits;
        for (const ShaderResource* r : resources) {
            const int ci = ClassIndex(r->registerClass);
            if (next[ci] >= limits[ci]) {
                error = std::string("too many '") + r->registerClass + "' registers in " + sh.entry;
                return false;
            }
            StageSlot s;
            s.set = r->set;
            s.binding = r->binding;
            s.kind = r->kind;
            s.dim = r->dim;
            s.registerClass = r->registerClass;
            s.slot = next[ci]++;
            if (r->set < kMaxBindGroups)
                if (const BindLayout* l = bindLayouts.Get(layout.sets[r->set])) {
                    uint32_t dynamic = 0;
                    for (const BindLayoutEntry& e : l->entries) {
                        if (e.binding == r->binding) {
                            if (e.type == BindingType::UniformBufferDynamic)
                                s.dynamicIndex = dynamic;
                            break;
                        }
                        if (e.type == BindingType::UniformBufferDynamic)
                            dynamic++;
                    }
                }
            slots.push_back(s);
            edits.push_back({r->clauseBegin, r->clauseEnd, std::string("register(") + r->registerClass + std::to_string(s.slot) + ")"});
        }

        std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.begin > b.begin; });
        std::string patched = sh.source;
        for (const Edit& e : edits)
            patched.replace(e.begin, e.end - e.begin, e.text);

        const char* profile = stageIndex == kStageVertex ? "vs_5_0" : stageIndex == kStagePixel ? "ps_5_0" : "cs_5_0";
        const std::string key = std::string(profile) + '|' + sh.entry + '|' + patched;
        {
            std::lock_guard lock(shaderCacheMutex);
            if (const auto it = blobCache.find(key); it != blobCache.end()) {
                blob = it->second;
                return true;
            }
        }
        if (!inst->lib.compile) {
            error = "d3dcompiler_47.dll is not available";
            return false;
        }
        winrt::com_ptr<ID3DBlob> errors;
        const HRESULT hr = inst->lib.compile(patched.data(), patched.size(), sh.entry.c_str(), nullptr, nullptr, sh.entry.c_str(), profile,
                                             kOptimizeFull, 0, blob.put(), errors.put());
        if (FAILED(hr)) {
            error = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "D3DCompile failed";
            return false;
        }
        std::lock_guard lock(shaderCacheMutex);
        blobCache[key] = blob;
        return true;
    }

    PipelineHandle Device::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) {
        const Shader* vs = shaders.Get(desc.vertex);
        const Shader* ps = shaders.Get(desc.fragment);
        const PipelineLayout* layout = pipelineLayouts.Get(desc.layout);
        if (!vs || !layout) {
            ReportError("CreateGraphicsPipeline: missing shader or layout");
            return {};
        }
        Pipeline p;
        std::string error;
        winrt::com_ptr<ID3DBlob> vsBlob, psBlob;
        if (!CompileStage(*vs, layout->desc, kStageVertex, p.stages[kStageVertex], vsBlob, error) ||
            (ps && !CompileStage(*ps, layout->desc, kStagePixel, p.stages[kStagePixel], psBlob, error))) {
            ReportError(std::string("shader compile failed (") + (desc.name ? desc.name : "?") + "): " + error);
            return {};
        }
        if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, p.vs.put())))
            return {};
        if (psBlob && FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, p.ps.put())))
            return {};

        std::array<D3D11_INPUT_ELEMENT_DESC, kMaxVertexAttributes> elements{};
        for (uint32_t i = 0; i < desc.attributeCount; i++) {
            const VertexAttributeDesc& a = desc.attributes[i];
            D3D11_INPUT_ELEMENT_DESC& e = elements[i];
            e.SemanticName = "TEXCOORD";
            e.SemanticIndex = a.location;
            e.Format = MapFormat(a.format).view;
            e.InputSlot = a.stream;
            e.AlignedByteOffset = a.offset;
            const bool instance = a.stream < desc.streamCount && desc.streams[a.stream].step == VertexStep::Instance;
            e.InputSlotClass = instance ? D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA;
            e.InstanceDataStepRate = instance ? 1 : 0;
        }
        if (desc.attributeCount &&
            FAILED(device->CreateInputLayout(elements.data(), desc.attributeCount, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                             p.inputLayout.put()))) {
            ReportError(std::string("input layout does not match the vertex shader (") + (desc.name ? desc.name : "?") + ")");
            return {};
        }
        p.streamCount = desc.streamCount;
        for (uint32_t i = 0; i < desc.streamCount; i++)
            p.strides[i] = desc.streams[i].stride;
        p.topology = ToD3D(desc.topology);

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = desc.raster.wireframe ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
        rd.CullMode = desc.raster.cull == CullMode::None ? D3D11_CULL_NONE : desc.raster.cull == CullMode::Back ? D3D11_CULL_BACK : D3D11_CULL_FRONT;
        rd.FrontCounterClockwise = desc.raster.frontFace == FrontFace::CounterClockwise;
        rd.DepthBias = static_cast<INT>(std::lround(desc.raster.depthBiasConstant));
        rd.DepthBiasClamp = desc.raster.depthBiasClamp;
        rd.SlopeScaledDepthBias = desc.raster.depthBiasSlope;
        rd.DepthClipEnable = desc.raster.depthClip;
        rd.ScissorEnable = TRUE;
        rd.MultisampleEnable = desc.samples > 1;
        if (FAILED(device->CreateRasterizerState(&rd, p.raster.put())))
            return {};

        D3D11_DEPTH_STENCIL_DESC dd{};
        const DepthStencilState& ds = desc.depthStencil;
        const bool hasDepth = desc.depthFormat != Format::Unknown;
        dd.DepthEnable = hasDepth && (ds.depthTest || ds.depthWrite);
        dd.DepthWriteMask = ds.depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = ds.depthTest ? ToD3D(ds.depthCompare) : D3D11_COMPARISON_ALWAYS;
        dd.StencilEnable = hasDepth && ds.stencilTest;
        dd.StencilReadMask = ds.stencilReadMask;
        dd.StencilWriteMask = ds.stencilWriteMask;
        dd.FrontFace = {ToD3D(ds.front.fail), ToD3D(ds.front.depthFail), ToD3D(ds.front.pass), ToD3D(ds.front.compare)};
        dd.BackFace = {ToD3D(ds.back.fail), ToD3D(ds.back.depthFail), ToD3D(ds.back.pass), ToD3D(ds.back.compare)};
        if (FAILED(device->CreateDepthStencilState(&dd, p.depthStencil.put())))
            return {};

        D3D11_BLEND_DESC bd{};
        bd.AlphaToCoverageEnable = desc.blend.alphaToCoverage;
        bd.IndependentBlendEnable = TRUE;
        for (uint32_t i = 0; i < kMaxColorTargets; i++) {
            const BlendTarget& t = desc.blend.targets[i];
            D3D11_RENDER_TARGET_BLEND_DESC& o = bd.RenderTarget[i];
            o.BlendEnable = t.enable;
            o.SrcBlend = t.enable ? ToD3D(t.srcColor, false) : D3D11_BLEND_ONE;
            o.DestBlend = t.enable ? ToD3D(t.dstColor, false) : D3D11_BLEND_ZERO;
            o.BlendOp = t.enable ? ToD3D(t.colorOp) : D3D11_BLEND_OP_ADD;
            o.SrcBlendAlpha = t.enable ? ToD3D(t.srcAlpha, true) : D3D11_BLEND_ONE;
            o.DestBlendAlpha = t.enable ? ToD3D(t.dstAlpha, true) : D3D11_BLEND_ZERO;
            o.BlendOpAlpha = t.enable ? ToD3D(t.alphaOp) : D3D11_BLEND_OP_ADD;
            o.RenderTargetWriteMask = static_cast<UINT8>(t.writeMask);
        }
        if (FAILED(device->CreateBlendState(&bd, p.blend.put())))
            return {};

        stats.livePipelines++;
        return pipelines.Alloc(std::move(p));
    }

    PipelineHandle Device::CreateComputePipeline(const ComputePipelineDesc& desc) {
        const Shader* cs = shaders.Get(desc.compute);
        const PipelineLayout* layout = pipelineLayouts.Get(desc.layout);
        if (!cs || !layout) {
            ReportError("CreateComputePipeline: missing shader or layout");
            return {};
        }
        Pipeline p;
        p.compute = true;
        std::string error;
        winrt::com_ptr<ID3DBlob> blob;
        if (!CompileStage(*cs, layout->desc, kStageCompute, p.stages[kStageCompute], blob, error)) {
            ReportError(std::string("shader compile failed (") + (desc.name ? desc.name : "?") + "): " + error);
            return {};
        }
        if (FAILED(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, p.cs.put())))
            return {};
        stats.livePipelines++;
        return pipelines.Alloc(std::move(p));
    }

    // ---- fences, frames -----------------------------------------------------------------------------------------------

    FenceHandle Device::CreateFence(uint64_t initialValue) {
        Fence f;
        f.value = initialValue;
        return fences.Alloc(std::move(f));
    }

    // caller holds the context lock
    void Device::PollFence(Fence& f) {
        while (!f.pending.empty()) {
            auto& [value, query] = f.pending.front();
            if (immediate->GetData(query.get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
                break;
            f.value = std::max(f.value, value);
            freeQueries.push_back(std::move(query));
            f.pending.pop_front();
        }
    }

    void Device::MarkSubmitted(FenceHandle handle, uint64_t value) {
        Fence* f = fences.Get(handle);
        if (!f)
            return;
        winrt::com_ptr<ID3D11Query> q;
        if (!freeQueries.empty()) {
            q = std::move(freeQueries.back());
            freeQueries.pop_back();
        } else {
            D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
            if (FAILED(device->CreateQuery(&qd, q.put()))) {
                std::lock_guard lock(fenceMutex);
                f->value = std::max(f->value, value);
                return;
            }
        }
        immediate->End(q.get());
        std::lock_guard lock(fenceMutex);
        f->pending.emplace_back(value, std::move(q));
    }

    uint64_t Device::FenceValue(FenceHandle fence) {
        Fence* f = fences.Get(fence);
        if (!f)
            return 0;
        auto lock = LockContext();
        std::lock_guard fenceLock(fenceMutex);
        PollFence(*f);
        return f->value;
    }

    void Device::SignalFence(FenceHandle fence, uint64_t value) {
        if (Fence* f = fences.Get(fence)) {
            std::lock_guard lock(fenceMutex);
            f->value = std::max(f->value, value);
        }
    }

    bool Device::WaitFence(FenceHandle fence, uint64_t value, uint64_t timeoutNs) {
        Fence* f = fences.Get(fence);
        if (!f)
            return true;
        const auto start = std::chrono::steady_clock::now();
        bool flushed = false;
        for (;;) {
            {
                auto lock = LockContext();
                std::lock_guard fenceLock(fenceMutex);
                PollFence(*f);
                if (f->value >= value)
                    return true;
                if (!flushed) {
                    immediate->Flush();
                    flushed = true;
                }
            }
            if (timeoutNs != ~0ull &&
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()) >= timeoutNs)
                return false;
            std::this_thread::yield();
        }
    }

    void Device::BeginFrame() {
        frame++;
        const uint64_t behind = std::max(1u, options.framesInFlight);
        if (frame > behind)
            WaitFence(frameFence, frame - behind, 2'000'000'000ull);
    }

    void Device::EndFrame() {
        auto lock = LockContext();
        MarkSubmitted(frameFence, frame);
    }

    void Device::WaitIdle() {
        if (!device)
            return;
        FenceHandle idle = CreateFence(0);
        {
            auto lock = LockContext();
            immediate->Flush();
            MarkSubmitted(idle, 1);
        }
        WaitFence(idle, 1, 10'000'000'000ull);
        Destroy(idle);
    }

    MemoryBudget Device::QueryMemoryBudget() {
        MemoryBudget m;
        if (auto a3 = adapter.try_as<IDXGIAdapter3>()) {
            DXGI_QUERY_VIDEO_MEMORY_INFO local{}, host{};
            if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
                SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &host))) {
                m.deviceBudget = local.Budget;
                m.deviceUsage = local.CurrentUsage;
                m.hostBudget = host.Budget;
                m.hostUsage = host.CurrentUsage;
                m.valid = true;
                return m;
            }
        }
        m.deviceUsage = stats.bufferBytes + stats.textureBytes;
        return m;
    }

    // ---- command lists, submission ------------------------------------------------------------------------------------

    CommandList* Device::AcquireCommandList(QueueType queue) {
        std::lock_guard lock(listMutex);
        CmdList* list;
        if (!freeLists.empty()) {
            list = freeLists.back();
            freeLists.pop_back();
        } else {
            auto made = std::make_unique<CmdList>(*this, queue);
            if (!made->Init()) {
                ReportError("could not create a deferred context");
                return nullptr;
            }
            lists.push_back(std::move(made));
            list = lists.back().get();
        }
        list->Begin();
        return list;
    }

    void Device::Submit(QueueType, const SubmitDesc& desc) {
        std::vector<winrt::com_ptr<ID3D11CommandList>> commands;
        for (rhi::CommandList* base : desc.lists) {
            auto* list = static_cast<CmdList*>(base);
            if (ID3D11CommandList* cl = list->Finish()) {
                winrt::com_ptr<ID3D11CommandList> owned;
                owned.copy_from(cl);
                commands.push_back(std::move(owned));
            }
        }
        {
            auto lock = LockContext();
            for (auto& cl : commands)
                immediate->ExecuteCommandList(cl.get(), FALSE);
            for (const FenceOp& op : desc.signals)
                MarkSubmitted(op.fence, op.value);
        }
        commands.clear();
        {
            std::lock_guard lock(listMutex);
            for (rhi::CommandList* base : desc.lists) {
                auto* list = static_cast<CmdList*>(base);
                list->ReleaseFinished();
                freeLists.push_back(list);
            }
        }
        stats.submits++;
    }

    std::unique_ptr<rhi::Swapchain> Device::CreateSwapchain(const SwapchainDesc& desc) { return MakeSwapchain(*this, desc); }
}
