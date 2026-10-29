#pragma once
#include <deque>
#include <mutex>
#include <future>
#include "client/renderer/RangeAllocator.hpp"
#include "client/renderer/RenderContext.hpp"
#include "client/renderer/Tessellator.hpp"
#include "client/renderer/shader/ShaderManager.hpp"

#include <bgfx/bgfx.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace perlin {
    class RenderContext_bgfx : public RenderContext {
    public:
        RenderContext_bgfx();
        ~RenderContext_bgfx() override;

        bool Initialize(AppPlatform& platform, const Options& options) override;
        std::string RendererName() const override { return rendererName; }
        std::string GpuVendor() const override;
        std::string GpuDescription() const override;
        void Shutdown() override;
        void BeginFrame(const FrameParams& params) override;
        void EndFrame() override;
        bool PresentGenerated(int index, int total) override;
        void RequestScreenshot(const std::string& path) override;
        void SetForcedSize(int w, int h) override { forcedW = w; forcedH = h; }
        void RequestFrameCapture(CaptureSink sink) override;
        bool SetExportTarget(bool on) override;
        bool QueryNativeHandles(NativeHandleInfo& out) const override;
        void DeliverCapture(uint32_t w, uint32_t h, uint32_t pitch, const uint8_t* bgra, bool flipY, bool rgba = false);
        void UploadSkyTextures(const Texture& sun, const Texture& moon, const Texture& clouds) override;
        void UploadCrosshair(const Texture& icons) override;
        void DrawHudCursor(float x, float y, float w, float h) override;
        void UploadAtlas(const uint8_t* rgba, int tilePx, int tiles) override;
        int MaxAtlasLayers(int tilePx) const override;
        bool ReloadShaders() override;
        bool PackShadowsActive() const override { return shadow.enabled; }
        void UploadTileFlags(const std::vector<uint8_t>& flags) override;
        void SetVsync(bool enabled) override;
        void UpdateAtlasRegion(int tile, const uint8_t* rgba) override;
        uint32_t UiCreateTexture(int width, int height, const uint8_t* rgba, bool linear, bool repeat = false) override;
        void UiDestroyTexture(uint32_t texture) override;
        uint32_t UiCreateMesh(const UiVertex* vertices, uint32_t vertexCount, const uint32_t* indices,
                              uint32_t indexCount) override;
        uint32_t UiCreateFrameMesh(const UiVertex* vertices, uint32_t vertexCount, const uint32_t* indices,
                                   uint32_t indexCount) override;
        void UiDestroyMesh(uint32_t mesh) override;
        uint32_t UiLiveMeshes() const override;
        void UiSubmit(UiLayer layer, const UiDraw& draw) override;
        void FramebufferSize(int& w, int& h) const override { w = width; h = height; }
        void UpdateChunkMesh(int cx, int cz, const Tessellator& tess, uint64_t version) override;
        void RemoveChunkMesh(int cx, int cz) override;
        void UpdateLodTile(int tx, int tz, int size, const Tessellator& tess, uint64_t version) override;
        void RemoveLodTile(int tx, int tz) override;
        void ClearChunkMeshes() override;
        void DrawChunkMeshes(const Frustum& frustum, float camX, float camZ) override;
        void SetChunkCullRadius(int chunks) override { chunkCullRadius = chunks; }

        void SaveScreenshotData(const std::string& path, uint32_t w, uint32_t h, uint32_t pitch,
                                const uint8_t* bgra, bool yflip);

    private:
        std::string rendererName;
        struct GpuChunk;
        struct VisibleChunk {
            float distance;
            const GpuChunk* chunk;
            bool fullyInside;
            uint32_t secBegin = 0, secEnd = 0;
        };
        struct Page {
            bgfx::DynamicVertexBufferHandle vb = BGFX_INVALID_HANDLE;
            std::array<bgfx::DynamicIndexBufferHandle, kRenderLayerCount> ib;
            RangeAllocator vAlloc;
            std::array<RangeAllocator, kRenderLayerCount> iAlloc;
            int emptyFrames = 0;
        };
        struct GpuSection {
            float minY, maxY;
            int sy = 0;
            std::array<uint32_t, kRenderLayerCount> start;
            std::array<uint32_t, kRenderLayerCount> count;
        };
        struct GpuChunk {
            int page = -1;
            uint32_t vOff = 0, vCount = 0;
            std::array<uint32_t, kRenderLayerCount> iOff{};
            std::array<uint32_t, kRenderLayerCount> layerCount{};
            std::vector<GpuSection> sections;
            uint64_t version = 0;
            float minY = 0, maxY = 0;
            int cx = 0, cz = 0;
            int size = 16;
            int visMin = 0;
            std::vector<uint16_t> conn;
            uint32_t secVis = ~0u;
        };
        void UpdateCaveVisibility(float camX, float camY, float camZ);
        uint64_t meshGeneration = 1;
        double caveLastRun = 0.0;
        uint64_t drawnIndices = 0;
        int chunkCullRadius = 0;
        uint64_t caveGeneration = 0;
        int caveCam[3] = {0, 0, 0};
        bool caveCulling = true;

        bool CreatePage(uint32_t vCap, const std::array<uint32_t, kRenderLayerCount>& iCap);
        void UploadMesh(std::unordered_map<int64_t, GpuChunk>& target, int cx, int cz, int size, const Tessellator& tess,
                        uint64_t version);
        bool AllocMesh(uint32_t v, const std::array<uint32_t, kRenderLayerCount>& i, GpuChunk& out);
        void FreeChunk(GpuChunk& g);
        void DestroyPages();
        void Resize(int w, int h);
        uint32_t ResetFlags() const;
        uint64_t PassState(RenderLayer layer) const;
        void ReleaseEmptyPages();
        void DrawLayer(RenderLayer layer, ShaderProgram program, const Frustum& frustum,
                       const std::vector<VisibleChunk>& chunks, bgfx::TextureHandle atlas);
        void DrawSky();
        void DrawClouds();
        void DestroySkyResources();
        void ConvertDepth(const float in[16], float out[16]) const;
        void UploadLightmap(const uint8_t* pixels);
        static int64_t ChunkKey(int cx, int cz) { return (static_cast<int64_t>(cx) << 32) | (uint32_t)cz; }

        struct ShadowMap {
            bgfx::FrameBufferHandle fb = BGFX_INVALID_HANDLE;
            bgfx::TextureHandle color = BGFX_INVALID_HANDLE;
            bgfx::FrameBufferHandle tintFb = BGFX_INVALID_HANDLE;
            bgfx::TextureHandle tint = BGFX_INVALID_HANDLE;
            float sample[16] = {};
            float raster[16] = {};
            float info[4] = {};
        };
        struct ShadowState {
            ShadowMap maps[2];
            int front = -1;
            bgfx::TextureHandle buildDepth = BGFX_INVALID_HANDLE;
            bgfx::FrameBufferHandle entityFb = BGFX_INVALID_HANDLE;
            bgfx::TextureHandle entityColor = BGFX_INVALID_HANDLE;
            bgfx::TextureHandle entityDepth = BGFX_INVALID_HANDLE;
            bool entityFailed = false;
            int size = 0;
            float distance = 80.0f;
            bool enabled = false;
            bool active = false;
            float sample[16] = {};
            float raster[16] = {};
            float info[4] = {};
            bgfx::TextureHandle sampled = BGFX_INVALID_HANDLE;
            bgfx::TextureHandle sampledTint = BGFX_INVALID_HANDLE;
            bool tintEnabled = false;
            int64_t key[6] = {};
            bool haveKey = false;
            int framesSince = 0;
            bool entities = false;
            float sampleE[16] = {};
            float rasterE[16] = {};
            float infoE[4] = {};
            bool fading = false;
            float fadeT = 0.0f;
            float fadeWeight = 0.0f;
            double lastTime = 0.0;
            double lastBuildAt = 0.0;
            float sampleB[16] = {};
            float infoB[4] = {};
            bgfx::TextureHandle sampledB = BGFX_INVALID_HANDLE;
            bgfx::TextureHandle sampledTintB = BGFX_INVALID_HANDLE;
            bool building = false;
            int buildSlice = 0, buildSlices = 1;
            std::vector<std::pair<int64_t, uint8_t>> buildList;
            size_t buildCursor = 0;
            glm::vec3 buildRight{}, buildUp{}, buildFwd{};
            float buildCx = 0, buildCy = 0, buildCz = 0;
        } shadow;
        int shadowQualityBuilt = -1, shadowDistanceBuilt = -1;
        bool shadowConfigDirty = true;
        void InitShadows(int quality, int distance);
        void DestroyShadows();
        void RenderShadows(float camX, float camY, float camZ);
        bool EnsureEntityShadowMap();

        class Callback;
        std::unique_ptr<Callback> callback;

        AppPlatform* platform = nullptr;
        bool vsync = true;
        bool initialized = false;

        bgfx::UniformHandle rdMaterial = BGFX_INVALID_HANDLE;

        bgfx::VertexLayout layout;
        bgfx::VertexLayout positionLayout;
        bgfx::VertexLayout colorLayout;
        bgfx::VertexLayout textureLayout;
        bgfx::VertexBufferHandle topSkyBuffer = BGFX_INVALID_HANDLE;
        bgfx::VertexBufferHandle bottomSkyBuffer = BGFX_INVALID_HANDLE;
        bgfx::VertexBufferHandle skyBoxBuffer = BGFX_INVALID_HANDLE;
        bgfx::IndexBufferHandle skyBoxIndices = BGFX_INVALID_HANDLE;
        bgfx::VertexBufferHandle screenBuffer = BGFX_INVALID_HANDLE;
        void DrawScreenTint();
        void DrawPortalOverlay();
        bgfx::TextureHandle portalTexture = BGFX_INVALID_HANDLE;

        struct MotionBlur {
            bgfx::TextureHandle worldColor = BGFX_INVALID_HANDLE, worldDepth = BGFX_INVALID_HANDLE;
            bgfx::FrameBufferHandle worldFb = BGFX_INVALID_HANDLE;
            bgfx::TextureHandle histColor[2] = {BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE};
            bgfx::FrameBufferHandle histFb[2] = {BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE};
            bgfx::UniformHandle sampCur = BGFX_INVALID_HANDLE, sampHist = BGFX_INVALID_HANDLE, params = BGFX_INVALID_HANDLE;
            bgfx::UniformHandle fx = BGFX_INVALID_HANDLE;
            int read = 0;
            bool valid = false;
            bool targets = false;
            bool scaled = false;
            int w = 0, h = 0;
            float lastTime = 0.0f;
        } blur;
        struct Plan {
            int w = 0, h = 0;
            bool scaled = false;
            bool temporal = false;
            bool frameGen = false;
            bool wantDepth = false;
            float jitter[2] = {0, 0};
            float jitterUv[2] = {0, 0};
        } plan;
        void PlanFrame(const FrameParams& params);
        struct Upscale {
            bgfx::TextureHandle tex[3] = {BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE};
            bgfx::FrameBufferHandle fb[3] = {BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE};
            bgfx::UniformHandle mode = BGFX_INVALID_HANDLE, size = BGFX_INVALID_HANDLE;
            bgfx::UniformHandle taa = BGFX_INVALID_HANDLE, taaSize = BGFX_INVALID_HANDLE, taaJit = BGFX_INVALID_HANDLE,
                                taaInvVp = BGFX_INVALID_HANDLE, taaPrevVp = BGFX_INVALID_HANDLE;
            bgfx::UniformHandle sampDepth = BGFX_INVALID_HANDLE, sampHist = BGFX_INVALID_HANDLE;
            int w = 0, h = 0;
            bool failed = false;
            int history = 0;
            bool historyValid = false;
            uint32_t frameIndex = 0;
        } upscale;
        void EnsureUpscaleUniforms();
        bool EnsureUpscale();
        void DestroyUpscale();
        bgfx::TextureHandle RunUpscale(bgfx::TextureHandle source, int sourceW, int sourceH, const FrameParams& params);
        struct CameraHistory {
            glm::dmat4 view{1.0}, proj{1.0}, prevView{1.0}, prevProj{1.0}, prev2View{1.0};
            glm::dvec3 eye{0.0}, prevEye{0.0};
            bool haveCur = false, havePrev = false, havePrev2 = false;
            bool teleported = false;
            int sizeW = 0, sizeH = 0;
            double time = 0.0, prevTime = 0.0;
        } cam;
        struct FrameGen {
            bgfx::TextureHandle finalColor = BGFX_INVALID_HANDLE, finalDepth = BGFX_INVALID_HANDLE, worldCopy = BGFX_INVALID_HANDLE;
            bgfx::FrameBufferHandle finalFb = BGFX_INVALID_HANDLE;
            bgfx::UniformHandle gen = BGFX_INVALID_HANDLE, invVp = BGFX_INVALID_HANDLE, curVp = BGFX_INVALID_HANDLE;
            bgfx::UniformHandle sampWorld = BGFX_INVALID_HANDLE, sampDepth = BGFX_INVALID_HANDLE, sampFinal = BGFX_INVALID_HANDLE;
            bool ready = false, failed = false;
            bool valid = false;
            int w = 0, h = 0;
            bool haveDepth = false;
        } fg;
        bool EnsureFrameGen();
        void DestroyFrameGen();
        void SubmitFrameGenCopies(float origin);
        struct SceneCopy {
            bgfx::TextureHandle color = BGFX_INVALID_HANDLE, depth = BGFX_INVALID_HANDLE;
            int w = 0, h = 0;
            bool valid = false;
            bool failed = false;
        } scene;
        bool depthReadable = false;
        bool EnsureSceneCopy();
        void DestroySceneCopy();
        struct Bloom {
            bgfx::TextureHandle level[4] = {BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE};
            bgfx::FrameBufferHandle fb[4] = {BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE};
            bgfx::UniformHandle params = BGFX_INVALID_HANDLE, texel = BGFX_INVALID_HANDLE, composite = BGFX_INVALID_HANDLE,
                                sampler = BGFX_INVALID_HANDLE;
            bool ready = false, failed = false;
        } bloom;
        bool EnsureBloom();
        void DestroyBloom();
        void RunBloom(float strength);
        bgfx::TextureHandle cloudMapTexture = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle tileFlagsTexture = BGFX_INVALID_HANDLE;
        bool EnsureMotionBlurTargets();
        void DestroyMotionBlur();
        void SetupMotionBlur(const FrameParams& params);
        void DrawCrosshair();
        bgfx::VertexBufferHandle starBuffer = BGFX_INVALID_HANDLE;
        bgfx::VertexBufferHandle sunriseBuffer = BGFX_INVALID_HANDLE;
        bgfx::VertexBufferHandle sunBuffer = BGFX_INVALID_HANDLE;
        bgfx::VertexBufferHandle moonBuffer = BGFX_INVALID_HANDLE;
        bgfx::IndexBufferHandle skyFanIndices = BGFX_INVALID_HANDLE;
        bgfx::IndexBufferHandle sunriseIndices = BGFX_INVALID_HANDLE;
        bgfx::IndexBufferHandle starIndices = BGFX_INVALID_HANDLE;
        bgfx::IndexBufferHandle quadIndices = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle sunTexture = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle moonTexture = BGFX_INVALID_HANDLE;
        uint32_t starIndexCount = 0;
        uint32_t skyFanIndexCount = 0;
        uint32_t sunriseIndexCount = 0;
        CloudTexture cloudTexture;
        bgfx::DynamicVertexBufferHandle cloudVertexBuffer = BGFX_INVALID_HANDLE;
        bgfx::DynamicIndexBufferHandle cloudIndexBuffer = BGFX_INVALID_HANDLE;
        uint32_t cloudVertexCapacity = 0;
        uint32_t cloudIndexCapacity = 0;
        uint32_t cloudIndexCount = 0;
        CloudFrame cloudBuiltFrame;
        bool cloudBuiltFancy = false;
        int cloudBuiltRange = 0;
        bool cloudBuilt = false;
        std::future<CloudMesh> cloudJob;
        bool cloudJobRunning = false;
        CloudFrame cloudJobFrame;
        float skyMvp[16] = {};
        FrameParams frame{};
        bgfx::TextureHandle lightmapTex = BGFX_INVALID_HANDLE;
        ShaderManager shaders;
        uint32_t lightmapFlags = 0;
        uint8_t lastLightmap[kLightmapBytes] = {};
        bool lightmapValid = false;
        bgfx::TextureHandle atlasTex = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle whiteTex = BGFX_INVALID_HANDLE;
        float mvp[16] = {};
        uint32_t samplerFlags = 0;
        uint64_t cullState = BGFX_STATE_CULL_CCW;
        bool homogeneousDepth = false;

        int width = 0, height = 0;
        int forcedW = 0, forcedH = 0;
        bool exportTarget = false;
        uint64_t nativeWindow = 0, nativeDisplay = 0;
        std::string adapterName;
        std::mutex captureMutex;
        std::deque<CaptureSink> captureSinks;
        struct FrameReadback {
            CaptureSink sink;
            bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
            std::vector<uint8_t> pixels;
            uint32_t width = 0, height = 0, ready = 0;
        };
        std::deque<FrameReadback> frameReadbacks;
        void FinishReadbacks(uint32_t frame);

        std::unordered_map<const uint8_t*, std::vector<std::vector<uint8_t>>> animatedTiles;
        int atlasLayers = 0;
        int tileSize = 16;
        bgfx::TextureHandle crosshairTexture = BGFX_INVALID_HANDLE;
        int crosshairPx = 0;

        std::vector<VisibleChunk> visible;
        std::vector<const GpuSection*> visibleSections;
        uint32_t drawCalls = 0;

        std::vector<Page> pages;
        std::unordered_map<int64_t, GpuChunk> gpuChunks;
        std::unordered_map<int64_t, GpuChunk> lodChunks;

        static constexpr uint32_t kUiMeshSlotBits = 20;
        static constexpr uint32_t kUiMeshSlotMask = (1u << kUiMeshSlotBits) - 1u;
        struct UiMeshGpu {
            bgfx::VertexBufferHandle vb = BGFX_INVALID_HANDLE;
            bgfx::IndexBufferHandle ib = BGFX_INVALID_HANDLE;
            uint32_t indexCount = 0;
            uint32_t generation = 0;
            bool used = false;
            bool transient = false;
            bgfx::TransientVertexBuffer tvb{};
            bgfx::TransientIndexBuffer tib{};
        };
        UiMeshGpu* LookupUiMesh(uint32_t id);
        uint32_t StoreUiMesh(const UiMeshGpu& mesh);
        void ReleaseUiMeshSlot(uint32_t slot);
        void ReleaseFrameMeshes();
        std::vector<uint32_t> freeUiMeshes;
        std::vector<uint32_t> frameUiMeshes;
        uint32_t staticUiMeshes = 0;
        uint32_t staticUiMeshWarnAt = 2048;
        struct UiTextureGpu {
            bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
            bool linear = false;
            bool used = false;
        };
        std::vector<UiMeshGpu> uiMeshes;
        std::vector<UiTextureGpu> uiTextures;
        bgfx::VertexLayout uiLayout;
        bgfx::UniformHandle uiSampler = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle uiParams = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle uiOverlay = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle uiGlintSampler = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle uiGlintA = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle uiGlintB = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle uiWhite = BGFX_INVALID_HANDLE;
        void DestroyUi();

        std::mutex shotMutex;
        std::atomic<bool> shotPending{false};
        std::atomic<bool> shotDone{false};
        bool shotOk = false;
    };
}
