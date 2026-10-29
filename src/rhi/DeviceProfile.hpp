#pragma once
#include "rhi/Device.hpp"

#include <string>
#include <vector>

namespace rhi {
    enum class Tier : uint8_t { Minimal, Low, Medium, High, Ultra };
    const char* TierName(Tier tier);

    enum class Upscaler : uint8_t { Native, Spatial, Fsr, XeSS, Dlss };
    const char* UpscalerName(Upscaler u);

    // one api + adapter pair the machine could run
    struct Candidate {
        AdapterInfo adapter;
        // filled when the probe created a device to read real caps; otherwise only the adapter facts are known
        bool haveCaps = false;
        DeviceCaps caps;
        double score = 0.0;
        std::vector<std::string> notes;
    };

    struct ProbeOptions {
        // empty = every compiled api
        std::vector<Api> apis;
        // create a device on the best adapter of each api to read exact features; costs a few hundred ms
        bool readCaps = true;
        bool allowSoftware = true;
        bool validation = false;
    };

    struct Probe {
        std::vector<Candidate> candidates;
        void Sort();
    };

    Probe ProbeSystem(const ProbeOptions& options);

    // what the user asked for; auto means the profile decides
    struct UserChoice {
        Api api = Api::None;
        int adapterIndex = -1;
        Tier tier = Tier::Medium;
        bool autoTier = true;
        bool allowFrameGeneration = true;
        bool allowUpscaling = true;
    };

    // a remembered failure of one api/adapter/driver, so the next launch does not repeat a crash
    struct KnownFailure {
        Api api = Api::None;
        uint32_t vendorId = 0;
        uint32_t deviceId = 0;
        uint64_t driverVersion = 0;
        uint32_t count = 0;
    };

    struct RenderProfile {
        Api api = Api::None;
        uint32_t adapterIndex = 0;
        std::string adapterName;
        Tier tier = Tier::Medium;

        // chunks
        uint32_t renderDistance = 12;
        uint32_t lodDistance = 48;
        uint32_t lodMaxLevel = 3;
        uint64_t meshBudgetBytes = 512ull << 20;
        uint64_t retainBudgetBytes = 128ull << 20;
        uint32_t maxMeshBuildsPerFrame = 4;

        // frame
        uint32_t framesInFlight = 2;
        bool gpuCulling = false;
        bool asyncCompute = false;
        bool tearing = false;
        bool hdr = false;
        uint32_t msaa = 1;
        float renderScale = 1.0f;
        float maxAnisotropy = 1.0f;

        // upscaling and frame generation in the order the vendor layer should try them
        std::vector<Upscaler> upscalers;
        bool frameGeneration = false;
        bool neuralFeatures = false;

        // every api/adapter pair to try in order when the first choice fails to start
        std::vector<Candidate> fallbacks;
        std::vector<std::string> reasons;
    };

    // pure policy: deterministic for a given probe, so it is tested without a gpu
    RenderProfile ChooseProfile(const Probe& probe, const UserChoice& user, const std::vector<KnownFailure>& failures, bool windows,
                                bool apple);

    Tier TierFor(const Candidate& c);
    double ScoreCandidate(const Candidate& c, bool windows, bool apple);

    std::string SerializeFailures(const std::vector<KnownFailure>& failures);
    std::vector<KnownFailure> ParseFailures(const std::string& text);
    void RecordFailure(std::vector<KnownFailure>& failures, const AdapterInfo& adapter);
}
