// Minimal java terrain stand-in: same vertex layout and MRT shape as the
// converted program, but a trivial body. Proves the slot before richer code.
#include "compat.slang"
#include "vv_resources.slang"

struct VsIn
{
    [[vk::location(0)]] float3 a_position : TEXCOORD0;
    [[vk::location(2)]] float4 a_color0 : TEXCOORD2;
    [[vk::location(4)]] float2 a_texcoord0 : TEXCOORD4;
    [[vk::location(5)]] float4 a_texcoord1 : TEXCOORD5;
};
struct VsOut
{
    float4 clipPosition : SV_Position;
    [[vk::location(0)]] float2 v_uv : TEXCOORD0;
    [[vk::location(1)]] float v_layer : TEXCOORD1;
    [[vk::location(2)]] float4 v_tint : TEXCOORD2;
    [[vk::location(3)]] float2 v_lm : TEXCOORD3;
};
struct FsIn
{
    float4 fragCoord : SV_Position;
    [[vk::location(0)]] float2 v_uv : TEXCOORD0;
    [[vk::location(1)]] float v_layer : TEXCOORD1;
    [[vk::location(2)]] float4 v_tint : TEXCOORD2;
    [[vk::location(3)]] float2 v_lm : TEXCOORD3;
};
struct FsOut
{
    float4 t0 : SV_Target0;
    float4 t1 : SV_Target1;
};

[shader("vertex")]
VsOut vsMain(VsIn input)
{
    VsOut output;
    output.clipPosition = mul(u_mvp, float4(input.a_position, 1.0));
    output.v_uv = input.a_texcoord0;
    // tile index packed in the light bytes by the mesher
    output.v_layer =
        floor(input.a_texcoord1.z * 255.0 + 0.5) + floor(input.a_texcoord1.w * 255.0 + 0.5) * 256.0;
    output.v_tint = input.a_color0;
    // engine light bytes hold 0..15 levels; the lightmap wants 0..1
    output.v_lm = input.a_texcoord1.xy * 17.0;
    return output;
}

[shader("fragment")]
FsOut fsMain(FsIn input)
{
    FsOut output;
    float4 tex = texture2DArray(s_atlas, float3(input.v_uv, input.v_layer));
    if (tex.a <= 0.5)
        discard;
    float3 light = texture2D(s_lightmap, clamp(input.v_lm, 0.0, 1.0)).rgb;
    output.t0 = float4(tex.rgb * input.v_tint.rgb * light, 1.0);
    output.t1 = float4(0.0, 0.0, 0.0, 1.0);
    return output;
}
