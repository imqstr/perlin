#pragma once
#include "client/javashaders/Transform.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace perlin::jshader {
    std::string PreprocessGlsl(const std::string& text, Stage stage, std::string& error);
    bool CompileToSpirv(const std::string& glsl450, Stage stage, std::vector<uint32_t>& spirv, std::string& log);
    MacroMap EngineMacros(int mcVersion, bool openglBackend);
}
