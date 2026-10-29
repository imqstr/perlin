#include "client/javashaders/ShaderBackend.hpp"

#include "client/javashaders/GlslangBridge.hpp"

#include <spirv_glsl.hpp>
#include <spirv_hlsl.hpp>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace perlin::jshader {
    namespace {
        constexpr uint8_t kShaderBinVersion = 12;
        constexpr uint8_t kUniformFragmentBit = 0x10;
        constexpr uint8_t kUniformSamplerBit = 0x20;
        constexpr uint8_t kUniformCompareBit = 0x80;
        constexpr uint8_t kUniformTypeSampler = 0;
        constexpr uint8_t kUniformTypeVec4 = 2;

        constexpr uint8_t kDim2D = 0x02;
        constexpr uint8_t kDim3D = 0x06;
        constexpr uint8_t kCompFloat = 0x00, kCompInt = 0x01, kCompUint = 0x02, kCompDepth = 0x03;

        struct Writer {
            std::vector<uint8_t>& out;
            void U8(uint8_t v) { out.push_back(v); }
            void U16(uint16_t v) { out.push_back(v & 0xff); out.push_back(v >> 8); }
            void U32(uint32_t v) { for (int i = 0; i < 4; i++) out.push_back((v >> (8 * i)) & 0xff); }
            void Bytes(const void* p, size_t n) {
                const uint8_t* b = static_cast<const uint8_t*>(p);
                out.insert(out.end(), b, b + n);
            }
        };

        uint32_t Hash(const void* data, size_t size) {
            uint32_t h = 2166136261u;
            const uint8_t* p = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < size; i++) { h ^= p[i]; h *= 16777619u; }
            return h;
        }

        const char* GlAttribName(int attrib) {
            static const char* names[] = {"a_position", "a_normal", "a_tangent", "a_bitangent", "a_color0", "a_color1",
                                          "a_color2", "a_color3", "a_indices", "a_weight", "a_texcoord0", "a_texcoord1",
                                          "a_texcoord2", "a_texcoord3", "a_texcoord4", "a_texcoord5", "a_texcoord6",
                                          "a_texcoord7", "a_texcoord8", "a_texcoord9", "a_texcoord10", "a_texcoord11",
                                          "a_texcoord12", "a_texcoord13", "a_texcoord14", "a_texcoord15"};
            return attrib >= 0 && attrib < 26 ? names[attrib] : nullptr;
        }

        std::string HlslSemantic(int attrib) {
            static const char* names[] = {"POSITION", "NORMAL", "TANGENT", "BITANGENT", "COLOR0", "COLOR1", "COLOR2",
                                          "COLOR3", "BLENDINDICES", "BLENDWEIGHT"};
            if (attrib >= 0 && attrib < 10) return names[attrib];
            if (attrib >= 10 && attrib < 26) return "TEXCOORD" + std::to_string(attrib - 10);
            return "TEXCOORD15";
        }

        struct CrossResult {
            std::string source;
            std::vector<SamplerInfo> samplers;
            std::vector<ContainerUniform> samplerUniforms;
            std::vector<uint16_t> attributes;
            int uniformBytes = 0;
        };

        uint8_t ComponentId(spirv_cross::SPIRType::BaseType t, bool depth) {
            if (depth) return kCompFloat;
            switch (t) {
            case spirv_cross::SPIRType::Int: return kCompInt;
            case spirv_cross::SPIRType::UInt: return kCompUint;
            default: return kCompFloat;
            }
        }

        template <class Compiler>
        bool CollectSamplers(Compiler& c, const spirv_cross::ShaderResources& res, CrossResult& out, bool fragment, std::string& error) {
            for (const auto& s : res.sampled_images) {
                const spirv_cross::SPIRType& type = c.get_type(s.type_id);
                const spirv_cross::SPIRType& base = c.get_type(s.base_type_id);
                SamplerInfo info;
                info.name = c.get_name(s.id);
                info.stage = static_cast<int>(c.get_decoration(s.id, spv::DecorationBinding));
                info.shadow = base.image.depth;
                info.dimension = base.image.dim == spv::Dim3D ? 3 : 2;
                info.component = static_cast<int>(ComponentId(c.get_type(base.image.type).basetype, false));
                if (base.image.dim != spv::Dim2D && base.image.dim != spv::Dim3D) {
                    error = "unsupported sampler dimension for '" + info.name + "'";
                    return false;
                }
                (void)type;
                out.samplers.push_back(info);
                ContainerUniform u;
                u.name = info.name;
                u.type = static_cast<uint8_t>(kUniformTypeSampler | kUniformSamplerBit | (info.shadow ? kUniformCompareBit : 0) |
                                              (fragment ? kUniformFragmentBit : 0));
                u.num = 1;
                u.regIndex = static_cast<uint16_t>(info.stage);
                u.regCount = 0;
                u.texComponent = ComponentId(c.get_type(base.image.type).basetype, base.image.depth);
                u.texDimension = base.image.dim == spv::Dim3D ? kDim3D : kDim2D;
                u.texFormat = 0;
                out.samplerUniforms.push_back(u);
            }
            if (!res.storage_images.empty() || !res.storage_buffers.empty() || !res.separate_images.empty()) {
                error = "storage images / buffers are not supported";
                return false;
            }
            return true;
        }

        bool CrossGlsl(const std::vector<uint32_t>& spirv, Stage stage, const BackendOptions& opt, CrossResult& out, std::string& error) {
            try {
                spirv_cross::CompilerGLSL c(spirv);
                spirv_cross::ShaderResources res = c.get_shader_resources();
                const bool fragment = stage == Stage::Fragment;
                if (!CollectSamplers(c, res, out, fragment, error)) return false;
                for (const auto& s : res.sampled_images) {
                    c.unset_decoration(s.id, spv::DecorationBinding);
                    c.unset_decoration(s.id, spv::DecorationDescriptorSet);
                }
                for (const auto& ub : res.uniform_buffers) {
                    c.set_name(ub.id, "IrisGlobals");
                    c.unset_decoration(ub.id, spv::DecorationBinding);
                    c.unset_decoration(ub.id, spv::DecorationDescriptorSet);
                    c.flatten_buffer_block(ub.id);
                }
                if (stage == Stage::Vertex) {
                    for (const auto& in : res.stage_inputs) {
                        const uint32_t loc = c.get_decoration(in.id, spv::DecorationLocation);
                        const char* name = GlAttribName(static_cast<int>(loc));
                        if (!name) { error = "bad vertex attribute location"; return false; }
                        c.set_name(in.id, name);
                        c.unset_decoration(in.id, spv::DecorationLocation);
                        out.attributes.push_back(static_cast<uint16_t>(loc));
                    }
                }
                spirv_cross::CompilerGLSL::Options o;
                o.version = static_cast<uint32_t>(opt.glslVersion);
                o.es = false;
                o.separate_shader_objects = false;
                o.enable_420pack_extension = false;
                o.vertex.fixup_clipspace = false;
                o.vertex.flip_vert_y = false;
                c.set_common_options(o);
                out.source = c.compile();
                return true;
            } catch (const std::exception& e) {
                error = e.what();
                return false;
            }
        }

        std::string ForceLod0(const std::string& src) {
            const std::string key = ".Sample(";
            std::string out;
            size_t pos = 0;
            for (;;) {
                const size_t at = src.find(key, pos);
                if (at == std::string::npos) { out += src.substr(pos); break; }
                out += src.substr(pos, at - pos);
                const size_t first = at + key.size();
                int depth = 1;
                std::vector<size_t> commas;
                size_t j = first;
                for (; j < src.size(); j++) {
                    const char c = src[j];
                    if (c == '(') depth++;
                    else if (c == ')') { if (--depth == 0) break; }
                    else if (c == ',' && depth == 1) commas.push_back(j);
                }
                if (depth != 0 || commas.empty()) { out += src.substr(at); break; }
                const std::string sampler = src.substr(first, commas[0] - first);
                const size_t coordEnd = commas.size() > 1 ? commas[1] : j;
                const std::string coord = src.substr(commas[0] + 1, coordEnd - commas[0] - 1);
                const std::string rest = commas.size() > 1 ? "," + src.substr(commas[1] + 1, j - commas[1] - 1) : "";
                out += ".SampleLevel(" + sampler + "," + coord + ", 0.0" + rest + ")";
                pos = j + 1;
            }
            return out;
        }

        bool CrossHlsl(const std::vector<uint32_t>& spirv, Stage stage, bool lod0, CrossResult& out, std::string& error) {
            try {
                spirv_cross::CompilerHLSL c(spirv);
                spirv_cross::ShaderResources res = c.get_shader_resources();
                const bool fragment = stage == Stage::Fragment;
                if (!CollectSamplers(c, res, out, fragment, error)) return false;
                for (const auto& ub : res.uniform_buffers) {
                    c.set_name(ub.id, "IrisGlobals");
                    const spirv_cross::SPIRType& t = c.get_type(ub.base_type_id);
                    out.uniformBytes = static_cast<int>(c.get_declared_struct_size(t));
                }
                for (const auto& s : res.sampled_images) {
                    const uint32_t binding = c.get_decoration(s.id, spv::DecorationBinding);
                    if (binding < 16) continue;
                    spirv_cross::HLSLResourceBinding rb;
                    rb.stage = fragment ? spv::ExecutionModelFragment : spv::ExecutionModelVertex;
                    rb.desc_set = c.get_decoration(s.id, spv::DecorationDescriptorSet);
                    rb.binding = binding;
                    rb.srv.register_space = 0;
                    rb.srv.register_binding = binding;
                    rb.sampler.register_space = 0;
                    rb.sampler.register_binding = binding % 16;
                    c.add_hlsl_resource_binding(rb);
                }
                if (stage == Stage::Vertex) {
                    for (const auto& in : res.stage_inputs) {
                        const uint32_t loc = c.get_decoration(in.id, spv::DecorationLocation);
                        spirv_cross::HLSLVertexAttributeRemap remap;
                        remap.location = loc;
                        remap.semantic = HlslSemantic(static_cast<int>(loc));
                        c.add_vertex_attribute_remap(remap);
                        out.attributes.push_back(static_cast<uint16_t>(loc));
                    }
                }
                spirv_cross::CompilerHLSL::Options h;
                h.shader_model = 50;
                h.point_size_compat = true;
                h.point_coord_compat = true;
                h.flatten_matrix_vertex_input_semantics = false;
                c.set_hlsl_options(h);
                out.source = c.compile();
                if (lod0 && stage == Stage::Fragment) out.source = ForceLod0(out.source);
                return true;
            } catch (const std::exception& e) {
                error = e.what();
                return false;
            }
        }
    }

    std::vector<uint8_t> WriteBgfxShader(bool fragment, const std::vector<ContainerUniform>& uniforms, const void* code,
                                         size_t codeSize, const std::vector<uint16_t>& attributes, uint16_t constantBytes,
                                         uint32_t interfaceHash) {
        std::vector<uint8_t> out;
        Writer w{out};
        w.U8(fragment ? 'F' : 'V');
        w.U8('S');
        w.U8('H');
        w.U8(kShaderBinVersion);
        w.U32(fragment ? interfaceHash : 0u);
        w.U32(fragment ? 0u : interfaceHash);
        w.U32(0);
        w.U32(0);
        w.U16(static_cast<uint16_t>(uniforms.size()));
        for (const ContainerUniform& u : uniforms) {
            w.U8(static_cast<uint8_t>(u.name.size()));
            w.Bytes(u.name.data(), u.name.size());
            w.U8(u.type);
            w.U8(u.num);
            w.U16(u.regIndex);
            w.U16(u.regCount);
            w.U8(u.texComponent);
            w.U8(u.texDimension);
            w.U16(u.texFormat);
        }
        w.U32(static_cast<uint32_t>(codeSize));
        w.Bytes(code, codeSize);
        w.U8(0);
        w.U8(static_cast<uint8_t>(attributes.size()));
        for (uint16_t a : attributes) w.U16(a);
        w.U16(constantBytes);
        return out;
    }

    BackendProgram BuildBackendProgram(const ProgramResult& program, const BackendOptions& options) {
        BackendProgram result;
        result.uniformRegs = program.uniformRegs;
        if (!program.ok()) {
            result.error = "program did not transform";
            return result;
        }
        struct Stg { const StageResult* s; bool fragment; };
        const Stg stages[2] = {{&program.vertex, false}, {&program.fragment, true}};
        std::vector<uint8_t>* outputs[2] = {&result.vertex, &result.fragment};
        std::string* sources[2] = {&result.vertexSource, &result.fragmentSource};
        std::map<std::string, int> stageOf;
        const uint32_t interfaceHash = Hash(program.vertex.glsl.data(), program.vertex.glsl.size()) ^
                                       (Hash(program.fragment.glsl.data(), program.fragment.glsl.size()) * 16777619u);

        for (int i = 0; i < 2; i++) {
            std::vector<uint32_t> spirv;
            std::string log;
            if (!CompileToSpirv(stages[i].s->glsl, stages[i].s->stage, spirv, log)) {
                result.error = std::string(stages[i].fragment ? "fragment" : "vertex") + " shader failed to compile: " + log;
                return result;
            }
            CrossResult cross;
            std::string err;
            bool ok = false;
            if (options.kind == BackendKind::OpenGL) ok = CrossGlsl(spirv, stages[i].s->stage, options, cross, err);
            else if (options.kind == BackendKind::D3D11) ok = CrossHlsl(spirv, stages[i].s->stage, options.explicitLod0, cross, err);
            else err = "this renderer is not supported by Java shader packs yet";
            if (!ok) {
                result.error = std::string(stages[i].fragment ? "fragment" : "vertex") + " shader conversion failed: " + err;
                return result;
            }
            *sources[i] = cross.source;

            std::vector<ContainerUniform> uniforms;
            const bool hasBlock = program.uniformRegs > 0 && cross.source.find("IrisGlobals") != std::string::npos;
            if (hasBlock) {
                ContainerUniform u;
                u.name = "IrisGlobals";
                u.type = static_cast<uint8_t>(kUniformTypeVec4 | (stages[i].fragment ? kUniformFragmentBit : 0));
                u.num = static_cast<uint8_t>(std::min(program.uniformRegs, 255));
                u.regIndex = 0;
                u.regCount = static_cast<uint16_t>(program.uniformRegs);
                uniforms.push_back(u);
            }
            for (const ContainerUniform& u : cross.samplerUniforms) uniforms.push_back(u);
            for (const SamplerInfo& s : cross.samplers) {
                const auto it = stageOf.find(s.name);
                if (it != stageOf.end() && it->second != s.stage) {
                    result.error = "sampler '" + s.name + "' has different bindings in the two stages";
                    return result;
                }
                if (it == stageOf.end()) {
                    stageOf[s.name] = s.stage;
                    result.samplers.push_back(s);
                }
            }

            std::vector<uint8_t> code;
            const void* codePtr = nullptr;
            size_t codeSize = 0;
            if (options.kind == BackendKind::OpenGL) {
                codePtr = cross.source.data();
                codeSize = cross.source.size();
            } else {
                if (!options.hlslCompiler) {
                    result.error = "no HLSL compiler available";
                    return result;
                }
                std::string cerr;
                if (!options.hlslCompiler(cross.source, stages[i].fragment, code, cerr)) {
                    result.error = std::string(stages[i].fragment ? "pixel" : "vertex") + " shader: HLSL compile failed: " + cerr;
                    return result;
                }
                codePtr = code.data();
                codeSize = code.size();
            }
            const uint16_t cbBytes = options.kind == BackendKind::OpenGL ? 0 : static_cast<uint16_t>(program.uniformRegs * 16);
            *outputs[i] = WriteBgfxShader(stages[i].fragment, uniforms, codePtr, codeSize,
                                          stages[i].fragment ? std::vector<uint16_t>{} : cross.attributes, cbBytes, interfaceHash);
        }
        result.ok = true;
        return result;
    }
}
