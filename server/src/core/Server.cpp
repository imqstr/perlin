#include "core/Server.hpp"

#include "core/Json.hpp"
#include "core/Log.hpp"
#include "world/BlockCatalog.hpp"
#include "world/Blocks.hpp"

#include "protocol/Packets.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <thread>

namespace perlin::server {
    namespace {
        std::string Trim(std::string s) {
            const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
            s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
            return s;
        }

        std::string Lower(std::string s) {
            for (char& c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        // a block name with or without the namespace
        std::string FullBlockName(const std::string& n) { return n.find(':') == std::string::npos ? "minecraft:" + n : n; }
    }

    Server::Server(ServerConfig cfg) : config(std::move(cfg)) {
        loopback = std::make_unique<LoopbackListener>();
        plugins = std::make_unique<PluginHost>(*this);
    }

    Server::~Server() { Stop(); }

    bool Server::Start(std::string& error) {
        if (started)
            return true;
        if (config.generator != "flat") {
            error = "unknown generator '" + config.generator + "' (only \"flat\" exists yet)";
            return false;
        }
        {
            BlockCatalog& catalog = BlockCatalog::Instance();
            std::string catalogError;
            const std::string states = BlockCatalog::FindFile(config.blockDataDir, "block_states.json");
            const std::string names = BlockCatalog::FindFile(config.blockDataDir, "block_destroy.tsv");
            if (states.empty() || !catalog.Load(states, names, catalogError)) {
                catalog.LoadBuiltin();
                LogWarn("block data not found" + (catalogError.empty() ? std::string() : " (" + catalogError + ")") +
                        ": using the built-in list of " + std::to_string(catalog.Items().size()) +
                        " blocks; set block_data_dir to a folder with block_states.json and block_destroy.tsv");
            } else {
                LogInfo("block data: " + std::to_string(catalog.StateCount()) + " states, " + std::to_string(catalog.Items().size()) + " items");
            }
            BuildItemPackets();
        }
        BackendChoice choice = CreateTerrainBackend(config);
        if (!choice.backend) {
            error = "no terrain backend: " + choice.note;
            return false;
        }
        LogInfo("terrain: " + std::string(choice.backend->Name()) + (choice.note.empty() ? "" : " (" + choice.note + ")"));
        chunks = std::make_unique<ChunkManager>(std::move(choice.backend));

        RegisterBuiltins();

        std::string listenError;
        if (!loopback->Start(events, listenError)) {
            error = "loopback: " + listenError;
            return false;
        }
        if (config.nethernet && !config.loopbackOnly) {
            nethernet = CreateNetherNetListener(config, [this] {
                std::ostringstream o;
                o << "{\"name\":\"" << json::Escape(config.name) << "\",\"protocol\":2225,\"version\":\"1.26.60\",\"level\":\""
                  << json::Escape(config.levelName) << "\",\"players\":" << InGamePlayers().size() << ",\"maxPlayers\":" << config.maxPlayers
                  << ",\"gameType\":" << config.gameMode << "}";
                return o.str();
            });
            if (!nethernet) {
                error = "this build has no NetherNet support (rebuild with PERLIN_SERVER_NETHERNET=ON), or set loopback_only";
                return false;
            }
            if (!nethernet->Start(events, error))
                return false;
            LogInfo("nethernet signalling on " + config.bindAddress + ":" + std::to_string(config.port));
        }
        plugins->LoadDirectory(config.pluginDir);
        started = true;
        LogInfo("server '" + config.name + "' ready");
        return true;
    }

    void Server::Stop() {
        if (!started)
            return;
        started = false;
        for (auto& [id, session] : sessions)
            session->Kick("Server closed");
        sessions.clear();
        if (nethernet)
            nethernet->Stop();
        loopback->Stop();
        plugins->UnloadAll();
        chunks.reset();
    }

    void Server::Run() {
        using clock = std::chrono::steady_clock;
        const auto period = std::chrono::nanoseconds(1'000'000'000ll / std::max<uint32_t>(1, config.tickRate));
        auto next = clock::now();
        while (!stopRequested) {
            Tick();
            next += period;
            const auto now = clock::now();
            if (next < now) {
                // fell behind: drop the backlog instead of spiralling
                next = now;
            } else {
                std::this_thread::sleep_until(next);
            }
        }
    }

    void Server::Tick() {
        tick++;
        chunks->Pump();
        Pump();
        if (plugins->AnyoneListens(PS_EVENT_TICK)) {
            PayloadWriter w;
            w.U64(tick);
            plugins->Dispatch(PS_EVENT_TICK, w.bytes);
        }
        // position broadcast: once per tick for players that moved is plenty at 20 Hz
        std::vector<Session*> players = InGamePlayers();
        for (Session* s : players)
            if (s->TakeMoved())
                for (Session* other : players)
                    if (other != s)
                        other->SendMove(*s, 0);
        for (auto& [id, session] : sessions)
            session->Tick(tick);
        ReapClosed();
    }

    void Server::Pump() {
        std::vector<NetEvent> batch;
        events.Drain(batch);
        for (NetEvent& e : batch) {
            switch (e.type) {
            case NetEvent::Type::Open: {
                const uint64_t id = nextRuntimeId++;
                sessions[id] = std::make_unique<Session>(*this, e.connection, id);
                e.connection->id = id;
                break;
            }
            case NetEvent::Type::Message:
                if (const auto it = sessions.find(e.connection->id); it != sessions.end())
                    it->second->OnMessage(e.data);
                break;
            case NetEvent::Type::Close:
                if (const auto it = sessions.find(e.connection->id); it != sessions.end())
                    it->second->Close();
                break;
            }
        }
    }

    void Server::ReapClosed() {
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (it->second->IsClosed())
                it = sessions.erase(it);
            else
                ++it;
        }
    }

    std::vector<Session*> Server::InGamePlayers() {
        std::vector<Session*> out;
        for (auto& [id, s] : sessions)
            if (s->InGame())
                out.push_back(s.get());
        return out;
    }

    Session* Server::FindSession(uint64_t id) {
        const auto it = sessions.find(id);
        return it == sessions.end() ? nullptr : it->second.get();
    }

    Session* Server::FindByName(const std::string& name) {
        const std::string wanted = Lower(name);
        for (auto& [id, s] : sessions)
            if (!s->Name().empty() && Lower(s->Name()) == wanted)
                return s.get();
        return nullptr;
    }

    bool Server::IsOperatorName(const std::string& name) const {
        // embedded singleplayer: the one player owns the world
        if (config.loopbackOnly)
            return true;
        return std::find_if(config.operators.begin(), config.operators.end(), [&](const std::string& o) { return Lower(o) == Lower(name); }) != config.operators.end();
    }

    bool Server::IsOperator(const std::string& name, const std::string& xuid) const {
        if (config.loopbackOnly)
            return true;
        for (const std::string& o : config.operators) {
            if (!xuid.empty() && o == xuid)
                return true;
            if (!config.xboxAuth && Lower(o) == Lower(name))
                return true;
        }
        return false;
    }

    bool Server::Full() const {
        size_t n = 0;
        for (const auto& [id, s] : sessions)
            if (s->GetState() != Session::State::Closed && !s->Name().empty())
                n++;
        return n > config.maxPlayers;
    }

    double Server::SpawnY() const { return static_cast<double>(FlatSurfaceY(config)); }

    void Server::OnPlayerJoined(Session& joined) {
        LogInfo(joined.Name() + " joined (" + joined.Conn().Describe() + ")");
        for (Session* other : InGamePlayers()) {
            if (other == &joined)
                continue;
            other->ShowPlayer(joined);
            joined.ShowPlayer(*other);
        }
        PayloadWriter w;
        w.U64(joined.Id());
        w.Str(joined.Name());
        plugins->Dispatch(PS_EVENT_PLAYER_JOIN, w.bytes);
        Broadcast("§e" + joined.Name() + " joined the game");
    }

    void Server::OnPlayerLeft(Session& left) {
        LogInfo(left.Name() + " left");
        for (Session* other : InGamePlayers())
            if (other != &left)
                other->HidePlayer(left);
        PayloadWriter w;
        w.U64(left.Id());
        w.Str(left.Name());
        plugins->Dispatch(PS_EVENT_PLAYER_QUIT, w.bytes);
        for (Session* other : InGamePlayers())
            if (other != &left)
                other->SendRaw("§e" + left.Name() + " left the game");
    }

    void Server::HandleChat(Session& from, const std::string& message) {
        if (!message.empty() && message[0] == '/') {
            RunPlayerCommand(from, message);
            return;
        }
        PayloadWriter w;
        w.U64(from.Id());
        w.Str(from.Name());
        w.Str(message);
        if (plugins->Dispatch(PS_EVENT_CHAT, w.bytes) & 1u)
            return;
        LogInfo("<" + from.Name() + "> " + message);
        for (Session* s : InGamePlayers())
            s->SendText(1, from.Name(), message);
    }

    void Server::RunPlayerCommand(Session& from, const std::string& line) {
        std::string text = Trim(line);
        if (!text.empty() && text[0] == '/')
            text.erase(0, 1);
        const size_t space = text.find(' ');
        const std::string name = Lower(text.substr(0, space));
        const std::string args = space == std::string::npos ? "" : Trim(text.substr(space + 1));
        const auto it = commands.find(name);
        if (it == commands.end()) {
            from.SendRaw("§cUnknown command: /" + name + ". Try /help");
            return;
        }
        if (it->second.operatorOnly && !from.Operator()) {
            from.SendRaw("§cYou do not have permission to use /" + name);
            return;
        }
        if (!it->second.plugin.empty()) {
            plugins->DispatchCommand(it->second.plugin, from.Id(), name, args);
            return;
        }
        it->second.run(&from, args);
    }

    void Server::RunConsoleCommand(const std::string& line) {
        std::string text = Trim(line);
        if (!text.empty() && text[0] == '/')
            text.erase(0, 1);
        if (text.empty())
            return;
        const size_t space = text.find(' ');
        const std::string name = Lower(text.substr(0, space));
        const std::string args = space == std::string::npos ? "" : Trim(text.substr(space + 1));
        const auto it = commands.find(name);
        if (it == commands.end()) {
            LogWarn("unknown command: " + name);
            return;
        }
        if (!it->second.plugin.empty())
            plugins->DispatchCommand(it->second.plugin, 0, name, args);
        else
            it->second.run(nullptr, args);
    }

    void Server::RegisterBuiltins() {
        const auto reply = [](Session* to, const std::string& text) {
            if (to)
                to->SendRaw(text);
            else
                LogInfo(text);
        };
        const auto add = [this](const char* name, const char* description, bool op, std::function<void(Session*, const std::string&)> fn) {
            CommandDef d;
            d.name = name;
            d.description = description;
            d.operatorOnly = op;
            d.run = std::move(fn);
            commands[name] = std::move(d);
        };

        add("help", "List the commands you can use", false, [this, reply](Session* from, const std::string&) {
            std::string out = "§aCommands:";
            for (const auto& [n, d] : commands)
                if (!d.operatorOnly || !from || from->Operator())
                    out += "\n§7/" + n + "§r - " + d.description;
            reply(from, out);
        });
        add("list", "Show who is online", false, [this, reply](Session* from, const std::string&) {
            std::string names;
            const auto players = InGamePlayers();
            for (Session* p : players)
                names += (names.empty() ? "" : ", ") + p->Name();
            reply(from, std::to_string(players.size()) + "/" + std::to_string(config.maxPlayers) + " online: " + names);
        });
        add("say", "Broadcast a message", true, [this](Session*, const std::string& args) {
            if (!args.empty())
                Broadcast("§d[Server] " + args);
        });
        add("tp", "Teleport yourself: /tp <x> <y> <z>", true, [reply](Session* from, const std::string& args) {
            if (!from) {
                reply(from, "only players can teleport");
                return;
            }
            std::istringstream in(args);
            double x, y, z;
            if (!(in >> x >> y >> z)) {
                reply(from, "§cUsage: /tp <x> <y> <z>");
                return;
            }
            // y is feet height; the protocol position is the eyes
            from->Teleport(x, y + 1.62, z);
        });
        add("kick", "Kick a player: /kick <name> [reason]", true, [this, reply](Session* from, const std::string& args) {
            std::istringstream in(args);
            std::string who;
            in >> who;
            std::string reason;
            std::getline(in, reason);
            reason = Trim(reason);
            Session* target = FindByName(who);
            if (!target) {
                reply(from, "§cNo such player: " + who);
                return;
            }
            target->Kick(reason.empty() ? "Kicked by an operator" : reason);
        });
        add("plugins", "List loaded plugins", false, [this, reply](Session* from, const std::string&) {
            std::string out = "Plugins:";
            for (const auto& p : plugins->List())
                out += "\n" + p.name + (p.version.empty() ? "" : " " + p.version) + (p.enabled ? "" : " (disabled: " + p.error + ")");
            reply(from, out);
        });
        add("stop", "Stop the server", true, [this](Session*, const std::string&) { RequestStop(); });
    }

    void Server::Broadcast(const std::string& text) {
        LogInfo(text);
        for (Session* s : InGamePlayers())
            s->SendRaw(text);
    }

    bool Server::Message(uint64_t player, const std::string& text) {
        // player 0 is the console
        if (player == 0) {
            LogInfo(text);
            return true;
        }
        Session* s = FindSession(player);
        if (!s || !s->InGame())
            return false;
        s->SendRaw(text);
        return true;
    }

    bool Server::Kick(uint64_t player, const std::string& reason) {
        Session* s = FindSession(player);
        if (!s)
            return false;
        s->Kick(reason);
        return true;
    }

    void Server::RegisterCommand(const std::string& plugin, const std::string& name, const std::string& description) {
        CommandDef d;
        d.name = Lower(name);
        d.description = description;
        d.plugin = plugin;
        commands[d.name] = std::move(d);
    }

    std::vector<PluginApi::PlayerInfo> Server::Players() {
        std::vector<PlayerInfo> out;
        for (Session* s : InGamePlayers())
            out.push_back({s->Id(), s->Name(), s->X(), s->Y() - 1.62, s->Z()});
        return out;
    }

    bool Server::Teleport(uint64_t player, double x, double y, double z) {
        Session* s = FindSession(player);
        if (!s || !s->InGame())
            return false;
        s->Teleport(x, y + 1.62, z);
        return true;
    }

    void Server::ApplyBlock(int x, int y, int z, uint16_t block) {
        chunks->SetBlock(x, y, z, block);
        // the cached payload for this column rebuilds on its version bump; players who already have it get the single change now
        const int32_t hash = BlockRegistry::Instance().Hash(block);
        for (Session* s : InGamePlayers())
            if (s->HasSent({x >> 4, z >> 4}))
                s->SendBlockUpdate(x, y, z, hash);
        if (plugins->AnyoneListens(PS_EVENT_BLOCK_CHANGE)) {
            PayloadWriter w;
            w.I32(x);
            w.I32(y);
            w.I32(z);
            w.Str(BlockRegistry::Instance().Name(block));
            plugins->Dispatch(PS_EVENT_BLOCK_CHANGE, w.bytes);
        }
    }

    void Server::SendBlockTo(Session& to, int x, int y, int z) {
        if (!chunks->Peek({x >> 4, z >> 4}))
            return;
        to.SendBlockUpdate(x, y, z, BlockRegistry::Instance().Hash(chunks->GetBlock(x, y, z)));
    }

    int Server::SetBlock(int x, int y, int z, const std::string& block) {
        if (y < kMinY || y >= kMinY + kHeight)
            return PS_ERR_BAD_ARGUMENTS;
        // by name means the default state; a block with no item is not one the catalog can place
        const CatalogItem* item = BlockCatalog::Instance().ItemByName(FullBlockName(block));
        const bool air = FullBlockName(block) == "minecraft:air";
        if (!item && !air)
            return PS_ERR_BAD_ARGUMENTS;
        if (!chunks->Peek({x >> 4, z >> 4}))
            return PS_ERR_NOT_LOADED;
        ApplyBlock(x, y, z, air ? BlockRegistry::kAir : BlockCatalog::Instance().RegistryId(item->blockHash));
        return PS_OK;
    }

    namespace {
        // block face index to the neighbour it points at, the order the game numbers faces in
        constexpr int kFaceOffset[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
    }

    bool Server::PlayerBreak(Session& by, int x, int y, int z) {
        if (y < kMinY || y >= kMinY + kHeight || !by.InReach(x, y, z) || !chunks->Peek({x >> 4, z >> 4})) {
            by.CorrectBlock(x, y, z);
            return false;
        }
        const uint16_t current = chunks->GetBlock(x, y, z);
        if (current == BlockRegistry::kAir) {
            // a second message for a break that already happened: confirm, change nothing
            by.CorrectBlock(x, y, z);
            return true;
        }
        const std::string name = BlockRegistry::Instance().Name(current);
        if (!by.Creative() && name == "minecraft:bedrock") {
            by.CorrectBlock(x, y, z);
            return false;
        }
        if (plugins->AnyoneListens(PS_EVENT_BLOCK_BREAK)) {
            PayloadWriter w;
            w.U64(by.Id());
            w.I32(x);
            w.I32(y);
            w.I32(z);
            w.Str(name);
            if (plugins->Dispatch(PS_EVENT_BLOCK_BREAK, w.bytes) & 1u) {
                by.CorrectBlock(x, y, z);
                return false;
            }
        }
        ApplyBlock(x, y, z, BlockRegistry::kAir);
        // no item entities yet: a survival player gets the block straight into the inventory
        if (!by.Creative())
            if (const CatalogItem* item = BlockCatalog::Instance().ItemByName(name)) {
                by.Inventory().Give(item->networkId, 1);
                by.SendInventory();
            }
        return true;
    }

    bool Server::PlayerPlace(Session& by, int x, int y, int z, int face) {
        const auto reject = [&](int tx, int ty, int tz) {
            by.CorrectBlock(x, y, z);
            by.CorrectBlock(tx, ty, tz);
            by.SendSlot(by.Inventory().selected);
            return false;
        };
        if (face < 0 || face > 5)
            return false;
        const int tx = x + kFaceOffset[face][0], ty = y + kFaceOffset[face][1], tz = z + kFaceOffset[face][2];
        const ItemStack& held = by.Inventory().Held();
        const CatalogItem* item = held.Empty() ? nullptr : BlockCatalog::Instance().ItemById(held.networkId);
        if (!item || ty < kMinY || ty >= kMinY + kHeight || !by.InReach(tx, ty, tz) || !chunks->Peek({tx >> 4, tz >> 4}) ||
            chunks->GetBlock(tx, ty, tz) != BlockRegistry::kAir)
            return reject(tx, ty, tz);
        if (plugins->AnyoneListens(PS_EVENT_BLOCK_PLACE)) {
            PayloadWriter w;
            w.U64(by.Id());
            w.I32(tx);
            w.I32(ty);
            w.I32(tz);
            w.Str(item->name);
            if (plugins->Dispatch(PS_EVENT_BLOCK_PLACE, w.bytes) & 1u)
                return reject(tx, ty, tz);
        }
        ApplyBlock(tx, ty, tz, BlockCatalog::Instance().RegistryId(item->blockHash));
        if (!by.Creative()) {
            by.Inventory().TakeHeld(1);
            by.SendSlot(by.Inventory().selected);
        }
        return true;
    }

    int Server::GiveItem(uint64_t player, const std::string& itemName, int count) {
        Session* s = FindSession(player);
        if (!s || !s->InGame())
            return PS_ERR_NO_SUCH_PLAYER;
        const CatalogItem* item = BlockCatalog::Instance().ItemByName(FullBlockName(itemName));
        if (!item)
            return PS_ERR_UNKNOWN_ITEM;
        const int left = s->Inventory().Give(item->networkId, count);
        s->SendInventory();
        return left;
    }

    void Server::BuildItemPackets() {
        const BlockCatalog& catalog = BlockCatalog::Instance();
        proto2225::ItemRegistryPacket registry;
        proto2225::CreativeContentPacket creative;
        proto2225::CreativeContentPacketGroupsEntry group;
        group.category = proto2225::CreativeContentPacketGroupsEntryCategory::Construction;
        group.name = "itemGroup.name.construction";
        for (const CatalogItem& item : catalog.Items()) {
            proto2225::ItemstatesEntry entry;
            entry.name = item.name;
            entry.runtimeId = item.networkId;
            entry.componentBased = false;
            entry.version = proto2225::ItemstatesEntryVersion::Legacy;
            registry.itemstates.push_back(std::move(entry));

            proto2225::ItemLegacy legacy;
            legacy.networkId = item.networkId;
            legacy.count = 1;
            legacy.blockRuntimeId = item.blockHash;
            if (group.iconItem.networkId == 0)
                group.iconItem = legacy;
            proto2225::CreativeContentPacketItemsEntry e;
            e.entryId = static_cast<uint32_t>(item.networkId);
            e.item = legacy;
            e.groupIndex = 0;
            creative.items.push_back(std::move(e));
        }
        creative.groups.push_back(std::move(group));
        net::EncodeChecked(registry, itemRegistryBody);
        net::EncodeChecked(creative, creativeBody);
    }

    int Server::GetBlock(int x, int y, int z, std::string& block) {
        if (!chunks->Peek({x >> 4, z >> 4}))
            return PS_ERR_NOT_LOADED;
        block = BlockRegistry::Instance().Name(chunks->GetBlock(x, y, z));
        return PS_OK;
    }

    PluginApi::Status Server::GetStatus() {
        Status s;
        s.name = config.name;
        s.players = static_cast<uint32_t>(InGamePlayers().size());
        s.maxPlayers = config.maxPlayers;
        s.tick = tick;
        s.backend = chunks ? chunks->BackendName() : "";
        return s;
    }

    std::string Server::Describe() const {
        return config.name + " (" + std::to_string(sessions.size()) + " connections, " + std::to_string(chunks ? chunks->Loaded() : 0) + " chunks)";
    }
}
