#pragma once
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

namespace perlin {
    // GameMode::getPickRange, china client 0x141f87030; binary32 constants
    inline double PickRange(bool creative, int inputMode, bool vr = false) {
        if (vr) return creative ? 12.0f : 7.0f;
        if (inputMode == 1) return 5.7f;
        if (inputMode == 3 || inputMode == 4) return 5.6f;
        return creative ? 12.0f : 6.7f;
    }
    // InGamePlayScreen::_pick 0x140f0a6e9..0x140f0a713; separate from block reach
    inline double EntityPickRange(bool creative) { return creative ? 7.0f : 3.0f; }
}

namespace perlin::targeting {
        inline bool RayBox(const glm::dvec3& o, const glm::dvec3& d, const glm::dvec3& lo, const glm::dvec3& hi, double maxT, double& tOut, int& face) {
            double t0 = 0.0, t1 = maxT;
            int f = -1;
            for (int a = 0; a < 3; a++) {
                const double origin = o[a], dir = d[a], mn = lo[a], mx = hi[a];
                if (std::fabs(dir) < 1e-12) {
                    if (origin < mn || origin > mx)
                        return false;
                    continue;
                }
                double ta = (mn - origin) / dir, tb = (mx - origin) / dir;
                int fa = dir > 0 ? (a == 0 ? 4 : a == 1 ? 0 : 2) : (a == 0 ? 5 : a == 1 ? 1 : 3);
                if (ta > tb)
                    std::swap(ta, tb);
                if (ta > t0) {
                    t0 = ta;
                    f = fa;
                }
                t1 = std::min(t1, tb);
                if (t0 > t1)
                    return false;
            }
            tOut = t0;
            if (f < 0) {
                const glm::dvec3 ad = glm::abs(d);
                f = ad.y >= ad.x && ad.y >= ad.z ? (d.y > 0 ? 0 : 1) : ad.x >= ad.z ? (d.x > 0 ? 4 : 5) : (d.z > 0 ? 2 : 3);
            }
            face = f;
            return true;
        }
}

namespace perlin::targeting {
    // Actor::getPickRadius 0x14105c580: _pick expands actor aabb by this
    constexpr float PickRadius = 0.1f;
    // _pick 0x140f0b1e2: expanded-box hit + radius strictly before nearest block/range limit;
    // no allowance for targets behind a wall
    inline bool EntityBeforeLimit(double distance, double limit) {
        return static_cast<float>(static_cast<float>(distance) + PickRadius) < static_cast<float>(limit);
    }
}
