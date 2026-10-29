#include "client/javashaders/GlslangBridge.hpp"
#include "client/javashaders/PackSource.hpp"
#include "client/javashaders/ProgramSet.hpp"
#include "client/javashaders/ShaderBackend.hpp"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include "client/javashaders/Transform.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <set>

using namespace perlin::jshader;

namespace perlin::jshader {
    std::unique_ptr<PackSource> OpenZipPack(const std::string&) { return nullptr; }
}

namespace {
    EShLanguage ToLang(Stage st) { return st == Stage::Vertex ? EShLangVertex : EShLangFragment; }

    bool ValidateGl(const std::string& text, Stage st, std::string& log) {
        glslang::TShader shader(ToLang(st));
        const char* src = text.c_str();
        shader.setStrings(&src, 1);
        if (!shader.parse(GetDefaultResources(), 430, false, EShMsgDefault)) { log = shader.getInfoLog(); return false; }
        return true;
    }

    bool ValidateHlsl(const std::string& text, bool fragment, std::string& log) {
        const EShLanguage lang = fragment ? EShLangFragment : EShLangVertex;
        glslang::TShader shader(lang);
        const char* src = text.c_str();
        shader.setStrings(&src, 1);
        shader.setEnvInput(glslang::EShSourceHlsl, lang, glslang::EShClientVulkan, 100);
        shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
        shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
        shader.setEntryPoint("main");
        shader.setSourceEntryPoint("main");
        if (!shader.parse(GetDefaultResources(), 100, false, static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl))) {
            log = shader.getInfoLog();
            return false;
        }
        return true;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: jshader_check <pack folder> [world0|world-1|world1] [-v] [-p program]\n");
        return 2;
    }
    std::string dim = argc > 2 && argv[2][0] != '-' ? argv[2] : "world0";
    bool verbose = false;
    std::string only, backend;
    for (int i = 2; i < argc; i++) {
        if (std::string(argv[i]) == "-v") verbose = true;
        if (std::string(argv[i]) == "-p" && i + 1 < argc) only = argv[++i];
        if (std::string(argv[i]) == "-b" && i + 1 < argc) backend = argv[++i];
    }
    std::shared_ptr<PackSource> pack = OpenPack(argv[1]);
    if (!pack) {
        std::fprintf(stderr, "cannot open pack\n");
        return 2;
    }
    ShaderPackInstance instance(pack, {}, EngineMacros(260300, false));
    instance.SetDimension(dim);
    std::printf("pack %s: %zu options, %zu block ids\n", instance.Name().c_str(), instance.Options().size(), instance.BlockIds().size());

    TransformOptions opts;
    opts.macros = instance.EngineMacros();
    opts.preprocess = PreprocessGlsl;
    opts.flipFragCoordY = backend != "gl";
    opts.zeroToOneDepth = backend != "gl";

    std::vector<std::string> names;
    for (const ProgramInfo& p : StandardPrograms()) names.push_back(p.name);
    for (const char* base : {"setup", "begin", "prepare", "deferred", "composite", "shadowcomp"})
        for (const std::string& n : instance.PassChain(base)) names.push_back(n);
    int ok = 0, bad = 0, skipped = 0;
    for (const std::string& name : names) {
        if (!only.empty() && name != only) continue;
        if (!instance.ProgramExists(name)) { skipped++; continue; }
        auto src = instance.Program(name);
        if (!src->found) { skipped++; continue; }
        if (src->fragment.text.empty() && src->vertex.text.empty()) { skipped++; continue; }
        ProgramResult result = TransformProgram(src->vertex, src->fragment, opts);
        std::string err;
        if (!result.ok()) {
            bad++;
            std::printf("FAIL  %-34s transform: %s %s\n", name.c_str(), result.vertex.error.c_str(), result.fragment.error.c_str());
            continue;
        }
        bool pass = true;
        for (StageResult* s : {&result.vertex, &result.fragment}) {
            std::vector<uint32_t> spv;
            std::string log;
            if (!CompileToSpirv(s->glsl, s->stage, spv, log)) {
                pass = false;
                std::printf("FAIL  %-34s %s: ", name.c_str(), s->stage == Stage::Vertex ? "vertex" : "fragment");
                size_t a = log.find("ERROR");
                size_t b = log.find('\n', a == std::string::npos ? 0 : a);
                std::printf("%s\n", log.substr(a == std::string::npos ? 0 : a, b - (a == std::string::npos ? 0 : a)).c_str());
                if (verbose) {
                    std::printf("%s\n", log.c_str());
                    FILE* f = std::fopen(("/tmp/jshader_" + name + (s->stage == Stage::Vertex ? ".vert" : ".frag")).c_str(), "w");
                    if (f) { std::fputs(s->glsl.c_str(), f); std::fclose(f); }
                }
            } else if (verbose) {
                std::printf("ok    %-34s %s (%zu words)\n", name.c_str(), s->stage == Stage::Vertex ? "vertex" : "fragment", spv.size());
            }
        }
        if (pass && !backend.empty()) {
            BackendOptions bo;
            bo.kind = backend == "gl" ? BackendKind::OpenGL : BackendKind::D3D11;
            std::string hlslErr;
            bo.hlslCompiler = [&](const std::string& hlsl, bool fragment, std::vector<uint8_t>& blob, std::string& error) {
                if (!ValidateHlsl(hlsl, fragment, error)) return false;
                blob.assign(8, 0);
                return true;
            };
            BackendProgram built = BuildBackendProgram(result, bo);
            if (!built.ok) {
                pass = false;
                std::printf("FAIL  %-34s backend: %.300s\n", name.c_str(), built.error.c_str());
                if (verbose) {
                    FILE* f = std::fopen(("/tmp/jshader_" + name + ".vs.txt").c_str(), "w");
                    if (f) { std::fputs(built.vertexSource.c_str(), f); std::fclose(f); }
                    f = std::fopen(("/tmp/jshader_" + name + ".fs.txt").c_str(), "w");
                    if (f) { std::fputs(built.fragmentSource.c_str(), f); std::fclose(f); }
                }
            } else if (verbose) {
                FILE* f = std::fopen(("/tmp/jshader_" + name + ".vs.txt").c_str(), "w");
                if (f) { std::fputs(built.vertexSource.c_str(), f); std::fclose(f); }
                f = std::fopen(("/tmp/jshader_" + name + ".fs.txt").c_str(), "w");
                if (f) { std::fputs(built.fragmentSource.c_str(), f); std::fclose(f); }
                std::printf("      %s: %d regs, %zu samplers\n", name.c_str(), built.uniformRegs, built.samplers.size());
            }
            if (built.ok && backend == "gl") {
                std::string log;
                if (!ValidateGl(built.vertexSource, Stage::Vertex, log)) { pass = false; std::printf("FAIL  %-34s GL vertex: %.300s\n", name.c_str(), log.c_str()); }
                else if (!ValidateGl(built.fragmentSource, Stage::Fragment, log)) { pass = false; std::printf("FAIL  %-34s GL fragment: %.300s\n", name.c_str(), log.c_str()); }
            }
        }
        (pass ? ok : bad)++;
    }
    std::printf("summary: %d ok, %d failed, %d absent\n", ok, bad, skipped);
    return bad ? 1 : 0;
}
