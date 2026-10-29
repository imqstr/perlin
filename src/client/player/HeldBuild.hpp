#pragma once
#include "client/player/PlayerPhysics.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Placement while the use button stays held, following the game's own rules (ClientInstance::_tickBuildAction and
// GameMode::startBuildBlock / continueBuildBlock / stopBuildBlock):
//  - A fresh press always places at the crosshair (Start + Built).
//  - While held, each attempt waits for the repeat delay: 300 ms when sneaking, after an interactive block, or with a
//    snappable block before a build line exists; otherwise min(900 / speed, 180) ms (speed in blocks per second,
//    3D, from this tick's movement), 200 ms when standing still.
//  - Snappable blocks (full cubes, stairs, slabs, fences, panes and bars) form a build line: once a repeat lands one
//    block from the last one, only the next cell along that line is ever placed, and only while the look ray passes
//    through it. Moving players extend from the last placed block on the face they move toward.
//  - Releasing use ends the line.
namespace perlin::heldbuild {
    // A block and the face of it to click (0 down, 1 up, 2 north, 3 south, 4 west, 5 east).
    struct Target {
        glm::ivec3 support{};
        int face = 0;
    };

    struct Hit {
        bool block = false;     // the crosshair is on a block (a direct hit)
        Target target;
        glm::vec3 point{};      // where the ray stopped: the hit point, or eye + look * reach without a hit
    };

    class HeldBuild {
    public:
        // Fresh press (GameMode::startBuildBlock): forgets the previous block's kind and any line.
        void Start(uint64_t nowMs, uint64_t simulationTick);
        // Use released (GameMode::stopBuildBlock).
        void Stop();

        // The repeat to attempt now, or nothing. `canBuildOver(pos)` says whether placing on `pos` replaces that
        // block (tall grass, snow layer...). `posDelta` is this tick's movement in blocks.
        std::optional<Target> Next(const Hit& hit, glm::vec3 eye, glm::vec3 posDelta, bool sneaking, uint64_t nowMs, uint64_t simulationTick,
                                   const std::function<bool(glm::ivec3)>& canBuildOver);

        // A placement went through: the click on `clicked` produced a block at `cell`. `snappable` / `interactive`
        // describe the held block and the clicked block, as GameMode::useItemOn records them.
        void Built(const Target& clicked, glm::ivec3 cell, bool snappable, bool interactive, uint64_t nowMs, bool fromRepeat);

    private:
        uint64_t Delay(glm::vec3 posDelta, bool sneaking) const;

        std::optional<uint64_t> lastAttemptTick;
        bool active = false;
        bool lastInteractive = false;
        bool lastSnappable = false;
        bool hasDirection = false;
        bool hasLastBuilt = false;
        glm::ivec3 lastBuilt{};
        glm::ivec3 nextBuild{};
        glm::ivec3 direction{};
        int continueFacing = 0;
        uint64_t lastBuildMs = 0;
        uint64_t nextBuildMs = 0;  // lastBuildMs after the attempt Next returned succeeds
        bool pendingLine = false;  // the attempt in flight is the line's next cell
    };

    // Face index along the dominant axis of `v` (Facing::fromVec3), vertical included.
    int FacingFromVec(glm::vec3 v);

    // Full cubes, stairs, slabs, fences, walls, panes and bars: blocks that start a build line.
    bool IsSnappable(const std::string& blockName, const std::vector<physics::Box>& shape);

    // Whether a block with collision `shape` (block-local boxes) placed at `cell` would overlap `body`.
    bool OverlapsBody(glm::ivec3 cell, const std::vector<physics::Box>& shape, const physics::Box& body);

    // Whether the segment a..b passes through the unit cell (touching counts).
    bool SegmentHitsCell(glm::vec3 a, glm::vec3 b, glm::ivec3 cell);
}
