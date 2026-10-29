#include "rhi/ShaderLibrary.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

static int g_failed = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            g_failed++;                                                          \
        }                                                                        \
    } while (0)

using namespace rhi;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: rhi_shaderlib_test <pack>\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(!bytes.empty());

    ShaderLibrary lib;
    std::string error;
    CHECK(lib.Load(bytes, &error));
    CHECK(lib.Programs().size() >= 6);

    const ProgramInfo* post = lib.Find("post");
    CHECK(post != nullptr);
    if (post) {
        const UniformMember* m = post->FindUniform("u_post");
        CHECK(m && m->offset == 0 && m->bytes == 16);
        CHECK(post->uniformBytes >= 48);
        // every graphics program has both stages for every target the pack was built with
        for (ShaderTarget t : {ShaderTarget::SpirV, ShaderTarget::Dxil, ShaderTarget::Hlsl, ShaderTarget::Msl, ShaderTarget::Wgsl}) {
            CHECK(!lib.Code(*post, ShaderStage::Vertex, t).empty());
            CHECK(!lib.Code(*post, ShaderStage::Fragment, t).empty());
        }
        CHECK(lib.Code(*post, ShaderStage::Compute, ShaderTarget::SpirV).empty());

        const auto spirv = lib.Code(*post, ShaderStage::Fragment, ShaderTarget::SpirV);
        uint32_t magic = 0;
        std::memcpy(&magic, spirv.data(), 4);
        CHECK(magic == 0x07230203u);
        const auto dxil = lib.Code(*post, ShaderStage::Fragment, ShaderTarget::Dxil);
        CHECK(dxil.size() > 4 && std::memcmp(dxil.data(), "DXBC", 4) == 0);
    }

    // matrices and arrays keep their size so the engine can copy whole values
    for (const ProgramInfo& p : lib.Programs()) {
        for (const UniformMember& u : p.uniforms) {
            CHECK(u.offset % 16 == 0);
            CHECK(u.offset + u.bytes <= p.uniformBytes);
        }
    }
    if (const ProgramInfo* up = lib.Find("vv_terrain_solid")) {
        const UniformMember* lights = up->FindUniform("u_dynLights");
        CHECK(lights && lights->elements == 8 && lights->bytes == 128);
        const UniformMember* mvp = up->FindUniform("u_mvp");
        CHECK(mvp && mvp->bytes == 64);
    }

    const TextureSlot* atlas = lib.FindTexture("s_atlas");
    CHECK(atlas && atlas->slot == 0 && atlas->array);
    const TextureSlot* cur = lib.FindTexture("s_cur");
    CHECK(cur && !cur->array);

    CHECK(ShaderLibrary::TargetFor(Api::D3D12) == ShaderTarget::Dxil);
    CHECK(ShaderLibrary::FormatFor(ShaderTarget::Hlsl) == ShaderFormat::Hlsl);

    std::vector<uint8_t> broken(bytes.begin(), bytes.begin() + 40);
    ShaderLibrary bad;
    CHECK(!bad.Load(broken, &error));

    if (g_failed) {
        std::fprintf(stderr, "%d failure(s)\n", g_failed);
        return 1;
    }
    std::puts("rhi_shaderlib_test: ok");
    return 0;
}
