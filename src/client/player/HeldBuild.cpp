#include "client/player/HeldBuild.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace perlin::heldbuild {
    namespace {
        const glm::ivec3 kFaceNormal[6] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};

        bool Contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
    }

    int FacingFromVec(glm::vec3 v) {
        const float ax = std::fabs(v.x), ay = std::fabs(v.y), az = std::fabs(v.z);
        if (ax >= ay && ax >= az)
            return v.x > 0.0f ? 5 : 4;
        if (ay >= az)
            return v.y > 0.0f ? 1 : 0;
        return v.z > 0.0f ? 3 : 2;
    }

    bool IsSnappable(const std::string& name, const std::vector<physics::Box>& shape) {
        if (Contains(name, "stairs") || Contains(name, "slab") || Contains(name, "fence") || Contains(name, "_wall") ||
            Contains(name, "glass_pane") || Contains(name, "iron_bars"))
            return true;
        return shape.size() == 1 && shape[0].x0 <= 1e-6 && shape[0].y0 <= 1e-6 && shape[0].z0 <= 1e-6 && shape[0].x1 >= 1.0 - 1e-6 &&
               shape[0].y1 >= 1.0 - 1e-6 && shape[0].z1 >= 1.0 - 1e-6;
    }

    bool SegmentHitsCell(glm::vec3 a, glm::vec3 b, glm::ivec3 cell) {
        float enter = 0.0f, exit = 1.0f;
        for (int axis = 0; axis < 3; axis++) {
            const float lo = static_cast<float>(cell[axis]), hi = lo + 1.0f, d = b[axis] - a[axis];
            if (!std::isfinite(a[axis]) || !std::isfinite(d))
                return false;
            if (std::fabs(d) < 1e-9f) {
                if (a[axis] < lo || a[axis] > hi)
                    return false;
                continue;
            }
            const float t0 = (lo - a[axis]) / d, t1 = (hi - a[axis]) / d;
            enter = std::max(enter, std::min(t0, t1));
            exit = std::min(exit, std::max(t0, t1));
            if (enter > exit)
                return false;
        }
        return true;
    }

    bool OverlapsBody(glm::ivec3 cell, const std::vector<physics::Box>& shape, const physics::Box& body) {
        // hair of tolerance: standing exactly on or against the new block is allowed
        constexpr double kEpsilon = 1e-4;
        const physics::Box inner{body.x0 + kEpsilon, body.y0 + kEpsilon, body.z0 + kEpsilon,
                                 body.x1 - kEpsilon, body.y1 - kEpsilon, body.z1 - kEpsilon};
        for (const physics::Box& b : shape)
            if (b.Offset(cell.x, cell.y, cell.z).Intersects(inner))
                return true;
        return false;
    }

    void HeldBuild::Start(uint64_t nowMs, uint64_t simulationTick) {
        active = true;
        lastAttemptTick = simulationTick;
        lastBuildMs = nextBuildMs = nowMs;
        lastInteractive = lastSnappable = false;
        hasDirection = hasLastBuilt = false;
        pendingLine = false;
    }

    void HeldBuild::Stop() {
        active = false;
        hasDirection = hasLastBuilt = false;
        pendingLine = false;
    }

    uint64_t HeldBuild::Delay(glm::vec3 posDelta, bool sneaking) const {
        const float speed = glm::length(posDelta) * 20.0f;
        if (sneaking || lastInteractive || (lastSnappable && !hasDirection))
            return 300;
        if (speed > 0.0f)
            return static_cast<uint64_t>(std::min(900.0f / speed, 180.0f));
        return 200;
    }

    std::optional<Target> HeldBuild::Next(const Hit& hit, glm::vec3 eye, glm::vec3 posDelta, bool sneaking, uint64_t nowMs, uint64_t simulationTick,
                                          const std::function<bool(glm::ivec3)>& canBuildOver) {
        pendingLine = false;
        if (!active || lastAttemptTick == simulationTick)
            return std::nullopt;
        lastAttemptTick = simulationTick;
        // Which block to click (ClientInstance::_tickBuildAction).
        Target click;
        if (hasDirection && lastSnappable) {
            if (!SegmentHitsCell(eye, hit.point, nextBuild))
                return std::nullopt;
            click = {nextBuild - direction, continueFacing};
        } else if (hit.block) {
            const glm::vec3 d = posDelta;
            if (!lastSnappable || hasDirection || !hasLastBuilt || glm::dot(d, d) <= 0.0099999998f)
                click = hit.target;
            else
                click = {lastBuilt, FacingFromVec(d)};
        } else {
            return std::nullopt;
        }

        // may the repeat go now, and the build line (GameMode::continueBuildBlock)
        const glm::ivec3 cell = canBuildOver(click.support) ? click.support : click.support + kFaceNormal[std::clamp(click.face, 0, 5)];
        const uint64_t deadline = lastBuildMs + Delay(posDelta, sneaking);
        if (deadline >= nowMs)
            return std::nullopt;
        // moving: a late repeat banks at most 180 ms of lost time, so the next isn't instant
        nextBuildMs = glm::length(posDelta) > 0.0f ? std::max(deadline, nowMs > 180 ? nowMs - 180 : 0) : nowMs;
        if (sneaking || !lastSnappable)
            return click;
        if (!hasDirection) {
            if (!hasLastBuilt)
                return click;
            const glm::ivec3 d = cell - lastBuilt;
            if (std::abs(d.x) + std::abs(d.y) + std::abs(d.z) == 1) {
                hasDirection = true;
                nextBuild = cell;
                direction = d;
                continueFacing = FacingFromVec(glm::vec3(d));
            }
        }
        if (!hasDirection || cell != nextBuild)
            return std::nullopt;
        pendingLine = true;
        return click;
    }

    void HeldBuild::Built(const Target&, glm::ivec3 cell, bool snappable, bool interactive, uint64_t nowMs, bool fromRepeat) {
        if (fromRepeat && pendingLine)
            nextBuild += direction;
        pendingLine = false;
        lastInteractive = interactive;
        lastSnappable = !interactive && snappable;
        lastBuilt = cell;
        hasLastBuilt = true;
        lastBuildMs = fromRepeat ? nextBuildMs : nowMs;
    }
}
