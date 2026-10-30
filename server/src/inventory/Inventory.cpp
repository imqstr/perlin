#include "inventory/Inventory.hpp"

#include "world/BlockCatalog.hpp"

#include <algorithm>
#include <map>

namespace perlin::server {
    namespace proto = proto2225;
    using Container = proto::ContainerSlotType;
    using Action = proto::ItemStackRequestActionsEntryTypeId;

    int MaxStack(int16_t networkId) {
        const CatalogItem* item = BlockCatalog::Instance().ItemById(networkId);
        if (!item)
            return 64;
        const std::string& n = item->name;
        const auto ends = [&](const char* suffix) {
            const std::string s = suffix;
            return n.size() >= s.size() && n.compare(n.size() - s.size(), s.size(), s) == 0;
        };
        if (ends("_shulker_box") || n == "minecraft:shulker_box" || ends("_bed"))
            return 1;
        if (ends("_sign") || ends("_banner"))
            return 16;
        return 64;
    }

    ItemStack* PlayerInventory::Resolve(Container type, int slot) {
        switch (type) {
        case Container::HotbarAndInventory:
            return slot >= 0 && slot < kMainSlots ? &main[static_cast<size_t>(slot)] : nullptr;
        case Container::Hotbar:
            return slot >= 0 && slot < kHotbar ? &main[static_cast<size_t>(slot)] : nullptr;
        // the inventory container counts from the first slot after the hotbar
        case Container::Inventory:
            return slot >= 0 && slot < kMainSlots - kHotbar ? &main[static_cast<size_t>(slot + kHotbar)] : nullptr;
        case Container::Armor:
            return slot >= 0 && slot < 4 ? &armor[static_cast<size_t>(slot)] : nullptr;
        case Container::Offhand:
            return &offhand;
        case Container::Cursor:
            return &cursor;
        case Container::CreativeOutput:
            return &output;
        default:
            return nullptr;
        }
    }

    ItemStack PlayerInventory::MakeStack(int16_t networkId, int count) {
        ItemStack s;
        s.networkId = networkId;
        s.count = static_cast<uint16_t>(count);
        s.stackId = NewStackId();
        return s;
    }

    int PlayerInventory::Give(int16_t networkId, int count) {
        const int limit = MaxStack(networkId);
        for (ItemStack& s : main) {
            if (count <= 0)
                break;
            if (!s.Empty() && s.networkId == networkId && s.count < limit) {
                const int add = std::min<int>(count, limit - s.count);
                s.count = static_cast<uint16_t>(s.count + add);
                s.stackId = NewStackId();
                count -= add;
            }
        }
        for (ItemStack& s : main) {
            if (count <= 0)
                break;
            if (s.Empty()) {
                const int add = std::min(count, limit);
                s = MakeStack(networkId, add);
                count -= add;
            }
        }
        return count;
    }

    bool PlayerInventory::TakeHeld(int count) {
        ItemStack& s = main[static_cast<size_t>(selected)];
        if (s.Empty() || s.count < count)
            return false;
        s.count = static_cast<uint16_t>(s.count - count);
        if (s.count == 0)
            s = ItemStack{};
        else
            s.stackId = NewStackId();
        return true;
    }

    proto::ItemV4 ToWire(const ItemStack& stack) {
        proto::ItemV4 out;
        if (stack.Empty())
            return out;
        out.networkId = stack.networkId;
        out.count = stack.count;
        out.hasStackId = true;
        out.stackId = stack.stackId;
        if (const CatalogItem* item = BlockCatalog::Instance().ItemById(stack.networkId))
            out.blockRuntimeId = static_cast<uint32_t>(item->blockHash);
        return out;
    }

    namespace {
        // a client names a stack by the id the server gave it; ids created inside a request are negative and unknown here
        bool StackIdMatches(const ItemStack& s, int32_t quoted) { return quoted < 0 || s.Empty() || quoted == 0 || quoted == s.stackId; }

        bool Move(ItemStack& src, ItemStack& dst, int count) {
            if (count <= 0 || src.Empty() || src.count < count)
                return false;
            if (!dst.Empty()) {
                if (!dst.Same(src) || dst.count + count > MaxStack(dst.networkId))
                    return false;
                dst.count = static_cast<uint16_t>(dst.count + count);
            } else {
                dst.networkId = src.networkId;
                dst.count = static_cast<uint16_t>(count);
            }
            src.count = static_cast<uint16_t>(src.count - count);
            if (src.count == 0)
                src = ItemStack{};
            return true;
        }
    }

    RequestResult ApplyStackRequest(PlayerInventory& inv, const proto::ItemStackRequest& request, bool creative) {
        RequestResult result;
        PlayerInventory work = inv;
        std::vector<std::pair<Container, int>> touched;

        const auto note = [&](const proto::StackRequestSlotInfo& info) {
            const auto key = std::make_pair(info.slotType.containerId, static_cast<int>(info.slot));
            if (info.slotType.containerId != Container::CreativeOutput &&
                std::find(touched.begin(), touched.end(), key) == touched.end())
                touched.push_back(key);
        };

        for (const proto::ItemStackRequestActionsEntry& a : request.actions) {
            switch (a.typeId) {
            case Action::Take:
            case Action::Place: {
                ItemStack* src = work.Resolve(a.source.slotType.containerId, a.source.slot);
                ItemStack* dst = work.Resolve(a.destination.slotType.containerId, a.destination.slot);
                if (!src || !dst || src == dst || !StackIdMatches(*src, a.source.stackId))
                    return result;
                // armor slots take wearables only, and the catalog has none
                if (a.destination.slotType.containerId == Container::Armor)
                    return result;
                if (!Move(*src, *dst, a.count))
                    return result;
                note(a.source);
                note(a.destination);
                break;
            }
            case Action::Swap: {
                ItemStack* src = work.Resolve(a.source.slotType.containerId, a.source.slot);
                ItemStack* dst = work.Resolve(a.destination.slotType.containerId, a.destination.slot);
                if (!src || !dst || src == dst || !StackIdMatches(*src, a.source.stackId) || !StackIdMatches(*dst, a.destination.stackId))
                    return result;
                if (a.destination.slotType.containerId == Container::Armor || a.source.slotType.containerId == Container::Armor)
                    return result;
                std::swap(*src, *dst);
                note(a.source);
                note(a.destination);
                break;
            }
            case Action::Drop:
            case Action::Destroy: {
                // no item entities exist yet: a dropped stack is simply gone
                ItemStack* src = work.Resolve(a.source.slotType.containerId, a.source.slot);
                if (!src || !StackIdMatches(*src, a.source.stackId) || src->Empty() || src->count < a.count || a.count == 0)
                    return result;
                src->count = static_cast<uint16_t>(src->count - a.count);
                if (src->count == 0)
                    *src = ItemStack{};
                note(a.source);
                break;
            }
            case Action::CraftCreative: {
                if (!creative)
                    return result;
                // creative entry ids are the item ids
                const CatalogItem* item = BlockCatalog::Instance().ItemById(static_cast<int16_t>(a.itemId));
                if (!item)
                    return result;
                work.output = work.MakeStack(item->networkId, MaxStack(item->networkId));
                break;
            }
            case Action::Optional:
            case Action::NonImplemented:
            case Action::ResultsDeprecated:
                break;
            default:
                return result;
            }
        }
        work.output = ItemStack{};
        // every slot a request changed gets a new stack id so a stale quote cannot match it later
        for (const auto& [type, slot] : touched)
            if (ItemStack* s = work.Resolve(type, slot); s && !s->Empty())
                s->stackId = work.NewStackId();
        inv = work;
        result.ok = true;
        result.touched = std::move(touched);
        return result;
    }

    proto::ItemStackResponsesEntry MakeResponse(PlayerInventory& inv, int32_t requestId, const RequestResult& result) {
        proto::ItemStackResponsesEntry entry;
        entry.requestId = requestId;
        entry.status = result.ok ? proto::ItemStackResponsesEntryStatus::Ok : proto::ItemStackResponsesEntryStatus::Error;
        if (!result.ok)
            return entry;
        std::vector<proto::ItemStackResponsesEntryContainersEntry> containers;
        for (const auto& [type, slot] : result.touched) {
            const ItemStack* s = inv.Resolve(type, slot);
            if (!s)
                continue;
            auto it = std::find_if(containers.begin(), containers.end(), [&](const auto& c) { return c.slotType.containerId == type; });
            if (it == containers.end()) {
                proto::ItemStackResponsesEntryContainersEntry c;
                c.slotType.containerId = type;
                containers.push_back(std::move(c));
                it = containers.end() - 1;
            }
            proto::ItemStackResponsesEntryContainersEntrySlotsEntry slotEntry;
            slotEntry.slot = static_cast<uint8_t>(slot);
            slotEntry.hotbarSlot = static_cast<uint8_t>(slot);
            slotEntry.count = s->Empty() ? 0 : static_cast<uint8_t>(std::min<int>(s->count, 255));
            slotEntry.itemStackId = s->Empty() ? 0 : s->stackId;
            it->slots.push_back(std::move(slotEntry));
        }
        entry.containers = std::move(containers);
        return entry;
    }
}
