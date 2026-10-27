// Replacement for the vertex shader of the game's sun shadow casters, shadowdepth_
// VS0 (DXIL hash 8a7e6d1b768c0f25..., 2652 bytes) and, with -D AK, shadowdepth_ak_
// (438298d6a74b9383..., 2900 bytes), reconstructed from their DXIL by the decomp
// session (re\shaders_reconstructed). Same inputs, bindings (b1, b3), signatures and
// operation order; the one change is that clip-space z is not clamped to 0.
//
// The originals do FMax(z, 0) per vertex, so a big caster triangle (a bedroom wall,
// the window frame over the board-game board, a cliff overhang) with a vertex above a
// cascade's near plane gets that vertex lifted to depth 0 and the whole triangle's
// interpolated depth is wrong: its shadow vanishes in a straight-edged patch that
// jumps as the cascade refits to the camera (re\STOCK_BUGS.md). gfx_d3d12.cpp
// ([graphics] shadow_noclamp_vs=1) uses this only in the pipelines it binds while the
// shadow map is the depth target, which also have DepthClipEnable off, so the
// rasterizer clamps the depth per pixel instead: correct "pancaking".
//
// shadowdepth_ VS1 (99af0b69..., z = 0 constant) never clamps per vertex and is left
// alone. The skinned "Depth" vertex shaders clamp the same way; not replaced (yet).
//
// Built by build_shaders.cmd.

cbuffer PerView : register(b1)      // 448 bytes = 7 float4x4; rows 16..19 = light view-projection
{
    float4 view_rows[28];
};

cbuffer PerInstance : register(b3)  // 128 bytes = 2 float4x4; rows 0..2 = world (3x4)
{
    float4 inst_rows[8];
};

struct VSIn
{
    float3 pos : POSITION;
#ifdef AK
    float2 uv  : TEXCOORD0;
#endif
};

struct VSOut
{
    float2 depth : COLOR0;      // (z, w); the pixel shader never reads it
#ifdef AK
    float2 uv    : TEXCOORD0;   // packed into COLOR0's register .zw, as in the original
#endif
    float4 pos   : SV_Position;
};

// Same operation order as the original: c.x*v.x, mad y, mad z, + c.w
float row3(float4 c, float3 v)
{
    float t = c.x * v.x;
    t = mad(c.y, v.y, t);
    t = mad(c.z, v.z, t);
    return t + c.w;
}

VSOut main(VSIn i)
{
    float3 w = float3(row3(inst_rows[0], i.pos), row3(inst_rows[1], i.pos), row3(inst_rows[2], i.pos));
    float4 c = float4(row3(view_rows[16], w), row3(view_rows[17], w), row3(view_rows[18], w), row3(view_rows[19], w));
    VSOut o;
    o.depth = float2(c.z, c.w);   // original: max(c.z, 0)
#ifdef AK
    o.uv = i.uv;
#endif
    o.pos = c;                    // original: z = max(c.z, 0)
    return o;
}
