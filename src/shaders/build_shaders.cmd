@echo off
rem Compiles the replacement pixel shaders to signed DXIL and regenerates the
rem headers gfx_d3d12.cpp embeds. Needs the Windows SDK's dxc.exe (next to dxil.dll).
rem Only needed after editing a .hlsl here; build.cmd uses the generated headers.
setlocal
set DXC="C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0\x64\dxc.exe"
set BIN2H=python -I "%~dp0..\..\tools\bin2h.py"
cd /d "%~dp0"
%DXC% -T ps_6_0 -E main -O3 -Fo shadowmask_soft.dxil shadowmask_soft.hlsl || exit /b 1
%DXC% -T ps_6_0 -E main -O3 -D NODB -Fo shadowmask_soft_nodb.dxil shadowmask_soft.hlsl || exit /b 1
%DXC% -T ps_6_0 -E main -O3 -Fo ssao_hq.dxil ssao_hq.hlsl || exit /b 1
%DXC% -T ps_6_0 -E main -O3 -Fo shadowmaskchar_soft.dxil shadowmaskchar_soft.hlsl || exit /b 1
%DXC% -T ps_6_0 -E main -O3 -Fo ssao_off.dxil ssao_off.hlsl || exit /b 1
%DXC% -T ps_6_0 -E main -O3 -D CASCADE_DEBUG -Fo shadowmask_debug.dxil shadowmask_soft.hlsl || exit /b 1
%DXC% -T ps_6_0 -E main -O3 -D CASCADE_DEBUG -D NODB -Fo shadowmask_debug_nodb.dxil shadowmask_soft.hlsl || exit /b 1
rem shadow_bias_fix (grazing-angle guard) at each shadow_slope_cap, and shadow_debug=2's bias view
%DXC% -T ps_6_0 -E main -O3 -D SLOPE_CAP=0.25 -Fo shadowmask_fix25.dxil shadowmask_soft.hlsl || exit /b 1
%DXC% -T ps_6_0 -E main -O3 -D SLOPE_CAP=0.1 -Fo shadowmask_fix10.dxil shadowmask_soft.hlsl || exit /b 1
%DXC% -T ps_6_0 -E main -O3 -D SLOPE_CAP=0.05 -Fo shadowmask_fix05.dxil shadowmask_soft.hlsl || exit /b 1
%DXC% -T ps_6_0 -E main -O3 -D SLOPE_CAP=0.02 -Fo shadowmask_fix02.dxil shadowmask_soft.hlsl || exit /b 1
%DXC% -T ps_6_0 -E main -O3 -D BIAS_VIEW -Fo shadowmask_bias.dxil shadowmask_soft.hlsl || exit /b 1
rem shadow_bias_fix=2: the slope clamped to shadow_slope_max_tan / 640 per stock texel
%DXC% -T ps_6_0 -E main -O3 -D MAX_TAN=5.0 -Fo shadowmask_tan5.dxil shadowmask_soft.hlsl || exit /b 1
%DXC% -T ps_6_0 -E main -O3 -D MAX_TAN=8.0 -Fo shadowmask_tan8.dxil shadowmask_soft.hlsl || exit /b 1
%DXC% -T ps_6_0 -E main -O3 -D MAX_TAN=12.0 -Fo shadowmask_tan12.dxil shadowmask_soft.hlsl || exit /b 1
rem shadow_cascade_blend=1 on top of those: cascades fade into the next one over their last 20%
%DXC% -T ps_6_0 -E main -O3 -D MAX_TAN=5.0 -D CASCADE_BLEND -Fo shadowmask_tan5b.dxil shadowmask_soft.hlsl || exit /b 1
%DXC% -T ps_6_0 -E main -O3 -D MAX_TAN=8.0 -D CASCADE_BLEND -Fo shadowmask_tan8b.dxil shadowmask_soft.hlsl || exit /b 1
%DXC% -T ps_6_0 -E main -O3 -D MAX_TAN=12.0 -D CASCADE_BLEND -Fo shadowmask_tan12b.dxil shadowmask_soft.hlsl || exit /b 1
%DXC% -T vs_6_0 -E main -O3 -Fo shadowdepth_noclamp.dxil shadowdepth_noclamp.hlsl || exit /b 1
%DXC% -T vs_6_0 -E main -O3 -D AK -Fo shadowdepth_noclamp_ak.dxil shadowdepth_noclamp.hlsl || exit /b 1
set DSK=%DXC% -T vs_6_0 -E main -O3
%DSK% -Fo dsk_a46a66fa.dxil depth_skin_noclamp.hlsl || exit /b 1
%DSK% -D PERINST_2168 -Fo dsk_4c707152.dxil depth_skin_noclamp.hlsl || exit /b 1
%DSK% -D COLOR0_IN -Fo dsk_e6f8ad75.dxil depth_skin_noclamp.hlsl || exit /b 1
%DSK% -D COLOR0_IN -D COLOR1_IN -Fo dsk_1f78d030.dxil depth_skin_noclamp.hlsl || exit /b 1
%DSK% -D TANGENT_LAYOUT -Fo dsk_a6af0f02.dxil depth_skin_noclamp.hlsl || exit /b 1
%DSK% -D TANGENT_LAYOUT -D COLOR0_IN -D COLOR1_IN -Fo dsk_f6814a0c.dxil depth_skin_noclamp.hlsl || exit /b 1
%DSK% -D UV_PASS -Fo dsk_437a6334.dxil depth_skin_noclamp.hlsl || exit /b 1
%DSK% -D UV_SCROLL -Fo dsk_e0f23be1.dxil depth_skin_noclamp.hlsl || exit /b 1
%DSK% -D UV_GLASS -Fo dsk_81557717.dxil depth_skin_noclamp.hlsl || exit /b 1
rem SMAA (iryoku/smaa, MIT): low, medium, high and ultra presets for the preset-dependent entry points.
set SMAA=%DXC% -O3 -I "%~dp0..\..\third_party\smaa_71c806a"
%SMAA% -T vs_6_0 -E EdgeVS -Fo smaa_edge_vs.dxil smaa_passes.hlsl || exit /b 1
%SMAA% -T ps_6_0 -E EdgePS -Fo smaa_edge_ps.dxil smaa_passes.hlsl || exit /b 1
%SMAA% -T ps_6_0 -E EdgePS -D SMAA_PRESET_ULTRA -Fo smaa_edge_ps_u.dxil smaa_passes.hlsl || exit /b 1
%SMAA% -T ps_6_0 -E EdgePS -D SMAA_PRESET_LOW -Fo smaa_edge_ps_l.dxil smaa_passes.hlsl || exit /b 1
%SMAA% -T ps_6_0 -E EdgePS -D SMAA_PRESET_MEDIUM -Fo smaa_edge_ps_m.dxil smaa_passes.hlsl || exit /b 1
%SMAA% -T vs_6_0 -E BlendVS -Fo smaa_blend_vs.dxil smaa_passes.hlsl || exit /b 1
%SMAA% -T vs_6_0 -E BlendVS -D SMAA_PRESET_ULTRA -Fo smaa_blend_vs_u.dxil smaa_passes.hlsl || exit /b 1
%SMAA% -T vs_6_0 -E BlendVS -D SMAA_PRESET_LOW -Fo smaa_blend_vs_l.dxil smaa_passes.hlsl || exit /b 1
%SMAA% -T vs_6_0 -E BlendVS -D SMAA_PRESET_MEDIUM -Fo smaa_blend_vs_m.dxil smaa_passes.hlsl || exit /b 1
%SMAA% -T ps_6_0 -E BlendPS -Fo smaa_blend_ps.dxil smaa_passes.hlsl || exit /b 1
%SMAA% -T ps_6_0 -E BlendPS -D SMAA_PRESET_ULTRA -Fo smaa_blend_ps_u.dxil smaa_passes.hlsl || exit /b 1
%SMAA% -T ps_6_0 -E BlendPS -D SMAA_PRESET_LOW -Fo smaa_blend_ps_l.dxil smaa_passes.hlsl || exit /b 1
%SMAA% -T ps_6_0 -E BlendPS -D SMAA_PRESET_MEDIUM -Fo smaa_blend_ps_m.dxil smaa_passes.hlsl || exit /b 1
%SMAA% -T vs_6_0 -E NeighborVS -Fo smaa_nb_vs.dxil smaa_passes.hlsl || exit /b 1
%SMAA% -T ps_6_0 -E NeighborPS -Fo smaa_nb_ps.dxil smaa_passes.hlsl || exit /b 1
%SMAA% -T ps_6_0 -E NeighborPS -D MANUAL_SRGB_OUT -Fo smaa_nb_ps_enc.dxil smaa_passes.hlsl || exit /b 1
%SMAA% -T ps_6_0 -E DebugPS -Fo smaa_debug_ps.dxil smaa_passes.hlsl || exit /b 1
(%BIN2H% shadowmask_soft.dxil kShadowMaskSoft && %BIN2H% shadowmask_soft_nodb.dxil kShadowMaskSoftNoDb) > ..\shadowmask_soft.h || exit /b 1
%BIN2H% ssao_hq.dxil kSsaoHq > ..\ssao_hq.h || exit /b 1
%BIN2H% shadowmaskchar_soft.dxil kShadowMaskCharSoft > ..\shadowmaskchar_soft.h || exit /b 1
%BIN2H% ssao_off.dxil kSsaoOff > ..\ssao_off.h || exit /b 1
(%BIN2H% shadowmask_debug.dxil kShadowMaskDebug && %BIN2H% shadowmask_debug_nodb.dxil kShadowMaskDebugNoDb) > ..\shadowmask_debug.h || exit /b 1
(%BIN2H% shadowmask_fix25.dxil kShadowMaskFix25 && %BIN2H% shadowmask_fix10.dxil kShadowMaskFix10 && %BIN2H% shadowmask_fix05.dxil kShadowMaskFix05 && %BIN2H% shadowmask_fix02.dxil kShadowMaskFix02 && %BIN2H% shadowmask_bias.dxil kShadowMaskBias && %BIN2H% shadowmask_tan5.dxil kShadowMaskTan5 && %BIN2H% shadowmask_tan8.dxil kShadowMaskTan8 && %BIN2H% shadowmask_tan12.dxil kShadowMaskTan12 && %BIN2H% shadowmask_tan5b.dxil kShadowMaskTan5B && %BIN2H% shadowmask_tan8b.dxil kShadowMaskTan8B && %BIN2H% shadowmask_tan12b.dxil kShadowMaskTan12B) > ..\shadowmask_biasfix.h || exit /b 1
(%BIN2H% shadowdepth_noclamp.dxil kShadowDepthNoClamp && %BIN2H% shadowdepth_noclamp_ak.dxil kShadowDepthNoClampAk) > ..\shadowdepth_noclamp.h || exit /b 1
(%BIN2H% dsk_a46a66fa.dxil kDskA46a && %BIN2H% dsk_4c707152.dxil kDsk4c70 && %BIN2H% dsk_e6f8ad75.dxil kDskE6f8 && %BIN2H% dsk_1f78d030.dxil kDsk1f78 && %BIN2H% dsk_a6af0f02.dxil kDskA6af && %BIN2H% dsk_f6814a0c.dxil kDskF681 && %BIN2H% dsk_437a6334.dxil kDsk437a && %BIN2H% dsk_e0f23be1.dxil kDskE0f2 && %BIN2H% dsk_81557717.dxil kDsk8155) > ..\depth_skin_noclamp.h || exit /b 1
(%BIN2H% smaa_edge_vs.dxil kSmaaEdgeVS && %BIN2H% smaa_edge_ps.dxil kSmaaEdgePS && %BIN2H% smaa_edge_ps_u.dxil kSmaaEdgePSU && %BIN2H% smaa_blend_vs.dxil kSmaaBlendVS && %BIN2H% smaa_blend_vs_u.dxil kSmaaBlendVSU && %BIN2H% smaa_blend_ps.dxil kSmaaBlendPS && %BIN2H% smaa_blend_ps_u.dxil kSmaaBlendPSU && %BIN2H% smaa_edge_ps_l.dxil kSmaaEdgePSL && %BIN2H% smaa_edge_ps_m.dxil kSmaaEdgePSM && %BIN2H% smaa_blend_vs_l.dxil kSmaaBlendVSL && %BIN2H% smaa_blend_vs_m.dxil kSmaaBlendVSM && %BIN2H% smaa_blend_ps_l.dxil kSmaaBlendPSL && %BIN2H% smaa_blend_ps_m.dxil kSmaaBlendPSM && %BIN2H% smaa_nb_vs.dxil kSmaaNbVS && %BIN2H% smaa_nb_ps.dxil kSmaaNbPS && %BIN2H% smaa_nb_ps_enc.dxil kSmaaNbPSEnc && %BIN2H% smaa_debug_ps.dxil kSmaaDebugPS) > ..\smaa_shaders.h || exit /b 1
echo shaders built
