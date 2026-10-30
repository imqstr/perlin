#include "client/renderer/gpu/RenderContext_rhi.hpp"

#include "client/resources/ShaderPack.hpp"
#include "util/Env.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

namespace perlin {
    namespace {
        constexpr rhi::Format kPostColor = rhi::Format::RGBA8Unorm;
        constexpr rhi::Format kPostDepth = rhi::Format::D32Float;

        float Halton(uint32_t index, uint32_t base) {
            float f = 1.0f, r = 0.0f;
            while (index > 0) {
                f /= static_cast<float>(base);
                r += f * static_cast<float>(index % base);
                index /= base;
            }
            return r;
        }

        void StoreMatrix(const glm::dmat4& m, float* out) {
            for (int c = 0; c < 4; c++)
                for (int row = 0; row < 4; row++)
                    out[c * 4 + row] = static_cast<float>(m[c][row]);
        }

        // y origin: every backend here renders with the top-left origin, so the shaders flip the uv
        constexpr float kOrigin = 0.0f;
        // pushes the projection by a sub-pixel amount in clip space
        void ApplyJitter(float m[16], float ox, float oy) {
            for (int c = 0; c < 4; c++) {
                m[c * 4 + 0] += ox * m[c * 4 + 3];
                m[c * 4 + 1] += oy * m[c * 4 + 3];
            }
        }
    }

    // -- targets ----------------------------------------------------------------------------------------------------

    namespace {
        rhi::TextureHandle MakeTarget(gpu::Gpu& gpu, uint32_t w, uint32_t h, rhi::Format format, rhi::TextureUsage usage, const char* name) {
            rhi::TextureDesc td;
            td.format = format;
            td.width = std::max(1u, w);
            td.height = std::max(1u, h);
            td.usage = usage;
            td.name = name;
            const rhi::TextureHandle t = gpu.Dev().CreateTexture(td);
            if (t)
                gpu.SetState(t, rhi::State::Undefined);
            return t;
        }

        constexpr rhi::TextureUsage kColorRt = rhi::TextureUsage::ColorTarget | rhi::TextureUsage::Sampled | rhi::TextureUsage::CopySrc | rhi::TextureUsage::CopyDst;
    }

    bool RenderContext_rhi::JavaChainReady() const {
        if (!javaActive)
            return false;
        for (Program p : {Program::JavaDeferred, Program::JavaComposite, Program::JavaComposite1, Program::JavaComposite3,
                          Program::JavaComposite4, Program::JavaComposite5, Program::JavaComposite6, Program::JavaComposite7,
                          Program::JavaFinal})
            if (ProgramIndex(p) < 0)
                return false;
        return true;
    }

    bool RenderContext_rhi::EnsureJavaChainTargets(uint32_t w, uint32_t h) {
        const rhi::Format kPostColor = javaActive ? rhi::Format::RGBA16Float : rhi::Format::RGBA8Unorm;
        if (post.javaC1 && post.javaChainW == w && post.javaChainH == h)
            return true;
        gpu.Dev().WaitIdle();
        for (rhi::TextureHandle* t : {&post.javaC1, &post.javaC2, &post.javaC3, &post.javaC4, &post.javaC5, &post.javaC7,
                                      &post.javaTempA, &post.javaTempB, &post.javaTempC}) {
            if (*t)
                gpu.DestroyTexture(*t);
            *t = {};
        }
        postTextures.erase(std::remove_if(postTextures.begin(), postTextures.end(),
                                          [](rhi::TextureHandle t) { return !t; }),
                           postTextures.end());
        const auto make = [&](const char* name) {
            rhi::TextureHandle t = MakeTarget(gpu, w, h, kPostColor, kColorRt, name);
            if (t)
                postTextures.push_back(t);
            return t;
        };
        post.javaC1 = make("java colortex1");
        post.javaC2 = make("java colortex2");
        post.javaC3 = make("java colortex3");
        post.javaC4 = make("java colortex4");
        post.javaC5 = make("java colortex5");
        post.javaC7 = make("java colortex7");
        post.javaTempA = make("java temp a");
        post.javaTempB = make("java temp b");
        post.javaTempC = make("java temp c");
        post.javaChainClear = true;
        post.javaChainW = w;
        post.javaChainH = h;
        if (!post.javaC1 || !post.javaC2 || !post.javaC3 || !post.javaC4 || !post.javaC5 || !post.javaC7 || !post.javaTempA ||
            !post.javaTempB || !post.javaTempC) {
            std::cout << "[render] could not create java chain targets" << std::endl;
            return false;
        }
        return true;
    }

    void RenderContext_rhi::BuildJavaChainStages() {
        const rhi::Format kPostColor = javaActive ? rhi::Format::RGBA16Float : rhi::Format::RGBA8Unorm;
        const uint32_t w = static_cast<uint32_t>(plan.w), h = static_cast<uint32_t>(plan.h);
        if (post.javaChainClear) {
            for (auto texture : {post.javaC1, post.javaC2, post.javaC3, post.javaC5, post.javaC7}) {
                Stage clear;
                clear.target.color = texture;
                clear.target.w = w;
                clear.target.h = h;
                clear.target.colorFormat = kPostColor;
                clear.clearColor = true;
                stages.push_back(clear);
            }
            post.javaChainClear = false;
        }
        // each program renders into temps, then copies land in the colortex
        // buffers: passes both read and write the same buffers
        const auto pushCopy = [&](rhi::TextureHandle src, rhi::TextureHandle dst) {
            Stage c;
            c.kind = Stage::Kind::Copy;
            c.copySrc = src;
            c.copyDst = dst;
            c.copyW = w;
            c.copyH = h;
            stages.push_back(c);
        };
        // deferred: t0->C0, t1->C5, t2->C4
        {
            Target t;
            t.color = post.javaTempA;
            t.color1 = post.javaTempB;
            t.color2 = post.javaTempC;
            t.w = w;
            t.h = h;
            t.colorFormat = kPostColor;
            Stage s;
            s.target = t;
            s.views = {View::JavaDeferred};
            stages.push_back(s);
            pushCopy(post.javaTempA, post.worldColor);
            pushCopy(post.javaTempB, post.javaC5);
            pushCopy(post.javaTempC, post.javaC4);
        }
        pushCopy(post.worldColor, post.sceneColor);
        pushCopy(post.worldDepth, post.sceneDepth);
        Stage clearWater;
        clearWater.target.color = post.javaC3;
        clearWater.target.w = w;
        clearWater.target.h = h;
        clearWater.target.colorFormat = kPostColor;
        clearWater.clearColor = true;
        stages.push_back(clearWater);
        Stage water;
        water.target.color = post.worldColor;
        water.target.color1 = post.javaC3;
        water.target.depth = post.worldDepth;
        water.target.w = w;
        water.target.h = h;
        water.target.colorFormat = kPostColor;
        water.views = {View::Late};
        stages.push_back(water);
        const struct {
            Program program;
            View view;
            rhi::TextureHandle d0, d1;
        } rest[] = {
            {Program::JavaComposite, View::JavaComposite, post.javaC7, post.javaC1},
            {Program::JavaComposite1, View::JavaComposite1, post.worldColor, post.javaC5},
            {Program::JavaComposite3, View::JavaComposite3, post.worldColor, {}},
            {Program::JavaComposite4, View::JavaComposite4, post.javaC3, {}},
            {Program::JavaComposite5, View::JavaComposite5, post.javaC3, {}},
            {Program::JavaComposite6, View::JavaComposite6, post.javaC3, post.javaC2},
            {Program::JavaComposite7, View::JavaComposite7, post.javaC3, {}},
        };
        for (const auto& r : rest) {
            Target t;
            t.color = post.javaTempA;
            t.w = w;
            t.h = h;
            t.colorFormat = kPostColor;
            if (r.d1)
                t.color1 = post.javaTempB;
            Stage s;
            s.target = t;
            s.views = {r.view};
            stages.push_back(s);
            pushCopy(post.javaTempA, r.d0);
            if (r.d1)
                pushCopy(post.javaTempB, r.d1);
        }
        Stage out;
        out.toScreen = true;
        out.views = {View::JavaFinal};
        stages.push_back(out);
        {
            ViewData& vd = views[static_cast<size_t>(View::JavaFinal)];
            vd.colorFormat = gpu.TargetFormat();
            vd.depthFormat = rhi::Format::Unknown;
        }
    }

    void RenderContext_rhi::DestroyPostTargets() {
        for (rhi::TextureHandle* t : {&post.worldColor, &post.worldDepth, &post.hist[0], &post.hist[1], &post.bloom[0], &post.bloom[1],
                                      &post.bloom[2], &post.bloom[3], &post.up[0], &post.up[1], &post.up[2], &post.sceneColor,
                                      &post.sceneDepth, &post.javaAux, &post.javaC1, &post.javaC2, &post.javaC3, &post.javaC4,
                                      &post.javaC5, &post.javaC7, &post.javaTempA, &post.javaTempB, &post.javaTempC})
            if (*t) {
                gpu.DestroyTexture(*t);
                *t = {};
            }
        post.worldW = post.worldH = post.upW = post.upH = 0;
        post.histValid = post.upHistoryValid = post.sceneValid = false;
        postTextures.clear();
    }

    bool RenderContext_rhi::EnsureWorldTargets(uint32_t w, uint32_t h, bool, bool history) {
        const rhi::Format kPostColor = javaActive ? rhi::Format::RGBA16Float : rhi::Format::RGBA8Unorm;
        if (post.worldColor && post.worldW == w && post.worldH == h && (!history || post.hist[0]))
            return true;
        // a size change drops everything built for the old size
        gpu.Dev().WaitIdle();
        DestroyPostTargets();
        post.worldColor = MakeTarget(gpu, w, h, kPostColor, kColorRt, "world color");
        post.worldDepth = MakeTarget(gpu, w, h, kPostDepth, rhi::TextureUsage::DepthTarget | rhi::TextureUsage::Sampled | rhi::TextureUsage::CopySrc,
                                     "world depth");
        post.worldW = w;
        post.worldH = h;
        bool ok = post.worldColor && post.worldDepth;
        if (history)
            for (int i = 0; i < 2 && ok; i++) {
                post.hist[i] = MakeTarget(gpu, w, h, kPostColor, kColorRt, "blur history");
                ok = static_cast<bool>(post.hist[i]);
            }
        if (!ok) {
            std::cout << "[render] could not create the offscreen targets; post effects off" << std::endl;
            DestroyPostTargets();
            post.failed = true;
            return false;
        }
        postTextures = {post.worldColor, post.worldDepth, post.hist[0], post.hist[1]};
        postTextures.erase(std::remove_if(postTextures.begin(), postTextures.end(), [](rhi::TextureHandle t) { return !t; }), postTextures.end());
        return true;
    }

    bool RenderContext_rhi::EnsureBloomTargets() {
        if (post.bloom[0])
            return true;
        for (int i = 0; i < 4; i++) {
            const uint32_t div = 2u << i;
            post.bloomW[i] = std::max(1u, static_cast<uint32_t>(width) / div);
            post.bloomH[i] = std::max(1u, static_cast<uint32_t>(height) / div);
            post.bloom[i] = MakeTarget(gpu, post.bloomW[i], post.bloomH[i], kPostColor, kColorRt, "bloom");
            if (!post.bloom[i]) {
                for (int j = 0; j <= i; j++)
                    if (post.bloom[j]) {
                        gpu.DestroyTexture(post.bloom[j]);
                        post.bloom[j] = {};
                    }
                std::cout << "[bloom] could not create the bloom targets; bloom off" << std::endl;
                return false;
            }
            postTextures.push_back(post.bloom[i]);
        }
        return true;
    }

    bool RenderContext_rhi::EnsureUpscaleTargets() {
        if (post.up[0] && post.upW == static_cast<uint32_t>(width) && post.upH == static_cast<uint32_t>(height))
            return true;
        for (rhi::TextureHandle& t : post.up)
            if (t) {
                gpu.Dev().WaitIdle();
                gpu.DestroyTexture(t);
                t = {};
            }
        postTextures.erase(std::remove_if(postTextures.begin(), postTextures.end(),
                                          [&](rhi::TextureHandle t) { return t == post.up[0] || t == post.up[1] || t == post.up[2]; }),
                           postTextures.end());
        for (int i = 0; i < 3; i++) {
            post.up[i] = MakeTarget(gpu, static_cast<uint32_t>(width), static_cast<uint32_t>(height), kPostColor, kColorRt, "upscaled");
            if (!post.up[i]) {
                for (rhi::TextureHandle& t : post.up)
                    if (t) {
                        gpu.DestroyTexture(t);
                        t = {};
                    }
                std::cout << "[render] could not create the upscaler targets; using plain bilinear" << std::endl;
                return false;
            }
            postTextures.push_back(post.up[i]);
        }
        post.upW = static_cast<uint32_t>(width);
        post.upH = static_cast<uint32_t>(height);
        post.upHistoryValid = false;
        return true;
    }

    bool RenderContext_rhi::EnsureSceneTargets() {
        const rhi::Format kPostColor = javaActive ? rhi::Format::RGBA16Float : rhi::Format::RGBA8Unorm;
        if (post.sceneColor && post.sceneColor && post.worldW)
            return true;
        post.sceneColor = MakeTarget(gpu, post.worldW, post.worldH, kPostColor,
                                     rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst | rhi::TextureUsage::CopySrc, "scene color");
        post.sceneDepth = MakeTarget(gpu, post.worldW, post.worldH, kPostDepth,
                                     rhi::TextureUsage::DepthTarget | rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst, "scene depth");
        if (!post.sceneColor || !post.sceneDepth) {
            for (rhi::TextureHandle* t : {&post.sceneColor, &post.sceneDepth})
                if (*t) {
                    gpu.DestroyTexture(*t);
                    *t = {};
                }
            return false;
        }
        postTextures.push_back(post.sceneColor);
        postTextures.push_back(post.sceneDepth);
        return true;
    }

    // -- planning ---------------------------------------------------------------------------------------------------

    void RenderContext_rhi::PlanPost(const FrameParams& params) {
        const rhi::Format kPostColor = javaActive ? rhi::Format::RGBA16Float : rhi::Format::RGBA8Unorm;
        plan = PostPlan{};
        stages.clear();
        shadowStages.clear();
        static const bool noUpscale = PERLIN_ENV("PERLIN_NO_UPSCALE") != nullptr;
        static const bool noSsr = PERLIN_ENV("PERLIN_NO_SSR") != nullptr;
        static const bool noBloom = PERLIN_ENV("PERLIN_NO_BLOOM") != nullptr;
        static const bool noBlur = PERLIN_ENV("PERLIN_NO_BLUR") != nullptr;

        const float scale = noUpscale ? 1.0f : std::clamp(params.renderScale, 0.25f, 1.0f);
        plan.scaled = scale < 0.999f;
        plan.w = plan.scaled ? std::max(32, static_cast<int>(std::lround(static_cast<float>(width) * scale))) : width;
        plan.h = plan.scaled ? std::max(32, static_cast<int>(std::lround(static_cast<float>(height) * scale))) : height;
        plan.temporal = !noUpscale && params.upscaler == 3 && ProgramIndex(Program::Temporal) >= 0;
        plan.bloom = params.bloom > 0.001f && !noBloom && ProgramIndex(Program::Bloom) >= 0;
        plan.blur = (params.motionBlur > 0.001f || params.menuBlur > 0.01f) && !noBlur && ProgramIndex(Program::Post) >= 0;
        ShaderPack& pack = ShaderPack::Instance();
        plan.sceneCopy = packActive && pack.Flag("sceneCopy", false) && !noSsr;
        plan.active = !post.failed && (plan.blur || plan.sceneCopy || plan.bloom || plan.scaled || plan.temporal) && ProgramIndex(Program::Post) >= 0;
        // java MRT needs the offscreen chain even with every effect off
        if (javaActive && !post.failed && ProgramIndex(Program::Post) >= 0)
            plan.active = true;
        // the converted chain replaces post effects and scaling wholesale
        if (JavaChainReady()) {
            plan.scaled = plan.temporal = plan.bloom = plan.blur = false;
            plan.sceneCopy = true;
            plan.w = width;
            plan.h = height;
        }

        const float clear[4] = {params.fogColor[0], params.fogColor[1], params.fogColor[2], 1.0f};
        const auto direct = [&] {
            plan.active = false;
            Stage world;
            world.toScreen = true;
            world.useScreenDepth = true;
            world.views = {View::World, View::Entity, View::Late};
            world.clearColor = world.clearDepth = true;
            std::memcpy(world.clear, clear, sizeof(clear));
            stages.push_back(world);
        };
        const auto overlays = [&] {
            Stage hand;
            hand.toScreen = hand.useScreenDepth = hand.clearDepth = true;
            hand.views = {View::Hand, View::Gui};
            stages.push_back(hand);
            Stage top;
            top.toScreen = top.useScreenDepth = top.clearDepth = true;
            top.views = {View::Doll, View::Top, View::ExportUi};
            stages.push_back(top);
        };

        if (!plan.active) {
            if (post.worldColor) {
                gpu.Dev().WaitIdle();
                DestroyPostTargets();
            }
            direct();
            overlays();
            return;
        }
        if (!EnsureWorldTargets(static_cast<uint32_t>(plan.w), static_cast<uint32_t>(plan.h), plan.temporal || plan.sceneCopy, true)) {
            direct();
            overlays();
            return;
        }
        if (plan.sceneCopy && !EnsureSceneTargets())
            plan.sceneCopy = false;
        const bool bloomRun = plan.bloom && EnsureBloomTargets();
        plan.bloom = bloomRun;
        if (!plan.bloom && post.bloom[0]) {
            gpu.Dev().WaitIdle();
            for (rhi::TextureHandle& t : post.bloom) {
                postTextures.erase(std::remove(postTextures.begin(), postTextures.end(), t), postTextures.end());
                gpu.DestroyTexture(t);
                t = {};
            }
        }
        if (plan.temporal) {
            const uint32_t index = (post.frameIndex++ % 16) + 1;
            const float jx = Halton(index, 2) - 0.5f, jy = Halton(index, 3) - 0.5f;
            const float ySign = kOrigin > 0.5f ? 1.0f : -1.0f;
            plan.jitter[0] = 2.0f * jx / static_cast<float>(plan.w);
            plan.jitter[1] = 2.0f * jy / static_cast<float>(plan.h);
            plan.jitterUv[0] = jx / static_cast<float>(plan.w);
            plan.jitterUv[1] = ySign * jy / static_cast<float>(plan.h);
            // the world views draw with the jittered projection; everything else stays crisp
            glm::mat4& proj = views[static_cast<size_t>(View::World)].proj;
            float m[16];
            std::memcpy(m, glm::value_ptr(proj), sizeof(m));
            ApplyJitter(m, plan.jitter[0], plan.jitter[1]);
            const glm::mat4 jittered = glm::make_mat4(m);
            for (View v : {View::World, View::Entity, View::Late})
                views[static_cast<size_t>(v)].proj = jittered;
        }
        for (View v : {View::World, View::Entity, View::Late}) {
            ViewData& vd = views[static_cast<size_t>(v)];
            vd.w = static_cast<uint32_t>(plan.w);
            vd.h = static_cast<uint32_t>(plan.h);
            vd.colorFormat = kPostColor;
            vd.depthFormat = kPostDepth;
            // second MRT attachment carries the java colortex1 buffer
            vd.colorCount = javaActive && post.javaAux ? 2 : 1;
            vd.colorFormat1 = kPostColor;
        }

        if (JavaChainReady() && !EnsureJavaChainTargets(static_cast<uint32_t>(plan.w), static_cast<uint32_t>(plan.h))) {
            direct();
            overlays();
            return;
        }
        Target world;
        world.color = post.worldColor;
        world.depth = post.worldDepth;
        if (javaActive) {
            if (!post.javaAux || post.worldW != static_cast<uint32_t>(plan.w) || post.worldH != static_cast<uint32_t>(plan.h)) {
                if (post.javaAux)
                    gpu.DestroyTexture(post.javaAux);
                post.javaAux = MakeTarget(gpu, static_cast<uint32_t>(plan.w), static_cast<uint32_t>(plan.h), kPostColor,
                                          kColorRt, "java aux");
                if (!post.javaAux)
                    return;
                postTextures.push_back(post.javaAux);
            }
            world.color1 = post.javaAux;
            world.color2 = post.javaC4;
            for (View v : {View::World, View::Entity, View::Late}) {
                views[static_cast<size_t>(v)].colorCount = 3;
                views[static_cast<size_t>(v)].colorFormat2 = kPostColor;
                views[static_cast<size_t>(v)].colorFormat1 = kPostColor;
            }
        }
        world.w = static_cast<uint32_t>(plan.w);
        world.h = static_cast<uint32_t>(plan.h);
        world.colorFormat = kPostColor;
        Stage opaque;
        opaque.target = world;
        opaque.clearColor = opaque.clearDepth = true;
        std::memcpy(opaque.clear, clear, sizeof(clear));
        if (JavaChainReady()) {
            opaque.views = {View::World, View::Entity};
            stages.push_back(opaque);
            post.sceneValid = true;
            ViewData& late = views[static_cast<size_t>(View::Late)];
            late.colorCount = 2;
        } else if (plan.sceneCopy) {
            opaque.views = {View::World, View::Entity};
            stages.push_back(opaque);
            Stage copyColor;
            copyColor.kind = Stage::Kind::Copy;
            copyColor.copySrc = post.worldColor;
            copyColor.copyDst = post.sceneColor;
            copyColor.copyW = world.w;
            copyColor.copyH = world.h;
            stages.push_back(copyColor);
            Stage copyDepth = copyColor;
            copyDepth.copySrc = post.worldDepth;
            copyDepth.copyDst = post.sceneDepth;
            copyDepth.copyDepth = true;
            stages.push_back(copyDepth);
            Stage late;
            late.target = world;
            late.views = {View::Late};
            stages.push_back(late);
            post.sceneValid = true;
        } else {
            opaque.views = {View::World, View::Entity, View::Late};
            stages.push_back(opaque);
            post.sceneValid = false;
        }

        if (JavaChainReady() && EnsureJavaChainTargets(static_cast<uint32_t>(plan.w), static_cast<uint32_t>(plan.h))) {
            BuildJavaChainStages();
            overlays();
            return;
        }

        if (plan.bloom) {
            // down to the smallest level, then back up additively
            static const struct {
                View view;
                int target;
                bool additive;
            } kBloom[7] = {{View::Bloom0, 0, false}, {View::Bloom1, 1, false}, {View::Bloom2, 2, false}, {View::Bloom3, 3, false},
                           {View::Bloom4, 2, true},  {View::Bloom5, 1, true},  {View::Bloom6, 0, true}};
            for (const auto& pass : kBloom) {
                Stage s;
                s.target.color = post.bloom[pass.target];
                s.target.w = post.bloomW[pass.target];
                s.target.h = post.bloomH[pass.target];
                s.target.colorFormat = kPostColor;
                s.views = {pass.view};
                s.clearColor = !pass.additive;
                stages.push_back(s);
                ViewData& vd = views[static_cast<size_t>(pass.view)];
                vd.colorFormat = kPostColor;
                vd.depthFormat = rhi::Format::Unknown;
            }
        }

        const bool blurRun = plan.blur;
        if (blurRun) {
            Stage s;
            s.target.color = post.hist[1 - post.read];
            s.target.w = world.w;
            s.target.h = world.h;
            s.target.colorFormat = kPostColor;
            s.views = {View::Blur};
            stages.push_back(s);
            ViewData& vd = views[static_cast<size_t>(View::Blur)];
            vd.colorFormat = kPostColor;
            vd.depthFormat = rhi::Format::Unknown;
        }

        const bool spatial = !plan.temporal && plan.scaled && params.upscaler != 0 && !noUpscale && ProgramIndex(Program::Upscale) >= 0;
        const bool upscaled = (plan.temporal || spatial) && EnsureUpscaleTargets();
        if (!upscaled) {
            plan.temporal = false;
        } else {
            const int write = plan.temporal ? 1 - post.upHistory : 0;
            Stage s;
            s.target.color = post.up[write];
            s.target.w = static_cast<uint32_t>(width);
            s.target.h = static_cast<uint32_t>(height);
            s.target.colorFormat = kPostColor;
            s.views = {View::Up0};
            stages.push_back(s);
            ViewData& vd = views[static_cast<size_t>(View::Up0)];
            vd.colorFormat = kPostColor;
            vd.depthFormat = rhi::Format::Unknown;
            if (params.upscaleSharpness > 0.001f && ProgramIndex(Program::Upscale) >= 0) {
                Stage sharp;
                sharp.target.color = post.up[plan.temporal ? 2 : 1];
                sharp.target.w = static_cast<uint32_t>(width);
                sharp.target.h = static_cast<uint32_t>(height);
                sharp.target.colorFormat = kPostColor;
                sharp.views = {View::Up1};
                stages.push_back(sharp);
                ViewData& v1 = views[static_cast<size_t>(View::Up1)];
                v1.colorFormat = kPostColor;
                v1.depthFormat = rhi::Format::Unknown;
            }
        }
        if (!upscaled && plan.scaled) {
            // no filter available: the scaled image is stretched by the final pass
        }

        Stage out;
        out.toScreen = true;
        out.views = {View::BlurOut};
        stages.push_back(out);
        {
            ViewData& vd = views[static_cast<size_t>(View::BlurOut)];
            vd.colorFormat = gpu.TargetFormat();
            vd.depthFormat = rhi::Format::Unknown;
        }
        overlays();
    }

    // -- recording --------------------------------------------------------------------------------------------------

    void RenderContext_rhi::FullscreenPass(View view, Program program, rhi::Format colorFormat, uint32_t w, uint32_t h, bool additive) {
        ViewData& vd = views[static_cast<size_t>(view)];
        vd.x = vd.y = 0;
        vd.w = w;
        vd.h = h;
        vd.colorFormat = colorFormat;
        vd.depthFormat = rhi::Format::Unknown;
        DrawState s = Base(program, false, false, rhi::CompareOp::Always);
        s.layout = gpu::VertexLayout::Position;
        s.mask = rhi::ColorMask::All;
        s.cull = rhi::CullMode::None;
        if (additive) {
            s.blend = true;
            s.srcColor = s.srcAlpha = rhi::BlendFactor::One;
            s.dstColor = s.dstAlpha = rhi::BlendFactor::One;
        }
        Submit(view, s, {screenTri, 0}, nullptr, rhi::IndexType::U32, 0, 3);
    }

    void RenderContext_rhi::RecordBloom(float strength) {
        const float threshold = 0.82f - 0.12f * std::clamp(strength, 0.0f, 1.0f);
        const float W = static_cast<float>(std::max(1, width)), H = static_cast<float>(std::max(1, height));
        const auto pass = [&](View view, int target, rhi::TextureHandle source, float sourceScale, float mode, bool additive) {
            uni.Vec4("u_post", 0.0f, kOrigin, 0.0f, 0.0f);
            uni.Vec4("u_bloomParams", mode, threshold, 0.18f, 0.0f);
            const bool fromWorld = source == post.worldColor && plan.scaled;
            uni.Vec4("u_bloomTexel", fromWorld ? 1.0f / static_cast<float>(std::max(1, plan.w)) : sourceScale / W,
                     fromWorld ? 1.0f / static_cast<float>(std::max(1, plan.h)) : sourceScale / H, 0.0f, 0.0f);
            BindTexture("s_cur", source, gpu::SamplerKind::LinearClamp);
            FullscreenPass(view, Program::Bloom, kPostColor, post.bloomW[target], post.bloomH[target], additive);
            ResetBindings();
        };
        pass(View::Bloom0, 0, post.worldColor, 1.0f, 0.0f, false);
        pass(View::Bloom1, 1, post.bloom[0], 2.0f, 1.0f, false);
        pass(View::Bloom2, 2, post.bloom[1], 4.0f, 1.0f, false);
        pass(View::Bloom3, 3, post.bloom[2], 8.0f, 1.0f, false);
        pass(View::Bloom4, 2, post.bloom[3], 16.0f, 2.0f, true);
        pass(View::Bloom5, 1, post.bloom[2], 8.0f, 2.0f, true);
        pass(View::Bloom6, 0, post.bloom[1], 4.0f, 2.0f, true);
    }

    rhi::TextureHandle RenderContext_rhi::RecordUpscale(rhi::TextureHandle source, uint32_t sourceW, uint32_t sourceH) {
        const FrameParams& params = frame;
        const float sharp = std::clamp(params.upscaleSharpness, 0.0f, 1.0f);
        const bool sharpen = sharp > 0.001f && ProgramIndex(Program::Upscale) >= 0;
        const uint32_t W = static_cast<uint32_t>(width), H = static_cast<uint32_t>(height);
        const auto spatialPass = [&](View view, rhi::TextureHandle from, float mode, uint32_t fromW, uint32_t fromH) {
            uni.Vec4("u_post", 0.0f, kOrigin, 0.0f, 0.0f);
            uni.Vec4("u_up", mode, sharp, 0.0f, 0.0f);
            uni.Vec4("u_upSize", static_cast<float>(fromW), static_cast<float>(fromH), static_cast<float>(W), static_cast<float>(H));
            BindTexture("s_cur", from, gpu::SamplerKind::LinearClamp);
            FullscreenPass(view, Program::Upscale, kPostColor, W, H, false);
            ResetBindings();
        };
        if (!plan.temporal) {
            post.upHistoryValid = false;
            spatialPass(View::Up0, source, params.upscaler == 1 ? 0.0f : 1.0f, sourceW, sourceH);
            if (!sharpen)
                return post.up[0];
            spatialPass(View::Up1, post.up[0], 2.0f, W, H);
            return post.up[1];
        }

        const int write = 1 - post.upHistory;
        const int read = post.upHistory;
        const bool useHistory = post.upHistoryValid && cam.havePrev && !cam.teleported;
        const double ratio = static_cast<double>(sourceW) / static_cast<double>(std::max(1u, W));
        const float alpha = static_cast<float>(0.08 + (1.0 - std::min(1.0, ratio)) * 0.2);
        const float ySign = kOrigin > 0.5f ? 1.0f : -1.0f;
        const glm::dmat4 shift = glm::translate(glm::dmat4(1.0), cam.eye);
        const glm::dmat4 curVp = cam.proj * cam.view * shift;
        const glm::dmat4 prevVp = cam.prevProj * cam.prevView * shift;
        float invVp[16], prevVpF[16];
        StoreMatrix(glm::inverse(curVp), invVp);
        StoreMatrix(prevVp, prevVpF);
        uni.Vec4("u_post", 0.0f, kOrigin, 0.0f, 0.0f);
        uni.Vec4("u_taa", alpha, useHistory ? 1.0f : 0.0f, ySign, 0.0f);
        uni.Vec4("u_taaSize", static_cast<float>(sourceW), static_cast<float>(sourceH), static_cast<float>(W), static_cast<float>(H));
        uni.Vec4("u_taaJit", plan.jitterUv[0], plan.jitterUv[1], 0.0f, 0.0f);
        uni.Mat4("u_taaInvVp", invVp);
        uni.Mat4("u_taaPrevVp", prevVpF);
        BindTexture("s_cur", source, gpu::SamplerKind::PointClamp);
        BindTexture("s_depth", post.worldDepth, gpu::SamplerKind::PointClamp);
        BindTexture("s_hist", post.up[read], gpu::SamplerKind::LinearClamp);
        FullscreenPass(View::Up0, Program::Temporal, kPostColor, W, H, false);
        ResetBindings();
        post.upHistory = write;
        post.upHistoryValid = true;
        if (!sharpen)
            return post.up[write];
        spatialPass(View::Up1, post.up[write], 2.0f, W, H);
        return post.up[2];
    }

    void RenderContext_rhi::RecordPost() {
        if (!plan.active || !post.worldColor)
            return;
        if (JavaChainReady()) {
            RecordJavaChain();
            return;
        }
        const FrameParams& params = frame;
        const float intensity = std::clamp(params.motionBlur, 0.0f, 1.0f);
        const float menuPx = std::max(0.0f, params.menuBlur);
        const uint32_t worldW = static_cast<uint32_t>(plan.w), worldH = static_cast<uint32_t>(plan.h);
        const uint32_t W = static_cast<uint32_t>(width), H = static_cast<uint32_t>(height);

        if (plan.bloom)
            RecordBloom(params.bloom);

        const float now = params.timeSeconds;
        const float dt = std::clamp(now - post.lastTime, 1.0f / 480.0f, 0.1f);
        post.lastTime = now;
        float weight = std::pow(0.95f * intensity, dt * 60.0f);
        if (!post.histValid || menuPx > 0.01f)
            weight = 0.0f;
        const int write = 1 - post.read;

        const auto fullscreen = [&](View view, rhi::TextureHandle cur, rhi::TextureHandle hist, float w, float dir, uint32_t outW, uint32_t outH,
                                    rhi::Format fmt) {
            uni.Vec4("u_post", w, kOrigin, menuPx, dir);
            const bool underwater = view == View::BlurOut && packActive && params.cameraMedium == 1;
            uni.Vec4("u_postFx", underwater ? 1.0f : 0.0f, std::fmod(params.timeSeconds, 3600.0f), 0.0f, 0.0f);
            uni.Vec4("u_bloom", plan.bloom && view == View::BlurOut ? params.bloom * 0.55f : 0.0f, 0.0f, 0.0f, 0.0f);
            BindTexture("s_cur", cur, gpu::SamplerKind::LinearClamp);
            BindTexture("s_hist", hist, gpu::SamplerKind::LinearClamp);
            if (plan.bloom)
                BindTexture("s_bloom", post.bloom[0], gpu::SamplerKind::LinearClamp);
            FullscreenPass(view, Program::Post, fmt, outW, outH, false);
            ResetBindings();
        };

        rhi::TextureHandle shown = post.worldColor;
        if (plan.blur) {
            fullscreen(View::Blur, post.worldColor, post.hist[post.read], weight, 0.0f, worldW, worldH, kPostColor);
            shown = post.hist[write];
        }
        const bool filtered = !stages.empty() && std::any_of(stages.begin(), stages.end(), [](const Stage& s) {
            return !s.views.empty() && s.views.front() == View::Up0;
        });
        if (filtered)
            shown = RecordUpscale(shown, worldW, worldH);
        // blur: horizontal pass above, vertical here; without it this is a plain composite with the bloom
        fullscreen(View::BlurOut, shown, shown, 0.0f, 1.0f, W, H, gpu.TargetFormat());
        if (plan.blur) {
            post.read = write;
            post.histValid = true;
        } else {
            post.histValid = false;
        }
    }

    // -- execution --------------------------------------------------------------------------------------------------

    void RenderContext_rhi::ReadyForSampling(rhi::CommandList& list, rhi::TextureHandle except0, rhi::TextureHandle except1) {
        const auto ready = [&](rhi::TextureHandle t) {
            if (t && t != except0 && t != except1 && gpu.StateOf(t) != rhi::State::Undefined)
                gpu.Transition(list, t, rhi::State::ShaderRead);
        };
        for (rhi::TextureHandle t : postTextures)
            ready(t);
        for (const ShadowMap& m : shadow.maps) {
            ready(m.color);
            ready(m.tint);
        }
        ready(shadow.entityColor);
    }

    void RenderContext_rhi::RunStage(rhi::CommandList& list, const Stage& stage) {
        if (stage.kind == Stage::Kind::Copy) {
            if (!stage.copySrc || !stage.copyDst)
                return;
            gpu.Transition(list, stage.copySrc, rhi::State::CopySrc);
            gpu.Transition(list, stage.copyDst, rhi::State::CopyDst);
            rhi::TextureCopy c;
            c.width = stage.copyW;
            c.height = stage.copyH;
            list.CopyTexture(stage.copyDst, stage.copySrc, std::span<const rhi::TextureCopy>(&c, 1));
            gpu.Transition(list, stage.copyDst, rhi::State::ShaderRead);
            return;
        }
        size_t draws = 0;
        for (View v : stage.views)
            draws += views[static_cast<size_t>(v)].draws.size();
        if (!draws && !stage.clearColor && !stage.clearDepth)
            return;
        if (PERLIN_ENV("PERLIN_JAVA_DBG") && stage.toScreen) {
            static int s = 0;
            if (s++ % 60 == 0)
                std::fprintf(stderr, "[javadbg] screen stage draws=%zu\n", draws);
        }
        const Target& target = stage.target;
        ReadyForSampling(list, target.color, target.depth);
        gpu.Transition(list, target.color, rhi::State::ColorTarget);
        if (target.color1)
            gpu.Transition(list, target.color1, rhi::State::ColorTarget);
        if (target.color2)
            gpu.Transition(list, target.color2, rhi::State::ColorTarget);
        if (target.depth)
            gpu.Transition(list, target.depth, rhi::State::DepthWrite);
        rhi::RenderingDesc rd;
        rd.colorCount = target.color2 ? 3 : target.color1 ? 2 : 1;
        rd.colors[0].texture = target.color;
        rd.colors[0].load = stage.clearColor ? rhi::LoadOp::Clear : rhi::LoadOp::Load;
        rd.colors[0].store = rhi::StoreOp::Store;
        rd.colors[0].clear = {stage.clear[0], stage.clear[1], stage.clear[2], stage.clear[3]};
        if (target.color1) {
            rd.colors[1].texture = target.color1;
            rd.colors[1].load = stage.clearColor ? rhi::LoadOp::Clear : rhi::LoadOp::Load;
            rd.colors[1].store = rhi::StoreOp::Store;
            rd.colors[1].clear = {0.0f, 0.0f, 0.0f, 1.0f};
        }
        if (target.color2) {
            rd.colors[2].texture = target.color2;
            rd.colors[2].load = stage.clearColor ? rhi::LoadOp::Clear : rhi::LoadOp::Load;
            rd.colors[2].store = rhi::StoreOp::Store;
            rd.colors[2].clear = {0.0f, 0.0f, 0.0f, 1.0f};
        }
        if (target.depth) {
            rd.depth.texture = target.depth;
            rd.depth.load = stage.clearDepth ? rhi::LoadOp::Clear : rhi::LoadOp::Load;
            rd.depth.store = rhi::StoreOp::Store;
            rd.depth.clearDepth = 1.0f;
        }
        rd.name = "pass";
        list.BeginRendering(rd);
        for (View v : stage.views) {
            ViewData& vd = views[static_cast<size_t>(v)];
            if (vd.draws.empty())
                continue;
            list.SetViewport({static_cast<float>(vd.x), static_cast<float>(vd.y), static_cast<float>(vd.w), static_cast<float>(vd.h), 0.0f, 1.0f});
            list.SetScissor({static_cast<int32_t>(vd.x), static_cast<int32_t>(vd.y), vd.w, vd.h});
            Replay(list, vd.draws);
        }
        list.EndRendering();
    }
}
