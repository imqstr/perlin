#pragma once
#include "compat.slang"

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
    vec4 u_sunDir;
    vec4 u_time;
    vec4 u_heldLight;
    vec4 u_tiles;
    vec4 u_skyParams;
    vec4 u_dynParams;
    vec4 u_dynLights[8];
    vec4 u_packSettings[8];
    mat4 u_shadowMtx;
    vec4 u_shadowInfo;
    mat4 u_shadowMtxB;
    vec4 u_shadowInfoB;
    mat4 u_shadowMtxE;
    vec4 u_shadowInfoE;
    vec4 u_cloudMap;
    vec4 u_sceneInfo;
    mat4 u_invMvp;
    vec4 u_viewTexel;
UNIFORMS_END

SAMPLER2DARRAY(s_atlas, 0, 1);
SAMPLER2D(s_lightmap, 2, 3);
SAMPLER2D(s_shadow, 4, 5);
SAMPLER2D(s_clouds, 6, 7);
SAMPLER2D(s_tileFlags, 12, 13);
SAMPLER2D(s_shadowTint, 14, 15);
SAMPLER2D(s_shadowB, 16, 17);
SAMPLER2D(s_shadowTintB, 18, 19);
SAMPLER2D(s_shadowEnt, 20, 21);
