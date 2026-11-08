# Handoff: animation parity and leftover client fixes

Branch: `anim-parity` on `imqstr/perlin`, based on `upstream/main` at `d6af9f8` ("fix textures").
Older work lives on `session/work-ui-text-packs-java-shaders`, pushed to the same fork. Commit hashes
below refer to that branch unless noted.

## State of this branch

| Commit | What | Status |
|---|---|---|
| `7e3733e` | Cull entities by their geometry's `visible_bounds` (Skywars cage border flicker) | Cherry-picked from `d4193f2`; conflicts resolved, **not built or tested on this base** |

First thing to do:

```bash
cmake --build build -j$(nproc)
ctest --test-dir build -j$(nproc)
```

Then launch, join a Skywars match, turn away from the cage border and walk off; it must stay drawn.

Upstream already has its own version of the Build Battle hologram fix (`SetVariant` now takes
`std::optional<int>` in `EntityTracker.cpp:309` and `BedrockHandlers.cpp:194`). Do not port `415f290`.

## The task: animations 1:1

Target set by the user:

- Anything that existed in **Java 1.8.9** (walk, swing, sneak, idle bob, riding, bow, sword, eating,
  head and body yaw): match **1.8.9** exactly.
- Anything newer (shield block, trident throw/riptide, crossbow, spyglass, goat horn, brush, swim,
  elytra, crawl): match the **latest Java client**.
- Movement stays Bedrock-exact; animations are render only and must not change prediction.

### Where animation code lives

- `src/client/entity/JavaAnimation.{hpp,cpp}`: `AnimatePlayer` (Java `HumanoidModel`/`PlayerModel`
  pose, checked against the Java 26.3 client JAR), `ApplyPlayerPose` (re-expresses Java's root-level
  parts in Bedrock's parented skeleton), `BodyRotation` (body/head yaw), `PlayerRootTransform`.
- `src/client/entity/EntityRenderer.cpp`: per-entity tracking (`Track`), swing timing
  (`v.attackTime`), limb swing (`v.limbSwing`, `v.limbAmount`), the Molang animation pass
  (`RunAnimations`), `ApplyJavaCrouch`, armor and attachable meshes.
- `src/client/entity/EntityAssets.cpp`: `kJavaAnimations`, a Molang override file written to
  `<data dir>/animation_overrides/java.animation.json` on top of the vanilla Bedrock animations.
- `src/client/hand/HandRenderer.cpp`: first-person hand and swing (20 Hz, Java's restart rule).
- Reference data: `tests/data/java_animation_reference.txt` (400 poses) and
  `tests/data/java_animation_root_reference.txt` (120 root matrices), both from the real Java 26.3
  client JAR. Generator: `tools/gen_java_animation_reference.py` + `tools/animation_reference/AnimationOracle.java`,
  documented in `docs/entity-animation.md`. `entity_animation_selftest` checks against them.

### What is wrong today (on upstream)

The verified Java path (`AnimatePlayer` + `ApplyPlayerPose`) is **not called by the renderer**.
Third-person players are posed by Bedrock Molang plus a few patches, which is why animations look off.
On the session branch these commits fixed most of that; port them first, in this order:

1. `4830ccb` Fix head yaw sign in the crouch refit. `ApplyJavaCrouch` negated yaw when re-extracting
   Euler angles; turning while sneaking rolled the head onto the shoulder. Numerically verified.
2. `5d5cf57` Pose third-person players with the Java 26.3 model code. Wires `AnimatePlayer` +
   `ApplyPlayerPose` after the Molang pass; maps held items to `ArmPose` (shield → Block, bow, trident,
   crossbow, spyglass, horn, brush). `PERLIN_NO_JAVA_POSE` turns it off.
3. `4fa04b6` Fit worn armor to the Java pose. Armor geometry parents bones differently
   (legs under body); refit per armor geometry, using the player's pivots. Fixed leggings/boots
   detaching while sneaking. Verified on frames.
4. `d2f92fd` Third-person swing plays whole swings (restart only when finished) so mining does not
   loop the first half every tick.
5. `b7fbd49` Install `java.animation.json` overrides unconditionally (producer/consumer gate mismatch).

Expect conflicts: upstream moved `EntityRenderer.cpp` a lot since then.

### What still has to be built

1. **A 1.8.9 reference.** None is on disk. Do it the same way as 26.3: run the real 1.8.9 client and dump
   poses. 1.8.9 is obfuscated, so `AnimationOracle.java` needs MCP 1.8.9 (stable_22) mappings
   for `ModelBiped`, `ModelPlayer`, `EntityLivingBase`, `RenderPlayer`. Client JAR comes from Mojang's
   version manifest (`piston-meta.mojang.com`). Write `tests/data/java189_animation_reference.txt`
   and a selftest like the 26.3 one. **Do not hand-tune constants from memory.** An attempt to change
   `BodyRotation::PlayerTick` to 1.8.9 from recall (±75° clamp, `f*f > 2500` drift, no 95° flip) broke
   the existing asserts and was reverted, because those asserts were its only spec.
2. **Split by feature.** Choose per pose: 1.8.9 path for parts that existed then, 26.x path for newer
   `ArmPose` values (Block, Trident, CrossbowCharge/Hold, Spyglass, Horn, Brush), swim, glide, crawl.
3. **Bone-fit stage has no reference rows.** The 400-row reference stops at `AnimatePlayer` output;
   `ApplyPlayerPose`/`ApplyJavaCrouch` (fit + Euler decomposition) are only checked by round-trip tests.
   Extend the oracle to dump post-hierarchy world matrices.
4. **Chestplate arms show skin** below the shoulder pad, in the old path too. Inheritance and bone
   inflate in `Geometry.cpp` `Link()` look right; cause not found yet.
5. **Mining swing**: Java 1.8.9 first person loops the first half every 3 ticks while mining
   (`swingItem` each tick, restart past half). The user said it looked too fast; third person was
   changed to whole swings (`d2f92fd`). Confirm with the user against a real 1.8.9 recording.
6. **Animals** were reported broken; not investigated. They go through Bedrock Molang only.

### Bedrock client database

`~/Downloads/WindowsClient_26.51.7z` contains `Minecraft.Windows.exe.i64` (6.1 GB unpacked), an IDA
database of the Bedrock Windows client 26.51. It does **not** help with Java animations; useful for
Bedrock-side render behaviour (actor render, visible bounds, culling). IDA 9.4 is at
`~/.local/share/ida/ida94/` and runs headless under wine:

```bash
cd ~/.local/share/ida/ida94 && WINEDEBUG=-all wine idat.exe -A -S'Z:\path\script.py' -L'Z:\path\log.txt' 'Z:\path\Minecraft.Windows.exe.i64'
```

In `-A` mode IDA answers prompts by itself. If an unpacked database (`.id0/.id1/...`) from a crash sits
next to the `.i64` it may pick "restore packed base" and open an empty database (`internal error 1228`).
Keep a copy.

## Other open bugs (reported by the user, not fixed on upstream)

All of these have fixes on the session branch; port and retest:

- `e40b699`, `ba63305` custom block planes vanishing from some angles (`alpha_test`/`double_sided` must
  draw both sides; `render_method` must beat texture alpha; read permutation `material_instances`)
- `e0a2406` custom block items in chests show as `minecraft:unknown` (negative ids = `255 - legacy block id`;
  name from the stack's `blockRuntimeId`)
- `5816c78` chests close the instant they open (stale `serverClosedInventory` from an earlier server close)
- `771f38f` arrows/projectiles face the wrong way (use server rotation; projectiles use `atan2(dx, dz)`)
- `50e6cb8` floating text too high on scaled entities (hitbox scale applied twice)
- `b1df8c9` disconnect reason text overlapping (wrap at 6 px per glyph)
- `4e6d31a` no fire overlay on burning entities (port of Java `renderFlame`; never checked on screen)
- `966d814` leave the server on SIGTERM/crash; NetherNet now closes its peer connection right away.
  Built, not verified: an immediate rejoin still conflicted before the NetherNet change and was not rerun.
- `fef3b44` slime bounce: bounce tick applies gravity only for the part of the tick after contact,
  `extra = 0.0784 * resolved / requested`. Fit from per-tick server traces; slime drops went 6 → 0
  corrections. Check upstream's `PlayerPhysics.cpp` before porting, it changed.

Still unfixed anywhere: `hive:wooden_stool` invisible (a local replica with the same geometry renders,
so it is something in that server's pack set), Hive logo texture, map selector row gap and other
JSON UI form layout, riptide/loyalty prediction, bridging (hold-to-place), sounds (XP sound),
chestplate arms.

## Testing setup

- `tools/restart_perlin.sh` (session branch) rebuilds, restarts the client by exact process name
  (`pkill -x Perlin`, never `pgrep -f`, which matches your own shell), launches through
  `mullvad-exclude`, and fails if the running process predates the binary.
  `PERLIN_SERVER=geo.hivebedrock.network:19132 bash tools/restart_perlin.sh` joins the Hive.
- Control port: `--control-port 18765`, token printed in the log. `GET /state`, `GET /frame` (JPEG),
  `POST /command` with `{"op":"input"|"look"|"command"|...}`. Frames are how poses were checked.
- Local BDS 1.26.60.6 in `server/bedrock_server/`. Start it with a console FIFO so you can run server
  commands (`tp`, `setblock`, `querytarget` for per-tick server position):

  ```bash
  mkfifo /tmp/bds_console; sleep infinity > /tmp/bds_console &
  cd server/bedrock_server && LD_LIBRARY_PATH=. ./bedrock_server < /tmp/bds_console > ../../output/bds-validation.log 2>&1 &
  echo "querytarget hesrllycool" > /tmp/bds_console
  ```
- `tools/test_packs/` (session branch): behavior + resource pack with custom-geometry, vanilla-texture-key
  and double-sided blocks. See its README.
- Restarting the client within ~30 s of the last run gets `ServerIdConflict` (reason 44) until the
  graceful-leave fix is verified.
- Comment style: `AGENTS.md`. Lowercase fragments, say why, no server or client names in comments.
