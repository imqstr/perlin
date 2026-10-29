#include "client/renderer/bgfx/JavaPackHost.hpp"

#include "client/javashaders/GlslangBridge.hpp"
#include "client/renderer/block/BlockLibrary.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace perlin {
    namespace {
        constexpr double kPi = 3.14159265358979323846;

        glm::mat4 ToMat(const float* m) {
            glm::mat4 out;
            std::memcpy(&out[0][0], m, sizeof(float) * 16);
            return out;
        }
        void FromMat(const glm::mat4& m, float* out) { std::memcpy(out, &m[0][0], sizeof(float) * 16); }

        void ToGlDepth(const float* in, float* out) {
            std::memcpy(out, in, sizeof(float) * 16);
            for (int c = 0; c < 4; c++) out[c * 4 + 2] = 2.0f * in[c * 4 + 2] - in[c * 4 + 3];
        }

        std::string Normalize(std::string n) {
            if (n.compare(0, 10, "minecraft:") == 0) n.erase(0, 10);
            return n;
        }

        std::string BaseName(const std::string& entry) {
            std::vector<std::string> parts;
            size_t start = 0;
            for (;;) {
                const size_t colon = entry.find(':', start);
                parts.push_back(entry.substr(start, colon == std::string::npos ? std::string::npos : colon - start));
                if (colon == std::string::npos) break;
                start = colon + 1;
            }
            std::vector<std::string> kept;
            for (const std::string& p : parts)
                if (p.find('=') == std::string::npos) kept.push_back(p);
            if (kept.empty()) return {};
            if (kept.size() >= 2) return kept[1];
            return kept[0];
        }

        const char* Alias(const std::string& n) {
            static const std::pair<const char*, const char*> table[] = {
                {"flowing_water", "water"}, {"flowing_lava", "lava"}, {"tallgrass", "short_grass"}, {"grass", "grass_block"},
                {"web", "cobweb"}, {"yellow_flower", "dandelion"}, {"red_flower", "poppy"}, {"reeds", "sugar_cane"},
                {"deadbush", "dead_bush"}, {"waterlily", "lily_pad"}, {"snow_layer", "snow"}, {"lit_furnace", "furnace"},
                {"lit_redstone_lamp", "redstone_lamp"}, {"lit_redstone_ore", "redstone_ore"}, {"portal", "nether_portal"},
                {"sea_lantern", "sea_lantern"}, {"mob_spawner", "spawner"}, {"stonebrick", "stone_bricks"},
                {"wooden_slab", "oak_slab"}, {"double_plant", "tall_grass"}, {"concretePowder", "white_concrete_powder"},
                {"leaves", "oak_leaves"}, {"leaves2", "acacia_leaves"}, {"log", "oak_log"}, {"log2", "acacia_log"}};
            for (const auto& e : table)
                if (n == e.first) return e.second;
            return nullptr;
        }

        void BoxDown(const uint8_t* src, int sw, int sh, int sstride, uint8_t* dst, int dstride) {
            const int dw = std::max(1, sw / 2), dh = std::max(1, sh / 2);
            for (int y = 0; y < dh; y++)
                for (int x = 0; x < dw; x++) {
                    int sum[4] = {0, 0, 0, 0}, alphaSum = 0;
                    for (int j = 0; j < 2; j++)
                        for (int i = 0; i < 2; i++) {
                            const int sx = std::min(sw - 1, x * 2 + i), sy = std::min(sh - 1, y * 2 + j);
                            const uint8_t* p = src + static_cast<size_t>(sy) * sstride + static_cast<size_t>(sx) * 4;
                            sum[0] += p[0] * p[3];
                            sum[1] += p[1] * p[3];
                            sum[2] += p[2] * p[3];
                            sum[3] += p[3];
                            alphaSum += p[3];
                        }
                    uint8_t* d = dst + static_cast<size_t>(y) * dstride + static_cast<size_t>(x) * 4;
                    if (alphaSum > 0) {
                        d[0] = static_cast<uint8_t>(sum[0] / alphaSum);
                        d[1] = static_cast<uint8_t>(sum[1] / alphaSum);
                        d[2] = static_cast<uint8_t>(sum[2] / alphaSum);
                    } else {
                        d[0] = d[1] = d[2] = 0;
                    }
                    d[3] = static_cast<uint8_t>(sum[3] / 4);
                }
        }
    }

    JavaPackHost::JavaPackHost() = default;

    JavaPackHost::~JavaPackHost() {
        pipeline.reset();
        if (bgfx::isValid(atlas)) bgfx::destroy(atlas);
    }

    const bgfx::VertexLayout& JavaPackHost::Layout() {
        static const bgfx::VertexLayout layout = [] {
            bgfx::VertexLayout l;
            l.begin()
                .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
                .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
                .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
                .add(bgfx::Attrib::TexCoord5, 2, bgfx::AttribType::Float)
                .add(bgfx::Attrib::Normal, 4, bgfx::AttribType::Int8, true)
                .add(bgfx::Attrib::TexCoord2, 2, bgfx::AttribType::Float)
                .add(bgfx::Attrib::TexCoord3, 2, bgfx::AttribType::Float)
                .add(bgfx::Attrib::TexCoord4, 4, bgfx::AttribType::Int8, false)
                .add(bgfx::Attrib::Tangent, 4, bgfx::AttribType::Int8, true)
                .end();
            return l;
        }();
        return layout;
    }

    bool JavaPackHost::Load(const LoadConfig& config, std::string& error) {
        pipeline.reset();
        instance.reset();
        auto source = jshader::OpenPack(config.path);
        if (!source) {
            error = "could not open shader pack '" + config.path + "'";
            return false;
        }
        instance = std::make_shared<jshader::ShaderPackInstance>(std::shared_ptr<jshader::PackSource>(std::move(source)), config.options,
                                                                  jshader::EngineMacros(260300, config.backend == jshader::BackendKind::OpenGL));
        instance->SetDimension(config.dimension);
        name = instance->Name().empty() ? config.path : instance->Name();
        originTopLeft = config.originTopLeft;
        blockName = config.blockName;
        jshader::PipelineHost host;
        host.backend = config.backend;
        host.hlslCompiler = config.hlsl;
        host.width = config.width;
        host.height = config.height;
        host.originTopLeft = config.originTopLeft;
        host.homogeneousDepth = true;
        host.firstView = 64;
        host.decodeImage = [](const std::string& bytes, int& w, int& h, std::vector<uint8_t>& rgba) {
            int n = 0;
            unsigned char* data = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()), &w, &h, &n, 4);
            if (!data) return false;
            rgba.assign(data, data + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
            stbi_image_free(data);
            return true;
        };
        pipeline = std::make_unique<jshader::Pipeline>(instance, host);
        if (!pipeline->Usable()) {
            error = pipeline->Error();
            pipeline.reset();
            return false;
        }
        BuildBlockMap();
        havePrev = false;
        return true;
    }

    void JavaPackHost::SetDimension(const std::string& folder) {
        if (instance && instance->Dimension() != folder) instance->SetDimension(folder);
    }

    void JavaPackHost::Resize(int w, int h) {
        if (pipeline) pipeline->Resize(w, h);
    }

    void JavaPackHost::BuildBlockMap() {
        nameToPackId.clear();
        paletteMap.assign(65536, -2);
        if (!instance) return;
        std::unordered_map<std::string, bool> plain;
        for (const auto& kv : instance->BlockIds())
            for (const std::string& entry : kv.second) {
                const std::string base = BaseName(entry);
                if (base.empty()) continue;
                const bool isPlain = entry.find('=') == std::string::npos;
                auto it = nameToPackId.find(base);
                if (it == nameToPackId.end() || (isPlain && !plain[base])) {
                    nameToPackId[base] = kv.first;
                    plain[base] = isPlain;
                }
            }
    }

    int JavaPackHost::PackBlockId(uint16_t palette) {
        if (paletteMap.empty()) return -1;
        int16_t& slot = paletteMap[palette];
        if (slot != -2) return slot;
        int result = -1;
        if (blockName) {
            std::string n = Normalize(blockName(palette));
            auto it = nameToPackId.find(n);
            if (it == nameToPackId.end())
                if (const char* alias = Alias(n)) it = nameToPackId.find(alias);
            if (it != nameToPackId.end()) result = it->second;
        }
        slot = static_cast<int16_t>(std::clamp(result, -1, 32000));
        return slot;
    }

    void JavaPackHost::SetAtlas(const uint8_t* rgba, int tilePx, int tiles) {
        if (!rgba || tilePx <= 0 || tiles <= 0) return;
        if (bgfx::isValid(atlas)) bgfx::destroy(atlas);
        atlas = BGFX_INVALID_HANDLE;
        atlasTilePx = tilePx;
        atlasTiles = tiles;
        const int maxTex = static_cast<int>(bgfx::getCaps()->limits.maxTextureSize);
        gridCols = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(tiles)))));
        gridCols = std::min(gridCols, std::max(1, maxTex / tilePx));
        gridRows = (tiles + gridCols - 1) / gridCols;
        if (gridRows * tilePx > maxTex) {
            std::fprintf(stderr, "[javapack] atlas of %d tiles (%dpx) does not fit a %d texture\n", tiles, tilePx, maxTex);
            gridRows = std::max(1, maxTex / tilePx);
            atlasTiles = gridRows * gridCols;
        }
        const int W = gridCols * tilePx, H = gridRows * tilePx;
        atlasWidth = W;
        int tileLevels = 0;
        while ((tilePx >> (tileLevels + 1)) >= 2 && tileLevels < 4) tileLevels++;
        atlasLevels = tileLevels + 1;
        std::vector<std::vector<uint8_t>> levels;
        std::vector<std::pair<int, int>> sizes;
        levels.emplace_back(static_cast<size_t>(W) * H * 4, 0);
        sizes.emplace_back(W, H);
        {
            std::vector<uint8_t>& base = levels[0];
            const size_t tileBytes = static_cast<size_t>(tilePx) * tilePx * 4;
            for (int t = 0; t < atlasTiles; t++) {
                const int gx = t % gridCols, gy = t / gridCols;
                for (int y = 0; y < tilePx; y++)
                    std::memcpy(&base[(static_cast<size_t>(gy * tilePx + y) * W + static_cast<size_t>(gx) * tilePx) * 4],
                                rgba + tileBytes * static_cast<size_t>(t) + static_cast<size_t>(y) * tilePx * 4, static_cast<size_t>(tilePx) * 4);
            }
        }
        for (int l = 1; l <= tileLevels; l++) {
            const int s = tilePx >> l, ps = tilePx >> (l - 1);
            const int lw = gridCols * s, lh = gridRows * s;
            levels.emplace_back(static_cast<size_t>(lw) * lh * 4, 0);
            sizes.emplace_back(lw, lh);
            const std::vector<uint8_t>& prev = levels[static_cast<size_t>(l) - 1];
            const int pw = sizes[static_cast<size_t>(l) - 1].first;
            for (int t = 0; t < atlasTiles; t++) {
                const int gx = t % gridCols, gy = t / gridCols;
                BoxDown(&prev[(static_cast<size_t>(gy * ps) * pw + static_cast<size_t>(gx) * ps) * 4], ps, ps, pw * 4,
                        &levels[static_cast<size_t>(l)][(static_cast<size_t>(gy * s) * lw + static_cast<size_t>(gx) * s) * 4], lw * 4);
            }
        }
        while (sizes.back().first > 1 || sizes.back().second > 1) {
            const auto [pw, ph] = sizes.back();
            const int nw = std::max(1, pw / 2), nh = std::max(1, ph / 2);
            std::vector<uint8_t> next(static_cast<size_t>(nw) * nh * 4);
            BoxDown(levels.back().data(), pw, ph, pw * 4, next.data(), nw * 4);
            levels.push_back(std::move(next));
            sizes.emplace_back(nw, nh);
        }
        size_t total = 0;
        for (const auto& l : levels) total += l.size();
        const bgfx::Memory* mem = bgfx::alloc(static_cast<uint32_t>(total));
        size_t offset = 0;
        for (const auto& l : levels) {
            std::memcpy(mem->data + offset, l.data(), l.size());
            offset += l.size();
        }
        atlas = bgfx::createTexture2D(static_cast<uint16_t>(W), static_cast<uint16_t>(H), true, 1, bgfx::TextureFormat::RGBA8,
                                      BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT, mem);
        if (!bgfx::isValid(atlas)) std::fprintf(stderr, "[javapack] atlas texture creation failed (%dx%d)\n", W, H);
    }

    void JavaPackHost::UpdateTile(int tile, const uint8_t* rgba) {
        if (!bgfx::isValid(atlas) || !rgba || tile < 0 || tile >= atlasTiles) return;
        const int gx = tile % gridCols, gy = tile / gridCols;
        std::vector<uint8_t> cur(rgba, rgba + static_cast<size_t>(atlasTilePx) * atlasTilePx * 4), next;
        int s = atlasTilePx;
        for (int l = 0; l < atlasLevels; l++) {
            bgfx::updateTexture2D(atlas, 0, static_cast<uint8_t>(l), static_cast<uint16_t>(gx * s), static_cast<uint16_t>(gy * s),
                                  static_cast<uint16_t>(s), static_cast<uint16_t>(s), bgfx::copy(cur.data(), static_cast<uint32_t>(cur.size())),
                                  static_cast<uint16_t>(s * 4));
            if (l + 1 >= atlasLevels) break;
            next.assign(static_cast<size_t>(s / 2) * (s / 2) * 4, 0);
            BoxDown(cur.data(), s, s, s * 4, next.data(), (s / 2) * 4);
            cur.swap(next);
            s /= 2;
        }
    }

    void JavaPackHost::Convert(const Tessellator& tess, std::vector<PackVertex>& out) {
        const std::vector<Vertex>& verts = tess.Vertices();
        const std::vector<QuadInfo>& quads = tess.Quads();
        const bool haveQuads = quads.size() * 4 == verts.size();
        out.resize(verts.size());
        constexpr float kTiles = static_cast<float>(BlockLibrary::kAtlasTiles);
        (void)kTiles;
        const float inset = 1.0f / (2.0f * static_cast<float>(BlockLibrary::TilePx()));
        const float span = 1.0f - 2.0f * inset;
        const float invCols = 1.0f / static_cast<float>(gridCols), invRows = 1.0f / static_cast<float>(gridRows);
        for (size_t q = 0; q + 3 < verts.size() + 0 && q * 1 < verts.size(); q += 4) {
            if (q + 3 >= verts.size()) break;
            const Vertex* v = &verts[q];
            const glm::vec3 p0(v[0].x, v[0].y, v[0].z), p1(v[1].x, v[1].y, v[1].z), p2(v[2].x, v[2].y, v[2].z), p3(v[3].x, v[3].y, v[3].z);
            glm::vec3 n = glm::cross(p1 - p0, p2 - p0);
            if (glm::dot(n, n) < 1e-12f) n = glm::cross(p2 - p0, p3 - p0);
            const float len = glm::length(n);
            n = len > 1e-8f ? n / len : glm::vec3(0, 1, 0);
            const glm::vec3 centre = (p0 + p1 + p2 + p3) * 0.25f;
            glm::vec3 blockOrigin;
            uint16_t block = 0;
            float layerType = 0.0f;
            if (haveQuads) {
                const QuadInfo& info = quads[q / 4];
                blockOrigin = glm::vec3(info.bx, info.by, info.bz);
                block = info.block;
                layerType = (info.layer == static_cast<uint8_t>(RenderLayer::Translucent) || info.layer == static_cast<uint8_t>(RenderLayer::TranslucentCull)) ? 1.0f : 0.0f;
            } else {
                blockOrigin = glm::floor(centre - n * 0.01f);
            }
            const glm::vec3 blockCentre = blockOrigin + glm::vec3(0.5f);
            const float side = glm::dot(n, centre - blockCentre);
            if (std::fabs(side) < 0.15f) n = glm::vec3(0, 1, 0);
            else if (side < 0.0f) n = -n;
            glm::vec2 uv0(v[0].u, v[0].v), uv1(v[1].u, v[1].v), uv2(v[2].u, v[2].v);
            const glm::vec3 e1 = p1 - p0, e2 = p2 - p0;
            const glm::vec2 d1 = uv1 - uv0, d2 = uv2 - uv0;
            const float det = d1.x * d2.y - d2.x * d1.y;
            glm::vec3 tangent = std::fabs(det) > 1e-8f ? (e1 * d2.y - e2 * d1.y) / det : e1;
            if (glm::dot(tangent, tangent) < 1e-12f) tangent = glm::vec3(1, 0, 0);
            tangent = glm::normalize(tangent);
            const int packId = block ? PackBlockId(block) : -1;
            float au[4], av[4];
            for (int i = 0; i < 4; i++) {
                const uint32_t tile = v[i].light >> 16;
                const int gx = static_cast<int>(tile % static_cast<uint32_t>(gridCols)), gy = static_cast<int>(tile / static_cast<uint32_t>(gridCols));
                au[i] = (static_cast<float>(gx) + inset + v[i].u * span) * invCols;
                av[i] = (static_cast<float>(gy) + inset + v[i].v * span) * invRows;
            }
            const float midU = (au[0] + au[1] + au[2] + au[3]) * 0.25f, midV = (av[0] + av[1] + av[2] + av[3]) * 0.25f;
            auto q8 = [](float f) { return static_cast<int8_t>(std::lround(std::clamp(f, -1.0f, 1.0f) * 127.0f)); };
            for (int i = 0; i < 4; i++) {
                PackVertex& o = out[q + static_cast<size_t>(i)];
                o.x = v[i].x;
                o.y = v[i].y;
                o.z = v[i].z;
                o.abgr = v[i].rgba;
                o.u = au[i];
                o.v = av[i];
                o.lu = static_cast<float>(v[i].light & 0xFF);
                o.lv = static_cast<float>((v[i].light >> 8) & 0xFF);
                o.nx = q8(n.x);
                o.ny = q8(n.y);
                o.nz = q8(n.z);
                o.nw = 0;
                o.entity[0] = static_cast<float>(packId);
                o.entity[1] = layerType;
                o.mid[0] = midU;
                o.mid[1] = midV;
                const glm::vec3 pos(v[i].x, v[i].y, v[i].z);
                const glm::vec3 toMid = (blockCentre - pos) * 64.0f;
                o.midBlock[0] = static_cast<int8_t>(std::clamp(std::lround(toMid.x), -127L, 127L));
                o.midBlock[1] = static_cast<int8_t>(std::clamp(std::lround(toMid.y), -127L, 127L));
                o.midBlock[2] = static_cast<int8_t>(std::clamp(std::lround(toMid.z), -127L, 127L));
                o.midBlock[3] = 0;
                o.tangent[0] = q8(tangent.x);
                o.tangent[1] = q8(tangent.y);
                o.tangent[2] = q8(tangent.z);
                o.tangent[3] = 127;
            }
        }
    }

    void JavaPackHost::BuildFrame(const FrameParams& params, jshader::FrameState& s, PackShadowSetup& shadow, bool shadowsOn) {
        s = jshader::FrameState();
        const glm::mat4 view = ToMat(params.view);
        glm::mat4 rotation = view;
        rotation[3] = glm::vec4(0, 0, 0, 1);
        FromMat(rotation, s.modelView);
        ToGlDepth(params.projection, s.projection);
        if (havePrev) {
            std::memcpy(s.prevModelView, prevModelView, sizeof(prevModelView));
            std::memcpy(s.prevProjection, prevProjection, sizeof(prevProjection));
            std::memcpy(s.previousCameraPosition, prevCamera, sizeof(prevCamera));
        } else {
            std::memcpy(s.prevModelView, s.modelView, sizeof(s.modelView));
            std::memcpy(s.prevProjection, s.projection, sizeof(s.projection));
            for (int i = 0; i < 3; i++) s.previousCameraPosition[i] = params.cameraPosition[i];
        }
        std::memcpy(prevModelView, s.modelView, sizeof(prevModelView));
        std::memcpy(prevProjection, s.projection, sizeof(prevProjection));
        for (int i = 0; i < 3; i++) {
            s.cameraPosition[i] = params.cameraPosition[i];
            prevCamera[i] = params.cameraPosition[i];
        }
        havePrev = true;

        const double a = params.sky.sunAngle;
        double frac = a / (2.0 * kPi) + 0.25;
        frac -= std::floor(frac);
        s.sunAngle = static_cast<float>(frac);
        s.shadowAngle = s.sunAngle <= 0.5f ? s.sunAngle : s.sunAngle - 0.5f;
        s.worldTime = static_cast<int>(frac * 24000.0) % 24000;
        s.worldDay = 0;
        s.moonPhase = static_cast<int>(std::floor(params.sky.moonPhase)) & 7;
        const glm::vec3 sunDir(-std::sin(static_cast<float>(a)), std::cos(static_cast<float>(a)), 0.0f);
        const glm::mat3 rot3(rotation);
        const glm::vec3 sunView = rot3 * sunDir * 100.0f, moonView = -sunView, upView = rot3 * glm::vec3(0, 1, 0) * 100.0f;
        for (int i = 0; i < 3; i++) {
            s.sunPosition[i] = sunView[i];
            s.moonPosition[i] = moonView[i];
            s.upPosition[i] = upView[i];
        }
        const bool dayLight = s.sunAngle < 0.5f;
        for (int i = 0; i < 3; i++) s.shadowLightPosition[i] = dayLight ? sunView[i] : moonView[i];
        for (int i = 0; i < 3; i++) {
            s.skyColor[i] = params.sky.skyColor[i];
            s.fogColor[i] = params.fogColor[i];
        }
        s.fogStart = params.renderDistanceFogStart;
        s.fogEnd = params.renderDistanceFogEnd;
        s.fogDensity = 1.0f;
        s.rainStrength = std::clamp(1.0f - params.sky.rainBrightness, 0.0f, 1.0f);
        s.wetness = s.rainStrength;
        s.thunderStrength = 0.0f;
        s.frameCounter = static_cast<int>(params.frameIndex);
        s.frameTimeCounter = params.timeSeconds;
        s.frameTime = std::clamp(params.timeSeconds - lastTime, 0.0f, 0.25f);
        lastTime = params.timeSeconds;
        const float c = s.projection[10], d = s.projection[14];
        if (std::fabs(c - 1.0f) > 1e-6f && std::fabs(c + 1.0f) > 1e-6f) {
            s.nearPlane = d / (c - 1.0f);
            s.farPlane = d / (c + 1.0f);
        }
        s.isEyeInWater = params.cameraMedium == 1 ? 1 : (params.cameraMedium == 2 ? 2 : 0);
        s.eyeAltitude = params.cameraPosition[1];
        s.heldBlockLightValue = static_cast<int>(params.heldLight);
        s.atlasSize[0] = atlasWidth;
        s.atlasSize[1] = gridRows * atlasTilePx;
        s.cloudTime = static_cast<float>(params.cloud.gameTicks);
        s.screenBrightness = 0.5f;
        s.dimensionNether = instance && instance->Dimension() == "world-1";
        s.dimensionEnd = instance && instance->Dimension() == "world1";
        s.heavyFog = s.dimensionNether ? 1.0f : 0.0f;
        s.cloudHeight = params.cloud.height;

        const jshader::PipelineConfig& cfg = pipeline->Config();
        shadow.resolution = cfg.shadowMapResolution;
        shadow.distance = cfg.shadowDistance;
        glm::vec3 lightDir = dayLight ? sunDir : -sunDir;
        if (std::fabs(cfg.sunPathRotation) > 0.001f) {
            const glm::mat4 tilt = glm::rotate(glm::mat4(1.0f), glm::radians(cfg.sunPathRotation), glm::vec3(1, 0, 0));
            lightDir = glm::vec3(tilt * glm::vec4(lightDir, 0.0f));
        }
        lightDir = glm::normalize(lightDir);
        glm::vec3 up(0, 0, 1);
        if (std::fabs(lightDir.z) > 0.99f) up = glm::vec3(0, 1, 0);
        const glm::mat4 lookAt = glm::lookAt(lightDir * 100.0f, glm::vec3(0), up);
        const glm::vec3 camera(params.cameraPosition[0], params.cameraPosition[1], params.cameraPosition[2]);
        const float r = std::max(8.0f, cfg.shadowDistance);
        const float texel = 2.0f * r / static_cast<float>(std::max(1, cfg.shadowMapResolution));
        const glm::vec3 camLight = glm::mat3(lookAt) * camera;
        const glm::vec2 snapped = glm::round(glm::vec2(camLight) / texel) * texel;
        const glm::vec2 delta = snapped - glm::vec2(camLight);
        const glm::mat4 shadowView = glm::translate(glm::mat4(1.0f), glm::vec3(-delta.x, -delta.y, 0.0f)) * lookAt;
        FromMat(shadowView, shadow.view);
        FromMat(glm::orthoRH_NO(-r, r, -r, r, 0.05f, 256.0f), shadow.projection);
        FromMat(shadowView * glm::translate(glm::mat4(1.0f), -camera), shadow.drawView);
        for (int i = 0; i < 3; i++) shadow.lightDir[i] = lightDir[i];
        if (shadowsOn) {
            std::memcpy(s.shadowModelView, shadow.view, sizeof(shadow.view));
            std::memcpy(s.shadowProjection, shadow.projection, sizeof(shadow.projection));
        }
    }

    jshader::PackDrawState JavaPackHost::TerrainDraw(const FrameParams& params, const jshader::FrameState& state) const {
        jshader::PackDrawState d;
        std::memcpy(d.modelView, params.view, sizeof(d.modelView));
        std::memcpy(d.projection, state.projection, sizeof(d.projection));
        const glm::mat3 normalMat(ToMat(params.view));
        std::memcpy(d.normalMatrix, &normalMat[0][0], sizeof(d.normalMatrix));
        const float lm[16] = {1.0f / 256.0f, 0, 0, 0, 0, 1.0f / 256.0f, 0, 0, 0, 0, 1, 0, 1.0f / 32.0f, 1.0f / 32.0f, 0, 1};
        std::memcpy(d.textureMatrix1, lm, sizeof(lm));
        for (int i = 0; i < 3; i++) d.fogColor[i] = state.fogColor[i];
        d.fogColor[3] = 1.0f;
        d.fogStart = state.fogStart;
        d.fogEnd = state.fogEnd;
        d.fogDensity = state.fogDensity;
        d.viewHeight = state.viewHeight;
        return d;
    }
}
