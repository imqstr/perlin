#include "client/ui/FormatCodes.hpp"

#include "client/ui/RmlEmit.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string_view>

namespace perlin::ui {
    bool FormatCodeColor(char code, uint32_t& rgb) {
        switch (std::tolower(static_cast<unsigned char>(code))) {
        case '0': rgb = 0x000000; return true;
        case '1': rgb = 0x0000AA; return true;
        case '2': rgb = 0x00AA00; return true;
        case '3': rgb = 0x00AAAA; return true;
        case '4': rgb = 0xAA0000; return true;
        case '5': rgb = 0xAA00AA; return true;
        case '6': rgb = 0xFFAA00; return true;
        case '7': rgb = 0xAAAAAA; return true;
        case '8': rgb = 0x555555; return true;
        case '9': rgb = 0x5555FF; return true;
        case 'a': rgb = 0x55FF55; return true;
        case 'b': rgb = 0x55FFFF; return true;
        case 'c': rgb = 0xFF5555; return true;
        case 'd': rgb = 0xFF55FF; return true;
        case 'e': rgb = 0xFFFF55; return true;
        case 'f': rgb = 0xFFFFFF; return true;
        case 'g': rgb = 0xDDD605; return true;
        case 'h': rgb = 0xE3D4D1; return true;
        case 'i': rgb = 0xCECACA; return true;
        case 'j': rgb = 0x443A3B; return true;
        case 'm': rgb = 0x971607; return true;
        case 'n': rgb = 0xB4684D; return true;
        case 'p': rgb = 0xDEB12D; return true;
        case 'q': rgb = 0x47A036; return true;
        case 's': rgb = 0x2CBAA8; return true;
        case 't': rgb = 0x21497B; return true;
        case 'u': rgb = 0x9A5CC6; return true;
        case 'v': rgb = 0xEB7114; return true;
        default: return false;
        }
    }

    namespace {
        bool IsSection(const std::string& s, size_t i) {
            return i + 2 < s.size() && static_cast<unsigned char>(s[i]) == 0xC2 && static_cast<unsigned char>(s[i + 1]) == 0xA7;
        }
    }

    bool IsFormatCode(const std::string& text, size_t i) { return IsSection(text, i); }

    std::string StripFormatCodes(const std::string& text) {
        std::string out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size();) {
            if (IsSection(text, i)) {
                i += 3;
                continue;
            }
            out += text[i++];
        }
        return out;
    }

    namespace {
        std::string Scramble(const std::string& in) {
            static const char* const kClasses[] = {"il!|'.,:;", "tI[] ", "fk<>(){}*\"", "abcdeghjmnopqrsuvwxyz0123456789ABCDEFGHJKLMNOPQRSTUVWXYZ"};
            static uint32_t state = 0x9E3779B9u;
            std::string out = in;
            for (char& c : out) {
                for (const char* cls : kClasses) {
                    const std::string_view set(cls);
                    if (set.find(c) == std::string_view::npos)
                        continue;
                    state ^= state << 13;
                    state ^= state >> 17;
                    state ^= state << 5;
                    c = set[state % set.size()];
                    break;
                }
            }
            return out;
        }
    }

    std::string FormatCodesToRml(const std::string& text, float alpha) {
        return FormatCodesToRml(text, alpha, false);
    }

    std::string FormatCodesToRmlLines(const std::string& text) {
        return FormatCodesToRml(text, 1.0f, true);
    }

    std::string FormatCodesToRml(const std::string& text, float alpha, bool breaks) {
        std::string out;
        std::string chunk;
        bool bold = false, italic = false, underline = false, strike = false;
        bool haveColor = false;
        bool obfuscated = false;
        uint32_t color = 0;
        bool open = false;
        const int alphaByte = std::clamp(static_cast<int>(std::lround(alpha * 255.0f)), 0, 255);
        const auto close = [&] {
            if (open)
                out += "</span>";
            open = false;
        };
        const auto flush = [&] {
            if (chunk.empty())
                return;
            const bool styled = haveColor || bold || italic || underline || strike;
            if (styled) {
                char buf[32];
                std::string style;
                if (haveColor) {
                    if (alphaByte < 255)
                        std::snprintf(buf, sizeof buf, "color:#%06X%02X;", color & 0xFFFFFFu, alphaByte);
                    else
                        std::snprintf(buf, sizeof buf, "color:#%06X;", color & 0xFFFFFFu);
                    style += buf;
                }
                if (bold)
                    style += "font-weight:bold;";
                if (italic)
                    style += "font-style:italic;";
                if (underline || strike)
                    style += std::string("text-decoration:") + (underline ? "underline" : "") + (underline && strike ? " " : "") +
                             (strike ? "line-through" : "") + ";";
                out += "<span style=\"" + style + "\">";
                open = true;
            }
            out += EscapeRml(obfuscated ? Scramble(chunk) : chunk);
            chunk.clear();
            close();
        };
        for (size_t i = 0; i < text.size();) {
            if (IsSection(text, i)) {
                // icon escape: section + 'm' + two hex section codes addresses a glyph page icon directly
                const auto hex = [](unsigned char ch) -> int {
                    if (ch >= '0' && ch <= '9') return ch - '0';
                    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                    return -1;
                };
                if (i + 9 <= text.size() && (text[i + 2] == 'm' || text[i + 2] == 'M') && IsSection(text, i + 3) &&
                    IsSection(text, i + 6)) {
                    const int hi = hex(static_cast<unsigned char>(text[i + 5]));
                    const int lo = hex(static_cast<unsigned char>(text[i + 8]));
                    if (hi >= 0 && lo >= 0) {
                        flush();
                        const uint32_t cp = 0xE100u | (static_cast<uint32_t>(hi) << 4) | static_cast<uint32_t>(lo);
                        chunk += static_cast<char>(0xE0 | (cp >> 12));
                        chunk += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        chunk += static_cast<char>(0x80 | (cp & 0x3F));
                        i += 9;
                        continue;
                    }
                }
                flush();
                const char code = text[i + 2];
                i += 3;
                uint32_t rgb;
                if (FormatCodeColor(code, rgb)) {
                    haveColor = true;
                    color = rgb;
                } else {
                    switch (std::tolower(static_cast<unsigned char>(code))) {
                    case 'l': bold = true; break;
                    case 'o': italic = true; break;
                    case 'k': obfuscated = true; break;
                    case 'r': haveColor = bold = italic = underline = strike = obfuscated = false; break;
                    default: break;
                    }
                }
                continue;
            }
            if (i + 1 < text.size() && static_cast<unsigned char>(text[i]) == 0xC2 && static_cast<unsigned char>(text[i + 1]) == 0xA7) {
                i += 2;
                continue;
            }
            const char c = text[i++];
            if (c == '\r')
                continue;
            if (c == '\n') {
                if (breaks) {
                    flush();
                    out += "<br/>";
                }
                continue;
            }
            chunk += c;
        }
        flush();
        return out;
    }
}
