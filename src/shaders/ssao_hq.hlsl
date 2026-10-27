// Replacement for the pixel shader of the game's ssao effect (DXIL digest
// 22bcf520...), swapped in by gfx_d3d12.cpp when the pipeline is created.
//
// The original is line-sampled volumetric obscurance with only 3 sample pairs on
// a disk, rotated per pixel by a 4x4 noise texture. Each pair compares the
// linear depth at +/- an offset against the centre with a range falloff, and the
// weights make a flat surface come out as exactly 1. This keeps that estimator,
// its constants and its output unchanged (so the game's AO strength and its
// ssao_blur pass still match) but uses 8 pairs on a Vogel spiral: much less
// noise and banding.
//
// Built by build_shaders.cmd.

cbuffer PerView : register(b1) { float4 c[15]; };    // row 14: .y radius scale, .z max distance
cbuffer PerDraw : register(b0) { float4 d[19]; };    // row 16: .xy target size; row 18: .y depth scale
Texture2D<float4> depthTex : register(t3);
Texture2D<float4> rotateMap : register(t4);
SamplerState samDepth : register(s3);
SamplerState samRotate : register(s4);

static const int kPairs = 8;

float4 main(float2 uv : TEXCOORD0) : SV_Target {
    float2 size = d[16].xy;
    float aspect = size.y / size.x;
    float2 rot = rotateMap.Sample(samRotate, uv * 0.25 * size).xy;      // (cos, sin), tiles every 4 px
    float z = depthTex.Sample(samDepth, uv).x * d[18].y;
    float r = min(c[14].y / z, 0.07);                                    // disk radius in uv
    float range = c[14].z;

    // Widths of the line samples through a unit sphere, normalised so that all
    // pairs together weigh 0.5 (the original: 0.0339 base + 0.466 over 3 pairs).
    float wsum = 0;
    [unroll] for (int k = 0; k < kPairs; k++) {
        float rr = 0.9 * sqrt((k + 0.5) / kPairs);
        wsum += 2.0 * sqrt(1.0 - rr * rr);
    }

    float sum = 0;
    [unroll] for (int i = 0; i < kPairs; i++) {
        float rr = 0.9 * sqrt((i + 0.5) / kPairs);
        float ang = i * 2.39996323;                                      // golden angle
        float2 p = rr * float2(cos(ang), sin(ang));
        float width = 2.0 * sqrt(1.0 - rr * rr);
        float2 o = float2(p.x * rot.y - p.y * rot.x, p.x * rot.x + p.y * rot.y);
        o *= r;
        o.x *= aspect;
        float zA = depthTex.Sample(samDepth, uv + o).x * d[18].y;
        float zB = depthTex.Sample(samDepth, uv - o).x * d[18].y;
        float dA = z - zA, dB = z - zB;
        float aA = saturate(dA / (width * r) + 0.5), aB = saturate(dB / (width * r) + 0.5);
        float fA = saturate((range - dA) / range), fB = saturate((range - dB) / range);
        float cA = fB * (0.5 - aB) + 0.5, cB = fA * (0.5 - aA) + 0.5;     // missing-data fallback, as the original
        float pair = cA + cB + (aA - cA) * fA + (aB - cB) * fB;
        sum += pair * (0.5 * width / wsum);
    }
    return float4(1.0 - saturate((sum - 0.5) * 2.0), 0, 0, 0);
}
