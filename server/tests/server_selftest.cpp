// units: json, block hashes, chunk encoding (decoded with the client's own codec), batches, config, plugin host
#include "test_util.hpp"
#include "test_auth.hpp"
#include "core/Auth.hpp"

#include "core/Config.hpp"
#include "core/Json.hpp"
#include "net/Batch.hpp"
#include "net/Connection.hpp"
#include "plugin/PluginHost.hpp"
#include "inventory/Inventory.hpp"
#include "world/BlockCatalog.hpp"
#include "world/Blocks.hpp"
#include "world/Chunk.hpp"
#include "world/Generator.hpp"

#include "world/level/chunk/ChunkCodec.hpp"
#include "protocol/Packets.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>

using namespace perlin::server;

static void TestJson() {
    json::Value v;
    CHECK(json::Parse(R"({"a":[1,2,{"b":"xé\n"}],"t":true,"n":null,"f":-1.5e1})", v));
    CHECK(v["a"][2]["b"].String() == "x\xc3\xa9\n");
    CHECK(v["t"].Bool() && v["n"].IsNull() && v["f"].Number() == -15.0);
    CHECK(v["missing"]["deeper"][3].IsNull());
    CHECK(!json::Parse("{\"a\":", v));
    CHECK(!json::Parse("[1,2] x", v));
    // depth guard: a hostile login token must not blow the stack
    CHECK(!json::Parse(std::string(500, '[') + std::string(500, ']'), v));
    // header.payload.signature with an unpadded base64url payload
    json::Value claims;
    CHECK(json::DecodeJwtPayload("e30.eyJleHRyYURhdGEiOnsiZGlzcGxheU5hbWUiOiJBbGljZSJ9fQ.sig", claims));
    CHECK(claims["extraData"]["displayName"].String() == "Alice");
    CHECK(json::Escape("a\"b\\c\n") == "a\\\"b\\\\c\\n");
}

static void TestBlockHash() {
    // the well known network id of air
    CHECK(BlockNetworkHash("minecraft:air", {}) == -604749536);
    BlockRegistry& r = BlockRegistry::Instance();
    const uint16_t dirt = r.Intern("minecraft:dirt");
    CHECK(dirt != 0 && r.Intern("minecraft:dirt") == dirt);
    CHECK(r.Name(dirt) == "minecraft:dirt" && r.Hash(dirt) == BlockNetworkHash("minecraft:dirt", {}));
    // state order must not matter: the registry sorts before hashing
    const uint16_t a = r.Intern("minecraft:log", {{"pillar_axis", std::string("y")}, {"stripped_bit", false}});
    const uint16_t b = r.Intern("minecraft:log", {{"stripped_bit", false}, {"pillar_axis", std::string("y")}});
    CHECK(a == b);
    CHECK(r.Find("minecraft:nope") == 0);
}

static ServerConfig FlatConfig() {
    ServerConfig c;
    c.terrainBackend = "cpu";
    return c;
}

static void TestFlatGeneration() {
    ServerConfig c = FlatConfig();
    FlatCpuBackend gen(c);
    std::vector<ChunkPos> pos = {{0, 0}, {5, -7}};
    std::vector<ChunkColumn> cols(pos.size());
    gen.Generate(pos, cols);
    BlockRegistry& r = BlockRegistry::Instance();
    for (const ChunkColumn& col : cols) {
        CHECK(col.Get(3, -64, 9) == r.Find("minecraft:bedrock"));
        CHECK(col.Get(0, -63, 0) == r.Find("minecraft:dirt"));
        CHECK(col.Get(15, -62, 15) == r.Find("minecraft:dirt"));
        CHECK(col.Get(7, -61, 7) == r.Find("minecraft:grass_block"));
        CHECK(col.Get(7, -60, 7) == 0);
        CHECK(col.Get(7, 319, 7) == 0);
        // above the layers every section is one uniform block: no per-cell storage
        CHECK(col.IsUniform(5) && col.IsUniform(23));
        CHECK(!col.IsUniform(0));
    }
    CHECK(FlatSurfaceY(c) == -60);
}

static void TestChunkEncodingDecodesWithClientCodec() {
    ServerConfig c = FlatConfig();
    FlatCpuBackend gen(c);
    std::vector<ChunkPos> pos = {{0, 0}};
    std::vector<ChunkColumn> cols(1);
    gen.Generate(pos, cols);
    cols[0].Set(2, -61, 3, BlockRegistry::Instance().Intern("minecraft:stone"));
    std::vector<uint8_t> payload;
    cols[0].EncodeNetwork(payload);

    std::vector<std::pair<int, perlin::SubChunkData>> subs;
    size_t used = 0;
    const char* why = nullptr;
    const bool ok = perlin::ChunkCodec::ParseColumn(payload.data(), payload.size(), kSections, kMinY >> 4, kSections, subs, used, &why);
    CHECK(ok);
    if (!ok) {
        std::fprintf(stderr, "ParseColumn: %s\n", why ? why : "?");
        return;
    }
    CHECK(subs.size() == static_cast<size_t>(kSections));
    // cells decode back to the hashes the registry hands out, in wire order
    BlockRegistry& r = BlockRegistry::Instance();
    for (const auto& [y, sc] : subs) {
        CHECK(sc.storages.size() == 1);
        perlin::ChunkCodec::CellIds cells;
        CHECK(perlin::ChunkCodec::Expand(sc.storages[0], cells));
        if (y == kMinY >> 4) {
            // section 0 holds y -64..-49
            CHECK(cells[perlin::server::CellIndex(2, 3, 3)] == r.Hash(r.Find("minecraft:stone")));
            CHECK(cells[perlin::server::CellIndex(0, 0, 0)] == r.Hash(r.Find("minecraft:bedrock")));
            CHECK(cells[perlin::server::CellIndex(8, 15, 8)] == r.Hash(0));
        }
    }
    CHECK(used <= payload.size());
}

static void TestBatchCodec() {
    BatchCodec plain, comp;
    comp.EnableCompression(64);
    std::vector<uint8_t> content;
    const std::vector<uint8_t> small = {1, 2, 3};
    const std::vector<uint8_t> big(5000, 7);
    BatchCodec::AppendPacket(content, 9, small);
    BatchCodec::AppendPacket(content, 58, big);

    for (BatchCodec* codec : {&plain, &comp}) {
        const std::vector<uint8_t> wire = codec->Seal(content);
        std::vector<uint8_t> back;
        CHECK(codec->Open(wire, back));
        CHECK(back == content);
        std::vector<std::pair<uint32_t, size_t>> seen;
        CHECK(BatchCodec::Split(back, [&](uint32_t id, std::span<const uint8_t> body) { seen.emplace_back(id, body.size()); }));
        const std::pair<uint32_t, size_t> first{9, 3}, second{58, 5000};
        CHECK(seen.size() == 2 && seen[0] == first && seen[1] == second);
    }
    // 5000 equal bytes must shrink; a short batch below the threshold goes out with the "none" marker
    CHECK(comp.Seal(content).size() < 200 && comp.Seal(content)[0] == 0x00);
    std::vector<uint8_t> tiny;
    BatchCodec::AppendPacket(tiny, 9, small);
    CHECK(comp.Seal(tiny)[0] == 0xFF);

    std::vector<uint8_t> junk;
    CHECK(!comp.Open(std::vector<uint8_t>{0x00, 1, 2, 3, 4}, junk));
    CHECK(!comp.Open(std::vector<uint8_t>{0x42, 1}, junk));
    CHECK(!BatchCodec::Split(std::vector<uint8_t>{0x05, 0x01}, [](uint32_t, std::span<const uint8_t>) {}));
}

static void TestFragments() {
    const std::vector<uint8_t> data(kFragmentSize * 2 + 10, 0xAB);
    const auto parts = SplitFragments(data.data(), data.size());
    CHECK(parts.size() == 3 && parts[0][0] == 2 && parts[1][0] == 1 && parts[2][0] == 0);
    FragmentAssembler asm_;
    std::vector<uint8_t> out;
    CHECK(!asm_.Push(parts[0].data(), parts[0].size(), out));
    CHECK(!asm_.Push(parts[1].data(), parts[1].size(), out));
    CHECK(asm_.Push(parts[2].data(), parts[2].size(), out));
    CHECK(out == data);
    CHECK(SplitFragments(data.data(), 0).empty());
}

static void TestConfig() {
    const std::string path = (std::filesystem::temp_directory_path() / "perlin_server_cfg.json").string();
    ServerConfig c;
    c.name = "Test \"quoted\"";
    c.port = 24000;
    c.operators = {"Alice", "Bob"};
    c.flatLayers = {{"minecraft:bedrock", 2}, {"minecraft:stone", 5}};
    CHECK(c.Save(path));
    std::string error;
    const ServerConfig d = ServerConfig::Load(path, &error);
    CHECK(error.empty() && d.name == c.name && d.port == 24000 && d.operators.size() == 2);
    CHECK(d.flatLayers.size() == 2 && d.flatLayers[1].block == "minecraft:stone" && d.flatLayers[1].count == 5);
    std::filesystem::remove(path);
    // a broken file is reported and the defaults stay
    std::ofstream(path) << "{ not json";
    const ServerConfig e = ServerConfig::Load(path, &error);
    CHECK(!error.empty() && e.port == ServerConfig{}.port);
    std::filesystem::remove(path);
}

struct MockApi final : PluginApi {
    std::vector<std::string> broadcasts, messages, commands;
    void Broadcast(const std::string& t) override { broadcasts.push_back(t); }
    bool Message(uint64_t p, const std::string& t) override { messages.push_back(std::to_string(p) + ":" + t); return true; }
    bool Kick(uint64_t, const std::string&) override { return true; }
    void RegisterCommand(const std::string& plugin, const std::string& name, const std::string&) override { commands.push_back(plugin + "/" + name); }
    std::vector<PlayerInfo> Players() override { return {{7, "Alice", 1, 2, 3}, {8, "Bob", 4, 5, 6}}; }
    bool Teleport(uint64_t, double, double, double) override { return true; }
    int SetBlock(int, int, int, const std::string&) override { return 0; }
    int GetBlock(int, int, int, std::string& b) override { b = "minecraft:stone"; return 0; }
    Status GetStatus() override { return {"mock", 2, 10, 99, "cpu"}; }
    int GiveItem(uint64_t, const std::string&, int) override { return 0; }
};

static void TestPluginHost(const char* wasm) {
    if (!wasm) {
        std::fprintf(stderr, "skip plugin test: no wasm path given\n");
        return;
    }
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "perlin_server_plugins";
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::copy_file(wasm, dir / "hello_server.wasm");
    // a file that is not a plugin must not stop the others from loading
    std::ofstream(dir / "broken.wasm") << "not wasm";

    MockApi api;
    PluginHost host(api);
    host.LoadDirectory(dir.string());
    const auto list = host.List();
    CHECK(list.size() == 1 && list[0].name == "hello_server" && list[0].enabled);
    CHECK(api.commands.size() == 1 && api.commands[0] == "hello_server/hello");

    PayloadWriter join;
    join.U64(7);
    join.Str("Alice");
    host.Dispatch(PS_EVENT_PLAYER_JOIN, join.bytes);
    CHECK(api.messages.size() == 1 && api.messages[0] == "7:Welcome, Alice!");

    PayloadWriter chat;
    chat.U64(7);
    chat.Str("Alice");
    chat.Str("look a Creeper!");
    CHECK((host.Dispatch(PS_EVENT_CHAT, chat.bytes) & 1u) == 1u);
    PayloadWriter fine;
    fine.U64(7);
    fine.Str("Alice");
    fine.Str("hello everyone");
    CHECK((host.Dispatch(PS_EVENT_CHAT, fine.bytes) & 1u) == 0u);

    api.messages.clear();
    CHECK(host.DispatchCommand("hello_server", 7, "hello", ""));
    CHECK(api.messages.size() == 1 && api.messages[0] == "7:Hello! Players online: 2");
    CHECK(!host.AnyoneListens(PS_EVENT_TICK));
    CHECK(host.AnyoneListens(PS_EVENT_CHAT));
    host.UnloadAll();
    fs::remove_all(dir);
}

static void TestBlockCatalog() {
    BlockCatalog& c = BlockCatalog::Instance();
    const std::string states = BlockCatalog::FindFile("", "block_states.json");
    const std::string names = BlockCatalog::FindFile("", "block_destroy.tsv");
    CHECK(!states.empty() && !names.empty());
    std::string error;
    CHECK(c.Load(states, names, error));
    CHECK(c.StateCount() > 20000 && c.Items().size() > 300);
    // the whole file is the oracle for the hash: a name and its states must give the id the client expects
    json::Value root;
    std::ifstream in(states);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(json::Parse(text, root));
    size_t bad = 0;
    for (const json::Value& e : root.items) {
        StateList list;
        for (const auto& [k, v] : e[2].members) {
            const std::string t = v[0].String();
            if (t == "b") list.emplace_back(k, StateValue(v[1].Int() != 0));
            else if (t == "i") list.emplace_back(k, StateValue(static_cast<int32_t>(v[1].Int())));
            else list.emplace_back(k, StateValue(v[1].String()));
        }
        if (BlockNetworkHash(e[1].String(), list) != static_cast<int32_t>(e[0].Int()))
            bad++;
    }
    CHECK(bad == 0);
    const CatalogItem* stone = c.ItemByName("minecraft:stone");
    CHECK(stone && c.ItemById(stone->networkId) == stone && c.ItemByBlockHash(stone->blockHash) == stone);
    // a stateful block resolves through its hash to the right registry entry
    const CatalogItem* log = c.ItemByName("minecraft:oak_log");
    CHECK(log && c.RegistryId(log->blockHash) != 0);
    CHECK(c.RegistryId(12345) == 0);
}

static perlin::proto2225::ItemStackRequestActionsEntry Act(perlin::proto2225::ItemStackRequestActionsEntryTypeId type, perlin::proto2225::ContainerSlotType from, int fromSlot,
                                                          perlin::proto2225::ContainerSlotType to, int toSlot, int count) {
    perlin::proto2225::ItemStackRequestActionsEntry a;
    a.typeId = type;
    a.count = static_cast<uint8_t>(count);
    a.source.slotType.containerId = from;
    a.source.slot = static_cast<uint8_t>(fromSlot);
    a.destination.slotType.containerId = to;
    a.destination.slot = static_cast<uint8_t>(toSlot);
    return a;
}

static void TestInventory() {
    namespace p = perlin::proto2225;
    using T = p::ItemStackRequestActionsEntryTypeId;
    using C = p::ContainerSlotType;
    const CatalogItem* stone = BlockCatalog::Instance().ItemByName("minecraft:stone");
    const CatalogItem* dirt = BlockCatalog::Instance().ItemByName("minecraft:dirt");
    CHECK(stone && dirt);
    PlayerInventory inv;

    // creative menu: craft, take to the cursor, put into hotbar slot 2
    p::ItemStackRequest r;
    r.requestId = -1;
    auto craft = Act(T::CraftCreative, C::HotbarAndInventory, 0, C::HotbarAndInventory, 0, 0);
    craft.itemId = static_cast<uint32_t>(stone->networkId);
    r.actions = {craft, Act(T::Take, C::CreativeOutput, 50, C::Cursor, 0, 64), Act(T::Place, C::Cursor, 0, C::HotbarAndInventory, 2, 40)};
    RequestResult res = ApplyStackRequest(inv, r, true);
    CHECK(res.ok);
    CHECK(inv.main[2].networkId == stone->networkId && inv.main[2].count == 40);
    CHECK(inv.cursor.count == 24 && inv.output.Empty());
    CHECK(inv.main[2].stackId > 0);
    const auto response = MakeResponse(inv, r.requestId, res);
    CHECK(response.status == p::ItemStackResponsesEntryStatus::Ok && response.containers && response.containers->size() == 2);

    // survival menu has no creative craft
    PlayerInventory survival;
    CHECK(!ApplyStackRequest(survival, r, false).ok);

    // a failing action rolls the whole request back
    PlayerInventory before = inv;
    p::ItemStackRequest bad;
    bad.requestId = -3;
    bad.actions = {Act(T::Place, C::Cursor, 0, C::HotbarAndInventory, 5, 10), Act(T::Take, C::HotbarAndInventory, 30, C::Cursor, 0, 1)};
    CHECK(!ApplyStackRequest(inv, bad, true).ok);
    CHECK(inv.cursor.count == before.cursor.count && inv.main[5].Empty());

    // a stale stack id is refused, a quoted negative (request local) one is accepted
    p::ItemStackRequest stale;
    stale.requestId = -5;
    auto move = Act(T::Place, C::HotbarAndInventory, 2, C::HotbarAndInventory, 3, 5);
    move.source.stackId = inv.main[2].stackId + 100;
    stale.actions = {move};
    CHECK(!ApplyStackRequest(inv, stale, true).ok);
    move.source.stackId = inv.main[2].stackId;
    stale.actions = {move};
    CHECK(ApplyStackRequest(inv, stale, true).ok && inv.main[3].count == 5 && inv.main[2].count == 35);

    // swap with a different item, destroy, then limits
    inv.main[4] = inv.MakeStack(dirt->networkId, 3);
    p::ItemStackRequest swap;
    swap.requestId = -7;
    swap.actions = {Act(T::Swap, C::HotbarAndInventory, 3, C::HotbarAndInventory, 4, 0)};
    CHECK(ApplyStackRequest(inv, swap, true).ok && inv.main[3].networkId == dirt->networkId && inv.main[4].networkId == stone->networkId);
    inv.main[4].count = 60;
    p::ItemStackRequest merge;
    merge.requestId = -9;
    merge.actions = {Act(T::Place, C::HotbarAndInventory, 2, C::HotbarAndInventory, 4, 30)};
    CHECK(!ApplyStackRequest(inv, merge, true).ok);
    p::ItemStackRequest destroy;
    destroy.requestId = -11;
    destroy.actions = {Act(T::Destroy, C::Cursor, 0, C::Cursor, 0, 24)};
    CHECK(ApplyStackRequest(inv, destroy, true).ok && inv.cursor.Empty());

    // Give fills partial stacks first and reports what did not fit
    PlayerInventory full;
    CHECK(full.Give(stone->networkId, 100) == 0);
    CHECK(full.main[0].count == 64 && full.main[1].count == 36);
    CHECK(full.Give(stone->networkId, 64 * 36) == 100);
    full.selected = 0;
    CHECK(full.TakeHeld(64) && full.main[0].Empty() && !full.TakeHeld(1));
}

static void TestAuth() {
    using namespace perlin::server;
    // the built-in root keys must at least be valid p-384 public keys: a typo in the base64 would not be
    for (const std::string& k : auth::DefaultRootKeys()) {
        const std::string der = json::Base64UrlDecode(k);
        const unsigned char* p = reinterpret_cast<const unsigned char*>(der.data());
        EVP_PKEY* key = d2i_PUBKEY(nullptr, &p, static_cast<long>(der.size()));
        CHECK(key != nullptr);
        EVP_PKEY_free(key);
    }

    testauth::Account alice("Alice", "2535400000000001");
    const std::vector<std::string> roots = {alice.root.spki};
    auto login = auth::VerifyLogin(alice.Identity(alice.Chain()), alice.ClientToken(), roots);
    CHECK(login.chainValid && login.xboxVerified && login.name == "Alice" && login.xuid == "2535400000000001" && login.clientKey == alice.player.spki);
    CHECK(login.clientDataValid && login.clientData["SkinId"].String() == "s");

    // a different root: the chain is still well formed, it is just not Xbox
    testauth::Key stranger;
    login = auth::VerifyLogin(alice.Identity(alice.Chain()), alice.ClientToken(), {stranger.spki});
    CHECK(login.chainValid && !login.xboxVerified && login.xuid.empty());

    // self signed (offline) login: valid chain, never verified, no xuid
    login = auth::VerifyLogin(alice.Identity(alice.Chain(false), 2), alice.ClientToken(), roots);
    CHECK(login.chainValid && !login.xboxVerified && login.name == "Alice" && login.xuid.empty());

    // an xuid written into the self signed link ahead of the root's signature is not believed
    {
        testauth::Account mallory("Mallory", "999");
        const std::string lie = "{\"displayName\":\"Victim\",\"XUID\":\"42\",\"identity\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\"}";
        std::vector<std::string> chain = {testauth::Link(mallory.player, mallory.root, lie), testauth::Link(mallory.root, mallory.intermediate),
                                          testauth::Link(mallory.intermediate, mallory.player)};
        login = auth::VerifyLogin(mallory.Identity(chain), mallory.ClientToken(), {mallory.root.spki});
        CHECK(login.chainValid && !login.xboxVerified && login.xuid != "42");
    }

    // tampering: a changed payload, links out of order, a link signed by the wrong key
    {
        std::vector<std::string> chain = alice.Chain();
        std::string forged = chain[2];
        forged[forged.find('.') + 5] ^= 1;
        CHECK(!auth::VerifyLogin(alice.Identity({chain[0], chain[1], forged}), alice.ClientToken(), roots).chainValid);
        CHECK(!auth::VerifyLogin(alice.Identity({chain[0], chain[2], chain[1]}), alice.ClientToken(), roots).chainValid);
        testauth::Key other;
        CHECK(!auth::VerifyLogin(alice.Identity({chain[0], chain[1], testauth::Link(other, alice.player, "{\"displayName\":\"X\",\"XUID\":\"1\"}")}),
                                 alice.ClientToken(), roots).chainValid);
        // client data signed by some other key
        testauth::Key thief;
        const std::string badClient = testauth::Jwt(thief, "{\"ThirdPartyName\":\"Alice\"}");
        CHECK(!auth::VerifyLogin(alice.Identity(chain), badClient, roots).chainValid);
        CHECK(!auth::VerifyLogin("not json", alice.ClientToken(), roots).chainValid);
        CHECK(!auth::VerifyLogin(alice.Identity({}), alice.ClientToken(), roots).chainValid);
    }

    // expired link
    {
        const int64_t now = testauth::Now();
        const std::string old = testauth::Jwt(alice.player, "{\"identityPublicKey\":\"" + alice.player.spki + "\",\"exp\":" + std::to_string(now - 7200) + "}");
        CHECK(!auth::VerifyLogin(alice.Identity({old}), alice.ClientToken(), roots).chainValid);
    }

    // the offer binding: fingerprints signed by the player's key
    const std::string sdp = "v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=fingerprint:sha-256 AB:CD:EF:01\r\nm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n";
    const std::string offer = testauth::AssertOffer(sdp, alice.player);
    std::string why;
    CHECK(auth::VerifyOfferBinding(offer, alice.player.spki, why));
    testauth::Key notHer;
    CHECK(!auth::VerifyOfferBinding(offer, notHer.spki, why));
    // the same assertion pasted onto a connection with other fingerprints
    std::string moved = offer;
    moved.replace(moved.find("AB:CD"), 5, "00:00");
    CHECK(!auth::VerifyOfferBinding(moved, alice.player.spki, why));
    CHECK(!auth::VerifyOfferBinding(sdp, alice.player.spki, why));
}

int main(int argc, char** argv) {
    TestJson();
    TestBlockHash();
    TestAuth();
    TestBlockCatalog();
    TestInventory();
    TestFlatGeneration();
    TestChunkEncodingDecodesWithClientCodec();
    TestBatchCodec();
    TestFragments();
    TestConfig();
    TestPluginHost(argc > 1 ? argv[1] : nullptr);
    if (g_failed) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failed);
        return 1;
    }
    std::puts("server_selftest ok");
    return 0;
}
