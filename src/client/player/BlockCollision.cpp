#include "client/player/BlockCollision.hpp"
#include "client/player/VoxelShapes.hpp"
#include "client/renderer/block/BlockGeometry.hpp"
#include "client/resources/ServerBlocks.hpp"

#include "client/renderer/block/BlockLibrary.hpp"
#include "util/Paths.hpp"
#include "util/Resources.hpp"
#include "world/level/block/BlockPalette.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <string>

namespace perlin {
    namespace {
        using physics::BlockShape;
        using physics::Box;

        bool Ends(const std::string& s, const char* suffix) {
            const size_t n = std::strlen(suffix);
            return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
        }

        bool Has(const std::string& s, const char* part) {
            return s.find(part) != std::string::npos;
        }

        void Add(BlockShape& s, double x0, double y0, double z0, double x1, double y1, double z1) {
            s.boxes.push_back({x0, y0, z0, x1, y1, z1});
        }

        int State(uint16_t id, const char* key, int fallback) {
            const BlockPalette& palette = BlockPalette::Instance();
            const std::string prefixed = std::string("minecraft:") + key;
            if (palette.HasState(id, key))
                return palette.StateInt(id, key, fallback);
            return palette.StateInt(id, prefixed, fallback);
        }

        std::string StateText(uint16_t id, const char* key, const char* fallback) {
            const BlockPalette& palette = BlockPalette::Instance();
            if (palette.HasState(id, key))
                return palette.StateString(id, key, fallback);
            return palette.StateString(id, std::string("minecraft:") + key, fallback);
        }

        bool HasAnyState(uint16_t id, const char* key) {
            const BlockPalette& palette = BlockPalette::Instance();
            return palette.HasState(id, key) || palette.HasState(id, std::string("minecraft:") + key);
        }

        struct PhysRecord {
            uint8_t flags = 0, surface = 0;
            float friction = 0.6f;
            std::vector<Box> boxes;
        };
        const std::unordered_map<int32_t, PhysRecord>& Registry() {
            static const std::unordered_map<int32_t, PhysRecord> table = [] {
                std::unordered_map<int32_t, PhysRecord> t;
                for (int once = 0; once < 1; once++) {
                    std::string raw;
                    if (!resources::Read("block_physics.bin", raw))
                        continue;
                    std::vector<uint8_t> d(raw.begin(), raw.end());
                    if (d.size() < 8 || std::memcmp(d.data(), "PHY1", 4) != 0)
                        continue;
                    uint32_t count;
                    std::memcpy(&count, d.data() + 4, 4);
                    size_t p = 8;
                    auto have = [&](size_t k) { return p + k <= d.size(); };
                    for (uint32_t i = 0; i < count && have(22); i++) {
                        int32_t hash;
                        PhysRecord r;
                        float hs, vs, fh;
                        std::memcpy(&hash, &d[p], 4);
                        r.flags = d[p + 4];
                        r.surface = d[p + 5];
                        std::memcpy(&r.friction, &d[p + 6], 4);
                        std::memcpy(&hs, &d[p + 10], 4);
                        std::memcpy(&vs, &d[p + 14], 4);
                        std::memcpy(&fh, &d[p + 18], 4);
                        p += 22;
                        if (!have(1))
                            break;
                        const int nb = d[p++];
                        if (!have(static_cast<size_t>(nb) * 24))
                            break;
                        for (int b = 0; b < nb; b++, p += 24) {
                            float v[6];
                            std::memcpy(v, &d[p], 24);
                            r.boxes.push_back({v[0], v[1], v[2], v[3], v[4], v[5]});
                        }
                        t.emplace(hash, std::move(r));
                    }
                    std::printf("[physics] block registry: %zu records from %s\n", t.size(), "embedded block_physics.bin");
                    break;
                }
                if (t.empty())
                    std::printf("[physics] block_physics.bin not found; using heuristic collision shapes\n");
                return t;
            }();
            return table;
        }

        bool Passable(const std::string& n) {
            static const char* const exact[] = {
                "short_grass", "tallgrass", "tall_grass", "fern", "large_fern", "double_plant", "yellow_flower",
                "red_flower", "dandelion", "poppy", "blue_orchid", "allium", "azure_bluet", "red_tulip",
                "orange_tulip", "white_tulip", "pink_tulip", "oxeye_daisy", "cornflower", "lily_of_the_valley",
                "wither_rose", "torchflower", "pitcher_plant", "pitcher_crop", "sunflower", "lilac", "rose_bush",
                "peony", "brown_mushroom", "red_mushroom", "crimson_fungus", "warped_fungus", "crimson_roots",
                "warped_roots", "nether_sprouts", "hanging_roots", "glow_lichen", "vine", "weeping_vines",
                "twisting_vines", "cave_vines", "cave_vines_body_with_berries", "cave_vines_head_with_berries",
                "reeds", "sugar_cane", "bamboo_sapling", "wheat", "carrots", "potatoes", "beetroot", "melon_stem",
                "pumpkin_stem", "sweet_berry_bush", "nether_wart", "lever", "tripwire", "tripwire_hook", "string",
                "redstone_wire", "fire", "soul_fire", "dead_bush", "deadbush", "light_block", "structure_void",
                "portal", "end_portal", "end_gateway", "item_frame", "glow_frame", "sculk_vein", "powder_snow",
                "sea_pickle", "pink_petals", "wildflowers", "leaf_litter", "short_dry_grass", "tall_dry_grass",
                "firefly_bush", "bush", "cactus_flower", "end_rod", "chain", "lantern", "soul_lantern", "bell",
                "candle", "turtle_egg", "pointed_dripstone", "flower_pot", "mangrove_propagule", "spore_blossom",
                "big_dripleaf_stem", "small_dripleaf_block", "sniffer_egg", "frog_spawn", "cocoa", "hay_block_none",
                "rail", "golden_rail", "detector_rail", "activator_rail", "torch", "redstone_torch", "soul_torch",
                "copper_torch", "unlit_redstone_torch", "colored_torch_rg", "colored_torch_bp", "underwater_torch",
                "coral", "brewing_stand", "snow_none", "heavy_core_none", "air", "cave_air", "void_air"};
            for (const char* e : exact)
                if (n == e)
                    return true;
            if (n.rfind("light_block", 0) == 0 || n == "piston_arm_collision" || n == "sticky_piston_arm_collision")
                return true;
            return Ends(n, "_sapling") || Ends(n, "sapling") || Ends(n, "_pressure_plate") || Ends(n, "_button") ||
                   n == "stone_button" || Ends(n, "_sign") || Ends(n, "_banner") || n == "standing_banner" ||
                   n == "wall_banner" || Ends(n, "_coral") || Ends(n, "_coral_fan") || Ends(n, "_coral_wall_fan") ||
                   Ends(n, "_skull") || Ends(n, "_head") || n == "skull" || Ends(n, "_torch") || Ends(n, "_candle") ||
                   Ends(n, "_roots") || Ends(n, "_wall_hanging_sign") || Ends(n, "hanging_sign") ||
                   Ends(n, "_fungus") || Ends(n, "_vines") || Ends(n, "_stem_none") || Ends(n, "_rail");
        }
    }

    namespace {
        struct SelectionTable {
            struct Shape {
                uint8_t pick = 1;
                std::vector<physics::Box> boxes;
            };
            std::vector<Shape> shapes;
            std::unordered_map<int32_t, uint16_t> byHash;
        };

        const SelectionTable& Selections() {
            static const SelectionTable table = [] {
                SelectionTable t;
                std::string raw;
                if (!resources::Read("block_selection.bin", raw) || raw.size() < 12 || std::memcmp(raw.data(), "SEL2", 4) != 0)
                    return t;
                const uint8_t* d = reinterpret_cast<const uint8_t*>(raw.data());
                uint32_t shapeCount, recordCount;
                std::memcpy(&shapeCount, d + 4, 4);
                std::memcpy(&recordCount, d + 8, 4);
                size_t p = 12;
                const auto have = [&](size_t n) { return p + n <= raw.size(); };
                t.shapes.reserve(shapeCount);
                for (uint32_t i = 0; i < shapeCount && have(2); i++) {
                    SelectionTable::Shape s;
                    s.pick = d[p++];
                    const int n = d[p++];
                    if (!have(static_cast<size_t>(n) * 24 + 1))
                        return t;
                    for (int b = 0; b < n; b++, p += 24) {
                        float v[6];
                        std::memcpy(v, d + p, 24);
                        s.boxes.push_back({v[0], v[1], v[2], v[3], v[4], v[5]});
                    }
                    if (d[p++])
                        p += 24;
                    t.shapes.push_back(std::move(s));
                }
                t.byHash.reserve(recordCount);
                for (uint32_t i = 0; i < recordCount && have(6); i++, p += 6) {
                    uint32_t hash;
                    uint16_t idx;
                    std::memcpy(&hash, d + p, 4);
                    std::memcpy(&idx, d + p + 4, 2);
                    t.byHash.emplace(static_cast<int32_t>(hash), idx);
                }
                return t;
            }();
            return table;
        }
    }

    physics::bds::ScaffoldingCell LevelCollision::ClimberCell(int x, int y, int z) const {
        using Cell = physics::bds::ScaffoldingCell;
        bool loaded = false;
        const auto id = level.GetBlock(x, y, z, loaded);
        if (!loaded || id == 0) return Cell::Air;
        const auto name = BlockPalette::Instance().Name(id);
        if (name == "minecraft:scaffolding" || name == "scaffolding") return Cell::Scaffold;
        if (name == "minecraft:powder_snow" || name == "powder_snow") return Cell::PowderSnow;
        if (name == "minecraft:air" || name == "air") return Cell::Air;
        if (name == "minecraft:water" || name == "water" ||
            name == "minecraft:flowing_water" || name == "flowing_water") return Cell::Water;
        return Cell::Other;
    }

    bool LevelCollision::Selection(int x, int y, int z, std::vector<physics::Box>& out) const {
        out.clear();
        bool loaded = false;
        const uint16_t id = level.GetBlock(x, y, z, loaded);
        if (!loaded || id == 0)
            return false;
        const Info& info = Lookup(id, blocks);
        if (info.selectionDefined) {
            out = info.selection;
            return !out.empty();
        }
        if (info.kind == Kind::Plain) {
            const SelectionTable& t = Selections();
            const auto it = t.byHash.find(BlockPalette::Instance().NetworkId(id));
            if (it != t.byHash.end()) {
                const SelectionTable::Shape& s = t.shapes[it->second];
                if (s.pick != 1 || s.boxes.empty())
                    return false;
                out = s.boxes;
                return true;
            }
        }
        const physics::BlockShape shape = QueryMain(x, y, z);
        if (shape.fluid != 0)
            return false;
        if (shape.Empty()) {
            out.push_back({0, 0, 0, 1, 1, 1});
            return true;
        }
        for (physics::Box box : shape.boxes) {
            if (info.kind == Kind::Fence || info.kind == Kind::Wall)
                box.y1 = std::min(box.y1, 1.0);
            out.push_back(box);
        }
        return true;
    }

    const physics::BlockShape& LevelCollision::ShapeOf(uint16_t id) {
        return Lookup(id, nullptr).shape;
    }

    const LevelCollision::Info& LevelCollision::Lookup(uint16_t id, const BlockLibrary* blocks) {
        static std::array<std::atomic<Info*>, 65536> table{};
        static std::mutex publishMutex;
        static std::vector<std::unique_ptr<Info>> snapshots;
        for (;;) {
            const auto definitions = ServerBlocks::Instance().CollisionRevision();
            const auto voxels = VoxelShapeRegistry::Instance().Revision();
            Info* slot = table[id].load(std::memory_order_acquire);
            if (slot && slot->definitionRevision == definitions && slot->voxelRevision == voxels)
                return *slot;
            Info info = Classify(id, blocks);
            if (id != 0 && BlockPalette::Instance().Name(id).empty()) {
                static thread_local Info transient;
                transient = std::move(info);
                return transient;
            }
            info.ready = true;
            info.definitionRevision = definitions;
            info.voxelRevision = voxels;
            std::lock_guard lock(publishMutex);
            if (ServerBlocks::Instance().CollisionRevision() != definitions ||
                VoxelShapeRegistry::Instance().Revision() != voxels)
                continue;
            slot = table[id].load(std::memory_order_acquire);
            if (slot && slot->definitionRevision == definitions && slot->voxelRevision == voxels)
                return *slot;
            snapshots.push_back(std::make_unique<Info>(std::move(info)));
            Info* fresh = snapshots.back().get();
            table[id].store(fresh, std::memory_order_release);
            return *fresh;
        }
    }

    LevelCollision::Info LevelCollision::Classify(uint16_t id, const BlockLibrary* blocks) {
        CustomBlockCollision custom;
        const bool hasCustom = id != 0 &&
            ServerBlocks::Instance().Collision(BlockPalette::Instance().Name(id), custom);
        const CollisionSet* chosen = hasCustom && custom.base.defined ? &custom.base : nullptr;
        if (hasCustom)
            for (const auto& [condition, set] : custom.permutations)
                if (EvaluateBlockCondition(condition, id))
                    chosen = &set;
        Info info = chosen ? Info{} : ClassifyVanilla(id, blocks);
        const CollisionSet* selection = hasCustom && custom.selection.defined ? &custom.selection : nullptr;
        if (hasCustom)
            for (const auto& [condition, set] : custom.selectionPermutations)
                if (EvaluateBlockCondition(condition, id)) selection = &set;
        if (selection) {
            info.selectionDefined = true;
            for (const auto& b : selection->boxes)
                info.selection.push_back({b[0], b[1], b[2], b[3], b[4], b[5]});
        }
        const std::string blockName = BlockPalette::Instance().Name(id);
        if (blockName == "minecraft:honey_block" || blockName == "honey_block") {
            info.shape.jumpProperties |= physics::bds::ReducedJumpSurfaceProperty;
            info.shape.honey = true;
        }
        bool replacedGeometry = chosen != nullptr;
        if (chosen)
            for (const CollisionBox& box : chosen->boxes)
                Add(info.shape, box[0], box[1], box[2], box[3], box[4], box[5]);
        if (hasCustom) {
            if (custom.friction >= 0.0f)
                info.shape.friction = custom.friction;
            info.shape.randomOffset = custom.randomOffset;
            for (const auto& [condition, offset] : custom.randomOffsetPermutations)
                if (EvaluateBlockCondition(condition, id))
                    info.shape.randomOffset = offset;
        }
        if (!chosen && id != 0 && info.shape.Empty() && info.shape.fluid == 0 &&
            !info.shape.climbable && !info.shape.web && !info.shape.powderSnow && !info.shape.berryBush && info.shape.scaffoldingStability < 0) {
            std::vector<Box> boxes;
            if (VoxelShapeRegistry::Instance().Find(BlockPalette::Instance().Name(id), boxes) && !boxes.empty()) {
                info.shape.boxes = std::move(boxes);
                replacedGeometry = true;
            }
        }
        if (replacedGeometry)
            info.full = info.shape.boxes.size() == 1 && info.shape.boxes[0].x0 <= 0 &&
                        info.shape.boxes[0].y0 <= 0 && info.shape.boxes[0].z0 <= 0 &&
                        info.shape.boxes[0].x1 >= 1 && info.shape.boxes[0].y1 >= 1 &&
                        info.shape.boxes[0].z1 >= 1;
        return info;
    }

    LevelCollision::Info LevelCollision::ClassifyVanilla(uint16_t id, const BlockLibrary* blocks) {
        Info info;
        BlockShape& s = info.shape;
        if (id == 0)
            return info;
        s.isAir = false;

        std::string n = BlockPalette::Instance().Name(id);
        if (n.empty()) {
            Add(s, 0, 0, 0, 1, 1, 1);
            info.full = true;
            return info;
        }
        if (n.rfind("minecraft:", 0) == 0)
            n.erase(0, 10);

        if (n == "portal" || n == "end_portal" || n == "end_gateway") {
            s.portal = true;
            return info;
        }
        if (n == "water" || n == "flowing_water" || n == "lava" || n == "flowing_lava") {
            const int depth = State(id, "liquid_depth", 0);
            s.fluid = Has(n, "lava") ? 2 : 1;
            s.fluidLevel = depth >= 8 ? 1.0f : static_cast<float>(9 - depth) / 9.0f;
            s.fluidDepth = static_cast<uint8_t>(depth);
            return info;
        }
        if (n == "kelp" || n == "seagrass" || n == "sea_grass" || n == "bubble_column" || n == "tall_seagrass" ||
            n == "kelp_plant") {
            s.fluid = 1;
            s.fluidLevel = 8.0f / 9.0f;
            if (n == "bubble_column")
                s.bubbleColumn = State(id, "drag_down", 0) ? -1 : 1;
            return info;
        }
        if (n == "web" || n == "cobweb") {
            s.web = true;
            return info;
        }
        if (n == "sweet_berry_bush") {
            s.berryBush = true;
            return info;
        }
        if (n == "powder_snow") {
            s.powderSnow = true;
            return info;
        }
        if (n == "scaffolding") {
            s.scaffoldingStability = static_cast<int8_t>(State(id, "stability", 0));
            return info;
        }
        if (n == "vine" || n == "scaffolding_none" || n == "twisting_vines" || n == "weeping_vines" ||
            n == "cave_vines" || n == "cave_vines_body_with_berries" || n == "cave_vines_head_with_berries") {
            s.climbable = true;
            return info;
        }
        {
            const auto& reg = Registry();
            const auto it = reg.find(BlockPalette::Instance().NetworkId(id));
            if (it != reg.end()) {
                const PhysRecord& r = it->second;
                for (const Box& b : r.boxes)
                    Add(s, b.x0, b.y0, b.z0, b.x1, b.y1, b.z1);
                s.friction = r.friction;
                s.surface = r.surface;
                s.climbable = (r.flags & 1) != 0;
                s.web = (r.flags & 8) != 0;
                info.full = r.boxes.size() == 1 && r.boxes[0].x0 <= 0 && r.boxes[0].y0 <= 0 && r.boxes[0].z0 <= 0 &&
                            r.boxes[0].x1 >= 1 && r.boxes[0].y1 >= 1 && r.boxes[0].z1 >= 1;
                return info;
            }
        }
        if (Passable(n))
            return info;

        if (Ends(n, "_slab") && !Has(n, "double")) {
            const bool top = StateText(id, "vertical_half", "bottom") == "top" || State(id, "top_slot_bit", 0) != 0;
            top ? Add(s, 0, 0.5, 0, 1, 1, 1) : Add(s, 0, 0, 0, 1, 0.5, 1);
            return info;
        }
        if (Ends(n, "_stairs")) {
            const int dir = State(id, "weirdo_direction", 0);
            const bool flip = State(id, "upside_down_bit", 0) != 0;
            const double base0 = flip ? 0.5 : 0.0, base1 = flip ? 1.0 : 0.5;
            const double step0 = flip ? 0.0 : 0.5, step1 = flip ? 0.5 : 1.0;
            Add(s, 0, base0, 0, 1, base1, 1);
            switch (dir & 3) {
            case 0: Add(s, 0.5, step0, 0, 1, step1, 1); break;
            case 1: Add(s, 0, step0, 0, 0.5, step1, 1); break;
            case 2: Add(s, 0, step0, 0.5, 1, step1, 1); break;
            default: Add(s, 0, step0, 0, 1, step1, 0.5); break;
            }
            return info;
        }
        if (Ends(n, "_trapdoor")) {
            if (State(id, "open_bit", 0)) {
                switch (State(id, "direction", 0) & 3) {
                case 0: Add(s, 0, 0, 0, 0.1875, 1, 1); break;
                case 1: Add(s, 0.8125, 0, 0, 1, 1, 1); break;
                case 2: Add(s, 0, 0, 0, 1, 1, 0.1875); break;
                default: Add(s, 0, 0, 0.8125, 1, 1, 1); break;
                }
            } else if (State(id, "upside_down_bit", 0)) {
                Add(s, 0, 0.8125, 0, 1, 1, 1);
            } else {
                Add(s, 0, 0, 0, 1, 0.1875, 1);
            }
            return info;
        }
        if (Ends(n, "_door")) {
            if (State(id, "open_bit", 0))
                return info;
            const std::string card = StateText(id, "cardinal_direction", "");
            int dir = State(id, "direction", 0);
            if (card == "east") dir = 0; else if (card == "south") dir = 1; else if (card == "west") dir = 2; else if (card == "north") dir = 3;
            switch (dir & 3) {
            case 0: Add(s, 0, 0, 0, 0.1875, 1, 1); break;
            case 1: Add(s, 0, 0, 0, 1, 1, 0.1875); break;
            case 2: Add(s, 0.8125, 0, 0, 1, 1, 1); break;
            default: Add(s, 0, 0, 0.8125, 1, 1, 1); break;
            }
            return info;
        }
        if (Ends(n, "_fence_gate") || n == "fence_gate") {
            if (State(id, "open_bit", 0))
                return info;
            const int dir = State(id, "direction", 0);
            (dir & 1) ? Add(s, 0.375, 0, 0, 0.625, 1.5, 1) : Add(s, 0, 0, 0.375, 1, 1.5, 0.625);
            return info;
        }
        if (Ends(n, "_fence") || n == "fence" || n == "nether_brick_fence") {
            info.kind = Kind::Fence;
            return info;
        }
        if (Ends(n, "_wall") && !Has(n, "_wall_")) {
            info.kind = Kind::Wall;
            return info;
        }
        if (Ends(n, "glass_pane") || n == "iron_bars" || n == "glass_pane" || n == "stained_glass_pane" ||
            n == "copper_bars" || Ends(n, "_bars")) {
            info.kind = Kind::Pane;
            return info;
        }
        if (Ends(n, "_carpet") || n == "carpet" || n == "moss_carpet" || n == "pale_moss_carpet") {
            Add(s, 0, 0, 0, 1, 0.0625, 1);
            return info;
        }
        if (n == "snow_layer") {
            const int h = State(id, "height", 0);
            if (h >= 7)
                Add(s, 0, 0, 0, 1, 1, 1);
            else if (h > 0)
                Add(s, 0, 0, 0, 1, h * 0.125, 1);
            return info;
        }
        if (n == "farmland" || n == "dirt_path" || n == "grass_path") {
            Add(s, 0, 0, 0, 1, 0.9375, 1);
            return info;
        }
        if (n == "soul_sand") {
            Add(s, 0, 0, 0, 1, 0.875, 1);
            s.speedFactor = 0.4f;
            return info;
        }
        if (n == "mud" || n == "packed_mud_none") {
            Add(s, 0, 0, 0, 1, 0.875, 1);
            return info;
        }
        if (n == "honey_block") {
            Add(s, 0.0625, 0, 0.0625, 0.9375, 0.9375, 0.9375);
            return info;
        }
        if (n == "chest" || n == "trapped_chest" || n == "ender_chest") {
            Add(s, 0.0625, 0, 0.0625, 0.9375, 0.875, 0.9375);
            return info;
        }
        if (n == "enchanting_table") { Add(s, 0, 0, 0, 1, 0.75, 1); return info; }
        if (n == "daylight_detector" || n == "daylight_detector_inverted") { Add(s, 0, 0, 0, 1, 0.375, 1); return info; }
        if (Ends(n, "_bed") || n == "bed") { Add(s, 0, 0, 0, 1, 0.5625, 1); return info; }
        if (n == "cake") { Add(s, 0.0625, 0, 0.0625, 0.9375, 0.5, 0.9375); return info; }
        if (n == "lily_pad" || n == "waterlily") { Add(s, 0.0625, 0, 0.0625, 0.9375, 0.09375, 0.9375); return info; }
        if (n == "end_portal_frame") { Add(s, 0, 0, 0, 1, 0.8125, 1); return info; }
        if (n == "cactus") { Add(s, 0.0625, 0, 0.0625, 0.9375, 0.9375, 0.9375); return info; }
        if (n == "campfire" || n == "soul_campfire") { Add(s, 0, 0, 0, 1, 0.4375, 1); return info; }
        if (n == "stonecutter_block" || n == "stonecutter") { Add(s, 0, 0, 0, 1, 0.5625, 1); return info; }
        if (n == "repeater" || n == "unpowered_repeater" || n == "powered_repeater" || n == "comparator" ||
            n == "unpowered_comparator" || n == "powered_comparator") {
            Add(s, 0, 0, 0, 1, 0.125, 1);
            return info;
        }

        if (blocks) {
            const BlockAppearance& look = blocks->Look(static_cast<int32_t>(id));
            if (look.shape == BlockAppearance::Shape::Cross || look.shape == BlockAppearance::Shape::Wall)
                return info;
        }

        Add(s, 0, 0, 0, 1, 1, 1);
        info.full = true;
        if (n == "ice" || n == "packed_ice" || n == "frosted_ice")
            s.friction = 0.98f;
        else if (n == "blue_ice")
            s.friction = 0.989f;
        else if (n == "slime" || n == "slime_block")
            s.friction = 0.8f;
        return info;
    }

    namespace {
        std::mutex gEntityBoxMutex;
        std::shared_ptr<const std::vector<physics::Box>> gEntityBoxes = std::make_shared<const std::vector<physics::Box>>();
    }

    void SetEntityCollisionBoxes(std::vector<physics::Box> boxes) {
        auto fresh = std::make_shared<const std::vector<physics::Box>>(std::move(boxes));
        std::lock_guard lock(gEntityBoxMutex);
        gEntityBoxes = std::move(fresh);
    }

    LevelCollision::LevelCollision(const Level& l, const BlockLibrary* b) : level(l), blocks(b) {
        std::lock_guard lock(gEntityBoxMutex);
        entityBoxes = gEntityBoxes;
    }

    bool LevelCollision::CollisionChunkReady(int x, int z) const {
        return level.HasChunk(x, z);
    }

    physics::bds::TerrainHeightLimits LevelCollision::CollisionHeightLimits() const {
        return {Chunk::MIN_Y, Chunk::MIN_Y + Chunk::HEIGHT};
    }

    physics::BlockShape LevelCollision::Query(int x, int y, int z) const {
        BlockShape s = QueryMain(x, y, z);
        if (entityBoxes && !entityBoxes->empty()) {
            for (const physics::Box& b : *entityBoxes) {
                if (b.x1 <= x || b.x0 >= x + 1 || b.y1 <= y || b.y0 >= y + 1 || b.z1 <= z || b.z0 >= z + 1)
                    continue;
                s.boxes.push_back({std::max<double>(b.x0, x) - x, std::max<double>(b.y0, y) - y, std::max<double>(b.z0, z) - z,
                                   std::min<double>(b.x1, x + 1) - x, std::min<double>(b.y1, y + 1) - y, std::min<double>(b.z1, z + 1) - z});
            }
        }
        if (s.fluid == 0 && y >= Chunk::MIN_Y && y < Chunk::MIN_Y + Chunk::HEIGHT) {
            const uint16_t liquid = level.GetLiquid(x, y, z);
            if (liquid != 0) {
                const Info& li = Lookup(liquid, blocks);
                if (li.shape.fluid != 0) {
                    s.fluid = li.shape.fluid;
                    s.fluidLevel = li.shape.fluidLevel;
                    s.fluidDepth = li.shape.fluidDepth;
                }
            }
        }
        return s;
    }

    physics::BlockShape LevelCollision::QueryMain(int x, int y, int z) const {
        if (y < Chunk::MIN_Y)
            return {};
        if (y >= Chunk::MIN_Y + Chunk::HEIGHT)
            return {};
        bool loaded = false;
        const uint16_t id = level.GetBlock(x, y, z, loaded);
        if (!loaded) {
            BlockShape solid;
            Add(solid, 0, 0, 0, 1, 1, 1);
            return solid;
        }
        const Info& info = Lookup(id, blocks);
        if (info.kind == Kind::Plain)
            return info.shape;

        auto joins = [&](int dx, int dz) {
            bool nl = false;
            const uint16_t other = level.GetBlock(x + dx, y, z + dz, nl);
            if (!nl || other == 0)
                return false;
            const Info& n = Lookup(other, blocks);
            if (n.kind == info.kind || n.full)
                return true;
            if (info.kind == Kind::Fence) {
                std::string name = BlockPalette::Instance().Name(other);
                return Ends(name, "_fence_gate") || name == "minecraft:fence_gate";
            }
            return false;
        };

        BlockShape s;
        s.friction = info.shape.friction;
        s.randomOffset = info.shape.randomOffset;
        double lo, hi, height;
        double armLo, armHi;
        switch (info.kind) {
        case Kind::Fence: lo = 0.375; hi = 0.625; height = 1.5; armLo = 0.375; armHi = 0.625; break;
        case Kind::Wall: lo = 0.25; hi = 0.75; height = 1.5; armLo = 0.3125; armHi = 0.6875; break;
        default: lo = 0.4375; hi = 0.5625; height = 1.0; armLo = 0.4375; armHi = 0.5625; break;
        }
        const bool north = joins(0, -1), south = joins(0, 1), west = joins(-1, 0), east = joins(1, 0);
        const bool any = north || south || west || east;
        (void)any;
        Add(s, lo, 0, lo, hi, height, hi);
        if (north) Add(s, armLo, 0, 0, armHi, height, hi);
        if (south) Add(s, armLo, 0, lo, armHi, height, 1);
        if (west) Add(s, 0, 0, armLo, hi, height, armHi);
        if (east) Add(s, lo, 0, armLo, 1, height, armHi);
        return s;
    }
}
