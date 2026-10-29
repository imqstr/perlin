// the parts of the vendor layer that need no vendor library: sizing, jitter, and the unavailable path
#include "rhi/vendor/Vendor.hpp"

#include <cmath>
#include <cstdio>
#include <set>

using namespace rhi;
using namespace rhi::vendor;

static int g_failed = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            g_failed++;                                                          \
        }                                                                        \
    } while (0)

int main() {
    uint32_t w = 0, h = 0;
    RenderSizeFor(Quality::Native, 1920, 1080, w, h);
    CHECK(w == 1920 && h == 1080);
    RenderSizeFor(Quality::Quality, 2560, 1440, w, h);
    CHECK(w == 1708 && h == 960);
    RenderSizeFor(Quality::Performance, 3840, 2160, w, h);
    CHECK(w == 1920 && h == 1080);
    RenderSizeFor(Quality::UltraPerformance, 1921, 1081, w, h);
    CHECK(w % 2 == 0 && h % 2 == 0 && w >= 641 && w <= 646);
    // never larger than the output, never degenerate
    RenderSizeFor(Quality::Quality, 1, 1, w, h);
    CHECK(w == 2 && h == 2);
    for (Quality q : {Quality::Native, Quality::UltraQuality, Quality::Quality, Quality::Balanced, Quality::Performance, Quality::UltraPerformance}) {
        RenderSizeFor(q, 1280, 720, w, h);
        CHECK(w <= 1280 && h <= 720 && w >= 2 && h >= 2);
        CHECK(ScaleOf(q) > 0.0f && ScaleOf(q) <= 1.0f);
    }

    // jitter stays inside the pixel, repeats after one cycle, and the cycle covers the pixel evenly enough to differ
    CHECK(JitterPhases(1920, 1920) == 8);
    CHECK(JitterPhases(960, 1920) == 32);
    std::set<std::pair<int, int>> seen;
    const uint32_t phases = JitterPhases(1280, 1920);
    for (uint32_t i = 0; i < phases; i++) {
        const auto j = JitterFor(i, 1280, 1920);
        CHECK(j[0] >= -0.5f && j[0] < 0.5f && j[1] >= -0.5f && j[1] < 0.5f);
        seen.insert({static_cast<int>(std::floor((j[0] + 0.5f) * 8)), static_cast<int>(std::floor((j[1] + 0.5f) * 8))});
        const auto again = JitterFor(i + phases, 1280, 1920);
        CHECK(again[0] == j[0] && again[1] == j[1]);
    }
    CHECK(seen.size() >= phases / 2);

    // without the sdk the session is simply absent
    VendorHooks hooks;
    std::string log;
    auto session = Session::Begin(Api::Vulkan, {}, hooks, &log);
    CHECK(session == nullptr);
    CHECK(!log.empty());

    if (g_failed) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failed);
        return 1;
    }
    std::puts("rhi_vendor_test ok");
    return 0;
}
