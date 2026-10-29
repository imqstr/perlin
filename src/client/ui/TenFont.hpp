#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Bedrock's "MinecraftTen" heading font (font_type: "MinecraftTen" in JSON UI). The font belongs to Mojang and is not
// shipped with Perlin: it is read at runtime from the player's own Bedrock install. Without one, labels fall back to
// the regular bitmap font.
namespace perlin::ui {
    class TenFont {
    public:
        // The em size the rest of the game measures against. Line and baseline metrics come from the
        // font's own hhea table (ascent 0.99 em, descent 0.25 em), not the bitmap grid.
        static constexpr float kEm = 10.0f;

        static TenFont& Instance();

        // Looks for minecraft-ten.ttf in known Bedrock install locations (PERLIN_TEN_FONT overrides). Safe to call
        // repeatedly; only the first successful load counts.
        bool Load();
        // Cheap repeated form of Load(): a single atomic read once it has been attempted.
        void EnsureLoaded();
        bool Available() const { return ready; }
        const std::string& Path() const { return path; }

        // Metrics in gui pixels for a label drawn at `scale` (font_scale_factor and font_size applied).
        float Em(float scale) const { return kEm * scale; }
        float Ascent(float scale) const;
        float Descent(float scale) const;
        float LineHeight(float scale) const;
        float Advance(uint32_t cp, float scale) const;
        float Width(const std::string& utf8, float scale) const;

        struct Glyph {
            float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
            int x = 0, y = 0, w = 0, h = 0;  // device pixels, y relative to the baseline
        };
        // A glyph atlas rasterised for an em of `emPx` device pixels. Render thread only.
        struct Page {
            int emPx = 0;
            int width = 0, height = 0;
            uint32_t version = 0;
            std::vector<uint8_t> rgba;
            std::unordered_map<uint32_t, Glyph> glyphs;
        };
        const Page* PageFor(int emPx);
        const Page* FindPage(int emPx) const;

    private:
        struct Impl;
        bool LoadLocked();
        std::shared_ptr<Impl> impl;
        std::string path;
        std::atomic<bool> ready{false};
        std::atomic<bool> attempted{false};
        bool tried = false;
        mutable std::mutex mutex;
        std::unordered_map<int, std::unique_ptr<Page>> pages;
        uint32_t pageVersion = 0;
    };

    // Whether a label's font_type asks for MinecraftTen.
    bool IsTenFontType(const std::string& fontType);
}
