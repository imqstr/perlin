#include "rhi/Backends.hpp"
#include "rhi/common/HandlePool.hpp"
#include "rhi/common/RetireQueue.hpp"

#include <cstdio>
#include <cstring>

namespace rhi {
    std::vector<std::string> NullDeviceTakeErrors(Device& device);
}

static int g_failed = 0;
#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
            g_failed++;                                                              \
        }                                                                            \
    } while (0)

using namespace rhi;

static std::unique_ptr<Device> MakeDevice() {
    auto instance = CreateInstance(Api::None);
    DeviceOptions options;
    options.stateValidation = true;
    return instance->CreateDevice(options);
}

static void TestHandleGenerations() {
    HandlePool<BufferTag, int> pool;
    BufferHandle a = pool.Alloc(7);
    CHECK(pool.Get(a) && *pool.Get(a) == 7);
    pool.Free(a);
    CHECK(pool.Get(a) == nullptr);
    BufferHandle b = pool.Alloc(9);
    CHECK(b.Index() == a.Index());
    CHECK(b.Generation() != a.Generation());
    CHECK(pool.Get(a) == nullptr);
    CHECK(pool.Get(b) && *pool.Get(b) == 9);
    CHECK(pool.Get(BufferHandle{}) == nullptr);
}

static void TestRetire() {
    RetireQueue q;
    QueueSerials submitted{};
    int freed = 0;
    submitted[0] = 1;
    q.Push(submitted, [&] { freed++; });
    QueueSerials done{};
    q.Collect(done);
    CHECK(freed == 0);
    done[0] = 1;
    q.Collect(done);
    CHECK(freed == 1);
}

static void TestCopies() {
    auto dev = MakeDevice();
    BufferDesc up;
    up.size = 256;
    up.usage = BufferUsage::CopySrc;
    up.memory = MemoryClass::Upload;
    up.name = "up";
    BufferHandle src = dev->CreateBuffer(up);
    BufferDesc rb = up;
    rb.usage = BufferUsage::CopyDst;
    rb.memory = MemoryClass::Readback;
    rb.name = "rb";
    BufferHandle dst = dev->CreateBuffer(rb);
    BufferDesc gpu;
    gpu.size = 256;
    gpu.usage = BufferUsage::CopySrc | BufferUsage::CopyDst;
    gpu.name = "gpu";
    BufferHandle mid = dev->CreateBuffer(gpu);

    uint8_t* p = dev->Map(src);
    CHECK(p != nullptr);
    CHECK(dev->Map(mid) == nullptr);
    for (int i = 0; i < 256; i++)
        p[i] = static_cast<uint8_t>(i);

    CommandList* cl = dev->AcquireCommandList(QueueType::Graphics);
    cl->Barrier(BufferBarrier{mid, State::Undefined, State::CopyDst});
    cl->CopyBuffer(mid, 0, src, 0, 256);
    cl->Barrier(BufferBarrier{mid, State::CopyDst, State::CopySrc});
    cl->CopyBuffer(dst, 0, mid, 0, 256);
    dev->Submit(QueueType::Graphics, cl);
    CHECK(std::memcmp(dev->Map(dst), p, 256) == 0);
    CHECK(NullDeviceTakeErrors(*dev).empty());

    // wrong 'before' state must be flagged
    cl = dev->AcquireCommandList(QueueType::Graphics);
    cl->Barrier(BufferBarrier{mid, State::ShaderWrite, State::CopySrc});
    dev->Submit(QueueType::Graphics, cl);
    CHECK(!NullDeviceTakeErrors(*dev).empty());

    // copy without transition into CopyDst
    cl = dev->AcquireCommandList(QueueType::Graphics);
    cl->Barrier(BufferBarrier{mid, State::CopySrc, State::ShaderRead});
    cl->CopyBuffer(mid, 0, src, 0, 16);
    dev->Submit(QueueType::Graphics, cl);
    CHECK(!NullDeviceTakeErrors(*dev).empty());
}

static void TestTextureRoundTrip() {
    auto dev = MakeDevice();
    TextureDesc td;
    td.format = Format::RGBA8Unorm;
    td.width = 8;
    td.height = 4;
    td.usage = TextureUsage::Sampled | TextureUsage::CopyDst | TextureUsage::CopySrc;
    td.name = "tex";
    TextureHandle tex = dev->CreateTexture(td);

    BufferDesc up;
    up.size = 8 * 4 * 4;
    up.usage = BufferUsage::CopySrc | BufferUsage::CopyDst;
    up.memory = MemoryClass::Upload;
    BufferHandle in = dev->CreateBuffer(up);
    BufferHandle out = dev->CreateBuffer(up);
    uint8_t* p = dev->Map(in);
    for (uint32_t i = 0; i < up.size; i++)
        p[i] = static_cast<uint8_t>(i * 7);

    BufferTextureCopy region;
    region.width = 8;
    region.height = 4;
    CommandList* cl = dev->AcquireCommandList(QueueType::Graphics);
    cl->Barrier(TextureBarrier{tex, State::Undefined, State::CopyDst});
    cl->CopyBufferToTexture(tex, in, std::span(&region, 1));
    cl->Barrier(TextureBarrier{tex, State::CopyDst, State::CopySrc});
    cl->CopyTextureToBuffer(out, tex, std::span(&region, 1));
    dev->Submit(QueueType::Graphics, cl);
    CHECK(std::memcmp(dev->Map(out), p, up.size) == 0);
    CHECK(NullDeviceTakeErrors(*dev).empty());
}

static void TestDrawValidation() {
    auto dev = MakeDevice();
    const uint8_t blob[4] = {};
    ShaderDesc sd;
    sd.code = blob;
    ShaderHandle vs = dev->CreateShader(sd);
    PipelineLayoutHandle layout = dev->CreatePipelineLayout({});
    GraphicsPipelineDesc gp;
    gp.vertex = vs;
    gp.layout = layout;
    gp.colorFormats[0] = Format::BGRA8Unorm;
    gp.colorCount = 1;
    PipelineHandle pipe = dev->CreateGraphicsPipeline(gp);

    TextureDesc td;
    td.format = Format::BGRA8Unorm;
    td.width = td.height = 16;
    td.usage = TextureUsage::ColorTarget;
    TextureHandle rt = dev->CreateTexture(td);

    CommandList* cl = dev->AcquireCommandList(QueueType::Graphics);
    cl->Barrier(TextureBarrier{rt, State::Undefined, State::ColorTarget});
    RenderingDesc rd;
    rd.colorCount = 1;
    rd.colors[0].texture = rt;
    cl->BeginRendering(rd);
    cl->SetPipeline(pipe);
    cl->Draw(3, 1, 0, 0);
    cl->EndRendering();
    dev->Submit(QueueType::Graphics, cl);
    CHECK(NullDeviceTakeErrors(*dev).empty());
    CHECK(dev->Stats().draws == 1);

    // drawing with a pass that has no colour target must be rejected
    cl = dev->AcquireCommandList(QueueType::Graphics);
    RenderingDesc empty;
    cl->BeginRendering(empty);
    cl->SetPipeline(pipe);
    cl->Draw(3, 1, 0, 0);
    cl->EndRendering();
    dev->Submit(QueueType::Graphics, cl);
    CHECK(!NullDeviceTakeErrors(*dev).empty());
}

static void TestDeferredDestroy() {
    auto dev = MakeDevice();
    BufferDesc d;
    d.size = 16;
    d.usage = BufferUsage::CopyDst;
    BufferHandle b = dev->CreateBuffer(d);
    dev->Destroy(b);
    dev->WaitIdle();
    CHECK(dev->GetDesc(b) == nullptr);
    CHECK(dev->Stats().liveBuffers == 0);
}

static void TestSwapchain() {
    auto dev = MakeDevice();
    SwapchainDesc sd;
    sd.width = 64;
    sd.height = 32;
    auto sc = dev->CreateSwapchain(sd);
    Swapchain::Image img;
    CHECK(sc->Acquire(img));
    CHECK(!sc->Resize(0, 0));
    CHECK(!sc->Acquire(img));
    CHECK(sc->Resize(128, 64));
    CHECK(sc->Acquire(img));
}

int main() {
    TestHandleGenerations();
    TestRetire();
    TestCopies();
    TestTextureRoundTrip();
    TestDrawValidation();
    TestDeferredDestroy();
    TestSwapchain();
    if (g_failed) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failed);
        return 1;
    }
    std::puts("rhi_null_selftest ok");
    return 0;
}
