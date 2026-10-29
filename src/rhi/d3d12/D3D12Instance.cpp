#include "rhi/Backends.hpp"
#include "rhi/d3d12/D3D12Convert.hpp"
#include "rhi/d3d12/D3D12Device.hpp"

#include <algorithm>
#include <cstdio>

namespace rhi::d3d12 {
    namespace {
        std::string Narrow(const wchar_t* w) {
            std::string out;
            for (; *w; w++)
                out.push_back(*w < 0x80 ? static_cast<char>(*w) : '?');
            return out;
        }

        class D3D12Instance final : public Instance {
        public:
            explicit D3D12Instance(const InstanceOptions& options) : options(options) {}

            bool Init() {
                state = std::make_shared<InstanceState>();
                Library& lib = state->lib;
                lib.d3d12 = LoadLibraryW(L"d3d12.dll");
                lib.dxgi = LoadLibraryW(L"dxgi.dll");
                if (!lib.d3d12 || !lib.dxgi)
                    return false;
                lib.createDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(lib.d3d12, "D3D12CreateDevice"));
                lib.getDebugInterface = reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(GetProcAddress(lib.d3d12, "D3D12GetDebugInterface"));
                lib.serializeRootSignature = reinterpret_cast<PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE>(
                    GetProcAddress(lib.d3d12, "D3D12SerializeVersionedRootSignature"));
                lib.createFactory2 = reinterpret_cast<decltype(lib.createFactory2)>(GetProcAddress(lib.dxgi, "CreateDXGIFactory2"));
                // an interposer (frame generation layer) supplies its own entry point
                if (options.hooks.dxgiCreateFactory)
                    lib.createFactory2 = reinterpret_cast<decltype(lib.createFactory2)>(options.hooks.dxgiCreateFactory);
                if (options.hooks.d3d12CreateDevice)
                    lib.createDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(options.hooks.d3d12CreateDevice);
                if (!lib.createDevice || !lib.serializeRootSignature || !lib.createFactory2)
                    return false;

                UINT factoryFlags = 0;
                if (options.validation && lib.getDebugInterface) {
                    winrt::com_ptr<ID3D12Debug> debug;
                    if (SUCCEEDED(lib.getDebugInterface(winrt::guid_of<ID3D12Debug>(), debug.put_void()))) {
                        debug->EnableDebugLayer();
                        state->validation = true;
                        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
                    }
                }
                if (FAILED(lib.createFactory2(factoryFlags, winrt::guid_of<IDXGIFactory4>(), state->factory.put_void())))
                    return false;

                winrt::com_ptr<IDXGIFactory5> f5;
                BOOL tearing = FALSE;
                if ((f5 = state->factory.try_as<IDXGIFactory5>()) && SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))))
                    state->tearing = tearing != FALSE;

                EnumerateAdapters();
                return true;
            }

            Api GetApi() const override { return Api::D3D12; }
            const std::vector<AdapterInfo>& Adapters() const override { return adapters; }

            std::unique_ptr<rhi::Device> CreateDevice(const DeviceOptions& deviceOptions) override {
                if (deviceOptions.adapterIndex >= adapters.size() || !adapters[deviceOptions.adapterIndex].usable)
                    return nullptr;
                auto device = std::make_unique<Device>(state, native[deviceOptions.adapterIndex], adapters[deviceOptions.adapterIndex], deviceOptions);
                std::string error;
                if (!device->Init(error)) {
                    std::fprintf(stderr, "[d3d12] device creation failed: %s\n", error.c_str());
                    return nullptr;
                }
                return device;
            }

        private:
            void Consider(winrt::com_ptr<IDXGIAdapter1> adapter) {
                DXGI_ADAPTER_DESC1 d{};
                adapter->GetDesc1(&d);
                // the basic render driver is listed last by the caller; other software adapters are noise
                AdapterInfo a;
                a.api = Api::D3D12;
                a.index = static_cast<uint32_t>(adapters.size());
                a.name = Narrow(d.Description);
                a.vendorId = d.VendorId;
                a.deviceId = d.DeviceId;
                a.dedicatedVram = d.DedicatedVideoMemory;
                a.sharedMemory = d.SharedSystemMemory;
                a.luid = (static_cast<uint64_t>(d.AdapterLuid.HighPart) << 32) | d.AdapterLuid.LowPart;
                const bool software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
                a.type = software ? AdapterType::Cpu : (d.DedicatedVideoMemory > (512ull << 20) ? AdapterType::Discrete : AdapterType::Integrated);

                LARGE_INTEGER umd{};
                if (SUCCEEDED(adapter->CheckInterfaceSupport(winrt::guid_of<IDXGIDevice>(), &umd)))
                    a.driverVersion = static_cast<uint64_t>(umd.QuadPart);

                winrt::com_ptr<ID3D12Device> probe;
                if (FAILED(state->lib.createDevice(adapter.get(), D3D_FEATURE_LEVEL_12_0, winrt::guid_of<ID3D12Device>(), probe.put_void()))) {
                    a.usable = false;
                    a.unusableReason = "no Direct3D 12 feature level 12_0";
                } else {
                    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0};
                    D3D12_FEATURE_DATA_FEATURE_LEVELS fl{};
                    fl.NumFeatureLevels = static_cast<UINT>(std::size(levels));
                    fl.pFeatureLevelsRequested = levels;
                    if (SUCCEEDED(probe->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl))))
                        a.apiVersion = fl.MaxSupportedFeatureLevel;
                    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_0};
                    if (FAILED(probe->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) || sm.HighestShaderModel < D3D_SHADER_MODEL_6_0) {
                        a.usable = false;
                        a.unusableReason = "needs shader model 6.0";
                    }
                }
                adapters.push_back(std::move(a));
                native.push_back(std::move(adapter));
            }

            void EnumerateAdapters() {
                winrt::com_ptr<IDXGIFactory6> f6;
                winrt::com_ptr<IDXGIAdapter1> warp;
                auto handle = [&](winrt::com_ptr<IDXGIAdapter1> a) {
                    DXGI_ADAPTER_DESC1 d{};
                    a->GetDesc1(&d);
                    if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                        if (!warp)
                            warp = a;
                        return;
                    }
                    Consider(a);
                };
                if ((f6 = state->factory.try_as<IDXGIFactory6>())) {
                    for (UINT i = 0;; i++) {
                        winrt::com_ptr<IDXGIAdapter1> a;
                        if (FAILED(f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, winrt::guid_of<IDXGIAdapter1>(), a.put_void())))
                            break;
                        handle(a);
                    }
                } else {
                    for (UINT i = 0;; i++) {
                        winrt::com_ptr<IDXGIAdapter1> a;
                        if (FAILED(state->factory->EnumAdapters1(i, a.put())))
                            break;
                        handle(a);
                    }
                }
                if (warp)
                    Consider(warp);
            }

            InstanceOptions options;
            std::shared_ptr<InstanceState> state;
            std::vector<AdapterInfo> adapters;
            std::vector<winrt::com_ptr<IDXGIAdapter1>> native;
        };
    }

    Device::Device(std::shared_ptr<InstanceState> instance, winrt::com_ptr<IDXGIAdapter1> adapterIn, const AdapterInfo& info, const DeviceOptions& opts)
        : adapter(std::move(adapterIn)), inst(std::move(instance)), options(opts) {
        caps.adapter = info;
        options.framesInFlight = std::clamp(options.framesInFlight, 1u, 4u);
        useValidator = options.stateValidation;
    }

    Device::~Device() {
        if (!device)
            return;
        WaitIdle();
        retire.FlushAll();
        // anything the owner forgot to destroy releases with the pools below
    }

    bool Device::Init(std::string& error) {
        const HRESULT hr = inst->lib.createDevice(adapter.get(), D3D_FEATURE_LEVEL_12_0, winrt::guid_of<ID3D12Device>(), device.put_void());
        if (FAILED(hr)) {
            error = "D3D12CreateDevice failed";
            return false;
        }
        device4 = device.try_as<ID3D12Device4>();
        waitEvent = winrt::handle{CreateEventW(nullptr, FALSE, FALSE, nullptr)};

        if (inst->validation && (infoQueue = device.try_as<ID3D12InfoQueue>())) {
            D3D12_MESSAGE_SEVERITY severities[] = {D3D12_MESSAGE_SEVERITY_INFO};
            D3D12_MESSAGE_ID noise[] = {D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
                                        D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
                                        D3D12_MESSAGE_ID_EXECUTECOMMANDLISTS_GPU_WRITTEN_READBACK_RESOURCE_MAPPED};
            D3D12_INFO_QUEUE_FILTER filter{};
            filter.DenyList.NumSeverities = 1;
            filter.DenyList.pSeverityList = severities;
            filter.DenyList.NumIDs = static_cast<UINT>(std::size(noise));
            filter.DenyList.pIDList = noise;
            infoQueue->PushStorageFilter(&filter);
        }

        const D3D12_COMMAND_LIST_TYPE types[kQueueCount] = {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE, D3D12_COMMAND_LIST_TYPE_COPY};
        for (size_t i = 0; i < kQueueCount; i++) {
            if (i == 1 && !options.allowAsyncCompute)
                continue;
            D3D12_COMMAND_QUEUE_DESC qd{};
            qd.Type = types[i];
            qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
            if (FAILED(device->CreateCommandQueue(&qd, winrt::guid_of<ID3D12CommandQueue>(), queues[i].queue.put_void()))) {
                error = "CreateCommandQueue failed";
                return false;
            }
            device->CreateFence(0, D3D12_FENCE_FLAG_NONE, winrt::guid_of<ID3D12Fence>(), queues[i].fence.put_void());
        }

        D3D12MA::ALLOCATOR_DESC ad{};
        ad.pDevice = device.get();
        ad.pAdapter = adapter.get();
        ad.Flags = D3D12MA::ALLOCATOR_FLAG_MSAA_TEXTURES_ALWAYS_COMMITTED;
        if (FAILED(D3D12MA::CreateAllocator(&ad, allocator.put()))) {
            error = "D3D12MA allocator creation failed";
            return false;
        }

        if (!rtvPool.Init(device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2048) || !dsvPool.Init(device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 512) ||
            !resourceHeap.Init(device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 400000) ||
            !samplerHeap.Init(device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048)) {
            error = "descriptor heap creation failed";
            return false;
        }

        winrt::com_ptr<ID3D12Device1> device1;
        if ((device1 = device.try_as<ID3D12Device1>())) {
            HRESULT lr = device1->CreatePipelineLibrary(options.pipelineCacheData.data(), options.pipelineCacheData.size(),
                                                        winrt::guid_of<ID3D12PipelineLibrary>(), pipelineLibrary.put_void());
            // a blob from another driver version is rejected: start with an empty library
            if (FAILED(lr) && !options.pipelineCacheData.empty())
                device1->CreatePipelineLibrary(nullptr, 0, winrt::guid_of<ID3D12PipelineLibrary>(), pipelineLibrary.put_void());
        }

        frameGroups.resize(options.framesInFlight);
        slotSerials.assign(options.framesInFlight, QueueSerials{});
        FillCaps();
        return true;
    }

    void Device::FillCaps() {
        DeviceFeatures& f = caps.features;
        DeviceLimits& l = caps.limits;
        D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
        device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o));
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof(o1)))) {
            f.subgroupOps = o1.WaveOps;
            l.minSubgroupSize = o1.WaveLaneCountMin;
            l.maxSubgroupSize = o1.WaveLaneCountMax;
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS4 o4{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &o4, sizeof(o4))))
            f.shaderFloat16 = o4.Native16BitShaderOpsSupported;
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5))))
            f.rayTracing = o5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;
        D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6))))
            f.variableRateShading = o6.VariableShadingRateTier != D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED;
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7))))
            f.meshShaders = o7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
        D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof(o12))))
            f.enhancedBarriers = o12.EnhancedBarriersSupported;
        D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &o16, sizeof(o16))))
            f.resizableBar = o16.GPUUploadHeapSupported;
        D3D12_FEATURE_DATA_ARCHITECTURE1 arch{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &arch, sizeof(arch))))
            uma = f.unifiedMemory = arch.UMA != FALSE;
        D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))))
            sm.HighestShaderModel = D3D_SHADER_MODEL_6_0;
        f.cooperativeMatrix = false;

        f.multiDrawIndirect = f.drawIndirectCount = f.drawIndirectFirstInstance = true;
        f.asyncCompute = options.allowAsyncCompute;
        f.dedicatedCopyQueue = true;
        f.timestampQueries = true;
        f.pipelineCacheBlob = static_cast<bool>(pipelineLibrary);
        f.textureCompressionBC = true;
        f.samplerAnisotropy = true;
        f.shaderInt64 = o1.Int64ShaderOps != FALSE;
        f.depthClipControl = true;
        f.bufferDeviceAddress = true;
        f.bindless = o.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3;
        f.tearing = inst->tearing;
        f.hdrOutput = true;
        f.conservativeRaster = o.ConservativeRasterizationTier != D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED;

        l.maxTexture2D = 16384;
        l.maxTextureLayers = 2048;
        l.maxSampledTexturesPerStage = 128;
        l.maxColorTargets = 8;
        l.uniformBufferAlignment = 256;
        l.storageBufferAlignment = 16;
        l.copyRowPitchAlignment = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
        l.copyOffsetAlignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
        l.maxAnisotropy = 16.0f;
        l.maxBufferSize = 1ull << 31;
        UINT64 freq = 0;
        if (queues[0].queue && SUCCEEDED(queues[0].queue->GetTimestampFrequency(&freq)) && freq)
            l.timestampPeriodNs = static_cast<float>(1.0e9 / static_cast<double>(freq));

        l.maxMsaaSamples = 1;
        for (UINT samples : {2u, 4u, 8u}) {
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS ms{DXGI_FORMAT_R8G8B8A8_UNORM, samples};
            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &ms, sizeof(ms))) && ms.NumQualityLevels > 0)
                l.maxMsaaSamples = samples;
        }

        for (size_t i = 1; i < static_cast<size_t>(Format::Count); i++) {
            const FormatMap fm = MapFormat(static_cast<Format>(i));
            D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{fm.dsv != DXGI_FORMAT_UNKNOWN ? fm.dsv : fm.view};
            if (fs.Format == DXGI_FORMAT_UNKNOWN || FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))))
                continue;
            FormatUse use = FormatUse::None;
            const UINT s1 = fs.Support1;
            // depth formats report sampling through their view format, the dsv format only says "depth target"
            UINT sampleBits = s1;
            if (fm.dsv != DXGI_FORMAT_UNKNOWN) {
                D3D12_FEATURE_DATA_FORMAT_SUPPORT vs{fm.view};
                sampleBits = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &vs, sizeof(vs))) ? vs.Support1 : 0;
            }
            if (sampleBits & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE)
                use = use | FormatUse::Sampled | FormatUse::SampledLinear;
            if (s1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET)
                use = use | FormatUse::ColorTarget | FormatUse::Msaa;
            if (s1 & D3D12_FORMAT_SUPPORT1_BLENDABLE)
                use = use | FormatUse::Blendable;
            if (s1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL)
                use = use | FormatUse::DepthTarget | FormatUse::Msaa;
            if (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)
                use = use | FormatUse::Storage;
            caps.formats[i] = use;
        }
    }

    NativeHandles Device::Native() const {
        NativeHandles n;
        n.api = Api::D3D12;
        n.device = device.get();
        n.graphicsQueue = queues[0].queue.get();
        n.computeQueue = queues[1].queue ? queues[1].queue.get() : queues[0].queue.get();
        n.adapter = adapter.get();
        n.factory = inst->factory.get();
        return n;
    }

    NativeTexture Device::NativeOf(TextureHandle h, State state) {
        TextureRec* t = textures.Get(h);
        if (!t || !t->resource)
            return {};
        NativeTexture n;
        n.resource = t->resource.get();
        n.state = static_cast<uint32_t>(ToStates(state, QueueType::Graphics));
        n.nativeFormat = static_cast<uint32_t>(t->resourceFormat);
        n.width = t->desc.width;
        n.height = t->desc.height;
        n.mips = t->mips;
        n.layers = t->layers;
        return n;
    }

    void Device::ReportError(const std::string& message) {
        {
            std::lock_guard lock(inst->messageMutex);
            inst->errorCount++;
            if (inst->messages.size() < 256)
                inst->messages.push_back(message);
        }
        std::fprintf(stderr, "[rhi/d3d12] %s\n", message.c_str());
    }

    void Device::CheckRemoved(HRESULT hr, const char* what) {
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG) {
            ReportError(std::string(what) + ": device removed (reason " + std::to_string(device->GetDeviceRemovedReason()) + ")");
        } else if (FAILED(hr)) {
            ReportError(std::string(what) + " failed (" + std::to_string(static_cast<long>(hr)) + ")");
        }
    }

    MemoryBudget Device::QueryMemoryBudget() {
        MemoryBudget out;
        winrt::com_ptr<IDXGIAdapter3> a3;
        if (!(a3 = adapter.try_as<IDXGIAdapter3>()))
            return out;
        DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonLocal{};
        if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
            SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal))) {
            out.deviceBudget = local.Budget;
            out.deviceUsage = local.CurrentUsage;
            out.hostBudget = nonLocal.Budget;
            out.hostUsage = nonLocal.CurrentUsage;
            out.valid = true;
        }
        return out;
    }

    std::string Device::DebugReport() {
        std::string out;
        {
            std::lock_guard lock(inst->messageMutex);
            for (auto& m : inst->messages)
                out += m + "\n";
            inst->messages.clear();
            inst->errorCount = 0;
        }
        if (infoQueue) {
            const UINT64 count = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
            for (UINT64 i = 0; i < count; i++) {
                SIZE_T len = 0;
                infoQueue->GetMessage(i, nullptr, &len);
                std::vector<uint8_t> raw(len);
                auto* msg = reinterpret_cast<D3D12_MESSAGE*>(raw.data());
                if (SUCCEEDED(infoQueue->GetMessage(i, msg, &len)) && msg->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
                    out += std::string("d3d12: ") + msg->pDescription + "\n";
            }
            infoQueue->ClearStoredMessages();
        }
        if (useValidator)
            for (auto& m : validator.TakeErrors())
                out += "state: " + m + "\n";
        return out;
    }

    // the Windows SDK (and DirectX-Headers) return the handle; mingw-w64 uses the older out-parameter form
    static D3D12_CPU_DESCRIPTOR_HANDLE HeapCpuStart(ID3D12DescriptorHeap* h) {
#ifdef __MINGW32__
        D3D12_CPU_DESCRIPTOR_HANDLE r{};
        h->GetCPUDescriptorHandleForHeapStart(&r);
        return r;
#else
        return h->GetCPUDescriptorHandleForHeapStart();
#endif
    }

    static D3D12_GPU_DESCRIPTOR_HANDLE HeapGpuStart(ID3D12DescriptorHeap* h) {
#ifdef __MINGW32__
        D3D12_GPU_DESCRIPTOR_HANDLE r{};
        h->GetGPUDescriptorHandleForHeapStart(&r);
        return r;
#else
        return h->GetGPUDescriptorHandleForHeapStart();
#endif
    }

    bool CpuDescriptorPool::Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t cap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = type;
        hd.NumDescriptors = cap;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device->CreateDescriptorHeap(&hd, winrt::guid_of<ID3D12DescriptorHeap>(), heap.put_void())))
            return false;
        cpuStart = HeapCpuStart(heap.get());
        increment = device->GetDescriptorHandleIncrementSize(type);
        capacity = cap;
        return true;
    }

    uint32_t CpuDescriptorPool::Alloc() {
        std::lock_guard lock(mutex);
        if (!freeList.empty()) {
            const uint32_t i = freeList.back();
            freeList.pop_back();
            return i;
        }
        return next < capacity ? next++ : UINT32_MAX;
    }

    void CpuDescriptorPool::Free(uint32_t index) {
        std::lock_guard lock(mutex);
        freeList.push_back(index);
    }

    bool RangeHeap::Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = type;
        hd.NumDescriptors = capacity;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&hd, winrt::guid_of<ID3D12DescriptorHeap>(), heap.put_void())))
            return false;
        cpuStart = HeapCpuStart(heap.get());
        gpuStart = HeapGpuStart(heap.get());
        increment = device->GetDescriptorHandleIncrementSize(type);
        freeRanges.push_back({0, capacity});
        return true;
    }

    bool RangeHeap::Alloc(uint32_t count, uint32_t& offset) {
        std::lock_guard lock(mutex);
        for (auto it = freeRanges.begin(); it != freeRanges.end(); ++it) {
            if (it->second < count)
                continue;
            offset = it->first;
            it->first += count;
            it->second -= count;
            if (it->second == 0)
                freeRanges.erase(it);
            used += count;
            return true;
        }
        return false;
    }

    void RangeHeap::Free(uint32_t offset, uint32_t count) {
        if (!count)
            return;
        std::lock_guard lock(mutex);
        used -= count;
        auto it = std::lower_bound(freeRanges.begin(), freeRanges.end(), std::make_pair(offset, 0u));
        it = freeRanges.insert(it, {offset, count});
        // merge with the following range, then the preceding one
        auto next = it + 1;
        if (next != freeRanges.end() && it->first + it->second == next->first) {
            it->second += next->second;
            freeRanges.erase(next);
        }
        if (it != freeRanges.begin()) {
            auto prev = it - 1;
            if (prev->first + prev->second == it->first) {
                prev->second += it->second;
                freeRanges.erase(it);
            }
        }
    }
}

namespace rhi::backends {
    std::unique_ptr<Instance> CreateD3D12Instance(const InstanceOptions& options) {
        auto instance = std::make_unique<d3d12::D3D12Instance>(options);
        if (!instance->Init())
            return nullptr;
        return instance;
    }
}
