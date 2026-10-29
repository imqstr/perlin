// dlss super resolution, dlss frame generation and reflex through nvidia streamline. The interposer is loaded at run
// time and supplies the api entry points, so every device, queue and swapchain the rhi makes is already a proxy
#include "rhi/vendor/Vendor.hpp"

#if defined(RHI_HAS_STREAMLINE) && RHI_HAS_STREAMLINE

#include <windows.h>

#include <sl.h>
#include <sl_appidentity.h>
#include <sl_consts.h>
#include <sl_device_wrappers.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#if defined(_MSC_VER)
#include <sl_security.h>
#define RHI_CAN_VERIFY_SIGNATURES 1
#else
// the vendor's verification helper needs the full windows sdk; other toolchains cannot check signatures
#define RHI_CAN_VERIFY_SIGNATURES 0
#endif

#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>

namespace rhi::vendor {
    namespace {
        // one logical viewport: the game renders a single view
        constexpr uint32_t kViewport = 0;

        struct SlApi {
            HMODULE module = nullptr;
            PFun_slInit* init = nullptr;
            PFun_slShutdown* shutdown = nullptr;
            PFun_slIsFeatureSupported* isFeatureSupported = nullptr;
            PFun_slSetD3DDevice* setD3DDevice = nullptr;
            PFun_slGetNewFrameToken* getNewFrameToken = nullptr;
            PFun_slSetConstants* setConstants = nullptr;
            PFun_slSetTagForFrame* setTagForFrame = nullptr;
            PFun_slEvaluateFeature* evaluateFeature = nullptr;
            PFun_slFreeResources* freeResources = nullptr;
            PFun_slGetFeatureFunction* getFeatureFunction = nullptr;

            template <class F>
            bool Bind(F*& slot, const char* name) {
                slot = reinterpret_cast<F*>(GetProcAddress(module, name));
                return slot != nullptr;
            }

            bool Load(const std::wstring& dir, bool verify, std::string& log) {
                std::wstring path = dir.empty() ? L"" : dir + (dir.back() == L'\\' || dir.back() == L'/' ? L"" : L"\\");
                path += L"sl.interposer.dll";
                // the signature check needs a full path
                wchar_t full[MAX_PATH * 2];
                const DWORD n = GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr);
                if (n == 0 || n >= std::size(full) || GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) {
                    log += "streamline: sl.interposer.dll not found; ";
                    return false;
                }
#if RHI_CAN_VERIFY_SIGNATURES
                if (verify && !sl::security::verifyEmbeddedSignature(full)) {
                    log += "streamline: sl.interposer.dll is not signed; ";
                    return false;
                }
#else
                if (verify) {
                    log += "streamline: this build cannot verify module signatures; ";
                    return false;
                }
#endif
                module = LoadLibraryW(full);
                if (!module) {
                    log += "streamline: the interposer failed to load; ";
                    return false;
                }
                const bool ok = Bind(init, "slInit") && Bind(shutdown, "slShutdown") && Bind(isFeatureSupported, "slIsFeatureSupported") &&
                                Bind(setD3DDevice, "slSetD3DDevice") && Bind(getNewFrameToken, "slGetNewFrameToken") &&
                                Bind(setConstants, "slSetConstants") && Bind(setTagForFrame, "slSetTagForFrame") &&
                                Bind(evaluateFeature, "slEvaluateFeature") && Bind(freeResources, "slFreeResources") &&
                                Bind(getFeatureFunction, "slGetFeatureFunction");
                if (!ok) {
                    log += "streamline: the interposer lacks entry points; ";
                    FreeLibrary(module);
                    module = nullptr;
                }
                return ok;
            }
        };

        void SlLog(sl::LogType type, const char* message) {
            if (type == sl::LogType::eInfo)
                return;
            std::fprintf(stderr, "[rhi/streamline] %s", message);
        }

        void ToSl(const Mat4& in, sl::float4x4& out) {
            for (uint32_t r = 0; r < 4; r++)
                out.setRow(r, sl::float4(in.m[r * 4 + 0], in.m[r * 4 + 1], in.m[r * 4 + 2], in.m[r * 4 + 3]));
        }

        sl::Boolean B(bool v) { return v ? sl::Boolean::eTrue : sl::Boolean::eFalse; }

        class Streamline;

        class DlssUpscaler final : public Upscaler {
        public:
            DlssUpscaler(Streamline& owner, const UpscaleDesc& desc) : owner(owner), desc(desc) {}
            ~DlssUpscaler() override;

            const char* Name() const override { return "DLSS"; }
            UpscaleDesc Desc() const override { return desc; }
            bool Resize(const UpscaleDesc& d) override {
                desc = d;
                optionsDirty = true;
                return true;
            }
            bool Evaluate(CommandList& list, const UpscaleInputs& in) override;

        private:
            Streamline& owner;
            UpscaleDesc desc;
            bool optionsDirty = true;
        };

        class Streamline final : public Session, public FrameGenerator, public LowLatency {
        public:
            ~Streamline() override { Shutdown(); }

            bool Init(Api api_, const SessionRequest& request_, VendorHooks& hooks, std::string& log) {
                api = api_;
                request = request_;
                if (api != Api::D3D12 && api != Api::Vulkan)
                    return false;
                if (!sl.Load(request.modulesPath, request.verifySignatures, log))
                    return false;

                std::vector<sl::Feature> features;
                if (request.upscaler)
                    features.push_back(sl::kFeatureDLSS);
                if (request.frameGeneration) {
                    features.push_back(sl::kFeatureDLSS_G);
                    // frame generation needs reflex and the latency markers to pace itself
                    request.lowLatency = true;
                }
                if (request.lowLatency) {
                    features.push_back(sl::kFeatureReflex);
                    features.push_back(sl::kFeaturePCL);
                }

                sl::Preferences pref{};
                pref.showConsole = false;
                pref.logLevel = request.verboseLog ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
                pref.logMessageCallback = SlLog;
                pref.pathToLogsAndData = request.logPath.empty() ? nullptr : request.logPath.c_str();
                pathToPlugins = request.modulesPath;
                if (!pathToPlugins.empty()) {
                    pluginPaths[0] = pathToPlugins.c_str();
                    pref.pathsToPlugins = pluginPaths;
                    pref.numPathsToPlugins = 1;
                }
                pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
                pref.featuresToLoad = features.data();
                pref.numFeaturesToLoad = static_cast<uint32_t>(features.size());
                pref.applicationId = request.applicationId;
                pref.engine = sl::EngineType::eCustom;
                pref.engineVersion = "1.0";
                pref.projectId = "5b6c3f0e-2d2e-4c55-9a79-7d6c2f6a1c11";
                pref.renderAPI = api == Api::Vulkan ? sl::RenderAPI::eVulkan : sl::RenderAPI::eD3D12;
                const sl::Result r = sl.init(pref, sl::kSDKVersion);
                if (r != sl::Result::eOk) {
                    log += "streamline: slInit failed (" + std::to_string(static_cast<int>(r)) + "); ";
                    return false;
                }
                initialised = true;

                // the interposer's entry points make every object created through them a streamline proxy
                if (api == Api::D3D12) {
                    hooks.d3d12CreateDevice = reinterpret_cast<void*>(GetProcAddress(sl.module, "D3D12CreateDevice"));
                    hooks.dxgiCreateFactory = reinterpret_cast<void*>(GetProcAddress(sl.module, "CreateDXGIFactory2"));
                    if (!hooks.d3d12CreateDevice || !hooks.dxgiCreateFactory) {
                        log += "streamline: the interposer does not export the d3d entry points; ";
                        return false;
                    }
                } else {
                    hooks.vkGetInstanceProcAddr = reinterpret_cast<void*>(GetProcAddress(sl.module, "vkGetInstanceProcAddr"));
                    if (!hooks.vkGetInstanceProcAddr) {
                        log += "streamline: the interposer does not export vkGetInstanceProcAddr; ";
                        return false;
                    }
                }
                return true;
            }

            bool Attach(Device& device) override {
                if (!initialised)
                    return false;
                this->device = &device;
                const NativeHandles native = device.Native();
                if (api == Api::D3D12 && sl.setD3DDevice(native.device) != sl::Result::eOk) {
                    caps.log += "streamline: slSetD3DDevice failed; ";
                    return false;
                }

                sl::AdapterInfo adapter{};
                uint64_t luid = device.Caps().adapter.luid;
                adapter.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
                adapter.deviceLUIDSizeInBytes = sizeof(luid);
                adapter.vkPhysicalDevice = native.physicalDevice;
                auto supported = [&](sl::Feature f) { return sl.isFeatureSupported(f, adapter) == sl::Result::eOk; };
                caps.dlss = request.upscaler && supported(sl::kFeatureDLSS);
                caps.dlssFrameGeneration = request.frameGeneration && supported(sl::kFeatureDLSS_G);
                caps.reflex = request.lowLatency && supported(sl::kFeatureReflex);

                if (caps.dlss)
                    Resolve(sl::kFeatureDLSS, "slDLSSSetOptions", setDlssOptions);
                if (caps.dlssFrameGeneration) {
                    Resolve(sl::kFeatureDLSS_G, "slDLSSGSetOptions", setDlssgOptions);
                    Resolve(sl::kFeatureDLSS_G, "slDLSSGGetState", getDlssgState);
                    caps.dlssFrameGeneration = setDlssgOptions && getDlssgState;
                }
                if (caps.reflex) {
                    Resolve(sl::kFeatureReflex, "slReflexSetOptions", reflexSetOptions);
                    Resolve(sl::kFeatureReflex, "slReflexSleep", reflexSleep);
                    Resolve(sl::kFeaturePCL, "slPCLSetMarker", pclSetMarker);
                    caps.reflex = reflexSetOptions && reflexSleep && pclSetMarker;
                }
                attached = true;
                return caps.dlss || caps.dlssFrameGeneration || caps.reflex;
            }

            Capabilities Caps() const override { return caps; }

            void BeginFrame(uint64_t frame) override {
                if (!attached)
                    return;
                const uint32_t index = static_cast<uint32_t>(frame);
                sl::FrameToken* token = nullptr;
                if (sl.getNewFrameToken(token, &index) == sl::Result::eOk)
                    current = token;
            }

            std::unique_ptr<Upscaler> CreateUpscaler(const UpscaleDesc& desc) override {
                if (!caps.dlss)
                    return nullptr;
                return std::make_unique<DlssUpscaler>(*this, desc);
            }
            FrameGenerator* FrameGen() override { return caps.dlssFrameGeneration ? this : nullptr; }
            LowLatency* Latency() override { return caps.reflex ? this : nullptr; }

            void Shutdown() override {
                if (!initialised)
                    return;
                initialised = false;
                attached = false;
                if (caps.dlss)
                    sl.freeResources(sl::kFeatureDLSS, sl::ViewportHandle(kViewport));
                if (caps.dlssFrameGeneration)
                    sl.freeResources(sl::kFeatureDLSS_G, sl::ViewportHandle(kViewport));
                sl.shutdown();
                caps = {};
                // the module stays loaded: proxies the process still holds point into it
            }

            // ---- frame generation
            bool SetMultiplier(uint32_t multiplier, uint32_t backBuffers, uint32_t width, uint32_t height) override {
                if (!caps.dlssFrameGeneration)
                    return false;
                sl::DLSSGOptions o{};
                o.mode = multiplier > 1 ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
                o.numFramesToGenerate = multiplier > 1 ? multiplier - 1 : 1;
                o.numBackBuffers = backBuffers;
                o.colorWidth = width;
                o.colorHeight = height;
                return setDlssgOptions(sl::ViewportHandle(kViewport), o) == sl::Result::eOk;
            }

            bool Tag(CommandList& list, const HudlessInputs& in) override {
                if (!caps.dlssFrameGeneration || !current || !device)
                    return false;
                sl::Constants c = ConstantsFor(in.camera, in.jitterX, in.jitterY, in.motionScaleX, in.motionScaleY, in.reset);
                const sl::ViewportHandle vp(kViewport);
                if (sl.setConstants(c, *current, vp) != sl::Result::eOk)
                    return false;
                sl::Resource depth, motion, hudless;
                if (!MakeResource(in.depth, in.depthState, depth) || !MakeResource(in.motion, in.motionState, motion) ||
                    !MakeResource(in.hudless, in.hudlessState, hudless))
                    return false;
                // frame generation reads these at present time, after the lists that wrote them were submitted
                sl::ResourceTag tags[] = {
                    {&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent},
                    {&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent},
                    {&hudless, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent},
                };
                const bool ok = sl.setTagForFrame(*current, vp, tags, 3, list.NativeHandle()) == sl::Result::eOk;
                list.InvalidateBoundState();
                return ok;
            }

            FrameGenState State() override {
                FrameGenState s;
                if (!caps.dlssFrameGeneration)
                    return s;
                sl::DLSSGState st{};
                if (getDlssgState(sl::ViewportHandle(kViewport), st, nullptr) != sl::Result::eOk)
                    return s;
                s.active = st.status == sl::DLSSGStatus::eOk;
                s.maxMultiplier = st.numFramesToGenerateMax + 1;
                s.presentedPerRenderedFrame = (std::max)(1u, st.numFramesActuallyPresented);
                s.status = s.active ? "ok" : "status " + std::to_string(static_cast<uint32_t>(st.status));
                return s;
            }

            // ---- latency
            bool SetMode(bool enabled, bool boost, uint32_t frameLimitUs) override {
                if (!caps.reflex)
                    return false;
                sl::ReflexOptions o{};
                o.mode = enabled ? (boost ? sl::ReflexMode::eLowLatencyWithBoost : sl::ReflexMode::eLowLatency) : sl::ReflexMode::eOff;
                o.frameLimitUs = frameLimitUs;
                return reflexSetOptions(o) == sl::Result::eOk;
            }
            void Sleep() override {
                if (caps.reflex && current)
                    reflexSleep(*current);
            }
            void Mark(LatencyMarker marker) override {
                if (!caps.reflex || !current)
                    return;
                sl::PCLMarker m = sl::PCLMarker::eSimulationStart;
                switch (marker) {
                case LatencyMarker::SimulationStart: m = sl::PCLMarker::eSimulationStart; break;
                case LatencyMarker::SimulationEnd: m = sl::PCLMarker::eSimulationEnd; break;
                case LatencyMarker::RenderSubmitStart: m = sl::PCLMarker::eRenderSubmitStart; break;
                case LatencyMarker::RenderSubmitEnd: m = sl::PCLMarker::eRenderSubmitEnd; break;
                case LatencyMarker::PresentStart: m = sl::PCLMarker::ePresentStart; break;
                case LatencyMarker::PresentEnd: m = sl::PCLMarker::ePresentEnd; break;
                }
                pclSetMarker(m, *current);
            }

            // used by the upscaler
            SlApi sl;
            Device* device = nullptr;
            sl::FrameToken* current = nullptr;
            PFun_slDLSSSetOptions* setDlssOptions = nullptr;

            bool MakeResource(TextureHandle h, rhi::State state, sl::Resource& out) {
                const NativeTexture n = device->NativeOf(h, state);
                if (!n.resource)
                    return false;
                out = sl::Resource(sl::ResourceType::eTex2d, n.resource, n.memory, n.view, n.state);
                out.width = n.width;
                out.height = n.height;
                out.nativeFormat = n.nativeFormat;
                out.mipLevels = n.mips;
                out.arrayLayers = n.layers;
                out.usage = n.usage;
                out.flags = n.flags;
                return true;
            }

            sl::Constants ConstantsFor(const CameraData& cam, float jitterX, float jitterY, float mvScaleX, float mvScaleY, bool reset) {
                sl::Constants c{};
                ToSl(cam.viewToClip, c.cameraViewToClip);
                ToSl(cam.clipToView, c.clipToCameraView);
                ToSl(cam.clipToPrevClip, c.clipToPrevClip);
                ToSl(cam.prevClipToClip, c.prevClipToClip);
                // no lens distortion
                ToSl(Mat4{}, c.clipToLensClip);
                c.jitterOffset = {jitterX, jitterY};
                c.mvecScale = {mvScaleX, mvScaleY};
                c.cameraPinholeOffset = {0.0f, 0.0f};
                c.cameraPos = {cam.position[0], cam.position[1], cam.position[2]};
                c.cameraUp = {cam.up[0], cam.up[1], cam.up[2]};
                c.cameraRight = {cam.right[0], cam.right[1], cam.right[2]};
                c.cameraFwd = {cam.forward[0], cam.forward[1], cam.forward[2]};
                c.cameraNear = cam.nearPlane;
                c.cameraFar = cam.farPlane;
                c.cameraFOV = cam.fovY;
                c.cameraAspectRatio = cam.aspect;
                c.motionVectorsInvalidValue = std::numeric_limits<float>::infinity();
                c.depthInverted = B(cam.reverseZ);
                c.cameraMotionIncluded = sl::Boolean::eTrue;
                c.motionVectors3D = sl::Boolean::eFalse;
                c.reset = B(reset);
                c.orthographicProjection = sl::Boolean::eFalse;
                c.motionVectorsDilated = sl::Boolean::eFalse;
                c.motionVectorsJittered = sl::Boolean::eFalse;
                return c;
            }

            Capabilities caps;
            bool dlssOptionsSet = false;

        private:
            template <class F>
            void Resolve(sl::Feature feature, const char* name, F*& out) {
                void* fn = nullptr;
                if (sl.getFeatureFunction(feature, name, fn) == sl::Result::eOk)
                    out = reinterpret_cast<F*>(fn);
            }

            Api api = Api::None;
            SessionRequest request;
            std::wstring pathToPlugins;
            const wchar_t* pluginPaths[1] = {};
            bool initialised = false;
            bool attached = false;
            PFun_slDLSSGSetOptions* setDlssgOptions = nullptr;
            PFun_slDLSSGGetState* getDlssgState = nullptr;
            PFun_slReflexSetOptions* reflexSetOptions = nullptr;
            PFun_slReflexSleep* reflexSleep = nullptr;
            PFun_slPCLSetMarker* pclSetMarker = nullptr;
        };

        DlssUpscaler::~DlssUpscaler() {
            // the plugin keeps per viewport buffers; they are released with the session
        }

        bool DlssUpscaler::Evaluate(CommandList& list, const UpscaleInputs& in) {
            if (!owner.current || !owner.device || !owner.setDlssOptions)
                return false;
            const sl::ViewportHandle vp(kViewport);

            if (optionsDirty) {
                sl::DLSSOptions o{};
                switch (desc.quality) {
                case Quality::Native: o.mode = sl::DLSSMode::eDLAA; break;
                case Quality::UltraQuality: o.mode = sl::DLSSMode::eUltraQuality; break;
                case Quality::Quality: o.mode = sl::DLSSMode::eMaxQuality; break;
                case Quality::Balanced: o.mode = sl::DLSSMode::eBalanced; break;
                case Quality::Performance: o.mode = sl::DLSSMode::eMaxPerformance; break;
                case Quality::UltraPerformance: o.mode = sl::DLSSMode::eUltraPerformance; break;
                }
                o.outputWidth = desc.outputWidth;
                o.outputHeight = desc.outputHeight;
                o.colorBuffersHDR = B(desc.hdr);
                o.useAutoExposure = B(desc.autoExposure);
                o.preExposure = in.preExposure;
                if (owner.setDlssOptions(vp, o) != sl::Result::eOk)
                    return false;
                optionsDirty = false;
            }

            sl::Constants c = owner.ConstantsFor(in.camera, in.jitterX, in.jitterY, in.motionScaleX, in.motionScaleY, in.reset);
            if (owner.sl.setConstants(c, *owner.current, vp) != sl::Result::eOk)
                return false;

            sl::Resource color, depth, motion, output, exposure;
            if (!owner.MakeResource(in.color, in.colorState, color) || !owner.MakeResource(in.depth, in.depthState, depth) ||
                !owner.MakeResource(in.motion, in.motionState, motion) || !owner.MakeResource(in.output, in.outputState, output))
                return false;
            const sl::Extent renderExtent{0, 0, desc.renderWidth, desc.renderHeight};
            const sl::Extent outputExtent{0, 0, desc.outputWidth, desc.outputHeight};

            sl::ResourceTag tags[5] = {
                {&color, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &renderExtent},
                {&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &renderExtent},
                {&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &renderExtent},
                {&output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &outputExtent},
                {&exposure, sl::kBufferTypeExposure, sl::ResourceLifecycle::eValidUntilEvaluate},
            };
            uint32_t tagCount = 4;
            if (in.exposure && owner.MakeResource(in.exposure, in.exposureState, exposure))
                tagCount = 5;

            void* native = list.NativeHandle();
            bool ok = owner.sl.setTagForFrame(*owner.current, vp, tags, tagCount, native) == sl::Result::eOk;
            if (ok) {
                const sl::BaseStructure* inputs[] = {&vp};
                ok = owner.sl.evaluateFeature(sl::kFeatureDLSS, *owner.current, inputs, 1, native) == sl::Result::eOk;
            }
            // streamline records its own state changes into the list
            list.InvalidateBoundState();
            return ok;
        }
    }

    std::unique_ptr<Session> MakeStreamlineSession(Api api, const SessionRequest& request, VendorHooks& hooks, std::string* log) {
        auto session = std::make_unique<Streamline>();
        std::string text;
        if (!session->Init(api, request, hooks, text)) {
            if (log)
                *log += text;
            return nullptr;
        }
        if (log)
            *log += text;
        return session;
    }
}

#endif
