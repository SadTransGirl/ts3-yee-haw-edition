// Replacement for the pixel shader of the game's shadowmask_4x4 effect (DXIL
// hash 6031e872ac8598e6f44fab41b7613441), swapped in by gfx_d3d12.cpp when the
// pipeline is created. Same inputs, bindings, cascade selection and slope-scaled
// depth bias as the original, which was reconstructed from its DXIL.
//
// The original takes 16 single-texel depth compares on a fixed 4x4 grid, laid
// out for the stock 640x1920 shadow atlas, with Gaussian weights (sigma = 1 stock
// texel). That gives stair-stepped edges, and with a larger shadow_res the taps
// end up several texels apart. Here every tap is a bilinear 2x2 compare (Gather),
// over the same Gaussian footprint, with one tap per real texel up to 7x7.
//
// With -D NODB it replaces shadowmask_4x4_nodb's pixel shader (digest 5eb986ee...)
// instead: same cascades and taps, but a constant depth bias (b0 row 6 .w) and no
// slope bias.
//
// With -D CASCADE_DEBUG (gfx_d3d12.cpp, [graphics] shadow_debug=1) the result is
// multiplied by a brightness per cascade that alternates, so every cascade edge is a
// hard line: tile 0 x1.0, tile 1 x0.3, tile 2 x1.0, outside all three x0. (A first
// try with 1.0/0.7/0.45/0.2 was too subtle to see in the Toy Box sunlight.) A shadow
// glitch whose edge lines up with a band edge is a cascade fit/selection problem; one
// that cuts across a band is a caster missing from that cascade (re\STOCK_BUGS.md).
//
// With -D SLOPE_CAP=<fraction> ([graphics] shadow_bias_fix=1, shadow_slope_cap) it fixes
// the lit triangles on surfaces seen at a grazing angle (the Toy Box cliff up close). The
// receiver-plane slope comes from the screen derivatives of the shadow coordinates; when
// the camera sees a facet nearly edge-on, ddx and ddy point almost the same way, the
// determinant goes to ~0, the slope explodes and every tap's bias hits the cap, 0.25 of
// the cascade's depth range (~2.7 m in the nearest one): casters that close are ignored
// over the whole facet. Here a facet seen that edge-on (sine of the angle between the
// derivatives < 0.05) gets the constant bias only, and the cap is SLOPE_CAP instead.
//
// That zeroing turned out too blunt (edge-on facets then band with acne), so -D MAX_TAN=<n>
// ([graphics] shadow_bias_fix=2, shadow_slope_max_tan) clamps the slope instead: every
// tap's bias stays within (tap radius + 0.5) * n stock texels, and an edge-on facet gets
// that largest bounded bias. With the decomp session (its derivation is in the code below).
//
// With -D CASCADE_BLEND (on top of MAX_TAN; [graphics] shadow_cascade_blend=1) the last
// fifth of cascades 0 and 1 (toward each face of their light-space box) fades into the next
// cascade's result, so the hand-over is no longer a hard line (the game has no blending
// at all: each pixel uses exactly one tile). The second filter runs only inside that band.
//
// With -D BIAS_VIEW ([graphics] shadow_debug=2) the result is the stock formula's bias
// for the outermost taps as a fraction of its cap: white where it's capped.
//
// Built by build_shaders.cmd (Windows SDK dxc, which signs the output with dxil.dll).

cbuffer PerView : register(b1) { float4 c[36]; };
#ifdef NODB
cbuffer PerDraw : register(b0) { float4 d[7]; };
#endif
Texture2D<float4> depthViewBuffer : register(t3);
Texture2D<float4> shadowTexture : register(t4);
SamplerState samDepth : register(s3);
SamplerState samShadow : register(s4);

static const float2 kStockTexel = float2(1.0 / 640.0, 1.0 / 1920.0);

#ifdef CASCADE_DEBUG
static const float kTint[4] = { 1.0, 0.3, 1.0, 0.0 };
#define TINT(t) kTint[t]
#else
#define TINT(t) 1.0
#endif

float3 ToCascade(int row, float3 p) {
    return float3(dot(c[row].xyz, p) + c[row].w,
                  dot(c[row + 1].xyz, p) + c[row + 1].w,
                  dot(c[row + 2].xyz, p) + c[row + 2].w);
}

bool Inside(float3 s, float centerY) {
    return max(max(abs(s.x - 0.5), abs(s.y - centerY) + 0.3343333), abs(s.z - 0.5)) < 0.5;
}

// Fraction of the 2x2 texels around uv that are lit, bilinearly weighted.
float BilinearLit(float2 uv, float2 dim, float z) {
    float2 t = uv * dim - 0.5;
    float2 f = frac(t);
    float4 g = shadowTexture.GatherRed(samShadow, (floor(t) + 1.0) / dim) * c[17].z + c[17].w;
    float4 lit = step(z, g);                  // z <= depth: lit.  x (0,1)  y (1,1)  z (1,0)  w (0,0)
    return lerp(lerp(lit.w, lit.z, f.x), lerp(lit.x, lit.y, f.x), f.y);
}

#ifdef CASCADE_BLEND
static const float kBlendBand = 0.1;    // of a tile's half-extent (0.5): the last 20% fades

// MAX_TAN's clamped receiver-plane slope from one tile's screen derivatives.
float SlopeFor(float3 dx, float3 dy) {
    float invDet = 1.0 / (dy.y * dx.x - dy.x * dx.y);
    float dzdu = abs((dy.y * dx.z - dy.z * dx.y) * invDet);
    float dzdv = abs((dy.z * dx.x - dy.x * dx.z) * invDet);
    float slope = dzdu * kStockTexel.x + dzdv * kStockTexel.y;
    float3 du = float3(dx.x, dx.y * 3.0, dx.z), dv = float3(dy.x, dy.y * 3.0, dy.z);
    bool degenerate = length(cross(du, dv)) < 0.05 * length(du) * length(dv);
    float slopeMax = MAX_TAN / 640.0;
    return degenerate ? slopeMax : min(slope, slopeMax);
}

// The same filter as main's, on another tile.
float FilterTile(int tile, float3 s, float scale, float slope, float zBase, float2 dim, int n) {
    float tileMin = tile / 3.0 + 1.0 / dim.y, tileMax = (tile + 1) / 3.0 - 1.0 / dim.y;
    float sum = 0, wsum = 0;
    for (int j = 0; j < n; j++) {
        float oy = -1.5 + 3.0 * j / (n - 1);
        for (int i = 0; i < n; i++) {
            float ox = -1.5 + 3.0 * i / (n - 1);
            float w = exp(-0.5 * (ox * ox + oy * oy));
            float bias = min((max(abs(ox), abs(oy)) + 0.5) * slope, scale * 0.25);
            float2 tuv = s.xy + float2(ox, oy) * kStockTexel;
            tuv.y = clamp(tuv.y, tileMin, tileMax);
            sum += w * BilinearLit(tuv, dim, saturate(zBase - bias));
            wsum += w;
        }
    }
    return sum / wsum;
}
#endif

float4 main(float3 ray : TEXCOORD0, float2 uv : TEXCOORD1) : SV_Target {
    float depth = depthViewBuffer.Sample(samDepth, uv).x;
    float3 p = float3(c[8].w, c[9].w, c[10].w) + depth * ray;

    float3 s0 = ToCascade(19, p), s1 = ToCascade(23, p), s2 = ToCascade(27, p);
    int tile = Inside(s0, 0.1666667) ? 0 : Inside(s1, 0.5) ? 1 : Inside(s2, 0.8333333) ? 2 : 3;
    float3 s = tile == 0 ? s0 : tile == 1 ? s1 : s2;
    float scale = tile == 0 ? 1.0 : tile == 1 ? c[35].y : c[35].z;

#ifdef NODB
    if (tile == 3) return float4(0, TINT(3), TINT(3), TINT(3));
    float slope = 0, zBase = s.z - d[6].w;
#else
    // Receiver-plane depth slope, as the original computes it (before any branch).
    float3 dx = ddx_coarse(s), dy = ddy_coarse(s);
    float ds = max(abs(ddx_coarse(scale)), abs(ddy_coarse(scale)));
#ifdef CASCADE_BLEND
    // The next tiles' derivatives too, while every pixel of the quad is still running.
    float3 dx1 = ddx_coarse(s1), dy1 = ddy_coarse(s1), dx2 = ddx_coarse(s2), dy2 = ddy_coarse(s2);
#endif
    if (tile == 3) return float4(0, TINT(3), TINT(3), TINT(3));

    float det = dy.y * dx.x - dy.x * dx.y;
    float invDet = 1.0 / det;
    float dzdu = abs((dy.y * dx.z - dy.z * dx.y) * invDet);
    float dzdv = abs((dy.z * dx.x - dy.x * dx.z) * invDet);
    float slope = dzdu * kStockTexel.x + dzdv * kStockTexel.y;
    float zBase = s.z - 0.001 - saturate(ds * 256.0) * 0.01;
#ifdef BIAS_VIEW
    return saturate(min(2.0 * slope, scale * 0.25) / (scale * 0.25)).xxxx;
#endif
#if defined(MAX_TAN)
    // The slope per stock texel is about tan(angle to the light-facing plane) / 640 (tiles are
    // about as wide as they are deep), so clamping it to MAX_TAN / 640 bounds every tap's bias
    // to (tap radius + 0.5) * MAX_TAN texels. A facet seen edge-on (derivatives nearly
    // parallel, in tile units: v spans 3 tiles) gets that largest bounded bias, not none.
    float3 du = float3(dx.x, dx.y * 3.0, dx.z), dv = float3(dy.x, dy.y * 3.0, dy.z);
    bool degenerate = length(cross(du, dv)) < 0.05 * length(du) * length(dv);
    float slopeMax = MAX_TAN / 640.0;
    slope = degenerate ? slopeMax : min(slope, slopeMax);
#elif defined(SLOPE_CAP)
    float sinA = abs(det) / (length(dx.xy) * length(dy.xy) + 1e-20);
    slope = sinA < 0.05 ? 0.0 : slope;                 // grazing view: the constant bias only
#endif
#endif
#ifndef SLOPE_CAP
#define SLOPE_CAP 0.25
#endif

    float2 dim;
    shadowTexture.GetDimensions(dim.x, dim.y);
    int n = clamp((int)round(1.0 + 3.0 * dim.x / 640.0), 4, 7);   // stock 4x4, 7x7 from 2x up
    float tileMin = tile / 3.0 + 1.0 / dim.y, tileMax = (tile + 1) / 3.0 - 1.0 / dim.y;

    float sum = 0, wsum = 0;
    for (int j = 0; j < n; j++) {
        float oy = -1.5 + 3.0 * j / (n - 1);
        for (int i = 0; i < n; i++) {
            float ox = -1.5 + 3.0 * i / (n - 1);
            float w = exp(-0.5 * (ox * ox + oy * oy));
            float bias = min((max(abs(ox), abs(oy)) + 0.5) * slope, scale * SLOPE_CAP);
            float2 tuv = s.xy + float2(ox, oy) * kStockTexel;
            tuv.y = clamp(tuv.y, tileMin, tileMax);
            sum += w * BilinearLit(tuv, dim, saturate(zBase - bias));
            wsum += w;
        }
    }
    float lit = sum / wsum;
#ifdef CASCADE_BLEND
    if (tile < 2) {
        // How far toward its box's faces this point is (0.5 = on a face), y in tile units.
        float cy = tile == 0 ? 0.1666667 : 0.5;
        float e = max(max(abs(s.x - 0.5), 3.0 * abs(s.y - cy) + 0.003), abs(s.z - 0.5));
        float bw = saturate((e - (0.5 - kBlendBand)) / kBlendBand);
        float3 sn = tile == 0 ? s1 : s2;
        [branch] if (bw > 0 && Inside(sn, tile == 0 ? 0.5 : 0.8333333)) {
            float scn = tile == 0 ? c[35].y : c[35].z;
            float slopeN = tile == 0 ? SlopeFor(dx1, dy1) : SlopeFor(dx2, dy2);
            float zn = sn.z - 0.001 - saturate(ds * 256.0) * 0.01;
            lit = lerp(lit, FilterTile(tile + 1, sn, scn, slopeN, zn, dim, n), bw);
        }
    }
#endif
    return (lit * TINT(tile)).xxxx;
}
