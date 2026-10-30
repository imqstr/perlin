#pragma once
#include "inventory/Inventory.hpp"
#include "net/Batch.hpp"
#include "net/Connection.hpp"
#include "world/Chunk.hpp"

#include <chrono>
#include <memory>
#include <span>
#include <utility>
#include <string>
#include <unordered_set>
#include <vector>

namespace perlin::proto2225 {
    struct Skin;
}

namespace perlin::server {

    class Server;

    // one connected player: the bedrock login handshake, chunk streaming and packet handling
    class Session {
    public:
        enum class State : uint8_t { AwaitSettings, AwaitLogin, ResourcePacks, ResourceStack, Loading, InGame, Closing, Closed };

        Session(Server& server, std::shared_ptr<Connection> connection, uint64_t runtimeId);
        ~Session();

        void OnMessage(std::span<const uint8_t> wire);
        // per tick: stream chunks, then flush whatever was queued
        void Tick(uint64_t tick);

        template <class P> void Send(const P& packet);
        void SendText(uint8_t type, const std::string& source, const std::string& message);
        void SendRaw(const std::string& message) { SendText(0, {}, message); }
        void Kick(const std::string& reason);
        void Teleport(double x, double y, double z);
        void Close();

        uint64_t Id() const { return runtimeId; }
        const std::string& Name() const { return name; }
        const std::string& Xuid() const { return xuid; }
        State GetState() const { return state; }
        bool InGame() const { return state == State::InGame; }
        bool IsClosed() const { return state == State::Closed; }
        bool Operator() const { return isOperator; }
        double X() const { return posX; }
        double Y() const { return posY; }
        double Z() const { return posZ; }
        const Connection& Conn() const { return *connection; }
        const proto2225::Skin& GetSkin() const;
        const std::array<uint8_t, 16>& UuidBytes() const { return uuid; }

        // other players entering / leaving this player's world
        void ShowPlayer(const Session& other);
        void HidePlayer(const Session& other);
        void SendBlockUpdate(int x, int y, int z, int32_t networkId);
        void SendMove(const Session& other, uint8_t mode);
        // sends a chunk to the player if inside their radius and not already sent
        bool WantsChunk(ChunkPos pos) const;
        bool HasSent(ChunkPos pos) const { return sent.count(pos) != 0; }
        PlayerInventory& Inventory() { return inventory; }
        bool Creative() const { return creative; }
        // whole inventory again, e.g. after the server changed it
        void SendInventory();
        void SendSlot(int slot);
        void SendHeldSelection();
        // the world's real block at a position, to undo a client prediction
        void CorrectBlock(int x, int y, int z);
        bool InReach(int x, int y, int z) const;
        // true once after the position changed
        bool TakeMoved() { return std::exchange(moved, false); }

    private:
        void Dispatch(uint32_t id, std::span<const uint8_t> body);
        void Queue(uint32_t id, const std::vector<uint8_t>& body);
        void Flush();
        void SendImmediate(uint32_t id, const std::vector<uint8_t>& body);

        void OnRequestSettings(std::span<const uint8_t> body);
        void OnLogin(std::span<const uint8_t> body);
        void OnPackResponse(std::span<const uint8_t> body);
        void OnChunkRadius(std::span<const uint8_t> body);
        void OnInitialized();
        void OnText(std::span<const uint8_t> body);
        void OnCommand(std::span<const uint8_t> body);
        void OnAuthInput(std::span<const uint8_t> body);
        void OnMovePlayer(std::span<const uint8_t> body);
        void OnStackRequest(std::span<const uint8_t> body);
        void OnMobEquipment(std::span<const uint8_t> body);
        void OnTransaction(std::span<const uint8_t> body);
        void OnPlayerAction(std::span<const uint8_t> body);
        void OnPickBlock(std::span<const uint8_t> body);
        void OnInteract(std::span<const uint8_t> body);
        void OnContainerClose(std::span<const uint8_t> body);

        void SendStartGame();
        void SendWorldSetup();
        void StreamChunks();
        void PublishCenter(bool force);
        void SetPosition(double x, double y, double z, float pitchValue, float yawValue);

        Server& server;
        std::shared_ptr<Connection> connection;
        BatchCodec codec;
        State state = State::AwaitSettings;
        std::vector<uint8_t> outgoing;

        uint64_t runtimeId;
        std::string name, xuid;
        std::array<uint8_t, 16> uuid{};
        std::unique_ptr<proto2225::Skin> skin;
        bool isOperator = false;
        bool creative = true;
        PlayerInventory inventory;

        double posX = 0.5, posY = 0, posZ = 0.5;
        float pitch = 0, yaw = 0;
        int32_t radius = 0;
        ChunkPos center{0, 0};
        bool centerSent = false;
        bool spawnSent = false;
        bool moved = false;
        std::unordered_set<ChunkPos, ChunkPosHash> sent;
        std::chrono::steady_clock::time_point lastPacket = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point stateSince = std::chrono::steady_clock::now();
    };
}
