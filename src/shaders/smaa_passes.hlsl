// SMAA 1x for [graphics] smaa=1 (gfx_d3d12.cpp): the three SMAA passes, using the
// unmodified SMAA.hlsl from iryoku/smaa (MIT, re\third_party\smaa_71c806a; its
// LICENSE.txt must ship with any build that includes these shaders). Written by the
// decomp session (re\shaders_proposed\smaa_passes.hlsl); DebugPS added here.
//
// Entry points (build_shaders.cmd, dxc -I ..\..\..\re\third_party\smaa_71c806a):
//   vs_6_0: EdgeVS, BlendVS, NeighborVS      ps_6_0: EdgePS, BlendPS, NeighborPS, DebugPS
//   SMAA_PRESET_HIGH by default; -D SMAA_PRESET_LOW, _MEDIUM or _ULTRA for the others
//   (EdgePS, BlendVS, BlendPS depend on the preset; the rest don't).
// Root signature (gfx_d3d12.cpp): b0 = 4 root constants (1/w, 1/h, w, h); one table
// t0..t4 (colour, colour sRGB view, edges or blend weights, areaTex, searchTex); static
// samplers s0 linear clamp, s1 point clamp. Full-screen triangle from SV_VertexID.

cbuffer Metrics : register(b0) { float4 g_rtMetrics; };   // 1/w, 1/h, w, h
#define SMAA_RT_METRICS g_rtMetrics

// SMAA_HLSL_4_1's own samplers have no registers, so dxc numbers whichever ones a pass
// uses from s0 (point in one pass, linear in another). These are the same macros with
// fixed registers, so one pair of static samplers fits all three passes.
SamplerState LinearSampler : register(s0);   // MIN_MAG_LINEAR_MIP_POINT, clamp
SamplerState PointSampler  : register(s1);   // MIN_MAG_MIP_POINT, clamp
#define SMAA_CUSTOM_SL 1
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAA_DECODE_VELOCITY(sample) sample.rg
#if !defined(SMAA_PRESET_LOW) && !defined(SMAA_PRESET_MEDIUM) && !defined(SMAA_PRESET_ULTRA)
#define SMAA_PRESET_HIGH 1
#endif
#include "SMAA.hlsl"

Texture2D colorTex     : register(t0);  // the pre-HUD image (UNORM view): edges are found in gamma space
Texture2D colorTexSrgb : register(t1);  // the same, sRGB view: blending in linear space, as SMAA recommends
Texture2D passTex      : register(t2);  // pass 2: edges; pass 3: blend weights
Texture2D areaTex      : register(t3);  // AreaTex.h, R8G8_UNORM 160x560
Texture2D searchTex    : register(t4);  // SearchTex.h, R8_UNORM 64x16

// One full-screen triangle from SV_VertexID: no vertex buffer, no input layout.
void FullScreen(uint id, out float4 pos, out float2 uv)
{
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

struct EdgeV { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 off[3] : TEXCOORD1; };
EdgeV EdgeVS(uint id : SV_VertexID)
{
    EdgeV o; FullScreen(id, o.pos, o.uv); SMAAEdgeDetectionVS(o.uv, o.off); return o;
}
float2 EdgePS(EdgeV i) : SV_Target
{
    return SMAAColorEdgeDetectionPS(i.uv, i.off, colorTex);
}

struct BlendV { float4 pos : SV_Position; float2 uv : TEXCOORD0; float2 pix : TEXCOORD1; float4 off[3] : TEXCOORD2; };
BlendV BlendVS(uint id : SV_VertexID)
{
    BlendV o; FullScreen(id, o.pos, o.uv); SMAABlendingWeightCalculationVS(o.uv, o.pix, o.off); return o;
}
float4 BlendPS(BlendV i) : SV_Target
{
    return SMAABlendingWeightCalculationPS(i.uv, i.pix, i.off, passTex, areaTex, searchTex, 0);
}

struct NbV { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 off : TEXCOORD1; };
NbV NeighborVS(uint id : SV_VertexID)
{
    NbV o; FullScreen(id, o.pos, o.uv); SMAANeighborhoodBlendingVS(o.uv, o.off); return o;
}
float4 NeighborPS(NbV i) : SV_Target   // written through an sRGB RTV of the back buffer
{
    float4 c = SMAANeighborhoodBlendingPS(i.uv, i.off, colorTexSrgb, passTex);
#ifdef MANUAL_SRGB_OUT
    // -D MANUAL_SRGB_OUT: for the engine's typed UNORM frame target, which can't take an
    // sRGB view. Same linear-space blend; the sRGB encode is done here instead of by the RTV.
    c.rgb = saturate(c.rgb);
    c.rgb = lerp(1.055 * pow(c.rgb, 1.0 / 2.4) - 0.055, c.rgb * 12.92, step(c.rgb, 0.0031308));
#endif
    return c;
}

// smaa_debug=1: the detected edges (passTex = the edges texture) instead of the image,
// so it's easy to see what gets smoothed (no edges should appear on the HUD).
float4 DebugPS(NbV i) : SV_Target
{
    return float4(passTex.SampleLevel(PointSampler, i.uv, 0).rg, 0, 1);
}
