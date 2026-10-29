// renders through the whole gpu layer headless: checks the matrix, winding and chunk draw conventions the shaders rely on
#include "client/renderer/Frustum.hpp"
#include "client/renderer/Tessellator.hpp"
#include "client/renderer/gpu/RenderContext_rhi.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

using namespace perlin;

namespace {
    int failures = 0;
    void Check(bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok)
            failures++;
    }

    struct Pixel {
        int r, g, b;
    };

    Pixel At(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t x, uint32_t y) {
        const uint8_t* p = rgba.data() + (static_cast<size_t>(y) * w + x) * 4;
        return {p[0], p[1], p[2]};
    }

    bool Red(Pixel p) { return p.r > 150 && p.g < 90 && p.b < 90; }
    bool Background(Pixel p) { return p.b > 150 && p.r < 90 && p.g < 90; }

    FrameParams MakeFrame(const glm::vec3& eye, const glm::vec3& target, const glm::vec3& up, uint32_t w, uint32_t h) {
        FrameParams f{};
        const glm::mat4 view = glm::lookAt(eye, target, up);
        const glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(70.0f), static_cast<float>(w) / static_cast<float>(h), 0.1f, 200.0f);
        const glm::mat4 vp = proj * view;
        std::memcpy(f.viewProjection, glm::value_ptr(vp), sizeof(f.viewProjection));
        std::memcpy(f.skyViewProjection, glm::value_ptr(vp), sizeof(f.skyViewProjection));
        std::memcpy(f.view, glm::value_ptr(view), sizeof(f.view));
        std::memcpy(f.projection, glm::value_ptr(proj), sizeof(f.projection));
        f.cameraPosition[0] = eye.x;
        f.cameraPosition[1] = eye.y;
        f.cameraPosition[2] = eye.z;
        f.fogColor[2] = 1.0f;
        f.fogColor[3] = 1.0f;
        f.environmentalFogStart = 1000.0f;
        f.environmentalFogEnd = 2000.0f;
        f.renderDistanceFogStart = 1000.0f;
        f.renderDistanceFogEnd = 2000.0f;
        f.skyFogEnd = 2000.0f;
        f.cloudFogEnd = 2000.0f;
        f.renderScale = 1.0f;
        f.frameGen = 0;
        f.timeSeconds = 1.0f;
        std::memset(f.lightmap, 255, sizeof(f.lightmap));
        return f;
    }

    bool Frame(RenderContext_rhi& ctx, const FrameParams& f, std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) {
        ctx.BeginFrame(f);
        ctx.DrawChunkMeshes(Frustum::FromViewProjection(glm::make_mat4(f.viewProjection)), f.cameraPosition[0], f.cameraPosition[2]);
        ctx.EndFrame();
        return ctx.ReadBackFrame(rgba, w, h);
    }
}

namespace perlin {
    void TestSetBundledPack(bool on);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: gpu_context_test <pack> [api]\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    const std::vector<uint8_t> pack{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    gpu::GpuSettings settings;
    settings.api = argc > 2 ? rhi::ApiFromId(argv[2]) : rhi::Api::Vulkan;
    settings.width = 128;
    settings.height = 96;
    settings.offscreen = true;
    settings.validation = true;

    RenderContext_rhi ctx;
    std::string error;
    if (!ctx.InitializeOffscreen(settings, pack, &error)) {
        std::printf("no device: %s\n", error.c_str());
        return 77;
    }

    std::vector<uint8_t> tile(16 * 16 * 4);
    for (size_t i = 0; i < tile.size(); i += 4) {
        tile[i] = 255;
        tile[i + 1] = 0;
        tile[i + 2] = 0;
        tile[i + 3] = 255;
    }
    ctx.UploadAtlas(tile.data(), 16, 1);

    // top face exactly as the chunk mesher emits it: clockwise seen from above
    Tessellator tess;
    tess.Begin();
    tess.EnterSection(0);
    const float v[4][3] = {{-2, 0, -2}, {2, 0, -2}, {2, 0, 2}, {-2, 0, 2}};
    const float c[4][3] = {{1, 1, 1}, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}};
    const float uv[4][2] = {{0, 0}, {0.01f, 0}, {0.01f, 0.01f}, {0, 0.01f}};
    tess.AddQuad(v, c, uv, false, 15.0f, 15.0f);
    tess.End();
    ctx.UpdateChunkMesh(0, 0, tess, 1);

    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;

    Check(Frame(ctx, MakeFrame({0, 3, 0}, {0, 0, 0}, {0, 0, -1}, 128, 96), rgba, w, h), "frame read back from above");
    if (!rgba.empty()) {
        Check(Red(At(rgba, w, w / 2, h / 2)), "front face visible from above");
        Check(Background(At(rgba, w, 1, 1)), "corner shows the clear colour");
    }
    Check(Frame(ctx, MakeFrame({0, -3, 0}, {0, 0, 0}, {0, 0, 1}, 128, 96), rgba, w, h), "frame read back from below");
    if (!rgba.empty())
        Check(!Red(At(rgba, w, w / 2, h / 2)), "back face culled from below");

    // screen orientation: a quad in the +x half must land on the right of the image
    Tessellator right;
    right.Begin();
    right.EnterSection(0);
    const float vr[4][3] = {{0.5f, 0, -2}, {2, 0, -2}, {2, 0, 2}, {0.5f, 0, 2}};
    right.AddQuad(vr, c, uv, false, 15.0f, 15.0f);
    right.End();
    ctx.UpdateChunkMesh(0, 0, right, 2);
    Check(Frame(ctx, MakeFrame({0, 3, 0}, {0, 0, 0}, {0, 0, -1}, 128, 96), rgba, w, h), "frame with the right-hand quad");
    if (!rgba.empty()) {
        Check(Red(At(rgba, w, w - 20, h / 2)), "+x lands on the right");
        Check(Background(At(rgba, w, 20, h / 2)), "-x stays background");
    }

    // everything on the gpu survives a device rebuild: mesh pages, atlas, lightmap
    // rebuild onto another api when asked, so the frames after it prove the switch
    const char* switchTo = std::getenv("PERLIN_TEST_SWITCH_TO");
    Check(ctx.RebuildDevice(switchTo ? switchTo : rhi::ApiId(settings.api)), "device rebuilt");
    Check(Frame(ctx, MakeFrame({0, 3, 0}, {0, 0, 0}, {0, 0, -1}, 128, 96), rgba, w, h), "frame after rebuild");
    if (!rgba.empty()) {
        Check(Red(At(rgba, w, w - 20, h / 2)), "mesh and atlas restored");
        Check(Background(At(rgba, w, 20, h / 2)), "layout unchanged after rebuild");
    }

    // post chain: the offscreen path must give the same layout as the direct one
    struct PostCase {
        const char* name;
        float scale;
        int upscaler;
        float bloom, blur, sharp;
    };
    const PostCase cases[] = {PostCase{"scaled bilinear", 0.5f, 1, 0, 0, 0}, PostCase{"scaled spatial sharpened", 0.5f, 2, 0, 0, 0.5f},
                              PostCase{"bloom", 1.0f, 0, 0.6f, 0, 0}, PostCase{"motion blur", 1.0f, 0, 0, 0.5f, 0},
                              PostCase{"temporal", 0.5f, 3, 0.4f, 0.3f, 0.3f}};
    for (const PostCase& pc : cases) {
        // a blank frame first: a stale image from the previous case must not pass for this one
        ctx.RemoveChunkMesh(0, 0);
        Frame(ctx, MakeFrame({0, 3, 0}, {0, 0, 0}, {0, 0, -1}, 128, 96), rgba, w, h);
        ctx.UpdateChunkMesh(0, 0, right, 20 + static_cast<uint64_t>(&pc - cases));
        bool got = true;
        for (int i = 0; i < 4 && got; i++) {
            FrameParams f = MakeFrame({0, 3, 0}, {0, 0, 0}, {0, 0, -1}, 128, 96);
            f.renderScale = pc.scale;
            f.upscaler = pc.upscaler;
            f.bloom = pc.bloom;
            f.motionBlur = pc.blur;
            f.upscaleSharpness = pc.sharp;
            f.timeSeconds = 0.02f * static_cast<float>(i);
            got = Frame(ctx, f, rgba, w, h);
        }
        Check(got, (std::string("post frame: ") + pc.name).c_str());
        if (got && !rgba.empty()) {
            Check(w == 128 && h == 96, (std::string("post size: ") + pc.name).c_str());
            if (!Red(At(rgba, w, w - 36, h / 2))) {
                const auto p = At(rgba, w, w - 36, h / 2);
                std::printf("     pixel %d %d %d\n", p.r, p.g, p.b);
            }
            Check(Red(At(rgba, w, w - 36, h / 2)), (std::string("post right quad: ") + pc.name).c_str());
            Check(!Red(At(rgba, w, 20, h / 2)), (std::string("post left empty: ") + pc.name).c_str());
        }
    }
    {
        FrameParams f = MakeFrame({0, 3, 0}, {0, 0, 0}, {0, 0, -1}, 128, 96);
        Check(Frame(ctx, f, rgba, w, h), "direct frame after post");
        if (!rgba.empty())
            Check(Red(At(rgba, w, w - 36, h / 2)), "direct path restored");
    }

    // removing the mesh clears the image
    ctx.RemoveChunkMesh(0, 0);
    Check(Frame(ctx, MakeFrame({0, 3, 0}, {0, 0, 0}, {0, 0, -1}, 128, 96), rgba, w, h), "frame after removal");
    if (!rgba.empty())
        Check(Background(At(rgba, w, w / 2, h / 2)), "removed mesh is gone");

    // sun shadows: a floating quad must darken the ground it covers, only with shadows on
    perlin::TestSetBundledPack(true);
    Check(ctx.ReloadShaders(), "bundled variant resolves");
    {
        Tessellator scene;
        scene.Begin();
        scene.EnterSection(0);
        const float ground[4][3] = {{-3, 0, -3}, {3, 0, -3}, {3, 0, 3}, {-3, 0, 3}};
        const float occl[4][3] = {{-1, 1.5f, -1}, {0.2f, 1.5f, -1}, {0.2f, 1.5f, 1}, {-1, 1.5f, 1}};
        scene.AddQuad(ground, c, uv, false, 0.0f, 15.0f);
        scene.AddQuad(occl, c, uv, false, 0.0f, 15.0f);
        scene.End();
        ctx.UpdateChunkMesh(0, 0, scene, 7);
        std::vector<uint8_t> lit, dark;
        const auto shot = [&](int quality, std::vector<uint8_t>& out) {
            bool ok = true;
            for (int i = 0; i < 3 && ok; i++) {
                FrameParams f = MakeFrame({0.5f, 5, 5}, {0.5f, 0, 0}, {0, 1, 0}, 128, 96);
                f.shadowQuality = quality;
                f.shadowEntities = false;
                f.sky.enabled = true;
                f.waterTile = f.lavaTile = f.waterFlowTile = -1.0f;
                f.sky.sunAngle = 0.6f;
                // mid-range pack settings keep the lit ground away from the tonemap ceiling
                for (float& v : f.packSettings)
                    v = 0.5f;
                f.timeSeconds = 2.0f + 0.5f * static_cast<float>(i);
                ok = Frame(ctx, f, out, w, h);
            }
            return ok;
        };
        Check(shot(0, lit), "frame without shadows");
        Check(shot(2, dark), "frame with shadows");
        if (!lit.empty() && lit.size() == dark.size()) {
            int darker = 0, brighter = 0;
            for (size_t i = 0; i + 3 < lit.size(); i += 4) {
                const int a = lit[i] + lit[i + 1] + lit[i + 2], b = dark[i] + dark[i + 1] + dark[i + 2];
                darker += b < a - 6;
                brighter += b > a + 6;
            }
            if (const char* dump = std::getenv("PERLIN_TEST_DUMP"))
                for (int k = 0; k < 2; k++) {
                    const std::string path = std::string(dump) + (k ? "_dark.ppm" : "_lit.ppm");
                    if (FILE* fp = std::fopen(path.c_str(), "wb")) {
                        std::fprintf(fp, "P6\n%u %u\n255\n", w, h);
                        const std::vector<uint8_t>& img = k ? dark : lit;
                        for (size_t i = 0; i + 3 < img.size(); i += 4)
                            std::fwrite(&img[i], 1, 3, fp);
                        std::fclose(fp);
                    }
                }
            std::printf("     shadow pixels: %d darker, %d brighter\n", darker, brighter);
            Check(darker > 40, "the occluder darkens the ground");
        }

        // scene copy: opaque pass, colour+depth copy, then translucent draws sampling the copy
        {
            bool ok = true;
            for (int i = 0; i < 3 && ok; i++) {
                FrameParams f = MakeFrame({0.5f, 5, 5}, {0.5f, 0, 0}, {0, 1, 0}, 128, 96);
                f.shadowQuality = 2;
                f.shadowEntities = false;
                f.sky.enabled = true;
                f.sky.sunAngle = 0.6f;
                f.waterTile = f.lavaTile = f.waterFlowTile = -1.0f;
                for (float& v : f.packSettings)
                    v = 0.5f;
                f.timeSeconds = 5.0f + static_cast<float>(i);
                ok = Frame(ctx, f, rgba, w, h);
            }
            Check(ok, "frame with the scene copy");
            if (ok && !rgba.empty())
                Check(!Background(At(rgba, w, w / 2, h / 2)), "scene copy keeps the ground");
        }
    }

    TestSetBundledPack(false);
    ctx.ReloadShaders();
    ctx.UnloadLevelResources();
    ctx.UnloadLevelResources();
    const auto afterLeave = MakeFrame({0, 3, 0}, {0, 0, 0}, {0, 0, -1}, 128, 96);
    Check(Frame(ctx, afterLeave, rgba, w, h), "frame after leaving the level");
    if (!rgba.empty())
        Check(Background(At(rgba, w, w / 2, h / 2)), "previous level mesh is gone");
    ctx.UploadAtlas(tile.data(), 16, 1);
    ctx.UpdateChunkMesh(0, 0, tess, 100);
    Check(Frame(ctx, afterLeave, rgba, w, h), "frame after loading the next level");
    if (!rgba.empty())
        Check(Red(At(rgba, w, w / 2, h / 2)), "next level atlas and mesh draw correctly");
    ctx.Shutdown();
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
