#include "rhi/vendor/Vendor.hpp"

#include <algorithm>
#include <cmath>

namespace rhi::vendor {
#if defined(RHI_HAS_STREAMLINE) && RHI_HAS_STREAMLINE
    std::unique_ptr<Session> MakeStreamlineSession(Api api, const SessionRequest& request, VendorHooks& hooks, std::string* log);
#endif

    std::unique_ptr<Session> Session::Begin(Api api, const SessionRequest& request, VendorHooks& hooks, std::string* log) {
#if defined(RHI_HAS_STREAMLINE) && RHI_HAS_STREAMLINE
        return MakeStreamlineSession(api, request, hooks, log);
#else
        (void)api;
        (void)request;
        (void)hooks;
        if (log)
            *log += "vendor sdks are not built into this binary; ";
        return nullptr;
#endif
    }

    float ScaleOf(Quality quality) {
        switch (quality) {
        case Quality::Native: return 1.0f;
        case Quality::UltraQuality: return 1.0f / 1.3f;
        case Quality::Quality: return 1.0f / 1.5f;
        case Quality::Balanced: return 1.0f / 1.72f;
        case Quality::Performance: return 0.5f;
        case Quality::UltraPerformance: return 1.0f / 3.0f;
        }
        return 1.0f;
    }

    const char* QualityName(Quality quality) {
        switch (quality) {
        case Quality::Native: return "native";
        case Quality::UltraQuality: return "ultra quality";
        case Quality::Quality: return "quality";
        case Quality::Balanced: return "balanced";
        case Quality::Performance: return "performance";
        case Quality::UltraPerformance: return "ultra performance";
        }
        return "native";
    }

    void RenderSizeFor(Quality quality, uint32_t outputWidth, uint32_t outputHeight, uint32_t& width, uint32_t& height) {
        const float s = ScaleOf(quality);
        auto even = [](float v) { return (std::max)(2u, (static_cast<uint32_t>(std::ceil(v)) + 1u) & ~1u); };
        width = even(static_cast<float>(outputWidth) * s);
        height = even(static_cast<float>(outputHeight) * s);
        width = (std::min)(width, (std::max)(2u, (outputWidth + 1u) & ~1u));
        height = (std::min)(height, (std::max)(2u, (outputHeight + 1u) & ~1u));
    }

    uint32_t JitterPhases(uint32_t renderWidth, uint32_t outputWidth) {
        const float ratio = static_cast<float>(outputWidth) / static_cast<float>(std::max(1u, renderWidth));
        return (std::max)(8u, static_cast<uint32_t>(std::ceil(8.0f * ratio * ratio)));
    }

    namespace {
        float Halton(uint32_t index, uint32_t base) {
            float f = 1.0f, r = 0.0f;
            while (index > 0) {
                f /= static_cast<float>(base);
                r += f * static_cast<float>(index % base);
                index /= base;
            }
            return r;
        }
    }

    std::array<float, 2> JitterFor(uint64_t frame, uint32_t renderWidth, uint32_t outputWidth) {
        const uint32_t phases = JitterPhases(renderWidth, outputWidth);
        const uint32_t i = static_cast<uint32_t>(frame % phases) + 1;
        return {Halton(i, 2) - 0.5f, Halton(i, 3) - 0.5f};
    }
}
