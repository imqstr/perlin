#include "client/ui/McFont.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    auto& font = perlin::ui::McFont::Instance();
    font.UseBuiltin();
    const int fallbackWidth = font.Get('?').width;
    std::vector<uint8_t> page(256 * 256 * 4, 255);
    font.AddGlyphPage(0xe0, page, 256, 256);
    const bool customGlyph = font.Get(0xe000).width == 8;
    const auto file = std::filesystem::temp_directory_path() / ("perlin-font-cache-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".hex");
    std::ofstream(file) << "4E00:" << std::string(32, 'F') << '\n';
    const bool loaded = font.LoadUnihexFile(file.string()) && font.HasUnihex();
    font.UnloadLevelResources();
    font.UnloadLevelResources();
    const bool cleared = font.Builtin() && !font.HasUnihex() && !font.HasVanillaSheets()
        && font.TtfCount() == 0 && font.Get(0xe000).width == fallbackWidth;
    const bool reload = font.LoadUnihexFile(file.string()) && font.HasUnihex();
    std::filesystem::remove(file);
    if (!customGlyph || !loaded || !cleared || !reload)
        std::cerr << "font cache survived unload or failed to reload\n";
    return customGlyph && loaded && cleared && reload ? 0 : 1;
}
