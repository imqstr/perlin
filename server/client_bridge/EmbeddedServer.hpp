#pragma once
// client side of singleplayer: loads the perlin_server library and exposes a world as an ordinary perlin::Transport,
// so BedrockSession joins it like any NetherNet server, without sockets
// (not wired into the title screen yet; BedrockSession needs `usedRak = false` for it, same as a NetherNet join)
#include "network/Transport.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace perlin {

    class EmbeddedServer {
    public:
        EmbeddedServer();
        ~EmbeddedServer();
        EmbeddedServer(const EmbeddedServer&) = delete;
        EmbeddedServer& operator=(const EmbeddedServer&) = delete;

        // libraryPath: perlin_server.dll / libperlin_server.so / .dylib next to the executable
        // dataDir: world and config directory; configJson overrides server.json keys (may be empty)
        bool Start(const std::string& libraryPath, const std::string& dataDir, const std::string& configJson, std::string& error);
        void Stop();
        bool Running() const;
        // one tick on the caller's thread when `ownThread` was false
        void Tick();
        void Command(const std::string& line);

        // a transport for BedrockSession; null if the server is not running
        std::unique_ptr<Transport> Connect();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
