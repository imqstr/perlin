#pragma once
#include "rhi/Device.hpp"

#include <array>
#include <memory>
#include <string>

// Vendor sdks (dlss, reflex, frame generation, xess) behind one interface. Nothing here links a vendor library: the
// sdk modules are loaded at run time, and every call degrades to "unavailable" when they are missing, unsigned, or the
// hardware refuses them. The renderer asks what it got and falls back to its own temporal upscaler.
namespace rhi::vendor {
    // internal render resolution as a fraction of the output
    enum class Quality : uint8_t { Native, UltraQuality, Quality, Balanced, Performance, UltraPerformance };
    float ScaleOf(Quality quality);
    const char* QualityName(Quality quality);
    // render size for an output size, rounded up to even
    void RenderSizeFor(Quality quality, uint32_t outputWidth, uint32_t outputHeight, uint32_t& width, uint32_t& height);

    // sub-pixel offset in [-0.5, 0.5] pixels of the render target for a frame: a halton(2,3) sequence long enough to
    // cover each output pixel eight times
    std::array<float, 2> JitterFor(uint64_t frame, uint32_t renderWidth, uint32_t outputWidth);
    uint32_t JitterPhases(uint32_t renderWidth, uint32_t outputWidth);

    // row-major 4x4, applied as M * v
    struct Mat4 {
        float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    };

    struct CameraData {
        Mat4 viewToClip;
        Mat4 clipToView;
        Mat4 clipToPrevClip;
        Mat4 prevClipToClip;
        float position[3] = {};
        float up[3] = {0, 1, 0};
        float right[3] = {1, 0, 0};
        float forward[3] = {0, 0, 1};
        float nearPlane = 0.05f;
        float farPlane = 1000.0f;
        float fovY = 1.2f;
        float aspect = 16.0f / 9.0f;
        bool reverseZ = true;
    };

    struct UpscaleDesc {
        uint32_t renderWidth = 0, renderHeight = 0;
        uint32_t outputWidth = 0, outputHeight = 0;
        Quality quality = Quality::Quality;
        // color buffers are linear HDR (before tonemapping)
        bool hdr = true;
        bool autoExposure = true;
    };

    // rhi states the textures are in when Evaluate is recorded
    struct UpscaleInputs {
        TextureHandle color;
        State colorState = State::ShaderReadOther;
        TextureHandle depth;
        State depthState = State::ShaderReadOther;
        TextureHandle motion;
        State motionState = State::ShaderReadOther;
        // optional 1x1 pre-exposure texture
        TextureHandle exposure;
        State exposureState = State::ShaderReadOther;
        TextureHandle output;
        State outputState = State::ShaderWrite;

        // the jitter that was applied to the projection this frame, in render target pixels
        float jitterX = 0, jitterY = 0;
        // motion vector scale taking the stored values to pixels (1/width, 1/height when stored as uv deltas)
        float motionScaleX = 1, motionScaleY = 1;
        float preExposure = 1.0f;
        // history is invalid: camera cut, teleport, renderer switch
        bool reset = false;
        CameraData camera;
    };

    class Upscaler {
    public:
        virtual ~Upscaler() = default;
        virtual const char* Name() const = 0;
        // records into `list`; the list's bound state is invalid afterwards
        virtual bool Evaluate(CommandList& list, const UpscaleInputs& inputs) = 0;
        virtual UpscaleDesc Desc() const = 0;
        virtual bool Resize(const UpscaleDesc& desc) = 0;
    };

    struct HudlessInputs {
        TextureHandle depth;
        State depthState = State::ShaderReadOther;
        TextureHandle motion;
        State motionState = State::ShaderReadOther;
        // scene color before the ui was drawn
        TextureHandle hudless;
        State hudlessState = State::ShaderReadOther;
        CameraData camera;
        float jitterX = 0, jitterY = 0;
        float motionScaleX = 1, motionScaleY = 1;
        bool reset = false;
    };

    struct FrameGenState {
        bool active = false;
        uint32_t maxMultiplier = 1;
        // frames shown for the last real one
        uint32_t presentedPerRenderedFrame = 1;
        std::string status;
    };

    // generated frames between rendered ones. The generator hooks Present of the swapchain it is attached to
    class FrameGenerator {
    public:
        virtual ~FrameGenerator() = default;
        // multiplier 1 turns it off
        virtual bool SetMultiplier(uint32_t multiplier, uint32_t backBuffers, uint32_t width, uint32_t height) = 0;
        virtual bool Tag(CommandList& list, const HudlessInputs& inputs) = 0;
        virtual FrameGenState State() = 0;
    };

    enum class LatencyMarker : uint8_t { SimulationStart, SimulationEnd, RenderSubmitStart, RenderSubmitEnd, PresentStart, PresentEnd };

    class LowLatency {
    public:
        virtual ~LowLatency() = default;
        // boost keeps clocks up while the gpu would otherwise idle
        virtual bool SetMode(bool enabled, bool boost, uint32_t frameLimitUs) = 0;
        // blocks until the pipeline wants the next frame's input; call once at the top of the frame
        virtual void Sleep() = 0;
        virtual void Mark(LatencyMarker marker) = 0;
    };

    struct SessionRequest {
        bool upscaler = true;
        bool frameGeneration = false;
        bool lowLatency = true;
        // directory holding sl.interposer.dll and the plugin dlls; empty = next to the executable
        std::wstring modulesPath;
        // directory for logs; empty disables the sdk's own log file
        std::wstring logPath;
        // refuse modules that are not signed by the vendor
        bool verifySignatures = true;
        bool verboseLog = false;
        // id registered with the vendor for this application, 0 = unregistered engine
        uint32_t applicationId = 0;
    };

    struct Capabilities {
        bool dlss = false;
        bool dlssFrameGeneration = false;
        bool reflex = false;
        std::string log;
    };

    // one per renderer: Begin before the instance exists (it supplies the hooks that route api creation through the
    // vendor layer), Attach once the device exists
    class Session {
    public:
        virtual ~Session() = default;
        static std::unique_ptr<Session> Begin(Api api, const SessionRequest& request, VendorHooks& hooks, std::string* log);

        virtual bool Attach(Device& device) = 0;
        virtual Capabilities Caps() const = 0;

        // one call per frame, before any vendor work for that frame
        virtual void BeginFrame(uint64_t frame) = 0;
        virtual std::unique_ptr<Upscaler> CreateUpscaler(const UpscaleDesc& desc) = 0;
        virtual FrameGenerator* FrameGen() = 0;
        virtual LowLatency* Latency() = 0;
        // must run before the device is destroyed
        virtual void Shutdown() = 0;
    };
}
