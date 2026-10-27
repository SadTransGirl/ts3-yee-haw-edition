// Replacement for the pixel shader of the game's shadowmaskchar effect (the
// character blob shadow; DXBC container 3532 bytes, digest 35c9d56b...), swapped in
// by gfx_d3d12.cpp when the pipeline is created. Same inputs, bindings and maths as
// the original, which was reconstructed from its DXIL:
//
//   1. uv from the projected position (TEXCOORD0 .xy / .w), depth from t5, made
//      linear with b1 row 17 (.z, .w);
//   2. world position through the inverse view-projection at b1 rows 12..15;
//   3. only inside a slab around the ground plane: |dot(p - ground, normal)| <
//      g_ShadowBlobThickness (b0 row 8 .w), with ground position/normal in b2;
//   4. project into the 512x512 character shadow map with the 3x4 matrix at
//      b1 rows 36..38, bias the receiver depth by b0 row 6 .w, and compare ONE
//      texel of t4: occluder nearer than the receiver = 1;
//   5. output ShadowFade (b2 row 1 .w) times that, in all channels.
//
// A single point compare gives a hard, stair-stepped blob edge. Here the compare is
// a 5x5 Gaussian of bilinear 2x2 compares (Gather), one tap per texel of the map.
//
// Built by build_shaders.cmd (Windows SDK dxc, which signs the output with dxil.dll).

cbuffer PerFrame : register(b0) { float4 f[9]; };
cbuffer PerView : register(b1) { float4 c[39]; };
cbuffer PerMaterial : register(b2) { float4 m[2]; };     // row 0 ground position, row 1 ground normal + fade
Texture2D<float4> shadowTexture : register(t4);
Texture2D<float4> depthViewBuffer : register(t5);
SamplerState samShadow : register(s4);
SamplerState samDepth : register(s5);

// Fraction of the 2x2 texels around uv that are occluded (stored depth nearer than z), bilinearly weighted.
float BilinearOccluded(float2 uv, float2 dim, float z) {
    float2 t = uv * dim - 0.5;
    float2 fr = frac(t);
    float4 g = shadowTexture.GatherRed(samShadow, (floor(t) + 1.0) / dim) * c[17].z + c[17].w;
    float4 occ = 1.0 - step(z, g);            // g < z: occluded.  x (0,1)  y (1,1)  z (1,0)  w (0,0)
    return lerp(lerp(occ.w, occ.z, fr.x), lerp(occ.x, occ.y, fr.x), fr.y);
}

float4 main(float4 pos : TEXCOORD0) : SV_Target {
    float2 uv = float2(pos.x / pos.w * 0.5 + 0.5, 0.5 - pos.y / pos.w * 0.5);
    float depth = depthViewBuffer.Sample(samDepth, uv).x * c[17].z + c[17].w;
    float4 ndc = float4(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0, depth, 1.0);
    float4 h = float4(dot(c[12], ndc), dot(c[13], ndc), dot(c[14], ndc), dot(c[15], ndc));
    float3 p = h.xyz / h.w;

    float height = dot(p - m[0].xyz, m[1].xyz);
    if (abs(height) >= f[8].w) return float4(0, 0, 0, 0);

    float4 p1 = float4(p, 1.0);
    float3 s = float3(dot(c[36], p1), dot(c[37], p1), dot(c[38], p1));
    float zRef = saturate(s.z - f[6].w);

    float2 dim;
    shadowTexture.GetDimensions(dim.x, dim.y);
    float sum = 0, wsum = 0;
    for (int j = 0; j < 5; j++) {
        float oy = j - 2.0;
        for (int i = 0; i < 5; i++) {
            float ox = i - 2.0;
            float w = exp(-0.32 * (ox * ox + oy * oy));              // sigma 1.25 texels
            sum += w * BilinearOccluded(s.xy + float2(ox, oy) / dim, dim, zRef);
            wsum += w;
        }
    }
    return (m[1].w * sum / wsum).xxxx;
}
