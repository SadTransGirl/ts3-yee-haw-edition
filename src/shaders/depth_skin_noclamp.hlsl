// Copied from re\shaders_reconstructed\depth_skin_noclamp.hlsl (decomp session) for [graphics]
// shadow_noclamp_vs (gfx_d3d12.cpp): the material Depth VSs used for nearly every world caster
// (rocks, cliffs, buildings), swapped only into the shadow-map twins with depth clipping off.
// Built by build_shaders.cmd (the 9 define sets below). See shadowdepth_noclamp.hlsl for why.
//
// Reconstruction of the material "Depth" technique vertex shaders (the *_skin.tfx effects),
// without the per-vertex FMax(z, 0). Build: dxc -T vs_6_0 -E main -O3 <defines below>.
// Pair with DepthClipEnable = FALSE so the rasterizer clamps depth per pixel instead.
//
//   original VS (hash bytes 4..11)  size   defines
//   a46a66fa..                      3940   (none)
//   4c707152..                      3944   -D PERINST_2168     same code as a46a66fa, 8-byte longer cbuffer
//   e6f8ad75..                      4036   -D COLOR0_IN
//   1f78d030..                      4128   -D COLOR0_IN -D COLOR1_IN
//   a6af0f02..                      4136   -D TANGENT_LAYOUT
//   f6814a0c..                      4320   -D TANGENT_LAYOUT -D COLOR0_IN -D COLOR1_IN
//   437a6334..                      4096   -D UV_PASS          (alpha-tested: uv to the PS)
//   e0f23be1..                      4136   -D UV_SCROLL        (uv + per-instance scroll)
//   81557717..                      4336   -D UV_GLASS         (glass: uv + scale * direction)
//
// All of them skin with up to 4 bones (indices in TEXCOORD0 * 255, weights in TEXCOORD1) from a
// 42-bone palette of 3x4 matrices, then apply the instance's world 3x4 and the view's
// view-projection (PerView rows 16..19). Inputs the original declares but never reads are
// declared here too, in the same order, so the input signatures match.

cbuffer PerView : register(b1)      // 448 bytes
{
    float4 view_rows[28];
};

cbuffer PerInstance : register(b3)  // rows 0..2 world, row 8.x unused here, rows 9.. bones (3 rows each)
{
    float4 inst_rows[135];          // 2160 bytes
#if defined(UV_SCROLL) || defined(PERINST_2168)
    float2 uv_scroll;               // row 135.xy (2168 bytes)
#endif
};

#ifdef UV_GLASS
cbuffer GlassParams : register(b0)  // 100 bytes; only row 6.x is read
{
    float4 glass_pad[6];
    float  glass_scale;
};
cbuffer GlassDir : register(b2)     // 8 bytes
{
    float2 glass_dir;
};
#endif

struct VSIn
{
    float4 bone_idx : TEXCOORD0;
    float4 bone_w   : TEXCOORD1;
    float3 normal   : NORMAL0;      // unused
#ifdef TANGENT_LAYOUT
    float3 tan0     : TEXCOORD2;    // unused
    float3 tan1     : TEXCOORD3;    // unused
    float3 pos      : POSITION0;
    float2 uv       : TEXCOORD4;    // unused
#else
    float3 pos      : POSITION0;
    float2 uv       : TEXCOORD2;
#endif
#ifdef COLOR0_IN
    float4 color0   : COLOR0;       // unused
#endif
#ifdef COLOR1_IN
    float4 color1   : COLOR1;       // unused
#endif
};

struct VSOut
{
#if defined(UV_PASS) || defined(UV_SCROLL) || defined(UV_GLASS)
    float2 uv  : TEXCOORD0;
#endif
    float4 pos : SV_Position;
};

// Same operation order as the original: c.x*v.x, mad y, mad z, + c.w
float row3(float4 c, float3 v)
{
    float t = c.x * v.x;
    t = mad(c.y, v.y, t);
    t = mad(c.z, v.z, t);
    return c.w + t;
}

VSOut main(VSIn i)
{
    int4 b = int4(i.bone_idx * 255.0);
    float4 m[3];
    [unroll] for (int k = 0; k < 3; k++)
    {
        float4 r = inst_rows[b.y * 3 + 9 + k] * i.bone_w.y + inst_rows[b.x * 3 + 9 + k] * i.bone_w.x;
        r = r + inst_rows[b.z * 3 + 9 + k] * i.bone_w.z;
        r = r + inst_rows[b.w * 3 + 9 + k] * i.bone_w.w;
        m[k] = r;
    }
    float3 s = float3(row3(m[0], i.pos), row3(m[1], i.pos), row3(m[2], i.pos));
    float3 w = float3(row3(inst_rows[0], s), row3(inst_rows[1], s), row3(inst_rows[2], s));
    float4 c = float4(row3(view_rows[16], w), row3(view_rows[17], w), row3(view_rows[18], w), row3(view_rows[19], w));

    VSOut o;
#if defined(UV_PASS)
    o.uv = i.uv;
#elif defined(UV_SCROLL)
    o.uv = float2(uv_scroll.x + i.uv.x, i.uv.y - uv_scroll.y);
#elif defined(UV_GLASS)
    o.uv = float2(glass_scale * glass_dir.x + i.uv.x, glass_scale * glass_dir.y + i.uv.y);
#endif
    o.pos = c;                       // original: z = max(c.z, 0)
    return o;
}
