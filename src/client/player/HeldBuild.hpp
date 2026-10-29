#pragma once
#include "client/player/PlayerPhysics.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// placement while use stays held, per the game's rules (ClientInstance::_tickBuildAction,
// GameMode::startBuildBlock / continueBuildBlock / stopBuildBlock):
//  - fresh press always places at the crosshair (Start + Built)
//  - held: each attempt waits the repeat delay: 300 ms when sneaking, after an interactive block, or with a
//    snappable block before a build line exists; else min(900 / speed, 180) ms (speed in blocks/s,
//    3d, from this tick's movement); 200 ms standing still
//  - snappable blocks (full cubes, stairs, slabs, fences, panes, bars) form a build line: once a repeat lands one
//    block from the last, only the next cell along the line is placed, and only while the look ray passes
//    through it; moving players extend from the last placed block on the face they move toward
//  - releasing use ends the line
namespace perlin::heldbuild {
    // a block and the face to click (0 down, 1 up, 2 north, 3 south, 4 west, 5 east)
    struct Target {
        glm::ivec3 support{};
        int face = 0;
    };

    struct Hit {
        bool block = false;     // crosshair on a block (direct hit)
        Target target;
        glm::vec3 point{};      // where the ray stopped: hit point, or eye + look * reach without a hit
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

        // placement went through: click on `clicked` produced a block at `cell`; `snappable` / `interactive`
        // describe the held and clicked blocks, as GameMode::useItemOn records them
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
        uint64_t nextBuildMs = 0;  // lastBuildMs once the attempt Next returned succeeds
        bool pendingLine = false;  // attempt in flight is the line's next cell
    };

    // face index along the dominant axis of `v` (Facing::fromVec3), vertical included
    int FacingFromVec(glm::vec3 v);

    // full cubes, stairs, slabs, fences, walls, panes, bars: blocks starting a build line
    bool IsSnappable(const std::string& blockName, const std::vector<physics::Box>& shape);

    // block with collision `shape` (block-local boxes) at `cell` overlaps `body`
    bool OverlapsBody(glm::ivec3 cell, const std::vector<physics::Box>& shape, const physics::Box& body);

    // segment a..b passes through the unit cell (touching counts)
    bool SegmentHitsCell(glm::vec3 a, glm::vec3 b, glm::ivec3 cell);
}
