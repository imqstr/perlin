#include "client/player/Targeting.hpp"
#include <cstdio>
#include <limits>
using namespace perlin;
int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* what) { if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); } };
    check(PickRange(false, 1) == double(5.7f), "mouse block range is binary32 5.7");
    check(PickRange(true, 1) == double(5.7f), "creative mouse follows the input-mode branch");
    check(PickRange(false, 3) == double(5.6f), "controller block range");
    check(PickRange(true, 2) == 12, "creative touch block range");
    check(EntityPickRange(false) == 3 && EntityPickRange(true) == 7, "separate entity caps");
    check(targeting::EntityBeforeLimit(2.899f, 3), "survival inside native boundary");
    check(!targeting::EntityBeforeLimit(2.9f, 3), "strict hit-plus-radius boundary");
    check(!targeting::EntityBeforeLimit(3.5, 3), "no block reach for PvP");
    check(!targeting::EntityBeforeLimit(1.95, 2), "block occlusion has no extra allowance");
    double t; int face;
    const double p = targeting::PickRadius;
    // 0.6-wide player: pick radius admits a ray 0.05 outside the box
    check(targeting::RayBox({0.35,1.62,0}, {0,0,1}, {-0.3-p,-p,2.7-p}, {0.3+p,1.8+p,3.3+p}, 3,t,face), "native pick padding");
    check(!targeting::RayBox({0.401,1.62,0}, {0,0,1}, {-0.3-p,-p,2.7-p}, {0.3+p,1.8+p,3.3+p}, 3,t,face), "outside pick padding");
    check(targeting::RayBox({0,1.62,0}, {0,0,1}, {-0.3-p,-p,2.7-p}, {0.3+p,1.8+p,3.3+p}, 3,t,face)
          && targeting::EntityBeforeLimit(t,3), "in-range player");
    std::printf("targeting: %d failures\n", failures);
    return failures != 0;
}
