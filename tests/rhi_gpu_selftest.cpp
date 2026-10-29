// end-to-end checks of the vulkan backend on whatever device the loader finds (swiftshader in ci)
#include "rhi/Device.hpp"
#include "rhi/common/Util.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>

using namespace rhi;

static int g_failed = 0;
#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
            g_failed++;                                                              \
        }                                                                            \
    } while (0)

static std::string g_shaderDir;
static Api g_api = Api::Vulkan;

static std::vector<uint8_t> LoadFile(const std::string& name) {
    std::ifstream in(g_shaderDir + "/" + name, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct Ctx {
    std::unique_ptr<Instance> instance;
    std::unique_ptr<Device> dev;
};

static void ExpectNoErrors(Device& dev, const char* where) {
    const std::string report = dev.DebugReport();
    if (!report.empty()) {
        std::fprintf(stderr, "FAIL %s produced validation output:\n%s", where, report.c_str());
        g_failed++;
    }
}

static BufferHandle MakeBuffer(Device& dev, uint64_t size, BufferUsage usage, MemoryClass mem, const char* name) {
    BufferDesc d;
    d.size = size;
    d.usage = usage;
    d.memory = mem;
    d.name = name;
    // structured storage buffers need an element size on d3d12; raw views are not used by these shaders
    if (Has(usage, BufferUsage::Storage))
        d.stride = 4;
    return dev.CreateBuffer(d);
}

static ShaderHandle MakeShader(Device& dev, const char* file, ShaderStage stage) {
    std::string path = file;
    path.resize(path.size() - 3);
    path += g_api == Api::D3D12 ? "dxil" : "spv";
    auto code = LoadFile(path.c_str());
    ShaderDesc sd;
    sd.format = g_api == Api::D3D12 ? ShaderFormat::Dxil : ShaderFormat::SpirV;
    sd.stage = stage;
    sd.code = code;
    sd.name = file;
    return dev.CreateShader(sd);
}

static void TestBufferRoundTrip(Device& dev) {
    BufferHandle up = MakeBuffer(dev, 1024, BufferUsage::CopySrc, MemoryClass::Upload, "up");
    BufferHandle mid = MakeBuffer(dev, 1024, BufferUsage::CopySrc | BufferUsage::CopyDst, MemoryClass::GpuOnly, "mid");
    BufferHandle rb = MakeBuffer(dev, 1024, BufferUsage::CopyDst, MemoryClass::Readback, "rb");
    uint8_t* p = dev.Map(up);
    CHECK(p != nullptr);
    CHECK(dev.Map(mid) == nullptr);
    for (int i = 0; i < 1024; i++)
        p[i] = static_cast<uint8_t>(i * 13);
    dev.Flush(up, 0, 1024);

    CommandList* cl = dev.AcquireCommandList(QueueType::Graphics);
    cl->Barrier(BufferBarrier{mid, State::Undefined, State::CopyDst});
    cl->CopyBuffer(mid, 0, up, 0, 1024);
    cl->Barrier(BufferBarrier{mid, State::CopyDst, State::CopySrc});
    cl->CopyBuffer(rb, 0, mid, 0, 1024);
    dev.Submit(QueueType::Graphics, cl);
    dev.WaitIdle();
    dev.Invalidate(rb, 0, 1024);
    CHECK(std::memcmp(dev.Map(rb), p, 1024) == 0);
    ExpectNoErrors(dev, "buffer round trip");
    dev.Destroy(up);
    dev.Destroy(mid);
    dev.Destroy(rb);
}

static void TestFillAndFences(Device& dev) {
    BufferHandle gpu = MakeBuffer(dev, 256, BufferUsage::CopySrc | BufferUsage::CopyDst, MemoryClass::GpuOnly, "fill");
    BufferHandle rb = MakeBuffer(dev, 256, BufferUsage::CopyDst, MemoryClass::Readback, "fill-rb");
    FenceHandle fence = dev.CreateFence(0);

    CommandList* cl = dev.AcquireCommandList(QueueType::Graphics);
    cl->Barrier(BufferBarrier{gpu, State::Undefined, State::CopyDst});
    cl->FillBuffer(gpu, 0, 256, 0xA5A5A5A5u);
    cl->Barrier(BufferBarrier{gpu, State::CopyDst, State::CopySrc});
    cl->CopyBuffer(rb, 0, gpu, 0, 256);
    FenceOp signal{fence, 7};
    SubmitDesc sd;
    sd.lists = std::span<CommandList* const>(&cl, 1);
    sd.signals = std::span(&signal, 1);
    dev.Submit(QueueType::Graphics, sd);
    CHECK(dev.WaitFence(fence, 7, 5'000'000'000ull));
    CHECK(dev.FenceValue(fence) >= 7);
    dev.Invalidate(rb, 0, 256);
    const uint32_t* words = reinterpret_cast<const uint32_t*>(dev.Map(rb));
    CHECK(words[0] == 0xA5A5A5A5u && words[63] == 0xA5A5A5A5u);
    ExpectNoErrors(dev, "fill and fences");
    dev.Destroy(fence);
    dev.Destroy(gpu);
    dev.Destroy(rb);
}

struct Scene {
    ShaderHandle vs, fs;
    SamplerHandle sampler;
    BindLayoutHandle layout;
    PipelineLayoutHandle pipeLayout;
    PipelineHandle pipeFront, pipeBack;
    BufferHandle vertices, instances, indices, ubo;
    TextureHandle tex, target;
    BindGroupHandle group;
};

static Scene BuildScene(Device& dev) {
    Scene s;
    s.vs = MakeShader(dev, "raster.vs.spv", ShaderStage::Vertex);
    s.fs = MakeShader(dev, "raster.fs.spv", ShaderStage::Fragment);
    s.sampler = dev.CreateSampler({});

    const BindLayoutEntry entries[] = {
        {0, BindingType::UniformBufferDynamic, ShaderStage::Vertex},
        {1, BindingType::SampledTexture, ShaderStage::Fragment},
        {2, BindingType::Sampler, ShaderStage::Fragment},
    };
    s.layout = dev.CreateBindLayout({entries});
    PipelineLayoutDesc pl;
    pl.sets[0] = s.layout;
    pl.pushConstantBytes = 16;
    pl.pushConstantStages = ShaderStage::Fragment;
    s.pipeLayout = dev.CreatePipelineLayout(pl);

    GraphicsPipelineDesc gp;
    gp.vertex = s.vs;
    gp.fragment = s.fs;
    gp.layout = s.pipeLayout;
    gp.streams[0] = {16, VertexStep::Vertex};
    gp.streams[1] = {8, VertexStep::Instance};
    gp.streamCount = 2;
    gp.attributes[0] = {0, 0, Format::RG32Float, 0};
    gp.attributes[1] = {1, 0, Format::RG32Float, 8};
    gp.attributes[2] = {2, 1, Format::RG32Float, 0};
    gp.attributeCount = 3;
    gp.raster.cull = CullMode::Back;
    gp.colorFormats[0] = Format::RGBA8Unorm;
    gp.colorCount = 1;
    gp.name = "front-ccw";
    s.pipeFront = dev.CreateGraphicsPipeline(gp);
    gp.raster.frontFace = FrontFace::Clockwise;
    gp.name = "front-cw";
    s.pipeBack = dev.CreateGraphicsPipeline(gp);

    // counter-clockwise as seen with y up
    const float verts[] = {-0.2f, -0.2f, 0, 0, 0.2f, -0.2f, 1, 0, 0.0f, 0.2f, 0.5f, 1};
    const float inst[8][2] = {{-0.75f, -0.5f}, {-0.25f, -0.5f}, {0.25f, -0.5f}, {0.75f, -0.5f},
                              {-0.75f, 0.5f},  {-0.25f, 0.5f},  {0.25f, 0.5f},  {0.75f, 0.5f}};
    const uint16_t idx[] = {0, 1, 2};
    const float frame[] = {1.0f, 1.0f, 0, 0};

    BufferHandle staging = MakeBuffer(dev, 4096, BufferUsage::CopySrc, MemoryClass::Upload, "staging");
    uint8_t* st = dev.Map(staging);
    std::memcpy(st + 0, verts, sizeof(verts));
    std::memcpy(st + 256, inst, sizeof(inst));
    std::memcpy(st + 512, idx, sizeof(idx));
    std::memset(st + 1024, 0, 256);
    std::memcpy(st + 1024, frame, sizeof(frame));
    // second dynamic slot (256 apart) doubles the scale
    const float frame2[] = {2.0f, 2.0f, 0, 0};
    std::memcpy(st + 1280, frame2, sizeof(frame2));
    const uint8_t white[16] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    std::memset(st + 2048, 0, 512);
    std::memcpy(st + 2048, white, 8);
    std::memcpy(st + 2048 + 256, white, 8);
    dev.Flush(staging, 0, 4096);

    s.vertices = MakeBuffer(dev, sizeof(verts), BufferUsage::Vertex | BufferUsage::CopyDst, MemoryClass::GpuOnly, "vertices");
    s.instances = MakeBuffer(dev, sizeof(inst), BufferUsage::Vertex | BufferUsage::CopyDst, MemoryClass::GpuOnly, "instances");
    s.indices = MakeBuffer(dev, 16, BufferUsage::Index | BufferUsage::CopyDst, MemoryClass::GpuOnly, "indices");
    s.ubo = MakeBuffer(dev, 512, BufferUsage::Uniform | BufferUsage::CopyDst, MemoryClass::GpuOnly, "ubo");

    TextureDesc td;
    td.format = Format::RGBA8Unorm;
    td.width = td.height = 2;
    td.usage = TextureUsage::Sampled | TextureUsage::CopyDst;
    td.name = "white";
    s.tex = dev.CreateTexture(td);
    td.width = td.height = 64;
    td.usage = TextureUsage::ColorTarget | TextureUsage::CopySrc;
    td.name = "target";
    s.target = dev.CreateTexture(td);

    const BindGroupEntry ge[] = {
        {0, 0, {s.ubo, 0, 16}, {}, {}},
        {1, 0, {}, {s.tex}, {}},
        {2, 0, {}, {}, s.sampler},
    };
    BindGroupDesc gd;
    gd.layout = s.layout;
    gd.entries = ge;
    s.group = dev.CreateBindGroup(gd);

    CommandList* cl = dev.AcquireCommandList(QueueType::Graphics);
    const BufferBarrier toCopy[] = {{s.vertices, State::Undefined, State::CopyDst},
                                    {s.instances, State::Undefined, State::CopyDst},
                                    {s.indices, State::Undefined, State::CopyDst},
                                    {s.ubo, State::Undefined, State::CopyDst}};
    cl->Barrier({}, toCopy);
    cl->Barrier(TextureBarrier{s.tex, State::Undefined, State::CopyDst});
    cl->CopyBuffer(s.vertices, 0, staging, 0, sizeof(verts));
    cl->CopyBuffer(s.instances, 0, staging, 256, sizeof(inst));
    cl->CopyBuffer(s.indices, 0, staging, 512, 16);
    cl->CopyBuffer(s.ubo, 0, staging, 1024, 512);
    BufferTextureCopy region;
    region.bufferOffset = 2048;
    region.bufferRowPitch = 256;
    region.width = region.height = 2;
    cl->CopyBufferToTexture(s.tex, staging, std::span(&region, 1));
    const BufferBarrier toUse[] = {{s.vertices, State::CopyDst, State::VertexBuffer},
                                   {s.instances, State::CopyDst, State::VertexBuffer},
                                   {s.indices, State::CopyDst, State::IndexBuffer},
                                   {s.ubo, State::CopyDst, State::UniformBuffer}};
    cl->Barrier({}, toUse);
    cl->Barrier(TextureBarrier{s.tex, State::CopyDst, State::ShaderReadFragment});
    dev.Submit(QueueType::Graphics, cl);
    dev.WaitIdle();
    dev.Destroy(staging);
    return s;
}

static void ReadTarget(Device& dev, Scene& s, std::vector<uint8_t>& out) {
    BufferHandle rb = MakeBuffer(dev, 64 * 256, BufferUsage::CopyDst, MemoryClass::Readback, "target-rb");
    CommandList* cl = dev.AcquireCommandList(QueueType::Graphics);
    cl->Barrier(TextureBarrier{s.target, State::ColorTarget, State::CopySrc});
    BufferTextureCopy r;
    r.bufferRowPitch = 256;
    r.width = r.height = 64;
    cl->CopyTextureToBuffer(rb, s.target, std::span(&r, 1));
    cl->Barrier(TextureBarrier{s.target, State::CopySrc, State::ColorTarget});
    dev.Submit(QueueType::Graphics, cl);
    dev.WaitIdle();
    dev.Invalidate(rb, 0, 64 * 256);
    out.assign(dev.Map(rb), dev.Map(rb) + 64 * 256);
    dev.Destroy(rb);
}

static const uint8_t* Pixel(const std::vector<uint8_t>& img, int x, int y) { return &img[static_cast<size_t>(y) * 256 + x * 4]; }

// pixel at the middle of instance i's triangle
static const uint8_t* InstancePixel(const std::vector<uint8_t>& img, int i) {
    const float cx = -0.75f + 0.5f * (i % 4);
    const float cy = i < 4 ? -0.5f : 0.5f;
    return Pixel(img, static_cast<int>((cx + 1.0f) * 32.0f), static_cast<int>((1.0f - cy) * 32.0f));
}

static void BeginPass(CommandList* cl, Scene& s) {
    RenderingDesc rd;
    rd.colorCount = 1;
    rd.colors[0].texture = s.target;
    rd.colors[0].load = LoadOp::Clear;
    cl->BeginRendering(rd);
}

static void TestRaster(Device& dev) {
    Scene s = BuildScene(dev);
    CHECK(s.pipeFront && s.pipeBack && s.group);
    const float tint[4] = {1.0f, 0.5f, 0.25f, 1.0f};
    const uint32_t offsets[] = {0};

    for (int variant = 0; variant < 2; variant++) {
        CommandList* cl = dev.AcquireCommandList(QueueType::Graphics);
        cl->Barrier(TextureBarrier{s.target, variant == 0 ? State::Undefined : State::ColorTarget, State::ColorTarget});
        BeginPass(cl, s);
        cl->SetPipeline(variant == 0 ? s.pipeFront : s.pipeBack);
        cl->SetBindGroup(0, s.group, offsets);
        cl->PushConstants(tint, sizeof(tint));
        cl->SetVertexBuffer(0, s.vertices);
        cl->SetVertexBuffer(1, s.instances);
        cl->SetIndexBuffer(s.indices, IndexType::U16);
        cl->DrawIndexed(3, 8, 0, 0, 0);
        cl->EndRendering();
        dev.Submit(QueueType::Graphics, cl);

        std::vector<uint8_t> img;
        ReadTarget(dev, s, img);
        if (variant == 0) {
            // all eight instances drawn, tinted white texture, upright (instance 0 sits in the lower half)
            for (int i = 0; i < 8; i++) {
                const uint8_t* p = InstancePixel(img, i);
                CHECK(p[0] == 255 && p[1] >= 127 && p[1] <= 129 && p[2] >= 63 && p[2] <= 65 && p[3] == 255);
            }
            CHECK(Pixel(img, 1, 1)[3] == 0);
            // the triangle apex points up: just under the apex is covered, just outside the base corner is not
            CHECK(Pixel(img, 8, 48 - 4)[0] == 255);
            CHECK(Pixel(img, 8, 48 + 7)[0] == 0 || Pixel(img, 8, 48 + 7)[0] == 255);
        } else {
            // clockwise-front pipeline culls the counter-clockwise triangles: winding matches d3d
            for (int i = 0; i < 8; i++)
                CHECK(InstancePixel(img, i)[3] == 0);
        }
    }

    // second dynamic uniform slot doubles the scale: triangles grow past their old edge
    {
        CommandList* cl = dev.AcquireCommandList(QueueType::Graphics);
        cl->Barrier(TextureBarrier{s.target, State::ColorTarget, State::ColorTarget});
        BeginPass(cl, s);
        cl->SetPipeline(s.pipeFront);
        const uint32_t second[] = {256};
        cl->SetBindGroup(0, s.group, second);
        cl->PushConstants(tint, sizeof(tint));
        cl->SetVertexBuffer(0, s.vertices);
        cl->SetVertexBuffer(1, s.instances);
        cl->SetIndexBuffer(s.indices, IndexType::U16);
        cl->DrawIndexed(3, 1, 0, 0, 0);
        cl->EndRendering();
        dev.Submit(QueueType::Graphics, cl);
        std::vector<uint8_t> img;
        ReadTarget(dev, s, img);
        // instance 0 at x=8,y=48 now spans 12.8px wide at the base: x=8-5 is covered only with the larger scale
        CHECK(Pixel(img, 8 - 4, 48 + 2)[0] == 255);
    }

    ExpectNoErrors(dev, "raster");
    for (auto h : {s.vertices, s.instances, s.indices, s.ubo})
        dev.Destroy(h);
    dev.Destroy(s.tex);
    dev.Destroy(s.target);
    dev.Destroy(s.group);
    dev.Destroy(s.pipeFront);
    dev.Destroy(s.pipeBack);
    dev.Destroy(s.pipeLayout);
    dev.Destroy(s.layout);
    dev.Destroy(s.vs);
    dev.Destroy(s.fs);
    dev.WaitIdle();
    CHECK(dev.Stats().liveBuffers == 0);
    CHECK(dev.Stats().liveTextures == 0);
}

// compute decides which instances draw by writing the indirect arguments
static void TestComputeIndirect(Device& dev) {
    Scene s = BuildScene(dev);
    ShaderHandle cs = MakeShader(dev, "cull.cs.spv", ShaderStage::Compute);
    const BindLayoutEntry entries[] = {{0, BindingType::StorageBufferRW, ShaderStage::Compute}, {1, BindingType::StorageBufferRW, ShaderStage::Compute}};
    BindLayoutHandle cl_layout = dev.CreateBindLayout({entries});
    PipelineLayoutDesc pl;
    pl.sets[0] = cl_layout;
    PipelineLayoutHandle cpl = dev.CreatePipelineLayout(pl);
    ComputePipelineDesc cd;
    cd.compute = cs;
    cd.layout = cpl;
    PipelineHandle cpipe = dev.CreateComputePipeline(cd);
    CHECK(cpipe);

    const uint64_t argBytes = sizeof(DrawIndexedIndirectCommand) * 8;
    BufferHandle args = MakeBuffer(dev, argBytes, BufferUsage::Storage | BufferUsage::Indirect | BufferUsage::CopyDst, MemoryClass::GpuOnly, "args");
    BufferHandle counter = MakeBuffer(dev, 16, BufferUsage::Storage | BufferUsage::Indirect | BufferUsage::CopyDst, MemoryClass::GpuOnly, "counter");
    const BindGroupEntry ge[] = {{0, 0, {args, 0, argBytes}, {}, {}}, {1, 0, {}, {}, {}}};
    BindGroupEntry group[2] = {ge[0], ge[1]};
    group[1].buffer = {counter, 0, 16};
    BindGroupDesc gd;
    gd.layout = cl_layout;
    gd.entries = group;
    BindGroupHandle cg = dev.CreateBindGroup(gd);

    const float tint[4] = {1.0f, 0.5f, 0.25f, 1.0f};
    const uint32_t offsets[] = {0};
    CommandList* cl = dev.AcquireCommandList(QueueType::Graphics);
    cl->Barrier(TextureBarrier{s.target, State::Undefined, State::ColorTarget});
    const BufferBarrier zero[] = {{args, State::Undefined, State::CopyDst}, {counter, State::Undefined, State::CopyDst}};
    cl->Barrier({}, zero);
    cl->FillBuffer(args, 0, argBytes, 0);
    cl->FillBuffer(counter, 0, 16, 0);
    const BufferBarrier toShader[] = {{args, State::CopyDst, State::ShaderWrite}, {counter, State::CopyDst, State::ShaderWrite}};
    cl->Barrier({}, toShader);
    cl->SetPipeline(cpipe);
    cl->SetBindGroup(0, cg, {});
    cl->Dispatch(1, 1, 1);
    const BufferBarrier toDraw[] = {{args, State::ShaderWrite, State::IndirectArgument}, {counter, State::ShaderWrite, State::IndirectArgument}};
    cl->Barrier({}, toDraw);
    BeginPass(cl, s);
    cl->SetPipeline(s.pipeFront);
    cl->SetBindGroup(0, s.group, offsets);
    cl->PushConstants(tint, sizeof(tint));
    cl->SetVertexBuffer(0, s.vertices);
    cl->SetVertexBuffer(1, s.instances);
    cl->SetIndexBuffer(s.indices, IndexType::U16);
    if (dev.Caps().features.drawIndirectCount)
        cl->DrawIndexedIndirectCount(args, 0, counter, 0, 8, sizeof(DrawIndexedIndirectCommand));
    else
        cl->DrawIndexedIndirect(args, 0, 8, sizeof(DrawIndexedIndirectCommand));
    cl->EndRendering();
    dev.Submit(QueueType::Graphics, cl);

    std::vector<uint8_t> img;
    ReadTarget(dev, s, img);
    for (int i = 0; i < 8; i++) {
        const bool drawn = i == 0 || i == 2 || i == 5;
        CHECK((InstancePixel(img, i)[3] == 255) == drawn);
    }
    ExpectNoErrors(dev, "compute indirect");
    std::printf("  indirect path: %s\n", dev.Caps().features.drawIndirectCount ? "draw-indirect-count" : "multi-draw-indirect");

    dev.Destroy(cg);
    dev.Destroy(args);
    dev.Destroy(counter);
    dev.Destroy(cpipe);
    dev.Destroy(cpl);
    dev.Destroy(cl_layout);
    dev.Destroy(cs);
    for (auto h : {s.vertices, s.instances, s.indices, s.ubo})
        dev.Destroy(h);
    dev.Destroy(s.tex);
    dev.Destroy(s.target);
    dev.Destroy(s.group);
    dev.Destroy(s.pipeFront);
    dev.Destroy(s.pipeBack);
    dev.Destroy(s.pipeLayout);
    dev.Destroy(s.layout);
    dev.Destroy(s.vs);
    dev.Destroy(s.fs);
    dev.WaitIdle();
}

static void TestNativeTexture(Device& dev) {
    TextureDesc td;
    td.width = 64;
    td.height = 32;
    td.format = Format::RGBA8Unorm;
    td.usage = TextureUsage::Sampled | TextureUsage::ColorTarget;
    td.name = "native";
    TextureHandle t = dev.CreateTexture(td);
    const NativeTexture n = dev.NativeOf(t, State::ShaderReadFragment);
    CHECK(n.resource != nullptr);
    CHECK(n.width == 64 && n.height == 32 && n.mips == 1);
    CHECK(n.nativeFormat != 0);
    if (g_api == Api::Vulkan) {
        CHECK(n.view != nullptr && n.memory != nullptr);
        // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        CHECK(n.state == 5);
    }
    dev.Destroy(t);
    CHECK(dev.NativeOf(t, State::ShaderReadFragment).resource == nullptr);
    ExpectNoErrors(dev, "native texture");
}

static void TestFramesAndRetire(Device& dev) {
    for (int f = 0; f < 9; f++) {
        dev.BeginFrame();
        BufferHandle b = MakeBuffer(dev, 4096, BufferUsage::CopyDst, MemoryClass::GpuOnly, "frame-temp");
        BufferHandle up = MakeBuffer(dev, 4096, BufferUsage::CopySrc, MemoryClass::Upload, "frame-up");
        CommandList* cl = dev.AcquireCommandList(QueueType::Graphics);
        cl->Barrier(BufferBarrier{b, State::Undefined, State::CopyDst});
        cl->CopyBuffer(b, 0, up, 0, 4096);
        dev.Submit(QueueType::Graphics, cl);
        dev.Destroy(b);
        dev.Destroy(up);
        dev.EndFrame();
    }
    dev.WaitIdle();
    CHECK(dev.Stats().liveBuffers == 0);
    ExpectNoErrors(dev, "frames");
}

static void TestThreadedRecording(Device& dev) {
    constexpr int kThreads = 4;
    BufferHandle src = MakeBuffer(dev, 256, BufferUsage::CopySrc, MemoryClass::Upload, "t-src");
    std::memset(dev.Map(src), 0x5A, 256);
    BufferHandle dst[kThreads];
    CommandList* lists[kThreads];
    for (int i = 0; i < kThreads; i++)
        dst[i] = MakeBuffer(dev, 256, BufferUsage::CopyDst, MemoryClass::Readback, "t-dst");
    dev.BeginFrame();
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; i++)
        threads.emplace_back([&, i] {
            lists[i] = dev.AcquireCommandList(QueueType::Graphics);
            lists[i]->Barrier(BufferBarrier{dst[i], State::Undefined, State::CopyDst});
            lists[i]->CopyBuffer(dst[i], 0, src, 0, 256);
        });
    for (auto& t : threads)
        t.join();
    SubmitDesc sd;
    sd.lists = std::span<CommandList* const>(lists, kThreads);
    dev.Submit(QueueType::Graphics, sd);
    dev.EndFrame();
    dev.WaitIdle();
    for (int i = 0; i < kThreads; i++) {
        dev.Invalidate(dst[i], 0, 256);
        CHECK(dev.Map(dst[i])[0] == 0x5A && dev.Map(dst[i])[255] == 0x5A);
        dev.Destroy(dst[i]);
    }
    dev.Destroy(src);
    ExpectNoErrors(dev, "threaded recording");
}

static void TestValidatorCatchesMistakes(Device& dev) {
    BufferHandle b = MakeBuffer(dev, 64, BufferUsage::CopyDst | BufferUsage::CopySrc, MemoryClass::GpuOnly, "v");
    CommandList* cl = dev.AcquireCommandList(QueueType::Graphics);
    cl->Barrier(BufferBarrier{b, State::Undefined, State::CopyDst});
    // claims it was a vertex buffer: the state tracker must say so
    cl->Barrier(BufferBarrier{b, State::VertexBuffer, State::CopySrc});
    dev.Submit(QueueType::Graphics, cl);
    dev.WaitIdle();
    CHECK(!dev.DebugReport().empty());
    dev.Destroy(b);
}

static void TestQueriesAndCache(Device& dev) {
    if (dev.Caps().features.timestampQueries) {
        QuerySetHandle q = dev.CreateQuerySet({4, "ts"});
        BufferHandle rb = MakeBuffer(dev, 64, BufferUsage::CopyDst, MemoryClass::Readback, "ts-rb");
        CommandList* cl = dev.AcquireCommandList(QueueType::Graphics);
        cl->WriteTimestamp(q, 0);
        cl->WriteTimestamp(q, 1);
        cl->Barrier(BufferBarrier{rb, State::Undefined, State::CopyDst});
        cl->ResolveQueries(q, 0, 2, rb, 0);
        dev.Submit(QueueType::Graphics, cl);
        dev.WaitIdle();
        dev.Invalidate(rb, 0, 64);
        const uint64_t* t = reinterpret_cast<const uint64_t*>(dev.Map(rb));
        CHECK(t[1] >= t[0]);
        dev.Destroy(q);
        dev.Destroy(rb);
    }
    const auto blob = dev.SerializePipelineCache();
    std::printf("  pipeline cache blob: %zu bytes\n", blob.size());
    ExpectNoErrors(dev, "queries");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: rhi_gpu_selftest <shader dir> [vulkan|d3d12]\n");
        return 2;
    }
    g_shaderDir = argv[1];
    if (argc > 2)
        g_api = ApiFromId(argv[2]);

    InstanceOptions io;
    io.validation = true;
    auto instance = CreateInstance(g_api, io);
    if (!instance || instance->Adapters().empty()) {
        std::printf("no %s driver on this machine: skipped\n", ApiName(g_api));
        return 77;
    }
    uint32_t pick = 0;
    for (const auto& a : instance->Adapters()) {
        std::printf("adapter %u: %s (%s) usable=%d %s\n", a.index, a.name.c_str(), VendorName(a.vendorId), a.usable, a.unusableReason.c_str());
        if (a.usable)
            pick = a.index;
    }
    DeviceOptions dopts;
    dopts.adapterIndex = pick;
    dopts.validation = true;
    dopts.stateValidation = true;
    auto dev = instance->CreateDevice(dopts);
    if (!dev) {
        std::fprintf(stderr, "device creation failed\n");
        return 1;
    }

    TestBufferRoundTrip(*dev);
    TestFillAndFences(*dev);
    TestRaster(*dev);
    TestComputeIndirect(*dev);
    TestNativeTexture(*dev);
    TestFramesAndRetire(*dev);
    TestThreadedRecording(*dev);
    TestValidatorCatchesMistakes(*dev);
    TestQueriesAndCache(*dev);

    dev->WaitIdle();
    ExpectNoErrors(*dev, "shutdown");
    dev.reset();
    instance.reset();
    if (g_failed) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failed);
        return 1;
    }
    std::puts("rhi_vulkan_selftest ok");
    return 0;
}
