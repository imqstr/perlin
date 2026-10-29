// renderer selection policy on synthetic machines, plus a real probe of whatever the host has
#include "rhi/DeviceProfile.hpp"

#include <cstdio>

using namespace rhi;

static int g_failed = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            g_failed++;                                                          \
        }                                                                        \
    } while (0)

static Candidate Make(Api api, uint32_t index, const char* name, uint32_t vendor, AdapterType type, uint64_t vramGiB,
                      uint64_t sharedGiB = 0) {
    Candidate c;
    c.adapter.api = api;
    c.adapter.index = index;
    c.adapter.name = name;
    c.adapter.vendorId = vendor;
    c.adapter.deviceId = 0x1234 + index;
    c.adapter.type = type;
    c.adapter.dedicatedVram = vramGiB << 30;
    c.adapter.sharedMemory = sharedGiB << 30;
    c.adapter.driverVersion = 555;
    return c;
}

static void WithCaps(Candidate& c) {
    c.haveCaps = true;
    c.caps.adapter = c.adapter;
    c.caps.features.multiDrawIndirect = true;
    c.caps.features.drawIndirectCount = true;
    c.caps.features.asyncCompute = true;
    c.caps.features.tearing = true;
    c.caps.limits.maxAnisotropy = 16;
}

static Probe Machine(std::initializer_list<Candidate> list) {
    Probe p;
    p.candidates = list;
    return p;
}

static void TestDesktopNvidia() {
    Candidate vk = Make(Api::Vulkan, 0, "RTX 4080", vendor_id::kNvidia, AdapterType::Discrete, 16);
    Candidate dx = Make(Api::D3D12, 0, "RTX 4080", vendor_id::kNvidia, AdapterType::Discrete, 16);
    Candidate d11 = Make(Api::D3D11, 0, "RTX 4080", vendor_id::kNvidia, AdapterType::Discrete, 16);
    Candidate igpu = Make(Api::D3D12, 1, "UHD", vendor_id::kIntel, AdapterType::Integrated, 0, 16);
    WithCaps(vk);
    WithCaps(dx);
    const RenderProfile p = ChooseProfile(Machine({d11, igpu, vk, dx}), {}, {}, true, false);
    CHECK(p.api == Api::D3D12);
    CHECK(p.adapterIndex == 0);
    CHECK(p.tier == Tier::Ultra);
    CHECK(p.gpuCulling && p.asyncCompute && p.frameGeneration && p.tearing);
    CHECK(p.renderDistance == 32);
    CHECK(!p.upscalers.empty() && p.upscalers.front() == Upscaler::Dlss);
    CHECK(p.upscalers.back() == Upscaler::Native);
    // vulkan is next, and d3d11 is the last resort among the discrete card's apis
    CHECK(p.fallbacks.size() == 4);
    CHECK(p.fallbacks[1].adapter.api == Api::Vulkan);
    CHECK(p.fallbacks[2].adapter.api == Api::D3D11);
    CHECK(p.fallbacks.back().adapter.type == AdapterType::Integrated);
}

static void TestLinuxPrefersVulkan() {
    Candidate vk = Make(Api::Vulkan, 0, "RX 7800", vendor_id::kAmd, AdapterType::Discrete, 16);
    WithCaps(vk);
    const RenderProfile p = ChooseProfile(Machine({vk}), {}, {}, false, false);
    CHECK(p.api == Api::Vulkan);
    CHECK(p.upscalers.front() == Upscaler::Fsr);
}

static void TestIntegratedIsCapped() {
    Candidate igpu = Make(Api::D3D12, 0, "Radeon 780M", vendor_id::kAmd, AdapterType::Integrated, 0, 32);
    WithCaps(igpu);
    igpu.caps.features.asyncCompute = true;
    const RenderProfile p = ChooseProfile(Machine({igpu}), {}, {}, true, false);
    CHECK(p.tier == Tier::Medium);
    CHECK(!p.asyncCompute);
    CHECK(!p.frameGeneration);
}

static void TestSoftwareLast() {
    Candidate cpu = Make(Api::Vulkan, 0, "llvmpipe", 0x10005, AdapterType::Cpu, 0, 8);
    Candidate old = Make(Api::D3D11, 0, "GT 710", vendor_id::kNvidia, AdapterType::Discrete, 1);
    const RenderProfile p = ChooseProfile(Machine({cpu, old}), {}, {}, true, false);
    CHECK(p.api == Api::D3D11);
    CHECK(p.tier == Tier::Minimal);
    CHECK(p.fallbacks.back().adapter.type == AdapterType::Cpu);
    const RenderProfile only = ChooseProfile(Machine({cpu}), {}, {}, false, false);
    CHECK(only.api == Api::Vulkan && only.tier == Tier::Minimal);
}

static void TestUserOverride() {
    Candidate vk = Make(Api::Vulkan, 0, "RTX 4080", vendor_id::kNvidia, AdapterType::Discrete, 16);
    Candidate dx = Make(Api::D3D12, 0, "RTX 4080", vendor_id::kNvidia, AdapterType::Discrete, 16);
    UserChoice u;
    u.api = Api::Vulkan;
    RenderProfile p = ChooseProfile(Machine({vk, dx}), u, {}, true, false);
    CHECK(p.api == Api::Vulkan);
    // an api that is not there falls back to automatic choice
    u.api = Api::Metal;
    p = ChooseProfile(Machine({vk, dx}), u, {}, true, false);
    CHECK(p.api == Api::D3D12);
    u = {};
    u.autoTier = false;
    u.tier = Tier::Low;
    u.allowFrameGeneration = false;
    p = ChooseProfile(Machine({dx}), u, {}, true, false);
    CHECK(p.tier == Tier::Low && p.renderDistance == 8 && !p.frameGeneration);
}

static void TestFailureMemory() {
    Candidate vk = Make(Api::Vulkan, 0, "GPU", vendor_id::kAmd, AdapterType::Discrete, 8);
    Candidate dx = Make(Api::D3D12, 0, "GPU", vendor_id::kAmd, AdapterType::Discrete, 8);
    std::vector<KnownFailure> failures;
    RecordFailure(failures, dx.adapter);
    // one failure is bad luck
    CHECK(ChooseProfile(Machine({vk, dx}), {}, failures, true, false).api == Api::D3D12);
    RecordFailure(failures, dx.adapter);
    CHECK(ChooseProfile(Machine({vk, dx}), {}, failures, true, false).api == Api::Vulkan);
    // a new driver gets another chance
    dx.adapter.driverVersion = 556;
    CHECK(ChooseProfile(Machine({vk, dx}), {}, failures, true, false).api == Api::D3D12);

    const std::vector<KnownFailure> round = ParseFailures(SerializeFailures(failures));
    CHECK(round.size() == 1);
    CHECK(round[0].api == Api::D3D12 && round[0].count == 2 && round[0].driverVersion == 555);
}

static void TestNothingUsable() {
    Candidate bad = Make(Api::Vulkan, 0, "ancient", 0x1, AdapterType::Discrete, 1);
    bad.adapter.usable = false;
    const RenderProfile p = ChooseProfile(Machine({bad}), {}, {}, false, false);
    CHECK(p.api == Api::None);
    CHECK(p.fallbacks.empty());
}

static void TestRealProbe() {
    ProbeOptions o;
    o.readCaps = true;
    const Probe probe = ProbeSystem(o);
    for (const Candidate& c : probe.candidates)
        std::printf("candidate %s %s score %.0f caps=%d\n", ApiName(c.adapter.api), c.adapter.name.c_str(), c.score, c.haveCaps);
    const RenderProfile p = ChooseProfile(probe, {}, {}, false, false);
    std::printf("profile: %s tier %s\n", ApiName(p.api), TierName(p.tier));
    for (const std::string& r : p.reasons)
        std::printf("  %s\n", r.c_str());
    // the null backend is always compiled in and never listed as a candidate renderer
    for (const Candidate& c : probe.candidates)
        CHECK(c.adapter.api != Api::None);
}

int main() {
    TestDesktopNvidia();
    TestLinuxPrefersVulkan();
    TestIntegratedIsCapped();
    TestSoftwareLast();
    TestUserOverride();
    TestFailureMemory();
    TestNothingUsable();
    TestRealProbe();
    if (g_failed) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failed);
        return 1;
    }
    std::puts("rhi_profile_test ok");
    return 0;
}
