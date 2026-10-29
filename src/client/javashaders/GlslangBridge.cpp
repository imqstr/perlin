#include "client/javashaders/GlslangBridge.hpp"

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <mutex>

namespace perlin::jshader {
    namespace {
        std::once_flag gInit;
        void Init() { std::call_once(gInit, [] { glslang::InitializeProcess(); }); }

        EShLanguage Lang(Stage s) {
            switch (s) {
            case Stage::Vertex: return EShLangVertex;
            case Stage::Geometry: return EShLangGeometry;
            case Stage::Compute: return EShLangCompute;
            default: return EShLangFragment;
            }
        }

        class NoIncluder : public glslang::TShader::Includer {
        public:
            IncludeResult* includeSystem(const char*, const char*, size_t) override { return nullptr; }
            IncludeResult* includeLocal(const char*, const char*, size_t) override { return nullptr; }
            void releaseInclude(IncludeResult*) override {}
        };
    }

    std::string PreprocessGlsl(const std::string& text, Stage stage, std::string& error) {
        Init();
        glslang::TShader shader(Lang(stage));
        const char* src = text.c_str();
        shader.setStrings(&src, 1);
        shader.setEnvInput(glslang::EShSourceGlsl, Lang(stage), glslang::EShClientVulkan, 450);
        shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
        shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
        NoIncluder includer;
        std::string out;
        if (!shader.preprocess(GetDefaultResources(), 450, ENoProfile, false, false, EShMsgDefault, &out, includer)) {
            error = shader.getInfoLog();
            return {};
        }
        error = shader.getInfoLog();
        return out;
    }

    bool CompileToSpirv(const std::string& glsl, Stage stage, std::vector<uint32_t>& spirv, std::string& log) {
        Init();
        glslang::TShader shader(Lang(stage));
        const char* src = glsl.c_str();
        shader.setStrings(&src, 1);
        shader.setEnvInput(glslang::EShSourceGlsl, Lang(stage), glslang::EShClientVulkan, 450);
        shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
        shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
        const EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
        if (!shader.parse(GetDefaultResources(), 450, false, messages)) {
            log = shader.getInfoLog();
            return false;
        }
        glslang::TProgram program;
        program.addShader(&shader);
        if (!program.link(messages)) {
            log = std::string(shader.getInfoLog()) + program.getInfoLog();
            return false;
        }
        spv::SpvBuildLogger logger;
        glslang::SpvOptions options;
        options.disableOptimizer = true;
        glslang::GlslangToSpv(*program.getIntermediate(Lang(stage)), spirv, &logger, &options);
        log = logger.getAllMessages();
        return !spirv.empty();
    }

    MacroMap EngineMacros(int mcVersion, bool opengl) {
        MacroMap m;
        m["MC_VERSION"] = std::to_string(mcVersion);
        m["MC_GL_VERSION"] = "450";
        m["MC_GLSL_VERSION"] = "450";
        m["MC_OS_WINDOWS"] = "";
        m["MC_GL_VENDOR_OTHER"] = "";
        m["MC_GL_RENDERER_OTHER"] = "";
        m["MC_NORMAL_MAP"] = "";
        m["MC_SPECULAR_MAP"] = "";
        m["MC_HAND_DEPTH"] = "0.125";
        m["MC_RENDER_QUALITY"] = "1.0";
        m["MC_SHADOW_QUALITY"] = "1.0";
        m["MC_FXAA_LEVEL"] = "0";
        m["DYNAMIC_HANDLIGHT"] = "";
        const char* stages[] = {"NONE", "SKY", "SUNSET", "CUSTOM_SKY", "SUN", "MOON", "STARS", "VOID", "TERRAIN_SOLID",
                                "TERRAIN_CUTOUT_MIPPED", "TERRAIN_CUTOUT", "ENTITIES", "BLOCK_ENTITIES", "DESTROY",
                                "OUTLINE", "DEBUG", "HAND_SOLID", "TERRAIN_TRANSLUCENT", "TRIPWIRE", "PARTICLES", "CLOUDS",
                                "RAIN_SNOW", "WORLD_BORDER", "HAND_TRANSLUCENT"};
        for (int i = 0; i < static_cast<int>(sizeof(stages) / sizeof(stages[0])); i++)
            m[std::string("MC_RENDER_STAGE_") + stages[i]] = std::to_string(i);
        (void)opengl;
        return m;
    }
}
