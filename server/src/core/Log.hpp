#pragma once
#include <functional>
#include <string>

namespace perlin::server {

    enum class LogLevel : int { Debug, Info, Warn, Error };

    // the embedding host can redirect output; the dedicated server prints to stdout
    using LogSink = std::function<void(LogLevel, const std::string&)>;
    void SetLogSink(LogSink sink);
    void Log(LogLevel level, const std::string& text);

    inline void LogInfo(const std::string& text) { Log(LogLevel::Info, text); }
    inline void LogWarn(const std::string& text) { Log(LogLevel::Warn, text); }
    inline void LogError(const std::string& text) { Log(LogLevel::Error, text); }
}
