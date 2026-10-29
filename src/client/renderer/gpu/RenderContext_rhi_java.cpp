#include "client/renderer/gpu/RenderContext_rhi.hpp"

#include "client/javashaders/PackSource.hpp"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <iostream>
#include <random>

namespace perlin {
    namespace {
        constexpr float kTau = 6.28318530717958647692f;

        glm::mat4 ToFloat(const glm::dmat4& m) { return glm::mat4(m); }

        // combined world->shadowUV split so the pack applies its own projection
        void SplitShadow(const float* s, float* mv, float* proj) {
            float r[3][3];
            for (int row = 0; row < 3; row++) {
                float x = s[row], y = s[4 + row], z = s[8 + row];
                const float l = std::sqrt(x * x + y * y + z * z);
                const float inv = l > 1e-9f ? 1.0f / l : 0.0f;
                r[row][0] = x * inv;
                r[row][1] = y * inv;
                r[row][2] = z * inv;
            }
            for (int c = 0; c < 4; c++)
                for (int rr = 0; rr < 4; rr++)
                    mv[c * 4 + rr] = 0.0f;
            for (int c = 0; c < 3; c++)
                for (int row = 0; row < 3; row++)
                    mv[c * 4 + row] = r[row][c];
            mv[15] = 1.0f;
            float rt[16] = {};
            for (int c = 0; c < 3; c++)
                for (int row = 0; row < 3; row++)
                    rt[c * 4 + row] = r[c][row];
            rt[15] = 1.0f;
            for (int c = 0; c < 4; c++)
                for (int row = 0; row < 4; row++) {
                    float acc = 0.0f;
                    for (int k = 0; k < 4; k++)
                        acc += s[k * 4 + row] * rt[c * 4 + k];
                    proj[c * 4 + row] = acc;
                }
        }
    } // namespace

    bool RenderContext_rhi::SetJavaShaderPack(const std::string& path, const std::map<std::string, std::string>&,
                                              std::string& error) {
        if (!jshader::LooksLikeJavaPack(path)) {
            error = "not a java shader pack: " + path;
            return false;
        }
        // milestone: the baked converted programs stand in for the zip until
        // load-time conversion lands; the zip only selects the mode for now
        javaPackPath = path;
        javaActive = true;
        gpu.ClearPipelines();
        if (!ResolvePrograms() || ProgramIndex(Program::TerrainSolid) < 0) {
            javaActive = false;
            ResolvePrograms();
            error = "converted java programs missing from the shader pack";
            return false;
        }
        std::cout << "[javapack] java terrain active (" << path << ")" << std::endl;
        return true;
    }

    void RenderContext_rhi::ClearJavaShaderPack() {
        if (!javaActive)
            return;
        javaActive = false;
        javaPackPath.clear();
        gpu.ClearPipelines();
        ResolvePrograms();
    }

    bool RenderContext_rhi::EnsureJavaSamplers() {
        if (javaTexReady)
            return true;
        const auto flat = [&](TexRec& rec, uint8_t r, uint8_t g, uint8_t b, uint8_t a, const char* name) {
            if (!MakeTexture(rec, 1, 1, 1, 1, rhi::Format::RGBA8Unorm, gpu::SamplerKind::LinearClamp, name))
                return false;
            const uint8_t px[4] = {r, g, b, a};
            gpu.WriteTexture(rec.handle, 0, 0, 0, 1, 1, px);
            return true;
        };
        if (!flat(javaBlack, 0, 0, 0, 255, "java black") || !flat(javaNormalDef, 128, 128, 255, 255, "java flat normal") ||
            !flat(javaSpecDef, 0, 0, 0, 255, "java rough specular"))
            return false;
        if (!MakeTexture(javaNoise, 128, 128, 1, 1, rhi::Format::RGBA8Unorm, gpu::SamplerKind::LinearRepeat, "java noise"))
            return false;
        std::vector<uint8_t> noise(128 * 128 * 4);
        std::mt19937 rng(0xC0FFEEu);
        for (uint8_t& v : noise)
            v = static_cast<uint8_t>(rng() & 0xff);
        gpu.WriteTexture(javaNoise.handle, 0, 0, 0, 128, 128, noise.data());
        javaTexReady = true;
        return true;
    }

    void RenderContext_rhi::BindJavaExtras() {
        if (!EnsureJavaSamplers())
            return;
        BindTexture("s_noise", javaNoise.handle, gpu::SamplerKind::LinearRepeat);
        BindTexture("s_normals", javaNormalDef.handle, gpu::SamplerKind::LinearClamp);
        BindTexture("s_specular", javaSpecDef.handle, gpu::SamplerKind::LinearClamp);
        const rhi::TextureHandle white = gpu.White2D();
        BindTexture("s_gaux2", white, gpu::SamplerKind::LinearClamp);
        BindTexture("s_gaux4", white, gpu::SamplerKind::LinearClamp);
        for (int i = 0; i < 16; i++)
            BindTexture(("s_colortex" + std::to_string(i)).c_str(), javaBlack.handle, gpu::SamplerKind::LinearClamp);
    }

    void RenderContext_rhi::FeedJavaUniforms(const FrameParams& params) {
        const glm::mat4 view = glm::make_mat4(params.view);
        const glm::mat4 proj = glm::make_mat4(params.projection);
        const glm::mat4 viewInv = glm::inverse(view);
        const glm::mat4 projInv = glm::inverse(proj);
        uni.Mat4("u_jgbufferModelView", glm::value_ptr(view));
        uni.Mat4("u_jgbufferModelViewInverse", glm::value_ptr(viewInv));
        uni.Mat4("u_jgbufferProjection", glm::value_ptr(proj));
        uni.Mat4("u_jgbufferProjectionInverse", glm::value_ptr(projInv));
        const glm::mat4 prevView = cam.havePrev ? ToFloat(cam.prevView) : view;
        const glm::mat4 prevProj = cam.havePrev ? ToFloat(cam.prevProj) : proj;
        uni.Mat4("u_jgbufferPreviousModelView", glm::value_ptr(prevView));
        uni.Mat4("u_jgbufferPreviousProjection", glm::value_ptr(prevProj));
        uni.Mat4("u_jModelViewMatrix", glm::value_ptr(view));
        uni.Mat4("u_jProjectionMatrix", glm::value_ptr(proj));
        const glm::mat4 identT(1.0f);
        uni.Mat4("u_jTextureMatrix0", glm::value_ptr(identT));
        uni.Mat4("u_jTextureMatrix1", glm::value_ptr(identT));
        const glm::mat3 normal = glm::transpose(glm::inverse(glm::mat3(view)));
        uni.Mat4("u_jNormalMatrix", glm::value_ptr(glm::mat4(normal)));

        if (shadow.active) {
            const float* s = shadow.maps[shadow.front].sample;
            float mv[16], pr[16];
            SplitShadow(s, mv, pr);
            uni.Mat4("u_jshadowModelView", mv);
            uni.Mat4("u_jshadowProjection", pr);
            uni.Mat4("u_jshadowModelViewInverse", glm::value_ptr(glm::inverse(glm::make_mat4(mv))));
            uni.Mat4("u_jshadowProjectionInverse", glm::value_ptr(glm::inverse(glm::make_mat4(pr))));
        } else {
            const glm::mat4 ident(1.0f);
            uni.Mat4("u_jshadowModelView", glm::value_ptr(ident));
            uni.Mat4("u_jshadowProjection", glm::value_ptr(ident));
            uni.Mat4("u_jshadowModelViewInverse", glm::value_ptr(ident));
            uni.Mat4("u_jshadowProjectionInverse", glm::value_ptr(ident));
        }

        uni.Set("u_jcameraPosition", params.cameraPosition, 3);
        const float eyeY[1] = {params.cameraPosition[1]};
        uni.Set("u_jeyeAltitude", eyeY, 1);
        uni.Set("u_jeyePosition", params.cameraPosition, 3);
        const float prevEye[3] = {static_cast<float>(cam.havePrev ? cam.prevEye.x : params.cameraPosition[0]),
                                  static_cast<float>(cam.havePrev ? cam.prevEye.y : params.cameraPosition[1]),
                                  static_cast<float>(cam.havePrev ? cam.prevEye.z : params.cameraPosition[2])};
        uni.Set("u_jpreviousCameraPosition", prevEye, 3);
        const auto splitPos = [&](const char* iname, const char* fname, const float* p) {
            const int32_t ip[3] = {static_cast<int32_t>(std::floor(p[0])), static_cast<int32_t>(std::floor(p[1])),
                                   static_cast<int32_t>(std::floor(p[2]))};
            uni.SetI(iname, ip, 3);
            const float fp[3] = {p[0] - static_cast<float>(ip[0]), p[1] - static_cast<float>(ip[1]),
                                 p[2] - static_cast<float>(ip[2])};
            uni.Set(fname, fp, 3);
        };
        splitPos("u_jcameraPositionInt", "u_jcameraPositionFract", params.cameraPosition);
        splitPos("u_jpreviousCameraPositionInt", "u_jpreviousCameraPositionFract", prevEye);

        uni.SetI("u_jframeCounter", static_cast<int32_t>(params.frameIndex));
        const float dt = cam.havePrev ? static_cast<float>(std::clamp(cam.time - cam.prevTime, 0.0, 0.1)) : 0.016f;
        uni.Set("u_jframeTime", &dt, 1);
        uni.Set("u_jframeTimeCounter", &params.timeSeconds, 1);
        uni.Set("u_jframeTimeSmooth", &dt, 1);
        const float fc = static_cast<float>(params.frameIndex);
        const float fm2 = std::fmod(fc, 2.0f), fm4 = std::fmod(fc, 4.0f);
        const float fm8 = std::fmod(fc, 8.0f), fm600 = std::fmod(fc, 600.0f);
        uni.Set("u_jframemod2", &fm2, 1);
        uni.Set("u_jframemod4", &fm4, 1);
        uni.Set("u_jframemod8", &fm8, 1);
        uni.Set("u_jframemod600", &fm600, 1);
        const int worldTick = params.worldTick >= 0 ? params.worldTick : static_cast<int>(params.timeSeconds * 20.0f) % 24000;
        const int worldDay = params.worldDay >= 0 ? params.worldDay : static_cast<int>(params.timeSeconds / 1200.0f);
        uni.SetI("u_jworldTime", static_cast<int32_t>(worldTick));
        uni.SetI("u_jworldDay", static_cast<int32_t>(worldDay));

        // engine angles are radians around noon; the pack wants a 0..1 day fraction
        const float sunFrac = std::fmod(params.sky.sunAngle / kTau + 0.25f, 1.0f);
        uni.Set("u_jsunAngle", &sunFrac, 1);
        uni.Set("u_jshadowAngle", &sunFrac, 1);
        uni.SetI("u_jmoonPhase", static_cast<int32_t>(params.sky.moonPhase));
        const float rain = 1.0f - params.sky.rainBrightness;
        uni.Set("u_jrainStrength", &rain, 1);
        uni.Set("u_jrainFactor", &rain, 1);
        uni.Set("u_jwetness", &rain, 1);
        uni.SetI("u_jisEyeInWater", params.cameraMedium == 1 ? 1 : 0);
        uni.SetI("u_jheavyFog", 0);
        uni.SetI("u_jisElytraFlying", 0);
        const float dayF = std::clamp((std::cos(params.sky.sunAngle) + 0.1f) / 0.35f, 0.0f, 1.0f);
        const int32_t eyeB[2] = {static_cast<int32_t>(params.heldLight * 16.0f), static_cast<int32_t>(240.0f * dayF)};
        uni.SetI("u_jeyeBrightness", eyeB, 2);
        const float eyeM[1] = {static_cast<float>(eyeB[0])};
        const float eyeM2[1] = {static_cast<float>(eyeB[1])};
        uni.Set("u_jeyeBrightnessM", eyeM, 1);
        uni.Set("u_jeyeBrightnessM2", eyeM2, 1);
        uni.Set("u_jcloudHeight", &params.cloud.height, 1);

        const float wh[2] = {static_cast<float>(width), static_cast<float>(height)};
        uni.Set("u_jviewWidth", &wh[0], 1);
        uni.Set("u_jviewHeight", &wh[1], 1);
        const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
        uni.Set("u_jaspectRatio", &aspect, 1);
        uni.Set("u_jfogColor", params.fogColor, 3);
        uni.Set("u_jskyColor", params.sky.skyColor, 3);
        const int32_t atlas[2] = {std::max(1, tileSize), std::max(1, tileSize)};
        uni.SetI("u_jatlasSize", atlas, 2);
        const float bright = 1.0f;
        uni.Set("u_jscreenBrightness", &bright, 1);
        const float zero = 0.0f;
        uni.Set("u_jnightVision", &zero, 1);
        uni.Set("u_jblindness", &zero, 1);
        uni.Set("u_jdarknessFactor", &zero, 1);
        uni.Set("u_jdarknessLightFactor", &zero, 1);
        uni.Set("u_jplayerMood", &zero, 1);
        uni.Set("u_jnear", &params.cameraNear, 1);
        uni.Set("u_jfar", &params.cameraFar, 1);
    }
} // namespace perlin
