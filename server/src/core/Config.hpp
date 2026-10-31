#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace perlin::server {

    struct FlatLayer {
        std::string block;
        int count = 1;
    };

    struct ServerConfig {
        std::string name = "Perlin Server";
        std::string motd = "A Perlin server";
        std::string levelName = "world";
        std::string dataDir = ".";
        // require an Xbox Live identity: logins must chain to Mojang's root key and, over nethernet, the offer must be signed by that identity.
        // off = anyone can join under any name (fine for a lan or singleplayer, not for a public server)
        bool xboxAuth = false;
        // extra or replacement trusted root keys (base64 der); empty = Mojang's
        std::vector<std::string> authRootKeys;
        std::string pluginDir;
        // where block_states.json and block_destroy.tsv are; empty = look beside the executable and in the source tree
        std::string blockDataDir;
        uint32_t maxPlayers = 20;
        uint32_t viewDistance = 12;
        uint32_t tickRate = 20;
        // 0 survival, 1 creative, 2 adventure
        int gameMode = 0;
        uint64_t seed = 0;
        // legacy list of operator names or xuids; ops.json is the real list now and is merged with this one
        std::vector<std::string> operators;
        // only players listed in allowlist.json (by name or xuid) may join (bds: allow-list). off by default here, bds ships it on
        bool whitelist = false;
        // answer lan discovery broadcasts (udp 7551) so the world shows up in the Friends tab of vanilla clients on the same network
        bool lanDiscovery = true;

        // nethernet signalling: http on this port, webrtc data channels behind it
        bool nethernet = true;
        uint16_t port = 19132;
        std::string bindAddress = "0.0.0.0";
        std::vector<std::string> iceServers;
        // one udp port for every player (needs one forwarded port); off = a port per connection from `port` upward
        bool iceUdpMux = true;
        // address webrtc binds its udp sockets to; empty = all interfaces. set it on hosts without ipv6 or with several nics
        std::string iceBindAddress;

        // "flat", or "vanilla" for the terrain port (its data is compiled in)
        std::string generator = "vanilla";
        // bottom to top from the dimension floor
        std::vector<FlatLayer> flatLayers = {{"minecraft:bedrock", 1}, {"minecraft:dirt", 2}, {"minecraft:grass_block", 1}};
        // auto = gpu when a device starts and passes its self check, else cpu
        std::string terrainBackend = "auto";

        // run without a network listener (embedded singleplayer)
        bool loopbackOnly = false;

        // bedrock dedicated server properties (server.properties keys in comments)
        // level-seed: empty = a random seed at start, a number is used as is, any other text is hashed like java's String.hashCode
        bool randomSeed = false;
        std::string levelSeedText;
        // difficulty: 0 peaceful, 1 easy, 2 normal, 3 hard
        int difficulty = 1;
        // force-gamemode, allow-cheats: read and kept; there are no saved players or achievements to apply them to yet
        bool forceGamemode = false;
        bool allowCheats = false;
        // transport: only nethernet is built; raknet is refused at start
        std::string transport = "nethernet";
        // server-udp-ports: [ip:][public:]local, each a port or a first-last range. unset (begin 0) = the older ice_udp_mux behaviour
        bool udpPortsSet = false;
        uint16_t udpPortBegin = 0, udpPortEnd = 0;
        // the public side of a mapping; webrtc cannot announce another address, so it is read and reported but not applied
        std::string udpPublicAddress;
        uint16_t udpPublicBegin = 0, udpPublicEnd = 0;
        uint16_t portV6 = 19133;
        uint32_t tickDistance = 4;
        // minutes without input before a player is kicked; 0 = never
        uint32_t idleTimeoutMinutes = 30;
        // worker threads for chunk generation; 0 = pick from the core count
        uint32_t maxThreads = 8;
        // permission for players not in permissions.json: visitor, member or operator
        std::string defaultPermission = "member";
        // network settings sent to clients. only zlib is built; snappy is accepted and falls back with a warning
        uint16_t compressionThreshold = 1;
        std::string compressionAlgorithm = "zlib";
        bool clientSideChunkGeneration = true;
        bool blockNetworkIdsAreHashes = true;
        bool disablePersona = false;
        bool disableCustomSkins = false;
        // None, Dropped or Disabled
        std::string chatRestriction = "None";
        bool packetRateLimiter = false;
        // properties this server does not act on, kept so a rewrite of server.properties does not lose them
        std::vector<std::pair<std::string, std::string>> extraProperties;

        // server.properties when the directory has one, else the old server.json, else the defaults
        static ServerConfig LoadDirectory(const std::string& dir, std::string* error = nullptr);
        // a path ending in .properties is read as key=value lines, anything else as json
        static ServerConfig LoadProperties(const std::string& path, std::string* error = nullptr);
        bool SaveProperties(const std::string& path) const;
        // sets one properties key; false for an unknown key (then it is kept in extraProperties)
        bool SetProperty(const std::string& key, const std::string& value, std::string* warning = nullptr);
        // level-seed text to a seed
        static uint64_t SeedFromText(const std::string& text);

        static ServerConfig Load(const std::string& path, std::string* error = nullptr);
        bool Save(const std::string& path) const;
    };
}
