#include <chrono>
#include "util/Relaunch.hpp"
#include "util/SystemInfo.hpp"
#include "util/Env.hpp"
#include <cstdio>
#include "client/renderer/bgfx/RenderContext_bgfx.hpp"

#include "client/platform/AppPlatform.hpp"
#include "client/renderer/Tessellator.hpp"
#include "client/renderer/MaterialDb.hpp"
#include "client/renderer/bgfx/GpuSelect.hpp"
#include "client/Options.hpp"
#include "client/resources/ShaderPack.hpp"
#include "client/ui/UiAssets.hpp"
#include "world/level/block/BlockPalette.hpp"
#include "util/Paths.hpp"

#include <bx/math.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace perlin {
    std::string RenderContext_bgfx::GpuVendor() const {
        const bgfx::Caps* caps = bgfx::getCaps();
        return caps ? sysinfo::VendorName(caps->vendorId) : std::string();
    }

    std::string RenderContext_bgfx::GpuDescription() const {
        const bgfx::Caps* caps = bgfx::getCaps();
        if (!caps)
            return {};
        const std::string name = sysinfo::GpuName(caps->vendorId, caps->deviceId);
        if (!name.empty())
            return name;
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "GPU %04x:%04x", caps->vendorId, caps->deviceId);
        return buffer;
    }

    namespace {
        constexpr bgfx::ViewId kViewHand = 2;
        constexpr bgfx::ViewId kViewGui = 3;
        constexpr bgfx::ViewId kViewEntity = 4;
        constexpr bgfx::ViewId kViewBlur = 5;
        constexpr bgfx::ViewId kViewBlurOut = 6;
        constexpr bgfx::ViewId kViewLate = 8;
        constexpr bgfx::ViewId kViewDoll = 7;
        constexpr bgfx::ViewId kViewTop = 9;
        constexpr bgfx::ViewId kViewCopy = 10;
        constexpr bgfx::ViewId kViewBloom = 11;
        constexpr bgfx::ViewId kViewShadowBuild = 1;
        constexpr bgfx::ViewId kViewShadowEntity = 18;
        constexpr bgfx::ViewId kViewShadowGlass = 19;
        constexpr bgfx::ViewId kViewUp0 = 20;
        constexpr bgfx::ViewId kViewUp1 = 21;
        constexpr bgfx::ViewId kViewCopy2 = 22;
        constexpr bgfx::ViewId kViewPresent = 23;
        constexpr bgfx::ViewId kViewGen = 24;
        constexpr bgfx::ViewId kViewExportUi = 25;
        constexpr uint32_t kPageVerts = 1u << 20;
        constexpr uint32_t kPageIdx = (kPageVerts / 4) * 6;

        glm::dmat4 ConvertDepthD(glm::dmat4 m, bool homogeneous) {
            if (homogeneous)
                for (int c = 0; c < 4; c++)
                    m[c][2] = 2.0 * m[c][2] - m[c][3];
            return m;
        }

        float Halton(uint32_t index, uint32_t base) {
            float f = 1.0f, r = 0.0f;
            while (index > 0) {
                f /= static_cast<float>(base);
                r += f * static_cast<float>(index % base);
                index /= base;
            }
            return r;
        }

        void ApplyJitter(float m[16], float ox, float oy) {
            for (int c = 0; c < 4; c++) {
                m[c * 4 + 0] += ox * m[c * 4 + 3];
                m[c * 4 + 1] += oy * m[c * 4 + 3];
            }
        }

        std::string Env(const char* name) {
            const char* v = std::getenv(name);
            return v ? v : "";
        }

        void DownsampleAlphaAware(const uint8_t* src, const size_t idx[4], uint8_t* d) {
            unsigned sum[4] = {0, 0, 0, 0};
            unsigned n = 0, na = 0;
            for (int k = 0; k < 4; k++) {
                na += src[idx[k] + 3];
                if (src[idx[k] + 3] > 128) {
                    for (int c = 0; c < 4; c++)
                        sum[c] += src[idx[k] + c];
                    n++;
                }
            }
            if (n == 0) {
                for (int c = 0; c < 3; c++) {
                    unsigned total = 0;
                    for (int k = 0; k < 4; k++)
                        total += src[idx[k] + c];
                    d[c] = (uint8_t)((total + 2) / 4);
                }
                d[3] = (uint8_t)((na + 2) / 4);
            } else {
                for (int c = 0; c < 3; c++)
                    d[c] = (uint8_t)((sum[c] + n / 2) / n);
                d[3] = (uint8_t)((na + 2) / 4);
            }
        }
    }

    class RenderContext_bgfx::Callback : public bgfx::CallbackI {
    public:
        explicit Callback(RenderContext_bgfx* owner) : owner(owner) {}
        void fatal(const char* filePath, uint16_t line, bgfx::Fatal::Enum code, const char* str) override {
            std::cout << "[bgfx] FATAL " << filePath << ":" << line << " (" << (int)code << "): " << str
                      << std::endl;
            const std::string message = str ? str : "";
            if (code == bgfx::Fatal::DeviceLost || message.find("DEVICE_REMOVED") != std::string::npos ||
                message.find("DEVICE_RESET") != std::string::npos || message.find("DEVICE_HUNG") != std::string::npos) {
                std::cout << "[bgfx] the GPU driver reset the device; restarting Perlin" << std::endl;
                if (RelaunchSelf())
                    std::_Exit(0);
            }
            std::abort();
        }
        void traceVargs(const char* filePath, uint16_t line, const char* format, va_list args) override {
            if (!PERLIN_ENV("PERLIN_BGFX_TRACE"))
                return;
            char buf[2048];
            vsnprintf(buf, sizeof(buf), format, args);
            std::cout << "[bgfx] " << filePath << ":" << line << " " << buf;
        }
        void profilerBegin(const char*, uint32_t, const char*, uint16_t) override {}
        void profilerBeginLiteral(const char*, uint32_t, const char*, uint16_t) override {}
        void profilerEnd() override {}
        uint32_t cacheReadSize(uint64_t) override { return 0; }
        bool cacheRead(uint64_t, void*, uint32_t) override { return false; }
        void cacheWrite(uint64_t, const void*, uint32_t) override {}
        void screenShot(const char* filePath, uint32_t w, uint32_t h, uint32_t pitch,
                        bgfx::TextureFormat::Enum format, const void* data, uint32_t, bool yflip) override {
            if (filePath && std::strcmp(filePath, "perlin:capture") == 0)
                owner->DeliverCapture(w, h, pitch, static_cast<const uint8_t*>(data), yflip, format == bgfx::TextureFormat::RGBA8);
            else
                owner->SaveScreenshotData(filePath, w, h, pitch, static_cast<const uint8_t*>(data), yflip);
        }
        void captureBegin(uint32_t, uint32_t, uint32_t, bgfx::TextureFormat::Enum, bool) override {}
        void captureEnd() override {}
        void captureFrame(const void*, uint32_t) override {}

    private:
        RenderContext_bgfx* owner;
    };

    RenderContext_bgfx::RenderContext_bgfx() = default;
    RenderContext_bgfx::~RenderContext_bgfx() = default;

    uint32_t RenderContext_bgfx::ResetFlags() const {
        uint32_t flags = BGFX_RESET_NONE;
        if (vsync)
            flags |= BGFX_RESET_VSYNC;
        const std::string msaa = Env("PERLIN_MSAA");
        if (msaa == "2")
            flags |= BGFX_RESET_MSAA_X2;
        else if (msaa == "4")
            flags |= BGFX_RESET_MSAA_X4;
        else if (msaa == "8")
            flags |= BGFX_RESET_MSAA_X8;
        else if (msaa == "16")
            flags |= BGFX_RESET_MSAA_X16;
        return flags;
    }

    bool RenderContext_bgfx::Initialize(AppPlatform& p, const Options& options) {
        platform = &p;
        vsync = options.vsync;

        NativeWindowInfo native = platform->GetNativeWindowInfo();
        platform->GetFramebufferSize(width, height);

        callback = std::make_unique<Callback>(this);

        bgfx::Init init;
        init.swapChain.maxFrameLatency = 1;
        std::string forced = Env("PERLIN_RENDERER");
        if (forced.empty())
            forced = options.renderer;
        init.type = bgfx::RendererType::Count;
        if (forced == "vulkan")
            init.type = bgfx::RendererType::Vulkan;
        else if (forced == "opengl" || forced == "gl")
            init.type = bgfx::RendererType::OpenGL;
        else if (forced == "d3d11")
            init.type = bgfx::RendererType::Direct3D11;
        else if (forced == "metal")
            init.type = bgfx::RendererType::Metal;

#ifdef __ANDROID__
        init.type = bgfx::RendererType::OpenGLES;
#endif
        ShaderPack& pack = ShaderPack::Instance();
        if (!options.shaderPack.empty()) {
            std::string error;
            if (!pack.Open(paths::DataDir() + "/shaderpacks", options.shaderPack, error))
                std::cout << "[shaders] " << error << " - using the built-in shaders" << std::endl;
        }
        if (pack.Active() && init.type == bgfx::RendererType::Count) {
#if defined(_WIN32)
            const bool native = pack.HasBackend("dxbc");
#elif defined(__APPLE__)
            const bool native = pack.HasBackend("metal");
#else
            const bool native = pack.HasBackend("spirv") && pack.HasBackend("glsl");
#endif
            if (!native) {
                if (pack.HasBackend("spirv"))
                    init.type = bgfx::RendererType::Vulkan;
                else if (pack.HasBackend("glsl"))
                    init.type = bgfx::RendererType::OpenGL;
            }
        }
        gpu::Choice gpuChoice;
        const bool autoType = init.type == bgfx::RendererType::Count || forced == "auto";
        gpuChoice = gpu::ChooseRenderer();
        if (gpuChoice.type != bgfx::RendererType::Count) {
            if (autoType)
                init.type = gpuChoice.type;
            init.vendorId = gpuChoice.vendorId;
            init.deviceId = gpuChoice.deviceId;
            std::cout << "[gpu] " << gpuChoice.adapterName << ": " << bgfx::getRendererName(init.type) << " ("
                      << (autoType ? gpuChoice.reason : std::string("chosen in settings")) << ")" << std::endl;
        }
        init.fallback = true;
        nativeWindow = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(native.window));
        nativeDisplay = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(native.display));
        adapterName = gpuChoice.adapterName;
        init.swapChain.nwh = native.window;
        init.swapChain.ndt = native.display;
        if (native.wayland)
            init.platformData.type = bgfx::NativeWindowHandleType::Wayland;
        init.swapChain.width = (uint32_t)width;
        init.swapChain.height = (uint32_t)height;
        init.reset = ResetFlags();
        init.debug = false;
        init.profile = false;
        init.limits.maxTransientVbSize = 32u << 20;
        init.limits.maxTransientIbSize = 16u << 20;
        init.callback = callback.get();
        if (!bgfx::init(init)) {
            std::cout << "[bgfx] init failed" << std::endl;
            return false;
        }
        initialized = true;
        caveCulling = !PERLIN_ENV("PERLIN_NO_CAVECULL");

        const bgfx::Caps* caps = bgfx::getCaps();
        homogeneousDepth = caps->homogeneousDepth;
        rendererName = bgfx::getRendererName(bgfx::getRendererType());
        std::cout << "[bgfx] renderer: " << bgfx::getRendererName(bgfx::getRendererType())
                  << (homogeneousDepth ? " (clip z -1..1)" : " (clip z 0..1)") << std::endl;

        layout.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord1, 4, bgfx::AttribType::Uint8, true)
            .end();
        static_assert(sizeof(Vertex) == 28, "Vertex layout must match the bgfx vertex layout");

        positionLayout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).end();
        colorLayout.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
            .end();
        textureLayout.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .end();
        static_assert(sizeof(PositionVertex) == 12 && sizeof(ColorVertex) == 16 && sizeof(TextureVertex) == 20);

        auto upload = [](const auto& vertices, const bgfx::VertexLayout& vertexLayout) {
            return bgfx::createVertexBuffer(
                bgfx::copy(vertices.data(), static_cast<uint32_t>(vertices.size() * sizeof(vertices[0]))), vertexLayout);
        };
        auto uploadIndices = [](const std::vector<uint16_t>& indices) {
            return bgfx::createIndexBuffer(
                bgfx::copy(indices.data(), static_cast<uint32_t>(indices.size() * sizeof(uint16_t))));
        };
        const std::vector<PositionVertex> topSky = BuildSkyDisc(16.0f);
        topSkyBuffer = upload(topSky, positionLayout);
        bottomSkyBuffer = upload(BuildSkyDisc(-16.0f), positionLayout);
        {
            std::vector<PositionVertex> box;
            for (int i = 0; i < 8; i++)
                box.push_back({(i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f});
            skyBoxBuffer = upload(box, positionLayout);
            skyBoxIndices = uploadIndices({0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5, 0, 4, 5, 0, 5, 1,
                                           2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3});
        }
        {
            const std::vector<PositionVertex> screen = {{-1.0f, -1.0f, 0.0f}, {3.0f, -1.0f, 0.0f}, {-1.0f, 3.0f, 0.0f}};
            screenBuffer = upload(screen, positionLayout);
        }
        const std::vector<uint16_t> skyFan = FanIndices(static_cast<int>(topSky.size()));
        skyFanIndexCount = static_cast<uint32_t>(skyFan.size());
        skyFanIndices = uploadIndices(skyFan);

        const std::vector<ColorVertex> sunrise = BuildSunriseFan();
        sunriseBuffer = upload(sunrise, colorLayout);
        const std::vector<uint16_t> sunriseFan = FanIndices(static_cast<int>(sunrise.size()));
        sunriseIndexCount = static_cast<uint32_t>(sunriseFan.size());
        sunriseIndices = uploadIndices(sunriseFan);

        const std::vector<PositionVertex> stars = BuildStars();
        starBuffer = upload(stars, positionLayout);
        const std::vector<uint16_t> starQuads = QuadIndices(static_cast<int>(stars.size() / 4));
        starIndexCount = static_cast<uint32_t>(starQuads.size());
        starIndices = uploadIndices(starQuads);

        sunBuffer = upload(BuildSunQuad(), textureLayout);
        moonBuffer = upload(BuildMoonQuads(), textureLayout);
        quadIndices = uploadIndices(QuadIndices(1));

        const bool shadersOk = shaders.Initialize();
        shadowConfigDirty = true;
        if (!shadersOk) {
            std::cout << "[bgfx] shader program creation failed for "
                      << bgfx::getRendererName(bgfx::getRendererType()) << std::endl;
            return false;
        }

        samplerFlags = BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;

        std::string cull = Env("PERLIN_CULL");
        if (!cull.empty() && cull[0] == '0')
            cullState = 0;
        else if (!cull.empty() && (cull[0] == 'c' || cull[0] == 'C'))
            cullState = BGFX_STATE_CULL_CW;

        const uint32_t white = 0xFFFFFFFFu;
        const uint32_t whiteLayers[2] = {white, white};
        whiteTex = bgfx::createTexture2D(1, 1, false, 2, bgfx::TextureFormat::RGBA8, samplerFlags,
                                         bgfx::copy(whiteLayers, sizeof(whiteLayers)));

        lightmapFlags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
        lightmapTex = bgfx::createTexture2D(kLightmapSize, kLightmapSize, false, 1,
                                            bgfx::TextureFormat::RGBA8, lightmapFlags, nullptr);

        bgfx::setViewMode(0, bgfx::ViewMode::Sequential);
        bgfx::setViewMode(kViewHand, bgfx::ViewMode::Sequential);
        bgfx::setViewMode(kViewEntity, bgfx::ViewMode::Sequential);
        bgfx::setViewMode(kViewLate, bgfx::ViewMode::Sequential);
        {
            const bgfx::ViewId order[25] = {kViewShadowBuild, kViewShadowGlass, kViewShadowEntity, 0, kViewEntity, kViewCopy, kViewLate,
                                            kViewBloom, kViewBloom + 1, kViewBloom + 2, kViewBloom + 3, kViewBloom + 4, kViewBloom + 5, kViewBloom + 6,
                                            kViewBlur, kViewUp0, kViewUp1, kViewBlurOut, kViewCopy2, kViewHand, kViewGui, kViewDoll, kViewTop,
                                            kViewPresent, kViewGen};
            bgfx::setViewOrder(0, 25, order);
            for (const bgfx::ViewId v : {kViewUp0, kViewUp1, kViewCopy2, kViewPresent, kViewGen})
                bgfx::setViewMode(v, bgfx::ViewMode::Sequential);
            bgfx::setViewMode(kViewShadowGlass, bgfx::ViewMode::Sequential);
            bgfx::setViewMode(kViewShadowEntity, bgfx::ViewMode::Sequential);
            for (int i = 0; i < 7; i++)
                bgfx::setViewMode(static_cast<bgfx::ViewId>(kViewBloom + i), bgfx::ViewMode::Sequential);
        }
        bgfx::setViewMode(kViewGui, bgfx::ViewMode::Sequential);
        bgfx::setViewMode(kViewDoll, bgfx::ViewMode::Sequential);
        bgfx::setViewMode(kViewTop, bgfx::ViewMode::Sequential);
        bgfx::setViewMode(kViewExportUi, bgfx::ViewMode::Sequential);
        bgfx::setViewMode(kViewBlur, bgfx::ViewMode::Sequential);
        bgfx::setViewMode(kViewBlurOut, bgfx::ViewMode::Sequential);

        uiLayout.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .end();
        rdMaterial = bgfx::createUniform("u_rdMaterial", bgfx::UniformType::Vec4);
        uiSampler = bgfx::createUniform("s_uiTex", bgfx::UniformType::Sampler);
        uiParams = bgfx::createUniform("u_uiParams", bgfx::UniformType::Vec4);
        uiOverlay = bgfx::createUniform("u_uiOverlay", bgfx::UniformType::Vec4);
        uiGlintSampler = bgfx::createUniform("s_uiGlint", bgfx::UniformType::Sampler);
        uiGlintA = bgfx::createUniform("u_uiGlintA", bgfx::UniformType::Vec4);
        uiGlintB = bgfx::createUniform("u_uiGlintB", bgfx::UniformType::Vec4);
        {
            const uint32_t whitePixel = 0xFFFFFFFFu;
            uiWhite = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8,
                                            BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
                                            bgfx::copy(&whitePixel, 4));
        }
        return true;
    }

    uint64_t RenderContext_bgfx::PassState(RenderLayer layer) const {
        uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS;
        switch (layer) {
        case RenderLayer::Solid:
            state |= BGFX_STATE_WRITE_A | cullState;
            break;
        case RenderLayer::Cutout:
            state = (state & ~BGFX_STATE_DEPTH_TEST_MASK) | BGFX_STATE_DEPTH_TEST_LEQUAL;
            state |= BGFX_STATE_WRITE_A;
            break;
        case RenderLayer::CutoutCull:
            state = (state & ~BGFX_STATE_DEPTH_TEST_MASK) | BGFX_STATE_DEPTH_TEST_LEQUAL;
            state |= BGFX_STATE_WRITE_A | cullState;
            break;
        case RenderLayer::TranslucentCull:
            state |= cullState;
            state &= ~BGFX_STATE_WRITE_Z;
            [[fallthrough]];
        case RenderLayer::Translucent:
            state |=
                BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA,
                                               BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);
            break;
        }
        return state;
    }

    void RenderContext_bgfx::UploadLightmap(const uint8_t* pixels) {
        if (lightmapValid && std::memcmp(lastLightmap, pixels, kLightmapBytes) == 0)
            return;
        std::memcpy(lastLightmap, pixels, kLightmapBytes);
        lightmapValid = true;
        bgfx::updateTexture2D(lightmapTex, 0, 0, 0, 0, kLightmapSize, kLightmapSize,
                              bgfx::copy(pixels, kLightmapBytes), kLightmapSize * 4);
    }

    void RenderContext_bgfx::Resize(int w, int h) {
        if (!initialized || w <= 0 || h <= 0)
            return;
        if (w == width && h == height)
            return;
        width = w;
        height = h;
        bgfx::SwapChain sc;
        sc.width = (uint32_t)w;
        sc.height = (uint32_t)h;
        bgfx::reset(ResetFlags(), &sc);
    }

    void RenderContext_bgfx::SetVsync(bool enabled) {
        if (!initialized || vsync == enabled)
            return;
        vsync = enabled;
        bgfx::SwapChain sc;
        sc.width = static_cast<uint32_t>(width);
        sc.height = static_cast<uint32_t>(height);
        bgfx::reset(ResetFlags(), &sc);
    }

    bool RenderContext_bgfx::EnsureMotionBlurTargets() {
        if (blur.targets)
            return true;
        const uint64_t sample = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
        const auto colorTarget = [&]() {
            return blur.scaled ? bgfx::createTexture2D(static_cast<uint16_t>(blur.w), static_cast<uint16_t>(blur.h), false, 1,
                                                       bgfx::TextureFormat::RGBA8, sample)
                               : bgfx::createTexture2D(bgfx::BackbufferRatio::Equal, false, 1, bgfx::TextureFormat::RGBA8, sample);
        };
        blur.worldColor = colorTarget();
        const uint64_t depthFlags = depthReadable ? BGFX_TEXTURE_RT : BGFX_TEXTURE_RT_WRITE_ONLY;
        blur.worldDepth = blur.scaled ? bgfx::createTexture2D(static_cast<uint16_t>(blur.w), static_cast<uint16_t>(blur.h), false, 1,
                                                              bgfx::TextureFormat::D24S8, depthFlags)
                                      : bgfx::createTexture2D(bgfx::BackbufferRatio::Equal, false, 1, bgfx::TextureFormat::D24S8, depthFlags);
        for (int i = 0; i < 2; i++)
            blur.histColor[i] = colorTarget();
        bool ok = bgfx::isValid(blur.worldColor) && bgfx::isValid(blur.worldDepth) && bgfx::isValid(blur.histColor[0]) &&
                  bgfx::isValid(blur.histColor[1]);
        if (ok) {
            const bgfx::TextureHandle world[2] = {blur.worldColor, blur.worldDepth};
            blur.worldFb = bgfx::createFrameBuffer(2, world, false);
            for (int i = 0; i < 2; i++) {
                const bgfx::TextureHandle h[1] = {blur.histColor[i]};
                blur.histFb[i] = bgfx::createFrameBuffer(1, h, false);
            }
            ok = bgfx::isValid(blur.worldFb) && bgfx::isValid(blur.histFb[0]) && bgfx::isValid(blur.histFb[1]);
        }
        if (!ok) {
            std::cout << "[blur] could not create the motion blur targets; disabled" << std::endl;
            DestroyMotionBlur();
            blur.targets = false;
            return false;
        }
        blur.sampCur = bgfx::createUniform("s_cur", bgfx::UniformType::Sampler);
        blur.sampHist = bgfx::createUniform("s_hist", bgfx::UniformType::Sampler);
        blur.params = bgfx::createUniform("u_post", bgfx::UniformType::Vec4);
        blur.fx = bgfx::createUniform("u_postFx", bgfx::UniformType::Vec4);
        blur.targets = true;
        blur.valid = false;
        return true;
    }

    void RenderContext_bgfx::DestroyMotionBlur() {
        const auto kill = [](auto& h) {
            if (bgfx::isValid(h))
                bgfx::destroy(h);
            h = BGFX_INVALID_HANDLE;
        };
        kill(blur.worldFb);
        kill(blur.histFb[0]);
        kill(blur.histFb[1]);
        kill(blur.worldColor);
        kill(blur.worldDepth);
        kill(blur.histColor[0]);
        kill(blur.histColor[1]);
        kill(blur.sampCur);
        kill(blur.sampHist);
        kill(blur.params);
        kill(blur.fx);
        blur.targets = false;
        blur.scaled = false;
        blur.valid = false;
    }


    namespace {
        glm::dmat4 DoubleMatrix(const float* m) {
            glm::dmat4 r(1.0);
            for (int c = 0; c < 4; c++)
                for (int row = 0; row < 4; row++)
                    r[c][row] = static_cast<double>(m[c * 4 + row]);
            return r;
        }

        void StoreMatrix(const glm::dmat4& m, float* out) {
            for (int c = 0; c < 4; c++)
                for (int row = 0; row < 4; row++)
                    out[c * 4 + row] = static_cast<float>(m[c][row]);
        }
    }

    void RenderContext_bgfx::PlanFrame(const FrameParams& params) {
        plan = Plan{};
        static const bool disabled = PERLIN_ENV("PERLIN_NO_UPSCALE") != nullptr;
        static const bool noFrameGen = PERLIN_ENV("PERLIN_NO_FRAMEGEN") != nullptr;
        static const bool noSsr = PERLIN_ENV("PERLIN_NO_SSR") != nullptr;
        const float scale = disabled ? 1.0f : std::clamp(params.renderScale, 0.25f, 1.0f);
        plan.scaled = scale < 0.999f;
        plan.w = plan.scaled ? std::max(32, static_cast<int>(std::lround(static_cast<float>(width) * scale))) : width;
        plan.h = plan.scaled ? std::max(32, static_cast<int>(std::lround(static_cast<float>(height) * scale))) : height;
        plan.temporal = !disabled && params.upscaler == 3 && !upscale.failed && shaders.Has(ShaderProgram::Temporal);
        plan.frameGen = params.frameGen >= 2 && !fg.failed && shaders.Has(ShaderProgram::FrameGen) &&
                        shaders.Has(ShaderProgram::Upscale) && !noFrameGen;
        ShaderPack& pack = ShaderPack::Instance();
        const bool sceneWanted = pack.Active() && pack.Flag("sceneCopy", false) && !scene.failed && !noSsr;
        plan.wantDepth = sceneWanted || plan.temporal || plan.frameGen;
        if (plan.temporal) {
            const uint32_t index = (upscale.frameIndex++ % 16) + 1;
            const float jx = Halton(index, 2) - 0.5f, jy = Halton(index, 3) - 0.5f;
            const float ySign = bgfx::getCaps()->originBottomLeft ? 1.0f : -1.0f;
            plan.jitter[0] = 2.0f * jx / static_cast<float>(plan.w);
            plan.jitter[1] = 2.0f * jy / static_cast<float>(plan.h);
            plan.jitterUv[0] = jx / static_cast<float>(plan.w);
            plan.jitterUv[1] = ySign * jy / static_cast<float>(plan.h);
        }
    }

    void RenderContext_bgfx::EnsureUpscaleUniforms() {
        const auto make = [](bgfx::UniformHandle& h, const char* name, bgfx::UniformType::Enum type) {
            if (!bgfx::isValid(h))
                h = bgfx::createUniform(name, type);
        };
        make(upscale.mode, "u_up", bgfx::UniformType::Vec4);
        make(upscale.size, "u_upSize", bgfx::UniformType::Vec4);
        make(upscale.taa, "u_taa", bgfx::UniformType::Vec4);
        make(upscale.taaSize, "u_taaSize", bgfx::UniformType::Vec4);
        make(upscale.taaJit, "u_taaJit", bgfx::UniformType::Vec4);
        make(upscale.taaInvVp, "u_taaInvVp", bgfx::UniformType::Mat4);
        make(upscale.taaPrevVp, "u_taaPrevVp", bgfx::UniformType::Mat4);
        make(upscale.sampDepth, "s_depth", bgfx::UniformType::Sampler);
    }

    bool RenderContext_bgfx::EnsureUpscale() {
        if (upscale.failed)
            return false;
        if (bgfx::isValid(upscale.tex[0]) && upscale.w == width && upscale.h == height) {
            EnsureUpscaleUniforms();
            return true;
        }
        DestroyUpscale();
        if (!shaders.Has(ShaderProgram::Upscale) && !shaders.Has(ShaderProgram::Temporal)) {
            upscale.failed = true;
            return false;
        }
        const uint64_t flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
        bool ok = true;
        for (int i = 0; i < 3; i++) {
            upscale.tex[i] = bgfx::createTexture2D(static_cast<uint16_t>(width), static_cast<uint16_t>(height), false, 1,
                                                   bgfx::TextureFormat::RGBA8, flags);
            if (bgfx::isValid(upscale.tex[i]))
                upscale.fb[i] = bgfx::createFrameBuffer(1, &upscale.tex[i], false);
            ok = ok && bgfx::isValid(upscale.tex[i]) && bgfx::isValid(upscale.fb[i]);
        }
        if (!ok) {
            std::cout << "[render] could not create the upscaler targets; using plain bilinear" << std::endl;
            DestroyUpscale();
            upscale.failed = true;
            return false;
        }
        upscale.w = width;
        upscale.h = height;
        upscale.historyValid = false;
        EnsureUpscaleUniforms();
        return true;
    }

    void RenderContext_bgfx::DestroyUpscale() {
        for (int i = 0; i < 3; i++) {
            if (bgfx::isValid(upscale.fb[i]))
                bgfx::destroy(upscale.fb[i]);
            if (bgfx::isValid(upscale.tex[i]))
                bgfx::destroy(upscale.tex[i]);
            upscale.fb[i] = BGFX_INVALID_HANDLE;
            upscale.tex[i] = BGFX_INVALID_HANDLE;
        }
        upscale.w = upscale.h = 0;
        upscale.historyValid = false;
    }

    bgfx::TextureHandle RenderContext_bgfx::RunUpscale(bgfx::TextureHandle source, int sourceW, int sourceH, const FrameParams& params) {
        const bool scaled = sourceW < width || sourceH < height;
        const bool temporal = plan.temporal && bgfx::isValid(blur.worldDepth) && bgfx::isValid(blur.params);
        if (PERLIN_ENV("PERLIN_NO_UPSCALE") || (!temporal && (!scaled || params.upscaler == 0))) {
            if (!scaled && !temporal && bgfx::isValid(upscale.tex[0]))
                DestroyUpscale();
            return source;
        }
        if (!EnsureUpscale())
            return source;
        const bool haveSpatial = shaders.Has(ShaderProgram::Upscale);
        if (!temporal && !haveSpatial)
            return source;
        const float origin = bgfx::getCaps()->originBottomLeft ? 1.0f : 0.0f;
        const float sharp = std::clamp(params.upscaleSharpness, 0.0f, 1.0f);
        const auto pass = [&](bgfx::ViewId view, int target, bgfx::TextureHandle from, float mode, int fromW, int fromH) {
            bgfx::setViewFrameBuffer(view, upscale.fb[target]);
            bgfx::setViewRect(view, 0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height));
            const float post[4] = {0.0f, origin, 0.0f, 0.0f};
            bgfx::setUniform(blur.params, post);
            const float m[4] = {mode, sharp, 0.0f, 0.0f};
            bgfx::setUniform(upscale.mode, m);
            const float sz[4] = {static_cast<float>(fromW), static_cast<float>(fromH), static_cast<float>(width), static_cast<float>(height)};
            bgfx::setUniform(upscale.size, sz);
            bgfx::setTexture(0, blur.sampCur, from, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
            bgfx::setVertexBuffer(0, screenBuffer);
            bgfx::setState(BGFX_STATE_WRITE_RGB);
            bgfx::submit(view, shaders.Program(ShaderProgram::Upscale));
        };
        const auto sharpen = [&](bgfx::TextureHandle from, int target) -> bgfx::TextureHandle {
            if (sharp <= 0.001f || !haveSpatial)
                return from;
            pass(kViewUp1, target, from, 2.0f, width, height);
            return upscale.tex[target];
        };

        if (!temporal) {
            upscale.historyValid = false;
            pass(kViewUp0, 0, source, params.upscaler == 1 ? 0.0f : 1.0f, sourceW, sourceH);
            return sharpen(upscale.tex[0], 1);
        }

        const int write = 1 - upscale.history;
        const int read = upscale.history;
        const bool useHistory = upscale.historyValid && cam.havePrev && !cam.teleported;
        const double ratio = static_cast<double>(sourceW) / static_cast<double>(std::max(1, width));
        const float alpha = static_cast<float>(0.08 + (1.0 - std::min(1.0, ratio)) * 0.2);
        const float ySign = origin > 0.5f ? 1.0f : -1.0f;
        const glm::dmat4 shift = glm::translate(glm::dmat4(1.0), cam.eye);
        const glm::dmat4 curVp = ConvertDepthD(cam.proj * cam.view * shift, homogeneousDepth);
        const glm::dmat4 prevVp = ConvertDepthD(cam.prevProj * cam.prevView * shift, homogeneousDepth);
        float invVp[16], prevVpF[16];
        StoreMatrix(glm::inverse(curVp), invVp);
        StoreMatrix(prevVp, prevVpF);
        bgfx::setViewFrameBuffer(kViewUp0, upscale.fb[write]);
        bgfx::setViewRect(kViewUp0, 0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height));
        const float post[4] = {0.0f, origin, 0.0f, 0.0f};
        bgfx::setUniform(blur.params, post);
        const float taa[4] = {alpha, useHistory ? 1.0f : 0.0f, ySign, homogeneousDepth ? 1.0f : 0.0f};
        bgfx::setUniform(upscale.taa, taa);
        const float taaSize[4] = {static_cast<float>(sourceW), static_cast<float>(sourceH), static_cast<float>(width), static_cast<float>(height)};
        bgfx::setUniform(upscale.taaSize, taaSize);
        const float jit[4] = {plan.jitterUv[0], plan.jitterUv[1], 0.0f, 0.0f};
        bgfx::setUniform(upscale.taaJit, jit);
        bgfx::setUniform(upscale.taaInvVp, invVp);
        bgfx::setUniform(upscale.taaPrevVp, prevVpF);
        const uint64_t point = BGFX_SAMPLER_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
        bgfx::setTexture(0, blur.sampCur, source, point);
        bgfx::setTexture(1, upscale.sampDepth, blur.worldDepth, point);
        bgfx::setTexture(2, blur.sampHist, upscale.tex[read], BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        bgfx::setVertexBuffer(0, screenBuffer);
        bgfx::setState(BGFX_STATE_WRITE_RGB);
        bgfx::submit(kViewUp0, shaders.Program(ShaderProgram::Temporal));
        upscale.history = write;
        upscale.historyValid = true;
        return sharpen(upscale.tex[write], 2);
    }


    bool RenderContext_bgfx::EnsureFrameGen() {
        if (fg.failed)
            return false;
        if (fg.ready)
            return true;
        const uint64_t clamp = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
        fg.finalColor = bgfx::createTexture2D(bgfx::BackbufferRatio::Equal, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT | clamp);
        fg.finalDepth = bgfx::createTexture2D(bgfx::BackbufferRatio::Equal, false, 1, bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT_WRITE_ONLY);
        fg.worldCopy = bgfx::createTexture2D(bgfx::BackbufferRatio::Equal, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_BLIT_DST | clamp);
        bool ok = bgfx::isValid(fg.finalColor) && bgfx::isValid(fg.finalDepth) && bgfx::isValid(fg.worldCopy);
        if (ok) {
            const bgfx::TextureHandle att[2] = {fg.finalColor, fg.finalDepth};
            fg.finalFb = bgfx::createFrameBuffer(2, att, false);
            ok = bgfx::isValid(fg.finalFb);
        }
        if (!ok) {
            std::cout << "[render] could not create the frame generation targets; frame generation disabled" << std::endl;
            DestroyFrameGen();
            fg.failed = true;
            return false;
        }
        EnsureUpscaleUniforms();
        fg.gen = bgfx::createUniform("u_gen", bgfx::UniformType::Vec4);
        fg.invVp = bgfx::createUniform("u_genInvVp", bgfx::UniformType::Mat4);
        fg.curVp = bgfx::createUniform("u_genCurVp", bgfx::UniformType::Mat4);
        fg.sampWorld = bgfx::createUniform("s_world", bgfx::UniformType::Sampler);
        fg.sampDepth = bgfx::createUniform("s_depth", bgfx::UniformType::Sampler);
        fg.sampFinal = bgfx::createUniform("s_final", bgfx::UniformType::Sampler);
        fg.ready = true;
        return true;
    }

    void RenderContext_bgfx::DestroyFrameGen() {
        const auto kill = [](auto& h) {
            if (bgfx::isValid(h))
                bgfx::destroy(h);
            h = BGFX_INVALID_HANDLE;
        };
        kill(fg.finalFb);
        kill(fg.finalColor);
        kill(fg.finalDepth);
        kill(fg.worldCopy);
        kill(fg.gen);
        kill(fg.invVp);
        kill(fg.curVp);
        kill(fg.sampWorld);
        kill(fg.sampDepth);
        kill(fg.sampFinal);
        fg.ready = false;
        fg.valid = false;
    }

    void RenderContext_bgfx::SubmitFrameGenCopies(float origin) {
        bgfx::TextureRegion dst, src;
        dst.init(fg.worldCopy);
        src.init(fg.finalColor);
        bgfx::blit(kViewCopy2, dst, src);
        bgfx::touch(kViewCopy2);

        bgfx::setViewFrameBuffer(kViewPresent, BGFX_INVALID_HANDLE);
        bgfx::setViewRect(kViewPresent, 0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height));
        const float post[4] = {0.0f, origin, 0.0f, 0.0f};
        bgfx::setUniform(blur.params, post);
        const float m[4] = {3.0f, 0.0f, 0.0f, 0.0f};
        bgfx::setUniform(upscale.mode, m);
        const float sz[4] = {static_cast<float>(width), static_cast<float>(height), static_cast<float>(width), static_cast<float>(height)};
        bgfx::setUniform(upscale.size, sz);
        bgfx::setTexture(0, blur.sampCur, fg.finalColor, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_POINT);
        bgfx::setVertexBuffer(0, screenBuffer);
        bgfx::setState(BGFX_STATE_WRITE_RGB);
        bgfx::submit(kViewPresent, shaders.Program(ShaderProgram::Upscale));
    }

    bool RenderContext_bgfx::PresentGenerated(int index, int total) {
        if (!initialized || exportTarget || !fg.ready || !fg.valid || !cam.haveCur || !cam.havePrev || cam.teleported || total < 2 ||
            !bgfx::isValid(blur.worldDepth) || !shaders.Has(ShaderProgram::FrameGen))
            return false;
        const double nowSec = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const double step = cam.time - cam.prevTime;
        if (!(step > 0.0005 && step < 0.25))
            return false;
        const double t = std::clamp((nowSec - cam.time) / step, 0.0, 1.0) * 0.95;
        (void)index;
        const glm::dmat4 shift = glm::translate(glm::dmat4(1.0), cam.eye);
        const glm::dmat4 curRel = cam.view * shift;
        const glm::dmat4 prevRel = cam.prevView * shift;
        const auto split = [](const glm::dmat4& m, glm::dmat3& r, glm::dvec3& c) {
            r = glm::dmat3(m);
            c = -(glm::transpose(r) * glm::dvec3(m[3]));
        };
        glm::dmat3 r0, r1;
        glm::dvec3 c0, c1;
        split(prevRel, r0, c0);
        split(curRel, r1, c1);
        glm::dquat q0 = glm::quat_cast(r0), q1 = glm::quat_cast(r1);
        if (glm::dot(q0, q1) < 0.0)
            q0 = -q0;
        const double angle = 2.0 * std::acos(std::clamp(glm::dot(q0, q1), -1.0, 1.0));
        if (!(angle < 0.5) || !(glm::length(c1 - c0) < 6.0))
            return false;
        double scale = 1.0;
        if (cam.havePrev2) {
            const glm::dmat4 prev2Rel = cam.prev2View * shift;
            glm::dmat3 r2;
            glm::dvec3 c2;
            split(prev2Rel, r2, c2);
            glm::dquat q2 = glm::quat_cast(r2);
            if (glm::dot(q2, q0) < 0.0)
                q2 = -q2;
            const double earlier = 2.0 * std::acos(std::clamp(glm::dot(q2, q0), -1.0, 1.0));
            const double lo = std::min(angle, earlier), hi = std::max(angle, earlier);
            if (hi > 1.0e-4 && lo < hi * 0.6)
                scale *= 0.4 + 0.6 * (lo / hi);
        }
        const double reach = angle * t * scale;
        if (reach > 0.09)
            scale *= 0.09 / reach;
        const double move = glm::length(c1 - c0) * t * scale;
        if (move > 1.2)
            scale *= 1.2 / move;
        const double tt = t * scale;
        const glm::dquat qg = glm::normalize(glm::slerp(q0, q1, 1.0 + tt));
        const glm::dmat3 rg = glm::mat3_cast(qg);
        const glm::dvec3 cg = c1 + (c1 - c0) * tt;
        glm::dmat4 viewGen(rg);
        viewGen[3] = glm::dvec4(-(rg * cg), 1.0);
        const glm::dmat4 genVp = ConvertDepthD(cam.proj * viewGen, homogeneousDepth);
        const glm::dmat4 curVp = ConvertDepthD(cam.proj * curRel, homogeneousDepth);
        float invGen[16], cur[16];
        StoreMatrix(glm::inverse(genVp), invGen);
        StoreMatrix(curVp, cur);

        const float origin = bgfx::getCaps()->originBottomLeft ? 1.0f : 0.0f;
        bgfx::setViewFrameBuffer(kViewGen, BGFX_INVALID_HANDLE);
        bgfx::setViewRect(kViewGen, 0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height));
        const float post[4] = {0.0f, origin, 0.0f, 0.0f};
        bgfx::setUniform(blur.params, post);
        const double pa = cam.proj[2][2], pb = cam.proj[3][2];
        const double nearZ = pb / pa, farZ = pb / (pa + 1.0);
        const float gen[4] = {static_cast<float>(nearZ), static_cast<float>(farZ), origin > 0.5f ? 1.0f : -1.0f, homogeneousDepth ? 1.0f : 0.0f};
        bgfx::setUniform(fg.gen, gen);
        bgfx::setUniform(fg.invVp, invGen);
        bgfx::setUniform(fg.curVp, cur);
        const uint64_t clamp = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
        bgfx::setTexture(0, fg.sampWorld, fg.worldCopy, clamp);
        bgfx::setTexture(1, fg.sampDepth, blur.worldDepth, clamp | BGFX_SAMPLER_POINT);
        bgfx::setTexture(2, fg.sampFinal, fg.finalColor, clamp | BGFX_SAMPLER_POINT);
        bgfx::setVertexBuffer(0, screenBuffer);
        bgfx::setState(BGFX_STATE_WRITE_RGB);
        bgfx::submit(kViewGen, shaders.Program(ShaderProgram::FrameGen));
        FinishReadbacks(bgfx::frame());
        ReleaseFrameMeshes();
        return true;
    }

    void RenderContext_bgfx::FinishReadbacks(uint32_t submitted) {
        while (!frameReadbacks.empty() && static_cast<int32_t>(submitted - frameReadbacks.front().ready) >= 0) {
            auto readback = std::move(frameReadbacks.front());
            frameReadbacks.pop_front();
            bgfx::destroy(readback.texture);
            for (size_t i = 0; i < readback.pixels.size(); i += 4)
                std::swap(readback.pixels[i], readback.pixels[i + 2]);
            readback.sink(readback.width, readback.height, readback.width * 4,
                          readback.pixels.data(), bgfx::getCaps()->originBottomLeft);
        }
    }

    bool RenderContext_bgfx::EnsureSceneCopy() {
        const int wantW = blur.scaled ? blur.w : 0, wantH = blur.scaled ? blur.h : 0;
        if (bgfx::isValid(scene.color) && bgfx::isValid(scene.depth)) {
            if (scene.w == wantW && scene.h == wantH)
                return true;
            DestroySceneCopy();
        }
        const uint64_t clamp = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
        const uint64_t depthFlags = BGFX_TEXTURE_RT | BGFX_TEXTURE_BLIT_DST | clamp | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT;
        if (wantW > 0) {
            scene.color = bgfx::createTexture2D(static_cast<uint16_t>(wantW), static_cast<uint16_t>(wantH), false, 1,
                                                bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_BLIT_DST | clamp);
            scene.depth = bgfx::createTexture2D(static_cast<uint16_t>(wantW), static_cast<uint16_t>(wantH), false, 1,
                                                bgfx::TextureFormat::D24S8, depthFlags);
        } else {
            scene.color = bgfx::createTexture2D(bgfx::BackbufferRatio::Equal, false, 1, bgfx::TextureFormat::RGBA8,
                                                BGFX_TEXTURE_BLIT_DST | clamp);
            scene.depth = bgfx::createTexture2D(bgfx::BackbufferRatio::Equal, false, 1, bgfx::TextureFormat::D24S8, depthFlags);
        }
        scene.w = wantW;
        scene.h = wantH;
        if (!bgfx::isValid(scene.color) || !bgfx::isValid(scene.depth)) {
            std::cout << "[shaders] could not create the scene copy; reflections fall back to the sky" << std::endl;
            DestroySceneCopy();
            scene.failed = true;
            return false;
        }
        std::cout << "[shaders] scene copy on (screen-space reflections)" << std::endl;
        return true;
    }

    void RenderContext_bgfx::DestroySceneCopy() {
        for (bgfx::TextureHandle* h : {&scene.color, &scene.depth})
            if (bgfx::isValid(*h)) {
                bgfx::destroy(*h);
                *h = BGFX_INVALID_HANDLE;
            }
        scene.valid = false;
    }

    bool RenderContext_bgfx::EnsureBloom() {
        if (bloom.ready)
            return true;
        if (bloom.failed || !shaders.Has(ShaderProgram::Bloom))
            return false;
        static const bgfx::BackbufferRatio::Enum kRatio[4] = {bgfx::BackbufferRatio::Half, bgfx::BackbufferRatio::Quarter,
                                                              bgfx::BackbufferRatio::Eighth, bgfx::BackbufferRatio::Sixteenth};
        bool ok = true;
        for (int i = 0; i < 4 && ok; i++) {
            bloom.level[i] = bgfx::createTexture2D(kRatio[i], false, 1, bgfx::TextureFormat::RGBA8,
                                                   BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
            if (bgfx::isValid(bloom.level[i]))
                bloom.fb[i] = bgfx::createFrameBuffer(1, &bloom.level[i], false);
            ok = bgfx::isValid(bloom.level[i]) && bgfx::isValid(bloom.fb[i]);
        }
        if (!ok) {
            std::cout << "[bloom] could not create the bloom targets; bloom off" << std::endl;
            DestroyBloom();
            bloom.failed = true;
            return false;
        }
        bloom.params = bgfx::createUniform("u_bloomParams", bgfx::UniformType::Vec4);
        bloom.texel = bgfx::createUniform("u_bloomTexel", bgfx::UniformType::Vec4);
        bloom.composite = bgfx::createUniform("u_bloom", bgfx::UniformType::Vec4);
        bloom.sampler = bgfx::createUniform("s_bloom", bgfx::UniformType::Sampler);
        bloom.ready = true;
        return true;
    }

    void RenderContext_bgfx::DestroyBloom() {
        for (int i = 0; i < 4; i++) {
            if (bgfx::isValid(bloom.fb[i]))
                bgfx::destroy(bloom.fb[i]);
            if (bgfx::isValid(bloom.level[i]))
                bgfx::destroy(bloom.level[i]);
            bloom.fb[i] = BGFX_INVALID_HANDLE;
            bloom.level[i] = BGFX_INVALID_HANDLE;
        }
        for (bgfx::UniformHandle* u : {&bloom.params, &bloom.texel, &bloom.composite, &bloom.sampler})
            if (bgfx::isValid(*u)) {
                bgfx::destroy(*u);
                *u = BGFX_INVALID_HANDLE;
            }
        bloom.ready = false;
    }

    void RenderContext_bgfx::RunBloom(float strength) {
        static const bgfx::BackbufferRatio::Enum kRatio[4] = {bgfx::BackbufferRatio::Half, bgfx::BackbufferRatio::Quarter,
                                                              bgfx::BackbufferRatio::Eighth, bgfx::BackbufferRatio::Sixteenth};
        const float origin = bgfx::getCaps()->originBottomLeft ? 1.0f : 0.0f;
        const float W = static_cast<float>(std::max(1, width)), H = static_cast<float>(std::max(1, height));
        const float threshold = 0.82f - 0.12f * std::clamp(strength, 0.0f, 1.0f);
        const auto pass = [&](int viewOffset, int target, bgfx::TextureHandle source, float sourceScale, float mode, bool additive) {
            const bgfx::ViewId view = static_cast<bgfx::ViewId>(kViewBloom + viewOffset);
            bgfx::setViewFrameBuffer(view, bloom.fb[target]);
            bgfx::setViewRect(view, 0, 0, kRatio[target]);
            const float post[4] = {0.0f, origin, 0.0f, 0.0f};
            bgfx::setUniform(blur.params, post);
            const float p[4] = {mode, threshold, 0.18f, 0.0f};
            bgfx::setUniform(bloom.params, p);
            const bool fromWorld = source.idx == blur.worldColor.idx && blur.scaled;
            const float t[4] = {fromWorld ? 1.0f / static_cast<float>(std::max(1, blur.w)) : sourceScale / W,
                                fromWorld ? 1.0f / static_cast<float>(std::max(1, blur.h)) : sourceScale / H, 0.0f, 0.0f};
            bgfx::setUniform(bloom.texel, t);
            bgfx::setTexture(0, blur.sampCur, source, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
            bgfx::setVertexBuffer(0, screenBuffer);
            bgfx::setState(BGFX_STATE_WRITE_RGB | (additive ? BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE) : 0));
            bgfx::submit(view, shaders.Program(ShaderProgram::Bloom));
        };
        pass(0, 0, blur.worldColor, 1.0f, 0.0f, false);
        pass(1, 1, bloom.level[0], 2.0f, 1.0f, false);
        pass(2, 2, bloom.level[1], 4.0f, 1.0f, false);
        pass(3, 3, bloom.level[2], 8.0f, 1.0f, false);
        pass(4, 2, bloom.level[3], 16.0f, 2.0f, true);
        pass(5, 1, bloom.level[2], 8.0f, 2.0f, true);
        pass(6, 0, bloom.level[1], 4.0f, 2.0f, true);
    }

    void RenderContext_bgfx::SetupMotionBlur(const FrameParams& params) {
        const float intensity = std::clamp(params.motionBlur, 0.0f, 1.0f);
        static bool failed = false;
        const float menuPx = std::max(0.0f, params.menuBlur);
        const bool blurOn = (intensity > 0.001f || menuPx > 0.01f) && !failed && !PERLIN_ENV("PERLIN_NO_BLUR");
        ShaderPack& pack = ShaderPack::Instance();
        const bool sceneWanted = pack.Active() && pack.Flag("sceneCopy", false) && !scene.failed && !PERLIN_ENV("PERLIN_NO_SSR");
        if (plan.wantDepth != depthReadable) {
            if (blur.targets)
                DestroyMotionBlur();
            depthReadable = plan.wantDepth;
        }
        scene.valid = false;
        const bool bloomOn = params.bloom > 0.001f && !bloom.failed && !PERLIN_ENV("PERLIN_NO_BLOOM");
        const int renderW = plan.w, renderH = plan.h;
        const bool scaled = plan.scaled;
        if (blur.targets && (blur.scaled != scaled || (scaled && (blur.w != renderW || blur.h != renderH)))) {
            DestroyMotionBlur();
            upscale.historyValid = false;
        }
        if (!blur.targets) {
            blur.scaled = scaled;
            blur.w = renderW;
            blur.h = renderH;
        }
        const bool on = blurOn || sceneWanted || bloomOn || scaled || plan.temporal || plan.frameGen || exportTarget;
        if (!on || !EnsureMotionBlurTargets()) {
            if (on && !blur.targets) {
                if (blurOn)
                    failed = true;
                if (sceneWanted) {
                    scene.failed = true;
                    std::cout << "[shaders] no offscreen targets; reflections fall back to the sky" << std::endl;
                }
            }
            bgfx::setViewFrameBuffer(0, BGFX_INVALID_HANDLE);
            bgfx::setViewFrameBuffer(kViewEntity, BGFX_INVALID_HANDLE);
            bgfx::setViewFrameBuffer(kViewLate, BGFX_INVALID_HANDLE);
            for (const bgfx::ViewId v : {kViewBlurOut, kViewHand, kViewGui, kViewDoll, kViewTop})
                bgfx::setViewFrameBuffer(v, BGFX_INVALID_HANDLE);
            if (fg.ready)
                DestroyFrameGen();
            fg.valid = false;
            blur.valid = false;
            if (blur.targets && !on)
                DestroyMotionBlur();
            if (!sceneWanted)
                DestroySceneCopy();
            if (bloom.ready)
                DestroyBloom();
            return;
        }
        const bool bloomRun = bloomOn && EnsureBloom();
        if (!bloomOn && bloom.ready)
            DestroyBloom();
        if (bloomRun)
            RunBloom(params.bloom);
        if (sceneWanted && EnsureSceneCopy()) {
            bgfx::TextureRegion dst, src;
            dst.init(scene.color);
            src.init(blur.worldColor);
            bgfx::blit(kViewCopy, dst, src);
            dst.init(scene.depth);
            src.init(blur.worldDepth);
            bgfx::blit(kViewCopy, dst, src);
            bgfx::touch(kViewCopy);
            scene.valid = true;
        }
        bgfx::setViewFrameBuffer(0, blur.worldFb);
        bgfx::setViewFrameBuffer(kViewEntity, blur.worldFb);
        bgfx::setViewFrameBuffer(kViewLate, blur.worldFb);
        for (const bgfx::ViewId view : {bgfx::ViewId(0), kViewEntity, kViewLate})
            bgfx::setViewRect(view, 0, 0, static_cast<uint16_t>(blur.scaled ? blur.w : width), static_cast<uint16_t>(blur.scaled ? blur.h : height));
        const int worldW = blur.scaled ? blur.w : width, worldH = blur.scaled ? blur.h : height;
        const bool fgRun = (plan.frameGen || exportTarget) && EnsureFrameGen();
        if (!fgRun && fg.ready)
            DestroyFrameGen();
        bgfx::FrameBufferHandle screenFb = BGFX_INVALID_HANDLE;
        if (fgRun)
            screenFb = fg.finalFb;
        for (const bgfx::ViewId v : {kViewBlurOut, kViewHand, kViewGui, kViewDoll, kViewTop})
            bgfx::setViewFrameBuffer(v, screenFb);
        fg.valid = fgRun;
        bgfx::setViewClear(kViewBlurOut, fgRun ? BGFX_CLEAR_DEPTH : BGFX_CLEAR_NONE, 0, 1.0f, 0);

        const float now = params.timeSeconds;
        const float dt = std::clamp(now - blur.lastTime, 1.0f / 480.0f, 0.1f);
        blur.lastTime = now;
        float weight = std::pow(0.95f * intensity, dt * 60.0f);
        if (!blur.valid || menuPx > 0.01f)
            weight = 0.0f;
        const int write = 1 - blur.read;
        const float origin = bgfx::getCaps()->originBottomLeft ? 1.0f : 0.0f;

        const auto fullscreen = [&](bgfx::ViewId view, bgfx::FrameBufferHandle fb, bgfx::TextureHandle cur,
                                    bgfx::TextureHandle hist, float w, float dir, bool nativeSize = true) {
            bgfx::setViewFrameBuffer(view, fb);
            bgfx::setViewRect(view, 0, 0, static_cast<uint16_t>(nativeSize ? width : worldW), static_cast<uint16_t>(nativeSize ? height : worldH));
            const float p[4] = {w, origin, menuPx, dir};
            bgfx::setUniform(blur.params, p);
            bgfx::setTexture(0, blur.sampCur, cur, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
            bgfx::setTexture(1, blur.sampHist, hist, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
            const float fx[4] = {view == kViewBlurOut && pack.Active() && params.cameraMedium == 1 ? 1.0f : 0.0f,
                                 std::fmod(params.timeSeconds, 3600.0f), 0.0f, 0.0f};
            bgfx::setUniform(blur.fx, fx);
            if (bloom.ready) {
                const float b[4] = {bloomRun && view == kViewBlurOut ? params.bloom * 0.55f : 0.0f, 0.0f, 0.0f, 0.0f};
                bgfx::setUniform(bloom.composite, b);
                bgfx::setTexture(2, bloom.sampler, bloom.level[0], BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
            }
            bgfx::setVertexBuffer(0, screenBuffer);
            bgfx::setState(BGFX_STATE_WRITE_RGB);
            bgfx::submit(view, shaders.Program(ShaderProgram::Post));
        };
        if (!blurOn) {
            const bgfx::TextureHandle shown = RunUpscale(blur.worldColor, worldW, worldH, params);
            fullscreen(kViewBlurOut, screenFb, shown, shown, 0.0f, 1.0f);
            if (fgRun)
                SubmitFrameGenCopies(origin);
            blur.valid = false;
            return;
        }
        fullscreen(kViewBlur, blur.histFb[write], blur.worldColor, blur.histColor[blur.read], weight, 0.0f, false);
        const bgfx::TextureHandle shown = RunUpscale(blur.histColor[write], worldW, worldH, params);
        fullscreen(kViewBlurOut, screenFb, shown, shown, 0.0f, 1.0f);
        if (fgRun)
            SubmitFrameGenCopies(origin);
        blur.read = write;
        blur.valid = true;
    }

    void RenderContext_bgfx::BeginFrame(const FrameParams& paramsIn) {
        const FrameParams& params = paramsIn;
        int w = 0, h = 0;
        platform->GetFramebufferSize(w, h);
        if (forcedW > 0 && forcedH > 0) {
            w = forcedW;
            h = forcedH;
        }
        if (platform->TakeSurfaceChanged() && w > 0 && h > 0 && initialized) {
            width = w + 1;
            height = h;
            bgfx::SwapChain sc;
            sc.width = static_cast<uint32_t>(w + 1);
            sc.height = static_cast<uint32_t>(h);
            bgfx::reset(ResetFlags(), &sc);
        } else if (w > 0 && h > 0 && (w != width || h != height)) {
            Resize(w, h);
        }

        PlanFrame(params);
        {
            const bool usable = std::isfinite(params.projection[0]) && params.projection[0] != 0.0f && std::isfinite(params.view[12]);
            cam.havePrev = cam.haveCur && usable && !params.teleported && cam.sizeW == width && cam.sizeH == height;
            if (usable) {
                cam.prev2View = cam.prevView;
                cam.havePrev2 = cam.havePrev;
                cam.prevView = cam.view;
                cam.prevProj = cam.proj;
                cam.prevEye = cam.eye;
                cam.view = DoubleMatrix(params.view);
                cam.proj = DoubleMatrix(params.projection);
                cam.eye = glm::dvec3(params.cameraPosition[0], params.cameraPosition[1], params.cameraPosition[2]);
            }
            if (usable) {
                cam.prevTime = cam.time;
                cam.time = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            }
            cam.haveCur = usable;
            cam.teleported = params.teleported;
            cam.sizeW = width;
            cam.sizeH = height;
        }

        auto quantize = [](float c) -> uint32_t {
            return static_cast<uint32_t>(std::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        const uint32_t clear = (quantize(params.fogColor[0]) << 24) | (quantize(params.fogColor[1]) << 16) |
                               (quantize(params.fogColor[2]) << 8) | 0xFF;
        bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, clear, 1.0f, 0);
        bgfx::setViewRect(0, 0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height));
        bgfx::touch(0);

        {
            float handProj[16];
            ConvertDepth(params.handProjection, handProj);
            static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            float worldProj[16];
            ConvertDepth(params.viewProjection, worldProj);
            if (plan.temporal)
                ApplyJitter(worldProj, plan.jitter[0], plan.jitter[1]);
            bgfx::setViewRect(kViewEntity, 0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height));
            bgfx::setViewTransform(kViewEntity, identity, worldProj);
            bgfx::touch(kViewEntity);
            bgfx::setViewRect(kViewLate, 0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height));
            bgfx::setViewTransform(kViewLate, identity, worldProj);
            bgfx::touch(kViewLate);
            bgfx::setViewRect(kViewHand, 0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height));
            bgfx::setViewClear(kViewHand, BGFX_CLEAR_DEPTH, 0, 1.0f, 0);
            bgfx::setViewTransform(kViewHand, identity, handProj);
            bgfx::touch(kViewHand);
            float guiProj[16];
            const glm::mat4 ortho = glm::orthoRH_ZO(0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f,
                                                    -1000.0f, 1000.0f);
            ConvertDepth(&ortho[0][0], guiProj);
            bgfx::setViewRect(kViewGui, 0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height));
            bgfx::setViewTransform(kViewGui, identity, guiProj);
            bgfx::touch(kViewGui);
            bgfx::setViewRect(kViewDoll, 0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height));
            bgfx::setViewClear(kViewDoll, BGFX_CLEAR_DEPTH, 0, 1.0f, 0);
            bgfx::setViewTransform(kViewDoll, identity, guiProj);
            bgfx::touch(kViewDoll);
            bgfx::setViewRect(kViewTop, 0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height));
            bgfx::setViewTransform(kViewTop, identity, guiProj);
            bgfx::touch(kViewTop);
            bgfx::setViewFrameBuffer(kViewExportUi, BGFX_INVALID_HANDLE);
            bgfx::setViewRect(kViewExportUi, 0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height));
            bgfx::setViewTransform(kViewExportUi, identity, guiProj);
            bgfx::touch(kViewExportUi);
        }
        if (shadowConfigDirty || params.shadowQuality != shadowQualityBuilt || params.shadowDistance != shadowDistanceBuilt) {
            shadowConfigDirty = false;
            shadowQualityBuilt = params.shadowQuality;
            shadowDistanceBuilt = params.shadowDistance;
            InitShadows(params.shadowQuality, params.shadowDistance);
        }
        SetupMotionBlur(params);

        std::memcpy(mvp, params.viewProjection, sizeof(mvp));
        if (homogeneousDepth) {
            for (int c = 0; c < 4; c++)
                mvp[c * 4 + 2] = 2.0f * params.viewProjection[c * 4 + 2] - params.viewProjection[c * 4 + 3];
        }
        shaders.SetFrame(params, tileSize);
        shaders.ApplyFrame();
        {
            float cloudMap[4] = {0.0f, 3.96f, 1.0f / 3072.0f, 1.0f / 3072.0f};
            if (cloudTexture.width > 0 && cloudTexture.height > 0) {
                const double widthBlocks = cloudTexture.width * 12.0;
                cloudMap[0] = static_cast<float>(std::fmod(params.cloud.gameTicks * 0.030000001, widthBlocks));
                cloudMap[2] = static_cast<float>(1.0 / widthBlocks);
                cloudMap[3] = static_cast<float>(1.0 / (cloudTexture.height * 12.0));
            }
            shaders.SetCloudMap(cloudMap);
            const glm::mat4 inverse = glm::inverse(glm::make_mat4(mvp));
            const float info[4] = {scene.valid ? 1.0f : 0.0f, homogeneousDepth ? 1.0f : 0.0f,
                                   bgfx::getCaps()->originBottomLeft ? 1.0f : 0.0f, 0.0f};
            shaders.SetScene(info, &inverse[0][0]);
        }
        UploadLightmap(params.lightmap);
        frame = params;
        ConvertDepth(params.skyViewProjection, skyMvp);
        if (plan.temporal) {
            ApplyJitter(mvp, plan.jitter[0], plan.jitter[1]);
            ApplyJitter(skyMvp, plan.jitter[0], plan.jitter[1]);
        }
    }

    void RenderContext_bgfx::ConvertDepth(const float in[16], float out[16]) const {
        std::memcpy(out, in, sizeof(float) * 16);
        if (homogeneousDepth)
            for (int c = 0; c < 4; c++)
                out[c * 4 + 2] = 2.0f * in[c * 4 + 2] - in[c * 4 + 3];
    }

    void RenderContext_bgfx::UploadSkyTextures(const Texture& sun, const Texture& moon, const Texture& clouds) {
        if (!initialized)
            return;
        auto create = [](const Texture& texture, uint64_t flags) {
            if (texture.width <= 0 || texture.height <= 0)
                return bgfx::TextureHandle(BGFX_INVALID_HANDLE);
            return bgfx::createTexture2D(static_cast<uint16_t>(texture.width), static_cast<uint16_t>(texture.height),
                                         false, 1, bgfx::TextureFormat::RGBA8, flags,
                                         bgfx::copy(texture.pixels.data(), static_cast<uint32_t>(texture.pixels.size())));
        };
        const uint64_t clamp = BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_U_CLAMP |
                               BGFX_SAMPLER_V_CLAMP;
        for (bgfx::TextureHandle* handle : {&sunTexture, &moonTexture})
            if (bgfx::isValid(*handle)) {
                bgfx::destroy(*handle);
                *handle = BGFX_INVALID_HANDLE;
            }
        sunTexture = create(sun, clamp);
        moonTexture = create(moon, clamp);
        if (bgfx::isValid(cloudMapTexture))
            bgfx::destroy(cloudMapTexture);
        cloudMapTexture = create(clouds, BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT);
        cloudTexture.width = clouds.width;
        cloudTexture.height = clouds.height;
        cloudTexture.solid.assign(static_cast<size_t>(std::max(0, clouds.width)) * std::max(0, clouds.height), 0);
        if (clouds.pixels.size() >= cloudTexture.solid.size() * 4)
            for (size_t i = 0; i < cloudTexture.solid.size(); i++)
                cloudTexture.solid[i] = clouds.pixels[i * 4 + 3] >= 10 ? 1 : 0;
        cloudBuilt = false;
    }

    void RenderContext_bgfx::UploadTileFlags(const std::vector<uint8_t>& flags) {
        if (!initialized)
            return;
        constexpr int kW = 256, kH = 64;
        std::vector<uint8_t> rgba(static_cast<size_t>(kW) * kH * 4, 0);
        for (size_t i = 0; i < flags.size() && i < static_cast<size_t>(kW) * kH; i++)
            rgba[i * 4] = flags[i];
        if (!bgfx::isValid(tileFlagsTexture))
            tileFlagsTexture = bgfx::createTexture2D(kW, kH, false, 1, bgfx::TextureFormat::RGBA8,
                                                     BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        if (bgfx::isValid(tileFlagsTexture))
            bgfx::updateTexture2D(tileFlagsTexture, 0, 0, 0, 0, kW, kH, bgfx::copy(rgba.data(), static_cast<uint32_t>(rgba.size())));
    }

    void RenderContext_bgfx::UploadCrosshair(const Texture& icons) {
        if (!initialized || icons.width < 16 || icons.height < 16)
            return;
        const int size = std::max(1, icons.width * 15 / 256);
        std::vector<uint8_t> crop((size_t)size * size * 4);
        for (int y = 0; y < size; y++)
            std::memcpy(crop.data() + (size_t)y * size * 4, icons.pixels.data() + (size_t)y * icons.width * 4,
                        (size_t)size * 4);
        if (bgfx::isValid(crosshairTexture))
            bgfx::destroy(crosshairTexture);
        const uint64_t clamp = BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_U_CLAMP |
                               BGFX_SAMPLER_V_CLAMP;
        crosshairTexture = bgfx::createTexture2D((uint16_t)size, (uint16_t)size, false, 1,
                                                 bgfx::TextureFormat::RGBA8, clamp,
                                                 bgfx::copy(crop.data(), (uint32_t)crop.size()));
        crosshairPx = size;
    }

    namespace {
        uint64_t MaterialState(const char* name, uint64_t base);
    }

    void RenderContext_bgfx::DrawCrosshair() {
    }

    void RenderContext_bgfx::DrawHudCursor(float x, float y, float w, float h) {
        if (!bgfx::isValid(crosshairTexture) || width <= 0 || height <= 0 || bgfx::getAvailTransientVertexBuffer(4, textureLayout) < 4)
            return;
        const float W = static_cast<float>(width), H = static_cast<float>(height);
        const auto nx = [&](float px) { return px / W * 2.0f - 1.0f; };
        const auto ny = [&](float py) { return 1.0f - py / H * 2.0f; };
        const float x0 = nx(std::round(x)), x1 = nx(std::round(x + w)), y0 = ny(std::round(y)), y1 = ny(std::round(y + h));
        bgfx::TransientVertexBuffer tvb;
        bgfx::allocTransientVertexBuffer(&tvb, 4, textureLayout);
        auto* v = reinterpret_cast<TextureVertex*>(tvb.data);
        v[0] = {x0, y1, 0.0f, 0.0f, 0.0f};
        v[1] = {x1, y1, 0.0f, 1.0f, 0.0f};
        v[2] = {x1, y0, 0.0f, 1.0f, 1.0f};
        v[3] = {x0, y0, 0.0f, 0.0f, 1.0f};
        static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        shaders.SetMatrix(identity);
        shaders.SetModulator(1.0f, 1.0f, 1.0f, 1.0f);
        shaders.BindSky(crosshairTexture, BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_U_CLAMP |
                                              BGFX_SAMPLER_V_CLAMP);
        bgfx::setVertexBuffer(0, &tvb);
        bgfx::setIndexBuffer(quadIndices);
        bgfx::setState(MaterialState("ui_crosshair", BGFX_STATE_WRITE_RGB) & ~BGFX_STATE_WRITE_A);
        bgfx::submit(kViewGui, shaders.Program(shaders.Has(ShaderProgram::SpriteUi) ? ShaderProgram::SpriteUi : ShaderProgram::Celestial));
    }

    uint32_t RenderContext_bgfx::UiCreateTexture(int w, int h, const uint8_t* rgba, bool linear, bool repeat) {
        if (!initialized || w <= 0 || h <= 0 || !rgba)
            return 0;
        uint64_t flags = repeat ? 0 : (BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        if (!linear)
            flags |= BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT;
        const bgfx::TextureHandle handle = bgfx::createTexture2D(
            static_cast<uint16_t>(w), static_cast<uint16_t>(h), false, 1, bgfx::TextureFormat::RGBA8, flags,
            bgfx::copy(rgba, static_cast<uint32_t>(w) * static_cast<uint32_t>(h) * 4));
        if (!bgfx::isValid(handle))
            return 0;
        for (size_t i = 0; i < uiTextures.size(); i++)
            if (!uiTextures[i].used) {
                uiTextures[i] = {handle, linear, true};
                return static_cast<uint32_t>(i + 1);
            }
        uiTextures.push_back({handle, linear, true});
        return static_cast<uint32_t>(uiTextures.size());
    }

    void RenderContext_bgfx::UiDestroyTexture(uint32_t id) {
        if (id == 0 || id > uiTextures.size() || !uiTextures[id - 1].used)
            return;
        bgfx::destroy(uiTextures[id - 1].handle);
        uiTextures[id - 1] = {};
    }

    RenderContext_bgfx::UiMeshGpu* RenderContext_bgfx::LookupUiMesh(uint32_t id) {
        const uint32_t slot = (id & kUiMeshSlotMask);
        if (slot == 0 || slot > uiMeshes.size())
            return nullptr;
        UiMeshGpu& mesh = uiMeshes[slot - 1];
        if (!mesh.used || mesh.generation != (id >> kUiMeshSlotBits))
            return nullptr;
        return &mesh;
    }

    uint32_t RenderContext_bgfx::StoreUiMesh(const UiMeshGpu& mesh) {
        uint32_t slot;
        if (!freeUiMeshes.empty()) {
            slot = freeUiMeshes.back();
            freeUiMeshes.pop_back();
        } else {
            if (uiMeshes.size() >= kUiMeshSlotMask)
                return 0;
            slot = static_cast<uint32_t>(uiMeshes.size());
            uiMeshes.emplace_back();
        }
        const uint32_t generation = uiMeshes[slot].generation;
        uiMeshes[slot] = mesh;
        uiMeshes[slot].generation = generation;
        uiMeshes[slot].used = true;
        if (!mesh.transient)
            staticUiMeshes++;
        return (generation << kUiMeshSlotBits) | (slot + 1);
    }

    void RenderContext_bgfx::ReleaseUiMeshSlot(uint32_t slot) {
        UiMeshGpu& mesh = uiMeshes[slot];
        if (!mesh.used)
            return;
        if (!mesh.transient) {
            if (bgfx::isValid(mesh.vb))
                bgfx::destroy(mesh.vb);
            if (bgfx::isValid(mesh.ib))
                bgfx::destroy(mesh.ib);
            staticUiMeshes--;
        }
        const uint32_t next = (mesh.generation + 1) & ((1u << (32 - kUiMeshSlotBits)) - 1u);
        mesh = {};
        mesh.generation = next;
        freeUiMeshes.push_back(slot);
    }

    uint32_t RenderContext_bgfx::UiCreateMesh(const UiVertex* vertices, uint32_t vertexCount, const uint32_t* indices,
                                              uint32_t indexCount) {
        if (!initialized || vertexCount == 0 || indexCount == 0 || !vertices || !indices)
            return 0;
        UiMeshGpu mesh;
        mesh.vb = bgfx::createVertexBuffer(bgfx::copy(vertices, vertexCount * sizeof(UiVertex)), uiLayout);
        mesh.ib = bgfx::createIndexBuffer(bgfx::copy(indices, indexCount * sizeof(uint32_t)), BGFX_BUFFER_INDEX32);
        if (!bgfx::isValid(mesh.vb) || !bgfx::isValid(mesh.ib)) {
            if (bgfx::isValid(mesh.vb))
                bgfx::destroy(mesh.vb);
            if (bgfx::isValid(mesh.ib))
                bgfx::destroy(mesh.ib);
            static uint32_t reported = 0;
            if (reported++ % 600 == 0)
                std::cout << "[render] out of GPU buffer handles; skipping a UI mesh (" << staticUiMeshes
                          << " UI meshes alive)" << std::endl;
            return 0;
        }
        mesh.indexCount = indexCount;
        const uint32_t id = StoreUiMesh(mesh);
        if (!id) {
            bgfx::destroy(mesh.vb);
            bgfx::destroy(mesh.ib);
            return 0;
        }
        if (staticUiMeshes >= staticUiMeshWarnAt) {
            std::cout << "[render] " << staticUiMeshes << " UI meshes alive" << std::endl;
            staticUiMeshWarnAt *= 2;
        }
        return id;
    }

    uint32_t RenderContext_bgfx::UiCreateFrameMesh(const UiVertex* vertices, uint32_t vertexCount, const uint32_t* indices,
                                                   uint32_t indexCount) {
        if (!initialized || vertexCount == 0 || indexCount == 0 || !vertices || !indices)
            return 0;
        uint32_t id = 0;
        if (bgfx::getAvailTransientVertexBuffer(vertexCount, uiLayout) < vertexCount ||
            bgfx::getAvailTransientIndexBuffer(indexCount, true) < indexCount) {
            static uint32_t dropped = 0;
            if (dropped++ % 600 == 0)
                std::cout << "[render] transient buffers full; dropping frame meshes (" << dropped << " so far)" << std::endl;
            return 0;
        } else {
            UiMeshGpu mesh;
            bgfx::allocTransientVertexBuffer(&mesh.tvb, vertexCount, uiLayout);
            bgfx::allocTransientIndexBuffer(&mesh.tib, indexCount, true);
            std::memcpy(mesh.tvb.data, vertices, static_cast<size_t>(vertexCount) * sizeof(UiVertex));
            std::memcpy(mesh.tib.data, indices, static_cast<size_t>(indexCount) * sizeof(uint32_t));
            mesh.indexCount = indexCount;
            mesh.transient = true;
            id = StoreUiMesh(mesh);
        }
        if (id)
            frameUiMeshes.push_back(id);
        return id;
    }

    uint32_t RenderContext_bgfx::UiLiveMeshes() const { return staticUiMeshes; }

    void RenderContext_bgfx::UiDestroyMesh(uint32_t id) {
        if (LookupUiMesh(id))
            ReleaseUiMeshSlot((id & kUiMeshSlotMask) - 1);
    }

    void RenderContext_bgfx::ReleaseFrameMeshes() {
        for (uint32_t id : frameUiMeshes)
            UiDestroyMesh(id);
        frameUiMeshes.clear();
    }

    namespace {
        uint64_t BlendFactorState(BlendFactor f) {
            switch (f) {
                case BlendFactor::Zero: return BGFX_STATE_BLEND_ZERO;
                case BlendFactor::One: return BGFX_STATE_BLEND_ONE;
                case BlendFactor::SrcColor: return BGFX_STATE_BLEND_SRC_COLOR;
                case BlendFactor::InvSrcColor: return BGFX_STATE_BLEND_INV_SRC_COLOR;
                case BlendFactor::SrcAlpha: return BGFX_STATE_BLEND_SRC_ALPHA;
                case BlendFactor::InvSrcAlpha: return BGFX_STATE_BLEND_INV_SRC_ALPHA;
                case BlendFactor::DstColor: return BGFX_STATE_BLEND_DST_COLOR;
                case BlendFactor::InvDstColor: return BGFX_STATE_BLEND_INV_DST_COLOR;
                case BlendFactor::DstAlpha: return BGFX_STATE_BLEND_DST_ALPHA;
                case BlendFactor::InvDstAlpha: return BGFX_STATE_BLEND_INV_DST_ALPHA;
                case BlendFactor::SrcAlphaSaturate: return BGFX_STATE_BLEND_SRC_ALPHA_SAT;
            }
            return BGFX_STATE_BLEND_ONE;
        }

        uint64_t MaterialState(const char* name, uint64_t base) {
            const material::Def* def = material::Registry::Instance().Get().Find(name);
            if (!def)
                return base;
            constexpr uint64_t kClear = BGFX_STATE_BLEND_MASK | BGFX_STATE_BLEND_EQUATION_MASK | BGFX_STATE_WRITE_RGB |
                                        BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z;
            uint64_t state = base & ~kClear;
            if (def->ColorWrite())
                state |= BGFX_STATE_WRITE_RGB;
            if (def->AlphaWrite())
                state |= BGFX_STATE_WRITE_A;
            if (def->DepthWrite() && (base & BGFX_STATE_WRITE_Z))
                state |= BGFX_STATE_WRITE_Z;
            if (def->Blending())
                state |= BGFX_STATE_BLEND_FUNC_SEPARATE(
                    BlendFactorState(def->blendSrc), BlendFactorState(def->blendDst),
                    BlendFactorState(def->blendSrc == BlendFactor::SrcAlpha ? BlendFactor::One : def->blendSrc),
                    BlendFactorState(def->blendDst));
            return state;
        }

        uint64_t DepthFuncState(DepthFunc f) {
            switch (f) {
                case DepthFunc::Less: return BGFX_STATE_DEPTH_TEST_LESS;
                case DepthFunc::Equal: return BGFX_STATE_DEPTH_TEST_EQUAL;
                case DepthFunc::Always: return BGFX_STATE_DEPTH_TEST_ALWAYS;
                case DepthFunc::Greater: return BGFX_STATE_DEPTH_TEST_GREATER;
                case DepthFunc::GreaterEqual: return BGFX_STATE_DEPTH_TEST_GEQUAL;
                case DepthFunc::NotEqual: return BGFX_STATE_DEPTH_TEST_NOTEQUAL;
                case DepthFunc::Never: return BGFX_STATE_DEPTH_TEST_NEVER;
                case DepthFunc::LessEqual: break;
            }
            return BGFX_STATE_DEPTH_TEST_LEQUAL;
        }
    }

    void RenderContext_bgfx::UiSubmit(UiLayer layer, const UiDraw& draw) {
        if (!initialized)
            return;
        const UiMeshGpu* found = LookupUiMesh(draw.mesh);
        if (!found || (!found->transient && (!bgfx::isValid(found->vb) || !bgfx::isValid(found->ib))))
            return;
        const UiMeshGpu& mesh = *found;
        bgfx::TextureHandle texture = uiWhite;
        if (draw.texture != 0 && draw.texture <= uiTextures.size() && uiTextures[draw.texture - 1].used)
            texture = uiTextures[draw.texture - 1].handle;
        const bgfx::ViewId view = layer == UiLayer::World ? kViewEntity : layer == UiLayer::WorldLate ? kViewLate : layer == UiLayer::Hand ? kViewHand : layer == UiLayer::Doll ? kViewDoll : layer == UiLayer::Top ? kViewTop : layer == UiLayer::Export ? kViewExportUi : kViewGui;

        float model[16];
        std::memcpy(model, draw.model, sizeof(model));
        const auto bindMesh = [&] {
            bgfx::setTransform(model);
            if (mesh.transient) {
                bgfx::setVertexBuffer(0, &mesh.tvb);
                bgfx::setIndexBuffer(&mesh.tib);
            } else {
                bgfx::setVertexBuffer(0, mesh.vb);
                bgfx::setIndexBuffer(mesh.ib);
            }
            bgfx::setTexture(0, uiSampler, texture);
        };
        const bool glint = draw.glintTexture != 0 && draw.glintTexture <= uiTextures.size() && uiTextures[draw.glintTexture - 1].used;

        if (layer == UiLayer::World && shadow.active && shadow.entities && !glint && draw.depth && draw.depthWrite &&
            draw.blend != 2 && shaders.Has(ShaderProgram::EntityShadow)) {
            bindMesh();
            shaders.SetMatrix(shadow.rasterE);
            shaders.SetShadow(shadow.sampleE, shadow.infoE, true);
            bgfx::setState(BGFX_STATE_WRITE_R | BGFX_STATE_WRITE_G | BGFX_STATE_WRITE_B | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
                           BGFX_STATE_DEPTH_TEST_LESS);
            bgfx::submit(kViewShadowEntity, shaders.Program(ShaderProgram::EntityShadow));
            shaders.SetMatrix(mvp);
        }
        if (draw.shadowOnly)
            return;
        bindMesh();
        const float params[4] = {draw.alphaCutoff, glint ? 1.0f : 0.0f, draw.glintStrength, static_cast<float>(draw.emissive)};
        const bool world = layer == UiLayer::World || layer == UiLayer::WorldLate;
        const bool entityView = world || layer == UiLayer::Hand || layer == UiLayer::Doll;
        const DrawMaterial domain = draw.material == DrawMaterial::Auto
            ? (entityView ? DrawMaterial::Entity : DrawMaterial::Auto) : draw.material;
        const float materialParams[4] = {world && domain != DrawMaterial::Nametag ? 1.0f : 0.0f, 0, 0, 0};
        bgfx::setUniform(rdMaterial, materialParams);
        bgfx::setUniform(uiParams, params);
        bgfx::setUniform(uiOverlay, draw.overlay);
        bgfx::setTexture(1, uiGlintSampler, glint ? uiTextures[draw.glintTexture - 1].handle : uiWhite);
        const float glintA[4] = {draw.glintMatrix[0], draw.glintMatrix[1], draw.glintMatrix[2], draw.glintMatrix[3]};
        const float glintB[4] = {draw.glintOffset[0], draw.glintOffset[1], 0, 0};
        bgfx::setUniform(uiGlintA, glintA);
        bgfx::setUniform(uiGlintB, glintB);
        uint64_t state = 0;
        if (draw.colorWrite)
            state |= BGFX_STATE_WRITE_RGB;
        if (draw.alphaWrite)
            state |= BGFX_STATE_WRITE_A;
        if (glint) {
            state = BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE);
            if (draw.depth)
                state |= BGFX_STATE_DEPTH_TEST_LEQUAL;
        } else if (draw.premultiplied)
            state |= BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA,
                                                    BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);
        else if (draw.customBlend)
            state |= BGFX_STATE_BLEND_FUNC_SEPARATE(BlendFactorState(draw.blendSrc), BlendFactorState(draw.blendDst),
                                                    BlendFactorState(draw.blendSrc == BlendFactor::SrcAlpha ? BlendFactor::One : draw.blendSrc),
                                                    BlendFactorState(draw.blendDst));
        else if (draw.blend == 1)
            state |= BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA,
                                                    BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);
        else if (draw.blend == 2)
            state |= BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_ONE);
        if (draw.depth && !glint) {
            state |= DepthFuncState(draw.depthFunc);
            if (draw.depthWrite)
                state |= BGFX_STATE_WRITE_Z;
        }
        if (draw.cull == CullMode::Back)
            state |= BGFX_STATE_CULL_CCW;
        else if (draw.cull == CullMode::Front)
            state |= BGFX_STATE_CULL_CW;
        if (draw.alphaToCoverage)
            state |= BGFX_STATE_BLEND_ALPHA_TO_COVERAGE;
        bgfx::setState(state);
        if (draw.scissor)
            bgfx::setScissor(static_cast<uint16_t>(std::max(0, draw.scissorRect[0])),
                             static_cast<uint16_t>(std::max(0, draw.scissorRect[1])),
                             static_cast<uint16_t>(std::max(0, draw.scissorRect[2])),
                             static_cast<uint16_t>(std::max(0, draw.scissorRect[3])));
        ShaderProgram program = ShaderProgram::Ui;
        if (glint)
            program = ShaderProgram::EntityGlint;
        else if (domain == DrawMaterial::Particle)
            program = draw.alphaCutoff >= 0.5f ? ShaderProgram::ParticleCutout : ShaderProgram::Particle;
        else if (domain == DrawMaterial::Weather)
            program = ShaderProgram::Weather;
        else if (domain == DrawMaterial::Cracks)
            program = ShaderProgram::Cracks;
        else if (domain == DrawMaterial::Nametag)
            program = draw.texture ? ShaderProgram::Ui : ShaderProgram::Nametag;
        else if (domain == DrawMaterial::Entity)
            program = draw.colorMask ? ShaderProgram::EntityMask
                : draw.emissive == 1 ? ShaderProgram::EntityEmissive : ShaderProgram::Entity;
        bgfx::submit(view, shaders.Program(program));
    }

    void RenderContext_bgfx::DestroyUi() {
        for (size_t i = 0; i < uiMeshes.size(); i++)
            ReleaseUiMeshSlot(static_cast<uint32_t>(i));
        frameUiMeshes.clear();
        staticUiMeshes = 0;
        for (size_t i = 0; i < uiTextures.size(); i++)
            UiDestroyTexture(static_cast<uint32_t>(i + 1));
        uiMeshes.clear();
        freeUiMeshes.clear();
        uiTextures.clear();
        if (bgfx::isValid(uiWhite)) bgfx::destroy(uiWhite);
        if (bgfx::isValid(uiSampler)) bgfx::destroy(uiSampler);
        if (bgfx::isValid(rdMaterial)) bgfx::destroy(rdMaterial);
        rdMaterial = BGFX_INVALID_HANDLE;
        if (bgfx::isValid(uiParams)) bgfx::destroy(uiParams);
        if (bgfx::isValid(uiOverlay)) bgfx::destroy(uiOverlay);
        if (bgfx::isValid(uiGlintSampler)) bgfx::destroy(uiGlintSampler);
        if (bgfx::isValid(uiGlintA)) bgfx::destroy(uiGlintA);
        if (bgfx::isValid(uiGlintB)) bgfx::destroy(uiGlintB);
        uiWhite = BGFX_INVALID_HANDLE;
        uiSampler = BGFX_INVALID_HANDLE;
        uiParams = BGFX_INVALID_HANDLE;
        uiOverlay = BGFX_INVALID_HANDLE;
        uiGlintSampler = uiGlintA = uiGlintB = BGFX_INVALID_HANDLE;
    }

    void RenderContext_bgfx::EndFrame() {
        static const bool memoryStats = PERLIN_ENV("PERLIN_MEMORY_STATS") != nullptr;
        static auto lastMemoryStats = std::chrono::steady_clock::now();
        static const bool gpuTime = PERLIN_ENV("PERLIN_GPUTIME") != nullptr;
        static uint32_t timedFrames = 0;
        static double gpuSum = 0, cpuSum = 0, viewSum[16] = {};
        if (gpuTime && timedFrames == 0)
            bgfx::setDebug(BGFX_DEBUG_PROFILER);
        FinishReadbacks(bgfx::frame());
        ReleaseFrameMeshes();
        if (memoryStats && std::chrono::steady_clock::now() - lastMemoryStats >= std::chrono::seconds(5)) {
            lastMemoryStats = std::chrono::steady_clock::now();
            uint64_t reserved = 0, used = 0;
            for (const Page& p : pages) {
                reserved += uint64_t(p.vAlloc.Capacity()) * sizeof(Vertex);
                used += uint64_t(p.vAlloc.Capacity() - p.vAlloc.FreeTotal()) * sizeof(Vertex);
                for (const auto& a : p.iAlloc) {
                    reserved += uint64_t(a.Capacity()) * sizeof(uint32_t);
                    used += uint64_t(a.Capacity() - a.FreeTotal()) * sizeof(uint32_t);
                }
            }
            const auto* stats = bgfx::getStats();
            std::printf("[memory] chunkMeshes=%zu meshMiB=%.1f reservedMiB=%.1f textures=%u textureMiB=%.1f staticUiMeshes=%u\n",
                        gpuChunks.size(), used / 1048576.0, reserved / 1048576.0, stats->numTextures,
                        stats->textureMemoryUsed / 1048576.0, staticUiMeshes);
            std::fflush(stdout);
        }
        if (gpuTime) {
            const bgfx::Stats* stats = bgfx::getStats();
            const double toMs = 1000.0 / static_cast<double>(stats->gpuTimerFreq ? stats->gpuTimerFreq : 1);
            gpuSum += static_cast<double>(stats->gpuTimeEnd - stats->gpuTimeBegin) * toMs;
            cpuSum += static_cast<double>(stats->cpuTimeEnd - stats->cpuTimeBegin) * 1000.0 / static_cast<double>(stats->cpuTimerFreq ? stats->cpuTimerFreq : 1);
            for (uint16_t i = 0; i < stats->numViews && i < 16; i++)
                viewSum[stats->viewStats[i].view < 16 ? stats->viewStats[i].view : 15] +=
                    static_cast<double>(stats->viewStats[i].gpuTimeEnd - stats->viewStats[i].gpuTimeBegin) * toMs;
            if (++timedFrames >= 120) {
                std::printf("[gpu] ms/frame: gpu=%.3f cpu=%.3f draws=%u", gpuSum / timedFrames, cpuSum / timedFrames, stats->numDraw);
                for (int v = 0; v < 16; v++)
                    if (viewSum[v] > 0.0)
                        std::printf(" v%d=%.3f", v, viewSum[v] / timedFrames);
                std::printf("\n");
                std::fflush(stdout);
                timedFrames = 1;
                gpuSum = cpuSum = 0;
                for (double& v : viewSum)
                    v = 0;
            }
        }
    }

    void RenderContext_bgfx::RequestScreenshot(const std::string& path) {
        if (!initialized)
            return;
        bgfx::requestScreenShot(BGFX_INVALID_HANDLE, path.c_str());
    }

    void RenderContext_bgfx::RequestFrameCapture(CaptureSink sink) {
        if (!initialized || !sink)
            return;
        if (PERLIN_ENV("PERLIN_CAPTURE_DEBUG"))
            std::cout << "[capture] export=" << exportTarget << " target=" << fg.valid
                      << " size=" << width << 'x' << height << std::endl;
        if (exportTarget && fg.valid && bgfx::isValid(fg.finalColor) &&
            bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK)) {
            FrameReadback readback;
            readback.width = static_cast<uint32_t>(width);
            readback.height = static_cast<uint32_t>(height);
            readback.texture = bgfx::createTexture2D(static_cast<uint16_t>(width), static_cast<uint16_t>(height),
                false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
            if (bgfx::isValid(readback.texture)) {
                readback.sink = std::move(sink);
                readback.pixels.resize(static_cast<size_t>(width) * height * 4);
                frameReadbacks.push_back(std::move(readback));
                auto& pending = frameReadbacks.back();
                bgfx::TextureRegion dst, src;
                dst.init(pending.texture);
                src.init(fg.finalColor);
                bgfx::blit(26, dst, src);
                pending.ready = bgfx::read(dst, pending.pixels.data());
                return;
            }
        }
        {
            std::lock_guard<std::mutex> lk(captureMutex);
            captureSinks.push_back(std::move(sink));
        }
        const bool offscreen = exportTarget && fg.valid && bgfx::isValid(fg.finalFb);
        bgfx::FrameBufferHandle target = BGFX_INVALID_HANDLE;
        if (offscreen)
            target = fg.finalFb;
        bgfx::requestScreenShot(target, "perlin:capture");
    }

    bool RenderContext_bgfx::QueryNativeHandles(NativeHandleInfo& out) const {
        if (!initialized)
            return false;
        out.api = rendererName;
        out.adapter = adapterName;
        out.window = nativeWindow;
        out.display = nativeDisplay;
        if (const bgfx::InternalData* data = bgfx::getInternalData())
            out.device = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(data->context));
        return true;
    }

    bool RenderContext_bgfx::SetExportTarget(bool on) {
        if (!on) {
            exportTarget = false;
            return true;
        }
        if (!initialized || PERLIN_ENV("PERLIN_EXPORT_DIRECT"))
            return false;
        if (!EnsureFrameGen())
            return false;
        exportTarget = true;
        return true;
    }

    void RenderContext_bgfx::DeliverCapture(uint32_t w, uint32_t h, uint32_t pitch, const uint8_t* bgra, bool flipY, bool rgba) {
        CaptureSink sink;
        {
            std::lock_guard<std::mutex> lk(captureMutex);
            if (captureSinks.empty())
                return;
            sink = std::move(captureSinks.front());
            captureSinks.pop_front();
        }
        if (rgba && w > 0 && h > 0) {
            std::vector<uint8_t> swapped(static_cast<size_t>(pitch) * h);
            for (uint32_t y = 0; y < h; y++) {
                const uint8_t* src = bgra + static_cast<size_t>(y) * pitch;
                uint8_t* dst = swapped.data() + static_cast<size_t>(y) * pitch;
                for (uint32_t x = 0; x < w; x++) {
                    dst[x * 4 + 0] = src[x * 4 + 2];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + 0];
                    dst[x * 4 + 3] = src[x * 4 + 3];
                }
            }
            sink(w, h, pitch, swapped.data(), flipY);
            return;
        }
        sink(w, h, pitch, bgra, flipY);
    }

    void RenderContext_bgfx::SaveScreenshotData(const std::string& path, uint32_t w, uint32_t h,
                                                uint32_t pitch, const uint8_t* bgra, bool yflip) {
        FILE* f = fopen(path.c_str(), "wb");
        if (!f)
            return;
        uint32_t rowBytes = w * 4;
        uint32_t fileSize = 14 + 40 + rowBytes * h;
        uint8_t fh[14] = {'B', 'M', 0, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0};
        std::memcpy(fh + 2, &fileSize, 4);
        uint8_t ih[40] = {};
        uint32_t biSize = 40;
        std::memcpy(ih + 0, &biSize, 4);
        std::memcpy(ih + 4, &w, 4);
        int32_t negH = yflip ? (int32_t)h : -(int32_t)h;
        std::memcpy(ih + 8, &negH, 4);
        ih[12] = 1;
        ih[14] = 32;
        uint32_t imgSize = rowBytes * h;
        std::memcpy(ih + 20, &imgSize, 4);
        fwrite(fh, 1, 14, f);
        fwrite(ih, 1, 40, f);
        std::vector<uint8_t> row(rowBytes);
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t* src = bgra + (size_t)y * pitch;
            for (uint32_t x = 0; x < w; x++) {
                row[x * 4 + 0] = src[x * 4 + 0];
                row[x * 4 + 1] = src[x * 4 + 1];
                row[x * 4 + 2] = src[x * 4 + 2];
                row[x * 4 + 3] = 255;
            }
            fwrite(row.data(), 1, rowBytes, f);
        }
        fclose(f);
    }

    namespace {
        int MipCount(int size) {
            int n = 1;
            while ((size >> n) > 0)
                n++;
            return n;
        }

        std::vector<std::vector<uint8_t>> BuildTileMips(int kTileSize, const uint8_t* rgba, size_t stride) {
            const int kTileMips = MipCount(kTileSize);
            std::vector<std::vector<uint8_t>> mips;
            mips.reserve(static_cast<size_t>(kTileMips));
            std::vector<uint8_t> level((size_t)kTileSize * kTileSize * 4);
            bool anyClear = false;
            for (int y = 0; y < kTileSize; y++) {
                std::memcpy(level.data() + (size_t)y * kTileSize * 4, rgba + (size_t)y * stride, (size_t)kTileSize * 4);
                for (int x = 0; x < kTileSize; x++)
                    anyClear = anyClear || level[((size_t)y * kTileSize + x) * 4 + 3] == 0;
            }
            if (anyClear) {
                std::vector<uint8_t> bled = level;
                for (int y = 0; y < kTileSize; y++)
                    for (int x = 0; x < kTileSize; x++) {
                        uint8_t* px = bled.data() + ((size_t)y * kTileSize + x) * 4;
                        if (level[((size_t)y * kTileSize + x) * 4 + 3] != 0)
                            continue;
                        unsigned sum[3] = {0, 0, 0}, n = 0;
                        for (int dy = -1; dy <= 1; dy++)
                            for (int dx = -1; dx <= 1; dx++) {
                                const int nx = x + dx, ny = y + dy;
                                if (nx < 0 || ny < 0 || nx >= kTileSize || ny >= kTileSize)
                                    continue;
                                const uint8_t* q = level.data() + ((size_t)ny * kTileSize + nx) * 4;
                                if (!q[3])
                                    continue;
                                sum[0] += q[0];
                                sum[1] += q[1];
                                sum[2] += q[2];
                                n++;
                            }
                        if (n) {
                            px[0] = (uint8_t)(sum[0] / n);
                            px[1] = (uint8_t)(sum[1] / n);
                            px[2] = (uint8_t)(sum[2] / n);
                        }
                    }
                level = std::move(bled);
            }
            for (int l = 0; l < kTileMips; l++) {
                const int w = std::max(1, kTileSize >> l);
                if (l + 1 == kTileMips) {
                    mips.push_back(std::move(level));
                    break;
                }
                const int h = std::max(1, w >> 1);
                std::vector<uint8_t> next((size_t)h * h * 4);
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < h; x++) {
                        const size_t idx[4] = {
                            ((size_t)(y * 2) * w + x * 2) * 4,
                            ((size_t)(y * 2) * w + x * 2 + 1) * 4,
                            ((size_t)(y * 2 + 1) * w + x * 2) * 4,
                            ((size_t)(y * 2 + 1) * w + x * 2 + 1) * 4,
                        };
                        DownsampleAlphaAware(level.data(), idx, next.data() + ((size_t)y * h + x) * 4);
                    }
                mips.push_back(std::move(level));
                level = std::move(next);
            }
            return mips;
        }

        void SubmitTileMips(bgfx::TextureHandle tex, int kTileSize, int layer, const std::vector<std::vector<uint8_t>>& mips) {
            for (size_t l = 0; l < mips.size(); l++) {
                const int w = std::max(1, kTileSize >> static_cast<int>(l));
                bgfx::updateTexture2D(tex, (uint16_t)layer, (uint8_t)l, 0, 0, (uint16_t)w, (uint16_t)w,
                                      bgfx::copy(mips[l].data(), (uint32_t)((size_t)w * w * 4)), (uint16_t)(w * 4));
            }
        }

        void UploadTile(bgfx::TextureHandle tex, int kTileSize, int layer, const uint8_t* rgba, size_t stride) {
            SubmitTileMips(tex, kTileSize, layer, BuildTileMips(kTileSize, rgba, stride));
        }
    }

    int RenderContext_bgfx::MaxAtlasLayers(int tilePx) const {
        const int limit = initialized ? std::max<int>(1, bgfx::getCaps()->limits.maxTextureLayers) : 2048;
        const double perLayer = static_cast<double>(std::max(1, tilePx)) * std::max(1, tilePx) * 4.0 * 4.0 / 3.0;
        const double budget = 1.3e9;
        return std::max(256, std::min(limit, static_cast<int>(budget / perLayer)));
    }

    void RenderContext_bgfx::UploadAtlas(const uint8_t* rgba, int tilePx, int tiles) {
        if (!initialized || !rgba || tilePx <= 0 || tiles <= 0)
            return;
        animatedTiles.clear();
        const int limit = std::max<int>(1, bgfx::getCaps()->limits.maxTextureLayers);
        const int layers = std::min(limit, (tiles + 255) / 256 * 256);
        bgfx::TextureHandle tex = bgfx::createTexture2D((uint16_t)tilePx, (uint16_t)tilePx, true, (uint16_t)layers,
                                                        bgfx::TextureFormat::RGBA8, samplerFlags, nullptr);
        if (!bgfx::isValid(tex)) {
            std::cout << "[bgfx] atlas upload failed (" << layers << " layers of " << tilePx << "px)" << std::endl;
            return;
        }
        const size_t stride = (size_t)tilePx * 4;
        const size_t tileBytes = (size_t)tilePx * tilePx * 4;
        const int count = std::min(tiles, layers);
        for (int tile = 0; tile < count; tile++)
            UploadTile(tex, tilePx, tile, rgba + tileBytes * (size_t)tile, stride);
        if (bgfx::isValid(atlasTex))
            bgfx::destroy(atlasTex);
        atlasTex = tex;
        atlasLayers = layers;
        tileSize = tilePx;
    }

    void RenderContext_bgfx::UpdateAtlasRegion(int tile, const uint8_t* rgba) {
        if (!initialized || !bgfx::isValid(atlasTex) || !rgba || tile < 0 || tile >= atlasLayers)
            return;
        auto it = animatedTiles.find(rgba);
        if (it == animatedTiles.end()) {
            if (animatedTiles.size() > 4096)
                animatedTiles.clear();
            it = animatedTiles.emplace(rgba, BuildTileMips(tileSize, rgba, (size_t)tileSize * 4)).first;
        }
        SubmitTileMips(atlasTex, tileSize, tile, it->second);
    }

    bool RenderContext_bgfx::CreatePage(uint32_t vCap, const std::array<uint32_t, kRenderLayerCount>& iCap) {
        Page p;
        for (auto& h : p.ib)
            h = BGFX_INVALID_HANDLE;
        p.vb = bgfx::createDynamicVertexBuffer(vCap, layout, BGFX_BUFFER_NONE);
        bool ok = bgfx::isValid(p.vb);
        for (int l = 0; l < kRenderLayerCount && ok; l++) {
            p.ib[l] = bgfx::createDynamicIndexBuffer(iCap[l], BGFX_BUFFER_INDEX32);
            ok = bgfx::isValid(p.ib[l]);
        }
        if (!ok) {
            if (bgfx::isValid(p.vb))
                bgfx::destroy(p.vb);
            for (auto& h : p.ib)
                if (bgfx::isValid(h))
                    bgfx::destroy(h);
            return false;
        }
        p.vAlloc.Reset(vCap);
        for (int l = 0; l < kRenderLayerCount; l++)
            p.iAlloc[l].Reset(iCap[l]);
        pages.push_back(std::move(p));
        return true;
    }

    bool RenderContext_bgfx::AllocMesh(uint32_t v, const std::array<uint32_t, kRenderLayerCount>& need, GpuChunk& out) {
        auto tryPage = [&](Page& pg, int index) {
            if (!bgfx::isValid(pg.vb))
                return false;
            pg.emptyFrames = 0;
            uint32_t vo = 0;
            if (!pg.vAlloc.Alloc(v, vo))
                return false;
            std::array<uint32_t, kRenderLayerCount> io{};
            for (int l = 0; l < kRenderLayerCount; l++) {
                if (need[l] && !pg.iAlloc[l].Alloc(need[l], io[l])) {
                    for (int k = 0; k < l; k++)
                        if (need[k])
                            pg.iAlloc[k].Free(io[k], need[k]);
                    pg.vAlloc.Free(vo, v);
                    return false;
                }
            }
            out.page = index;
            out.vOff = vo;
            out.iOff = io;
            return true;
        };
        for (size_t pi = 0; pi < pages.size(); pi++)
            if (tryPage(pages[pi], (int)pi))
                return true;
        const uint32_t pageVerts = std::min<uint32_t>(kPageVerts, (1u << 18) << std::min<size_t>(pages.size(), 2));
        const uint32_t kBase = (pageVerts / 4) * 6;
        std::array<uint32_t, kRenderLayerCount> caps = {kBase, kBase / 2, kBase / 2, kBase / 4, kBase / 8};
        for (int l = 0; l < kRenderLayerCount; l++)
            caps[l] = std::max(caps[l], need[l]);
        if (!CreatePage(std::max(pageVerts, v), caps))
            return false;
        return tryPage(pages.back(), (int)pages.size() - 1);
    }

    void RenderContext_bgfx::ReleaseEmptyPages() {
        for (Page& p : pages) {
            if (!bgfx::isValid(p.vb))
                continue;
            bool unused = p.vAlloc.Unused();
            for (int l = 0; l < kRenderLayerCount && unused; l++)
                unused = p.iAlloc[l].Unused() || p.iAlloc[l].Capacity() == 0;
            if (!unused) {
                p.emptyFrames = 0;
                continue;
            }
            if (++p.emptyFrames < 600)
                continue;
            bgfx::destroy(p.vb);
            p.vb = BGFX_INVALID_HANDLE;
            for (auto& h : p.ib) {
                if (bgfx::isValid(h))
                    bgfx::destroy(h);
                h = BGFX_INVALID_HANDLE;
            }
            p.vAlloc.Reset(0);
            for (auto& a : p.iAlloc)
                a.Reset(0);
        }
    }

    void RenderContext_bgfx::FreeChunk(GpuChunk& g) {
        if (g.page >= 0 && g.page < (int)pages.size()) {
            pages[g.page].vAlloc.Free(g.vOff, g.vCount);
            for (int l = 0; l < kRenderLayerCount; l++)
                if (g.layerCount[l])
                    pages[g.page].iAlloc[l].Free(g.iOff[l], g.layerCount[l]);
        }
        g.page = -1;
    }

    void RenderContext_bgfx::DestroyPages() {
        for (Page& p : pages) {
            if (bgfx::isValid(p.vb))
                bgfx::destroy(p.vb);
            for (auto& h : p.ib)
                if (bgfx::isValid(h))
                    bgfx::destroy(h);
        }
        pages.clear();
    }

    void RenderContext_bgfx::UpdateChunkMesh(int cx, int cz, const Tessellator& tess, uint64_t version) {
        UploadMesh(gpuChunks, cx, cz, 16, tess, version);
    }

    void RenderContext_bgfx::UpdateLodTile(int tx, int tz, int size, const Tessellator& tess, uint64_t version) {
        UploadMesh(lodChunks, tx, tz, size, tess, version);
    }

    void RenderContext_bgfx::RemoveLodTile(int tx, int tz) {
        auto it = lodChunks.find(ChunkKey(tx, tz));
        if (it == lodChunks.end())
            return;
        FreeChunk(it->second);
        lodChunks.erase(it);
    }

    void RenderContext_bgfx::UploadMesh(std::unordered_map<int64_t, GpuChunk>& gpuChunks, int cx, int cz, int size,
                                        const Tessellator& tess, uint64_t version) {
        const bool lod = &gpuChunks == &lodChunks;
        int64_t key = ChunkKey(cx, cz);
        const auto& verts = tess.Vertices();
        auto it = gpuChunks.find(key);
        if (verts.empty() || tess.Empty()) {
            if (it != gpuChunks.end()) {
                FreeChunk(it->second);
                gpuChunks.erase(it);
                if (!lod)
                    meshGeneration++;
            }
            return;
        }
        if (it != gpuChunks.end() && it->second.version == version)
            return;
        if (it != gpuChunks.end()) {
            FreeChunk(it->second);
            gpuChunks.erase(it);
        }

        GpuChunk g;
        g.vCount = (uint32_t)verts.size();
        for (int l = 0; l < kRenderLayerCount; l++)
            g.layerCount[l] = (uint32_t)tess.Indices(static_cast<RenderLayer>(l)).size();
        if (!AllocMesh(g.vCount, g.layerCount, g))
            return;
        Page& pg = pages[g.page];

        bgfx::update(pg.vb, g.vOff, bgfx::copy(verts.data(), g.vCount * (uint32_t)sizeof(Vertex)));
        for (int l = 0; l < kRenderLayerCount; l++) {
            const auto& list = tess.Indices(static_cast<RenderLayer>(l));
            if (list.empty())
                continue;
            const bgfx::Memory* im = bgfx::alloc((uint32_t)(list.size() * sizeof(uint32_t)));
            uint32_t* dst = reinterpret_cast<uint32_t*>(im->data);
            for (size_t i = 0; i < list.size(); i++)
                dst[i] = list[i] + g.vOff;
            bgfx::update(pg.ib[l], g.iOff[l], im);
        }

        float lo = verts[0].y, hi = verts[0].y;
        for (const Vertex& v : verts) {
            lo = std::min(lo, v.y);
            hi = std::max(hi, v.y);
        }
        g.minY = lo;
        g.maxY = hi;

        const auto& secs = tess.Sections();
        if (secs.empty()) {
            GpuSection whole{lo, hi, 0, {}, {}};
            for (int l = 0; l < kRenderLayerCount; l++) {
                whole.start[l] = 0;
                whole.count[l] = g.layerCount[l];
            }
            g.sections.push_back(whole);
        } else {
            g.sections.reserve(secs.size());
            for (const MeshSection& ms : secs) {
                GpuSection section{(float)(ms.sy * 16), (float)(ms.sy * 16 + 16), ms.sy, {}, {}};
                for (int l = 0; l < kRenderLayerCount; l++) {
                    section.start[l] = ms.start[l];
                    section.count[l] = ms.count[l];
                }
                g.sections.push_back(section);
            }
        }
        g.cx = cx;
        g.cz = cz;
        g.size = size;
        g.version = version;
        {
            const auto& vis = tess.Visibility();
            if (!vis.empty() && vis.size() <= 32) {
                g.visMin = vis.front().sy;
                for (const SectionVis& v : vis)
                    g.conn.push_back(v.conn);
            }
        }
        if (!lod)
            meshGeneration++;
        gpuChunks[key] = std::move(g);
    }

    void RenderContext_bgfx::RemoveChunkMesh(int cx, int cz) {
        auto it = gpuChunks.find(ChunkKey(cx, cz));
        if (it == gpuChunks.end())
            return;
        FreeChunk(it->second);
        gpuChunks.erase(it);
        meshGeneration++;
    }

    namespace {
        glm::mat4 FromArray(const float m[16]) {
            glm::mat4 out;
            std::memcpy(&out[0][0], m, sizeof(float) * 16);
            return out;
        }

        constexpr uint64_t kTranslucentBlend =
            BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA,
                                           BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);
    }

    void RenderContext_bgfx::DrawSky() {
        const SkyFrame& sky = frame.sky;
        shaders.ApplyFrame();
        if (!sky.enabled)
            return;

        const glm::mat4 view = FromArray(frame.skyViewProjection);
        float converted[16];
        auto submitMatrix = [&](const glm::mat4& m) {
            ConvertDepth(&m[0][0], converted);
            shaders.SetMatrix(converted);
        };
        constexpr float kHalfPi = 1.57079632679489661923f;
        const uint64_t write = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;

        submitMatrix(view);
        shaders.SetModulator(sky.skyColor[0], sky.skyColor[1], sky.skyColor[2], 1.0f);
        shaders.BindClouds(cloudMapTexture);
        ShaderPack& pack = ShaderPack::Instance();
        if (pack.Active() && pack.Flag("skyBox", false) && bgfx::isValid(skyBoxBuffer)) {
            bgfx::setVertexBuffer(0, skyBoxBuffer);
            bgfx::setIndexBuffer(skyBoxIndices);
        } else {
            bgfx::setVertexBuffer(0, topSkyBuffer);
            bgfx::setIndexBuffer(skyFanIndices, 0, skyFanIndexCount);
        }
        bgfx::setState(write);
        bgfx::submit(0, shaders.Program(ShaderProgram::Sky));

        if (sky.sunrise[3] > 0.001f) {
            glm::mat4 pose = glm::rotate(view, kHalfPi, glm::vec3(1.0f, 0.0f, 0.0f));
            const float flip = std::sin(sky.sunAngle) < 0.0f ? 3.14159265358979323846f : 0.0f;
            pose = glm::rotate(pose, flip + kHalfPi, glm::vec3(0.0f, 0.0f, 1.0f));
            pose = glm::scale(pose, glm::vec3(1.0f, 1.0f, sky.sunrise[3]));
            submitMatrix(pose);
            shaders.SetModulator(sky.sunrise[0], sky.sunrise[1], sky.sunrise[2], sky.sunrise[3]);
            bgfx::setVertexBuffer(0, sunriseBuffer);
            bgfx::setIndexBuffer(sunriseIndices, 0, sunriseIndexCount);
            bgfx::setState(write | kTranslucentBlend);
            bgfx::submit(0, shaders.Program(ShaderProgram::Sunrise));
        }

        const glm::mat4 celestial = glm::rotate(view, -kHalfPi, glm::vec3(0.0f, 1.0f, 0.0f));
        const uint64_t spriteFlags = BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_U_CLAMP |
                                     BGFX_SAMPLER_V_CLAMP;
        auto body = [&](float angle, float scale) {
            glm::mat4 pose = glm::rotate(celestial, angle, glm::vec3(1.0f, 0.0f, 0.0f));
            pose = glm::translate(pose, glm::vec3(0.0f, 100.0f, 0.0f));
            return glm::scale(pose, glm::vec3(scale, 1.0f, scale));
        };

        if (bgfx::isValid(sunTexture)) {
            submitMatrix(body(sky.sunAngle, 30.0f));
            shaders.SetModulator(1.0f, 1.0f, 1.0f, sky.rainBrightness);
            shaders.BindSky(sunTexture, spriteFlags);
            bgfx::setVertexBuffer(0, sunBuffer);
            bgfx::setIndexBuffer(quadIndices);
            bgfx::setState(MaterialState("sun_moon", write));
            bgfx::submit(0, shaders.Program(ShaderProgram::Celestial));
        }
        if (bgfx::isValid(moonTexture)) {
            submitMatrix(body(sky.moonAngle, 20.0f));
            shaders.SetModulator(1.0f, 1.0f, 1.0f, sky.rainBrightness);
            shaders.BindSky(moonTexture, spriteFlags);
            bgfx::setVertexBuffer(0, moonBuffer, static_cast<uint32_t>(sky.moonPhase) * 4, 4);
            bgfx::setIndexBuffer(quadIndices);
            bgfx::setState(MaterialState("sun_moon", write));
            bgfx::submit(0, shaders.Program(ShaderProgram::Celestial));
        }
        if (sky.starBrightness > 0.0f) {
            submitMatrix(glm::rotate(celestial, sky.starAngle, glm::vec3(1.0f, 0.0f, 0.0f)));
            shaders.SetModulator(sky.starBrightness, sky.starBrightness, sky.starBrightness, sky.starBrightness);
            bgfx::setVertexBuffer(0, starBuffer);
            bgfx::setIndexBuffer(starIndices, 0, starIndexCount);
            bgfx::setState(MaterialState("stars", write));
            bgfx::submit(0, shaders.Program(ShaderProgram::Star));
        }
        if (sky.darkDisc) {
            submitMatrix(glm::translate(view, glm::vec3(0.0f, 12.0f, 0.0f)));
            shaders.SetModulator(0.0f, 0.0f, 0.0f, 1.0f);
            bgfx::setVertexBuffer(0, bottomSkyBuffer);
            bgfx::setIndexBuffer(skyFanIndices, 0, skyFanIndexCount);
            bgfx::setState(write);
            bgfx::submit(0, shaders.Program(ShaderProgram::Sky));
        }
    }

    void RenderContext_bgfx::DrawClouds() {
        if (!ShaderPack::Instance().Flag("vanillaClouds", true))
            return;
        const CloudDraw& clouds = frame.cloud;
        material::Registry::Instance().Configure(clouds.fancy);
        if (!clouds.enabled || cloudTexture.solid.empty())
            return;

        const CloudFrame now = ComputeCloudFrame(frame.cameraPosition[0], frame.cameraPosition[1],
                                                 frame.cameraPosition[2], clouds.height, clouds.gameTicks, cloudTexture);
        const auto upload = [&](const CloudMesh& mesh, const CloudFrame& builtFor, bool fancy, int range) {
            const uint32_t vertexCount = static_cast<uint32_t>(mesh.vertices.size());
            const uint32_t indexCount = static_cast<uint32_t>(mesh.indices.size());
            if (vertexCount > cloudVertexCapacity) {
                if (bgfx::isValid(cloudVertexBuffer))
                    bgfx::destroy(cloudVertexBuffer);
                cloudVertexCapacity = vertexCount + vertexCount / 4;
                cloudVertexBuffer = bgfx::createDynamicVertexBuffer(cloudVertexCapacity, textureLayout);
            }
            if (indexCount > cloudIndexCapacity) {
                if (bgfx::isValid(cloudIndexBuffer))
                    bgfx::destroy(cloudIndexBuffer);
                cloudIndexCapacity = indexCount + indexCount / 4;
                cloudIndexBuffer = bgfx::createDynamicIndexBuffer(cloudIndexCapacity, BGFX_BUFFER_INDEX32);
            }
            if (vertexCount && indexCount) {
                bgfx::update(cloudVertexBuffer, 0, bgfx::copy(mesh.vertices.data(), vertexCount * sizeof(TextureVertex)));
                bgfx::update(cloudIndexBuffer, 0, bgfx::copy(mesh.indices.data(), indexCount * sizeof(uint32_t)));
            }
            cloudIndexCount = vertexCount && indexCount ? indexCount : 0;
            cloudBuiltFrame = builtFor;
            cloudBuiltFancy = fancy;
            cloudBuiltRange = range;
            cloudBuilt = true;
        };
        if (cloudJobRunning && cloudJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const CloudMesh mesh = cloudJob.get();
            cloudJobRunning = false;
            upload(mesh, cloudJobFrame, clouds.fancy, clouds.rangeChunks);
        }
        const bool stale = !cloudBuilt || now.position != cloudBuiltFrame.position || clouds.fancy != cloudBuiltFancy ||
                           clouds.rangeChunks != cloudBuiltRange;
        const bool moved = now.cellX != cloudBuiltFrame.cellX || now.cellZ != cloudBuiltFrame.cellZ;
        if (stale) {
            if (cloudJobRunning) {
                cloudJob.wait();
                cloudJobRunning = false;
            }
            upload(BuildCloudMesh(cloudTexture, now, clouds.fancy, clouds.rangeChunks), now, clouds.fancy, clouds.rangeChunks);
        } else if (moved && !cloudJobRunning) {
            cloudJobFrame = now;
            cloudJob = std::async(std::launch::async, [tex = cloudTexture, now, fancy = clouds.fancy, range = clouds.rangeChunks] {
                return BuildCloudMesh(tex, now, fancy, range);
            });
            cloudJobRunning = true;
        }
        if (!cloudIndexCount || !bgfx::isValid(cloudVertexBuffer) || !bgfx::isValid(cloudIndexBuffer))
            return;

        const auto wrapDelta = [](int d, int w) {
            if (w <= 0)
                return d;
            d %= w;
            if (d > w / 2)
                d -= w;
            else if (d < -w / 2)
                d += w;
            return d;
        };
        const float cellDx = static_cast<float>(wrapDelta(now.cellX - cloudBuiltFrame.cellX, cloudTexture.width)) * kCloudCellSize;
        const float cellDz = static_cast<float>(wrapDelta(now.cellZ - cloudBuiltFrame.cellZ, cloudTexture.height)) * kCloudCellSize;
        const float offset[3] = {-(now.xInCell + cellDx), now.relativeBottomY, -(now.zInCell + cellDz)};
        shaders.SetCloud(offset, clouds.color);
        shaders.SetMatrix(skyMvp);
        bgfx::setVertexBuffer(0, cloudVertexBuffer);
        bgfx::setIndexBuffer(cloudIndexBuffer, 0, cloudIndexCount);
        uint64_t cull = 0;
        if (clouds.fancy)
            cull = cullState == BGFX_STATE_CULL_CCW ? BGFX_STATE_CULL_CW
                                                    : (cullState == BGFX_STATE_CULL_CW ? BGFX_STATE_CULL_CCW : 0);
        bgfx::setState(MaterialState("clouds", BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LEQUAL) | cull);
        bgfx::submit(kViewLate, shaders.Program(ShaderProgram::Cloud));
    }

    void RenderContext_bgfx::UpdateCaveVisibility(float camX, float camY, float camZ) {
        const int ccx = (int)std::floor(camX / 16.0f), ccz = (int)std::floor(camZ / 16.0f);
        int csy = (int)std::floor(camY / 16.0f);
        const bool camMoved = caveCam[0] != ccx || caveCam[1] != csy || caveCam[2] != ccz;
        if (!camMoved) {
            if (caveGeneration == meshGeneration)
                return;
            const double now = frame.timeSeconds;
            if (now - caveLastRun < 0.15)
                return;
        }
        caveLastRun = frame.timeSeconds;

        int minX = INT_MAX, maxX = INT_MIN, minZ = INT_MAX, maxZ = INT_MIN, minS = INT_MAX, maxS = INT_MIN;
        bool allData = true;
        for (auto& entry : gpuChunks) {
            GpuChunk& g = entry.second;
            g.secVis = ~0u;
            if (g.conn.empty()) {
                allData = false;
                continue;
            }
            minX = std::min(minX, g.cx);
            maxX = std::max(maxX, g.cx);
            minZ = std::min(minZ, g.cz);
            maxZ = std::max(maxZ, g.cz);
            minS = std::min(minS, g.visMin);
            maxS = std::max(maxS, g.visMin + (int)g.conn.size() - 1);
        }
        caveGeneration = meshGeneration;
        caveCam[0] = ccx;
        caveCam[1] = csy;
        caveCam[2] = ccz;
        if (!allData || minX > maxX || minS > maxS)
            return;
        minS = std::min(minS, csy);
        maxS = std::max(maxS, csy);
        if (ccx < minX || ccx > maxX || ccz < minZ || ccz > maxZ)
            return;
        const int W = maxX - minX + 1, D = maxZ - minZ + 1, H = maxS - minS + 1;
        if (H > 32)
            return;

        static std::vector<GpuChunk*> grid;
        static std::vector<uint8_t> entered;
        static std::vector<uint32_t> reached;
        grid.assign((size_t)W * D, nullptr);
        entered.assign((size_t)W * D * H, 0);
        reached.assign((size_t)W * D, 0);
        for (auto& entry : gpuChunks)
            grid[(size_t)(entry.second.cz - minZ) * W + (entry.second.cx - minX)] = &entry.second;

        static const int kDx[6] = {0, 0, 0, 0, -1, 1};
        static const int kDy[6] = {-1, 1, 0, 0, 0, 0};
        static const int kDz[6] = {0, 0, -1, 1, 0, 0};

        auto connected = [&](int x, int z, int s, int a, int b) {
            if (a == b)
                return false;
            if (a > b)
                std::swap(a, b);
            const GpuChunk* g = grid[(size_t)z * W + x];
            if (!g)
                return true;
            const int i = (s + minS) - g->visMin;
            if (i < 0 || i >= (int)g->conn.size())
                return true;
            return ((g->conn[i] >> (a * (11 - a) / 2 + (b - a - 1))) & 1) != 0;
        };

        const int sx = ccx - minX, sz = ccz - minZ, ss = csy - minS;
        reached[(size_t)sz * W + sx] |= 1u << ss;
        struct Move {
            int x, z, s;
            uint8_t dir, dirs;
        };
        static std::vector<Move> moves;
        moves.clear();
        for (int d = 0; d < 6; d++)
            moves.push_back({sx, sz, ss, (uint8_t)d, (uint8_t)(1u << d)});
        size_t head = 0;
        while (head < moves.size()) {
            const Move m = moves[head++];
            const int nx = m.x + kDx[m.dir], nz = m.z + kDz[m.dir], ns = m.s + kDy[m.dir];
            if (nx < 0 || nx >= W || nz < 0 || nz >= D || ns < 0 || ns >= H)
                continue;
            const int entry = m.dir ^ 1;
            uint8_t& done = entered[((size_t)nz * W + nx) * H + ns];
            if (done & (1u << entry))
                continue;
            done |= (uint8_t)(1u << entry);
            reached[(size_t)nz * W + nx] |= 1u << ns;
            for (int d = 0; d < 6; d++) {
                if (m.dirs & (1u << (d ^ 1)))
                    continue;
                if (!connected(nx, nz, ns, entry, d))
                    continue;
                moves.push_back({nx, nz, ns, (uint8_t)d, (uint8_t)(m.dirs | (1u << d))});
            }
        }
        for (int dz = -1; dz <= 1; dz++)
            for (int dx = -1; dx <= 1; dx++) {
                const int x = sx + dx, z = sz + dz;
                if (x < 0 || x >= W || z < 0 || z >= D)
                    continue;
                for (int ds = -1; ds <= 1; ds++)
                    if (ss + ds >= 0 && ss + ds < H)
                        reached[(size_t)z * W + x] |= 1u << (ss + ds);
            }
        for (auto& entry : gpuChunks) {
            GpuChunk& g = entry.second;
            const uint32_t col = reached[(size_t)(g.cz - minZ) * W + (g.cx - minX)];
            const int shift = g.visMin - minS;
            g.secVis = shift >= 0 ? (col >> shift) : (col << -shift);
        }
    }

    void RenderContext_bgfx::InitShadows(int quality, int distance) {
        DestroyShadows();
        ShaderPack& pack = ShaderPack::Instance();
        if (!pack.Active() || !pack.Feature("shadows") || quality <= 0 || PERLIN_ENV("PERLIN_NO_SHADOWS"))
            return;
        if (!shaders.Has(ShaderProgram::Shadow) || !shaders.Has(ShaderProgram::ShadowCutout)) {
            std::cout << "[shadows] the pack asks for shadows but has no shadow shaders for this backend" << std::endl;
            return;
        }
        const bgfx::Caps* caps = bgfx::getCaps();
        if (!(caps->formats[bgfx::TextureFormat::R32F] & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER)) {
            std::cout << "[shadows] float render targets are not supported; shadows off" << std::endl;
            return;
        }
        static constexpr int kSizes[5] = {0, 1024, 2048, 3072, 4096};
        int size = kSizes[std::clamp(quality, 1, 4)];
        if (caps->limits.maxTextureSize > 0)
            size = std::min<int>(size, static_cast<int>(caps->limits.maxTextureSize));
        shadow.size = size;
        shadow.distance = distance > 0 ? static_cast<float>(distance) : static_cast<float>(pack.Number("shadowDistance", 80));
        shadow.distance = std::clamp(shadow.distance, 24.0f, 256.0f);
        const uint64_t rt = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_MIN_POINT |
                            BGFX_SAMPLER_MAG_POINT;
        const uint16_t s16 = static_cast<uint16_t>(size);
        shadow.buildDepth = bgfx::createTexture2D(s16, s16, false, 1, bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT_WRITE_ONLY);
        bool ok = bgfx::isValid(shadow.buildDepth);
        for (ShadowMap& m : shadow.maps) {
            if (!ok)
                break;
            m.color = bgfx::createTexture2D(s16, s16, false, 1, bgfx::TextureFormat::R32F, rt);
            bgfx::TextureHandle attachments[2] = {m.color, shadow.buildDepth};
            m.fb = bgfx::createFrameBuffer(2, attachments, false);
            ok = bgfx::isValid(m.color) && bgfx::isValid(m.fb);
        }
        if (!ok) {
            DestroyShadows();
            return;
        }
        if (shaders.Has(ShaderProgram::ShadowGlass) && (caps->formats[bgfx::TextureFormat::RGBA16F] & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER)) {
            const uint16_t h16 = static_cast<uint16_t>(std::max(1, size / 2));
            bool tintOk = true;
            for (ShadowMap& m : shadow.maps) {
                m.tint = bgfx::createTexture2D(h16, h16, false, 1, bgfx::TextureFormat::RGBA16F, rt);
                if (bgfx::isValid(m.tint))
                    m.tintFb = bgfx::createFrameBuffer(1, &m.tint, false);
                tintOk = tintOk && bgfx::isValid(m.tint) && bgfx::isValid(m.tintFb);
            }
            if (!tintOk)
                for (ShadowMap& m : shadow.maps) {
                    if (bgfx::isValid(m.tintFb))
                        bgfx::destroy(m.tintFb);
                    if (bgfx::isValid(m.tint))
                        bgfx::destroy(m.tint);
                    m.tintFb = BGFX_INVALID_HANDLE;
                    m.tint = BGFX_INVALID_HANDLE;
                }
            shadow.tintEnabled = tintOk;
        }
        shadow.enabled = true;
        std::cout << "[shadows] " << size << "x" << size << " map, " << shadow.distance << " blocks"
                  << (shadow.tintEnabled ? ", coloured light through glass" : "") << std::endl;
    }

    void RenderContext_bgfx::DestroyShadows() {
        for (ShadowMap& m : shadow.maps) {
            if (bgfx::isValid(m.fb))
                bgfx::destroy(m.fb);
            if (bgfx::isValid(m.color))
                bgfx::destroy(m.color);
            if (bgfx::isValid(m.tintFb))
                bgfx::destroy(m.tintFb);
            if (bgfx::isValid(m.tint))
                bgfx::destroy(m.tint);
        }
        if (bgfx::isValid(shadow.buildDepth))
            bgfx::destroy(shadow.buildDepth);
        if (bgfx::isValid(shadow.entityFb))
            bgfx::destroy(shadow.entityFb);
        if (bgfx::isValid(shadow.entityColor))
            bgfx::destroy(shadow.entityColor);
        if (bgfx::isValid(shadow.entityDepth))
            bgfx::destroy(shadow.entityDepth);
        shadow = ShadowState{};
        bgfx::setViewClear(kViewShadowBuild, BGFX_CLEAR_NONE);
        bgfx::setViewClear(kViewShadowEntity, BGFX_CLEAR_NONE);
        bgfx::setViewClear(kViewShadowGlass, BGFX_CLEAR_NONE);
    }

    bool RenderContext_bgfx::EnsureEntityShadowMap() {
        if (bgfx::isValid(shadow.entityFb))
            return true;
        if (shadow.entityFailed)
            return false;
        const uint16_t s16 = static_cast<uint16_t>(shadow.size);
        shadow.entityColor = bgfx::createTexture2D(s16, s16, false, 1, bgfx::TextureFormat::R32F,
                                                   BGFX_TEXTURE_RT | BGFX_TEXTURE_BLIT_DST | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP |
                                                       BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT);
        shadow.entityDepth = bgfx::createTexture2D(s16, s16, false, 1, bgfx::TextureFormat::D16, BGFX_TEXTURE_RT_WRITE_ONLY);
        if (bgfx::isValid(shadow.entityColor) && bgfx::isValid(shadow.entityDepth)) {
            bgfx::TextureHandle attachments[2] = {shadow.entityColor, shadow.entityDepth};
            shadow.entityFb = bgfx::createFrameBuffer(2, attachments, false);
        }
        if (!bgfx::isValid(shadow.entityFb)) {
            shadow.entityFailed = true;
            std::cout << "[shadows] could not create the mob shadow map; mobs and players cast no shadows" << std::endl;
            return false;
        }
        return true;
    }

    void RenderContext_bgfx::RenderShadows(float camX, float camY, float camZ) {
        shadow.active = false;
        shadow.entities = false;
        bgfx::setViewClear(kViewShadowBuild, BGFX_CLEAR_NONE);
        bgfx::setViewClear(kViewShadowEntity, BGFX_CLEAR_NONE);
        bgfx::setViewClear(kViewShadowGlass, BGFX_CLEAR_NONE);
        if (!shadow.enabled || !frame.sky.enabled)
            return;
        const float sunA = frame.sky.sunAngle;
        glm::vec3 L(-std::sin(sunA), std::cos(sunA), 0.0f);
        if (L.y < 0.0f)
            L = -L;
        if (L.y < 0.03f)
            return;
        const int64_t lightKey[2] = {static_cast<int64_t>(std::llround(L.x * 300.0f)), static_cast<int64_t>(std::llround(L.y * 300.0f))};
        const glm::vec3 Lq = glm::normalize(glm::vec3(static_cast<float>(lightKey[0]), static_cast<float>(lightKey[1]),
                                                      std::round(L.z * 300.0f)) / 300.0f);

        const float R = shadow.distance;
        const float D = R + 80.0f;
        const float texel = 2.0f * R / static_cast<float>(shadow.size);
        const float step = texel * 32.0f;
        const glm::vec3 cam(camX, camY, camZ);
        struct Basis {
            glm::vec3 right, up, fwd;
        };
        const auto basisOf = [](const glm::vec3& light) {
            Basis b;
            b.fwd = -light;
            const glm::vec3 upRef = std::abs(b.fwd.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
            b.right = glm::normalize(glm::cross(upRef, b.fwd));
            b.up = glm::cross(b.fwd, b.right);
            return b;
        };
        const auto fillMatrices = [&](const Basis& b, float cx, float cy, float cz, float* sample, float* raster, float* info) {
            float S[16] = {};
            auto col = [&](int c, int r) -> float& { return S[c * 4 + r]; };
            for (int j = 0; j < 3; j++) {
                col(j, 0) = b.right[j] / (2.0f * R);
                col(j, 1) = b.up[j] / (2.0f * R);
                col(j, 2) = b.fwd[j] / (2.0f * D);
            }
            col(3, 0) = 0.5f - cx / (2.0f * R);
            col(3, 1) = 0.5f - cy / (2.0f * R);
            col(3, 2) = 0.5f - cz / (2.0f * D);
            col(3, 3) = 1.0f;
            std::memcpy(sample, S, sizeof(S));
            const bgfx::Caps* caps = bgfx::getCaps();
            const float sy = caps->originBottomLeft ? 1.0f : -1.0f;
            const float sz = caps->homogeneousDepth ? 2.0f : 1.0f;
            const float tz = caps->homogeneousDepth ? -1.0f : 0.0f;
            for (int c = 0; c < 4; c++) {
                const float u = S[c * 4 + 0], v = S[c * 4 + 1], d = S[c * 4 + 2], w = S[c * 4 + 3];
                raster[c * 4 + 0] = 2.0f * u - (c == 3 ? 1.0f : 0.0f);
                raster[c * 4 + 1] = sy * (2.0f * v - (c == 3 ? 1.0f : 0.0f));
                raster[c * 4 + 2] = sz * d + (c == 3 ? tz : 0.0f);
                raster[c * 4 + 3] = w;
            }
            info[0] = 1.0f / static_cast<float>(shadow.size);
            info[1] = 2.0f * R;
            info[2] = 2.0f * D;
            info[3] = shadow.tintEnabled ? 2.0f : 1.0f;
        };

        const double nowSeconds = frame.timeSeconds;
        const float dtFrame = shadow.lastTime > 0.0 ? static_cast<float>(std::clamp(nowSeconds - shadow.lastTime, 0.0, 0.1)) : 0.0f;
        shadow.lastTime = nowSeconds;

        const Basis kb = basisOf(Lq);
        const int64_t key[6] = {static_cast<int64_t>(std::llround(std::floor(glm::dot(cam, kb.right) / step))),
                                static_cast<int64_t>(std::llround(std::floor(glm::dot(cam, kb.up) / step))),
                                static_cast<int64_t>(std::llround(std::floor(glm::dot(cam, kb.fwd) / step))),
                                lightKey[0], lightKey[1], static_cast<int64_t>(meshGeneration)};
        shadow.framesSince++;
        const bool keyChanged = !shadow.haveKey || std::memcmp(key, shadow.key, 5 * sizeof(int64_t)) != 0;
        const bool meshOnly = !keyChanged && key[5] != shadow.key[5];
        if (!shadow.building && !shadow.fading &&
            (keyChanged || shadow.framesSince > 600 || (meshOnly && nowSeconds - shadow.lastBuildAt > 0.5))) {
            shadow.lastBuildAt = nowSeconds;
            shadow.building = true;
            shadow.haveKey = true;
            shadow.framesSince = 0;
            std::memcpy(shadow.key, key, sizeof(key));
            const Basis eb = basisOf(L);
            const glm::vec3 right = eb.right, up = eb.up, fwd = eb.fwd;
            const float cx = std::floor(glm::dot(cam, right) / step) * step;
            const float cy = std::floor(glm::dot(cam, up) / step) * step;
            const float cz = std::floor(glm::dot(cam, fwd) / step) * step;
            shadow.buildRight = right;
            shadow.buildUp = up;
            shadow.buildFwd = fwd;
            shadow.buildCx = cx;
            shadow.buildCy = cy;
            shadow.buildCz = cz;
            ShadowMap& m = shadow.maps[shadow.front == 0 ? 1 : 0];
            fillMatrices(eb, cx, cy, cz, m.sample, m.raster, m.info);

            shadow.buildList.clear();
            std::vector<std::pair<float, std::pair<int64_t, uint8_t>>> found;
            const float reachXY = R, reachZ = D;
            for (const auto& entry : gpuChunks) {
                const GpuChunk& g = entry.second;
                if (g.page < 0)
                    continue;
                const glm::vec3 centre(g.cx * 16.0f + 8.0f, (g.minY + g.maxY) * 0.5f, g.cz * 16.0f + 8.0f);
                const float radius = std::sqrt(128.0f + 0.25f * (g.maxY - g.minY) * (g.maxY - g.minY));
                if (std::abs(glm::dot(centre, right) - cx) > reachXY + radius || std::abs(glm::dot(centre, up) - cy) > reachXY + radius ||
                    std::abs(glm::dot(centre, fwd) - cz) > reachZ + radius)
                    continue;
                const glm::vec3 rel = centre - cam;
                const float d2 = rel.x * rel.x + rel.z * rel.z;
                for (uint8_t pass = 0; pass < (shadow.tintEnabled ? 5 : 3); pass++) {
                    static constexpr RenderLayer kLayers[5] = {RenderLayer::Solid, RenderLayer::Cutout, RenderLayer::CutoutCull,
                                                               RenderLayer::Translucent, RenderLayer::TranslucentCull};
                    if (g.layerCount[static_cast<size_t>(kLayers[pass])])
                        found.push_back({d2, {entry.first, pass}});
                }
            }
            std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
                return a.second.second != b.second.second ? a.second.second < b.second.second : a.first < b.first;
            });
            shadow.buildList.reserve(found.size());
            for (const auto& f : found)
                shadow.buildList.push_back(f.second);
            shadow.buildCursor = 0;
            shadow.buildSlice = 0;
            shadow.buildSlices = shadow.front < 0 ? 1 : 4;
        }

        if (shadow.building) {
            ShadowMap& m = shadow.maps[shadow.front == 0 ? 1 : 0];
            bgfx::setViewFrameBuffer(kViewShadowBuild, m.fb);
            bgfx::setViewRect(kViewShadowBuild, 0, 0, static_cast<uint16_t>(shadow.size), static_cast<uint16_t>(shadow.size));
            if (shadow.buildSlice == 0)
                bgfx::setViewClear(kViewShadowBuild, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0xFFFFFFFFu, 1.0f, 0);
            bgfx::touch(kViewShadowBuild);
            if (shadow.tintEnabled) {
                const uint16_t h16 = static_cast<uint16_t>(std::max(1, shadow.size / 2));
                bgfx::setViewFrameBuffer(kViewShadowGlass, m.tintFb);
                bgfx::setViewRect(kViewShadowGlass, 0, 0, h16, h16);
                if (shadow.buildSlice == 0)
                    bgfx::setViewClear(kViewShadowGlass, BGFX_CLEAR_COLOR, 0xFFFFFFFFu, 1.0f, 0);
                bgfx::touch(kViewShadowGlass);
            }
            const size_t remainingSlices = static_cast<size_t>(std::max(1, shadow.buildSlices - shadow.buildSlice));
            const size_t left = shadow.buildList.size() - shadow.buildCursor;
            const size_t count = (left + remainingSlices - 1) / remainingSlices;
            const bgfx::TextureHandle atlas = bgfx::isValid(atlasTex) ? atlasTex : whiteTex;
            const uint64_t state = BGFX_STATE_WRITE_R | BGFX_STATE_WRITE_G | BGFX_STATE_WRITE_B | BGFX_STATE_WRITE_A |
                                   BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS;
            constexpr uint8_t kKeep = BGFX_DISCARD_INDEX_BUFFER | BGFX_DISCARD_VERTEX_STREAMS;
            static constexpr RenderLayer kLayers[5] = {RenderLayer::Solid, RenderLayer::Cutout, RenderLayer::CutoutCull,
                                                       RenderLayer::Translucent, RenderLayer::TranslucentCull};
            const uint64_t glassState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                                        BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_DST_COLOR, BGFX_STATE_BLEND_ZERO,
                                                                       BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE) |
                                        BGFX_STATE_BLEND_EQUATION_SEPARATE(BGFX_STATE_BLEND_EQUATION_ADD, BGFX_STATE_BLEND_EQUATION_MIN);
            int primedPass = -1;
            for (size_t i = 0; i < count; i++) {
                const auto [chunkKey, pass] = shadow.buildList[shadow.buildCursor + i];
                const auto it = gpuChunks.find(chunkKey);
                if (it == gpuChunks.end() || it->second.page < 0)
                    continue;
                const GpuChunk& g = it->second;
                const size_t slot = static_cast<size_t>(kLayers[pass]);
                if (!g.layerCount[slot])
                    continue;
                const bool glass = pass >= 3;
                if (primedPass != pass) {
                    if (primedPass >= 0)
                        bgfx::discard(BGFX_DISCARD_ALL);
                    shaders.SetMatrix(m.raster);
                    shaders.SetShadow(m.sample, m.info, true);
                    bgfx::setState(glass ? glassState : state);
                    shaders.Apply(atlas, lightmapTex, samplerFlags, lightmapFlags);
                    if (bgfx::isValid(tileFlagsTexture))
                        shaders.BindTileFlags(tileFlagsTexture);
                    if (glass)
                        shaders.BindShadow(m.color);
                    primedPass = pass;
                }
                const Page& pg = pages[g.page];
                bgfx::setVertexBuffer(0, pg.vb);
                bgfx::setIndexBuffer(pg.ib[slot], g.iOff[slot], g.layerCount[slot]);
                const ShaderProgram program = glass ? ShaderProgram::ShadowGlass : (pass == 0 ? ShaderProgram::Shadow : ShaderProgram::ShadowCutout);
                bgfx::submit(glass ? kViewShadowGlass : kViewShadowBuild, shaders.Program(program), 0, kKeep);
            }
            if (primedPass >= 0)
                bgfx::discard(BGFX_DISCARD_ALL);
            shadow.buildCursor += count;
            shadow.buildSlice++;
            if (shadow.buildSlice >= shadow.buildSlices || shadow.buildCursor >= shadow.buildList.size()) {
                shadow.building = false;
                if (shadow.front < 0) {
                    shadow.front = 0;
                } else {
                    shadow.fading = true;
                    shadow.fadeT = 0.0f;
                }
                shadow.buildList.clear();
                shadow.buildList.shrink_to_fit();
            }
        }
        if (shadow.front < 0)
            return;

        shadow.fadeWeight = 0.0f;
        if (shadow.fading) {
            static constexpr float kFadeSeconds = 0.45f;
            shadow.fadeT += dtFrame / kFadeSeconds;
            if (shadow.fadeT >= 1.0f) {
                shadow.front = shadow.front == 0 ? 1 : 0;
                shadow.fading = false;
                shadow.fadeT = 0.0f;
            } else {
                shadow.fadeWeight = shadow.fadeT * shadow.fadeT * (3.0f - 2.0f * shadow.fadeT);
            }
        }
        const ShadowMap& f = shadow.maps[shadow.front];
        std::memcpy(shadow.sample, f.sample, sizeof(f.sample));
        std::memcpy(shadow.raster, f.raster, sizeof(f.raster));
        std::memcpy(shadow.info, f.info, sizeof(f.info));
        shadow.sampled = f.color;
        shadow.sampledTint = shadow.tintEnabled ? f.tint : whiteTex;
        shadow.sampledB = f.color;
        shadow.sampledTintB = shadow.sampledTint;
        std::memcpy(shadow.sampleB, f.sample, sizeof(f.sample));
        std::memcpy(shadow.infoB, f.info, sizeof(f.info));
        if (shadow.fading) {
            const ShadowMap& next = shadow.maps[shadow.front == 0 ? 1 : 0];
            std::memcpy(shadow.sampleB, next.sample, sizeof(next.sample));
            std::memcpy(shadow.infoB, next.info, sizeof(next.info));
            shadow.sampledB = next.color;
            shadow.sampledTintB = shadow.tintEnabled ? next.tint : whiteTex;
        }
        shadow.active = true;

        if (frame.shadowEntities && shaders.Has(ShaderProgram::EntityShadow) && EnsureEntityShadowMap()) {
            const Basis eb = basisOf(L);
            const float ex = std::floor(glm::dot(cam, eb.right) / texel) * texel;
            const float ey = std::floor(glm::dot(cam, eb.up) / texel) * texel;
            const float ez = std::floor(glm::dot(cam, eb.fwd) / texel) * texel;
            fillMatrices(eb, ex, ey, ez, shadow.sampleE, shadow.rasterE, shadow.infoE);
            bgfx::setViewFrameBuffer(kViewShadowEntity, shadow.entityFb);
            bgfx::setViewRect(kViewShadowEntity, 0, 0, static_cast<uint16_t>(shadow.size), static_cast<uint16_t>(shadow.size));
            bgfx::setViewClear(kViewShadowEntity, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0xFFFFFFFFu, 1.0f, 0);
            bgfx::touch(kViewShadowEntity);
            shadow.entities = true;
        }
    }

    void RenderContext_bgfx::DrawLayer(RenderLayer layer, ShaderProgram program, const Frustum& frustum,
                                       const std::vector<VisibleChunk>& chunks, bgfx::TextureHandle atlas) {
        static const bool hideBlend = PERLIN_ENV("PERLIN_HIDE_TRANSLUCENT") != nullptr;
        if (hideBlend && (layer == RenderLayer::Translucent || layer == RenderLayer::TranslucentCull))
            return;
        const size_t slot = static_cast<size_t>(layer);
        const bool ordered = layer == RenderLayer::Translucent || layer == RenderLayer::TranslucentCull;
        const bool lateView = ordered;
        struct Run {
            int page;
            uint32_t start, count;
        };
        static thread_local std::vector<Run> runs;
        runs.clear();
        auto push = [&](int page, uint32_t start, uint32_t count) {
            if (!runs.empty() && runs.back().page == page && runs.back().start + runs.back().count == start)
                runs.back().count += count;
            else
                runs.push_back({page, start, count});
        };
        for (const VisibleChunk& item : chunks) {
            const GpuChunk& g = *item.chunk;
            if (!g.layerCount[slot])
                continue;
            for (uint32_t i = item.secBegin; i < item.secEnd; i++) {
                const GpuSection& sec = *visibleSections[i];
                const uint32_t count = sec.count[slot];
                if (count)
                    push(g.page, g.iOff[slot] + sec.start[slot], count);
            }
        }
        if (runs.empty())
            return;
        drawCalls += static_cast<uint32_t>(runs.size());
        for (const Run& r : runs)
            drawnIndices += r.count;

        const uint64_t state = PassState(layer);
        constexpr uint8_t kKeep = BGFX_DISCARD_INDEX_BUFFER | BGFX_DISCARD_VERTEX_STREAMS;
        bool primed = false;
        for (const Run& r : runs) {
            const Page& pg = pages[r.page];
            bgfx::setVertexBuffer(0, pg.vb);
            bgfx::setIndexBuffer(pg.ib[slot], r.start, r.count);
            if (!primed) {
                const float off[4] = {0, 0, 0, 0};
                shaders.SetShadow(shadow.sample, shadow.active ? shadow.info : off, shadow.active);
                shaders.SetMatrix(mvp);
                bgfx::setState(state);
                shaders.Apply(atlas, lightmapTex, samplerFlags, lightmapFlags);
                if (shadow.active)
                    shaders.BindShadow(shadow.sampled);
                shaders.BindShadowTint(shadow.active && bgfx::isValid(shadow.sampledTint) ? shadow.sampledTint : whiteTex);
                shaders.SetShadowNext(shadow.sampleB, shadow.infoB, shadow.active ? shadow.fadeWeight : 0.0f);
                shaders.BindShadowNext(shadow.active && bgfx::isValid(shadow.sampledB) ? shadow.sampledB : whiteTex,
                                       shadow.active && bgfx::isValid(shadow.sampledTintB) ? shadow.sampledTintB : whiteTex);
                shaders.SetShadowEntities(shadow.sampleE, shadow.infoE, shadow.active && shadow.entities);
                shaders.BindShadowEntities(shadow.active && shadow.entities ? shadow.entityColor : whiteTex);
                shaders.BindClouds(cloudMapTexture);
                if (!bgfx::isValid(tileFlagsTexture))
                    UploadTileFlags({});
                shaders.BindTileFlags(tileFlagsTexture);
                if (ordered && scene.valid)
                    shaders.BindScene(scene.color, scene.depth);
                primed = true;
            }
            bgfx::submit(lateView ? kViewLate : 0, shaders.Program(program), 0, kKeep);
        }
        if (primed)
            bgfx::discard(BGFX_DISCARD_ALL);
    }

    void RenderContext_bgfx::DrawScreenTint() {
        const float* t = frame.screenTint;
        if (t[3] <= 0.001f || !bgfx::isValid(screenBuffer))
            return;
        shaders.SetModulator(t[0], t[1], t[2], t[3]);
        bgfx::setVertexBuffer(0, screenBuffer);
        bgfx::setState(BGFX_STATE_WRITE_RGB | kTranslucentBlend);
        bgfx::submit(kViewLate, shaders.Program(ShaderProgram::Screen));
    }

    void RenderContext_bgfx::DrawPortalOverlay() {
        const float alpha = frame.portalOverlay;
        if (alpha <= 0.001f || width <= 0 || height <= 0)
            return;
        if (!bgfx::isValid(portalTexture)) {
            Texture tex;
            if (!ui::UiAssets::Instance().Image("textures/blocks/portal.png", tex) || tex.width <= 0 || tex.pixels.empty())
                return;
            portalTexture = bgfx::createTexture2D(static_cast<uint16_t>(tex.width), static_cast<uint16_t>(tex.height), false, 1,
                                                  bgfx::TextureFormat::RGBA8,
                                                  BGFX_SAMPLER_MIN_ANISOTROPIC | BGFX_SAMPLER_MAG_ANISOTROPIC,
                                                  bgfx::copy(tex.pixels.data(), static_cast<uint32_t>(tex.pixels.size())));
            if (!bgfx::isValid(portalTexture))
                return;
        }
        if (bgfx::getAvailTransientVertexBuffer(4, textureLayout) < 4)
            return;
        // Java-style nether swirl: tiled portal texture slowly rotating, alpha ramps with time inside the portal.
        const float t = frame.timeSeconds;
        const float ang = t * 0.6f;
        const float c = std::cos(ang), s = std::sin(ang);
        constexpr float kTile = 2.5f;
        bgfx::TransientVertexBuffer tvb;
        bgfx::allocTransientVertexBuffer(&tvb, 4, textureLayout);
        auto* v = reinterpret_cast<TextureVertex*>(tvb.data);
        const float corners[4][2] = {{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};
        const float uvs[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
        for (int i = 0; i < 4; i++) {
            const float dx = uvs[i][0] - 0.5f, dy = uvs[i][1] - 0.5f;
            v[i] = {corners[i][0], corners[i][1], 0.0f, 0.5f + (dx * c - dy * s) * kTile, 0.5f + (dx * s + dy * c) * kTile};
        }
        static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        shaders.SetMatrix(identity);
        shaders.SetModulator(1.0f, 1.0f, 1.0f, std::min(1.0f, alpha));
        shaders.BindSky(portalTexture, BGFX_SAMPLER_MIN_ANISOTROPIC | BGFX_SAMPLER_MAG_ANISOTROPIC);
        bgfx::setVertexBuffer(0, &tvb);
        bgfx::setIndexBuffer(quadIndices);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | kTranslucentBlend);
        bgfx::submit(kViewTop, shaders.Program(shaders.Has(ShaderProgram::SpriteUi) ? ShaderProgram::SpriteUi : ShaderProgram::Celestial));
    }

    void RenderContext_bgfx::DrawChunkMeshes(const Frustum& frustum, float camX, float camZ) {
        visible.clear();
        visibleSections.clear();
        DrawSky();
        if (PERLIN_ENV("PERLIN_STATS") || PERLIN_ENV("PERLIN_PROFILE")) {
            static uint32_t frames = 0;
            if (++frames % 240 == 0)
                std::printf("[draw] %u submits, %.0fk triangles last frame, %zu chunks, %zu lod tiles, %zu pages\n", drawCalls,
                            static_cast<double>(drawnIndices) / 3000.0, gpuChunks.size(), lodChunks.size(), pages.size());
        }
        drawCalls = 0;
        drawnIndices = 0;
        ReleaseEmptyPages();
        if (gpuChunks.empty() && lodChunks.empty()) {
            DrawClouds();
            DrawScreenTint();
            DrawPortalOverlay();
            DrawCrosshair();
            return;
        }

        if (caveCulling)
            UpdateCaveVisibility(frame.cameraPosition[0], frame.cameraPosition[1], frame.cameraPosition[2]);
        visible.clear();
        const int cullCx = static_cast<int>(std::floor(camX / 16.0f)), cullCz = static_cast<int>(std::floor(camZ / 16.0f));
        for (const auto& entry : gpuChunks) {
            const GpuChunk& g = entry.second;
            if (g.page < 0 || g.sections.empty())
                continue;
            if (chunkCullRadius > 0) {
                const int ddx = g.cx - cullCx, ddz = g.cz - cullCz;
                if (std::abs(ddx) > chunkCullRadius || std::abs(ddz) > chunkCullRadius ||
                    ddx * ddx + ddz * ddz > (chunkCullRadius + 1) * (chunkCullRadius + 1))
                    continue;
            }
            const float x0 = g.cx * 16.0f, z0 = g.cz * 16.0f;
            const auto result = frustum.Test(x0, g.minY, z0, x0 + 16.0f, g.maxY, z0 + 16.0f);
            if (result == Frustum::Result::Outside)
                continue;
            const float dx = x0 + 8.0f - camX, dz = z0 + 8.0f - camZ;
            visible.push_back({dx * dx + dz * dz, &g, result == Frustum::Result::Inside});
        }
        for (const auto& entry : lodChunks) {
            const GpuChunk& g = entry.second;
            if (g.page < 0 || g.sections.empty())
                continue;
            const float s = static_cast<float>(g.size);
            const float x0 = g.cx * s, z0 = g.cz * s;
            const auto result = frustum.Test(x0, g.minY, z0, x0 + s, g.maxY, z0 + s);
            if (result == Frustum::Result::Outside)
                continue;
            const float dx = x0 + s * 0.5f - camX, dz = z0 + s * 0.5f - camZ;
            visible.push_back({dx * dx + dz * dz, &g, result == Frustum::Result::Inside});
        }

        std::sort(visible.begin(), visible.end(),
                  [](const VisibleChunk& x, const VisibleChunk& y) { return x.distance < y.distance; });
        visibleSections.clear();
        for (VisibleChunk& item : visible) {
            const GpuChunk& g = *item.chunk;
            const float x0 = g.cx * static_cast<float>(g.size), z0 = g.cz * static_cast<float>(g.size);
            const float extent = static_cast<float>(g.size);
            item.secBegin = static_cast<uint32_t>(visibleSections.size());
            for (const GpuSection& sec : g.sections) {
                if (g.secVis != ~0u) {
                    const int bit = sec.sy - g.visMin;
                    if (bit >= 0 && bit < 32 && !(g.secVis & (1u << bit)))
                        continue;
                }
                if (!item.fullyInside && !frustum.Visible(x0, sec.minY, z0, x0 + extent, sec.maxY, z0 + extent))
                    continue;
                visibleSections.push_back(&sec);
            }
            item.secEnd = static_cast<uint32_t>(visibleSections.size());
        }
        const bgfx::TextureHandle atlas = bgfx::isValid(atlasTex) ? atlasTex : whiteTex;
        RenderShadows(frame.cameraPosition[0], frame.cameraPosition[1], frame.cameraPosition[2]);
        DrawLayer(RenderLayer::Solid, ShaderProgram::TerrainSolid, frustum, visible, atlas);
        DrawLayer(RenderLayer::Cutout, ShaderProgram::TerrainCutout, frustum, visible, atlas);
        DrawLayer(RenderLayer::CutoutCull, ShaderProgram::TerrainCutout, frustum, visible, atlas);
        std::sort(visible.begin(), visible.end(),
                  [](const VisibleChunk& a, const VisibleChunk& b) { return a.distance > b.distance; });
        DrawLayer(RenderLayer::Translucent, ShaderProgram::TerrainTranslucent, frustum, visible, atlas);
        DrawLayer(RenderLayer::TranslucentCull, ShaderProgram::TerrainTranslucent, frustum, visible, atlas);
        DrawClouds();
        DrawScreenTint();
        DrawPortalOverlay();
        DrawCrosshair();
    }

    void RenderContext_bgfx::ClearChunkMeshes() {
        meshGeneration++;
        gpuChunks.clear();
        lodChunks.clear();
        DestroyPages();
    }

    void RenderContext_bgfx::DestroySkyResources() {
        for (bgfx::VertexBufferHandle* handle :
             {&topSkyBuffer, &bottomSkyBuffer, &skyBoxBuffer, &screenBuffer, &starBuffer, &sunriseBuffer, &sunBuffer, &moonBuffer})
            if (bgfx::isValid(*handle)) {
                bgfx::destroy(*handle);
                *handle = BGFX_INVALID_HANDLE;
            }
        for (bgfx::IndexBufferHandle* handle : {&skyFanIndices, &skyBoxIndices, &sunriseIndices, &starIndices, &quadIndices})
            if (bgfx::isValid(*handle)) {
                bgfx::destroy(*handle);
                *handle = BGFX_INVALID_HANDLE;
            }
        for (bgfx::TextureHandle* handle : {&sunTexture, &moonTexture})
            if (bgfx::isValid(*handle)) {
                bgfx::destroy(*handle);
                *handle = BGFX_INVALID_HANDLE;
            }
        if (bgfx::isValid(cloudVertexBuffer))
            bgfx::destroy(cloudVertexBuffer);
        if (bgfx::isValid(cloudIndexBuffer))
            bgfx::destroy(cloudIndexBuffer);
        cloudVertexBuffer = BGFX_INVALID_HANDLE;
        cloudIndexBuffer = BGFX_INVALID_HANDLE;
        cloudVertexCapacity = cloudIndexCapacity = 0;
        cloudBuilt = false;
    }

    bool RenderContext_bgfx::ReloadShaders() {
        if (!initialized)
            return false;
        bgfx::frame();
        shadowConfigDirty = true;
        return shaders.Reload();
    }

    void RenderContext_bgfx::Shutdown() {
        if (!initialized)
            return;
        while (!frameReadbacks.empty()) EndFrame();
        gpuChunks.clear();
        lodChunks.clear();
        DestroyPages();
        DestroyMotionBlur();
        DestroyUpscale();
        DestroyFrameGen();
        DestroySceneCopy();
        DestroyBloom();
        if (bgfx::isValid(cloudMapTexture))
            bgfx::destroy(cloudMapTexture);
        cloudMapTexture = BGFX_INVALID_HANDLE;
        if (bgfx::isValid(tileFlagsTexture))
            bgfx::destroy(tileFlagsTexture);
        tileFlagsTexture = BGFX_INVALID_HANDLE;
        if (bgfx::isValid(portalTexture))
            bgfx::destroy(portalTexture);
        portalTexture = BGFX_INVALID_HANDLE;
        DestroyShadows();
        DestroyUi();
        if (bgfx::isValid(atlasTex))
            bgfx::destroy(atlasTex);
        if (bgfx::isValid(whiteTex))
            bgfx::destroy(whiteTex);
        if (bgfx::isValid(lightmapTex))
            bgfx::destroy(lightmapTex);
        DestroySkyResources();
        shaders.Shutdown();
        atlasTex = whiteTex = lightmapTex = BGFX_INVALID_HANDLE;
        bgfx::shutdown();
        initialized = false;
    }
}
