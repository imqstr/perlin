#include "client/player/LocalPlayer.hpp"

#include "client/player/BlockCollision.hpp"
#include "client/player/PerlinScaffolding.hpp"
#include "client/player/PerlinStatusEffect.hpp"
#include "protocol/Packets.hpp"
#include "world/level/block/BlockPalette.hpp"
#include "network/PlayerConstants.hpp"
#include "util/Env.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <limits>
#include <sstream>
#include "util/Log.hpp"

namespace perlin {
    namespace {
        constexpr int kMaxTicksPerFrame = 5;
        constexpr auto kDoubleTapWindow = std::chrono::milliseconds(350);
        constexpr auto kSprintTapWindow = std::chrono::milliseconds(300);

        float BedrockYaw(float cameraYaw) {
            float yaw = std::fmod(cameraYaw - 90.0f, 360.0f);
            if (yaw >= 180.0f)
                yaw -= 360.0f;
            if (yaw < -180.0f)
                yaw += 360.0f;
            return yaw;
        }

        float CameraYaw(float bedrockYaw) {
            return bedrockYaw + 90.0f;
        }

        float BedrockPitch(float cameraPitch) {
            return -cameraPitch;
        }

        void ApplyMotion(physics::State& state, const std::optional<std::array<double, 3>>& motion) {
            if (!motion)
                return;
            state.vx = (*motion)[0];
            state.vy = (*motion)[1];
            state.vz = (*motion)[2];
        }

        void ApplyTeleport(physics::State& state, const std::optional<BedrockSession::ServerMotion>& teleport) {
            if (!teleport)
                return;
            state.x = teleport->pos[0];
            state.y = teleport->pos[1];
            state.z = teleport->pos[2];
            state.vx = state.vy = state.vz = 0;
            state.onGround = teleport->onGround;
            state.hasTeleported = teleport->filterTravel;
            state.hasBox = false;
        }

        namespace Flag {
            constexpr uint32_t Ascend = 0;
            constexpr uint32_t Descend = 1;
            constexpr uint32_t JumpDown = 3;
            constexpr uint32_t SprintDown = 4;
            constexpr uint32_t Jumping = 6;
            constexpr uint32_t Sneaking = 8;
            constexpr uint32_t SneakDown = 9;
            constexpr uint32_t UpLeft = 14;
            constexpr uint32_t UpRight = 15;
            constexpr uint32_t WantUp = 16;
            constexpr uint32_t WantDown = 17;
            constexpr uint32_t Sprinting = 20;
            constexpr uint32_t StartSprinting = 25;
            constexpr uint32_t StopSprinting = 26;
            constexpr uint32_t StartSwimming = 29;
            constexpr uint32_t StopSwimming = 30;
            constexpr uint32_t StartSneaking = 27;
            constexpr uint32_t StopSneaking = 28;
            constexpr uint32_t StartJumping = 31;
            constexpr uint32_t StartGliding = 32;
            constexpr uint32_t StopGliding = 33;
            constexpr uint32_t StartCrawling = 40;
            constexpr uint32_t StopCrawling = 41;
            constexpr uint32_t HorizontalCollision = 49;
            constexpr uint32_t VerticalCollision = 50;
            constexpr uint32_t DownLeft = 51;
            constexpr uint32_t DownRight = 52;
            constexpr uint32_t JumpReleasedRaw = 59;
            constexpr uint32_t JumpPressedRaw = 60;
            constexpr uint32_t JumpCurrentRaw = 61;
            constexpr uint32_t SneakReleasedRaw = 62;
            constexpr uint32_t SneakPressedRaw = 63;
            constexpr uint32_t SneakCurrentRaw = 64;
        }
    }

    void LocalPlayer::ApplyEffectUpdates(BedrockSession& session) {
        const auto updates = session.TakeEffectUpdates();
        size_t first = history.size();
        for (const auto& update : updates) {
            if (update.tick == 0 || history.empty())
                continue;
            const uint64_t boundary = std::max(update.tick, history.front().tick);
            for (size_t i = 0; i < history.size(); ++i) {
                Record& record = history[i];
                if (record.tick <= boundary)
                    continue;
                const bool active = update.active && physics::bds::StatusEffectTicksAt(
                    update.duration, update.tick, record.tick - 1) != 0;
                auto& a = record.attributes;
                bool changed = false;
                switch (update.id) {
                case 8: {
                    const int value = active ? update.amplifier + 1 : 0;
                    changed = a.jumpBoost != value;
                    a.jumpBoost = value;
                    break;
                }
                case 24: {
                    const int value = active ? update.amplifier + 1 : 0;
                    changed = a.levitation != value;
                    a.levitation = value;
                    break;
                }
                case 27:
                    changed = a.slowFalling != active;
                    a.slowFalling = active;
                    break;
                case 33:
                    changed = a.weaving != active;
                    a.weaving = active;
                    break;
                default: break;
                }
                if (changed)
                    first = std::min(first, i);
            }
        }
        ReplayFrom(session, first);
    }

    void LocalPlayer::ApplyMotionUpdate(BedrockSession& session, const BedrockSession::MotionUpdate& update) {
        if (teleportMotionBoundary && update.tick <= *teleportMotionBoundary) {
            if (PERLIN_ENV("PERLIN_BLOCK_DEBUG"))
                Log("move", "motion_before_teleport stamp={} boundary={}", update.tick, *teleportMotionBoundary);
            return;
        }
        size_t first = history.size();
        bool inserted = false;
        if (!history.empty()) {
            const uint64_t boundary = std::max(update.tick, history.front().tick);
            for (size_t i = 0; i < history.size(); ++i) {
                if (history[i].tick <= boundary)
                    continue;
                history[i].motion = update.velocity;
                first = i;
                inserted = true;
                if (PERLIN_ENV("PERLIN_BLOCK_DEBUG"))
                    Log("move", "motion_replay stamp={} target={} records={}", update.tick, history[i].tick, history.size() - i);
                break;
            }
        }
        if (!inserted)
            futureMotion.push_back(update);
        ReplayFrom(session, first);
    }

    void LocalPlayer::ReplayFrom(BedrockSession& session, size_t first) {
        if (first >= history.size())
            return;
        physics::State replay = history[first].before;
        LevelCollision world(session.NetLevel(), blocksPtr);
        for (size_t i = first; i < history.size(); ++i) {
            Record& record = history[i];
            record.before = replay;
            replay.yaw = record.yaw;
            ApplyTeleport(replay, record.teleport);
            ApplyMotion(replay, record.motion);
            if (record.simulated)
                physics::Step(replay, record.input, record.attributes, world);
            else
                replay.vx = replay.vy = replay.vz = 0;
            record.after = replay;
        }
        state = replay;
        previous = history.back().before;
        session.SetPose(static_cast<float>(state.x), static_cast<float>(state.y),
                        static_cast<float>(state.z), state.yaw, session.GetPose().pitch);
    }

    void LocalPlayer::ApplyServerMotion(BedrockSession& session, Camera& camera,
                                       BedrockSession::ServerMotion motion) {
        if (motion.stampedVelocity) {
            ApplyMotionUpdate(session, {motion.tick, {motion.velocity[0], motion.velocity[1], motion.velocity[2]}});
            return;
        }
        bool handled = false;
        if (motion.teleport && motion.filterTravel && motion.tick != 0 && !history.empty() &&
            motion.tick >= history.front().tick) {
            const float dx = static_cast<float>(motion.pos[0]) - static_cast<float>(state.x);
            const float dy = static_cast<float>(motion.pos[1]) - static_cast<float>(state.y);
            const float dz = static_cast<float>(motion.pos[2]) - static_cast<float>(state.z);
            if (std::sqrt(dx * dx + dy * dy + dz * dz) < 16.0f) {
                teleportMotionBoundary = motion.tick;
                std::erase_if(futureMotion, [&](const auto& v) { return v.tick <= motion.tick; });
                size_t first = history.size();
                for (size_t i = 0; i < history.size(); ++i) {
                    if (history[i].tick <= motion.tick)
                        continue;
                    history[i].teleport = motion;
                    if (history[i].tick == motion.tick + 1)
                        history[i].motion.reset();
                    first = i;
                    break;
                }
                if (first == history.size())
                    futureTeleports.push_back(motion);
                else
                    ReplayFrom(session, first);
                if (PERLIN_ENV("PERLIN_BLOCK_DEBUG"))
                    Log("move", "teleport_replay stamp={} records={} queued={}", motion.tick, history.size() - first, (first == history.size()));
                handled = true;
                motion.hasVelocity = false;
            }
        }
        if (motion.correction) {
            if (teleportMotionBoundary && motion.tick > *teleportMotionBoundary)
                state.hasTeleported = false;
            while (!history.empty() && history.front().tick < motion.tick)
                history.pop_front();
            if (!history.empty() && history.front().tick == motion.tick) {
                Record& r = history.front();
                const double dx = r.after.x - motion.pos[0], dy = r.after.y - motion.pos[1],
                             dz = r.after.z - motion.pos[2];
                const double ex = r.after.vx - motion.velocity[0];
                const double ey = r.after.vy - motion.velocity[1];
                const double ez = r.after.vz - motion.velocity[2];
                const double posErr = dx * dx + dy * dy + dz * dz;
                const double velErr = ex * ex + ey * ey + ez * ez;
                if (posErr > 0.0 || velErr > 0.0 || r.after.onGround != motion.onGround) {
                    const physics::State shown = state;
                    if (PERLIN_ENV("PERLIN_BLOCK_DEBUG") || correctionReplays < 40 || correctionReplays % 100 == 0) {
                        const Level& lv = session.NetLevel();
                        auto name = [&](int bx, int by, int bz) { return BlockPalette::Instance().Name(lv.GetBlock(bx, by, bz)); };
                        const int fx = static_cast<int>(std::floor(r.before.x)), fz = static_cast<int>(std::floor(r.before.z));
                        const int fy = static_cast<int>(std::floor(r.before.y - 0.001));
                        std::ostringstream diagnostic;
                        diagnostic << std::setprecision(std::numeric_limits<double>::max_digits10);
                        diagnostic << "[move] mismatch tick=" << motion.tick << " ours=(" << r.after.x << "," << r.after.y << ","
                                  << r.after.z << ") server=(" << motion.pos[0] << "," << motion.pos[1] << ","
                                  << motion.pos[2] << ") vel ours=(" << r.after.vx << "," << r.after.vy << "," << r.after.vz
                                  << ") server=(" << motion.velocity[0] << "," << motion.velocity[1] << ","
                                  << motion.velocity[2] << ") ground ours=" << r.after.onGround
                                  << " server=" << motion.onGround << " fwd=" << r.input.forward << " strafe="
                                  << r.input.strafe << " jump=" << r.input.jump << " sprint=" << r.input.sprint
                                  << " sneak=" << r.input.sneak << " feet=" << name(fx, fy + 1, fz) << " under="
                                  << name(fx, fy, fz) << " east=" << name(fx + 1, fy + 1, fz) << " west="
                                  << name(fx - 1, fy + 1, fz) << " north=" << name(fx, fy + 1, fz - 1)
                                  << " south=" << name(fx, fy + 1, fz + 1)
                                  << " before=(" << r.before.x << "," << r.before.y << "," << r.before.z
                                  << ") beforeVel=(" << r.before.vx << "," << r.before.vy << "," << r.before.vz
                                  << ") yaw=" << r.yaw << " speed=" << r.attributes.speed;
                        std::cout << diagnostic.str() << std::endl;
                    }
                    r.after.x = motion.pos[0];
                    r.after.y = motion.pos[1];
                    r.after.z = motion.pos[2];
                    r.after.vx = motion.velocity[0];
                    r.after.vy = motion.velocity[1];
                    r.after.vz = motion.velocity[2];
                    r.after.onGround = motion.onGround;
                    physics::State s = r.after;
                    LevelCollision world(session.NetLevel(), blocksPtr);
                    for (size_t i = 1; i < history.size(); i++) {
                        Record& q = history[i];
                        q.before = s;
                        s.yaw = q.yaw;
                        ApplyTeleport(s, q.teleport);
                        ApplyMotion(s, q.motion);
                        if (q.simulated)
                            physics::Step(s, q.input, q.attributes, world);
                        else
                            s.vx = s.vy = s.vz = 0;
                        q.after = s;
                    }
                    state = s;
                    const double ox = shown.x - state.x, oy = shown.y - state.y, oz = shown.z - state.z;
                    if (ox * ox + oy * oy + oz * oz < 4.0) {
                        smoothOffset[0] += ox;
                        smoothOffset[1] += oy;
                        smoothOffset[2] += oz;
                    }
                    previous = state;
                    if (!history.empty())
                        previous = history.back().before;
                    session.SetPose(static_cast<float>(state.x), static_cast<float>(state.y),
                                    static_cast<float>(state.z), state.yaw, session.GetPose().pitch);
                    correctionReplays++;
                    if (correctionReplays <= 5 || correctionReplays % 100 == 0)
                        Log("move", "replayed correction tick={} posErr={} velErr={} replayed={}", motion.tick, std::sqrt(posErr), std::sqrt(velErr), (history.size() - 1));
                }
                history.pop_front();
                handled = true;
                motion.hasVelocity = false;
            }
        }
        if (motion.poseChanged && !handled) {
            state.x = motion.pos[0];
            state.y = motion.pos[1];
            state.z = motion.pos[2];
            previous = state;
            accumulator = 0.0;
            history.clear();
            if (motion.teleport) {
                teleportMotionBoundary = motion.tick ? motion.tick : session.LastAuthTick();
                futureMotion.clear();
                futureTeleports.clear();
                state.hasTeleported = motion.filterTravel;
                state.vx = state.vy = state.vz = 0;
                state.onGround = motion.onGround;
                smoothOffset[0] = smoothOffset[1] = smoothOffset[2] = 0;
            }
            if (motion.correction)
                state.onGround = motion.onGround;
        }
        if (motion.hasVelocity) {
            history.clear();
            state.vx = motion.velocity[0];
            state.vy = motion.velocity[1];
            state.vz = motion.velocity[2];
        }
        if (motion.hasRotation) {
            camera.yaw = CameraYaw(motion.yaw);
            camera.pitch = std::clamp(BedrockPitch(motion.pitch), -89.0f, 89.0f);
        }
    }

    void LocalPlayer::Tick(BedrockSession& session, const InputManager& input, const Camera& camera,
                           const physics::World& world) {
        const uint64_t nextTick = session.LastAuthTick() + 1;
        std::optional<BedrockSession::ServerMotion> tickTeleport;
        for (auto it = futureTeleports.begin(); it != futureTeleports.end();) {
            if (it->tick < nextTick) {
                tickTeleport = *it;
                it = futureTeleports.erase(it);
            } else {
                ++it;
            }
        }
        ApplyTeleport(state, tickTeleport);
        previous = state;
        const physics::State before = state;
        const bool currentWater = physics::TouchingWater(state, world);
        const bool itemSlow = this->itemSlow && input.IsDown(Action::Use)
                           && (!itemSlowUntil || tickCounter < itemSlowUntil);

        const bool asleep = session.Sleeping();
        const int dimension = session.DimensionId();
        const bool dimensionChanged = replayDimension && *replayDimension != dimension;
        replayDimension = dimension;
        const bool dimensionChanging = session.DimensionChanging();
        const bool savePrediction = !asleep && !rideLock && !dimensionChanged && !dimensionChanging;
        if (!savePrediction)
            history.clear();
        const float forwardKeys = asleep ? 0.0f : input.Axis(Action::Forward, Action::Back);
        const float strafeKeys = asleep ? 0.0f : input.Axis(Action::Right, Action::Left);
        const float forward = forwardKeys;
        const float strafe = strafeKeys;
        const bool jump = !asleep && input.IsDown(Action::Jump);
        const bool sneakHeldKey = input.IsDown(Action::Sneak);
        const bool sneakKey = sneakToggle ? sneakLatch : sneakHeldKey;
        if (rideLock && sneakKey && !sneakHeld && !(session.InputLocks() & 256u))
            session.SendLeaveVehicle();
        const bool sprintHeldKey = input.IsDown(Action::Sprint);
        const bool sprintKey = sprintToggle ? sprintLatch : sprintHeldKey;

        const bool wasSprinting = sprinting;
        const BedrockSession::Vitals vitals = session.GetVitals();
        tooHungryToSprint = vitals.known && vitals.hunger <= 6.0f;
        const bool stopSprintIntent = forward <= 0.0f || sneakKey || itemSlow || state.horizontalCollision || tooHungryToSprint;
        if ((stopSprintIntent && !(swimming && currentWater)) || (swimming && !currentWater))
            sprinting = false;
        else if (!stopSprintIntent && sprintKey && (sprinting || state.onGround || currentWater || flying))
            sprinting = true;

        const bool wasSneaking = sneaking;
        const auto headInWaterAt = [&](float eyeHeight) {
            const int x = static_cast<int>(std::floor(state.x)), z = static_cast<int>(std::floor(state.z));
            const float eyeY = static_cast<float>(state.y) + eyeHeight;
            const int y = static_cast<int>(std::floor(eyeY));
            const auto cell = world.Query(x, y, z);
            if (cell.fluid != 1) return false;
            const float top = world.Query(x, y + 1, z).fluid == 1 ? 1.0f : cell.fluidLevel;
            return eyeY < static_cast<float>(y) + top;
        };
        const bool headInWaterBeforePose = headInWaterAt(static_cast<float>(
            (swimming || crawling || gliding) ? physics::kCrawlEye : sneaking ? physics::kSneakEye : physics::kStandEye));
        bool forcedSneak = false;
        bool wantCrawl = false;
        if (!flying && !rideLock && !gliding && !swimming) {
            if (!physics::PoseFits(state, world, physics::kStandHeight)) {
                if (physics::PoseFits(state, world, physics::kSneakHeight))
                    forcedSneak = true;
                else
                    wantCrawl = crawling || state.crawling;
            }
        }
        crawling = wantCrawl;
        sneaking = (sneakKey || forcedSneak) && !flying;
        if (crawling) {
            sprinting = false;
            sneaking = true;
        }

        const bool column = session.ColumnSettled(static_cast<int>(std::floor(state.x)) >> 4,
                                                  static_cast<int>(std::floor(state.z)) >> 4);
        const bool simulate = column && !asleep && !dimensionChanging;

        bool levitating = false;
        for (const auto& effect : session.GetEffects())
            if (effect.id == 24)
                levitating = true;
        const auto swimBox = physics::BoundingBox(state, wasSneaking);
        const auto aboveSwimBox = world.Query(
            static_cast<int>(std::floor(static_cast<float>((swimBox.x0 + swimBox.x1) * 0.5))),
            static_cast<int>(std::floor(static_cast<float>((swimBox.y0 + swimBox.y1) * 0.5))) + 1,
            static_cast<int>(std::floor(static_cast<float>((swimBox.z0 + swimBox.z1) * 0.5))));
        const float lookPitch = physics::bds::InterpolateAngle(state.previousRotation.pitch,
                                                               BedrockPitch(camera.pitch), 1.0f);
        const float lookCoordinate = physics::bds::detail::Round(
            physics::bds::detail::Round(lookPitch * -0.017453292f) * 10430.378f);
        const float lookY = physics::bds::CurrentSineEntry(physics::bds::SineIndex(lookCoordinate));
        const bool canStartSwimUp = lookY < 0.15f || !aboveSwimBox.isAir
                                || aboveSwimBox.fluid != 0 || !aboveSwimBox.Empty();
        const bool wantsSwimming = sprinting && !stopSprintIntent && !flying
                                && (swimming ? currentWater : headInWaterBeforePose && canStartSwimUp);
        const bool canStopSwimming = !swimming || physics::PoseFits(state, world, physics::kStandHeight);
        swimming = !flying && (wantsSwimming || (swimming && !canStopSwimming));
        if (gliding && (!hasElytra || state.onGround || state.inWater || flying || rideLock))
            gliding = false;
        else if (!gliding && hasElytra && jump && !jumpHeld && !state.onGround && !state.inWater && !state.inLava &&
                 !state.onClimbable && !flying && !rideLock && !levitating && !crawling)
            gliding = true;
        {
            const auto glideNow = std::chrono::steady_clock::now();
            const bool serverGlide = session.ServerGliding();
            if (serverGlide)
                glideReported = true;
            if (gliding != glideLocal) {
                glideLocal = gliding;
                glideChangedAt = glideNow;
            }
            if (glideReported && glideNow - glideChangedAt > std::chrono::milliseconds(600)) {
                if (gliding && !serverGlide) {
                    gliding = false;
                    glideLocal = false;
                    glideChangedAt = glideNow;
                } else if (!gliding && serverGlide && hasElytra && !state.onGround && !state.inWater && !flying && !rideLock) {
                    gliding = true;
                    glideLocal = true;
                    glideChangedAt = glideNow;
                }
            }
        }
        physics::Input in;
        in.gliding = gliding;
        in.crawl = crawling;
        in.forceCrouch = forcedSneak;
        in.pitch = BedrockPitch(camera.pitch);
        in.swimming = swimming && !gliding;
        in.headInWater = headInWaterAt(static_cast<float>((swimming || crawling || gliding) ? physics::kCrawlEye
                                                         : sneaking ? physics::kSneakEye : physics::kStandEye));
        in.lookY = lookY;
        const physics::MoveVector move = physics::ProcessMoveInput(forward, strafe, sneaking, currentWater,
                                                                  itemSlow ? 0.35f : 1.0f);
        in.forward = move.forward;
        in.strafe = move.strafe;
        in.jump = jump;
        in.sneak = sneakKey;
        in.sprint = sprinting;
        in.flying = flying;
        in.flyUp = flying && jump;
        in.flyDown = flying && sneakKey;
        in.noClip = flying && (session.NoClip() || session.Spectator());
        in.creativeFlight = session.GameType() == 1 ||
            (session.GameType() == 5 && session.World().worldGameMode == 1);
        const auto actorFlags = session.OwnActorFlags();
        in.canClimb = (actorFlags[0] & proto::FlagsMetadataFlags1::CanClimb) != 0;
        in.wallClimbing = (actorFlags[0] & proto::FlagsMetadataFlags1::Wallclimbing) != 0;
        in.actorMovementFlags = static_cast<uint8_t>(actorFlags[0]);
        in.pushTowardsClosestSpace = (actorFlags[1] & proto::FlagsMetadataFlags2::PushTowardsClosestSpace) != 0;
        in.gravity.hasGravity = (actorFlags[0] & proto::FlagsMetadataFlags1::AffectedByGravity) != 0;
        in.gravity.inScaffolding = (actorFlags[1] & proto::FlagsMetadataFlags2::InScaffolding) != 0;
        in.gravity.overScaffolding = (actorFlags[1] & proto::FlagsMetadataFlags2::OverScaffolding) != 0;
        in.gravity.descendThroughBlock = (actorFlags[1] & proto::FlagsMetadataFlags2::FallThroughScaffolding) != 0;
        const auto boots = session.GetInventory().armor[3];
        in.canStandOnPowderSnow = !boots.Empty() &&
            (boots.name == "minecraft:leather_boots" || boots.name == "leather_boots");
        const auto contactBox = physics::BoundingBox(state, wasSneaking);
        const auto scaffold = physics::bds::SenseScaffolding(
            physics::bds::CollisionBox{static_cast<float>(contactBox.x0), static_cast<float>(contactBox.y0),
                static_cast<float>(contactBox.z0), static_cast<float>(contactBox.x1),
                static_cast<float>(contactBox.y1), static_cast<float>(contactBox.z1)},
            [&](int32_t x, int32_t y, int32_t z) {
                return world.ClimberCell(x, y, z);
            }, in.canStandOnPowderSnow);
        in.gravity.inScaffolding = scaffold.inside;
        in.gravity.overScaffolding = scaffold.over;
        in.inAscendableBlock = scaffold.ascendable;
        in.overDescendableBlock = scaffold.descendable;
        in.gravity.descendThroughBlock = in.sneak && in.overDescendableBlock;

        physics::Attributes attributes;
        attributes.speed = session.PredictedMovementSpeed(sprinting);
        attributes.flySpeed = session.FlySpeed();
        attributes.verticalFlySpeed = session.VFlySpeed();
        attributes.airDrag = session.AirDragAttribute();
        attributes.waterWalkSpeedEnchant = session.WaterWalkSpeedEnchant();
        attributes.underwaterSpeed = session.UnderwaterMovementSpeedAttribute();
        for (const auto& effect : session.GetEffects()) {
            if (effect.id == 8)
                attributes.jumpBoost = effect.amplifier + 1;
            else if (effect.id == 24)
                attributes.levitation = effect.amplifier + 1;
            else if (effect.id == 27)
                attributes.slowFalling = true;
            else if (effect.id == 33)
                attributes.weaving = true;
        }

        if (simulate)
            for (const auto& effect : session.TakeMovementEffects())
                in.movementEffects.push_back({effect.tick, std::bit_cast<int32_t>(effect.duration),
                                              static_cast<int32_t>(effect.type)});

        state.yaw = BedrockYaw(camera.yaw);
        Record rec;
        rec.input = in;
        rec.attributes = attributes;
        rec.yaw = state.yaw;
        rec.before = state;
        rec.simulated = simulate;
        rec.teleport = tickTeleport;
        for (auto it = futureMotion.begin(); it != futureMotion.end();) {
            if (it->tick < nextTick) {
                rec.motion = it->velocity;
                if (PERLIN_ENV("PERLIN_BLOCK_DEBUG"))
                    Log("move", "motion_apply stamp={} target={}", it->tick, nextTick);
                it = futureMotion.erase(it);
            } else {
                ++it;
            }
        }
        ApplyMotion(state, rec.motion);
        if (simulate) {
            physics::Step(state, in, attributes, world);
        } else {
            state.vx = state.vy = state.vz = 0;
        }

        if (rideLock) {
            state.x = rideX;
            state.y = rideY;
            state.z = rideZ;
            state.vx = state.vy = state.vz = 0;
            state.onGround = true;
            flying = false;
        }
        walkBob.Tick(std::hypot(state.x - before.x, state.z - before.z), state.onGround && !flying,
                     state.inWater && sprinting);
        ++tickCounter;

        if (flying && state.onGround && state.verticalCollision && !in.flyUp && !in.noClip) {
            flying = false;
            sendStopFlying = true;
            flyPending = true;
            flyPendingAt = std::chrono::steady_clock::now();
        }

        int slownessAmplifier = -1;
        for (const auto& effect : session.GetEffects())
            if (effect.id == 2)
                slownessAmplifier = effect.amplifier;
        fov.Tick(MovementFovTarget(session.MovementSpeedAttribute(), session.WalkSpeed(),
                                   flying, slownessAmplifier));

        BedrockSession::MoveInput packet;
        std::vector<uint32_t>& f = packet.flags;
        const bool goLeft = strafe < 0.0f, goRight = strafe > 0.0f;
        const bool goForward = forward > 0.0f, goBack = forward < 0.0f;
        if (goForward) f.push_back(InputFlag::Up);
        if (goBack) f.push_back(InputFlag::Down);
        if (goLeft) f.push_back(InputFlag::Left);
        if (goRight) f.push_back(InputFlag::Right);
        if (goForward && goLeft) f.push_back(Flag::UpLeft);
        if (goForward && goRight) f.push_back(Flag::UpRight);
        if (goBack && goLeft) f.push_back(Flag::DownLeft);
        if (goBack && goRight) f.push_back(Flag::DownRight);

        const bool jumpInitiated = !flying && before.jumpDelay == 0 && state.jumpDelay == 9;
        if (jump) {
            f.push_back(Flag::JumpDown);
            f.push_back(Flag::Jumping);
            f.push_back(Flag::JumpCurrentRaw);
            f.push_back(Flag::WantUp);
        }
        if (jumpInitiated)
            f.push_back(Flag::StartJumping);
        if (jump && !jumpHeld)
            f.push_back(Flag::JumpPressedRaw);
        if (!jump && jumpHeld)
            f.push_back(Flag::JumpReleasedRaw);
        jumpHeld = jump;

        if (sneakKey) {
            f.push_back(Flag::Sneaking);
            f.push_back(Flag::SneakDown);
            f.push_back(Flag::SneakCurrentRaw);
            f.push_back(Flag::WantDown);
        }
        if (sneaking && !wasSneaking)
            f.push_back(Flag::StartSneaking);
        if (!sneaking && wasSneaking)
            f.push_back(Flag::StopSneaking);
        if (sneakKey && !sneakHeld)
            f.push_back(Flag::SneakPressedRaw);
        if (!sneakKey && sneakHeld)
            f.push_back(Flag::SneakReleasedRaw);
        sneakHeld = sneakKey;

        if (sprintKey)
            f.push_back(Flag::SprintDown);
        if (sprinting)
            f.push_back(Flag::Sprinting);
        if (sprinting && !wasSprinting)
            f.push_back(Flag::StartSprinting);
        if (!sprinting && wasSprinting)
            f.push_back(Flag::StopSprinting);
        sprintHeld = sprintKey;
        if (swimming && !sentSwimming)
            f.push_back(Flag::StartSwimming);
        if (!swimming && sentSwimming)
            f.push_back(Flag::StopSwimming);
        sentSwimming = swimming;
        if (crawling && !sentCrawling)
            f.push_back(Flag::StartCrawling);
        if (!crawling && sentCrawling)
            f.push_back(Flag::StopCrawling);
        sentCrawling = crawling;
        if (gliding && !sentGliding)
            f.push_back(Flag::StartGliding);
        if (!gliding && sentGliding)
            f.push_back(Flag::StopGliding);
        sentGliding = gliding;

        if (flying && jump)
            f.push_back(Flag::Ascend);
        if (flying && sneakKey)
            f.push_back(Flag::Descend);
        if (sendStartFlying) {
            f.push_back(InputFlag::StartFlying);
            sendStartFlying = false;
        }
        if (sendStopFlying) {
            f.push_back(InputFlag::StopFlying);
            sendStopFlying = false;
        }
        if (state.horizontalCollision)
            f.push_back(Flag::HorizontalCollision);
        if (state.verticalCollision)
            f.push_back(Flag::VerticalCollision);

        packet.moveX = -move.strafe;
        packet.moveZ = move.forward;
        packet.rawX = -strafeKeys;
        packet.rawZ = forwardKeys;
        packet.yaw = BedrockYaw(camera.yaw);
        packet.pitch = BedrockPitch(camera.pitch);
        packet.eyeHeight = kPlayerEyeHeight;
        packet.position[0] = static_cast<float>(state.x);
        packet.position[1] = static_cast<float>(state.y);
        packet.position[2] = static_cast<float>(state.z);
        packet.delta[0] = static_cast<float>(state.vx);
        packet.delta[1] = static_cast<float>(state.vy);
        packet.delta[2] = static_cast<float>(state.vz);
        packet.hasDelta = true;

        session.SetPose(packet.position[0], packet.position[1], packet.position[2], packet.yaw, packet.pitch);
        if (session.SendMoveInput(packet)) {
            rec.tick = session.LastAuthTick();
            rec.after = state;
            if (tickObserver) tickObserver(rec.tick, state, rec.input);
            if (savePrediction)
                history.push_back(rec);
            while (history.size() > 200)
                history.pop_front();
        }
    }

    void LocalPlayer::ModMotion(bool add, double x, double y, double z, bool ax, bool ay, bool az) {
        std::lock_guard lock(modMutex);
        if (modQueue.size() < 256)
            modQueue.push_back({false, add, false, ax, ay, az, x, y, z});
    }

    void LocalPlayer::ModTeleport(double x, double y, double z, bool keepVelocity) {
        std::lock_guard lock(modMutex);
        if (modQueue.size() < 256)
            modQueue.push_back({true, false, keepVelocity, true, true, true, x, y, z});
    }

    void LocalPlayer::ApplyModCommands() {
        std::vector<ModCommand> list;
        {
            std::lock_guard lock(modMutex);
            list.swap(modQueue);
        }
        for (const ModCommand& c : list) {
            if (c.teleport) {
                state.x = c.x;
                state.y = c.y;
                state.z = c.z;
                state.hasBox = false;
                if (!c.keep)
                    state.vx = state.vy = state.vz = 0.0;
                previous = state;
                history.clear();
                continue;
            }
            if (c.ax)
                state.vx = c.add ? state.vx + c.x : c.x;
            if (c.ay)
                state.vy = c.add ? state.vy + c.y : c.y;
            if (c.az)
                state.vz = c.add ? state.vz + c.z : c.z;
        }
    }

    void LocalPlayer::Update(BedrockSession& session, InputManager& input, Camera& camera,
                             const BlockLibrary& blocks, float dt) {
        BedrockSession::PlayerPose pose = session.GetPose();
        if (!pose.known)
            return;

        if (!initialised) {
            state = {};
            state.x = pose.x;
            state.y = pose.y;
            state.z = pose.z;
            previous = state;
            initialised = true;
            flying = session.IsFlying();
        }
        blocksPtr = &blocks;
        if (sneakToggle) {
            if (input.Pressed(Action::Sneak))
                sneakLatch = !sneakLatch;
        } else {
            sneakLatch = false;
        }
        if (sprintToggle) {
            if (input.Pressed(Action::Sprint))
                sprintLatch = !sprintLatch;
        } else {
            sprintLatch = false;
        }
        if (session.Sleeping() || (replayDimension && *replayDimension != session.DimensionId()))
            history.clear();
        if (rideLock) {
            session.TakeEffectUpdates();
            futureMotion.clear();
            futureTeleports.clear();
            BedrockSession::ServerMotion ignored;
            while (session.TakeServerMotion(ignored)) {}
            history.clear();
        } else {
            ApplyEffectUpdates(session);
            BedrockSession::ServerMotion motion;
            while (session.TakeServerMotion(motion))
                ApplyServerMotion(session, camera, motion);
        }
        ApplyModCommands();

        const auto now = std::chrono::steady_clock::now();
        const bool spectator = session.NoClip() || session.Spectator();
        if (spectator) {
            if (!flying) {
                flying = true;
                sendStartFlying = true;
            }
        } else if (flying && !session.MayFly()) {
            flying = false;
            sendStopFlying = true;
        }
        if (flyPending && (session.IsFlying() == flying || now - flyPendingAt > std::chrono::milliseconds(750)))
            flyPending = false;
        if (!flyPending && session.IsFlying() != serverFlying) {
            serverFlying = session.IsFlying();
            if (serverFlying && session.MayFly() && !flying) {
                flying = true;
            } else if (!serverFlying && flying && !spectator) {
                flying = false;
            }
        }
        if (session.MayFly() && !spectator && input.Pressed(Action::Jump)) {
            if (now - lastJumpPress <= kDoubleTapWindow) {
                flying = !flying;
                (flying ? sendStartFlying : sendStopFlying) = true;
                flyPending = true;
                flyPendingAt = now;
                lastJumpPress = {};
            } else {
                lastJumpPress = now;
            }
        }
        if (input.Pressed(Action::Forward)) {
            if (now - lastForwardPress <= kSprintTapWindow && !sneaking && !tooHungryToSprint)
                sprinting = true;
            lastForwardPress = now;
        }

        LevelCollision world(session.NetLevel(), &blocks);
        if (!session.Initialized()) {
            accumulator = 0.0;
            previous = state;
        } else {
            accumulator += dt;
        }
        int ticks = 0;
        while (accumulator >= physics::kTickSeconds) {
            accumulator -= physics::kTickSeconds;
            if (++ticks > kMaxTicksPerFrame) {
                accumulator = 0.0;
                break;
            }
            input.SetControlTick(tickCounter);
            Tick(session, input, camera, world);
        }

        const double t = std::clamp(accumulator / physics::kTickSeconds, 0.0, 1.0);
        const double x = previous.x + (state.x - previous.x) * t;
        const double y = previous.y + (state.y - previous.y) * t;
        const double z = previous.z + (state.z - previous.z) * t;
        const float targetEye = session.Sleeping() ? 0.35f : rideLock ? 1.2f : (crawling || gliding || swimming) ? static_cast<float>(physics::kCrawlEye) : sneaking ? static_cast<float>(physics::kSneakEye) : static_cast<float>(physics::kStandEye);
        eyeOffset += (targetEye - eyeOffset) * (1.0f - std::exp(-dt * 20.0f));
        const double offsetSize = std::sqrt(smoothOffset[0] * smoothOffset[0] + smoothOffset[1] * smoothOffset[1] +
                                            smoothOffset[2] * smoothOffset[2]);
        const double decay = std::exp(-dt * (offsetSize < 0.4 ? 7.0 : 14.0));
        for (double& o : smoothOffset)
            o = std::fabs(o) < 1e-4 ? 0.0 : o * decay;
        camera.x = static_cast<float>(x + smoothOffset[0]);
        camera.y = static_cast<float>(y + smoothOffset[1]) + eyeOffset;
        camera.z = static_cast<float>(z + smoothOffset[2]);
    }
}
