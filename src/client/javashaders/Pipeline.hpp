#pragma once
#include "client/javashaders/PipelineConfig.hpp"
#include "client/javashaders/ShaderBackend.hpp"
#include "client/javashaders/Transform.hpp"
#include "client/javashaders/UniformTable.hpp"

#include <bgfx/bgfx.h>

#include <functional>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace perlin::jshader {
    struct PipelineHost {
        BackendKind backend = BackendKind::OpenGL;
        HlslCompiler hlslCompiler;
        int width = 1280, height = 720;
        bgfx::ViewId firstView = 64;
        bool originTopLeft = false;
        bool homogeneousDepth = true;
        int threads = 3;
        std::function<bool(const std::string& bytes, int& width, int& height, std::vector<uint8_t>& rgba)> decodeImage;
    };

    constexpr uint32_t AttribBit(int attrib) { return 1u << attrib; }

    enum class Slot {
        Shadow, Begin, Sky, Terrain, Block, Entity, Misc, HandSolid, Deferred, Translucent, Clouds, Weather, HandTranslucent, Composite, Final
    };

    struct DrawResult {
        bool ok = false;
        bgfx::ViewId view = 0;
        bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
        uint64_t state = 0;
        bool flipWinding = false;
    };

    struct EngineTextures {
        bgfx::TextureHandle blockAtlas = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle lightmap = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle normals = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle specular = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle overlay = BGFX_INVALID_HANDLE;
    };

    class Pipeline {
    public:
        Pipeline(std::shared_ptr<ShaderPackInstance> pack, const PipelineHost& host);
        ~Pipeline();
        Pipeline(const Pipeline&) = delete;
        Pipeline& operator=(const Pipeline&) = delete;

        const PipelineConfig& Config() const { return config; }
        ShaderPackInstance& Pack() { return *pack; }
        bool Usable() const { return usable; }
        const std::string& Error() const { return error; }

        uint16_t ViewCount() const;
        void AppendViewOrder(std::vector<bgfx::ViewId>& out) const;

        void Resize(int width, int height);
        void SetEngineTextures(const EngineTextures& tex) { engine = tex; }

        int WarmUp(uint32_t terrainMask);

        bool FinalOk() const { return finalOk; }
        int PassesRun() const { return passesRun; }
        bool ProgramUsable(const std::string& name, uint32_t mask, bool toBackbuffer = false);

        void BeginFrame(const FrameState& state);

        void Prefetch(const std::string& program, uint32_t attribMask);

        DrawResult Prepare(Slot slot, const std::string& program, uint32_t attribMask, const PackDrawState& draw, uint64_t baseState);

        void RunPostPasses(bgfx::FrameBufferHandle backbuffer, int outWidth, int outHeight, const bgfx::VertexLayout& quadLayout,
                           bool depthToBackbuffer = false);

        bgfx::ViewId ShadowView() const;
        int ShadowResolution() const { return config.shadowMapResolution; }

        bgfx::TextureHandle SceneColor() const;
        bool IsProgramAvailable(const std::string& program) const;

        static bgfx::VertexLayout FullscreenLayout();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
        std::shared_ptr<ShaderPackInstance> pack;
        PipelineHost host;
        PipelineConfig config;
        EngineTextures engine;
        bool usable = false;
        bool finalOk = false;
        int passesRun = 0;
        std::string error;
    };
}
