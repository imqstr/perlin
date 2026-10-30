// full bedrock handshake against a live Server over the in-process loopback transport,
// with a scripted client built from the same generated packets the real client uses
#include "test_util.hpp"
#include "test_auth.hpp"

#include "core/Json.hpp"
#include "core/Server.hpp"
#include "net/Batch.hpp"

#include "protocol/Packets.hpp"
#include "world/BlockCatalog.hpp"
#include "world/Blocks.hpp"
#include "world/level/chunk/ChunkCodec.hpp"

#include <filesystem>
#include <fstream>

using namespace perlin::server;
namespace proto = perlin::proto2225;
namespace net = perlin::net;

namespace {
    std::string B64Url(const std::string& s) {
        static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        std::string out;
        for (size_t i = 0; i < s.size(); i += 3) {
            uint32_t v = static_cast<unsigned char>(s[i]) << 16;
            if (i + 1 < s.size()) v |= static_cast<unsigned char>(s[i + 1]) << 8;
            if (i + 2 < s.size()) v |= static_cast<unsigned char>(s[i + 2]);
            out += t[(v >> 18) & 63];
            out += t[(v >> 12) & 63];
            if (i + 1 < s.size()) out += t[(v >> 6) & 63];
            if (i + 2 < s.size()) out += t[v & 63];
        }
        return out;
    }

    std::string Jwt(const std::string& payload) { return B64Url("{\"alg\":\"none\"}") + "." + B64Url(payload) + ".sig"; }

    struct Received {
        uint32_t id;
        std::vector<uint8_t> body;
    };

    class Client {
    public:
        Client(Server& server, std::string name)
            : server(server), name(name), endpoint(server.Loopback().Connect()), account(name, "") {}

        template <class P> void Send(const P& packet) {
            std::vector<uint8_t> body;
            net::EncodeChecked(packet, body);
            std::vector<uint8_t> content;
            BatchCodec::AppendPacket(content, P::kId, body);
            const std::vector<uint8_t> wire = codec.Seal(content);
            endpoint->Send(wire.data(), wire.size());
        }

        // pump the server a few ticks and collect what arrived
        void Settle(int ticks = 3) {
            for (int i = 0; i < ticks; i++) {
                server.Tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            Drain();
        }

        void Drain() {
            std::vector<uint8_t> wire;
            while (endpoint->Receive(wire)) {
                std::vector<uint8_t> content;
                if (!codec.Open(wire, content)) {
                    corrupt = true;
                    continue;
                }
                BatchCodec::Split(content, [&](uint32_t id, std::span<const uint8_t> body) {
                    inbox.push_back({id, std::vector<uint8_t>(body.begin(), body.end())});
                    if (id == proto::NetworkSettingsPacket::kId)
                        codec.EnableCompression(256);
                });
            }
        }

        template <class P> std::vector<P> Take() {
            std::vector<P> out;
            for (auto it = inbox.begin(); it != inbox.end();) {
                if (it->id == P::kId) {
                    P p;
                    net::ReadStream s(it->body.data(), it->body.size());
                    if (Read(s, p) && s.ok)
                        out.push_back(std::move(p));
                    else
                        malformed++;
                    it = inbox.erase(it);
                } else {
                    ++it;
                }
            }
            return out;
        }

        void Login(int protocol = 2225) {
            proto::RequestNetworkSettingsPacket req;
            req.clientProtocol = protocol;
            Send(req);
            Settle();
            if (protocol != 2225)
                return;
            proto::LoginPacket login;
            login.protocolVersion = protocol;
            // an offline login: one self-signed link, real signatures
            login.tokens.identity = account.Identity(account.Chain(false), 2);
            login.tokens.client = account.ClientToken();
            Send(login);
            Settle();
        }

        Server& server;
        std::string name;
        std::shared_ptr<LoopbackListener::Endpoint> endpoint;
        testauth::Account account;
        BatchCodec codec;
        std::vector<Received> inbox;
        bool corrupt = false;
        int malformed = 0;
    };

    void Join(Client& c, int radius = 4) {
        c.Login();
        proto::ResourcePackClientResponsePacket have;
        have.responseStatus = proto::ResourcePackClientResponsePacketResponseStatus::HaveAllPacks;
        c.Send(have);
        c.Settle();
        proto::ResourcePackClientResponsePacket done;
        done.responseStatus = proto::ResourcePackClientResponsePacketResponseStatus::Completed;
        c.Send(done);
        c.Settle();
        proto::RequestChunkRadiusPacket r;
        r.chunkRadius = radius;
        r.maxRadius = static_cast<uint8_t>(radius);
        c.Send(r);
        // terrain is generated on a worker: give it a moment
        for (int i = 0; i < 60; i++)
            c.Settle(2);
    }
}

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    const fs::path plugins = fs::temp_directory_path() / "perlin_session_plugins";
    fs::remove_all(plugins);
    fs::create_directories(plugins);
    if (argc > 1)
        fs::copy_file(argv[1], plugins / "hello_server.wasm");

    ServerConfig config;
    config.loopbackOnly = false;
    config.nethernet = false;
    // PS_TEST_BACKEND=gpu runs the whole session on the gpu backend
    config.terrainBackend = std::getenv("PS_TEST_BACKEND") ? std::getenv("PS_TEST_BACKEND") : "cpu";
    config.pluginDir = plugins.string();
    config.viewDistance = 6;
    config.operators = {"Alice"};
    Server server(config);
    std::string error;
    CHECK(server.Start(error));
    if (!error.empty()) {
        std::fprintf(stderr, "start: %s\n", error.c_str());
        return 1;
    }

    // wrong protocol: told so, then dropped
    {
        Client old(server, "Old");
        old.Login(100);
        const auto status = old.Take<proto::PlayStatusPacket>();
        CHECK(status.size() == 1 && status[0].status == proto::PlayStatusPacketStatus::FailedClient);
    }

    Client alice(server, "Alice");
    alice.Login();
    CHECK(alice.Take<proto::NetworkSettingsPacket>().size() == 1);
    CHECK(alice.Take<proto::PlayStatusPacket>().size() == 1);
    CHECK(alice.Take<proto::ResourcePacksInfoPacket>().size() == 1);
    CHECK(!alice.corrupt && alice.malformed == 0);

    proto::ResourcePackClientResponsePacket have;
    have.responseStatus = proto::ResourcePackClientResponsePacketResponseStatus::HaveAllPacks;
    alice.Send(have);
    alice.Settle();
    CHECK(alice.Take<proto::ResourcePackStackPacket>().size() == 1);

    proto::ResourcePackClientResponsePacket done;
    done.responseStatus = proto::ResourcePackClientResponsePacketResponseStatus::Completed;
    alice.Send(done);
    alice.Settle();
    const auto start = alice.Take<proto::StartGamePacket>();
    CHECK(start.size() == 1);
    if (!start.empty()) {
        CHECK(start[0].blockNetworkIdsAreHashes);
        CHECK(start[0].permissionLevel == proto::PermissionLevel::Operator);
        CHECK(start[0].playerPosition.y > -61.0f && start[0].playerPosition.y < -58.0f);
    }
    CHECK(alice.Take<proto::AvailableCommandsPacket>().size() == 1);

    proto::RequestChunkRadiusPacket req;
    req.chunkRadius = 4;
    req.maxRadius = 4;
    alice.Send(req);
    for (int i = 0; i < 60; i++)
        alice.Settle(2);
    const auto radius = alice.Take<proto::ChunkRadiusUpdatePacket>();
    CHECK(radius.size() == 1 && radius[0].chunkRadius == 4);
    CHECK(alice.Take<proto::NetworkChunkPublisherUpdatePacket>().size() >= 1);
    const auto chunks = alice.Take<proto::LevelChunkPacket>();
    // radius 4 is a disc of 49 columns
    CHECK(chunks.size() == 49);
    for (const auto& c : chunks) {
        std::vector<std::pair<int, perlin::SubChunkData>> subs;
        size_t used = 0;
        CHECK(perlin::ChunkCodec::ParseColumn(c.payload.data(), c.payload.size(), c.subChunkCount, -4, 24, subs, used));
        CHECK(subs.size() == 24);
    }
    const auto spawn = alice.Take<proto::PlayStatusPacket>();
    CHECK(!spawn.empty() && spawn.back().status == proto::PlayStatusPacketStatus::PlayerSpawn);

    // joining
    proto::SetLocalPlayerAsInitializedPacket init;
    alice.Send(init);
    alice.Settle();
    auto texts = alice.Take<proto::TextPacket>();
    bool welcomed = false, joined = false;
    for (const auto& t : texts) {
        welcomed = welcomed || t.message == "Welcome, Alice!";
        joined = joined || t.message.find("Alice joined") != std::string::npos;
    }
    CHECK(joined);
    CHECK(welcomed == (argc > 1));
    CHECK(server.InGamePlayers().size() == 1);

    // a second player: each sees the other
    Client bob(server, "Bob");
    Join(bob);
    proto::SetLocalPlayerAsInitializedPacket init2;
    bob.Send(init2);
    bob.Settle();
    alice.Settle();
    CHECK(server.InGamePlayers().size() == 2);
    const auto seenByAlice = alice.Take<proto::AddPlayerPacket>();
    CHECK(seenByAlice.size() == 1 && seenByAlice[0].username == "Bob");
    const auto seenByBob = bob.Take<proto::AddPlayerPacket>();
    CHECK(seenByBob.size() == 1 && seenByBob[0].username == "Alice");
    CHECK(alice.Take<proto::PlayerListPacket>().size() >= 1);
    alice.inbox.clear();
    bob.inbox.clear();

    // chat reaches both with the sender's name
    proto::TextPacket say;
    say.type = proto::TextPacketType::Chat;
    say.sourceName = "Alice";
    say.message = "hi bob";
    alice.Send(say);
    alice.Settle();
    bob.Settle();
    auto aliceChat = alice.Take<proto::TextPacket>();
    auto bobChat = bob.Take<proto::TextPacket>();
    CHECK(aliceChat.size() == 1 && bobChat.size() == 1);
    if (!bobChat.empty())
        CHECK(bobChat[0].sourceName == "Alice" && bobChat[0].message == "hi bob" && bobChat[0].type == proto::TextPacketType::Chat);

    // the example plugin swallows one word
    if (argc > 1) {
        proto::TextPacket bad;
        bad.type = proto::TextPacketType::Chat;
        bad.message = "a CREEPER!";
        bob.Send(bad);
        bob.Settle();
        alice.Settle();
        CHECK(alice.Take<proto::TextPacket>().empty());
        const auto reply = bob.Take<proto::TextPacket>();
        CHECK(reply.size() == 1 && reply[0].message.find("do not say that word") != std::string::npos);
    }

    // commands: /list for anyone, /say refused for a non operator, allowed for Alice
    proto::CommandRequestPacket cmd;
    cmd.command = "/list";
    bob.Send(cmd);
    bob.Settle();
    auto listReply = bob.Take<proto::TextPacket>();
    CHECK(listReply.size() == 1 && listReply[0].message.find("2/20 online") != std::string::npos);
    cmd.command = "/say nope";
    bob.Send(cmd);
    bob.Settle();
    listReply = bob.Take<proto::TextPacket>();
    CHECK(listReply.size() == 1 && listReply[0].message.find("permission") != std::string::npos);
    cmd.command = "/say server notice";
    alice.Send(cmd);
    alice.Settle();
    bob.Settle();
    bool notice = false;
    for (const auto& t : bob.Take<proto::TextPacket>())
        notice = notice || t.message.find("server notice") != std::string::npos;
    CHECK(notice);
    if (argc > 1) {
        cmd.command = "/hello";
        bob.Send(cmd);
        bob.Settle();
        const auto hello = bob.Take<proto::TextPacket>();
        CHECK(hello.size() == 1 && hello[0].message == "Hello! Players online: 2");
    }

    // moving is relayed
    alice.inbox.clear();
    bob.inbox.clear();
    proto::PlayerAuthInputPacket move;
    move.position = {40.5f, -58.38f, 3.5f};
    alice.Send(move);
    alice.Settle();
    bob.Settle();
    const auto moves = bob.Take<proto::MovePlayerPacket>();
    CHECK(!moves.empty() && moves.back().position.x == 40.5f);

    // plugin api edits reach the world and the players who have the chunk
    CHECK(server.SetBlock(1, -60, 1, "stone") == 0);
    bob.Settle();
    CHECK(bob.Take<proto::UpdateBlockPacket>().size() == 1);
    std::string block;
    CHECK(server.GetBlock(1, -60, 1, block) == 0 && block == "minecraft:stone");

    // ---- building: creative menu, placing, breaking, picking
    {
        using T = proto::ItemStackRequestActionsEntryTypeId;
        using C = proto::ContainerSlotType;
        const CatalogItem* stone = BlockCatalog::Instance().ItemByName("minecraft:stone");
        CHECK(stone != nullptr);
        // the item and creative lists reach the client
        CHECK(!start.empty());
        // back next to the build site: reach is checked against where the server last saw her
        proto::PlayerAuthInputPacket home;
        home.position = {2.5f, -58.38f, 2.5f};
        alice.Send(home);
        alice.Settle();
        alice.inbox.clear();
        bob.inbox.clear();

        proto::ItemStackRequestPacket req;
        proto::ItemStackRequest r;
        r.requestId = -1;
        proto::ItemStackRequestActionsEntry craft;
        craft.typeId = T::CraftCreative;
        craft.itemId = static_cast<uint32_t>(stone->networkId);
        craft.timesCrafted = 1;
        proto::ItemStackRequestActionsEntry take;
        take.typeId = T::Take;
        take.count = 64;
        take.source.slotType.containerId = C::CreativeOutput;
        take.source.slot = 50;
        take.destination.slotType.containerId = C::Cursor;
        proto::ItemStackRequestActionsEntry put;
        put.typeId = T::Place;
        put.count = 64;
        put.source.slotType.containerId = C::Cursor;
        put.destination.slotType.containerId = C::HotbarAndInventory;
        put.destination.slot = 0;
        r.actions = {craft, take, put};
        req.requests.push_back(r);
        alice.Send(req);
        alice.Settle();
        const auto responses = alice.Take<proto::ItemStackResponsePacket>();
        CHECK(responses.size() == 1 && responses[0].responses.size() == 1);
        if (!responses.empty() && !responses[0].responses.empty()) {
            const auto& e = responses[0].responses[0];
            CHECK(e.status == proto::ItemStackResponsesEntryStatus::Ok && e.requestId == -1);
            CHECK(e.containers && !e.containers->empty());
        }

        // place on top of the grass block at the origin column: face 1 is up
        const int32_t stoneHash = stone->blockHash;
        proto::InventoryTransactionPacket place;
        place.transaction.transactionType = proto::TransactionTransactionType::ItemUse;
        proto::TransactionUseItem use;
        use.actionType = proto::TransactionUseItemActionType::ClickBlock;
        use.blockPosition = {2, -61, 2};
        use.face = 1;
        use.hotbarSlot = 0;
        place.transaction.transactionData = use;
        alice.Send(place);
        alice.Settle();
        bob.Settle();
        std::string name;
        CHECK(server.GetBlock(2, -60, 2, name) == 0 && name == "minecraft:stone");
        const auto updates = bob.Take<proto::UpdateBlockPacket>();
        CHECK(updates.size() == 1 && updates[0].position.y == -60 && static_cast<int32_t>(updates[0].blockRuntimeId) == stoneHash);
        CHECK(alice.Take<proto::UpdateBlockPacket>().size() == 1);

        // placing into an occupied cell is refused and the client is corrected
        alice.Send(place);
        alice.Settle();
        bob.Settle();
        CHECK(bob.Take<proto::UpdateBlockPacket>().empty());
        CHECK(alice.Take<proto::UpdateBlockPacket>().size() >= 1);

        // too far away: refused
        proto::InventoryTransactionPacket far = place;
        proto::TransactionUseItem farUse = use;
        farUse.blockPosition = {120, -61, 120};
        far.transaction.transactionData = farUse;
        alice.Send(far);
        alice.Settle();
        CHECK(server.GetBlock(120, -60, 120, name) != 0 || name == "minecraft:air");

        // break it again with the creative start-break action
        proto::PlayerActionPacket dig;
        dig.runtimeEntityId = 1;
        dig.action = proto::Action::StartBreak;
        dig.position = {2, -60, 2};
        alice.inbox.clear();
        bob.inbox.clear();
        alice.Send(dig);
        alice.Settle();
        bob.Settle();
        CHECK(server.GetBlock(2, -60, 2, name) == 0 && name == "minecraft:air");
        CHECK(bob.Take<proto::UpdateBlockPacket>().size() == 1);
        // the break transaction that follows in the same click changes nothing more
        proto::InventoryTransactionPacket brk;
        brk.transaction.transactionType = proto::TransactionTransactionType::ItemUse;
        proto::TransactionUseItem bu;
        bu.actionType = proto::TransactionUseItemActionType::BreakBlock;
        bu.blockPosition = {2, -60, 2};
        brk.transaction.transactionData = bu;
        alice.Send(brk);
        alice.Settle();

        // pick block copies the block into the hotbar
        proto::BlockPickRequestPacket pick;
        pick.x = 3;
        pick.y = -61;
        pick.z = 3;
        pick.selectedSlot = 0;
        alice.inbox.clear();
        alice.Send(pick);
        alice.Settle();
        CHECK(!alice.Take<proto::MobEquipmentPacket>().empty());

        // the plugin api can hand out items
        CHECK(server.GiveItem(server.FindByName("Alice")->Id(), "dirt", 5) == 0);
        alice.Settle();
        CHECK(alice.Take<proto::InventoryContentPacket>().size() >= 1);
    }


    // leaving removes the player for the others
    bob.endpoint->Close();
    alice.Settle();
    CHECK(alice.Take<proto::RemoveEntityPacket>().size() == 1);
    CHECK(server.InGamePlayers().size() == 1);


    server.Stop();

    // ---- survival: placing spends the item, breaking pays it into the inventory, no creative menu
    {
        ServerConfig survivalConfig = config;
        survivalConfig.gameMode = 0;
        survivalConfig.pluginDir.clear();
        survivalConfig.operators = {};
        Server survival(survivalConfig);
        std::string e2;
        CHECK(survival.Start(e2));
        Client carol(survival, "Carol");
        Join(carol);
        proto::SetLocalPlayerAsInitializedPacket initCarol;
        carol.Send(initCarol);
        carol.Settle();
        CHECK(survival.GiveItem(1, "stone", 2) == 0);
        carol.Settle();
        carol.inbox.clear();
        proto::InventoryTransactionPacket place;
        place.transaction.transactionType = proto::TransactionTransactionType::ItemUse;
        proto::TransactionUseItem use;
        use.actionType = proto::TransactionUseItemActionType::ClickBlock;
        use.blockPosition = {1, -61, 1};
        use.face = 1;
        place.transaction.transactionData = use;
        carol.Send(place);
        carol.Settle();
        std::string n;
        CHECK(survival.GetBlock(1, -60, 1, n) == 0 && n == "minecraft:stone");
        carol.Send(place);
        carol.Settle();
        // second attempt: the cell is taken, nothing is spent
        proto::PlayerActionPacket stop;
        stop.action = proto::Action::StopBreak;
        stop.position = {1, -60, 1};
        carol.Send(stop);
        carol.Settle();
        CHECK(survival.GetBlock(1, -60, 1, n) == 0 && n == "minecraft:air");
        carol.inbox.clear();
        // bedrock does not break in survival
        stop.position = {1, -64, 1};
        carol.Send(stop);
        carol.Settle();
        CHECK(survival.GetBlock(1, -64, 1, n) == 0 && n == "minecraft:bedrock");
        survival.Stop();
    }
    fs::remove_all(plugins);
    if (g_failed) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failed);
        return 1;
    }
    std::puts("server_session_test ok");
    return 0;
}
