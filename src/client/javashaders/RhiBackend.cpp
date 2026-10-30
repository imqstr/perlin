#include <cstdio>
#include <sstream>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include "client/javashaders/RhiBackend.hpp"

#include "client/javashaders/GlslangBridge.hpp"

#include <spirv_hlsl.hpp>

#include <algorithm>
#include <cstring>
#include <map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3dcompiler.h>
#endif

namespace perlin::jshader {
    void MatchStageInterface(std::string& vs, std::string& ps);
    namespace {
        bool CrossHlsl(const std::vector<uint32_t>& spirv, Stage stage, std::string& out, std::string& error) {
            try {
                spirv_cross::CompilerHLSL c(spirv);
                const spirv_cross::ShaderResources res = c.get_shader_resources();
                if (!res.storage_images.empty() || !res.storage_buffers.empty()) {
                    error = "storage images / buffers are not supported";
                    return false;
                }
                if (stage == Stage::Vertex) {
                    for (const auto& in : res.stage_inputs) {
                        const uint32_t loc = c.get_decoration(in.id, spv::DecorationLocation);
                        spirv_cross::HLSLVertexAttributeRemap remap;
                        remap.location = loc;
                        remap.semantic = "TEXCOORD" + std::to_string(loc);
                        c.add_vertex_attribute_remap(remap);
                    }
                }
                spirv_cross::CompilerHLSL::Options h;
                // spaces carry the descriptor set, which both d3d backends read back
                h.shader_model = 51;
                h.point_size_compat = true;
                h.point_coord_compat = true;
                h.flatten_matrix_vertex_input_semantics = false;
                c.set_hlsl_options(h);
                out = c.compile();
                return true;
            } catch (const std::exception& e) {
                error = e.what();
                return false;
            }
        }

        std::vector<uint8_t> Bytes(const std::vector<uint32_t>& words) {
            std::vector<uint8_t> out(words.size() * 4);
            std::memcpy(out.data(), words.data(), out.size());
            return out;
        }
    }

    RhiProgram BuildRhiProgram(const ProgramResult& program, RhiShaderKind kind, const DxbcCompiler& dxbc) {
        RhiProgram result;
        result.kind = kind;
        result.uniformRegs = program.uniformRegs;
        if (!program.ok()) {
            result.error = "program did not transform";
            return result;
        }
        if (kind == RhiShaderKind::Dxbc && !dxbc) {
            result.error = "no HLSL compiler available";
            return result;
        }
        std::map<std::string, RhiSampler> samplers;
        for (const StageResult* st : {&program.vertex, &program.fragment})
            for (const SamplerDecl& s : st->samplers) {
                RhiSampler r;
                r.name = s.name;
                r.slot = s.binding;
                r.shadow = s.type.find("Shadow") != std::string::npos;
                samplers[s.name] = r;
            }
        for (const auto& kv : samplers)
            result.samplers.push_back(kv.second);

        const StageResult* stages[2] = {&program.vertex, &program.fragment};
        std::vector<uint8_t>* outputs[2] = {&result.vertex, &result.fragment};
        std::string* sources[2] = {&result.vertexSource, &result.fragmentSource};
        for (int i = 0; i < 2; i++) {
            const bool fragment = i == 1;
            std::vector<uint32_t> spirv;
            std::string log;
            if (!CompileToSpirv(stages[i]->glsl, stages[i]->stage, spirv, log)) {
                result.error = std::string(fragment ? "fragment" : "vertex") + " shader failed to compile: " + log;
                return result;
            }
            if (kind == RhiShaderKind::SpirV) {
                *outputs[i] = Bytes(spirv);
                continue;
            }
            std::string hlsl, err;
            if (!CrossHlsl(spirv, stages[i]->stage, hlsl, err)) {
                result.error = std::string(fragment ? "fragment" : "vertex") + " shader conversion failed: " + err;
                return result;
            }
            // packs ask for every loop to be unrolled (optionNV unroll all); fxc fails on the big ones, so it decides itself
            int unrolled = 0;
            for (size_t at = hlsl.find("[unroll]"); at != std::string::npos; at = hlsl.find("[unroll]", at)) {
                hlsl.replace(at, 8, "");
                unrolled++;
            }
            if (unrolled && std::getenv("JSHADER_TRACE"))
                std::fprintf(stderr, "[jshader] dropped %d unroll attributes from the %s shader\n", unrolled, fragment ? "pixel" : "vertex");
            // shadow maps have no mips, and a comparison sample with implicit derivatives inside a loop makes fxc unroll it
            for (size_t at = hlsl.find(".SampleCmp("); at != std::string::npos; at = hlsl.find(".SampleCmp(", at))
                hlsl.replace(at, 11, ".SampleCmpLevelZero(");
            *sources[i] = hlsl;
        }
        if (kind != RhiShaderKind::SpirV) {
            MatchStageInterface(result.vertexSource, result.fragmentSource);
            for (int i = 0; i < 2; i++) {
                const bool fragment = i == 1;
                if (kind == RhiShaderKind::Hlsl) {
                    outputs[i]->assign(sources[i]->begin(), sources[i]->end());
                    continue;
                }
                std::string cerr;
                if (!dxbc(*sources[i], fragment, *outputs[i], cerr)) {
                    result.error = std::string(fragment ? "pixel" : "vertex") + " shader: HLSL compile failed: " + cerr;
                    return result;
                }
            }
        }
        result.ok = true;
        return result;
    }

    // D3D rejects a pipeline whose pixel shader reads a varying the vertex shader never writes (packs
    // declare some under ifdefs); give the vertex shader the missing outputs, zero filled.
    void MatchStageInterface(std::string& vs, std::string& ps) {
        auto body = [](const std::string& src, const char* structName, size_t& b, size_t& e) {
            b = src.find(std::string("struct ") + structName);
            if (b == std::string::npos) return false;
            b = src.find('{', b);
            e = src.find("};", b);
            return b != std::string::npos && e != std::string::npos;
        };
        size_t pb, pe, vb, ve;
        if (!body(ps, "SPIRV_Cross_Input", pb, pe) || !body(vs, "SPIRV_Cross_Output", vb, ve)) return;
        std::string vsBody = vs.substr(vb, ve - vb);
        std::string add, init;
        std::istringstream in(ps.substr(pb + 1, pe - pb - 1));
        std::string line;
        while (std::getline(in, line)) {
            size_t colon = line.find(" : TEXCOORD");
            if (colon == std::string::npos) continue;
            std::string sem = line.substr(colon + 3);
            while (!sem.empty() && (sem.back() == ';' || sem.back() == '\r' || sem.back() == ' ')) sem.pop_back();
            if (vsBody.find(" : " + sem + ";") != std::string::npos) continue;
            size_t t0 = line.find_first_not_of(" \t");
            std::string decl = line.substr(t0, colon - t0);
            size_t sp = decl.rfind(' ');
            if (sp == std::string::npos) continue;
            add += "    " + decl + " : " + sem + ";\n";
            init += "    stage_output." + decl.substr(sp + 1) + " = (" + decl.substr(0, sp) + ")0;\n";
        }
        if (add.empty()) return;
        vs.insert(ve, add);
        size_t ret = vs.rfind("return stage_output;");
        if (ret == std::string::npos) ret = vs.rfind("return ");
        if (ret != std::string::npos) vs.insert(ret, init);
        if (std::getenv("JSHADER_TRACE")) std::fprintf(stderr, "[jshader] vertex shader padded with missing pixel inputs\n");
    }

    DxbcCompiler SystemDxbcCompiler() {
#ifdef _WIN32
        return [](const std::string& hlsl, bool fragment, std::vector<uint8_t>& dxbc, std::string& error) -> bool {
            static HMODULE module = LoadLibraryA("d3dcompiler_47.dll");
            static pD3DCompile compile = module ? reinterpret_cast<pD3DCompile>(GetProcAddress(module, "D3DCompile")) : nullptr;
            if (!compile) {
                error = "d3dcompiler_47.dll is not available";
                return false;
            }
            // compiling a big pack takes minutes: finished shaders are kept on disk keyed by their source
            std::filesystem::path cacheFile;
            try {
                uint64_t h = 1469598103934665603ull;
                for (unsigned char c : hlsl) {
                    h ^= c;
                    h *= 1099511628211ull;
                }
                h ^= fragment ? 0x9e3779b97f4a7c15ull : 0x517cc1b727220a95ull;
                char name[48];
                std::snprintf(name, sizeof(name), "%016llx_%zx.dxbc", static_cast<unsigned long long>(h), hlsl.size());
                const std::filesystem::path dir = std::filesystem::temp_directory_path() / "perlin_dxbc_cache2";
                std::filesystem::create_directories(dir);
                cacheFile = dir / name;
                std::ifstream in(cacheFile, std::ios::binary);
                if (in) {
                    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                    if (bytes.size() > 32) {
                        dxbc = std::move(bytes);
                        return true;
                    }
                }
            } catch (...) {
                cacheFile.clear();
            }
            const auto store = [&]() {
                if (cacheFile.empty())
                    return;
                try {
                    std::ofstream out(cacheFile, std::ios::binary | std::ios::trunc);
                    out.write(reinterpret_cast<const char*>(dxbc.data()), static_cast<std::streamsize>(dxbc.size()));
                } catch (...) {
                }
            };
            const auto run = [&](const std::string& text, UINT flags, std::string& err) -> bool {
                ID3DBlob* code = nullptr;
                ID3DBlob* messages = nullptr;
                const HRESULT hr = compile(text.data(), text.size(), "javapack", nullptr, nullptr, "main", fragment ? "ps_5_1" : "vs_5_1", flags, 0,
                                           &code, &messages);
                if (FAILED(hr) || !code) {
                    err = messages ? std::string(static_cast<const char*>(messages->GetBufferPointer()), messages->GetBufferSize()) : "D3DCompile failed";
                    // warnings (pow of a possibly negative value and the like) can fill the log before the actual errors do
                    std::string errors;
                    size_t at = 0;
                    while (at < err.size()) {
                        size_t end = err.find('\n', at);
                        if (end == std::string::npos)
                            end = err.size();
                        const std::string line = err.substr(at, end - at);
                        if (line.find("error") != std::string::npos && errors.size() < 1200)
                            errors += line + "\n";
                        at = end + 1;
                    }
                    if (!errors.empty())
                        err = errors;
                    if (err.size() > 1200)
                        err.resize(1200);
                    if (messages)
                        messages->Release();
                    if (code)
                        code->Release();
                    return false;
                }
                const uint8_t* p = static_cast<const uint8_t*>(code->GetBufferPointer());
                dxbc.assign(p, p + code->GetBufferSize());
                code->Release();
                if (messages)
                    messages->Release();
                return true;
            };
            // loops stay loops: fxc otherwise tries to unroll the long ones with dynamic counts and gives up
            std::string looped;
            looped.reserve(hlsl.size() + 4096);
            for (size_t at = 0; at < hlsl.size();) {
                const size_t nl = hlsl.find('\n', at);
                const size_t end = nl == std::string::npos ? hlsl.size() : nl + 1;
                const size_t first = hlsl.find_first_not_of(" \t", at);
                if (first != std::string::npos && first < end &&
                    (hlsl.compare(first, 5, "for (") == 0 || hlsl.compare(first, 7, "while (") == 0))
                    looped.append(hlsl, at, first - at).append("[loop] ");
                else
                    looped.append(hlsl, at, first == std::string::npos || first >= end ? end - at : first - at);
                if (first != std::string::npos && first < end)
                    looped.append(hlsl, first, end - first);
                at = end;
            }
            std::string first, second, third;
            // fully optimised first (the pack is the whole frame, so shader quality is frame time); the cache keeps this a one time cost
            if (run(hlsl, D3DCOMPILE_OPTIMIZATION_LEVEL3, first) || run(looped, D3DCOMPILE_OPTIMIZATION_LEVEL3, second) ||
                run(looped, D3DCOMPILE_OPTIMIZATION_LEVEL1, third)) {
                store();
                return true;
            }
            // the optimizer gives up on some very large shaders: try again without it
            std::string fourth;
            if (run(hlsl, D3DCOMPILE_SKIP_OPTIMIZATION, fourth)) {
                store();
                return true;
            }
            error = first + (second.empty() || second == first ? "" : "\n(without loop hints) " + second) + (third.empty() ? "" : "\n(level 1) " + third) + (fourth.empty() ? "" : "\n(without optimization) " + fourth);
            return false;
        };
#else
        return {};
#endif
    }
}
