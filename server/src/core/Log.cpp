#include "core/Log.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace perlin::server {
    namespace {
        std::mutex g_mutex;
        LogSink g_sink;
    }

    void SetLogSink(LogSink sink) {
        std::lock_guard lock(g_mutex);
        g_sink = std::move(sink);
    }

    void Log(LogLevel level, const std::string& text) {
        std::lock_guard lock(g_mutex);
        if (g_sink) {
            g_sink(level, text);
            return;
        }
        static const char* names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
        const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        char stamp[16];
        std::strftime(stamp, sizeof stamp, "%H:%M:%S", std::localtime(&t));
        std::fprintf(stdout, "[%s %s] %s\n", stamp, names[static_cast<int>(level)], text.c_str());
        std::fflush(stdout);
    }
}
