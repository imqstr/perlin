// builds a pipeline for every program in the shader pack with the engine's standard layouts
#include "rhi/Device.hpp"
#include "rhi/ShaderLibrary.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>

using namespace rhi;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: rhi_pipeline_smoke <pack> [api]\n");
        return 2;
    }
    const Api api = argc > 2 ? ApiFromId(argv[2]) : Api::Vulkan;
    std::ifstream in(argv[1], std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    ShaderLibrary lib;
    std::string error;
    if (!lib.Load(bytes, &error)) {
        std::fprintf(stderr, "pack: %s\n", error.c_str());
        return 1;
    }

    InstanceOptions io;
    io.validation = true;
    auto instance = CreateInstance(api, io);
    if (!instance || instance->Adapters().empty()) {
        std::puts("no driver: skipped");
        return 77;
    }
    DeviceOptions dopts;
    for (const auto& a : instance->Adapters())
        if (a.usable)
            dopts.adapterIndex = a.index;
    dopts.validation = true;
    auto dev = instance->CreateDevice(dopts);
    if (!dev) {
        std::fprintf(stderr, "device creation failed\n");
        return 1;
    }

    const ShaderTarget target = ShaderLibrary::TargetFor(api);
    std::vector<BindLayoutEntry> uniformEntries{{0, BindingType::UniformBufferDynamic, ShaderStage::All, 1, TextureDim::Tex2D}};
    std::vector<BindLayoutEntry> textureEntries;
    for (const TextureSlot& s : lib.TextureSlots()) {
        textureEntries.push_back({s.slot * 2, BindingType::SampledTexture, ShaderStage::Fragment | ShaderStage::Vertex, 1,
                                  s.array ? TextureDim::Tex2DArray : TextureDim::Tex2D});
        textureEntries.push_back({s.slot * 2 + 1, BindingType::Sampler, ShaderStage::Fragment | ShaderStage::Vertex, 1, TextureDim::Tex2D});
    }
    BindLayoutDesc uld;
    uld.entries = uniformEntries;
    BindLayoutDesc tld;
    tld.entries = textureEntries;
    PipelineLayoutDesc pld;
    pld.sets[0] = dev->CreateBindLayout(uld);
    pld.sets[1] = dev->CreateBindLayout(tld);
    PipelineLayoutHandle layout = dev->CreatePipelineLayout(pld);

    int failed = 0;
    for (const ProgramInfo& p : lib.Programs()) {
        auto vs = lib.Code(p, ShaderStage::Vertex, target);
        auto fs = lib.Code(p, ShaderStage::Fragment, target);
        if (vs.empty() || fs.empty())
            continue;
        ShaderDesc sd;
        sd.format = ShaderLibrary::FormatFor(target);
        sd.stage = ShaderStage::Vertex;
        sd.code = vs;
        sd.entry = ShaderLibrary::EntryFor(ShaderStage::Vertex, target);
        sd.name = p.name.c_str();
        ShaderHandle v = dev->CreateShader(sd);
        sd.stage = ShaderStage::Fragment;
        sd.code = fs;
        sd.entry = ShaderLibrary::EntryFor(ShaderStage::Fragment, target);
        ShaderHandle f = dev->CreateShader(sd);

        GraphicsPipelineDesc gd;
        gd.vertex = v;
        gd.fragment = f;
        gd.layout = layout;
        gd.streams[0] = {128, VertexStep::Vertex};
        gd.streamCount = 1;
        for (uint32_t i = 0; i < 8; i++)
            gd.attributes[i] = {i, 0, Format::RGBA32Float, i * 16};
        gd.attributeCount = 8;
        gd.colorFormats[0] = Format::RGBA8Unorm;
        gd.colorCount = 1;
        PipelineHandle pipe = dev->CreateGraphicsPipeline(gd);
        const std::string report = dev->DebugReport();
        if (!pipe || !report.empty()) {
            std::fprintf(stderr, "FAIL %s %s\n%s", p.name.c_str(), pipe ? "validation output" : "no pipeline", report.c_str());
            failed++;
        }
        if (pipe)
            dev->Destroy(pipe);
        dev->Destroy(v);
        dev->Destroy(f);
    }
    dev->WaitIdle();
    if (failed) {
        std::fprintf(stderr, "%d program(s) failed\n", failed);
        return 1;
    }
    std::printf("rhi_pipeline_smoke ok (%zu programs)\n", lib.Programs().size());
    return 0;
}
