// Test replacement for the pixel shader of the game's ssao effect (DXIL digest
// 22bcf520...): no ambient occlusion at all. Output R = 1 means "unoccluded",
// and the game's ssao_blur pass passes 1 through, so the scene renders with AO
// switched off. Swapped in by gfx_d3d12.cpp with [graphics] ao_off=1 (it takes
// priority over better_ao), to check whether the stock SSAO draws the dark band
// under ledges on the Toy Box cliffs (re\STOCK_BUGS.md).
//
// Same input as the original (TEXCOORD0), so it fits the game's pipeline.
// Built by build_shaders.cmd.

float4 main(float2 uv : TEXCOORD0) : SV_Target {
    return float4(1.0, 1.0, 1.0, 1.0);
}
