#include <chrono>
#include "client/javashaders/Pipeline.hpp"

#include "client/javashaders/GlslangBridge.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <regex>
#include <set>
#include <thread>

namespace perlin::jshader {
    namespace {
        enum VG {
            VClear, VShadow, VShadowComp, VBegin, VSky, VTerrain, VBlock, VEntity, VMisc, VCopy2, VHandSolid, VCopy1,
            VPrepare, VDeferred, VTranslucent, VClouds, VWeather, VHandTrans, VComposite, VFinal, VBackDepth, VCount
        };
        constexpr int kViewCounts[VCount] = {4, 1, 16, 16, 8, 8, 4, 8, 8, 1, 4, 1, 16, 16, 8, 4, 4, 4, 16, 1, 1};

        int ViewOffset(int group) {
            int o = 0;
            for (int i = 0; i < group; i++) o += kViewCounts[i];
            return o;
        }
        int TotalViews() { return ViewOffset(VCount); }

        int GroupForSlot(Slot s) {
            switch (s) {
            case Slot::Shadow: return VShadow;
            case Slot::Begin: return VBegin;
            case Slot::Sky: return VSky;
            case Slot::Terrain: return VTerrain;
            case Slot::Block: return VBlock;
            case Slot::Entity: return VEntity;
            case Slot::Misc: return VMisc;
            case Slot::HandSolid: return VHandSolid;
            case Slot::Deferred: return VDeferred;
            case Slot::Translucent: return VTranslucent;
            case Slot::Clouds: return VClouds;
            case Slot::Weather: return VWeather;
            case Slot::HandTranslucent: return VHandTrans;
            case Slot::Composite: return VComposite;
            case Slot::Final: return VFinal;
            }
            return VMisc;
        }

        bgfx::TextureFormat::Enum ToBgfx(BufferFormat f) {
            using F = bgfx::TextureFormat;
            switch (f) {
            case BufferFormat::RGBA8: return F::RGBA8;
            case BufferFormat::RGBA16: return F::RGBA16;
            case BufferFormat::RGBA16F: return F::RGBA16F;
            case BufferFormat::RGBA32F: return F::RGBA32F;
            case BufferFormat::R8: return F::R8;
            case BufferFormat::RG8: return F::RG8;
            case BufferFormat::R16F: return F::R16F;
            case BufferFormat::RG16F: return F::RG16F;
            case BufferFormat::R32F: return F::R32F;
            case BufferFormat::RGB8: return F::RGBA8;
            case BufferFormat::RGB16F: return F::RGBA16F;
            case BufferFormat::R11F_G11F_B10F: return F::RG11B10F;
            case BufferFormat::RGB10_A2: return F::RGB10A2;
            case BufferFormat::RG16: return F::RG16;
            case BufferFormat::R16: return F::R16;
            case BufferFormat::RGBA8_SNORM: return F::RGBA8S;
            case BufferFormat::RGB16: return F::RGBA16;
            case BufferFormat::RGBA32I: return F::RGBA32I;
            case BufferFormat::RG32F: return F::RG32F;
            }
            return F::RGBA8;
        }

        uint64_t BlendFactor(int gl) {
            switch (gl) {
            case 0: return BGFX_STATE_BLEND_ZERO;
            case 1: return BGFX_STATE_BLEND_ONE;
            case 0x300: return BGFX_STATE_BLEND_SRC_COLOR;
            case 0x301: return BGFX_STATE_BLEND_INV_SRC_COLOR;
            case 0x302: return BGFX_STATE_BLEND_SRC_ALPHA;
            case 0x303: return BGFX_STATE_BLEND_INV_SRC_ALPHA;
            case 0x304: return BGFX_STATE_BLEND_DST_ALPHA;
            case 0x305: return BGFX_STATE_BLEND_INV_DST_ALPHA;
            case 0x306: return BGFX_STATE_BLEND_DST_COLOR;
            case 0x307: return BGFX_STATE_BLEND_INV_DST_COLOR;
            case 0x308: return BGFX_STATE_BLEND_SRC_ALPHA_SAT;
            }
            return BGFX_STATE_BLEND_ONE;
        }

        ExpandedSource MakeSource(const char* text) {
            ExpandedSource s;
            s.text = text;
            const size_t lines = static_cast<size_t>(std::count(s.text.begin(), s.text.end(), '\n')) + 1;
            s.origins.assign(lines, LineOrigin{"builtin", 0});
            s.files.insert("builtin");
            return s;
        }
        const char* kFullscreenVertex =
            "#version 120\nvarying vec2 texcoord;\nvoid main() { gl_Position = ftransform(); texcoord = gl_MultiTexCoord0.xy; }\n";
        const char* kCopyDepthFragment =
            "#version 120\nuniform sampler2D depthtex0;\nvarying vec2 texcoord;\nvoid main() { gl_FragDepth = texture2D(depthtex0, texcoord).r; }\n";
        const char* kCopyColorFragment =
            "#version 120\nuniform sampler2D colortex0;\nvarying vec2 texcoord;\n/* DRAWBUFFERS:0 */\nvoid main() { gl_FragData[0] = texture2D(colortex0, texcoord); }\n";

        struct Vec2 { float x, y; };
    }

    struct Pipeline::Impl {
        Pipeline& o;
        explicit Impl(Pipeline& owner) : o(owner) {}

        class Pool {
        public:
            void Start(int n) {
                for (int i = 0; i < std::max(1, n); i++) threads.emplace_back([this] { Loop(); });
            }
            ~Pool() {
                {
                    std::lock_guard<std::mutex> lock(m);
                    quit = true;
                }
                cv.notify_all();
                for (auto& t : threads) t.join();
            }
            template <class F>
            auto Submit(F fn) {
                using R = decltype(fn());
                auto task = std::make_shared<std::packaged_task<R()>>(std::move(fn));
                auto fut = task->get_future().share();
                {
                    std::lock_guard<std::mutex> lock(m);
                    jobs.emplace_back([task] { (*task)(); });
                }
                cv.notify_one();
                return fut;
            }

        private:
            void Loop() {
                for (;;) {
                    std::function<void()> job;
                    {
                        std::unique_lock<std::mutex> lock(m);
                        cv.wait(lock, [this] { return quit || !jobs.empty(); });
                        if (quit && jobs.empty()) return;
                        job = std::move(jobs.front());
                        jobs.pop_front();
                    }
                    job();
                }
            }
            std::vector<std::thread> threads;
            std::deque<std::function<void()>> jobs;
            std::mutex m;
            std::condition_variable cv;
            bool quit = false;
        };

        struct Converted {
            bool ok = false;
            std::string error;
            ProgramResult transform;
            BackendProgram backend;
        };
        enum class SamplerKind { Dummy, Color, Depth, ShadowDepth, ShadowColor, Noise, Atlas, Lightmap, Normals, Specular, Overlay, Custom };
        struct SamplerBind {
            int stage = 0;
            bgfx::UniformHandle uniform = BGFX_INVALID_HANDLE;
            SamplerKind kind = SamplerKind::Dummy;
            int index = 0;
            bool shadow = false;
            std::string name;
        };
        struct UniformSlot {
            UniformDecl decl;
            bool perDraw = false;
            bool known = false;
        };
        struct Program {
            bgfx::ProgramHandle handle = BGFX_INVALID_HANDLE;
            bgfx::UniformHandle globals = BGFX_INVALID_HANDLE;
            int regs = 0;
            std::vector<SamplerBind> samplers;
            std::vector<UniformSlot> slots;
            std::vector<int> attachments;
            std::shared_ptr<ProgramSource> source;
            std::vector<float> frameRegs, drawRegs;
            int frameStamp = -1;
            PackDrawState lastDraw;
            bool haveDraw = false;
            bool anyPerDraw = false;
            bool valid = false;
            bool debugCopy = false;
            bool logged = false;
        };

        Pool pool;
        std::unordered_map<std::string, std::shared_future<std::shared_ptr<Converted>>> pending;
        std::unordered_map<std::string, std::unique_ptr<Program>> programs;
        std::unordered_map<std::string, bgfx::UniformHandle> samplerUniforms;
        bgfx::UniformHandle globalsUniform = BGFX_INVALID_HANDLE;
        std::set<std::string> loggedMissing;

        struct Buffer {
            bgfx::TextureHandle tex[2] = {BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE};
            int cur = 0;
            bool used = false;
            int w = 0, h = 0;
        };
        std::array<Buffer, kMaxColorTex> bufs;
        bgfx::TextureHandle depth[3] = {BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE};
        bgfx::TextureHandle shadowDepth = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle shadowColor[2] = {BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE};
        bool shadowColorUsed[2] = {false, false};
        bgfx::TextureHandle white = BGFX_INVALID_HANDLE, flatNormal = BGFX_INVALID_HANDLE, blackTex = BGFX_INVALID_HANDLE, noise = BGFX_INVALID_HANDLE;
        std::map<std::string, bgfx::TextureHandle> customTextures;
        std::vector<bgfx::TextureHandle> ownedTextures;
        std::map<std::string, bgfx::FrameBufferHandle> frameBuffers;
        bgfx::TextureFormat::Enum depthFormat = bgfx::TextureFormat::D32F;
        bool shadowUsed = false;
        int width = 0, height = 0;

        UniformTable table;
        CustomUniforms custom;
        FrameState frame;
        int frameStamp = 0;
        std::map<std::pair<int, std::string>, int> viewSlots;
        std::map<int, int> nextSub;
        std::set<int> viewsPrepared;
        bgfx::VertexLayout quadLayout;
        std::string lastError;

        bool Flip() const { return o.host.originTopLeft; }
        bool Trace() const { static const bool on = std::getenv("JSHADER_TRACE") != nullptr; return on; }

        bgfx::TextureHandle MakeTexture(int w, int h, bgfx::TextureFormat::Enum fmt, uint64_t flags, const void* data, uint32_t size, bool mips = false) {
            const bgfx::Memory* mem = data ? bgfx::copy(data, size) : nullptr;
            bgfx::TextureHandle t = bgfx::createTexture2D(static_cast<uint16_t>(w), static_cast<uint16_t>(h), mips, 1, fmt, flags, mem);
            if (bgfx::isValid(t)) ownedTextures.push_back(t);
            return t;
        }

        void CreateStatic() {
            const uint8_t whitePx[4] = {255, 255, 255, 255}, normalPx[4] = {128, 128, 255, 255}, blackPx[4] = {0, 0, 0, 0};
            const uint64_t f = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
            white = MakeTexture(1, 1, bgfx::TextureFormat::RGBA8, f, whitePx, 4);
            flatNormal = MakeTexture(1, 1, bgfx::TextureFormat::RGBA8, f, normalPx, 4);
            blackTex = MakeTexture(1, 1, bgfx::TextureFormat::RGBA8, f, blackPx, 4);
            noise = MakeTexture(1, 1, bgfx::TextureFormat::RGBA8, f, whitePx, 4);
            auto load = [&](const std::string& path) -> bgfx::TextureHandle {
                std::string bytes;
                if (!o.host.decodeImage) return BGFX_INVALID_HANDLE;
                std::string clean = path;
                while (!clean.empty() && clean[0] == '/') clean.erase(0, 1);
                if (!o.pack->Source().Read(clean, bytes)) return BGFX_INVALID_HANDLE;
                int w = 0, h = 0;
                std::vector<uint8_t> rgba;
                if (!o.host.decodeImage(bytes, w, h, rgba) || w <= 0 || h <= 0) return BGFX_INVALID_HANDLE;
                return MakeTexture(w, h, bgfx::TextureFormat::RGBA8, BGFX_SAMPLER_NONE, rgba.data(), static_cast<uint32_t>(rgba.size()));
            };
            if (!o.config.noiseTexture.empty()) {
                const bgfx::TextureHandle t = load(o.config.noiseTexture);
                if (bgfx::isValid(t)) noise = t;
            }
            for (const CustomTexture& ct : o.config.textures) {
                if (customTextures.count(ct.sampler)) continue;
                const bgfx::TextureHandle t = load(ct.path);
                if (bgfx::isValid(t)) customTextures[ct.sampler] = t;
            }
        }

        void ScanUsage() {
            std::set<std::string> names;
            for (const ProgramInfo& p : StandardPrograms()) names.insert(p.name);
            for (const char* base : {"begin", "prepare", "deferred", "composite", "shadowcomp"})
                for (const std::string& n : o.pack->PassChain(base)) names.insert(n);
            names.insert("final");
            std::regex re("(colortex|shadowcolor|gaux)([0-9]+)|\\b(gcolor|gdepth)\\b|\\bshadowtex[01]\\b|\\bshadow\\b|\\bwatershadow\\b|\\bshadowcolor\\b");
            bufs[0].used = true;
            for (const std::string& n : names) {
                if (!o.pack->ProgramExists(n)) continue;
                auto src = o.pack->Program(n);
                if (!src || !src->found) continue;
                for (const ExpandedSource* s : {&src->vertex, &src->fragment}) {
                    for (auto it = std::sregex_iterator(s->text.begin(), s->text.end(), re); it != std::sregex_iterator(); ++it) {
                        const std::smatch& m = *it;
                        const std::string whole = m.str(0);
                        if (m[1].matched) {
                            const int idx = std::atoi(m.str(2).c_str());
                            if (m.str(1) == "colortex" && idx < kMaxColorTex) bufs[static_cast<size_t>(idx)].used = true;
                            else if (m.str(1) == "gaux" && idx >= 1 && idx <= 4) bufs[static_cast<size_t>(3 + idx)].used = true;
                            else if (m.str(1) == "shadowcolor" && idx < 2) { shadowColorUsed[idx] = true; shadowUsed = true; }
                        } else if (whole == "gcolor") bufs[0].used = true;
                        else if (whole == "gdepth") bufs[1].used = true;
                        else if (whole == "shadowcolor") { shadowColorUsed[0] = true; shadowUsed = true; }
                        else shadowUsed = true;
                    }
                }
                for (int b : src->drawBuffers)
                    if (b >= 0 && b < kMaxColorTex) bufs[static_cast<size_t>(b)].used = true;
            }
            if (o.pack->ProgramExists("shadow")) shadowUsed = true;
            o.config.shadowEnabled = shadowUsed;
        }

        void PickDepthFormat() {
            const bgfx::Caps* caps = bgfx::getCaps();
            const uint32_t need = BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER;
            if (caps->formats[bgfx::TextureFormat::D32F] & need) depthFormat = bgfx::TextureFormat::D32F;
            else if (caps->formats[bgfx::TextureFormat::D24S8] & need) depthFormat = bgfx::TextureFormat::D24S8;
            else depthFormat = bgfx::TextureFormat::D16F;
        }

        void CreateTargets(int w, int h) {
            width = w;
            height = h;
            const uint64_t clamp = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
            for (int i = 0; i < kMaxColorTex; i++) {
                Buffer& b = bufs[static_cast<size_t>(i)];
                if (!b.used) continue;
                const BufferSetup& cfg = o.config.color[static_cast<size_t>(i)];
                b.w = cfg.fixedWidth > 0 ? cfg.fixedWidth : std::max(1, static_cast<int>(w * cfg.scaleX));
                b.h = cfg.fixedHeight > 0 ? cfg.fixedHeight : std::max(1, static_cast<int>(h * cfg.scaleY));
                for (int k = 0; k < 2; k++) b.tex[k] = MakeTexture(b.w, b.h, ToBgfx(cfg.format), clamp, nullptr, 0, cfg.mipmap);
                b.cur = 0;
            }
            const uint64_t dflags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT;
            for (int k = 0; k < 3; k++) depth[k] = MakeTexture(w, h, depthFormat, dflags, nullptr, 0);
            if (shadowUsed) {
                const int res = o.config.shadowMapResolution;
                shadowDepth = MakeTexture(res, res, depthFormat, BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, nullptr, 0);
                for (int i = 0; i < 2; i++)
                    if (shadowColorUsed[i]) shadowColor[i] = MakeTexture(res, res, ToBgfx(o.config.shadowColorFormat[i]), clamp, nullptr, 0);
            }
        }

        void DestroyTargets() {
            for (auto& kv : frameBuffers) bgfx::destroy(kv.second);
            frameBuffers.clear();
            std::vector<bgfx::TextureHandle> keep;
            for (bgfx::TextureHandle t : ownedTextures) {
                bool persistent = t.idx == white.idx || t.idx == flatNormal.idx || t.idx == blackTex.idx || t.idx == noise.idx;
                for (auto& kv : customTextures) persistent = persistent || kv.second.idx == t.idx;
                if (persistent) keep.push_back(t);
                else bgfx::destroy(t);
            }
            ownedTextures = keep;
            for (Buffer& b : bufs) b.tex[0] = b.tex[1] = BGFX_INVALID_HANDLE;
            depth[0] = depth[1] = depth[2] = BGFX_INVALID_HANDLE;
            shadowDepth = BGFX_INVALID_HANDLE;
            shadowColor[0] = shadowColor[1] = BGFX_INVALID_HANDLE;
        }

        bgfx::FrameBufferHandle GetFb(const std::vector<bgfx::TextureHandle>& textures) {
            std::string key;
            for (auto t : textures) key += std::to_string(t.idx) + ",";
            const auto it = frameBuffers.find(key);
            if (it != frameBuffers.end()) return it->second;
            std::vector<bgfx::Attachment> att(textures.size());
            for (size_t i = 0; i < textures.size(); i++) {
                bool isDepth = textures[i].idx == shadowDepth.idx;
                for (auto d : depth) isDepth = isDepth || d.idx == textures[i].idx;
                att[i].init(textures[i], bgfx::Access::Write, 0, 1, 0, isDepth ? BGFX_ATTACHMENT_NONE : BGFX_ATTACHMENT_AUTO_GEN_MIPS);
            }
            const bgfx::FrameBufferHandle fb = bgfx::createFrameBuffer(static_cast<uint8_t>(att.size()), att.data(), false);
            if (!bgfx::isValid(fb)) {
                std::fprintf(stderr, "[jshader] framebuffer creation failed (%zu attachments)\n", att.size());
                for (size_t i = 0; i < att.size(); i++) std::fprintf(stderr, "    attachment %zu: texture %u\n", i, textures[i].idx);
                return fb;
            }
            frameBuffers[key] = fb;
            return fb;
        }

        std::string KeyFor(const std::string& name, uint32_t mask, bool toBackbuffer) const {
            return name + "#" + std::to_string(mask) + (toBackbuffer && Flip() ? "B" : "O");
        }

        std::shared_future<std::shared_ptr<Converted>> Schedule(const std::string& key, const std::string& name, uint32_t mask,
                                                                bool toBackbuffer, const ExpandedSource* vs, const ExpandedSource* fs) {
            const auto it = pending.find(key);
            if (it != pending.end()) return it->second;
            ExpandedSource vertex, fragment;
            if (vs) {
                vertex = *vs;
                fragment = *fs;
            } else {
                auto src = o.pack->Program(name);
                if (src) {
                    vertex = src->vertex;
                    fragment = src->fragment;
                }
            }
            TransformOptions opts;
            opts.macros = o.pack->EngineMacros();
            opts.preprocess = PreprocessGlsl;
            opts.availableAttribs = mask;
            const bool topLeft = Flip();
            opts.zeroToOneDepth = o.host.backend != BackendKind::OpenGL;
            opts.flipVertexY = topLeft && !toBackbuffer;
            opts.flipFragCoordY = topLeft && toBackbuffer;
            BackendOptions bo;
            bo.kind = o.host.backend;
            bo.hlslCompiler = o.host.hlslCompiler;
            bo.explicitLod0 = name.compare(0, 9, "gbuffers_") != 0 && name != "shadow";
            auto fut = pool.Submit([vertex = std::move(vertex), fragment = std::move(fragment), opts, bo]() {
                auto c = std::make_shared<Converted>();
                c->transform = TransformProgram(vertex, fragment, opts);
                if (!c->transform.ok()) {
                    c->error = c->transform.vertex.ok ? c->transform.fragment.error : c->transform.vertex.error;
                    return c;
                }
                c->backend = BuildBackendProgram(c->transform, bo);
                c->ok = c->backend.ok;
                if (!c->ok) c->error = c->backend.error;
                return c;
            });
            pending[key] = fut;
            return fut;
        }

        SamplerBind ResolveSampler(const SamplerInfo& s) {
            SamplerBind b;
            b.stage = s.stage;
            b.shadow = s.shadow;
            b.name = s.name;
            auto uit = samplerUniforms.find(s.name);
            if (uit == samplerUniforms.end()) uit = samplerUniforms.emplace(s.name, bgfx::createUniform(s.name.c_str(), bgfx::UniformType::Sampler)).first;
            b.uniform = uit->second;
            const std::string& n = s.name;
            auto numeric = [&](const char* prefix, int& out) {
                const size_t len = std::strlen(prefix);
                if (n.compare(0, len, prefix) != 0 || n.size() == len) return false;
                for (size_t i = len; i < n.size(); i++)
                    if (!std::isdigit(static_cast<unsigned char>(n[i]))) return false;
                out = std::atoi(n.c_str() + len);
                return true;
            };
            int idx = 0;
            if (numeric("colortex", idx) && idx < kMaxColorTex) { b.kind = SamplerKind::Color; b.index = idx; }
            else if (n == "gcolor") { b.kind = SamplerKind::Color; b.index = 0; }
            else if (n == "gdepth") { b.kind = SamplerKind::Color; b.index = 1; }
            else if (n.compare(0, 4, "gaux") == 0 && n.size() == 5 && n[4] >= '1' && n[4] <= '4') { b.kind = SamplerKind::Color; b.index = 3 + (n[4] - '0'); }
            else if (n == "depthtex0" || n == "gdepthtex") { b.kind = SamplerKind::Depth; b.index = 0; }
            else if (n == "depthtex1") { b.kind = SamplerKind::Depth; b.index = 1; }
            else if (n == "depthtex2") { b.kind = SamplerKind::Depth; b.index = 2; }
            else if (n == "shadowtex0" || n == "shadowtex1" || n == "shadow" || n == "watershadow") { b.kind = SamplerKind::ShadowDepth; b.index = 0; }
            else if (n == "shadowcolor0" || n == "shadowcolor") { b.kind = SamplerKind::ShadowColor; b.index = 0; }
            else if (n == "shadowcolor1") { b.kind = SamplerKind::ShadowColor; b.index = 1; }
            else if (n == "noisetex") b.kind = SamplerKind::Noise;
            else if (n == "gtexture" || n == "tex" || n == "texture" || n == "iris_texture") b.kind = SamplerKind::Atlas;
            else if (n == "lightmap" || n == "iris_lightmap") b.kind = SamplerKind::Lightmap;
            else if (n == "normals") b.kind = SamplerKind::Normals;
            else if (n == "specular") b.kind = SamplerKind::Specular;
            else if (n == "iris_overlay") b.kind = SamplerKind::Overlay;
            else if (customTextures.count(n)) b.kind = SamplerKind::Custom;
            else b.kind = SamplerKind::Dummy;
            return b;
        }

        Program* Finish(const std::string& key, const std::string& name, const Converted& c, std::shared_ptr<ProgramSource> source) {
            auto p = std::make_unique<Program>();
            p->source = std::move(source);
            const BackendProgram& bp = c.backend;
            const bgfx::ShaderHandle vs = bgfx::createShader(bgfx::copy(bp.vertex.data(), static_cast<uint32_t>(bp.vertex.size())));
            const bgfx::ShaderHandle fs = bgfx::createShader(bgfx::copy(bp.fragment.data(), static_cast<uint32_t>(bp.fragment.size())));
            p->handle = bgfx::createProgram(vs, fs, true);
            p->valid = bgfx::isValid(p->handle);
            p->regs = bp.uniformRegs;
            p->globals = globalsUniform;
            for (const SamplerInfo& s : bp.samplers) p->samplers.push_back(ResolveSampler(s));
            for (const UniformDecl& d : c.transform.uniforms) {
                UniformSlot slot;
                slot.decl = d;
                slot.known = table.Known(d.name);
                static const std::set<std::string> perDraw = {"entityColor", "entityId", "blockEntityId", "currentRenderedItemId", "renderStage"};
                slot.perDraw = d.name.compare(0, 5, "iris_") == 0 || perDraw.count(d.name) != 0;
                if (!slot.known && loggedMissing.insert(name + ":" + d.name).second) {
                    std::fprintf(stderr, "[jshader] %s: unknown uniform '%s' (left at 0)\n", name.c_str(), d.name.c_str());
                }
                p->anyPerDraw = p->anyPerDraw || slot.perDraw;
                p->slots.push_back(std::move(slot));
            }
            int maxLoc = -1;
            for (int l : c.transform.fragment.drawBuffers) maxLoc = std::max(maxLoc, l);
            const std::vector<int>& tag = p->source ? p->source->drawBuffers : std::vector<int>();
            for (int l = 0; l <= maxLoc; l++) p->attachments.push_back(l < static_cast<int>(tag.size()) ? tag[static_cast<size_t>(l)] : l);
            p->frameRegs.assign(static_cast<size_t>(std::max(1, p->regs)) * 4, 0.0f);
            p->drawRegs = p->frameRegs;
            Program* raw = p.get();
            programs[key] = std::move(p);
            return raw;
        }

        Program* GetProgram(const std::string& name, uint32_t mask, bool toBackbuffer, const ExpandedSource* vs = nullptr, const ExpandedSource* fs = nullptr) {
            const std::string key = KeyFor(name, mask, toBackbuffer);
            const auto ready = programs.find(key);
            if (ready != programs.end()) return ready->second->valid ? ready->second.get() : nullptr;
            std::shared_ptr<ProgramSource> source;
            if (!vs) {
                source = o.pack->Program(name);
                if (!source || !source->found) {
                    programs[key] = std::make_unique<Program>();
                    return nullptr;
                }
            }
            auto fut = Schedule(key, name, mask, toBackbuffer, vs, fs);
            if (fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return nullptr;
            std::shared_ptr<Converted> c = fut.get();
            if (!c->ok) {
                std::fprintf(stderr, "[jshader] program %s failed: %.1500s\n", name.c_str(), c->error.c_str());
                lastError = c->error;
                programs[key] = std::make_unique<Program>();
                return nullptr;
            }
            Program* p = Finish(key, name, *c, source);
            if (!p->valid) {
                std::fprintf(stderr, "[jshader] program %s could not be linked by the renderer\n", name.c_str());
                return nullptr;
            }
            if (!vs) {
            }
            return p;
        }

        void FillUniforms(Program& p, const PackDrawState& draw, bool perDrawOnly) {
            UniformTable::Context ctx;
            ctx.frame = &frame;
            ctx.draw = &draw;
            float natural[96];
            for (const UniformSlot& s : p.slots) {
                if (perDrawOnly && !s.perDraw) continue;
                std::memset(natural, 0, sizeof(natural));
                int comps = 0;
                if (s.known) table.Evaluate(s.decl.name, ctx, natural, comps);
                float* dst = (perDrawOnly ? p.drawRegs : p.frameRegs).data() + s.decl.reg * 4;
                PackUniform(s.decl, natural, dst);
            }
        }

        void BindProgram(Program& p, const PackDrawState& draw) {
            if (p.frameStamp != frameStamp) {
                p.frameStamp = frameStamp;
                p.haveDraw = false;
                FillUniforms(p, PackDrawState(), false);
                p.drawRegs = p.frameRegs;
            }
            if (p.anyPerDraw && (!p.haveDraw || std::memcmp(&p.lastDraw, &draw, sizeof(PackDrawState)) != 0)) {
                FillUniforms(p, draw, true);
                p.lastDraw = draw;
                p.haveDraw = true;
            }
            bgfx::setUniform(p.globals, p.drawRegs.data(), static_cast<uint16_t>(std::max(1, p.regs)));
            for (const SamplerBind& s : p.samplers) {
                bgfx::TextureHandle t = white;
                uint32_t flags = UINT32_MAX;
                const uint32_t clamp = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
                switch (s.kind) {
                case SamplerKind::Color: {
                    int bi = s.index;
                    if (p.debugCopy) { static const char* dbg = std::getenv("JSHADER_DEBUG_BUFFER"); if (dbg) bi = std::atoi(dbg) % kMaxColorTex; }
                    const Buffer& b = bufs[static_cast<size_t>(bi)];
                    if (b.used) t = b.tex[b.cur];
                    else t = blackTex;
                    const BufferSetup& cfg = o.config.color[static_cast<size_t>(s.index)];
                    flags = clamp | (cfg.linear ? 0u : (BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT));
                    break;
                }
                case SamplerKind::Depth:
                    t = depth[s.index];
                    flags = clamp | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT;
                    break;
                case SamplerKind::ShadowDepth:
                    t = bgfx::isValid(shadowDepth) ? shadowDepth : white;
                    if (s.shadow) flags = clamp | BGFX_SAMPLER_COMPARE_LEQUAL;
                    else flags = clamp | (o.config.shadowFilterLinear[0] ? 0u : (BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT));
                    break;
                case SamplerKind::ShadowColor:
                    t = bgfx::isValid(shadowColor[s.index]) ? shadowColor[s.index] : white;
                    flags = clamp | (o.config.shadowColorLinear[s.index] ? 0u : (BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT));
                    break;
                case SamplerKind::Noise: t = noise; flags = BGFX_SAMPLER_NONE; break;
                case SamplerKind::Atlas: if (bgfx::isValid(o.engine.blockAtlas)) t = o.engine.blockAtlas; break;
                case SamplerKind::Lightmap: if (bgfx::isValid(o.engine.lightmap)) t = o.engine.lightmap; break;
                case SamplerKind::Normals: t = bgfx::isValid(o.engine.normals) ? o.engine.normals : flatNormal; break;
                case SamplerKind::Specular: t = bgfx::isValid(o.engine.specular) ? o.engine.specular : blackTex; break;
                case SamplerKind::Overlay: t = bgfx::isValid(o.engine.overlay) ? o.engine.overlay : blackTex; break;
                case SamplerKind::Custom: t = customTextures[s.name]; flags = BGFX_SAMPLER_NONE; break;
                case SamplerKind::Dummy: break;
                }
                if (!bgfx::isValid(t)) t = white;
                if (Trace() && p.frameStamp == frameStamp && !p.logged) std::fprintf(stderr, "[jshader]   sampler %s stage=%d kind=%d index=%d tex=%u\n", s.name.c_str(), s.stage, static_cast<int>(s.kind), s.index, t.idx);
                bgfx::setTexture(static_cast<uint8_t>(s.stage), s.uniform, t, flags);
            }
            p.logged = true;
        }

        bgfx::ViewId ViewId(int group, int sub) const { return static_cast<bgfx::ViewId>(o.host.firstView + ViewOffset(group) + sub); }

        bgfx::ViewId ViewFor(int group, const std::string& fbKey) {
            const auto key = std::make_pair(group, fbKey);
            auto it = viewSlots.find(key);
            if (it == viewSlots.end()) {
                int& next = nextSub[group];
                const int sub = std::min(next, kViewCounts[group] - 1);
                next++;
                it = viewSlots.emplace(key, sub).first;
            }
            return ViewId(group, it->second);
        }

        void SetupView(bgfx::ViewId view, bgfx::FrameBufferHandle fb, int w, int h) {
            if (!viewsPrepared.insert(view).second) return;
            bgfx::setViewFrameBuffer(view, fb);
            bgfx::setViewRect(view, 0, 0, static_cast<uint16_t>(w), static_cast<uint16_t>(h));
            bgfx::setViewMode(view, bgfx::ViewMode::Sequential);
            bgfx::setViewClear(view, BGFX_CLEAR_NONE);
            bgfx::touch(view);
        }

        std::vector<bgfx::TextureHandle> SceneAttachments(const std::vector<int>& att, bool withDepth) {
            std::vector<bgfx::TextureHandle> out;
            for (int idx : att) {
                if (idx < 0 || idx >= kMaxColorTex) idx = 0;
                Buffer& b = bufs[static_cast<size_t>(idx)];
                if (!b.used) b.used = true;
                out.push_back(bgfx::isValid(b.tex[b.cur]) ? b.tex[b.cur] : bufs[0].tex[bufs[0].cur]);
            }
            if (withDepth) out.push_back(depth[0]);
            return out;
        }

        PackDrawState FullscreenDraw(int w, int h) {
            PackDrawState d;
            const float mv[16] = {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, -1, -1, 0, 1};
            std::memcpy(d.modelView, mv, sizeof(mv));
            d.viewHeight = h;
            (void)w;
            return d;
        }

        bool SubmitQuad(bgfx::ViewId view, Program& p, const PackDrawState& draw, uint64_t state) {
            bgfx::TransientVertexBuffer tvb;
            if (bgfx::getAvailTransientVertexBuffer(6, quadLayout) < 6) return false;
            bgfx::allocTransientVertexBuffer(&tvb, 6, quadLayout);
            struct V { float x, y, z, u, v; };
            auto* v = reinterpret_cast<V*>(tvb.data);
            const V quad[6] = {{0, 0, 0, 0, 0}, {1, 0, 0, 1, 0}, {1, 1, 0, 1, 1}, {0, 0, 0, 0, 0}, {1, 1, 0, 1, 1}, {0, 1, 0, 0, 1}};
            std::memcpy(v, quad, sizeof(quad));
            bgfx::setVertexBuffer(0, &tvb);
            bgfx::setState(state);
            BindProgram(p, draw);
            bgfx::submit(view, p.handle);
            return true;
        }

        uint32_t FullscreenMask() const { return AttribBit(kAttribPosition) | AttribBit(kAttribTexCoord0); }

        void RunDepthCopy(bgfx::ViewId view, bgfx::TextureHandle dst) {
            static const ExpandedSource vs = MakeSource(kFullscreenVertex), fs = MakeSource(kCopyDepthFragment);
            Program* p = GetProgram("builtin_copy_depth", FullscreenMask(), false, &vs, &fs);
            if (!p) return;
            const bgfx::FrameBufferHandle fb = GetFb({dst});
            SetupView(view, fb, width, height);
            SubmitQuad(view, *p, FullscreenDraw(width, height), BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_ALWAYS);
        }

        void RunDepthToBackbuffer(bgfx::ViewId view, bgfx::FrameBufferHandle backbuffer, int w, int h) {
            static const ExpandedSource vs = MakeSource(kFullscreenVertex), fs = MakeSource(kCopyDepthFragment);
            Program* p = GetProgram("builtin_copy_depth", FullscreenMask(), true, &vs, &fs);
            if (!p) return;
            SetupView(view, backbuffer, w, h);
            SubmitQuad(view, *p, FullscreenDraw(w, h), BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_ALWAYS);
        }

        bool RunPass(bgfx::ViewId view, const std::string& name, bool toBackbuffer, bgfx::FrameBufferHandle backbuffer, int outW, int outH) {
            Program* p = GetProgram(name, FullscreenMask(), toBackbuffer);
            if (!p) return false;
            const ProgramConfig* pc = o.config.Find(name);
            float scale = pc && pc->hasScale ? pc->scale : 1.0f;
            int rw = width, rh = height;
            std::vector<bgfx::TextureHandle> targets;
            bgfx::FrameBufferHandle fb = backbuffer;
            if (toBackbuffer) {
                rw = outW;
                rh = outH;
            } else {
                for (int idx : p->attachments) {
                    if (idx < 0 || idx >= kMaxColorTex) idx = 0;
                    Buffer& b = bufs[static_cast<size_t>(idx)];
                    targets.push_back(b.tex[1 - b.cur]);
                }
                if (targets.empty()) return true;
                fb = GetFb(targets);
                const Buffer& b0 = bufs[static_cast<size_t>(std::max(0, std::min(p->attachments[0], kMaxColorTex - 1)))];
                rw = b0.w;
                rh = b0.h;
            }
            const int vw = std::max(1, static_cast<int>(rw * scale)), vh = std::max(1, static_cast<int>(rh * scale));
            SetupView(view, fb, vw, vh);
            PackDrawState d = FullscreenDraw(rw, rh);
            d.viewHeight = rh;
            uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;
            if (pc && pc->blend.set && !pc->blend.off)
                state |= BGFX_STATE_BLEND_FUNC_SEPARATE(BlendFactor(pc->blend.src), BlendFactor(pc->blend.dst), BlendFactor(pc->blend.srcA), BlendFactor(pc->blend.dstA));
            if (!SubmitQuad(view, *p, d, state)) return false;
            o.passesRun++;
            if (Trace()) {
                std::fprintf(stderr, "[jshader] pass %s view=%u viewport=%dx%d targets:", name.c_str(), view, vw, vh);
                for (int a : p->attachments) std::fprintf(stderr, " %d", a);
                std::fprintf(stderr, "\n");
            }
            for (int idx : p->attachments) {
                if (idx < 0 || idx >= kMaxColorTex) continue;
                Buffer& b = bufs[static_cast<size_t>(idx)];
                b.cur = 1 - b.cur;
            }
            return true;
        }

        void RunChain(const char* base, int group) {
            int sub = 0;
            for (const std::string& name : o.pack->PassChain(base)) {
                if (!o.pack->ProgramEnabled(name)) continue;
                const bgfx::ViewId view = ViewId(group, std::min(sub, kViewCounts[group] - 1));
                if (RunPass(view, name, false, BGFX_INVALID_HANDLE, 0, 0)) sub++;
            }
        }
    };

    Pipeline::Pipeline(std::shared_ptr<ShaderPackInstance> packInstance, const PipelineHost& hostInfo)
        : impl(std::make_unique<Impl>(*this)), pack(std::move(packInstance)), host(hostInfo) {
        Impl& m = *impl;
        m.pool.Start(host.threads);
        config = ParsePipelineConfig(*pack);
        m.custom.Load(config.customUniformProps);
        m.table.SetCustom(&m.custom);
        m.quadLayout = FullscreenLayout();
        m.PickDepthFormat();
        m.globalsUniform = bgfx::createUniform("IrisGlobals", bgfx::UniformType::Vec4, 256);
        m.ScanUsage();
        m.CreateStatic();
        m.CreateTargets(host.width, host.height);
        usable = true;
    }

    Pipeline::~Pipeline() {
        Impl& m = *impl;
        for (auto& kv : m.pending) kv.second.wait();
        for (auto& kv : m.programs)
            if (kv.second->valid) bgfx::destroy(kv.second->handle);
        for (auto& kv : m.samplerUniforms) bgfx::destroy(kv.second);
        if (bgfx::isValid(m.globalsUniform)) bgfx::destroy(m.globalsUniform);
        for (auto& kv : m.frameBuffers) bgfx::destroy(kv.second);
        for (bgfx::TextureHandle t : m.ownedTextures) bgfx::destroy(t);
    }

    bgfx::VertexLayout Pipeline::FullscreenLayout() {
        bgfx::VertexLayout l;
        l.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float).end();
        return l;
    }

    bool Pipeline::ProgramUsable(const std::string& name, uint32_t mask, bool toBackbuffer) {
        Impl& m = *impl;
        const auto it = m.programs.find(m.KeyFor(name, mask, toBackbuffer));
        return it != m.programs.end() && it->second->valid;
    }

    int Pipeline::WarmUp(uint32_t terrainMask) {
        Impl& m = *impl;
        int remaining = 0, started = 0;
        const auto touch = [&](const std::string& name, uint32_t mask, bool toBackbuffer) {
            const std::string key = m.KeyFor(name, mask, toBackbuffer);
            if (m.programs.count(key)) return;
            if (!m.pending.count(key)) {
                if (started >= 2) { remaining++; return; }
                started++;
            }
            m.GetProgram(name, mask, toBackbuffer);
            if (!m.programs.count(key)) remaining++;
        };
        for (const char* name : {"gbuffers_terrain", "gbuffers_water", "shadow"}) touch(name, terrainMask, false);
        for (const char* base : {"begin", "prepare", "deferred", "composite", "shadowcomp"})
            for (const std::string& name : pack->PassChain(base))
                if (pack->ProgramEnabled(name)) touch(name, m.FullscreenMask(), false);
        touch("final", m.FullscreenMask(), true);
        return remaining;
    }

    uint16_t Pipeline::ViewCount() const { return static_cast<uint16_t>(TotalViews()); }

    void Pipeline::AppendViewOrder(std::vector<bgfx::ViewId>& out) const {
        for (int i = 0; i < TotalViews(); i++) out.push_back(static_cast<bgfx::ViewId>(host.firstView + i));
    }

    bgfx::ViewId Pipeline::ShadowView() const { return static_cast<bgfx::ViewId>(host.firstView + ViewOffset(VShadow)); }

    void Pipeline::Resize(int w, int h) {
        if (w == host.width && h == host.height) return;
        host.width = w;
        host.height = h;
        impl->DestroyTargets();
        impl->CreateTargets(w, h);
    }

    bgfx::TextureHandle Pipeline::SceneColor() const {
        const Impl::Buffer& b = impl->bufs[0];
        return b.tex[b.cur];
    }

    bool Pipeline::IsProgramAvailable(const std::string& program) const { return pack->ProgramExists(program); }

    void Pipeline::Prefetch(const std::string& program, uint32_t attribMask) {
        impl->Schedule(impl->KeyFor(program, attribMask, false), program, attribMask, false, nullptr, nullptr);
    }

    void Pipeline::BeginFrame(const FrameState& state) {
        Impl& m = *impl;
        m.frame = state;
        passesRun = 0;
        finalOk = false;
        m.frame.viewWidth = host.width;
        m.frame.viewHeight = host.height;
        m.frame.dimensionId = pack->Dimension() == "world-1" ? -1 : (pack->Dimension() == "world1" ? 1 : 0);
        m.frameStamp++;
        m.viewsPrepared.clear();
        m.nextSub.clear();
        m.viewSlots.clear();
        m.custom.Evaluate(m.frame, m.table);

        std::vector<int> used;
        for (int i = 0; i < kMaxColorTex; i++)
            if (m.bufs[static_cast<size_t>(i)].used) used.push_back(i);
        for (size_t g = 0, view = 0; g < used.size() || g == 0; g += 7, view++) {
            std::vector<bgfx::TextureHandle> tex;
            uint8_t palette[8] = {UINT8_MAX, UINT8_MAX, UINT8_MAX, UINT8_MAX, UINT8_MAX, UINT8_MAX, UINT8_MAX, UINT8_MAX};
            for (size_t k = g; k < used.size() && k < g + 7; k++) {
                const int idx = used[k];
                Impl::Buffer& b = m.bufs[static_cast<size_t>(idx)];
                const BufferSetup& cfg = config.color[static_cast<size_t>(idx)];
                tex.push_back(b.tex[b.cur]);
                if (cfg.clear) {
                    float c[4] = {cfg.clearColor[0], cfg.clearColor[1], cfg.clearColor[2], cfg.clearColor[3]};
                    if (idx == 0 && pack->GetDirectives().Text("colortex0ClearColor").empty()) { c[0] = state.fogColor[0]; c[1] = state.fogColor[1]; c[2] = state.fogColor[2]; c[3] = 1.0f; }
                    if (idx == 1 && pack->GetDirectives().Text("colortex1ClearColor").empty()) { c[0] = c[1] = c[2] = c[3] = 1.0f; }
                    bgfx::setPaletteColor(static_cast<uint8_t>(idx), c);
                    palette[tex.size() - 1] = static_cast<uint8_t>(idx);
                }
            }
            const bool first = g == 0;
            if (first) tex.push_back(m.depth[0]);
            if (tex.empty()) break;
            const bgfx::ViewId v = m.ViewId(VClear, std::min<int>(static_cast<int>(view), kViewCounts[VClear] - 1));
            m.SetupView(v, m.GetFb(tex), host.width, host.height);
            uint16_t flags = BGFX_CLEAR_COLOR;
            if (first) flags |= BGFX_CLEAR_DEPTH;
            bgfx::setViewClear(v, flags, 1.0f, 0, palette[0], palette[1], palette[2], palette[3], palette[4], palette[5], palette[6], palette[7]);
            if (used.empty()) break;
        }

        if (m.shadowUsed && bgfx::isValid(m.shadowDepth)) {
            std::vector<bgfx::TextureHandle> tex;
            for (int i = 0; i < 2; i++)
                if (bgfx::isValid(m.shadowColor[i])) tex.push_back(m.shadowColor[i]);
            tex.push_back(m.shadowDepth);
            const bgfx::ViewId v = ShadowView();
            m.SetupView(v, m.GetFb(tex), config.shadowMapResolution, config.shadowMapResolution);
            bgfx::setViewClear(v, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x00000000, 1.0f, 0);
        }

        m.RunChain("begin", VBegin);
    }

    DrawResult Pipeline::Prepare(Slot slot, const std::string& program, uint32_t attribMask, const PackDrawState& draw, uint64_t baseState) {
        Impl& m = *impl;
        DrawResult r;
        Impl::Program* p = m.GetProgram(program, attribMask, false);
        if (!p) return r;
        std::vector<bgfx::TextureHandle> tex;
        int w = host.width, h = host.height;
        std::string fbKey;
        if (slot == Slot::Shadow) {
            for (int i = 0; i < 2; i++)
                if (bgfx::isValid(m.shadowColor[i])) tex.push_back(m.shadowColor[i]);
            tex.push_back(m.shadowDepth);
            w = h = config.shadowMapResolution;
            r.view = ShadowView();
        } else {
            tex = m.SceneAttachments(p->attachments, true);
            for (auto t : tex) fbKey += std::to_string(t.idx) + ",";
            r.view = m.ViewFor(GroupForSlot(slot), fbKey);
            m.SetupView(r.view, m.GetFb(tex), w, h);
        }
        r.program = p->handle;
        r.flipWinding = host.originTopLeft;
        uint64_t state = baseState;
        const ProgramConfig* pc = config.Find(program);
        if (pc && pc->blend.set) {
            state &= ~BGFX_STATE_BLEND_MASK;
            if (!pc->blend.off)
                state |= BGFX_STATE_BLEND_FUNC_SEPARATE(BlendFactor(pc->blend.src), BlendFactor(pc->blend.dst), BlendFactor(pc->blend.srcA), BlendFactor(pc->blend.dstA));
        }
        r.state = state;
        if (m.Trace()) std::fprintf(stderr, "[jshader] draw %s view=%u att=%zu\n", program.c_str(), r.view, p->attachments.size());
        m.BindProgram(*p, draw);
        r.ok = true;
        return r;
    }

    void Pipeline::RunPostPasses(bgfx::FrameBufferHandle backbuffer, int outW, int outH, const bgfx::VertexLayout& quadLayout, bool depthToBackbuffer) {
        Impl& m = *impl;
        m.quadLayout = quadLayout;
        m.RunDepthCopy(m.ViewId(VCopy2, 0), m.depth[2]);
        m.RunDepthCopy(m.ViewId(VCopy1, 0), m.depth[1]);
        m.RunChain("shadowcomp", VShadowComp);
        m.RunChain("prepare", VPrepare);
        m.RunChain("deferred", VDeferred);
        m.RunChain("composite", VComposite);
        const bgfx::ViewId finalView = m.ViewId(VFinal, 0);
        bool done = false;
        if (std::getenv("JSHADER_DEBUG_BUFFER")) {
        } else if (pack->ProgramExists("final")) done = m.RunPass(finalView, "final", true, backbuffer, outW, outH);
        if (!done) {
            static const ExpandedSource vs = MakeSource(kFullscreenVertex), fs = MakeSource(kCopyColorFragment);
            Impl::Program* p = m.GetProgram("builtin_copy_color", m.FullscreenMask(), true, &vs, &fs);
            if (p) {
                p->debugCopy = true;
                m.SetupView(finalView, backbuffer, outW, outH);
                PackDrawState d = m.FullscreenDraw(outW, outH);
                m.SubmitQuad(finalView, *p, d, BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
            }
        }
        finalOk = done;
        if (!done) {
            finalOk = passesRun > 0;
        }
        if (depthToBackbuffer) m.RunDepthToBackbuffer(m.ViewId(VBackDepth, 0), backbuffer, outW, outH);
    }
}
