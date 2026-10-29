#include <array>
#include "client/player/PlayerPhysics.hpp"
#include "client/player/PerlinPhysicsNumerics.hpp"
#include "client/player/PerlinSineTable.hpp"
#include "client/player/PerlinWaterDrag.hpp"
#include "client/player/PerlinGliding.hpp"
#include "client/player/PerlinRelativeMovement.hpp"
#include "client/player/PerlinMoveFinalization.hpp"
#include "client/player/PerlinSupport.hpp"
#include "client/player/PerlinScaffolding.hpp"
#include "client/player/PerlinPowderSnow.hpp"
#include "client/player/PerlinFallDistance.hpp"
#include "client/player/PerlinWorldCollision.hpp"
#include "client/player/PerlinBlockGeometry.hpp"
#include "client/player/PerlinCollisionCache.hpp"
#include "client/player/PerlinBlockSlowdown.hpp"
#include "client/player/PerlinSneak.hpp"
#include "client/player/PerlinInsideQuery.hpp"
#include "client/player/PerlinJumpRequests.hpp"
#include "client/player/PerlinHoney.hpp"
#include "client/player/PerlinFluidState.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace perlin::physics {
    bds::ScaffoldingCell World::ClimberCell(int x, int y, int z) const {
        using Cell = bds::ScaffoldingCell;
        const auto shape = Query(x, y, z);
        if (shape.scaffoldingStability >= 0) return Cell::Scaffold;
        if (shape.powderSnow) return Cell::PowderSnow;
        if (shape.fluid == 1) return Cell::Water;
        return shape.isAir && shape.Empty() ? Cell::Air : Cell::Other;
    }
    namespace {
        constexpr float kFloatEps = 1.1920929e-7f;
        constexpr double kClimbSpeed = 0.2;

        struct Vec3 {
            double x = 0, y = 0, z = 0;
            double& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
            double operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
        };

        double F(double v) {
            return static_cast<double>(static_cast<float>(v));
        }

        struct ShapeMetadata {
            uint8_t surface;
            bool preserveHorizontalMotion;
            bool hasBlock;
            bds::BlockPosition position;
        };

        bds::CollisionBox CollisionBox(const Box& box);

        void Gather(const World& world, const Box& region, std::vector<Box>& out,
                    std::vector<ShapeMetadata>* metadata = nullptr, const Box* actorBox = nullptr,
                    bool fallThrough = false, float fallDistance = 0, bool canStandOnSnow = false) {
            out.clear();
            if (metadata)
                metadata->clear();
            const auto nativeRegion = CollisionBox(region);
            bds::CollectUnavailableChunkBarriers(nativeRegion, [&](int32_t x, int32_t z) {
                return bds::ChunkCollisionState{world.CollisionChunkReady(x, z)};
            }, [&](const bds::CollisionBox& box) {
                out.push_back({box[0], box[1], box[2], box[3], box[4], box[5]});
                if (metadata)
                    metadata->push_back({0, true, false, {}});
            });
            bds::CollectTerrainCells(nativeRegion, world.CollisionHeightLimits(),
                                    [&](int32_t x, int32_t z) { return world.CollisionColumnPresent(x, z); },
                                    [&](int32_t x, int32_t y, int32_t z) {
                BlockShape shape = world.Query(x, y, z);
                const bds::BlockPosition position{x, y, z};
                if (shape.scaffoldingStability >= 0 && actorBox &&
                    bds::ScaffoldingSupports(CollisionBox(*actorBox), position,
                                             shape.scaffoldingStability, fallThrough)) {
                    shape.boxes.push_back({0, 0, 0, 1, 1, 1});
                }
                if (shape.powderSnow && actorBox) {
                    const float height = bds::PowderSnowSupportHeight(static_cast<float>(actorBox->y0),
                        static_cast<float>(y) + 1.0f, fallThrough, fallDistance, canStandOnSnow);
                    if (height > 0)
                        shape.boxes.push_back({0, 0, 0, 1, height, 1});
                }
                const auto offset = shape.randomOffset ? bds::GenerateBlockOffset(*shape.randomOffset, position)
                                                       : bds::Float3{};
                const auto origin = bds::BlockCollisionOrigin(offset, position);
                for (const auto& local : shape.boxes) {
                    const auto box = bds::TranslateBlockCollisionShape(CollisionBox(local), origin);
                    if (bds::AcceptBlockCollisionShape(box, &nativeRegion)) {
                        out.push_back({box[0], box[1], box[2], box[3], box[4], box[5]});
                        if (metadata)
                            metadata->push_back({shape.surface, false, true, {x, y, z}});
                    }
                }
            }, [&](const bds::CollisionBox& box) {
                out.push_back({box[0], box[1], box[2], box[3], box[4], box[5]});
                if (metadata)
                    metadata->push_back({0, false, false, {}});
            });
        }

        bool NoCollision(const World& world, const Box& box, std::vector<Box>& scratch) {
            Gather(world, box, scratch);
            for (const Box& b : scratch)
                if (b.Intersects(box))
                    return false;
            return true;
        }

        bds::CollisionBox CollisionBox(const Box& box) {
            return {static_cast<float>(box.x0), static_cast<float>(box.y0), static_cast<float>(box.z0),
                    static_cast<float>(box.x1), static_cast<float>(box.y1), static_cast<float>(box.z1)};
        }

        Box PhysicsBox(const bds::CollisionBox& box) {
            return {box[0], box[1], box[2], box[3], box[4], box[5]};
        }

        struct Motion {
            Box box;
            Vec3 resolved;
            bds::Float3 requested;
            Vec3 velocity;
            bool cx = false, cy = false, cz = false;
            bool stepped = false;
            uint8_t supportingSurface = 0;
            bool preserveHorizontalMotion = false;
            float penetration = 0;
        };

        Motion ResolveMotion(const World& world, const Box& start, const Vec3& velocity, Vec3 storedVelocity, bool wasOnGround,
                             std::vector<Box>& scratch, bds::CollisionVector recoveryLimits, float stepHeight,
                             bool flyingAbility, bool& oneShotStep, bool sneakEdge = false, bool fallThrough = false,
                             float fallDistance = 0, bool canStandOnSnow = false) {
            Motion result;
            const auto search =
                bds::SearchMovement(CollisionBox(start),
                                    {static_cast<float>(velocity.x), static_cast<float>(velocity.y),
                                     static_cast<float>(velocity.z)},
                                    stepHeight);
            std::vector<ShapeMetadata> metadata;
            bds::CollisionCache cache;
            bds::RefreshCollisionCache(cache, search.region, [&](const bds::CollisionBox& region) {
                std::vector<ShapeMetadata> incoming;
                Gather(world, PhysicsBox(region), scratch, &incoming, &start, fallThrough, fallDistance, canStandOnSnow);
                const size_t base = metadata.size();
                metadata.insert(metadata.end(), incoming.begin(), incoming.end());
                std::vector<bds::CollisionRecord> records;
                records.reserve(scratch.size());
                for (size_t i = 0; i < scratch.size(); ++i) {
                    const auto& entry = incoming[i];
                    records.push_back({CollisionBox(scratch[i]),
                                       {entry.hasBlock ? static_cast<uint64_t>(base + i + 1) : 0, entry.position},
                                       entry.preserveHorizontalMotion});
                }
                return records;
            });
            const auto& shapes = cache.shapes;
            auto requested = search.movement;
            if (sneakEdge) {
                const auto clipped = bds::ClipSneakMovement(
                    CollisionBox(start), stepHeight, requested,
                    {static_cast<float>(storedVelocity.x), static_cast<float>(storedVelocity.y),
                     static_cast<float>(storedVelocity.z)}, shapes);
                requested = clipped.movement;
                storedVelocity = {clipped.velocity[0], clipped.velocity[1], clipped.velocity[2]};
            }
            result.velocity = storedVelocity;
            result.requested = {requested[0], requested[1], requested[2]};
            const std::array<bds::CollisionVector, 3> moves{
                {{0, requested[1], 0}, {requested[0], 0, 0}, {0, 0, requested[2]}}};
            auto resolved = bds::ResolveMoves(CollisionBox(start), moves, shapes, recoveryLimits);
            result.penetration = resolved.penetration;
            if (bds::ShouldAutoStep(stepHeight, requested, resolved.displacement, wasOnGround, flyingAbility,
                                    oneShotStep))
                result.stepped = bds::TryAutoStep(CollisionBox(start), requested, stepHeight, shapes,
                                                  recoveryLimits, resolved);
            result.box = PhysicsBox(resolved.box);
            const auto support = bds::SelectSupportingShape(resolved.box, shapes);
            if (support.index && cache.blocks[*support.index].block)
                result.supportingSurface = metadata[cache.blocks[*support.index].block - 1].surface;
            result.preserveHorizontalMotion = cache.preserveHorizontalMotion;
            result.resolved = {resolved.displacement[0], resolved.displacement[1], resolved.displacement[2]};
            result.cx = std::fabs(requested[0] - resolved.displacement[0]) > kFloatEps;
            result.cy = std::fabs(requested[1] - resolved.displacement[1]) > kFloatEps;
            result.cz = std::fabs(requested[2] - resolved.displacement[2]) > kFloatEps;
            return result;
        }

        Vec3 FinalizeMotion(State& state, const Motion& motion, const Vec3& velocity) {
            const auto result = bds::FinalizeMove(
                CollisionBox(motion.box), 0.0f, motion.requested,
                {static_cast<float>(motion.resolved.x), static_cast<float>(motion.resolved.y),
                 static_cast<float>(motion.resolved.z)},
                {static_cast<float>(velocity.x), static_cast<float>(velocity.y), static_cast<float>(velocity.z)},
                {state.horizontalCollision, state.verticalCollision, state.onGround,
                 state.collision, state.collidableMobNear}, false,
                motion.preserveHorizontalMotion);
            state.x = result.position.x;
            state.y = result.position.y;
            state.z = result.position.z;
            state.box = motion.box;
            state.hasBox = true;
            state.horizontalCollision = result.flags.horizontalCollision;
            state.verticalCollision = result.flags.verticalCollision;
            state.onGround = result.flags.onGround;
            state.fallDistance = bds::FallDistanceAfterMove(state.fallDistance,
                static_cast<float>(motion.resolved.y), state.onGround, state.inWater);
            state.collision = result.flags.collision;
            state.collidableMobNear = result.flags.collidableMobNear;
            return {result.velocity.x, result.velocity.y, result.velocity.z};
        }

        Vec3 LandingResponse(State& state, Vec3 velocity, uint8_t supportingSurface, bool collided, bool sneaking) {
            bds::Float3 motion{static_cast<float>(velocity.x), static_cast<float>(velocity.y),
                               static_cast<float>(velocity.z)};
            if (collided) {
                const auto surface = supportingSurface == 1 ? bds::LandingSurface::Slime
                                     : supportingSurface == 2 ? bds::LandingSurface::Bed
                                                            : bds::LandingSurface::Ordinary;
                motion = bds::VerticalCollision(motion, {}, surface, sneaking, false, false, true,
                                                state.restitution).velocity;
            }
            if (state.restitution) {
                motion = bds::ApplyRestitution(motion, *state.restitution);
                state.restitution.reset();
            }
            return {motion.x, motion.y, motion.z};
        }

        struct CellRange {
            int x0, y0, z0, x1, y1, z1;
        };

        CellRange Cells(const Box& b) {
            return {static_cast<int>(std::floor(b.x0)), static_cast<int>(std::floor(b.y0)),
                    static_cast<int>(std::floor(b.z0)), static_cast<int>(std::floor(b.x1)),
                    static_cast<int>(std::floor(b.y1)), static_cast<int>(std::floor(b.z1))};
        }

        struct Environment {
            bool water = false, lava = false, web = false;
        };

        bool TouchesFluid(const World& world, const Box& shrunk, int kind) {
            return bds::CollectLiquidCells(CollisionBox(shrunk), kind == 1 ? 5 : 6, false,
                [&](bds::BlockPosition position) {
                    const auto shape = world.Query(position[0], position[1], position[2]);
                    return shape.fluid == 1 ? 5u : (shape.fluid == 2 ? 6u : 0u);
                }, [](bds::BlockPosition, uint8_t) {});
        }

        Environment Scan(const World& world, const Box& box) {
            Environment env;
            const auto bounds = bds::MakeLiquidQueryBounds(CollisionBox(box));
            env.lava = TouchesFluid(world, PhysicsBox(bounds.lava), 2);
            env.water = TouchesFluid(world, PhysicsBox(bounds.water), 1);
            const Box shrunk = box.Inflate(-0.001);
            const CellRange c = Cells(shrunk);
            for (int y = c.y0; y <= c.y1; y++)
                for (int x = c.x0; x <= c.x1; x++)
                    for (int z = c.z0; z <= c.z1; z++)
                        env.web = env.web || world.Query(x, y, z).web;
            return env;
        }

        bool SolidMotion(const BlockShape& s) { return !s.Empty() && !s.fluid && !s.climbable && !s.web; }

        struct Cell { int mat = 0; int depth = 0; };
        Cell LiquidCell(const World& w, int x, int y, int z, bool& unknown) {
            const BlockShape s = w.Query(x, y, z);
            if (!s.fluid) return {};
            if (s.fluidDepth == 255) { unknown = true; return {s.fluid, 0}; }
            return {s.fluid, s.fluidDepth};
        }
        inline int EffDepth(int d) { return d < 8 ? d : 0; }

        void FlowAt(const World& w, int x, int y, int z, int mat, float out[3]) {
            static const int dirAxis[4] = {2, 0, 2, 0};
            static const int dirStep[4] = {-1, 1, 1, -1};
            bool unk = false;
            const Cell c = LiquidCell(w, x, y, z, unk);
            const bool same = c.mat == mat;
            const int depth = same ? EffDepth(c.depth) : -1;
            float f[3] = {0, 0, 0};
            for (int d = 0; d < 4; d++) {
                int n[3] = {x, y, z};
                n[dirAxis[d]] += dirStep[d];
                const Cell o = LiquidCell(w, n[0], n[1], n[2], unk);
                if (o.mat == mat) {
                    f[dirAxis[d]] += static_cast<float>((EffDepth(o.depth) - depth) * dirStep[d]);
                    continue;
                }
                if (SolidMotion(w.Query(n[0], n[1], n[2])))
                    continue;
                const Cell below = LiquidCell(w, n[0], n[1] - 1, n[2], unk);
                if (below.mat == mat)
                    f[dirAxis[d]] += static_cast<float>((EffDepth(below.depth) - depth + 8) * dirStep[d]);
            }
            if (same && c.depth >= 8) {
                bool solid = false;
                for (int d = 0; d < 4 && !solid; d++) {
                    int n[3] = {x, y, z};
                    n[dirAxis[d]] += dirStep[d];
                    const BlockShape a = w.Query(n[0], n[1], n[2]);
                    const BlockShape b = w.Query(n[0], n[1] + 1, n[2]);
                    auto full = [](const BlockShape& s) {
                        return s.boxes.size() == 1 && s.boxes[0].x0 <= 0 && s.boxes[0].z0 <= 0 && s.boxes[0].x1 >= 1 &&
                               s.boxes[0].z1 >= 1 && s.boxes[0].y0 <= 0 && s.boxes[0].y1 >= 1;
                    };
                    solid = full(a) || full(b);
                }
                if (solid) {
                    float l = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
                    if (l >= 0.0001f) { f[0] /= l; f[1] /= l; f[2] /= l; } else f[0] = f[1] = f[2] = 0;
                    f[1] += -6.0f;
                }
            }
            const float l = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
            if (l >= 0.0001f) { out[0] = f[0] / l; out[1] = f[1] / l; out[2] = f[2] / l; }
            else out[0] = out[1] = out[2] = 0;
        }

        void ApplyCurrent(State& s, const World& w, const Box& sensingBox) {
            const auto bounds = bds::MakeLiquidQueryBounds(CollisionBox(sensingBox));
            for (int mat = 2; mat >= 1; mat--) {
                const auto& box = mat == 1 ? bounds.water : bounds.lava;
                const float lx = box[0], ly = box[1], lz = box[2], hx = box[3], hy = box[4], hz = box[5];
                const int x0 = static_cast<int>(std::floor(lx)), y0 = static_cast<int>(std::floor(ly)),
                          z0 = static_cast<int>(std::floor(lz));
                const int x1 = static_cast<int>(std::floor(hx + 1.0f)), y1 = static_cast<int>(std::floor(hy + 1.0f)),
                          z1 = static_cast<int>(std::floor(hz + 1.0f));
                bool unknown = false, any = false;
                std::vector<std::array<int, 4>> cells;
                for (int y = y0; y < y1; y++)
                    for (int z = z0; z < z1; z++)
                        for (int x = x0; x < x1; x++) {
                            const Cell c = LiquidCell(w, x, y, z, unknown);
                            if (c.mat == mat) { any = true; cells.push_back({x, y, z, c.depth}); }
                        }
                if (!any) continue;
                if (unknown) return;
                bool admitted = false;
                for (auto& c : cells) if (c[3] > 0) { admitted = true; break; }
                if (!admitted) {
                    static const int ax[4] = {2, 0, 2, 0}, st[4] = {-1, 1, 1, -1};
                    for (auto& c : cells) {
                        for (int d = 0; d < 4 && !admitted; d++) {
                            const int edge = ax[d] == 0 ? (st[d] < 0 ? x0 : x1 - 1) : (st[d] < 0 ? z0 : z1 - 1);
                            if (c[ax[d] == 0 ? 0 : 2] != edge) continue;
                            int n[3] = {c[0], c[1], c[2]};
                            n[ax[d]] += st[d];
                            bool u = false;
                            const Cell o = LiquidCell(w, n[0], n[1], n[2], u);
                            if (o.mat == mat && !u && o.depth != 0) admitted = true;
                        }
                    }
                }
                if (!admitted) return;
                float sum[3] = {0, 0, 0};
                for (auto& c : cells) {
                    float f[3];
                    FlowAt(w, c[0], c[1], c[2], mat, f);
                    sum[0] += f[0]; sum[1] += f[1]; sum[2] += f[2];
                }
                const float l = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
                if (l >= 0.0001f) {
                    const float k = mat == 1 ? 0.014f : 0.0035f;
                    s.vx = F(static_cast<float>(s.vx) + sum[0] / l * k);
                    s.vy = F(static_cast<float>(s.vy) + sum[1] / l * k);
                    s.vz = F(static_cast<float>(s.vz) + sum[2] / l * k);
                }
                return;
            }
        }

        void MoveRelative(State& s, double strafeIn, double forwardIn, double speed, bool currentMovementMath) {
            const auto movement = currentMovementMath ? bds::CurrentRelativeMovement : bds::RelativeMovement;
            const auto velocity = movement(
                {static_cast<float>(s.vx), static_cast<float>(s.vy), static_cast<float>(s.vz)}, s.yaw,
                {-static_cast<float>(strafeIn), 0.0f, static_cast<float>(forwardIn)}, static_cast<float>(speed));
            s.vx = velocity.x;
            s.vy = velocity.y;
            s.vz = velocity.z;
        }

        double DampHorizontal(double value, float retention) {
            return bds::DampComponent(static_cast<float>(value), retention);
        }

        Vec3 ApplyStoredBlockSlowdown(State& state, Vec3 requested) {
            bds::Float3 multiplier{state.stuckX, state.stuckY, state.stuckZ};
            bds::Float3 movement{static_cast<float>(requested.x), static_cast<float>(requested.y),
                                 static_cast<float>(requested.z)};
            bds::Float3 velocity{static_cast<float>(state.vx), static_cast<float>(state.vy),
                                 static_cast<float>(state.vz)};
            bds::ApplyBlockSlowdown(multiplier, movement, velocity);
            state.stuckX = multiplier.x;
            state.stuckY = multiplier.y;
            state.stuckZ = multiplier.z;
            state.vx = velocity.x;
            state.vy = velocity.y;
            state.vz = velocity.z;
            return {movement.x, movement.y, movement.z};
        }

        const BlockShape& Below(const World& world, const State& s, BlockShape& storage) {
            storage = world.Query(static_cast<int>(std::floor(s.x)), static_cast<int>(std::floor(s.y - 0.1f)),
                                  static_cast<int>(std::floor(s.z)));
            return storage;
        }
    }

    Box Box::Expand(double dx, double dy, double dz) const {
        Box b = *this;
        (dx < 0 ? b.x0 : b.x1) += dx;
        (dy < 0 ? b.y0 : b.y1) += dy;
        (dz < 0 ? b.z0 : b.z1) += dz;
        return b;
    }

    bool Box::Intersects(const Box& o) const {
        return static_cast<float>(x1) > static_cast<float>(o.x0) && static_cast<float>(x0) < static_cast<float>(o.x1) &&
               static_cast<float>(y1) > static_cast<float>(o.y0) && static_cast<float>(y0) < static_cast<float>(o.y1) &&
               static_cast<float>(z1) > static_cast<float>(o.z0) && static_cast<float>(z0) < static_cast<float>(o.z1);
    }

    bool World::InsideRegionReady(const bds::CollisionBox& bounds) const {
        return bds::InsideRegionReady(bounds, false, [&](int32_t x, int32_t z) {
            return bds::ChunkCollisionState{CollisionChunkReady(x, z)};
        });
    }

    MoveVector ProcessMoveInput(float forward, float strafe, bool sneaking, bool inWater, float itemUseModifier) {
        const float lengthSq = forward * forward + strafe * strafe;
        if (lengthSq > 1.0f) {
            const float length = std::sqrt(lengthSq);
            forward /= length;
            strafe /= length;
        }
        if (sneaking && !inWater) {
            forward *= 0.3f;
            strafe *= 0.3f;
        }
        const float useScale = itemUseModifier * itemUseModifier;
        return {forward * useScale, strafe * useScale};
    }

    namespace {
        double BoxCentre(double lo, double hi) {
            return F((static_cast<float>(lo) + static_cast<float>(hi)) * 0.5f);
        }
    }

    Box BoundingBox(const State& s, bool sneaking) {
        const float half = static_cast<float>(kWidth) * 0.5f;
        const float x = static_cast<float>(s.x), y = static_cast<float>(s.y), z = static_cast<float>(s.z);
        const float h = static_cast<float>((s.crawling || s.swimming) ? kCrawlHeight : sneaking ? kSneakHeight : kStandHeight);
        if (s.hasBox && BoxCentre(s.box.x0, s.box.x1) == F(s.x) && BoxCentre(s.box.z0, s.box.z1) == F(s.z) &&
            static_cast<float>(s.box.y0) == y) {
            Box b = s.box;
            b.y1 = static_cast<double>(y + h);
            return b;
        }
        return {static_cast<double>(x - half), static_cast<double>(y), static_cast<double>(z - half),
                static_cast<double>(x + half), static_cast<double>(y + h), static_cast<double>(z + half)};
    }

    bool TouchingWater(const State& s, const World& world) {
        const auto env = Scan(world, BoundingBox(s, s.collisionHeight == static_cast<float>(kSneakHeight)));
        return bds::SelectFluidMaterial(env.water, env.lava) == 5;
    }

    bool PoseFits(const State& s, const World& world, double height) {
        thread_local std::vector<Box> scratch;
        auto bounds = CollisionBox(BoundingBox(s, false));
        bounds[4] = bounds[1] + static_cast<float>(height);
        for (int axis = 0; axis < 3; ++axis) {
            bounds[axis] += 0.01f;
            bounds[axis + 3] -= 0.01f;
            if (bounds[axis] > bounds[axis + 3])
                bounds[axis] = bounds[axis + 3] = (bounds[axis] + bounds[axis + 3]) * 0.5f;
        }
        const Box box{bounds[0], bounds[1], bounds[2], bounds[3], bounds[4], bounds[5]};
        return NoCollision(world, box, scratch);
    }

    static void StepMovement(State& s, const Input& in, const Attributes& attr, const World& world) {
        thread_local std::vector<Box> scratch;
        const float swimSpeedMultiplier = bds::SwimSpeedMultiplier(in.swimming, s.movementEffects);
        const Box fluidSensingBox = BoundingBox(s, s.collisionHeight == static_cast<float>(kSneakHeight));
        s.crawling = in.crawl && !in.flying;
        s.swimming = in.swimming && !in.flying;
        const bool sneaking = in.sneak && !in.flying;
        const bool crouched = (in.sneak || in.forceCrouch) && !in.flying;
        const float collisionHeight = static_cast<float>((s.crawling || s.swimming) ? kCrawlHeight
                                                        : crouched ? kSneakHeight : kStandHeight);
        if (s.collisionHeight != collisionHeight) {
            s.collisionHeight = collisionHeight;
            s.hasBox = false;
        }

        s.x = F(s.x); s.y = F(s.y); s.z = F(s.z);
        s.vx = F(s.vx); s.vy = F(s.vy); s.vz = F(s.vz);
        if (s.vx * s.vx + s.vy * s.vy + s.vz * s.vz < 1.0e-12)
            s.vx = s.vy = s.vz = 0;

        if (!in.flying)
            ApplyCurrent(s, world, fluidSensingBox);

        const bool prevHorizontal = s.horizontalCollision;
        const bool grounded = s.onGround;
        if (!in.jump)
            s.jumpDelay = 0;

        const Environment env = Scan(world, fluidSensingBox);
        const uint32_t fluidMaterial = bds::SelectFluidMaterial(env.water, env.lava);
        bds::UpdateWaterState(fluidMaterial, s.inWater, s.fallDistance,
            s.waterSplashRequest, [] { return false; }, [] { return false; });
        s.inLava = bds::WasInLava(fluidMaterial);
        bds::UpdateLavaFallDistance(fluidMaterial, s.fallDistance);
        const BlockShape feet = world.Query(static_cast<int>(std::floor(s.x)), static_cast<int>(std::floor(s.y)),
                                            static_cast<int>(std::floor(s.z)));
        s.onClimbable = feet.climbable;
        bds::ResetClimbingFallDistance(s.fallDistance, in.wallClimbing, in.canClimb,
                                      feet.climbable, feet.powderSnow, in.canStandOnPowderSnow);

        BlockShape frictionStorage;
        const BlockShape& under = Below(world, s, frictionStorage);
        const float blockFriction = under.Empty() && !under.fluid && under.friction <= 0 ? 0.6f : under.friction;

        float nf = in.forward, ns = in.strafe;
        {
            const float lengthSq = nf * nf + ns * ns;
            if (lengthSq > 1.0f) {
                const float length = std::sqrt(lengthSq);
                nf /= length;
                ns /= length;
            }
        }
        const auto intent = bds::AttenuateTravelIntent({ns, 0.0f, nf}, 0.0f);
        const double forward = intent.movement.z;
        const double strafe = intent.movement.x;

        if (in.flying) {
            if (s.hasTeleported)
                return;
            const bool hover = std::max(std::fabs(in.forward), std::fabs(in.strafe)) < 0.01f;
            MoveRelative(s, strafe, forward, static_cast<double>(attr.flySpeed) * (in.sprint ? 2.0 : 1.0),
                         attr.currentMovementMath);
            s.vy = bds::FlyingControl(static_cast<float>(s.vy), attr.verticalFlySpeed,
                hover, in.creativeFlight, in.flyUp, in.flyDown, in.flyUpSlow, in.flyDownSlow);
            const float hMod = hover ? (in.creativeFlight ? 0.375f : 0.75f) : 1.0f;
            const Vec3 req = ApplyStoredBlockSlowdown(s, {s.vx, s.vy, s.vz});
            if (in.noClip) {
                s.hasBox = false;
                s.x = F(s.x + req.x);
                s.y = F(s.y + req.y);
                s.z = F(s.z + req.z);
                s.collidableMobNear = false;
                const auto velocity = LandingResponse(s, {s.vx, s.vy, s.vz}, 0, false, in.sneak);
                s.vx = velocity.x;
                s.vy = velocity.y;
                s.vz = velocity.z;
                s.jumpDelay = 0;
                const float h = bds::FlyingHorizontalRetention(1.0f,
                    hover ? std::optional<float>(hMod) : std::nullopt, attr.airDrag);
                s.vx = DampHorizontal(s.vx, h);
                s.vz = DampHorizontal(s.vz, h);
                s.vy = bds::FlyingVerticalDrag(static_cast<float>(s.vy), s.fallDistance, attr.airDrag);
                return;
            }
            const double groundFriction = grounded ? blockFriction : 1.0;
            const Box box = BoundingBox(s, false);
            const Motion m =
                ResolveMotion(world, box, req, {s.vx, s.vy, s.vz}, grounded, scratch, bds::RecoveryLimits(s.recovery),
                              attr.maxAutoStep, in.flying, s.canAlwaysAutoStep, false,
                              in.gravity.descendThroughBlock, s.fallDistance, in.canStandOnPowderSnow);
            bds::UpdateRecovery(s.recovery, m.penetration >= 1.0e-6f, in.pushTowardsClosestSpace);
            auto finalized = FinalizeMotion(s, m, m.velocity);
            if (m.stepped)
                finalized.y = 0;
            finalized = LandingResponse(s, finalized, m.supportingSurface, m.cy, in.sneak);
            s.vx = finalized.x;
            s.vy = finalized.y;
            s.vz = finalized.z;
            const float h = bds::FlyingHorizontalRetention(static_cast<float>(groundFriction),
                hover ? std::optional<float>(hMod) : std::nullopt, attr.airDrag);
            s.vx = DampHorizontal(s.vx, h);
            s.vz = DampHorizontal(s.vz, h);
            s.vy = bds::FlyingVerticalDrag(static_cast<float>(s.vy), s.fallDistance, attr.airDrag);
            return;
        }

        const bool water = s.inWater, lava = s.inLava;
        auto gravity = in.gravity;
        if (in.overDescendableBlock)
            gravity.descendThroughBlock = in.sneak;
        if (gravity.descendThroughBlock && in.overDescendableBlock) {
            s.vy = -0.15f;
            s.fallDistance = 0;
        }
        const float friction = grounded ? 0.91f * blockFriction : 0.91f;
        double relSpeed;
        if (water) {
            relSpeed = bds::WaterAcceleration(attr.speed, attr.underwaterSpeed, attr.speed,
                swimSpeedMultiplier, attr.waterWalkSpeedEnchant, attr.dolphin, grounded, true);
        } else if (lava) {
            relSpeed = 0.02;
        } else if (grounded) {
            float accFriction = blockFriction;
            if (under.surface == 4)
                accFriction *= 1.225f;
            relSpeed = bds::GroundAcceleration(attr.speed, accFriction);
        } else {
            relSpeed = in.sprint ? 0.026 : 0.02;
        }

        const bool jumpBlockedBySwim = (s.swimAmount > 0.0f && s.swimAmount < 1.0f)
                                    || (in.swimming && !in.headInWater);
        const bool scaffoldJump = in.jump && !jumpBlockedBySwim &&
                                  (gravity.inScaffolding || in.inAscendableBlock) &&
                                  !gravity.descendThroughBlock;
        if (scaffoldJump) {
            s.vy = 0.15f;
            s.jumpDelay = 10;
        }
        if (in.jump && (water || lava)) {
            if (jumpBlockedBySwim) {
                if (water) s.vy = 0;
            } else {
                s.vy = F(static_cast<float>(s.vy) + 0.04f);
            }
        }

        if (water && in.swimming && !in.jump) {
            const float look = in.lookY;
            const float gain = look < -0.2f ? 0.085f : 0.06f;
            if (look <= 0.0f || in.headInWater)
                s.vy = F(static_cast<float>(s.vy) + (look - static_cast<float>(s.vy)) * gain);
            else
                s.vy = 0;
        }

        const bool glide = in.gliding && !water && !lava && !s.hasTeleported;
        if (glide) {
            const bool boost = s.movementEffects[static_cast<int32_t>(bds::MovementEffectType::GlideBoost)].type != -1;
            const auto velocity = bds::GlideVelocity(
                {static_cast<float>(s.vx), static_cast<float>(s.vy), static_cast<float>(s.vz)},
                s.previousRotation, {in.pitch, s.yaw}, attr.slowFalling, boost, s.fallDistance);
            s.vx = velocity.x;
            s.vy = velocity.y;
            s.vz = velocity.z;
        } else if (!s.hasTeleported) {
            MoveRelative(s, strafe, forward, relSpeed, attr.currentMovementMath);
        }

        const bool climbJump = in.jump && !jumpBlockedBySwim && s.onClimbable;
        const bool jumpInitiated = in.jump && !scaffoldJump && !jumpBlockedBySwim && !glide && !climbJump && s.onGround && s.jumpDelay == 0 && !water && !lava &&
            bds::GroundJumpAllowed((gravity.inScaffolding ? 0x20ull : 0ull) |
                                   (gravity.overScaffolding ? 0x40ull : 0ull) |
                                   (gravity.descendThroughBlock ? 0x80ull : 0ull), under.powderSnow,
                                   [&] { return in.canStandOnPowderSnow; }, [] { return false; });
        if (jumpInitiated) {
            bds::JumpParameters jump;
            jump.bodyMinimumY = static_cast<float>(BoundingBox(s, crouched).y0);
            jump.position = {static_cast<float>(s.x), static_cast<float>(s.y), static_cast<float>(s.z)};
            jump.velocity = {static_cast<float>(s.vx), static_cast<float>(s.vy), static_cast<float>(s.vz)};
            jump.previousJumpPosition = s.previousJumpPosition;
            jump.yaw = s.yaw;
            jump.sprinting = in.sprint;
            jump.currentMovementMath = attr.currentMovementMath;
            jump.customHeight = attr.jumpHeight;
            if (attr.jumpBoost != 0)
                jump.effectAmplifier = std::bit_cast<int32_t>(static_cast<uint32_t>(attr.jumpBoost) - 1);
            const auto result = bds::GroundJump(jump, [&](bds::BlockPosition position) {
                return world.Query(position[0], position[1], position[2]).jumpProperties;
            });
            s.vx = result.velocity.x;
            s.vy = result.velocity.y;
            s.vz = result.velocity.z;
            s.previousJumpPosition = result.previousJumpPosition;
            s.jumpDelay = 10;
        }

        if (s.hasTeleported)
            return;

        if (s.onClimbable) {
            s.vy = std::max(s.vy, -kClimbSpeed);
            if (climbJump || prevHorizontal)
                s.vy = kClimbSpeed;
            else if (sneaking && s.vy < 0)
                s.vy = 0;
        }
        if (water && sneaking)
            s.vy = F(static_cast<float>(s.vy) - 0.04f);

        Vec3 req = ApplyStoredBlockSlowdown(s, {s.vx, s.vy, s.vz});
        const Box box = BoundingBox(s, crouched);
        bool requestSneakFlag = false;
        const bool clipSneak = bds::ShouldClipSneak(in.actorMovementFlags, sneaking, grounded, requestSneakFlag);
        const Motion m = ResolveMotion(world, box, req, {s.vx, s.vy, s.vz}, grounded, scratch, bds::RecoveryLimits(s.recovery),
                                       attr.maxAutoStep, in.flying, s.canAlwaysAutoStep, clipSneak,
                                       gravity.descendThroughBlock, s.fallDistance, in.canStandOnPowderSnow);
        bds::UpdateRecovery(s.recovery, m.penetration >= 1.0e-6f, in.pushTowardsClosestSpace);
        Vec3 v = FinalizeMotion(s, m, m.velocity);
        if (m.stepped)
            v.y = 0;
        v = LandingResponse(s, v, m.supportingSurface, m.cy, sneaking);

        const bool autoClimb =
            !water && !lava && (m.cx || m.cz) &&
            world.Query(static_cast<int>(std::floor(s.x)), static_cast<int>(std::floor(s.y)),
                        static_cast<int>(std::floor(s.z)))
                .climbable;
        if (autoClimb)
            v.y = kClimbSpeed;

        if (water || lava) {
            if (water) {
                const auto dragged = bds::WaterDrag(
                    {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)},
                    s.onGround, in.sprint, swimSpeedMultiplier, attr.waterWalkSpeedEnchant, attr.waterDrag);
                v = {dragged.x, dragged.y, dragged.z};
            } else {
                v.x = F(static_cast<float>(v.x) * 0.5f);
                v.y = F(static_cast<float>(v.y) * 0.5f);
                v.z = F(static_cast<float>(v.z) * 0.5f);
            }
            if (water)
                v.y = bds::PostWaterVertical(static_cast<float>(v.y), s.fallDistance, attr.levitation, in.swimming);
            else if (attr.levitation > 0)
                v.y = bds::LevitateVelocity(static_cast<float>(v.y), attr.levitation);
            else
                v.y = F(static_cast<float>(v.y) - 0.02f);
        } else if (!glide) {
            v.y = bds::PostGroundVertical(static_cast<float>(v.y), s.fallDistance, gravity,
                                         attr.levitation, attr.slowFalling, autoClimb);
            v.x = DampHorizontal(v.x, friction);
            v.z = DampHorizontal(v.z, friction);
        }

        if ((water || lava) && (m.cx || m.cz)) {
            const float dy = ((static_cast<float>(box.y0) - static_cast<float>(s.y)) + 0.6f)
                             + static_cast<float>(v.y);
            const Box probe = BoundingBox(s, crouched).Offset(static_cast<float>(v.x), dy,
                                                              static_cast<float>(v.z));
            if (!TouchesFluid(world, probe, 1) && !TouchesFluid(world, probe, 2) &&
                NoCollision(world, probe, scratch))
                v.y = 0.3f;
        }
        // SlimeBlock::onStandOn (0x1421c9d70): fabs(mPosDelta.y) < 0.1, not one-sided; fast rise (vy > 0.1, bounce) not damped
        if (s.onGround && !sneaking && std::fabs(static_cast<float>(v.y)) < 0.1f) {
            const BlockShape stand = world.Query(static_cast<int>(std::floor(s.x)),
                                                 static_cast<int>(std::floor(s.y - 0.5)),
                                                 static_cast<int>(std::floor(s.z)));
            if (stand.surface == 1 || stand.surface == 3) {
                const float factor = std::fabs(static_cast<float>(v.y)) * 0.2f + 0.4f;
                v.x = F(static_cast<float>(v.x) * factor);
                v.z = F(static_cast<float>(v.z) * factor);
            }
        }
        if (!water && !lava && !in.flying) {
            const int fx = static_cast<int>(std::floor(s.x)), fz = static_cast<int>(std::floor(s.z));
            float factor = world.Query(fx, static_cast<int>(std::floor(s.y)), fz).speedFactor;
            if (factor == 1.0f)
                factor = world.Query(fx, static_cast<int>(std::floor(s.y - 0.5000001)), fz).speedFactor;
            if (factor != 1.0f) {
                v.x = F(static_cast<float>(v.x) * factor);
                v.z = F(static_cast<float>(v.z) * factor);
            }
        }
        s.vx = v.x; s.vy = v.y; s.vz = v.z;
        s.jumpDelay = bds::TickJumpDelay(s.jumpDelay);
    }

    static void UpdateBlockContacts(State& s, const Input& in, const Attributes& attr, const World& world, bool wasInWater) {
        const bool crouched = (in.sneak || in.forceCrouch) && !in.flying;
        {
            const Box inside = BoundingBox(s, crouched);
            bool web = false, powderSnow = false, berryBush = false;
            std::vector<bds::BlockPosition> honey;
            bds::CollectInsideCells(CollisionBox(inside), 0.0f, true,
                                   [&](const bds::CollisionBox& region) { return world.InsideRegionReady(region); },
                                   [&](int32_t x, int32_t y, int32_t z) {
                const auto shape = world.Query(x, y, z);
                web |= shape.web;
                powderSnow |= shape.powderSnow;
                berryBush |= shape.berryBush;
                if (shape.bubbleColumn && !in.flying) {
                    const auto above = world.Query(x, y + 1, z);
                    const bool surface = above.isAir && !above.fluid && above.Empty();
                    s.vy = bds::BubbleColumnVelocity(static_cast<float>(s.vy), shape.bubbleColumn < 0,
                                                     s.fallDistance, surface);
                }
                if (shape.honey)
                    honey.push_back({x, y, z});
            });
            if (!wasInWater && !honey.empty()) {
                const auto velocity = bds::HoneyContacts(
                    {static_cast<float>(s.x), static_cast<float>(s.y), static_cast<float>(s.z)},
                    {static_cast<float>(s.vx), static_cast<float>(s.vy), static_cast<float>(s.vz)},
                    static_cast<float>(kWidth), honey, s.fallDistance);
                s.vx = velocity.x;
                s.vy = velocity.y;
                s.vz = velocity.z;
            }
            if (web || powderSnow || berryBush) {
                bds::Float3 multiplier{s.stuckX, s.stuckY, s.stuckZ};
                std::optional<bool> firstApplication;
                if (web)
                    bds::AccumulateBlockSlowdown(multiplier, {0.25f, 0.05f, 0.25f}, firstApplication);
                if (powderSnow)
                    bds::AccumulateBlockSlowdown(multiplier, {0.9f, 1.5f, 0.9f}, firstApplication);
                if (berryBush)
                    bds::AccumulateBlockSlowdown(multiplier, {0.8f, 0.75f, 0.8f}, firstApplication);
                bds::ApplyPlayerSlowdownImmunity(multiplier, firstApplication, in.flying, in.creativeFlight);
                if (firstApplication && attr.weaving && web && !powderSnow && !berryBush)
                    multiplier = {0.5f, 0.25f, 0.5f};
                if (firstApplication)
                    bds::AdjustSlowdownFallDistance(*firstApplication, s.fallDistance);
                s.stuckX = multiplier.x;
                s.stuckY = multiplier.y;
                s.stuckZ = multiplier.z;
            }
        }
    }

    void Step(State& s, const Input& in, const Attributes& attr, const World& world) {
        for (const auto& effect : in.movementEffects)
            bds::ApplyMovementEffect(s.movementEffects, effect);
        StepMovement(s, in, attr, world);
        const auto climber = bds::SenseScaffolding(CollisionBox(BoundingBox(s, in.sneak)),
            [&](int32_t x, int32_t y, int32_t z) { return world.ClimberCell(x, y, z); }, in.canStandOnPowderSnow);
        if (!climber.overPowderSnow && (climber.inside || climber.over || climber.ascendable || climber.descendable))
            s.fallDistance = 0;
        UpdateBlockContacts(s, in, attr, world, s.inWater);
        bds::TickMovementEffects(s.movementEffects);
        s.swimAmount = in.swimming ? std::fmin(s.swimAmount + 0.1f, 1.0f)
                                  : std::fmax(s.swimAmount - 0.1f, 0.0f);
        s.previousRotation = {in.pitch, s.yaw};
        s.hasTeleported = false;
    }
}
