#include "client/ui/ChestScreen.hpp"

#include "client/resources/TexturePack.hpp"
#include "client/ui/Hud.hpp"
#include "client/ui/HudData.hpp"
#include "client/ui/HudRenderers.hpp"
#include "client/ui/ItemTooltip.hpp"
#include "client/ui/McFont.hpp"
#include "client/ui/TenFont.hpp"
#include "client/ui/UiAssets.hpp"
#include "client/ui/UiTemplate.hpp"
#include "util/Paths.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace perlin::ui {
    namespace {
        // every container screen the game's ui files define, built ahead of time
        const char* const kScreens[] = {"chest.small_chest_screen", "chest.large_chest_screen", "chest.ender_chest_screen", "chest.barrel_screen",
                                        "chest.shulker_box_screen", "redstone.hopper_screen", "redstone.dispenser_screen", "redstone.dropper_screen",
                                        "furnace.furnace_screen", "blast_furnace.blast_furnace_screen", "smoker.smoker_screen",
                                        "brewing_stand.brewing_stand_screen", "anvil.anvil_screen", "grindstone.grindstone_screen",
                                        "smithing_table.smithing_table_screen", "enchanting.enchanting_screen", "stonecutter.stonecutter_screen",
                                        "loom.loom_screen", "beacon.beacon_screen"};

        enum Kind { None, Chest, Hopper, Dispenser, Dropper, Furnace, BlastFurnace, Smoker, Brewing, Anvil, Grindstone, Smithing, Enchanting,
                    Stonecutter, Loom, Beacon };

        // the container's item collections and the chest cells each one shows
        std::vector<int> Cells(const InventoryView& v, const std::string& collection) {
            const auto range = [](int from, int n) {
                std::vector<int> out;
                for (int i = 0; i < n; i++)
                    out.push_back(from + i);
                return out;
            };
            switch (v.container) {
            case Hopper:
            case Dispenser:
            case Dropper:
                if (collection == "container_items")
                    return range(0, v.container == Hopper ? 5 : 9);
                break;
            case Furnace:
            case BlastFurnace:
            case Smoker:
                if (collection == "furnace_ingredient_items") return {0};
                if (collection == "furnace_fuel_items") return {1};
                if (collection == "furnace_output_items") return {2};
                break;
            case Brewing:
                if (collection == "brewing_input_item") return {0};
                if (collection == "brewing_result_items") return {1, 2, 3};
                if (collection == "brewing_fuel_item") return {4};
                break;
            case Anvil:
                if (collection == "anvil_input_items") return {0};
                if (collection == "anvil_material_items") return {1};
                if (collection == "anvil_result_items") return {2};
                break;
            case Grindstone:
                if (collection == "grindstone_input_items") return {0};
                if (collection == "grindstone_additional_items") return {1};
                if (collection == "grindstone_result_items") return {2};
                break;
            case Smithing:
                if (collection == "smithing_table_template_items") return {0};
                if (collection == "smithing_table_input_items") return {1};
                if (collection == "smithing_table_material_items") return {2};
                if (collection == "smithing_table_result_items") return {3};
                break;
            case Enchanting:
                if (collection == "enchanting_input_items") return {0};
                if (collection == "enchanting_lapis_items") return {1};
                break;
            case Stonecutter:
                if (collection == "stonecutter_input_items") return {0};
                if (collection == "stonecutter_result_items") return {1};
                break;
            case Loom:
                if (collection == "loom_input_items") return {0};
                if (collection == "loom_dye_items") return {1};
                if (collection == "loom_material_items") return {2};
                if (collection == "loom_result_items") return {3};
                break;
            case Beacon:
                if (collection == "beacon_payment_items") return {0};
                break;
            default:
                if (collection == "container_items")
                    return range(0, std::min(v.chestRows * 9, 54));
                break;
            }
            return {};
        }

        const char* const kBeaconButtons[] = {"speed", "haste", "resist", "jump", "strength", "regen", "extra", "confirm"};
        // effect ids and the pyramid tier each beacon button needs
        constexpr int kBeaconEffects[] = {1, 3, 11, 8, 5, 10};
        constexpr int kBeaconTiers[] = {1, 1, 2, 2, 3, 4, 4, 0};

        int BeaconButton(const std::string& name) {
            for (int i = 0; i < 8; i++)
                if (name == kBeaconButtons[i])
                    return i;
            return -1;
        }

        const HudItem* SlotItem(const InventoryView& v, const std::string& collection, int index) {
            if (index < 0)
                return nullptr;
            if (collection == "inventory_items")
                return index < 27 ? &v.slots[static_cast<size_t>(9 + index)] : nullptr;
            if (collection == "hotbar_items")
                return index < 9 ? &v.slots[static_cast<size_t>(index)] : nullptr;
            if (collection == "stones")
                return index < static_cast<int>(v.stones.size()) ? &v.stones[static_cast<size_t>(index)] : nullptr;
            const std::vector<int> cells = Cells(v, collection);
            return index < static_cast<int>(cells.size()) ? &v.chestItems[static_cast<size_t>(cells[static_cast<size_t>(index)])] : nullptr;
        }

        std::string SlotName(const InventoryView& v, const std::string& collection, int index) {
            if (index < 0)
                return {};
            if (collection == "inventory_items")
                return "s" + std::to_string(9 + index);
            if (collection == "hotbar_items")
                return "s" + std::to_string(index);
            const std::vector<int> cells = Cells(v, collection);
            return index < static_cast<int>(cells.size()) ? "k" + std::to_string(cells[static_cast<size_t>(index)]) : std::string();
        }

        class ChestData final : public UiData {
        public:
            explicit ChestData(const InventoryView& view) : v(view) {}

            bool Global(const std::string& name, Json& out) const override {
                const auto set = [&](Json value) { out = std::move(value); return true; };
                if (name == "#container_title")
                    return set(Json(v.title));
                const int total = v.container == Furnace ? 200 : 100;
                if (name == "#furnace_arrow_ratio")
                    return set(Json(1.0 - std::clamp(static_cast<double>(v.props[0]) / total, 0.0, 1.0)));
                if (name == "#furnace_flame_ratio")
                    return set(Json(v.props[1] > 0 ? 1.0 - std::clamp(static_cast<double>(v.props[1]) / std::max(1, v.props[2]), 0.0, 1.0) : 1.0));
                // brewing: brew time counts down from 400, fuel from its total
                if (name == "#brewing_arrow_ratio")
                    return set(Json(v.props[0] > 0 ? std::clamp(v.props[0] / 400.0, 0.0, 1.0) : 1.0));
                if (name == "#brewing_fuel_ratio")
                    return set(Json(1.0 - std::clamp(static_cast<double>(v.props[1]) / std::max(1, v.props[2] > 0 ? v.props[2] : 20), 0.0, 1.0)));
                if (name == "#brewing_bubbles_ratio")
                    return set(Json(v.props[0] > 0 ? (v.props[0] % 20) / 20.0 : 1.0));
                if (name == "#text_box_item_name" || name == "#item_name")
                    return set(Json(v.anvilName));
                if (name == "#cost_text") {
                    if (v.anvilCost <= 0)
                        return set(Json(std::string()));
                    std::string text = UiAssets::Instance().Localize("container.repair.cost");
                    if (text.empty() || text == "container.repair.cost")
                        text = "Enchantment Cost: %1";
                    // the lang string is "Enchantment Cost: %1$d" in the game's files
                    for (const char* token : {"%1$d", "%1$s", "%1", "%d", "%s"})
                        if (const size_t at = text.find(token); at != std::string::npos) {
                            text.replace(at, std::char_traits<char>::length(token), std::to_string(v.anvilCost));
                            break;
                        }
                    return set(Json(text));
                }
                if (name == "#cost_text_red")
                    return set(Json(v.anvilCost > 0 && !v.creative && (v.anvilCost > v.level || v.anvilCost >= 40)));
                if (name == "#cost_text_green")
                    return set(Json(v.anvilCost > 0 && (v.creative || (v.anvilCost <= v.level && v.anvilCost < 40))));
                if (name == "#cross_out_icon") {
                    const int input = v.container == Smithing ? 1 : 0;
                    return set(Json(v.resultCell >= 0 && !v.chestItems[static_cast<size_t>(input)].Empty() &&
                                    v.chestItems[static_cast<size_t>(v.resultCell)].Empty()));
                }
                if (name == "#stone_selector_total_items")
                    return set(Json(static_cast<double>(v.stones.size())));
                if (name == "#pattern_selector_total_items")
                    return set(Json(static_cast<double>(v.patterns.size())));
                if (name == "#supports_netherite")
                    return set(Json(true));
                return false;
            }

            int CollectionSize(const std::string& collection) const override {
                if (collection == "inventory_items")
                    return 27;
                if (collection == "hotbar_items")
                    return 9;
                if (collection == "stones")
                    return static_cast<int>(v.stones.size());
                if (collection == "patterns")
                    return static_cast<int>(v.patterns.size());
                if (collection == "#enchant_buttons")
                    return v.container == Enchanting ? 3 : 0;
                if (v.container == Beacon && BeaconButton(collection) >= 0)
                    return 1;
                return static_cast<int>(Cells(v, collection).size());
            }

            bool CollectionValue(const std::string& collection, int index, const std::string& binding, Json& out) const override {
                const auto set = [&](Json value) { out = std::move(value); return true; };
                if (collection == "#enchant_buttons") {
                    const bool exists = index >= 0 && index < static_cast<int>(v.enchants.size());
                    const bool enabled = exists && (v.creative || (v.level >= v.enchants[static_cast<size_t>(index)].cost &&
                                                                   v.chestItems[1].count > index));
                    if (binding == "#selectable_button_visibility" || binding == "#selectable_dust_is_visible") return set(Json(enabled));
                    if (binding == "#unselectable_button_visibility" || binding == "#unselectable_dust_is_visible") return set(Json(exists && !enabled));
                    if (binding == "#cost") return set(Json(exists ? std::to_string(v.enchants[static_cast<size_t>(index)].cost) : std::string()));
                    if (binding == "#runes" || binding == "#hover_text")
                        return set(Json(exists ? v.enchants[static_cast<size_t>(index)].name : std::string()));
                    return false;
                }
                if (const int b = v.container == Beacon ? BeaconButton(collection) : -1; b >= 0) {
                    const bool selected = b < 5 ? v.beaconPrimary == kBeaconEffects[b] : b < 7 && v.beaconSecondary == (b == 5 ? 10 : v.beaconPrimary);
                    const bool enabled = v.beaconLevel >= kBeaconTiers[b] && (b != 7 || (v.beaconPrimary != 0 && !v.chestItems[0].Empty()));
                    if (binding == "#button_visible") return set(Json(true));
                    if (binding == "#active") return set(Json(enabled && !selected));
                    if (binding == "#inactive") return set(Json(!enabled));
                    if (binding == "#selected") return set(Json(enabled && selected));
                    return false;
                }
                if (collection == "patterns") {
                    if (index < 0 || index >= static_cast<int>(v.patterns.size()))
                        return false;
                    if (binding == "#banner_patterns") return set(Json(v.patterns[static_cast<size_t>(index)]));
                    if (binding == "#pattern_selector_total_items") return set(Json(static_cast<double>(v.patterns.size())));
                    if (binding == "#pattern_cell_background_texture")
                        return set(Json(std::string(index == v.stationPick ? "textures/ui/cell_image_invert" : "textures/ui/cell_image_normal")));
                    return false;
                }
                if (collection == "stones") {
                    if (binding == "#stone_selector_total_items") return set(Json(static_cast<double>(v.stones.size())));
                    if (binding == "#stone_cell_background_texture")
                        return set(Json(std::string(index == v.stationPick ? "textures/ui/cell_image_invert" : "textures/ui/cell_image_normal")));
                }
                const HudItem* item = SlotItem(v, collection, index);
                if (!item)
                    return false;
                const bool empty = item->Empty();
                if (binding == "#item_id_aux" || binding == "#item_renderer_data") return set(Json(empty ? 0.0 : 1.0));
                if (binding == "#inventory_stack_count" || binding == "#item_stack_count")
                    return set(Json(!empty && item->count > 1 ? std::to_string(item->count) : std::string()));
                if (binding == "#stack_count_visible") return set(Json(!empty && item->count > 1));
                if (binding == "#item_durability_visible") return set(Json(!empty && item->maxDamage > 0 && item->aux > 0));
                if (binding == "#item_durability_total_amount") return set(Json(static_cast<double>(item->maxDamage)));
                if (binding == "#item_durability_current_amount") return set(Json(static_cast<double>(std::max(0, item->maxDamage - item->aux))));
                if (binding == "#empty_bottle_image_visible" || binding == "#empty_fuel_image_visible" || binding == "#empty_image_visible")
                    return set(Json(empty));
                if (binding == "#item_storage_visible" || binding == "#inactive_item" || binding == "#slot_selected" ||
                    binding == "#is_item_locked" || binding == "#item_lock_cell_image_visible")
                    return set(Json(false));
                if (binding == "#item_storage_total_amount" || binding == "#item_storage_current_amount") return set(Json(0.0));
                // 0 picks the plain cell image (the furnace's cells choose theirs from this)
                if (binding == "#container_item_background") return set(Json(0.0));
                return false;
            }

        private:
            const InventoryView& v;
        };

        const LNode* FindNode(const LNode& n, const std::string& id) {
            if (n.id == id)
                return &n;
            for (const auto& kid : n.kids)
                if (const LNode* found = FindNode(*kid, id))
                    return found;
            return nullptr;
        }

        const char* kChestTemplate = R"PERLINRML(<rml>
<head>
<title>chest</title>
<style>
body { position: absolute; left: 0px; top: 0px; width: {{w}}px; height: {{h}}px; margin: 0; padding: 0; background: transparent; }
div { display: block; position: absolute; }
.cnt { font-family: minecraft-shadow; font-size: 8px; line-height: 9px; color: #ffffff; text-align: right; white-space: nowrap; z-index: 6; }
#cursor { z-index: 30; pointer-events: none; width: 16px; height: 16px; }
#cursor .cnt { z-index: 31; }
{{tipstyle}}{{rcss}}
</style>
</head>
<body>
{{hud}}
<div id="top" style="left: 0px; top: 0px; width: {{w}}px; height: {{h}}px; z-index: 1000; pointer-events: none;"><mclayer layer="top" style="display: block; position: absolute; width: 0px; height: 0px; z-index: -999;"></mclayer>{{cursor}}{{tooltip}}<mclayer layer="gui" style="display: block; position: absolute; width: 0px; height: 0px; z-index: 999;"></mclayer></div>
</body>
</rml>
)PERLINRML";
        const ScreenTemplateDefault kChestDefault("chest_screen.rml", kChestTemplate);
    }

    std::string ChestScreenName(const std::string& titleKey, int rows) {
        if (titleKey == "container.enderchest")
            return "chest.ender_chest_screen";
        if (titleKey == "container.barrel")
            return "chest.barrel_screen";
        if (titleKey == "container.shulkerBox")
            return "chest.shulker_box_screen";
        return rows > 3 ? "chest.large_chest_screen" : "chest.small_chest_screen";
    }

    std::string ContainerScreenName(int container) {
        static const char* const kNames[] = {"", "", "redstone.hopper_screen", "redstone.dispenser_screen", "redstone.dropper_screen",
                                             "furnace.furnace_screen", "blast_furnace.blast_furnace_screen", "smoker.smoker_screen",
                                             "brewing_stand.brewing_stand_screen", "anvil.anvil_screen", "grindstone.grindstone_screen",
                                             "smithing_table.smithing_table_screen", "enchanting.enchanting_screen", "stonecutter.stonecutter_screen",
                                             "loom.loom_screen", "beacon.beacon_screen"};
        return container >= 0 && container < static_cast<int>(std::size(kNames)) ? kNames[container] : "";
    }

    ChestUi::ChestUi() {
        services.textWidth = [](const std::string& text, float s, bool ten) {
            return ten ? TenFont::Instance().Width(text, s) : McFont::Instance().Width(text, s);
        };
        services.lineHeight = [](float s, bool ten) {
            return ten ? TenFont::Instance().LineHeight(s) : static_cast<float>(McFont::kLineHeight) * s;
        };
        services.textureSize = [](const std::string& tex, int& w, int& h) { return UiAssets::Instance().ImageSize(tex, w, h); };
        services.localize = [](const std::string& k) { return UiAssets::Instance().Localize(k); };
        emitter.SetTemplates(kChestTemplate, std::string());
        RegisterHudRenderers(emitter, hudState, services);
        emitter.Register("inventory_item_renderer", [this](const LNode& n, RmlWriter& w) {
            const HudItem* item = current ? SlotItem(*current, n.collection, n.index) : nullptr;
            if (item && !item->Empty())
                w.Item(n.ax, n.ay, n.w, n.h, n.layer, item->Key(), n.alpha, item->enchanted);
        });
        // banner patterns and the enchanting book have no renderer of their own here: their items stand in
        emitter.Register("banner_pattern_renderer", [](const LNode& n, RmlWriter& w) {
            w.Item(n.ax, n.ay, n.w, n.h, n.layer, "minecraft:banner", n.alpha, false);
        });
        emitter.Register("enchanting_book_renderer", [](const LNode& n, RmlWriter& w) {
            w.Item(n.ax, n.ay, n.w, n.h, n.layer, "minecraft:enchanted_book", n.alpha, false);
        });
        emitter.SetEditText([this](const LNode&, std::string& text, std::string&, bool& focused) {
            if (!current || current->container != Anvil)
                return false;
            text = current->anvilName;
            focused = current->anvilFocused;
            return true;
        });
        emitter.SetSlotNamer([this](const LNode& n) {
            if (!current)
                return std::string();
            if (n.type == "input_panel" && n.collection != "stones")
                return SlotName(*current, n.collection, n.index);
            // station buttons: "b:" + collection + ":" + index, routed by the inventory screen
            if (n.type == "button" && !n.collection.empty() && n.index >= 0 &&
                (n.collection == "stones" || n.collection == "patterns" || n.collection == "#enchant_buttons" || BeaconButton(n.collection) >= 0))
                return "b:" + n.collection + ":" + std::to_string(n.index);
            if (n.type == "edit_box" && current->container == Anvil)
                return std::string("anvil_name");
            return std::string();
        });
    }

    ChestUi::~ChestUi() {
        if (worker.joinable())
            worker.join();
    }

    void ChestUi::Reset() {
        if (worker.joinable())
            worker.join();
        state = 0;
        key.clear();
        problem.clear();
        trees.clear();
        library = UiLibrary{};
        hudState = HudState{};
        current = nullptr;
        dumped.clear();
    }

    void ChestUi::Warm(const std::vector<std::string>& packDirs) {
        std::string wanted;
        for (const std::string& d : packDirs)
            wanted += d + "|";
        wanted += paths::DataDir() + "|" + UiAssets::Instance().VanillaRoot() + "|" + std::to_string(TexturePack::Instance().Generation());
        if (state.load() == 1)
            return;
        if (state.load() != 0 && wanted == key)
            return;
        if (worker.joinable())
            worker.join();
        key = wanted;
        state = 1;
        dumped.clear();
        std::vector<std::string> roots;
        for (size_t i = packDirs.size(); i-- > 0;)
            roots.push_back(packDirs[i] + "/ui");
        const std::string data = paths::DataDir();
        if (const std::string packUi = ExtractPackUi(data, "pack_ui_chest"); !packUi.empty())
            roots.push_back(packUi + "/ui");
        roots.push_back(data + "/ui");
        roots.push_back(data + "/ui_defaults");
        roots.push_back(UiAssets::Instance().VanillaRoot() + "/ui");
        worker = std::thread([this, roots]() {
            library.SetRoots(roots);
            trees.clear();
            problem.clear();
            if (!library.Scan()) {
                problem = "no ui folder";
                state = 3;
                return;
            }
            Vars vars = EngineRootVariables();
            vars["$container_title"] = Json(std::string("#container_title"));
            vars["$localize_title"] = Json(false);
            // the current smithing table, with its template slot
            vars["$use_smithing_table_2_ui"] = Json(true);
            for (const char* screen : kScreens) {
                BuildReport report;
                if (auto tree = BuildTree(library, screen, vars, report))
                    trees[screen] = std::move(tree);
                else
                    std::cout << "[chest] could not build " << screen << std::endl;
            }
            if (!trees.count("chest.small_chest_screen") || !trees.count("chest.large_chest_screen")) {
                problem = "chest.small_chest_screen / chest.large_chest_screen could not be built";
                state = 3;
                return;
            }
            state = 2;
        });
    }

    bool ChestUi::WaitReady(int timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (state.load() == 1 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return Ready();
    }

    bool ChestUi::Build(const InventoryView& view, const std::string& screen, int guiW, int guiH, std::string& rml, float (&panel)[4]) {
        if (state.load() != 2)
            return false;
        auto it = trees.find(screen);
        if (it == trees.end() && view.container <= Chest)
            it = trees.find(view.chestRows > 3 ? "chest.large_chest_screen" : "chest.small_chest_screen");
        if (it == trees.end() || !it->second)
            return false;
        ChestData data(view);
        current = &view;
        const auto root = EvaluateTree(*it->second, data, services, static_cast<float>(guiW), static_cast<float>(guiH));
        if (!root) {
            current = nullptr;
            return false;
        }
        emitter.SetTemplates(ScreenTemplate("chest_screen.rml", kChestTemplate), std::string());
        rml = emitter.Emit(*root, static_cast<float>(guiW), static_cast<float>(guiH), std::string(), false);
        current = nullptr;
        if ((view.container <= Chest && rml.find("id=\"k0\"") == std::string::npos) || rml.find("id=\"s0\"") == std::string::npos) {
            problem = "the ui files produced no container slots";
            return false;
        }
        ReplaceAll(rml, "cursor", InventoryCursorRml(view));
        ReplaceAll(rml, "tooltip", InventoryTooltipRml(view, guiW, guiH));
        ReplaceAll(rml, "tipstyle", TooltipStyles());

        const float fallbackH = static_cast<float>(InventoryPanelH(view));
        panel[0] = static_cast<float>(guiW - kInvPanelW) * 0.5f;
        panel[1] = (static_cast<float>(guiH) - fallbackH) * 0.5f;
        panel[2] = static_cast<float>(kInvPanelW);
        panel[3] = fallbackH;
        if (const LNode* p = FindNode(*root, "root_panel"); p && p->w > 0 && p->h > 0) {
            panel[0] = p->ax;
            panel[1] = p->ay;
            panel[2] = p->w;
            panel[3] = p->h;
        }

        if (dumped.insert(it->first).second) {
            const std::filesystem::path out = std::filesystem::path(paths::DataDir()) / "ui_generated" / (it->first.substr(it->first.find('.') + 1) + ".rml");
            std::error_code ec;
            std::filesystem::create_directories(out.parent_path(), ec);
            std::ofstream f(out, std::ios::binary | std::ios::trunc);
            f << rml;
            for (const std::string& u : emitter.UnknownRenderers())
                std::cout << "[chest] custom renderer not implemented: " << u << std::endl;
        }
        return true;
    }
}
