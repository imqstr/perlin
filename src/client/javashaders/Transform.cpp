#include "client/javashaders/Transform.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace perlin::jshader {
    namespace {
        struct Tok {
            enum Kind { Ident, Number, Punct, Directive } kind = Punct;
            std::string text;
            std::string ws;
        };

        bool IsIdStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
        bool IsIdChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

        std::vector<Tok> Lex(const std::string& s) {
            std::vector<Tok> out;
            size_t i = 0;
            std::string ws;
            bool lineStart = true;
            while (i < s.size()) {
                const char c = s[i];
                if (c == '\n') {
                    ws += c;
                    i++;
                    lineStart = true;
                    continue;
                }
                if (c == ' ' || c == '\t' || c == '\r') {
                    ws += c;
                    i++;
                    continue;
                }
                if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
                    while (i < s.size() && s[i] != '\n') i++;
                    ws += ' ';
                    continue;
                }
                if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
                    size_t end = s.find("*/", i + 2);
                    const size_t stop = end == std::string::npos ? s.size() : end + 2;
                    for (size_t k = i; k < stop; k++)
                        if (s[k] == '\n') ws += '\n';
                    ws += ' ';
                    i = stop;
                    continue;
                }
                if (c == '#' && lineStart) {
                    size_t e = i;
                    while (e < s.size() && s[e] != '\n') {
                        if (s[e] == '\\' && e + 1 < s.size() && (s[e + 1] == '\n' || s[e + 1] == '\r')) {
                            e += 2;
                            if (e < s.size() && s[e - 1] == '\r' && s[e] == '\n') e++;
                            continue;
                        }
                        e++;
                    }
                    out.push_back({Tok::Directive, s.substr(i, e - i), ws});
                    ws.clear();
                    i = e;
                    continue;
                }
                lineStart = false;
                if (IsIdStart(c)) {
                    size_t e = i;
                    while (e < s.size() && IsIdChar(s[e])) e++;
                    out.push_back({Tok::Ident, s.substr(i, e - i), ws});
                    ws.clear();
                    i = e;
                    continue;
                }
                if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
                    size_t e = i;
                    if (c == '0' && e + 1 < s.size() && (s[e + 1] == 'x' || s[e + 1] == 'X')) {
                        e += 2;
                        while (e < s.size() && std::isxdigit(static_cast<unsigned char>(s[e]))) e++;
                    } else {
                        while (e < s.size() && (std::isdigit(static_cast<unsigned char>(s[e])) || s[e] == '.')) e++;
                        if (e < s.size() && (s[e] == 'e' || s[e] == 'E')) {
                            size_t k = e + 1;
                            if (k < s.size() && (s[k] == '+' || s[k] == '-')) k++;
                            if (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) {
                                e = k;
                                while (e < s.size() && std::isdigit(static_cast<unsigned char>(s[e]))) e++;
                            }
                        }
                    }
                    while (e < s.size() && (s[e] == 'f' || s[e] == 'F' || s[e] == 'u' || s[e] == 'U' || s[e] == 'l' || s[e] == 'L')) e++;
                    out.push_back({Tok::Number, s.substr(i, e - i), ws});
                    ws.clear();
                    i = e;
                    continue;
                }
                out.push_back({Tok::Punct, std::string(1, c), ws});
                ws.clear();
                i++;
            }
            if (!ws.empty())
                out.push_back({Tok::Punct, "", ws});
            return out;
        }

        bool IsOpaqueType(const std::string& t) {
            return t.compare(0, 7, "sampler") == 0 || t.compare(0, 8, "isampler") == 0 || t.compare(0, 8, "usampler") == 0 ||
                   t.compare(0, 5, "image") == 0 || t.compare(0, 6, "iimage") == 0 || t.compare(0, 6, "uimage") == 0 ||
                   t == "atomic_uint";
        }

        const std::unordered_set<std::string>& BuiltinTypes() {
            static const std::unordered_set<std::string> set = {
                "void", "bool", "int", "uint", "float", "double", "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4", "uvec2",
                "uvec3", "uvec4", "bvec2", "bvec3", "bvec4", "mat2", "mat3", "mat4", "mat2x2", "mat2x3", "mat2x4", "mat3x2",
                "mat3x3", "mat3x4", "mat4x2", "mat4x3", "mat4x4", "dvec2", "dvec3", "dvec4"};
            return set;
        }

        bool IsType(const std::string& t) { return BuiltinTypes().count(t) != 0 || IsOpaqueType(t); }

        bool IsQualifier(const std::string& t) {
            static const std::unordered_set<std::string> set = {
                "const", "uniform", "varying", "attribute", "in", "out", "inout", "flat", "smooth", "noperspective", "centroid",
                "invariant", "precise", "highp", "mediump", "lowp", "buffer", "shared", "coherent", "volatile", "restrict",
                "readonly", "writeonly", "sample", "patch"};
            return set.count(t) != 0;
        }

        std::string Join(const std::vector<Tok>& t, size_t a, size_t b) {
            std::string out;
            for (size_t i = a; i < b && i < t.size(); i++) {
                out += t[i].ws;
                out += t[i].text;
            }
            return out;
        }

        int TypeSlots(const std::string& type) {
            if (type == "mat2" || type == "mat2x2" || type == "mat2x3" || type == "mat2x4") return 2;
            if (type == "mat3" || type == "mat3x2" || type == "mat3x3" || type == "mat3x4") return 3;
            if (type == "mat4" || type == "mat4x2" || type == "mat4x3" || type == "mat4x4") return 4;
            if (type == "dvec3" || type == "dvec4") return 2;
            return 1;
        }

        struct BuiltinUse {
            bool modelView = false, modelViewInv = false, proj = false, projInv = false, normalMat = false;
            bool textureMat = false, fogColor = false, fogDensity = false, fogStart = false, fogEnd = false, fogScale = false;
            bool fogCoord = false, fragCoord = false, shadow2D = false, shadow2DLod = false, shadow2DProj = false;
            bool vertexIndex = false;
            int fragData = 0;
            bool fragColor = false;
        };

        const std::map<std::string, std::string>& TextureFunctionMap() {
            static const std::map<std::string, std::string> m = {
                {"texture1D", "texture"}, {"texture2D", "texture"}, {"texture3D", "texture"}, {"textureCube", "texture"},
                {"texture1DLod", "textureLod"}, {"texture2DLod", "textureLod"}, {"texture3DLod", "textureLod"},
                {"textureCubeLod", "textureLod"}, {"texture1DProj", "textureProj"}, {"texture2DProj", "textureProj"},
                {"texture3DProj", "textureProj"}, {"texture2DProjLod", "textureProjLod"}, {"texture2DGrad", "textureGrad"},
                {"texture3DGrad", "textureGrad"}, {"texelFetch2D", "texelFetch"}, {"texelFetch3D", "texelFetch"},
                {"texelFetch1D", "texelFetch"}, {"textureSize2D", "textureSize"}, {"textureSize3D", "textureSize"}};
            return m;
        }

        const std::unordered_set<std::string>& NewKeywords() {
            static const std::unordered_set<std::string> s = {"sample", "input", "output", "filter", "common", "partition", "active",
                                                               "buffer", "patch", "shared", "coherent", "restrict", "readonly",
                                                               "writeonly", "resource", "subroutine", "interface", "inline",
                                                               "noinline", "public", "static", "extern", "external", "packed",
                                                               "template", "this", "union", "enum", "typedef", "class", "using",
                                                               "namespace", "cast", "goto", "long", "short", "half", "fixed",
                                                               "unsigned", "sizeof", "superp", "row_major", "smooth"};
            return s;
        }

        const std::unordered_set<std::string>& Builtin450Functions() {
            static const std::unordered_set<std::string> s = {
                "round", "roundEven", "trunc", "sinh", "cosh", "tanh", "asinh", "acosh", "atanh", "isnan", "isinf", "transpose",
                "inverse", "determinant", "outerProduct", "modf", "frexp", "ldexp", "fma", "findLSB", "findMSB", "bitCount",
                "mix", "smoothstep", "step", "clamp", "lessThan", "greaterThan", "equal", "notEqual", "any", "all", "not",
                "dFdx", "dFdy", "fwidth", "noise1", "noise2", "noise3", "noise4", "reflect", "refract", "faceforward",
                "distance", "length", "normalize", "cross", "dot", "pow", "exp", "log", "exp2", "log2", "sqrt", "inversesqrt",
                "abs", "sign", "floor", "ceil", "fract", "mod", "min", "max", "radians", "degrees", "sin", "cos", "tan", "asin",
                "acos", "atan", "texture", "textureLod", "textureProj", "textureGrad", "texelFetch", "textureSize",
                "packUnorm4x8", "unpackUnorm4x8", "packHalf2x16", "unpackHalf2x16", "uaddCarry", "usubBorrow", "bitfieldExtract",
                "bitfieldInsert", "bitfieldReverse", "floatBitsToInt", "floatBitsToUint", "intBitsToFloat", "uintBitsToFloat"};
            return s;
        }

        std::string Slot(int n) { return std::to_string(n); }
    }

    const std::vector<UniformDecl>& BuiltinUniformDecls() {
        static const std::vector<UniformDecl> decls = {
            {"iris_ModelViewMat", "mat4", "", 0},        {"iris_ModelViewMatInverse", "mat4", "", 0},
            {"iris_ProjMat", "mat4", "", 0},             {"iris_ProjMatInverse", "mat4", "", 0},
            {"iris_NormalMat", "mat3", "", 0},           {"iris_TextureMat", "mat4", "2", 2},
            {"iris_FogColor", "vec4", "", 0},            {"iris_FogDensity", "float", "", 0},
            {"iris_FogStart", "float", "", 0},           {"iris_FogEnd", "float", "", 0},
            {"iris_FogScale", "float", "", 0},           {"iris_ViewHeight", "float", "", 0},
        };
        return decls;
    }

    namespace {
        struct Statement {
            size_t begin = 0, end = 0;
            bool function = false;
        };

        struct Prepared {
            Stage stage;
            std::vector<Tok> tokens;
            std::vector<Statement> statements;
            int version = 120;
            bool compat = false;
        };

        std::vector<Statement> SplitStatements(const std::vector<Tok>& t) {
            std::vector<Statement> out;
            size_t i = 0;
            while (i < t.size()) {
                if (t[i].kind == Tok::Directive) {
                    out.push_back({i, i + 1, false});
                    i++;
                    continue;
                }
                if (t[i].text.empty()) {
                    i++;
                    continue;
                }
                const size_t start = i;
                int paren = 0, bracket = 0;
                bool sawParen = false;
                bool finished = false;
                while (i < t.size() && !finished) {
                    const std::string& x = t[i].text;
                    if (t[i].kind == Tok::Directive)
                        break;
                    if (x == "(") { paren++; sawParen = true; }
                    else if (x == ")") paren--;
                    else if (x == "[") bracket++;
                    else if (x == "]") bracket--;
                    else if (x == ";" && paren == 0 && bracket == 0) {
                        i++;
                        out.push_back({start, i, false});
                        finished = true;
                        break;
                    } else if (x == "{" && paren == 0) {
                        const bool isFunction = sawParen && i > start && t[i - 1].text == ")";
                        int depth = 0;
                        size_t k = i;
                        for (; k < t.size(); k++) {
                            if (t[k].text == "{") depth++;
                            else if (t[k].text == "}") {
                                depth--;
                                if (depth == 0) { k++; break; }
                            }
                        }
                        i = k;
                        if (isFunction) {
                            out.push_back({start, i, true});
                            finished = true;
                            break;
                        }
                        continue;
                    }
                    i++;
                }
                if (!finished && i > start)
                    out.push_back({start, i, false});
            }
            return out;
        }

        size_t MatchClose(const std::vector<Tok>& t, size_t open) {
            const std::string o = t[open].text;
            const std::string c = o == "(" ? ")" : o == "[" ? "]" : "}";
            int depth = 0;
            for (size_t i = open; i < t.size(); i++) {
                if (t[i].text == o) depth++;
                else if (t[i].text == c) {
                    depth--;
                    if (depth == 0) return i;
                }
            }
            return t.size();
        }

        bool ParseIntLiteral(const std::string& s, int& out) {
            if (s.empty()) return false;
            char* end = nullptr;
            const long v = std::strtol(s.c_str(), &end, 0);
            while (end && *end && (*end == 'u' || *end == 'U')) end++;
            if (end == s.c_str() || (end && *end)) return false;
            out = static_cast<int>(v);
            return true;
        }

        std::string StripSpace(const std::string& s) {
            std::string out;
            for (char c : s)
                if (!std::isspace(static_cast<unsigned char>(c))) out += c;
            return out;
        }

        struct DeclInfo {
            std::vector<std::string> qualifiers;
            std::string layout;
            std::string type;
            struct Name {
                std::string name;
                std::string arraySize;
                std::string init;
            };
            std::vector<Name> names;
            bool valid = false;
        };

        DeclInfo ParseDeclaration(const std::vector<Tok>& t, size_t begin, size_t end) {
            DeclInfo d;
            size_t i = begin;
            while (i < end) {
                const std::string& x = t[i].text;
                if (x == "layout" && i + 1 < end && t[i + 1].text == "(") {
                    const size_t close = MatchClose(t, i + 1);
                    d.layout = Join(t, i + 2, close);
                    i = close + 1;
                    continue;
                }
                if (t[i].kind == Tok::Ident && IsQualifier(x)) {
                    d.qualifiers.push_back(x);
                    i++;
                    continue;
                }
                break;
            }
            if (i >= end || t[i].kind != Tok::Ident)
                return d;
            if (t[i].text == "struct")
                return d;
            d.type = t[i].text;
            i++;
            while (i < end) {
                if (t[i].kind != Tok::Ident)
                    return d;
                DeclInfo::Name n;
                n.name = t[i].text;
                i++;
                while (i < end && t[i].text == "[") {
                    const size_t close = MatchClose(t, i);
                    n.arraySize = StripSpace(Join(t, i + 1, close));
                    i = close + 1;
                }
                if (i < end && t[i].text == "=") {
                    i++;
                    size_t k = i;
                    int depth = 0;
                    for (; k < end; k++) {
                        if (t[k].text == "(" || t[k].text == "[" || t[k].text == "{") depth++;
                        else if (t[k].text == ")" || t[k].text == "]" || t[k].text == "}") depth--;
                        else if ((t[k].text == "," || t[k].text == ";") && depth == 0) break;
                    }
                    n.init = Join(t, i, k);
                    i = k;
                }
                d.names.push_back(n);
                if (i < end && t[i].text == ",") {
                    i++;
                    continue;
                }
                break;
            }
            d.valid = !d.names.empty();
            return d;
        }

        bool HasQualifier(const DeclInfo& d, const char* q) {
            return std::find(d.qualifiers.begin(), d.qualifiers.end(), q) != d.qualifiers.end();
        }

        std::string LayoutWithout(const std::string& layout, const char* key, int* value = nullptr) {
            std::vector<std::string> parts;
            std::string cur;
            for (char c : layout) {
                if (c == ',') { parts.push_back(cur); cur.clear(); }
                else cur += c;
            }
            if (!cur.empty()) parts.push_back(cur);
            std::string out;
            for (const std::string& p : parts) {
                const std::string s = StripSpace(p);
                if (s.compare(0, std::char_traits<char>::length(key), key) == 0 && s.size() > std::char_traits<char>::length(key) &&
                    s[std::char_traits<char>::length(key)] == '=') {
                    if (value) *value = std::atoi(s.c_str() + std::char_traits<char>::length(key) + 1);
                    continue;
                }
                if (!s.empty()) {
                    if (!out.empty()) out += ", ";
                    out += s;
                }
            }
            return out;
        }

        struct StageContext {
            Stage stage;
            const TransformOptions* options;
            std::string error;
            BuiltinUse use;
            VertexInputs vin;
            std::vector<UniformDecl> uniforms;
            std::vector<SamplerDecl> samplers;
            std::vector<InterfaceVar> inputs, outputs;
            std::vector<std::string> bodyDecls;
            std::set<std::string> userFunctions;
            std::vector<int> fragmentLocations;
        };

        std::string ReplaceSampler(const std::string& type) { return type; }
    }

    namespace {
        std::string BuildPrefix(const TransformOptions& options) {
            std::string out = "#version 450\n";
            for (const auto& m : options.macros)
                out += "#define " + m.first + " " + m.second + "\n";
            return out;
        }

        bool ExtractVersion(const std::string& src, int& version, bool& compat, std::string& body) {
            std::istringstream in(src);
            std::string line;
            body.clear();
            version = 110;
            compat = false;
            while (std::getline(in, line)) {
                size_t a = line.find_first_not_of(" \t");
                if (a != std::string::npos && line[a] == '#') {
                    size_t b = line.find_first_not_of(" \t", a + 1);
                    if (b != std::string::npos && line.compare(b, 7, "version") == 0) {
                        std::istringstream vs(line.substr(b + 7));
                        std::string v, profile;
                        vs >> v >> profile;
                        version = std::atoi(v.c_str());
                        compat = profile == "compatibility";
                        body += "\n";
                        continue;
                    }
                    if (b != std::string::npos && line.compare(b, 9, "extension") == 0) {
                        body += "\n";
                        continue;
                    }
                }
                body += line;
                body += '\n';
            }
            return true;
        }

        int ArrayCountOf(const std::string& expr) {
            int n = 0;
            if (ParseIntLiteral(expr, n))
                return n;
            return -1;
        }

        int EvalSize(const std::string& expr, const std::map<std::string, int>& consts) {
            std::string e = StripSpace(expr);
            int n = 0;
            if (ParseIntLiteral(e, n)) return n;
            const auto it = consts.find(e);
            if (it != consts.end()) return it->second;
            for (char op : {'*', '+', '-', '/'}) {
                const size_t at = e.find(op);
                if (at != std::string::npos && at > 0) {
                    const int a = EvalSize(e.substr(0, at), consts), b = EvalSize(e.substr(at + 1), consts);
                    if (a < 0 || b < 0) return -1;
                    return op == '*' ? a * b : op == '+' ? a + b : op == '-' ? a - b : (b ? a / b : -1);
                }
            }
            return -1;
        }
    }

    namespace {
        StageResult TransformStage(Stage stage, const std::string& rawSource, const TransformOptions& options) {
            StageResult result;
            result.stage = stage;
            if (!options.preprocess) {
                result.ok = false;
                result.error = "no preprocessor available";
                return result;
            }
            int version = 120;
            bool compat = false;
            std::string body;
            ExtractVersion(rawSource, version, compat, body);

            std::string error;
            const std::string pre = options.preprocess(BuildPrefix(options) + body, stage, error);
            if (pre.empty() && !error.empty()) {
                result.ok = false;
                result.error = "preprocess: " + error;
                return result;
            }

            std::vector<Tok> tokens = Lex(pre);
            for (Tok& tk : tokens) {
                if (tk.kind == Tok::Directive) {
                    std::string d = tk.text;
                    size_t a = d.find_first_not_of("# \t");
                    const std::string name = a == std::string::npos ? std::string() : d.substr(a, 8);
                    if (name.compare(0, 7, "version") == 0 || name.compare(0, 9, "extension") == 0 || name.compare(0, 4, "line") == 0 ||
                        name.compare(0, 6, "pragma") == 0) {
                        tk.text.clear();
                        tk.kind = Tok::Punct;
                    }
                }
            }
            const std::vector<Statement> statements = SplitStatements(tokens);

            StageContext ctx;
            ctx.stage = stage;
            ctx.options = &options;

            std::set<std::string> renameFunctions;
            if (version < 130) {
                for (const Statement& st : statements) {
                    if (!st.function) continue;
                    size_t p = st.begin;
                    while (p < st.end && tokens[p].text != "(") p++;
                    if (p > st.begin && tokens[p - 1].kind == Tok::Ident && Builtin450Functions().count(tokens[p - 1].text))
                        renameFunctions.insert(tokens[p - 1].text);
                }
            }
            for (const Statement& st : statements) {
                if (!st.function) continue;
                size_t p = st.begin;
                while (p < st.end && tokens[p].text != "(") p++;
                if (p > st.begin && tokens[p - 1].kind == Tok::Ident)
                    ctx.userFunctions.insert(tokens[p - 1].text);
            }
            const bool userDefinesTexture = ctx.userFunctions.count("texture") != 0;

            std::set<std::string> declaredNames;
            for (size_t i = 1; i < tokens.size(); i++)
                if (tokens[i].kind == Tok::Ident && tokens[i - 1].kind == Tok::Ident && (IsType(tokens[i - 1].text) || IsQualifier(tokens[i - 1].text)))
                    declaredNames.insert(tokens[i].text);

            std::map<std::string, std::string> constInts;
            std::vector<std::string> out(tokens.size());
            for (size_t i = 0; i < tokens.size(); i++) {
                Tok& tk = tokens[i];
                if (tk.kind != Tok::Ident) {
                    out[i] = tk.ws + tk.text;
                    continue;
                }
                const std::string& id = tk.text;
                const bool nextCall = i + 1 < tokens.size() && tokens[i + 1].text == "(";
                const bool afterDot = i > 0 && tokens[i - 1].text == ".";
                std::string rep;
                if (afterDot) {
                    rep = id;
                } else if (renameFunctions.count(id)) {
                    rep = "iris_" + id;
                } else if (!declaredNames.count(id) && (id == "projectionMatrix" || id == "modelViewMatrix" || id == "normalMatrix" ||
                                                        id == "modelViewMatrixInverse" || id == "projectionMatrixInverse" ||
                                                        id == "textureMatrix" || id == "chunkOffset" || id == "vaPosition" ||
                                                        id == "vaColor" || id == "vaUV0" || id == "vaUV2" || id == "vaNormal")) {
                    if (id == "projectionMatrix") { ctx.use.proj = true; rep = "iris_ProjMat"; }
                    else if (id == "modelViewMatrix") { ctx.use.modelView = true; rep = "iris_ModelViewMat"; }
                    else if (id == "normalMatrix") { ctx.use.normalMat = true; rep = "iris_NormalMat"; }
                    else if (id == "modelViewMatrixInverse") { ctx.use.modelViewInv = true; rep = "iris_ModelViewMatInverse"; }
                    else if (id == "projectionMatrixInverse") { ctx.use.projInv = true; rep = "iris_ProjMatInverse"; }
                    else if (id == "textureMatrix") { ctx.use.textureMat = true; rep = "iris_TextureMat[0]"; }
                    else if (id == "chunkOffset") rep = "vec3(0.0)";
                    else if (id == "vaPosition") { ctx.vin.position = true; rep = "iris_Position"; }
                    else if (id == "vaColor") { ctx.vin.color = true; rep = "iris_Color"; }
                    else if (id == "vaUV0") { ctx.vin.uv0 = true; rep = "iris_UV0"; }
                    else if (id == "vaUV2") { ctx.vin.uv2 = true; rep = "ivec2(iris_UV2)"; }
                    else { ctx.vin.normal = true; rep = "iris_Normal"; }
                } else if (id == "gl_Vertex") {
                    ctx.vin.position = true;
                    rep = "vec4(iris_Position, 1.0)";
                } else if (id == "gl_Color") {
                    ctx.vin.color = true;
                    rep = "iris_Color";
                } else if (id == "gl_Normal") {
                    ctx.vin.normal = true;
                    rep = "iris_Normal";
                } else if (id == "gl_MultiTexCoord0") {
                    ctx.vin.uv0 = true;
                    rep = "vec4(iris_UV0, 0.0, 1.0)";
                } else if (id == "gl_MultiTexCoord1") {
                    ctx.vin.uv1 = true;
                    rep = "vec4(iris_UV1, 0.0, 1.0)";
                } else if (id == "gl_MultiTexCoord2") {
                    ctx.vin.uv2 = true;
                    rep = "vec4(iris_UV2, 0.0, 1.0)";
                } else if (id.compare(0, 16, "gl_MultiTexCoord") == 0) {
                    rep = "vec4(0.0, 0.0, 0.0, 1.0)";
                } else if (id == "gl_ModelViewMatrix") {
                    ctx.use.modelView = true;
                    rep = "iris_ModelViewMat";
                } else if (id == "gl_ModelViewMatrixInverse") {
                    ctx.use.modelViewInv = true;
                    rep = "iris_ModelViewMatInverse";
                } else if (id == "gl_ProjectionMatrix") {
                    ctx.use.proj = true;
                    rep = "iris_ProjMat";
                } else if (id == "gl_ProjectionMatrixInverse") {
                    ctx.use.projInv = true;
                    rep = "iris_ProjMatInverse";
                } else if (id == "gl_ModelViewProjectionMatrix") {
                    ctx.use.modelView = ctx.use.proj = true;
                    rep = "(iris_ProjMat * iris_ModelViewMat)";
                } else if (id == "gl_NormalMatrix") {
                    ctx.use.normalMat = true;
                    rep = "iris_NormalMat";
                } else if (id == "gl_TextureMatrix") {
                    ctx.use.textureMat = true;
                    rep = "iris_TextureMat";
                } else if (id == "ftransform" && nextCall) {
                    ctx.use.modelView = ctx.use.proj = true;
                    ctx.vin.position = true;
                    rep = "(iris_ProjMat * iris_ModelViewMat * vec4(iris_Position, 1.0))";
                    if (i + 2 < tokens.size() && tokens[i + 2].text == ")") {
                        out[i] = tk.ws + rep;
                        out[i + 1].clear();
                        out[i + 2].clear();
                        tokens[i + 1].text.clear();
                        tokens[i + 2].text.clear();
                        i += 2;
                        continue;
                    }
                } else if (id == "gl_Fog" && i + 2 < tokens.size() && tokens[i + 1].text == ".") {
                    const std::string field = tokens[i + 2].text;
                    if (field == "color") { ctx.use.fogColor = true; rep = "iris_FogColor"; }
                    else if (field == "density") { ctx.use.fogDensity = true; rep = "iris_FogDensity"; }
                    else if (field == "start") { ctx.use.fogStart = true; rep = "iris_FogStart"; }
                    else if (field == "end") { ctx.use.fogEnd = true; rep = "iris_FogEnd"; }
                    else if (field == "scale") { ctx.use.fogScale = true; rep = "iris_FogScale"; }
                    else rep = "0.0";
                    out[i] = tk.ws + rep;
                    out[i + 1].clear();
                    out[i + 2].clear();
                    tokens[i + 1].text.clear();
                    tokens[i + 2].text.clear();
                    i += 2;
                    continue;
                } else if (id == "gl_FogFragCoord") {
                    ctx.use.fogCoord = true;
                    rep = "iris_FogFragCoord";
                } else if (id == "gl_FragCoord") {
                    ctx.use.fragCoord = true;
                    rep = options.flipFragCoordY ? "iris_FragCoord" : "gl_FragCoord";
                } else if (id == "gl_FragData" && i + 3 < tokens.size() && tokens[i + 1].text == "[" && tokens[i + 3].text == "]") {
                    const int idx = std::atoi(tokens[i + 2].text.c_str());
                    ctx.use.fragData = std::max(ctx.use.fragData, idx + 1);
                    out[i] = tk.ws + "iris_FragData" + Slot(idx);
                    out[i + 1].clear();
                    out[i + 2].clear();
                    out[i + 3].clear();
                    tokens[i + 1].text.clear();
                    tokens[i + 2].text.clear();
                    tokens[i + 3].text.clear();
                    i += 3;
                    continue;
                } else if (id == "gl_FragColor") {
                    ctx.use.fragColor = true;
                    ctx.use.fragData = std::max(ctx.use.fragData, 1);
                    rep = "iris_FragData0";
                } else if (id == "gl_VertexID") {
                    rep = "gl_VertexIndex";
                } else if (id == "gl_InstanceID") {
                    rep = "gl_InstanceIndex";
                } else if (id == "shadow2D" && nextCall) {
                    ctx.use.shadow2D = true;
                    rep = "iris_shadow2D";
                } else if (id == "shadow2DLod" && nextCall) {
                    ctx.use.shadow2DLod = true;
                    rep = "iris_shadow2DLod";
                } else if (id == "shadow2DProj" && nextCall) {
                    ctx.use.shadow2DProj = true;
                    rep = "iris_shadow2DProj";
                } else if (TextureFunctionMap().count(id) && nextCall && !ctx.userFunctions.count(id)) {
                    rep = TextureFunctionMap().at(id);
                } else if (id == "texture" && !nextCall && !userDefinesTexture) {
                    rep = "iris_texture";
                } else if (NewKeywords().count(id) && !nextCall) {
                    if (id == "smooth") {
                        const bool qualifierUse = i + 1 < tokens.size() && tokens[i + 1].kind == Tok::Ident &&
                                                  (IsType(tokens[i + 1].text) || IsQualifier(tokens[i + 1].text));
                        rep = qualifierUse ? id : "iris_kw_" + id;
                    } else if (id == "sample" && i + 1 < tokens.size() && tokens[i + 1].kind == Tok::Ident && IsType(tokens[i + 1].text)) {
                        rep = id;
                    } else {
                        rep = "iris_kw_" + id;
                    }
                } else {
                    rep = id;
                }
                out[i] = tk.ws + rep;
            }

            std::string bodyText;
            std::map<std::string, int> arrayConsts;
            std::set<std::string> seenUniforms, seenSamplers;
            bool hasMain = false;
            std::string uniformBlockPlaceholder = "/*IRIS_UNIFORM_BLOCK*/";
            bool blockPlaced = false;
            std::vector<std::string> vertexAttribLines;
            std::set<std::string> userAttribNames;

            auto emitRange = [&](size_t a, size_t b) {
                std::string s;
                for (size_t k = a; k < b; k++) s += out[k];
                return s;
            };

            for (const Statement& st : statements) {
                if (st.function) {
                    size_t p = st.begin;
                    while (p < st.end && tokens[p].text != "(") p++;
                    std::string text = emitRange(st.begin, st.end);
                    if (p > st.begin && tokens[p - 1].text == "main") {
                        hasMain = true;
                        size_t at = text.find("main");
                        if (at != std::string::npos) text.replace(at, 4, "iris_user_main");
                    }
                    bodyText += text;
                    continue;
                }
                if (tokens[st.begin].kind == Tok::Directive) {
                    bodyText += out[st.begin];
                    continue;
                }
                const DeclInfo d = ParseDeclaration(tokens, st.begin, st.end);
                if (!d.valid) {
                    bodyText += emitRange(st.begin, st.end);
                    continue;
                }
                const bool isUniform = HasQualifier(d, "uniform");
                const bool isVarying = HasQualifier(d, "varying");
                const bool isAttribute = HasQualifier(d, "attribute");
                const bool isIn = HasQualifier(d, "in");
                const bool isOut = HasQualifier(d, "out");
                const bool isConst = HasQualifier(d, "const");

                if (isConst && !isUniform && d.type == "int") {
                    for (const auto& n : d.names) {
                        int v = 0;
                        if (n.arraySize.empty() && ParseIntLiteral(StripSpace(n.init), v))
                            arrayConsts[n.name] = v;
                    }
                }

                if (isUniform) {
                    const std::string typeName = d.type == "texture" ? "iris_texture" : d.type;
                    if (IsOpaqueType(d.type)) {
                        for (const auto& n : d.names) {
                            std::string name = n.name;
                            if (name == "texture") name = "iris_texture";
                            if (!seenSamplers.insert(name).second) continue;
                            SamplerDecl s;
                            s.name = name;
                            s.type = d.type;
                            s.image = d.type.compare(0, 5, "image") == 0 || d.type.compare(0, 6, "iimage") == 0 || d.type.compare(0, 6, "uimage") == 0;
                            s.imageFormat = LayoutWithout(d.layout, "binding");
                            ctx.samplers.push_back(s);
                        }
                        bodyText += "/*SAMPLERS*/";
                        continue;
                    }
                    for (const auto& n : d.names) {
                        if (!seenUniforms.insert(n.name).second) continue;
                        UniformDecl u;
                        u.name = n.name;
                        u.type = typeName;
                        u.arraySize = n.arraySize;
                        u.arrayCount = n.arraySize.empty() ? 0 : EvalSize(n.arraySize, arrayConsts);
                        if (!n.arraySize.empty() && u.arrayCount < 0) {
                            result.ok = false;
                            result.error = "uniform array size is not a constant: " + n.name + "[" + n.arraySize + "]";
                        }
                        ctx.uniforms.push_back(u);
                    }
                    if (!blockPlaced) {
                        blockPlaced = true;
                        bodyText += uniformBlockPlaceholder;
                    }
                    continue;
                }

                bool toFragmentOut = false;
                const bool vertexStage = stage == Stage::Vertex;
                if (isAttribute || (vertexStage && isIn)) {
                    std::string layoutRest = d.layout;
                    int loc = -1;
                    layoutRest = LayoutWithout(d.layout, "location", &loc);
                    for (const auto& n : d.names) {
                        InterfaceVar v;
                        v.name = n.name;
                        v.type = d.type;
                        v.arrayCount = n.arraySize.empty() ? 0 : EvalSize(n.arraySize, arrayConsts);
                        v.location = loc;
                        ctx.inputs.push_back(v);
                        userAttribNames.insert(n.name);
                        if (n.name == "mc_Entity") ctx.vin.mcEntity = true;
                        else if (n.name == "mc_midTexCoord") ctx.vin.mcMidTexCoord = true;
                        else if (n.name == "at_tangent") ctx.vin.atTangent = true;
                        else if (n.name == "at_midBlock") ctx.vin.atMidBlock = true;
                    }
                    bodyText += "/*ATTR*/";
                    continue;
                }
                if (isVarying) {
                    toFragmentOut = false;
                }
                const bool outputDecl = isOut || (isVarying && vertexStage);
                const bool inputDecl = isIn || (isVarying && !vertexStage);
                if (outputDecl || inputDecl) {
                    int loc = -1;
                    LayoutWithout(d.layout, "location", &loc);
                    std::string interp;
                    for (const std::string& q : d.qualifiers)
                        if (q == "flat" || q == "centroid" || q == "noperspective" || q == "smooth") interp = q == "smooth" ? "" : q;
                    for (const auto& n : d.names) {
                        InterfaceVar v;
                        v.name = n.name;
                        v.type = d.type;
                        v.arrayCount = n.arraySize.empty() ? 0 : EvalSize(n.arraySize, arrayConsts);
                        v.interpolation = interp;
                        v.location = loc;
                        if (stage == Stage::Fragment && outputDecl)
                            ctx.outputs.push_back(v);
                        else if (outputDecl)
                            ctx.outputs.push_back(v);
                        else
                            ctx.inputs.push_back(v);
                    }
                    bodyText += outputDecl ? (stage == Stage::Fragment ? "/*FRAGOUT*/" : "/*VARYING_OUT*/") : "/*VARYING_IN*/";
                    continue;
                }
                (void)toFragmentOut;
                bodyText += emitRange(st.begin, st.end);
            }
            if (!hasMain) {
                result.ok = false;
                result.error = "no main()";
            }

            result.uniforms = ctx.uniforms;
            result.samplers = ctx.samplers;
            result.inputs = ctx.inputs;
            result.outputs = ctx.outputs;
            result.vertexInputs = ctx.vin;
            result.glsl = bodyText;

            auto addBuiltin = [&](bool used, const char* name) {
                if (!used) return;
                for (const UniformDecl& b : BuiltinUniformDecls())
                    if (b.name == name) {
                        for (const UniformDecl& u : result.uniforms)
                            if (u.name == name) return;
                        result.uniforms.push_back(b);
                    }
            };
            addBuiltin(ctx.use.modelView, "iris_ModelViewMat");
            addBuiltin(ctx.use.modelViewInv, "iris_ModelViewMatInverse");
            addBuiltin(ctx.use.proj, "iris_ProjMat");
            addBuiltin(ctx.use.projInv, "iris_ProjMatInverse");
            addBuiltin(ctx.use.normalMat, "iris_NormalMat");
            addBuiltin(ctx.use.textureMat, "iris_TextureMat");
            addBuiltin(ctx.use.fogColor, "iris_FogColor");
            addBuiltin(ctx.use.fogDensity, "iris_FogDensity");
            addBuiltin(ctx.use.fogStart, "iris_FogStart");
            addBuiltin(ctx.use.fogEnd, "iris_FogEnd");
            addBuiltin(ctx.use.fogScale, "iris_FogScale");
            addBuiltin(options.flipFragCoordY && ctx.use.fragCoord, "iris_ViewHeight");

            std::string flags;
            if (ctx.use.fogCoord) flags += "FOGCOORD;";
            if (ctx.use.fragCoord) flags += "FRAGCOORD;";
            if (ctx.use.shadow2D) flags += "SHADOW2D;";
            if (ctx.use.shadow2DLod) flags += "SHADOW2DLOD;";
            if (ctx.use.shadow2DProj) flags += "SHADOW2DPROJ;";
            if (ctx.use.fragData) flags += "FRAGDATA=" + std::to_string(ctx.use.fragData) + ";";
            result.glsl = "/*FLAGS:" + flags + "*/" + result.glsl;
            return result;
        }

        bool HasFlag(const std::string& glsl, const std::string& flag) {
            const size_t end = glsl.find("*/");
            if (end == std::string::npos) return false;
            return glsl.substr(0, end).find(flag) != std::string::npos;
        }

        int FlagNumber(const std::string& glsl, const std::string& flag) {
            const size_t end = glsl.find("*/");
            const size_t at = glsl.find(flag + "=");
            if (end == std::string::npos || at == std::string::npos || at > end) return 0;
            return std::atoi(glsl.c_str() + at + flag.size() + 1);
        }

        std::string StripFlags(const std::string& glsl) {
            if (glsl.compare(0, 9, "/*FLAGS:") != 0) return glsl;
            const size_t end = glsl.find("*/");
            return end == std::string::npos ? glsl : glsl.substr(end + 2);
        }

        std::string ReplaceFirst(std::string text, const std::string& from, const std::string& to) {
            const size_t at = text.find(from);
            if (at != std::string::npos) text.replace(at, from.size(), to);
            return text;
        }

        std::string ReplaceAll(std::string text, const std::string& from, const std::string& to) {
            size_t at = 0;
            while ((at = text.find(from, at)) != std::string::npos) {
                text.replace(at, from.size(), to);
                at += to.size();
            }
            return text;
        }
    }

    ProgramResult TransformProgram(const ExpandedSource& vertexSource, const ExpandedSource& fragmentSource,
                                   const TransformOptions& options) {
        ProgramResult program;
        static const char* kDefaultVertex =
            "#version 120\nvarying vec4 color;\nvarying vec2 texcoord;\nvoid main(){ gl_Position = ftransform(); texcoord = gl_MultiTexCoord0.st; color = gl_Color; }\n";
        static const char* kDefaultFragment =
            "#version 120\nuniform sampler2D iris_texture_default;\nvarying vec4 color;\nvarying vec2 texcoord;\nvoid main(){ gl_FragData[0] = texture2D(iris_texture_default, texcoord) * color; }\n";
        const std::string vs = vertexSource.text.empty() ? kDefaultVertex : vertexSource.text;
        const std::string fs = fragmentSource.text.empty() ? kDefaultFragment : fragmentSource.text;
        program.vertex = TransformStage(Stage::Vertex, vs, options);
        program.fragment = TransformStage(Stage::Fragment, fs, options);
        if (!program.ok())
            return program;

        {
            std::vector<UniformDecl> merged = program.vertex.uniforms;
            for (const UniformDecl& u : program.fragment.uniforms) {
                bool found = false;
                for (const UniformDecl& m : merged)
                    if (m.name == u.name) {
                        found = true;
                        if (m.type != u.type || m.arrayCount != u.arrayCount) {
                            program.fragment.ok = false;
                            program.fragment.error = "uniform '" + u.name + "' is declared with different types in the two stages";
                        }
                    }
                if (!found) merged.push_back(u);
            }
            program.uniforms = std::move(merged);
            program.vertex.uniforms = program.uniforms;
            program.fragment.uniforms = program.uniforms;
            if (!program.ok())
                return program;
        }

        std::map<std::string, int> samplerBindings;
        {
            std::set<int> used;
            for (const StageResult* st : {&program.vertex, &program.fragment})
                for (const SamplerDecl& smp : st->samplers) {
                    const auto fixed = options.fixedSamplerSlots.find(smp.name);
                    if (fixed != options.fixedSamplerSlots.end()) { samplerBindings[smp.name] = fixed->second; used.insert(fixed->second); }
                }
            int cursor = 1;
            // Binding 0 is reserved for the IrisGlobals uniform buffer: Vulkan requires unique
            // set+binding pairs, while GL/D3D tolerate the UBO and first sampler sharing 0.
            for (const StageResult* st : {&program.vertex, &program.fragment})
                for (const SamplerDecl& smp : st->samplers) {
                    if (samplerBindings.count(smp.name)) continue;
                    while (used.count(cursor)) cursor++;
                    samplerBindings[smp.name] = cursor;
                    used.insert(cursor);
                }
        }

        struct Link {
            std::string type, interp;
            int arrayCount = 0;
            int location = -1;
            bool inVertexOut = false, inFragmentIn = false;
        };
        std::map<std::string, Link> links;
        bool fogCoord = HasFlag(program.vertex.glsl, "FOGCOORD") || HasFlag(program.fragment.glsl, "FOGCOORD");
        for (const InterfaceVar& v : program.vertex.outputs) {
            Link& l = links[v.name];
            l.type = v.type; l.interp = v.interpolation; l.arrayCount = v.arrayCount; l.inVertexOut = true;
            if (v.location >= 0) l.location = v.location;
        }
        for (const InterfaceVar& v : program.fragment.inputs) {
            Link& l = links[v.name];
            if (l.type.empty()) { l.type = v.type; l.arrayCount = v.arrayCount; }
            if (l.interp.empty()) l.interp = v.interpolation;
            l.inFragmentIn = true;
            if (v.location >= 0 && l.location < 0) l.location = v.location;
        }
        if (fogCoord) {
            Link& l = links["iris_FogFragCoord"];
            l.type = "float";
            l.inVertexOut = l.inFragmentIn = true;
        }
        std::set<int> taken;
        for (const auto& kv : links)
            if (kv.second.location >= 0) {
                const int slots = TypeSlots(kv.second.type) * std::max(1, kv.second.arrayCount);
                for (int s = 0; s < slots; s++) taken.insert(kv.second.location + s);
            }
        int next = 0;
        for (auto& kv : links) {
            Link& l = kv.second;
            if (l.location >= 0) continue;
            const int slots = TypeSlots(l.type) * std::max(1, l.arrayCount);
            for (;;) {
                bool free = true;
                for (int s = 0; s < slots; s++)
                    if (taken.count(next + s)) { free = false; break; }
                if (free) break;
                next++;
            }
            l.location = next;
            for (int s = 0; s < slots; s++) taken.insert(next + s);
            next += slots;
        }

        auto varyingDecl = [&](const std::string& name, const Link& l, bool out) {
            std::string s = "layout(location = " + std::to_string(l.location) + ") ";
            if (!l.interp.empty()) s += l.interp + " ";
            s += out ? "out " : "in ";
            s += l.type + " " + name;
            if (l.arrayCount > 0) s += "[" + std::to_string(l.arrayCount) + "]";
            return s + ";\n";
        };

        auto sharedPrelude = [&](StageResult& r, bool vertex) -> std::string {
            std::string glsl = StripFlags(r.glsl);
            std::string block;
            if (!r.uniforms.empty()) {
                std::string globals;
                int reg = 0;
                block = "layout(std140, set = 0, binding = 0) uniform IrisGlobals {\n";
                for (UniformDecl& u : r.uniforms) {
                    const std::string member = "iris_u_" + u.name;
                    const int count = std::max(1, u.arrayCount);
                    const bool matrix = u.type.compare(0, 3, "mat") == 0;
                    int comps = 1;
                    if (u.type.size() > 3 && (u.type.compare(0, 3, "vec") == 0)) comps = u.type[3] - '0';
                    else if (u.type.size() > 4 && (u.type.compare(0, 4, "ivec") == 0 || u.type.compare(0, 4, "uvec") == 0 || u.type.compare(0, 4, "bvec") == 0)) comps = u.type[4] - '0';
                    u.reg = reg;
                    u.regsPerElement = matrix ? 4 : 1;
                    u.regs = u.regsPerElement * count;
                    u.components = matrix ? (u.type == "mat2" ? 4 : u.type == "mat3" ? 9 : 16) : comps;
                    reg += u.regs;
                    block += std::string("    ") + (matrix ? "mat4 " : "vec4 ") + member;
                    if (count > 1 || u.arrayCount > 0) block += "[" + std::to_string(count) + "]";
                    block += ";\n";
                    auto unpack = [&](const std::string& ref) -> std::string {
                        const std::string& t = u.type;
                        if (t == "float") return ref + ".x";
                        if (t == "int") return "int(" + ref + ".x)";
                        if (t == "uint") return "uint(" + ref + ".x)";
                        if (t == "bool") return "(" + ref + ".x != 0.0)";
                        if (t == "vec2") return ref + ".xy";
                        if (t == "vec3") return ref + ".xyz";
                        if (t == "vec4" || t == "mat4") return ref;
                        if (t == "mat3" || t == "mat2") return t + "(" + ref + ")";
                        if (t == "ivec2" || t == "ivec3" || t == "ivec4" || t == "uvec2" || t == "uvec3" || t == "uvec4")
                            return t + "(" + ref + (t.back() == '2' ? ".xy" : t.back() == '3' ? ".xyz" : "") + ")";
                        if (t == "bvec2" || t == "bvec3" || t == "bvec4")
                            return "notEqual(" + ref + (t.back() == '2' ? ".xy" : t.back() == '3' ? ".xyz" : "") + ", vec" + t.back() + "(0.0))";
                        return ref;
                    };
                    if (u.arrayCount > 0) {
                        globals += u.type + " " + u.name + "[" + std::to_string(count) + "] = " + u.type + "[" + std::to_string(count) + "](";
                        for (int i = 0; i < count; i++) {
                            if (i) globals += ", ";
                            globals += unpack(member + "[" + std::to_string(i) + "]");
                        }
                        globals += ");\n";
                    } else {
                        globals += u.type + " " + u.name + " = " + unpack(member) + ";\n";
                    }
                }
                block += "};\n" + globals;
            }
            glsl = ReplaceFirst(glsl, "/*IRIS_UNIFORM_BLOCK*/", "");
            for (SamplerDecl& smp : r.samplers) smp.binding = samplerBindings.at(smp.name);
            std::string samplerText;
            for (const SamplerDecl& s : r.samplers) {
                samplerText += "layout(set = 0, binding = " + std::to_string(s.binding);
                if (s.image && !s.imageFormat.empty()) samplerText += ", " + s.imageFormat;
                samplerText += ") uniform " + s.type + " " + s.name + ";\n";
            }
            glsl = ReplaceAll(glsl, "/*SAMPLERS*/", "");
            return block + samplerText + "\x01" + glsl;
        };

        {
            StageResult& r = program.vertex;
            std::string head = "#version 450\n";
            std::string text = sharedPrelude(r, true);
            const size_t sep = text.find('\x01');
            std::string samplerText = text.substr(0, sep), body = text.substr(sep + 1);
            std::string attrs;
            const VertexInputs& vi = r.vertexInputs;
            auto have = [&](int loc) { return (options.availableAttribs >> loc) & 1u; };
            auto emitAttr = [&](int loc, const std::string& type, const std::string& name, const std::string& fallback) {
                if (have(loc)) attrs += "layout(location = " + std::to_string(loc) + ") in " + type + " " + name + ";\n";
                else attrs += type + " " + name + " = " + fallback + ";\n";
            };
            if (vi.position) emitAttr(kAttribPosition, "vec3", "iris_Position", "vec3(0.0)");
            if (vi.color) emitAttr(kAttribColor0, "vec4", "iris_Color", "vec4(1.0)");
            if (vi.uv0) emitAttr(kAttribTexCoord0, "vec2", "iris_UV0", "vec2(0.0)");
            if (vi.uv1) emitAttr(kAttribTexCoord1, "vec2", "iris_UV1", "vec2(0.0)");
            if (vi.normal) emitAttr(kAttribNormal, "vec3", "iris_Normal", "vec3(0.0, 1.0, 0.0)");
            if (vi.uv2) emitAttr(kAttribTexCoord5, "vec2", "iris_UV2", "vec2(240.0)");
            int userAttrib = 0;
            for (const InterfaceVar& v : r.inputs) {
                int loc;
                if (v.name == "mc_Entity") loc = kAttribTexCoord2;
                else if (v.name == "mc_midTexCoord") loc = kAttribTexCoord3;
                else if (v.name == "at_tangent") loc = kAttribTangent;
                else if (v.name == "at_midBlock") loc = kAttribTexCoord4;
                else loc = kAttribTexCoord6 + userAttrib++;
                if (loc > kAttribTexCoord15) { r.ok = false; r.error = "too many vertex attributes"; }
                std::string fallback = v.type + "(0.0)";
                if (v.name == "mc_Entity") fallback = v.type == "float" ? "-1.0" : v.type + "(-1.0)";
                else if (v.name == "at_tangent") fallback = v.type == "vec4" ? "vec4(1.0, 0.0, 0.0, 1.0)" : fallback;
                if (v.type.compare(0, 3, "vec") != 0 && v.type != "float") fallback = v.type + "(0)";
                emitAttr(loc, v.type, v.name, fallback);
            }
            body = ReplaceAll(body, "/*ATTR*/", "");
            std::string varyings;
            for (const InterfaceVar& v : r.outputs) {
                const Link& l = links[v.name];
                varyings += varyingDecl(v.name, l, true);
            }
            if (fogCoord) varyings += varyingDecl("iris_FogFragCoord", links["iris_FogFragCoord"], true);
            body = ReplaceAll(body, "/*VARYING_OUT*/", "");
            std::string prelude;
            prelude += "#define PERLIN_VULKAN_GLSL 1\n";
            std::string footer = "\nvoid main() {\n    iris_user_main();\n";
            if (options.flipVertexY) footer += "    gl_Position.y = -gl_Position.y;\n";
            if (options.zeroToOneDepth) footer += "    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n";
            footer += "}\n";
            r.glsl = head + attrs + varyings + samplerText + prelude + body + footer;
        }
        {
            StageResult& r = program.fragment;
            const std::string flagsSource = r.glsl;
            std::string head = "#version 450\n";
            std::string text = sharedPrelude(r, false);
            const size_t sep = text.find('\x01');
            std::string samplerText = text.substr(0, sep), body = text.substr(sep + 1);
            std::string varyings;
            for (const InterfaceVar& v : r.inputs) {
                const Link& l = links[v.name];
                varyings += varyingDecl(v.name, l, false);
            }
            if (fogCoord) varyings += varyingDecl("iris_FogFragCoord", links["iris_FogFragCoord"], false);
            body = ReplaceAll(body, "/*VARYING_IN*/", "");
            std::string outs;
            std::set<int> usedLoc;
            for (const InterfaceVar& v : r.outputs)
                if (v.location >= 0) usedLoc.insert(v.location);
            int loc = 0;
            std::vector<std::pair<int, InterfaceVar>> placed;
            for (const InterfaceVar& v : r.outputs) {
                int l = v.location;
                if (l < 0) {
                    while (usedLoc.count(loc)) loc++;
                    l = loc;
                    usedLoc.insert(l);
                }
                placed.emplace_back(l, v);
            }
            const int fragData = FlagNumber(flagsSource, "FRAGDATA");
            for (int i = 0; i < fragData; i++) {
                if (usedLoc.count(i) && !HasFlag(flagsSource, "NEVER")) {
                    bool user = false;
                    for (auto& p : placed) if (p.first == i) user = true;
                    if (user) continue;
                }
                outs += "layout(location = " + std::to_string(i) + ") out vec4 iris_FragData" + std::to_string(i) + ";\n";
                usedLoc.insert(i);
            }
            for (auto& p : placed) {
                outs += "layout(location = " + std::to_string(p.first) + ") out " + p.second.type + " " + p.second.name;
                if (p.second.arrayCount > 0) outs += "[" + std::to_string(p.second.arrayCount) + "]";
                outs += ";\n";
                r.drawBuffers.push_back(p.first);
            }
            for (int i = 0; i < fragData; i++) r.drawBuffers.push_back(i);
            std::sort(r.drawBuffers.begin(), r.drawBuffers.end());
            r.drawBuffers.erase(std::unique(r.drawBuffers.begin(), r.drawBuffers.end()), r.drawBuffers.end());
            body = ReplaceAll(body, "/*FRAGOUT*/", "");

            std::string prelude;
            if (HasFlag(flagsSource, "SHADOW2D"))
                prelude += "vec4 iris_shadow2D(sampler2DShadow s, vec3 c) { return vec4(texture(s, c)); }\n";
            if (HasFlag(flagsSource, "SHADOW2DLOD"))
                prelude += "vec4 iris_shadow2DLod(sampler2DShadow s, vec3 c, float l) { return vec4(textureLod(s, c, l)); }\n";
            if (HasFlag(flagsSource, "SHADOW2DPROJ"))
                prelude += "vec4 iris_shadow2DProj(sampler2DShadow s, vec4 c) { return vec4(textureProj(s, c)); }\n";
            std::string fragCoord;
            std::string mainPrefix;
            if (options.flipFragCoordY && HasFlag(flagsSource, "FRAGCOORD")) {
                fragCoord = "vec4 iris_FragCoord;\n";
                mainPrefix = "    iris_FragCoord = vec4(gl_FragCoord.x, iris_ViewHeight - gl_FragCoord.y, gl_FragCoord.z, gl_FragCoord.w);\n";
            }
            std::string footer = "\nvoid main() {\n" + mainPrefix + "    iris_user_main();\n}\n";
            r.glsl = head + varyings + outs + samplerText + prelude + fragCoord + body + footer;
            if (!mainPrefix.empty() && r.glsl.find("IrisGlobals") == std::string::npos) {
                r.ok = false;
                r.error = "internal: iris_ViewHeight requested without uniform block";
            }
        }
        program.uniforms = program.vertex.uniforms;
        program.uniformRegs = 0;
        for (const UniformDecl& u : program.uniforms) program.uniformRegs += u.regs;
        return program;
    }
}
