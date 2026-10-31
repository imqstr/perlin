#pragma once
// Java worldgen noise, ported from the 26.3 jar: gradient (perlin) noise with float lerps, octave stacks, and the "normal noise" definitions in worldgen/noise/*.json
#include "world/vanilla/JavaRandom.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace perlin::server::vanilla {

    class PerlinNoise {
    public:
        explicit PerlinNoise(Xoroshiro& random);
        float Get(double x, double y, double z) const;

        // the lattice walk shared with the smeared variant: `smoothY` drives the y blend, `fy` the gradient offset
        float SampleAndLerp(int ix, int iy, int iz, float fx, float fy, float fz, float smoothY) const;
        const std::array<uint8_t, 256>& Perms() const { return perms; }
        double OffsetX() const { return offsetX; }
        double OffsetY() const { return offsetY; }
        double OffsetZ() const { return offsetZ; }

    private:
        int Permute(int i) const { return perms[static_cast<size_t>(i & 255)]; }
        std::array<uint8_t, 256> perms{};
        double offsetX, offsetY, offsetZ;
    };

    // perlin noise whose y coordinate snaps to steps of `fudgeYScale` near the origin (the old terrain noise's vertical smearing)
    class SmearedPerlinNoise : public PerlinNoise {
    public:
        SmearedPerlinNoise(Xoroshiro& random, double fudgeYScale) : PerlinNoise(random), fudgeYScale(fudgeYScale) {}
        float Get(double x, double y, double z) const;
        double FudgeYScale() const { return fudgeYScale; }

    private:
        double fudgeYScale;
    };

    // the old_blended_noise density function: two limit noises blended by a third
    class BlendedNoise {
    public:
        BlendedNoise(double xzScale, double yScale, double xzFactor, double yFactor, double smearScaleMultiplier, Xoroshiro random);
        float Get(int x, int y, int z) const;

        struct Layer {
            SmearedPerlinNoise noise;
            double frequency;
            float amplitude;
        };
        const std::vector<Layer>& MinLimit() const { return minLimit; }
        const std::vector<Layer>& MaxLimit() const { return maxLimit; }
        const std::vector<Layer>& Main() const { return main; }
        double XzMultiplier() const { return xzMultiplier; }
        double YMultiplier() const { return yMultiplier; }
        double XzFactor() const { return xzFactor; }
        double YFactor() const { return yFactor; }

    private:
        static std::vector<Layer> Fbm(Xoroshiro& random, int firstOctave, double smearScale, double amplitude);
        static float Sum(const std::vector<Layer>& stack, double x, double y, double z);
        std::vector<Layer> minLimit, maxLimit, main;
        double xzMultiplier, yMultiplier, xzFactor, yFactor;
    };

    // one entry of worldgen/noise/<name>.json
    struct NoiseParameters {
        double baseAmplitude = 1.0;
        int baseOctave = 0;
        int octaveCount = 1;
        bool normalize = true;
        std::vector<double> amplitudeModifiers;
    };

    // octaves of two perlin noises each, summed in float the way NoiseStack does
    class NormalNoise {
    public:
        // `random` is the generator forked for this noise (positional.fromHashOf(name))
        NormalNoise(const NoiseParameters& parameters, Xoroshiro random);
        float Get(double x, double y, double z) const;

        struct Layer {
            PerlinNoise noise;
            double frequency;
            float amplitude;
        };
        const std::vector<Layer>& Layers() const { return layers; }

    private:
        std::vector<Layer> layers;
    };
}
