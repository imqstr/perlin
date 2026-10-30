// dedicated server: perlin-server [--data <dir>] [--port <n>] [--config-json '{...}']
#include "core/Config.hpp"
#include "core/Log.hpp"
#include "core/Server.hpp"

#include <atomic>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

using namespace perlin::server;

namespace {
    std::atomic<Server*> g_server{nullptr};

    void OnSignal(int) {
        if (Server* s = g_server.load())
            s->RequestStop();
    }
}

int main(int argc, char** argv) {
    std::string dir = ".";
    int port = 0;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--data" && i + 1 < argc) dir = argv[++i];
        else if (a == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (a == "--help" || a == "-h") {
            std::cout << "perlin-server [--data <dir>] [--port <n>]\n"
                         "  reads <dir>/server.json (written with defaults on first run), plugins from <dir>/plugins\n";
            return 0;
        }
    }
    std::filesystem::create_directories(dir);
    const std::string configPath = dir + "/server.json";
    std::string error;
    ServerConfig config = ServerConfig::Load(configPath, &error);
    if (!error.empty()) {
        LogError(error);
        return 2;
    }
    if (!std::filesystem::exists(configPath)) {
        config.Save(configPath);
        LogInfo("wrote default " + configPath);
    }
    config.dataDir = dir;
    if (config.pluginDir.empty())
        config.pluginDir = dir + "/plugins";
    if (port > 0)
        config.port = static_cast<uint16_t>(port);
    std::filesystem::create_directories(config.pluginDir);

    Server server(config);
    if (!server.Start(error)) {
        LogError("cannot start: " + error);
        return 1;
    }
    g_server = &server;
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    // console commands: a thread reads stdin; its commands run on the tick thread's behalf under the same single-threaded rules
    std::thread console;
    std::mutex lineMutex;
    std::vector<std::string> lines;
    std::atomic<bool> consoleDone{false};
    console = std::thread([&] {
        std::string line;
        while (std::getline(std::cin, line)) {
            std::lock_guard lock(lineMutex);
            lines.push_back(line);
        }
        consoleDone = true;
    });
    console.detach();

    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::nanoseconds(1'000'000'000ll / std::max<uint32_t>(1, config.tickRate));
    auto next = clock::now();
    while (!server.StopRequested()) {
        {
            std::vector<std::string> pending;
            {
                std::lock_guard lock(lineMutex);
                pending.swap(lines);
            }
            for (const std::string& l : pending)
                server.RunConsoleCommand(l);
        }
        server.Tick();
        next += period;
        const auto now = clock::now();
        if (next < now)
            next = now;
        else
            std::this_thread::sleep_until(next);
    }
    LogInfo("stopping");
    g_server = nullptr;
    server.Stop();
    return 0;
}
