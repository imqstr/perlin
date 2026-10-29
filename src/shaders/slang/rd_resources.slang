#pragma once
#include "compat.slang"

// every RenderDragon-derived program shares this block and these textures
UNIFORMS_BEGIN
    mat4 u_mvp;
    vec4 u_camera;
    vec4 u_fogColor;
    vec4 u_fogEnv;
    vec4 u_fogSky;
    vec4 u_modulator;
    vec4 u_textureInfo;
    vec4 u_chunk;
    vec4 u_cloudOffset;
    vec4 u_cloudColor;
    vec4 u_dynParams;
    vec4 u_dynLights[8];
    vec4 u_uiParams;
    vec4 u_uiOverlay;
    vec4 u_uiGlintA;
    vec4 u_uiGlintB;
    vec4 u_rdMaterial;
    mat4 u_modelViewProj;
    mat4 u_model[1];
UNIFORMS_END

SAMPLER2DARRAY(s_atlas, 0, 1);
SAMPLER2D(s_lightmap, 2, 3);
SAMPLER2D(s_sky, 22, 23);
SAMPLER2D(s_uiTex, 24, 25);
SAMPLER2D(s_uiGlint, 26, 27);
