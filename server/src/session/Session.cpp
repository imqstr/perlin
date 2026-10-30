#include "session/Session.hpp"

#include "core/Auth.hpp"
#include "core/Json.hpp"
#include "core/Log.hpp"
#include "core/Server.hpp"
#include "world/BlockCatalog.hpp"
#include "world/Blocks.hpp"

#include "protocol/Packets.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace perlin::server {
    namespace proto = proto2225;

    namespace {
        constexpr int32_t kProtocol = 2225;
        constexpr const char* kGameVersion = "1.26.60";
        constexpr double kEyeHeight = 1.62;
        // chunks sent per tick to one player; keeps a join from flooding the data channel
        constexpr int kChunksPerTick = 6;
        constexpr auto kIdleTimeout = std::chrono::seconds(30);
        constexpr auto kLoginTimeout = std::chrono::seconds(60);

        template <class P> bool Decode(std::span<const uint8_t> body, P& packet) {
            net::ReadStream stream(body.data(), body.size());
            return Read(stream, packet) && stream.ok;
        }

        std::array<uint8_t, 16> ParseUuid(const std::string& text, uint64_t fallbackSeed) {
            std::array<uint8_t, 16> text16{};
            int n = 0;
            for (size_t i = 0; i < text.size() && n < 32; i++) {
                const char c = text[i];
                int v;
                if (c >= '0' && c <= '9') v = c - '0';
                else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
                else continue;
                text16[static_cast<size_t>(n / 2)] = static_cast<uint8_t>((text16[static_cast<size_t>(n / 2)] << 4) | v);
                n++;
            }
            if (n != 32) {
                // no usable identity: derive a stable-looking one from the runtime id
                for (size_t i = 0; i < 16; i++)
                    text16[i] = static_cast<uint8_t>((fallbackSeed * 0x9E3779B97F4A7C15ull) >> (i * 4));
            }
            // the wire stores each 8 byte half little endian
            std::array<uint8_t, 16> wire{};
            for (size_t i = 0; i < 8; i++) {
                wire[i] = text16[7 - i];
                wire[8 + i] = text16[15 - i];
            }
            return wire;
        }

        int ImageSide(const json::Value& v, const char* key, int fallback) {
            const int64_t n = v[key].Int(fallback);
            return n > 0 && n <= 1024 ? static_cast<int>(n) : fallback;
        }

        proto::Skin SkinFromClientData(const json::Value& c, const std::string& playerName) {
            proto::Skin s;
            s.skinId = c["SkinId"].String("Standard_Custom");
            s.skinResourcePack = json::Base64UrlDecode(c["SkinResourcePatch"].String());
            s.skinData.width = ImageSide(c, "SkinImageWidth", 64);
            s.skinData.height = ImageSide(c, "SkinImageHeight", 64);
            const std::string rgba = json::Base64UrlDecode(c["SkinData"].String());
            const size_t want = static_cast<size_t>(s.skinData.width) * static_cast<size_t>(s.skinData.height) * 4;
            s.skinData.data.assign(rgba.begin(), rgba.end());
            if (s.skinData.data.size() != want) {
                // a missing or malformed skin shows as a flat grey player instead of breaking the list packet
                s.skinData.width = 64;
                s.skinData.height = 64;
                s.skinData.data.assign(64 * 64 * 4, 0x80);
            }
            s.capeData.width = ImageSide(c, "CapeImageWidth", 0);
            s.capeData.height = ImageSide(c, "CapeImageHeight", 0);
            const std::string cape = json::Base64UrlDecode(c["CapeData"].String());
            if (cape.size() == static_cast<size_t>(s.capeData.width) * static_cast<size_t>(s.capeData.height) * 4)
                s.capeData.data.assign(cape.begin(), cape.end());
            else
                s.capeData = {};
            s.geometryData = json::Base64UrlDecode(c["SkinGeometryData"].String());
            s.geometryDataVersion = c["SkinGeometryDataEngineVersion"].String();
            s.animationData = json::Base64UrlDecode(c["SkinAnimationData"].String());
            s.capeId = c["CapeId"].String();
            s.fullSkinId = s.skinId + s.capeId;
            s.armSize = c["ArmSize"].String() == "slim" ? proto::SkinArmSize::Slim : proto::SkinArmSize::Wide;
            s.premium = c["PremiumSkin"].Bool();
            s.persona = c["PersonaSkin"].Bool();
            s.capeOnClassic = c["CapeOnClassicSkin"].Bool();
            s.trusted = "true";
            (void)playerName;
            return s;
        }
    }

    Session::Session(Server& server, std::shared_ptr<Connection> connection, uint64_t runtimeId)
        : server(server), connection(std::move(connection)), runtimeId(runtimeId), skin(std::make_unique<proto::Skin>()) {
        posY = server.SpawnY() + kEyeHeight;
    }

    Session::~Session() = default;

    const proto::Skin& Session::GetSkin() const { return *skin; }

    template <class P> void Session::Send(const P& packet) {
        std::vector<uint8_t> body;
        if (!net::EncodeChecked(packet, body)) {
            LogWarn(std::string("refusing to send malformed ") + P::kName);
            return;
        }
        Queue(P::kId, body);
    }

    void Session::Queue(uint32_t id, const std::vector<uint8_t>& body) {
        if (state == State::Closed)
            return;
        BatchCodec::AppendPacket(outgoing, id, body);
    }

    void Session::Flush() {
        if (outgoing.empty() || state == State::Closed)
            return;
        const std::vector<uint8_t> wire = codec.Seal(outgoing);
        outgoing.clear();
        if (!connection->Send(wire.data(), wire.size(), true))
            Close();
    }

    // settings and early status replies go out before compression is on: send exactly this packet, uncompressed
    void Session::SendImmediate(uint32_t id, const std::vector<uint8_t>& body) {
        std::vector<uint8_t> content;
        BatchCodec::AppendPacket(content, id, body);
        const std::vector<uint8_t> wire = codec.Seal(content);
        connection->Send(wire.data(), wire.size(), true);
    }

    void Session::Close() {
        if (state == State::Closed)
            return;
        const bool wasInGame = state == State::InGame;
        state = State::Closed;
        if (wasInGame)
            server.OnPlayerLeft(*this);
        connection->Close();
    }

    void Session::Kick(const std::string& reason) {
        if (state == State::Closed)
            return;
        proto::DisconnectPacket p;
        p.reason = proto::DisconnectFailReason::Unknown;
        p.message = reason;
        p.filteredMessage = reason;
        Send(p);
        Flush();
        Close();
    }

    void Session::OnMessage(std::span<const uint8_t> wire) {
        if (state == State::Closed)
            return;
        lastPacket = std::chrono::steady_clock::now();
        std::vector<uint8_t> content;
        if (!codec.Open(wire, content)) {
            LogWarn(connection->Describe() + ": corrupt batch, closing");
            Close();
            return;
        }
        const bool ok = BatchCodec::Split(content, [this](uint32_t id, std::span<const uint8_t> body) {
            if (state != State::Closed)
                Dispatch(id, body);
        });
        if (!ok) {
            LogWarn(connection->Describe() + ": truncated batch, closing");
            Close();
        }
    }

    void Session::Dispatch(uint32_t id, std::span<const uint8_t> body) {
        switch (id) {
        case proto::RequestNetworkSettingsPacket::kId: OnRequestSettings(body); break;
        case proto::LoginPacket::kId: OnLogin(body); break;
        case proto::ResourcePackClientResponsePacket::kId: OnPackResponse(body); break;
        case proto::RequestChunkRadiusPacket::kId: OnChunkRadius(body); break;
        case proto::SetLocalPlayerAsInitializedPacket::kId: OnInitialized(); break;
        case proto::TextPacket::kId: OnText(body); break;
        case proto::CommandRequestPacket::kId: OnCommand(body); break;
        case proto::PlayerAuthInputPacket::kId: OnAuthInput(body); break;
        case proto::MovePlayerPacket::kId: OnMovePlayer(body); break;
        case proto::ItemStackRequestPacket::kId: OnStackRequest(body); break;
        case proto::MobEquipmentPacket::kId: OnMobEquipment(body); break;
        case proto::InventoryTransactionPacket::kId: OnTransaction(body); break;
        case proto::PlayerActionPacket::kId: OnPlayerAction(body); break;
        case proto::BlockPickRequestPacket::kId: OnPickBlock(body); break;
        case proto::InteractPacket::kId: OnInteract(body); break;
        case proto::ContainerClosePacket::kId: OnContainerClose(body); break;
        case proto::ClientToServerHandshakePacket::kId:
        case proto::ClientCacheStatusPacket::kId:
        default: break; // everything else a client may send is ignored for now
        }
    }

    void Session::OnRequestSettings(std::span<const uint8_t> body) {
        proto::RequestNetworkSettingsPacket req;
        if (state != State::AwaitSettings || !Decode(body, req)) {
            Close();
            return;
        }
        if (req.clientProtocol != kProtocol) {
            proto::PlayStatusPacket p;
            p.status = req.clientProtocol < kProtocol ? proto::PlayStatusPacketStatus::FailedClient : proto::PlayStatusPacketStatus::FailedSpawn;
            std::vector<uint8_t> out;
            net::EncodeChecked(p, out);
            SendImmediate(proto::PlayStatusPacket::kId, out);
            LogInfo(connection->Describe() + ": protocol " + std::to_string(req.clientProtocol) + " rejected (server speaks " +
                    std::to_string(kProtocol) + ")");
            Close();
            return;
        }
        proto::NetworkSettingsPacket settings;
        settings.compressionThreshold = 256;
        settings.compressionAlgorithm = proto::NetworkSettingsPacketCompressionAlgorithm::Deflate;
        std::vector<uint8_t> out;
        net::EncodeChecked(settings, out);
        // the settings reply itself is the last uncompressed batch
        SendImmediate(proto::NetworkSettingsPacket::kId, out);
        codec.EnableCompression(256);
        state = State::AwaitLogin;
        stateSince = std::chrono::steady_clock::now();
    }

    void Session::OnLogin(std::span<const uint8_t> body) {
        proto::LoginPacket login;
        if (state != State::AwaitLogin || !Decode(body, login)) {
            Close();
            return;
        }
        // signatures are always checked; what a valid chain proves depends on xbox_auth (see ServerConfig)
        const auth::Login login2 = auth::VerifyLogin(login.tokens.identity, login.tokens.client,
                                                     server.Config().authRootKeys.empty() ? auth::DefaultRootKeys() : server.Config().authRootKeys);
        if (!login2.chainValid) {
            LogWarn(connection->Describe() + ": login rejected: " + login2.error);
            Kick("Invalid login");
            return;
        }
        if (server.Config().xboxAuth) {
            std::string why;
            if (!login2.xboxVerified) {
                why = "not signed in to Xbox Live";
            } else if (!connection->offerSdp.empty() && !auth::VerifyOfferBinding(connection->offerSdp, login2.clientKey, why)) {
                // a replayed login cannot also sign this connection's dtls fingerprints
            } else {
                why.clear();
            }
            if (!why.empty()) {
                LogInfo(connection->Describe() + ": refused: " + why);
                proto::DisconnectPacket d;
                d.reason = proto::DisconnectFailReason::NotAuthenticated;
                d.message = "This server requires you to be signed in to Xbox Live";
                d.filteredMessage = d.message;
                Send(d);
                Flush();
                Close();
                return;
            }
        }
        name = login2.name;
        xuid = login2.xuid;
        std::string uuidText = login2.uuid;
        const json::Value clientData = login2.clientData;
        const bool haveClient = login2.clientDataValid;
        if (name.empty() && haveClient)
            name = clientData["ThirdPartyName"].String();
        if (name.empty()) {
            Kick("Invalid login: no player name");
            return;
        }
        // names are shown in chat and used by commands
        for (char& c : name)
            if (static_cast<unsigned char>(c) < 0x20)
                c = ' ';
        if (name.size() > 32)
            name.resize(32);
        uuid = ParseUuid(uuidText, runtimeId);
        *skin = SkinFromClientData(haveClient ? clientData : json::Value{}, name);
        isOperator = server.IsOperator(name, xuid);
        creative = server.Config().gameMode == 1;

        if (server.Full()) {
            proto::PlayStatusPacket p;
            p.status = proto::PlayStatusPacketStatus::FailedServerFull;
            Send(p);
            Flush();
            Close();
            return;
        }
        if (Session* other = server.FindByName(name); other && other != this && other->InGame()) {
            Kick("A player with this name is already connected");
            return;
        }

        proto::PlayStatusPacket ok;
        ok.status = proto::PlayStatusPacketStatus::LoginSuccess;
        Send(ok);

        proto::ResourcePacksInfoPacket info;
        info.mustAccept = false;
        Send(info);
        state = State::ResourcePacks;
        stateSince = std::chrono::steady_clock::now();
        LogInfo(name + " logging in from " + connection->Describe());
    }

    void Session::OnPackResponse(std::span<const uint8_t> body) {
        proto::ResourcePackClientResponsePacket r;
        if (!Decode(body, r)) {
            Close();
            return;
        }
        using Status = proto::ResourcePackClientResponsePacketResponseStatus;
        switch (r.responseStatus) {
        case Status::Refused:
            Kick("Resource packs refused");
            break;
        case Status::SendPacks:
            // the server offers none
            Kick("Requested a resource pack this server does not have");
            break;
        case Status::HaveAllPacks: {
            proto::ResourcePackStackPacket stack;
            stack.mustAccept = false;
            stack.gameVersion = "*";
            Send(stack);
            state = State::ResourceStack;
            break;
        }
        case Status::Completed:
            if (state == State::ResourceStack || state == State::ResourcePacks) {
                SendStartGame();
                SendWorldSetup();
                state = State::Loading;
                stateSince = std::chrono::steady_clock::now();
            }
            break;
        }
    }

    void Session::SendStartGame() {
        const ServerConfig& cfg = server.Config();
        proto::StartGamePacket g;
        g.entityId = static_cast<int64_t>(runtimeId);
        g.runtimeEntityId = runtimeId;
        g.playerGamemode = static_cast<proto::GameMode>(cfg.gameMode);
        g.playerPosition = {static_cast<float>(posX), static_cast<float>(posY), static_cast<float>(posZ)};
        g.rotation = {0.0f, 0.0f};
        g.seed = cfg.seed;
        g.biomeType = 0;
        g.biomeName = "plains";
        g.dimension = proto::StartGamePacketDimension::Overworld;
        // 2 = flat in the generator enum; informational for the client
        g.generator = 2;
        g.worldGamemode = static_cast<proto::GameMode>(cfg.gameMode);
        g.difficulty = 1;
        g.spawnPosition = {0, static_cast<int32_t>(std::floor(server.SpawnY())), 0};
        g.achievementsDisabled = true;
        g.rainLevel = 0;
        g.lightningLevel = 0;
        g.isMultiplayer = true;
        g.broadcastToLan = false;
        g.enableCommands = true;
        g.permissionLevel = isOperator ? proto::PermissionLevel::Operator : proto::PermissionLevel::Member;
        g.serverChunkTickRange = 4;
        g.gameVersion = "*";
        g.levelId = cfg.levelName;
        g.worldName = cfg.name;
        g.currentTick = static_cast<int64_t>(server.TickCount());
        g.multiplayerCorrelationId = "perlin-" + std::to_string(runtimeId);
        g.serverAuthoritativeInventory = true;
        g.engine = "Perlin Server";
        // block ids on the wire are the name hashes: the chunk encoder writes those
        g.blockNetworkIdsAreHashes = true;
        g.serverIdentifier = cfg.name;
        Send(g);
    }

    void Session::SendWorldSetup() {
        Queue(proto::ItemRegistryPacket::kId, server.ItemRegistryBody());
        Queue(proto::CreativeContentPacket::kId, server.CreativeContentBody());
        Send(proto::BiomeDefinitionListPacket{});
        Send(proto::AvailableEntityIdentifiersPacket{});

        proto::SetTimePacket t;
        t.time = 6000;
        Send(t);

        proto::AvailableCommandsPacket commands;
        for (const auto& [cmdName, def] : server.Commands()) {
            if (def.operatorOnly && !isOperator)
                continue;
            proto::AvailableCommandsPacketCommandDataEntry e;
            e.name = cmdName;
            e.description = def.description;
            e.permissionLevel = "any";
            e.alias = -1;
            proto::AvailableCommandsPacketCommandDataEntryOverloadsEntry overload;
            proto::AvailableCommandsPacketCommandDataEntryOverloadsEntryParametersEntry arg;
            arg.parameterName = "args";
            arg.valueType = proto::AvailableCommandsPacketCommandDataEntryOverloadsEntryParametersEntryValueType::RawText;
            arg.enumType = proto::AvailableCommandsPacketCommandDataEntryOverloadsEntryParametersEntryEnumType::Valid;
            arg.optional = true;
            overload.parameters.push_back(arg);
            e.overloads.push_back(std::move(overload));
            commands.commandData.push_back(std::move(e));
        }
        Send(commands);
        SendInventory();
    }

    void Session::SendInventory() {
        const auto content = [&](proto::WindowIDVarint window, proto::ContainerSlotType type, const ItemStack* stacks, size_t n) {
            proto::InventoryContentPacket p;
            p.windowId = window;
            p.container.containerId = type;
            for (size_t i = 0; i < n; i++)
                p.input.push_back(ToWire(stacks[i]));
            Send(p);
        };
        content(proto::WindowIDVarint::Inventory, proto::ContainerSlotType::HotbarAndInventory, inventory.main.data(), inventory.main.size());
        content(proto::WindowIDVarint::Armor, proto::ContainerSlotType::Armor, inventory.armor.data(), inventory.armor.size());
        content(proto::WindowIDVarint::Offhand, proto::ContainerSlotType::Offhand, &inventory.offhand, 1);
        SendHeldSelection();
    }

    void Session::SendSlot(int slot) {
        if (slot < 0 || slot >= PlayerInventory::kMainSlots)
            return;
        proto::InventorySlotPacket p;
        p.windowId = proto::WindowIDVarint::Inventory;
        p.slot = static_cast<uint32_t>(slot);
        proto::FullContainerName c;
        c.containerId = proto::ContainerSlotType::HotbarAndInventory;
        p.container = c;
        p.item = ToWire(inventory.main[static_cast<size_t>(slot)]);
        Send(p);
    }

    void Session::SendHeldSelection() {
        proto::MobEquipmentPacket m;
        m.runtimeEntityId = runtimeId;
        m.item = ToWire(inventory.Held());
        m.slot = static_cast<uint8_t>(inventory.selected);
        m.selectedSlot = static_cast<uint8_t>(inventory.selected);
        m.windowId = proto::WindowID::Inventory;
        Send(m);
    }

    void Session::CorrectBlock(int x, int y, int z) {
        server.SendBlockTo(*this, x, y, z);
    }

    bool Session::InReach(int x, int y, int z) const {
        const double dx = x + 0.5 - posX, dy = y + 0.5 - posY, dz = z + 0.5 - posZ;
        // a generous reach: the point is to stop a client editing across the map, not to police the exact limit
        return dx * dx + dy * dy + dz * dz <= 12.0 * 12.0;
    }

    void Session::OnStackRequest(std::span<const uint8_t> body) {
        proto::ItemStackRequestPacket p;
        if (!Decode(body, p) || state != State::InGame)
            return;
        proto::ItemStackResponsePacket reply;
        for (const proto::ItemStackRequest& r : p.requests) {
            const RequestResult result = ApplyStackRequest(inventory, r, creative);
            reply.responses.push_back(MakeResponse(inventory, r.requestId, result));
        }
        if (!reply.responses.empty())
            Send(reply);
    }

    void Session::OnMobEquipment(std::span<const uint8_t> body) {
        proto::MobEquipmentPacket m;
        if (!Decode(body, m) || state != State::InGame)
            return;
        if (m.selectedSlot < PlayerInventory::kHotbar)
            inventory.selected = m.selectedSlot;
    }

    void Session::OnInteract(std::span<const uint8_t> body) {
        proto::InteractPacket i;
        if (!Decode(body, i) || state != State::InGame)
            return;
        if (i.actionId == proto::InteractPacketActionId::OpenInventory) {
            proto::ContainerOpenPacket open;
            open.windowId = proto::WindowID::Inventory;
            open.windowType = proto::WindowType::Inventory;
            open.coordinates = {static_cast<int32_t>(std::floor(posX)), static_cast<int32_t>(std::floor(posY)), static_cast<int32_t>(std::floor(posZ))};
            open.runtimeEntityId = -1;
            Send(open);
        }
    }

    void Session::OnContainerClose(std::span<const uint8_t> body) {
        proto::ContainerClosePacket c;
        if (!Decode(body, c) || state != State::InGame)
            return;
        // closing returns the pointer stack to the player the way the game does: the client already moved it, nothing to undo
        proto::ContainerClosePacket ack;
        ack.windowId = c.windowId;
        ack.windowType = proto::WindowType::Inventory;
        ack.server = false;
        Send(ack);
    }

    void Session::OnPickBlock(std::span<const uint8_t> body) {
        proto::BlockPickRequestPacket p;
        if (!Decode(body, p) || state != State::InGame || !InReach(p.x, p.y, p.z))
            return;
        std::string blockName;
        if (server.GetBlock(p.x, p.y, p.z, blockName) != 0)
            return;
        const CatalogItem* item = BlockCatalog::Instance().ItemByName(blockName);
        if (!item)
            return;
        int slot = -1;
        for (int i = 0; i < PlayerInventory::kHotbar; i++)
            if (!inventory.main[static_cast<size_t>(i)].Empty() && inventory.main[static_cast<size_t>(i)].networkId == item->networkId)
                slot = i;
        if (slot < 0 && creative) {
            slot = inventory.selected;
            for (int i = 0; i < PlayerInventory::kHotbar && !inventory.main[static_cast<size_t>(slot)].Empty(); i++)
                if (inventory.main[static_cast<size_t>(i)].Empty())
                    slot = i;
            inventory.main[static_cast<size_t>(slot)] = inventory.MakeStack(item->networkId, MaxStack(item->networkId));
            SendSlot(slot);
        }
        if (slot < 0)
            return;
        inventory.selected = slot;
        SendHeldSelection();
    }

    void Session::OnPlayerAction(std::span<const uint8_t> body) {
        proto::PlayerActionPacket a;
        if (!Decode(body, a) || state != State::InGame)
            return;
        const auto& pos = a.position;
        switch (a.action) {
        // creative breaks on the first message; survival waits for the stop, since the dig time is the client's to count here
        case proto::Action::StartBreak:
            if (creative)
                server.PlayerBreak(*this, pos.x, pos.y, pos.z);
            break;
        case proto::Action::StopBreak:
        case proto::Action::CreativePlayerDestroyBlock:
            server.PlayerBreak(*this, pos.x, pos.y, pos.z);
            break;
        default:
            break;
        }
    }

    void Session::OnTransaction(std::span<const uint8_t> body) {
        proto::InventoryTransactionPacket t;
        if (!Decode(body, t) || state != State::InGame)
            return;
        if (t.transaction.transactionType != proto::TransactionTransactionType::ItemUse)
            return;
        const auto* use = std::get_if<proto::TransactionUseItem>(&t.transaction.transactionData);
        if (!use)
            return;
        if (use->hotbarSlot >= 0 && use->hotbarSlot < PlayerInventory::kHotbar)
            inventory.selected = use->hotbarSlot;
        const auto& at = use->blockPosition;
        switch (use->actionType) {
        case proto::TransactionUseItemActionType::BreakBlock:
            server.PlayerBreak(*this, at.x, at.y, at.z);
            break;
        case proto::TransactionUseItemActionType::ClickBlock:
            server.PlayerPlace(*this, at.x, at.y, at.z, use->face);
            break;
        default:
            break;
        }
    }

    void Session::OnChunkRadius(std::span<const uint8_t> body) {
        proto::RequestChunkRadiusPacket req;
        if (!Decode(body, req) || state == State::AwaitSettings || state == State::AwaitLogin)
            return;
        radius = std::clamp<int32_t>(req.chunkRadius, 2, static_cast<int32_t>(server.Config().viewDistance));
        proto::ChunkRadiusUpdatePacket reply;
        reply.chunkRadius = radius;
        Send(reply);
        PublishCenter(true);
    }

    void Session::PublishCenter(bool force) {
        const ChunkPos now{static_cast<int32_t>(std::floor(posX)) >> 4, static_cast<int32_t>(std::floor(posZ)) >> 4};
        if (!force && centerSent && now == center)
            return;
        center = now;
        centerSent = true;
        if (radius == 0)
            return;
        proto::NetworkChunkPublisherUpdatePacket u;
        u.coordinates = {static_cast<int32_t>(std::floor(posX)), static_cast<int32_t>(std::floor(posY)), static_cast<int32_t>(std::floor(posZ))};
        u.radius = static_cast<uint32_t>(radius * 16);
        Send(u);
        // columns that fell out of range may be sent again if the player returns
        for (auto it = sent.begin(); it != sent.end();) {
            if (std::abs(it->x - center.x) > radius + 1 || std::abs(it->z - center.z) > radius + 1)
                it = sent.erase(it);
            else
                ++it;
        }
    }

    bool Session::WantsChunk(ChunkPos pos) const {
        if (radius == 0 || sent.count(pos))
            return false;
        const int dx = pos.x - center.x, dz = pos.z - center.z;
        return dx * dx + dz * dz <= radius * radius;
    }

    void Session::StreamChunks() {
        if (radius == 0 || !centerSent)
            return;
        // nearest first, so the spawn area is there before the horizon
        std::vector<ChunkPos> want;
        for (int dx = -radius; dx <= radius; dx++)
            for (int dz = -radius; dz <= radius; dz++) {
                const ChunkPos p{center.x + dx, center.z + dz};
                if (WantsChunk(p))
                    want.push_back(p);
            }
        std::sort(want.begin(), want.end(), [this](const ChunkPos& a, const ChunkPos& b) {
            const int da = (a.x - center.x) * (a.x - center.x) + (a.z - center.z) * (a.z - center.z);
            const int db = (b.x - center.x) * (b.x - center.x) + (b.z - center.z) * (b.z - center.z);
            return da < db;
        });
        int budget = kChunksPerTick;
        // ask the manager for a window ahead so generation overlaps sending
        int asked = 0;
        for (const ChunkPos& pos : want) {
            auto entry = server.Chunks().Get(pos);
            if (!entry) {
                if (++asked >= 64)
                    break;
                continue;
            }
            if (budget <= 0)
                break;
            const std::vector<uint8_t>& payload = server.Chunks().Payload(*entry);
            proto::LevelChunkPacket lc;
            lc.x = pos.x;
            lc.z = pos.z;
            lc.dimension = 0;
            lc.subChunkCount = static_cast<uint32_t>(kSections);
            lc.cacheEnabled = false;
            lc.payload = payload;
            Send(lc);
            sent.insert(pos);
            budget--;
        }
        // the client waits for terrain under its feet before it drops the loading screen
        if (!spawnSent && state == State::Loading) {
            size_t near = 0;
            for (const ChunkPos& p : sent)
                if (std::abs(p.x - center.x) <= 1 && std::abs(p.z - center.z) <= 1)
                    near++;
            if (near >= 9 || (radius < 2 && !sent.empty())) {
                proto::PlayStatusPacket spawn;
                spawn.status = proto::PlayStatusPacketStatus::PlayerSpawn;
                Send(spawn);
                spawnSent = true;
            }
        }
    }

    void Session::OnInitialized() {
        if (state != State::Loading)
            return;
        state = State::InGame;
        server.OnPlayerJoined(*this);
    }

    void Session::OnText(std::span<const uint8_t> body) {
        proto::TextPacket t;
        if (!Decode(body, t) || state != State::InGame)
            return;
        if (t.type != proto::TextPacketType::Chat || t.message.empty())
            return;
        std::string message = t.message;
        if (message.size() > 512)
            message.resize(512);
        server.HandleChat(*this, message);
    }

    void Session::OnCommand(std::span<const uint8_t> body) {
        proto::CommandRequestPacket c;
        if (!Decode(body, c) || state != State::InGame)
            return;
        server.RunPlayerCommand(*this, c.command);
    }

    void Session::SetPosition(double x, double y, double z, float p, float yawValue) {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
            return;
        // far outside any sane world: ignore rather than let a bad value drive chunk loading
        if (std::abs(x) > 3.0e7 || std::abs(z) > 3.0e7 || std::abs(y) > 1.0e5)
            return;
        moved = moved || x != posX || y != posY || z != posZ || p != pitch || yawValue != yaw;
        posX = x;
        posY = y;
        posZ = z;
        pitch = p;
        yaw = yawValue;
        if (state == State::InGame || state == State::Loading)
            PublishCenter(false);
    }

    void Session::OnAuthInput(std::span<const uint8_t> body) {
        proto::PlayerAuthInputPacket in;
        if (!Decode(body, in) || (state != State::InGame && state != State::Loading))
            return;
        SetPosition(in.position.x, in.position.y, in.position.z, in.pitch, in.yaw);
    }

    void Session::OnMovePlayer(std::span<const uint8_t> body) {
        proto::MovePlayerPacket m;
        if (!Decode(body, m) || (state != State::InGame && state != State::Loading))
            return;
        SetPosition(m.position.x, m.position.y, m.position.z, m.pitch, m.yaw);
    }

    void Session::Teleport(double x, double y, double z) {
        posX = x;
        posY = y;
        posZ = z;
        proto::MovePlayerPacket m;
        m.runtimeId = runtimeId;
        m.position = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
        m.pitch = pitch;
        m.yaw = yaw;
        m.headYaw = yaw;
        m.mode = proto::MovePlayerPacketMode::Teleport;
        m.teleport = proto::MovePlayerPacketTeleport{};
        Send(m);
        PublishCenter(false);
    }

    void Session::SendText(uint8_t type, const std::string& source, const std::string& message) {
        proto::TextPacket t;
        t.needsTranslation = false;
        t.category = type == static_cast<uint8_t>(proto::TextPacketType::Chat) ? proto::TextPacketCategory::Authored : proto::TextPacketCategory::MessageOnly;
        t.type = static_cast<proto::TextPacketType>(type);
        t.sourceName = source;
        t.message = message;
        Send(t);
    }

    void Session::ShowPlayer(const Session& other) {
        proto::PlayerListPacket list;
        proto::PlayerRecord rec;
        rec.type = proto::PlayerRecordType::Add;
        rec.uuid.bytes = other.uuid;
        rec.entityUniqueId = static_cast<int64_t>(other.runtimeId);
        rec.username = other.name;
        rec.xboxUserId = other.xuid;
        rec.skinData = other.GetSkin();
        rec.playerColor = -1;
        list.records.push_back(std::move(rec));
        Send(list);

        proto::AddPlayerPacket add;
        add.uuid.bytes = other.uuid;
        add.username = other.name;
        add.runtimeId = other.runtimeId;
        add.position = {static_cast<float>(other.posX), static_cast<float>(other.posY), static_cast<float>(other.posZ)};
        add.pitch = other.pitch;
        add.yaw = other.yaw;
        add.headYaw = other.yaw;
        add.gamemode = static_cast<proto::GameMode>(server.Config().gameMode);
        add.uniqueId = static_cast<int64_t>(other.runtimeId);
        add.permissionLevel = other.isOperator ? proto::PermissionLevel::Operator : proto::PermissionLevel::Member;
        add.commandPermission = other.isOperator ? proto::CommandPermissionLevel::Operator : proto::CommandPermissionLevel::Normal;
        Send(add);
    }

    void Session::HidePlayer(const Session& other) {
        proto::RemoveEntityPacket r;
        r.entityIdSelf = static_cast<int64_t>(other.runtimeId);
        Send(r);
        proto::PlayerListPacket list;
        proto::PlayerRecord rec;
        rec.type = proto::PlayerRecordType::Remove;
        rec.uuid.bytes = other.uuid;
        list.records.push_back(std::move(rec));
        Send(list);
    }

    void Session::SendMove(const Session& other, uint8_t mode) {
        proto::MovePlayerPacket m;
        m.runtimeId = other.runtimeId;
        m.position = {static_cast<float>(other.posX), static_cast<float>(other.posY), static_cast<float>(other.posZ)};
        m.pitch = other.pitch;
        m.yaw = other.yaw;
        m.headYaw = other.yaw;
        m.mode = static_cast<proto::MovePlayerPacketMode>(mode);
        Send(m);
    }

    void Session::SendBlockUpdate(int x, int y, int z, int32_t networkId) {
        proto::UpdateBlockPacket u;
        u.position = {x, y, z};
        u.blockRuntimeId = static_cast<uint32_t>(networkId);
        // neighbours + network: the standard flags for a server side change
        u.flags = 3;
        u.layer = 0;
        Send(u);
    }

    void Session::Tick(uint64_t) {
        if (state == State::Closed)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (!connection->Alive()) {
            Close();
            return;
        }
        if ((state == State::InGame && now - lastPacket > kIdleTimeout) ||
            (state != State::InGame && now - stateSince > kLoginTimeout)) {
            LogInfo((name.empty() ? connection->Describe() : name) + " timed out");
            Close();
            return;
        }
        if (state == State::Loading || state == State::InGame)
            StreamChunks();
        Flush();
    }

}
