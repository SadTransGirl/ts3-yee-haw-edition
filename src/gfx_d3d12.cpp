// Graphics extensions for the xinput1_4.dll proxy: D3D12/DXGI hooks and a few
// more engine patches. Kept in its own file so it doesn't collide with the
// harness/netplay work in the other src\*.cpp modules; it doesn't include
// proxy_shared.h and shares nothing with them but the entry points below.
//
// Entry points (the two installers run at load, before the game has made its D3D12 device):
//   GfxExt_Install         from ApplyGraphicsPatches() (graphics_patches.cpp), which DllMain
//                          (xinput_proxy.cpp) calls when [graphics] enabled=1. Reads this
//                          file's own [graphics] keys, patches the exe, and hooks the imports
//                          the game uses to create its device and swap chain.
//   GfxExt_InstallOverlay  from DebugMenu_Install (debug_menu.cpp): the menu draws itself in
//                          our Present pass. Installs the swap-chain hooks even without [graphics].
//   GfxExt_Get/SetFxaa, GfxExt_Get/SetSmaa: the debug menu's live switches.
// After that everything runs from inside the hooks, on whichever game thread makes the
// call: device and pipeline creation, command-list recording (possibly several threads at
// once, hence the SRW locks) and Present (once per frame, under g_fxLock).
//
// How the hooks work: D3D12 and DXGI objects are COM objects, so each method is an
// entry in a vtable shared by every object of that class. Replacing an entry (HookVtable)
// catches the call for all of them; the old entry is kept and called through. The first
// objects (device, factory) are caught by swapping the exe's import table slots for
// D3D12CreateDevice and CreateDXGIFactory2 (HookIat).
//
//   anisotropy        16x anisotropic filtering on the game's bilinear/trilinear samplers
//                     (ID3D12Device::CreateSampler and root-signature static samplers)
//   vrr               swap chain created with ALLOW_TEARING, Present1 with interval 0 +
//                     ALLOW_TEARING when windowed/borderless: G-Sync/FreeSync without vsync
//   fxaa              FXAA 3.11 (quality) pass on the finished frame at Present1, recorded on
//                     our own command list on the game's queue (independent of engine state)
//   volumetric_res    light shafts at 1/2 or full instead of 1/4 resolution
//   shadow_cascades,  shadow cascade count / distance / split, overriding level data
//   shadow_far, shadow_split
//   shadow_res        sun shadow map 1..4 times the game's 640x1920 (more texels per cascade)
//   shadow_pancake    fix: draws into the sun shadow map don't depth-clip, so casters between the
//                     sun and a cascade's near plane still cast ("shadow pancaking")
//   shadow_pso_log    diagnostic: log each depth-only pipeline and depth view once
//   light_override    override named effect parameters (sun, fog, ...) as they're set
//   soft_shadows      swap the sun shadow filter (shadowmask_4x4 and _nodb pixel shaders) for a
//                     bilinear-PCF version (src\shaders\shadowmask_soft.hlsl) at PSO creation
//   better_ao         swap the SSAO pixel shader for an 8-pair version (src\shaders\ssao_hq.hlsl)
//   ao_off            test switch: swap the SSAO pixel shader for one with no occlusion at all
//                     (src\shaders\ssao_off.hlsl); takes priority over better_ao
//   soft_char_shadow  swap the character blob shadow's pixel shader (shadowmaskchar, one point
//                     compare) for a 5x5 bilinear-PCF version (src\shaders\shadowmaskchar_soft.hlsl)
//   shadow_debug      test view: 1 = the sun shadow filter shades each cascade at its own brightness
//                     (shadowmask_soft.hlsl -D CASCADE_DEBUG); 2 = its slope bias as a fraction of the
//                     cap, white where capped (-D BIAS_VIEW); takes priority over soft_shadows
//   shadow_bias_fix   fix for lit triangles on surfaces seen nearly edge-on, in the soft sun shadow
//                     filter: 1 = grazing-angle guard (slope bias off there) with shadow_slope_cap
//                     0.25 (game) / 0.1 / 0.05 / 0.02 of the cascade's depth range; 2 = the slope
//                     clamped to shadow_slope_max_tan 5 / 8 / 12 texels per texel (edge-on: the max);
//                     with 2, shadow_cascade_blend=1 also fades each cascade into the next over its
//                     last 20%, so there's no hard line where the sharp near shadows end
//   shadow_noclamp_vs fix: draws into the sun shadow map use caster vertex shaders without the
//                     per-vertex depth clamp (shadowdepth_noclamp.hlsl), with depth clipping off
//   shadow_cull_none  test: draws into the sun shadow map cast with both faces
//   shadow_draw_audit diagnostic: every 20 s, per shadow cascade, which pipelines drew into the
//                     shadow map and whether each had its no-clip twin and no-clamp vertex shader
//   shadow_flash_probe diagnostic for the one-frame full-sun flash on camera cuts (re\SUN_FLASH.md):
//                     whether the sun shadow mask draw's vertex rays and constants are overwritten
//                     between recording and the GPU reading them
//   smaa              SMAA 1x (iryoku/smaa, MIT) on the 3D image before the HUD is drawn, or on
//                     the whole frame at Present where that isn't possible; smaa_preset
//                     low|medium|high|ultra (switchable live in the debug menu, with off),
//                     smaa_debug shows the detected edges. Wins over fxaa.
//
// Every exe patch checks the bytes first and is skipped, with a log line, if this
// build differs. Addresses: image base 0x140000000, see re\ENGINE_GFX_PLAN.md.
//
// Reading the addresses: they were found by reverse engineering the game's exe (the
// "decomp session", Ghidra and IDA). FUN_1401f3b30 / DAT_140b2ce48 are Ghidra's names for
// the function / data at that address when the exe loads at 0x140000000. An RVA is the
// same address as an offset from the exe's base (0x1401f3b30 -> RVA 0x1f3b30); the code
// adds it to ExeBase(), so it works wherever Windows loads the exe. The byte arrays next
// to each patch (prologue, sig, fs...) are the instructions expected there in the Steam
// build; BytesAt compares them before anything is written.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "shadowmask_soft.h"
#include "shadowmask_debug.h"
#include "shadowmask_biasfix.h"
#include "shadowdepth_noclamp.h"
#include "depth_skin_noclamp.h"
#include "shadowmaskchar_soft.h"
#include "ssao_hq.h"
#include "ssao_off.h"
#include "smaa_shaders.h"
#include "../third_party/smaa_71c806a/Textures/AreaTex.h"     // MIT, (c) Jorge Jimenez et al.
#include "../third_party/smaa_71c806a/Textures/SearchTex.h"

typedef void (*GfxLogFn)(const char* fmt, ...);
static GfxLogFn Log;                    // the harness log (harness_core.cpp's Log), set by GfxExt_Install

// ------------------------------------------------------------------ helpers
//
// Private copies of the patch helpers (patch_util.cpp has the harness's own), so this
// file stands alone.

// The game exe's load address (its module handle). Every RVA below is added to it.
static BYTE* ExeBase() { return (BYTE*)GetModuleHandleW(NULL); }

// True if the n bytes at exe + rva are exactly 'expect': the check every patch makes
// before writing, so a different build of the game is skipped instead of broken. An
// unreadable address counts as a mismatch.
static bool BytesAt(DWORD rva, const void* expect, size_t n) {
    __try {
        return memcmp(ExeBase() + rva, expect, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Copy n bytes over dst, lifting the page protection for the write (the exe's code is
// read/execute only, its import table read-only). code: also flush the CPU's instruction
// cache so the new instructions are the ones that run.
static bool WriteAbs(void* dst, const void* src, size_t n, bool code) {
    DWORD old;
    if (!VirtualProtect(dst, n, code ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &old)) return false;
    memcpy(dst, src, n);
    VirtualProtect(dst, n, old, &old);
    if (code) FlushInstructionCache(GetCurrentProcess(), dst, n);
    return true;
}

// Executable memory within 2 GB of site, searched outward one allocation granule at a
// time, so a 5-byte call/jump with a 32-bit offset at site can reach it. NULL if none.
static BYTE* AllocNear(BYTE* site, size_t size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity;
    uintptr_t base = (uintptr_t)site & ~(gran - 1);
    for (uintptr_t d = gran; d < 0x7FF00000; d += gran) {
        uintptr_t cand[2] = { base + d, base > d ? base - d : 0 };
        for (uintptr_t a : cand) {
            if (!a) continue;
            void* p = VirtualAlloc((void*)a, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) return (BYTE*)p;
        }
    }
    return NULL;
}

// A 14-byte jump to any 64-bit address: FF 25 00000000 reads its target from the 8
// bytes right after the instruction.
static void WriteAbsJmp(BYTE* at, void* target) {
    at[0] = 0xFF; at[1] = 0x25;                    // jmp qword ptr [rip+0]
    *(int32_t*)(at + 2) = 0;
    memcpy(at + 6, &target, 8);
}

// Point the 5-byte "call rel32" at callRva (which must call expectRva) at hook.
// Our DLL may be loaded more than 2 GB away from the exe, out of a rel32's reach, so
// the call goes to a stub near the exe that jumps on to the hook. Only that one call
// site changes; other callers of expectRva still reach the original.
static bool RedirectCall(DWORD callRva, DWORD expectRva, void* hook) {
    BYTE* call = ExeBase() + callRva;
    __try {
        if (call[0] != 0xE8 || call + 5 + *(int32_t*)(call + 1) != ExeBase() + expectRva) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    BYTE* stub = AllocNear(call, 16);
    if (!stub) return false;
    WriteAbsJmp(stub, hook);
    FlushInstructionCache(GetCurrentProcess(), stub, 14);
    int64_t rel = stub - (call + 5);
    if (rel != (int32_t)rel) return false;
    BYTE patch[5] = { 0xE8 };
    int32_t rel32 = (int32_t)rel;
    memcpy(patch + 1, &rel32, 4);
    return WriteAbs(call, patch, sizeof patch, true);
}

// Detour a function whose first 'len' bytes (>= 14, whole instructions, nothing
// RIP-relative) are 'prologue'. Returns a callable trampoline to the original.
// The function's first 14 bytes become a jump to hook; the trampoline runs the
// saved prologue and jumps back to rva + len. NULL if the bytes don't match.
static void* Detour(DWORD rva, const BYTE* prologue, size_t len, void* hook) {
    if (len < 14 || !BytesAt(rva, prologue, len)) return NULL;
    BYTE* fn = ExeBase() + rva;
    BYTE* tramp = AllocNear(fn, 64);
    if (!tramp) return NULL;
    memcpy(tramp, prologue, len);
    WriteAbsJmp(tramp + len, fn + len);
    FlushInstructionCache(GetCurrentProcess(), tramp, len + 14);
    BYTE patch[14];
    WriteAbsJmp(patch, hook);
    if (!WriteAbs(fn, patch, sizeof patch, true)) return NULL;
    return tramp;
}

// Replace method 'index' in obj's vtable with hook; *orig gets the method it replaced,
// which the hook calls through. The vtable is shared by every object of that class, so
// this hooks them all, including ones made later. Slot numbers are the method's position
// in the interface declaration (d3d12.h / dxgi1_6.h), inherited methods first.
static bool HookVtable(void* obj, int index, void* hook, void** orig) {
    void** vt = *(void***)obj;
    if (vt[index] == hook) return true;            // shared vtable, already ours
    *orig = vt[index];
    return WriteAbs(&vt[index], &hook, sizeof hook, false);
}

// IAT slot at rva must currently hold 'expect'.
// The import address table holds the address of each DLL function the exe calls
// (D3D12CreateDevice...); every call goes through its slot, so writing hook there
// redirects them all. Returns the original function, or NULL if nothing was changed.
static void* HookIat(DWORD rva, void* expect, void* hook) {
    void** slot = (void**)(ExeBase() + rva);
    __try {
        if (!expect || *slot != expect) return NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
    void* orig = *slot;
    return WriteAbs(slot, &hook, sizeof hook, false) ? orig : NULL;
}

// ------------------------------------------------------------------ config

// The [graphics] keys of xinput_harness.ini that this file handles (graphics_patches.cpp
// reads the others into g_gfx). Filled once by LoadExtConfig; the debug menu may flip
// fxaa (and the SMAA level, kept in g_sm.level) later.
struct ExtConfig {
    int   anisotropy;          // 0 = game's filtering
    bool  vrr;
    bool  fxaa;
    float fxaaSubpix, fxaaEdge, fxaaEdgeMin;
    int   volumetricRes;       // 4 = game (1/4), 2 = half, 1 = full
    int   shadowCascades;      // 0 = level data
    float shadowFar, shadowSplit;
    int   shadowRes;           // 1 = game (640x1920 atlas), 2..4 = that many times larger
    bool  softShadows;         // bilinear-PCF sun shadow filter
    bool  betterAo;            // 8-pair SSAO
    bool  aoOff;               // test: no SSAO at all (wins over betterAo)
    bool  softCharShadow;      // bilinear-PCF character blob shadow
    bool  shadowPsoLog;        // diagnostic: log each depth-only pipeline once
    bool  shadowPancake;       // shadow casters in front of a cascade's near plane still cast
    bool  shadowDebug;         // test view: each shadow cascade at its own brightness (shadow_debug=1)
    bool  shadowBiasView;      // test view: the slope bias against its cap (shadow_debug=2)
    bool  shadowBiasFix[4];    // shadow_bias_fix=1 at shadow_slope_cap 0.25 / 0.1 / 0.05 / 0.02 (one of them)
    bool  shadowBiasTan[3];    // shadow_bias_fix=2 at shadow_slope_max_tan 5 / 8 / 12 (one of them)
    bool  shadowBiasTanBlend[3];   // the same with shadow_cascade_blend=1 (instead)
    bool  shadowNoClampVs;     // shadow-map draws: caster VS without the per-vertex depth clamp
    bool  shadowCullNone;      // shadow-map draws: both faces cast (the game culls front faces)
    bool  shadowDrawAudit;     // diagnostic: per-draw audit of the shadow map's pipelines
    bool  shadowFlashProbe;    // diagnostic: are the shadow mask's buffers rewritten before the GPU reads them
    bool  smaa;                // SMAA 1x before the HUD (falls back to the whole frame at Present); wins over fxaa
    int   smaaPreset;          // smaa_preset low|medium|high|ultra = 1..4 (default medium)
    bool  smaaDebug;           // show SMAA's detected edges instead of the image
    char  lightOverride[1024];
};
static ExtConfig g_x;

// A float [graphics] key; def when it's missing or empty.
static float IniFloat(const wchar_t* ini, const wchar_t* key, float def) {
    wchar_t buf[64];
    GetPrivateProfileStringW(L"graphics", key, L"", buf, 64, ini);
    if (!buf[0]) return def;
    return (float)_wtof(buf);
}

// Read this file's [graphics] keys into g_x (from GfxExt_Install). The shadow_bias_fix
// variants are shaders compiled in advance, one per value, so shadow_slope_cap and
// shadow_slope_max_tan snap to the nearest value there is a shader for.
static void LoadExtConfig(const wchar_t* ini) {
    g_x.anisotropy     = GetPrivateProfileIntW(L"graphics", L"anisotropy", 0, ini);
    g_x.vrr            = GetPrivateProfileIntW(L"graphics", L"vrr", 0, ini) != 0;
    g_x.fxaa           = GetPrivateProfileIntW(L"graphics", L"fxaa", 0, ini) != 0;
    g_x.fxaaSubpix     = IniFloat(ini, L"fxaa_subpix", 0.75f);
    g_x.fxaaEdge       = IniFloat(ini, L"fxaa_edge", 0.125f);
    g_x.fxaaEdgeMin    = IniFloat(ini, L"fxaa_edge_min", 0.0312f);
    g_x.volumetricRes  = GetPrivateProfileIntW(L"graphics", L"volumetric_res", 4, ini);
    g_x.shadowCascades = GetPrivateProfileIntW(L"graphics", L"shadow_cascades", 0, ini);
    g_x.shadowFar      = IniFloat(ini, L"shadow_far", 0.0f);
    g_x.shadowSplit    = IniFloat(ini, L"shadow_split", 0.0f);
    g_x.shadowRes      = GetPrivateProfileIntW(L"graphics", L"shadow_res", 1, ini);
    g_x.softShadows    = GetPrivateProfileIntW(L"graphics", L"soft_shadows", 0, ini) != 0;
    g_x.betterAo       = GetPrivateProfileIntW(L"graphics", L"better_ao", 0, ini) != 0;
    g_x.aoOff          = GetPrivateProfileIntW(L"graphics", L"ao_off", 0, ini) != 0;
    g_x.softCharShadow = GetPrivateProfileIntW(L"graphics", L"soft_char_shadow", 0, ini) != 0;
    g_x.shadowPsoLog   = GetPrivateProfileIntW(L"graphics", L"shadow_pso_log", 0, ini) != 0;
    g_x.shadowPancake  = GetPrivateProfileIntW(L"graphics", L"shadow_pancake", 0, ini) != 0;
    int debugMode      = GetPrivateProfileIntW(L"graphics", L"shadow_debug", 0, ini);
    g_x.shadowDebug    = debugMode == 1;
    g_x.shadowBiasView = debugMode == 2;
    int biasFix = GetPrivateProfileIntW(L"graphics", L"shadow_bias_fix", 0, ini);
    if (biasFix == 2) {
        static const float kTans[3] = { 5.0f, 8.0f, 12.0f };
        float tan = IniFloat(ini, L"shadow_slope_max_tan", 8.0f);
        int best = 0;
        for (int i = 1; i < 3; i++) if (fabsf(tan - kTans[i]) < fabsf(tan - kTans[best])) best = i;
        if (GetPrivateProfileIntW(L"graphics", L"shadow_cascade_blend", 0, ini) != 0) g_x.shadowBiasTanBlend[best] = true;
        else g_x.shadowBiasTan[best] = true;
    } else if (biasFix != 0) {
        static const float kCaps[4] = { 0.25f, 0.1f, 0.05f, 0.02f };
        float cap = IniFloat(ini, L"shadow_slope_cap", 0.25f);
        int best = 0;
        for (int i = 1; i < 4; i++) if (fabsf(cap - kCaps[i]) < fabsf(cap - kCaps[best])) best = i;
        g_x.shadowBiasFix[best] = true;
    }
    g_x.shadowNoClampVs = GetPrivateProfileIntW(L"graphics", L"shadow_noclamp_vs", 0, ini) != 0;
    g_x.shadowCullNone  = GetPrivateProfileIntW(L"graphics", L"shadow_cull_none", 0, ini) != 0;
    g_x.shadowDrawAudit = GetPrivateProfileIntW(L"graphics", L"shadow_draw_audit", 0, ini) != 0;
    g_x.shadowFlashProbe = GetPrivateProfileIntW(L"graphics", L"shadow_flash_probe", 0, ini) != 0;
    g_x.smaa            = GetPrivateProfileIntW(L"graphics", L"smaa", 0, ini) != 0;
    g_x.smaaDebug       = GetPrivateProfileIntW(L"graphics", L"smaa_debug", 0, ini) != 0;
    {
        wchar_t p[16];
        GetPrivateProfileStringW(L"graphics", L"smaa_preset", L"medium", p, 16, ini);
        g_x.smaaPreset = !_wcsicmp(p, L"low") ? 1 : !_wcsicmp(p, L"high") ? 3 : !_wcsicmp(p, L"ultra") ? 4 : 2;
    }
    // light_override is kept as UTF-8 text; ParseLightOverrides splits it later.
    wchar_t w[1024];
    GetPrivateProfileStringW(L"graphics", L"light_override", L"", w, 1024, ini);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, g_x.lightOverride, sizeof g_x.lightOverride, NULL, NULL);
}

// ------------------------------------------------------------------ volumetric light

// RT_VolumetricLight_Setup sizes its two light-shaft targets (and the matching
// downsample) from the display size >> 2. Those passes bind no depth buffer, so
// they can be any size.
static const DWORD RVA_VOL_SHR_W = 0x1f5654;   // 66 C1 E8 02     shr ax,2
static const DWORD RVA_VOL_SHR_H = 0x1f565c;   // 66 C1 6D CD 02  shr word [rbp-33h],2

// volumetric_res: rewrite the shift count (the 02 in each "shr ..., 2") to 1 (half
// resolution) or 0 (full).
static void PatchVolumetric() {
    if (g_x.volumetricRes == 4) return;
    BYTE shift = g_x.volumetricRes == 2 ? 1 : g_x.volumetricRes == 1 ? 0 : 0xFF;
    if (shift == 0xFF) { Log("graphics: volumetric_res must be 4, 2 or 1; ignored"); return; }
    static const BYTE w[] = { 0x66, 0xC1, 0xE8, 0x02, 0x66, 0x89, 0x45, 0xCB };
    static const BYTE h[] = { 0x66, 0xC1, 0x6D, 0xCD, 0x02 };
    if (!BytesAt(RVA_VOL_SHR_W, w, sizeof w) || !BytesAt(RVA_VOL_SHR_H, h, sizeof h)) {
        Log("graphics: volumetric_res skipped: code doesn't match this build");
        return;
    }
    if (WriteAbs(ExeBase() + RVA_VOL_SHR_W + 3, &shift, 1, true) &&
        WriteAbs(ExeBase() + RVA_VOL_SHR_H + 4, &shift, 1, true))
        Log("graphics: volumetric light at 1/%d resolution (game: 1/4)", g_x.volumetricRes);
}

// ------------------------------------------------------------------ shadow cascades

// ShadowCascades_Create(cascades, settings) reads from the level's settings:
// +0x20 cascade count (1..4, else 3), +0x24 near, +0x28 far (10..360, else 50),
// +0x2c split (0..0.8, else 0.5). Its two callers are redirected here; the
// overrides are written for the call and the level's values put back after.
static const DWORD RVA_SHADOW_CREATE = 0x1de0f0;
static const DWORD RVA_SHADOW_CALL1  = 0x1dd494;
static const DWORD RVA_SHADOW_CALL2  = 0x1d62c6;
typedef void (*PFN_ShadowCreate)(void* cascades, BYTE* settings);

// Stands in for ShadowCascades_Create at both call sites: our values into the level's
// settings, the real call, then the level's values back. Logs the first call.
static void ShadowCreateHook(void* cascades, BYTE* s) {
    PFN_ShadowCreate orig = (PFN_ShadowCreate)(ExeBase() + RVA_SHADOW_CREATE);
    int saveN = *(int*)(s + 0x20);
    float saveFar = *(float*)(s + 0x28), saveSplit = *(float*)(s + 0x2c);
    if (g_x.shadowCascades) *(int*)(s + 0x20) = g_x.shadowCascades;
    if (g_x.shadowFar > 0.0f) *(float*)(s + 0x28) = g_x.shadowFar;
    if (g_x.shadowSplit > 0.0f) *(float*)(s + 0x2c) = g_x.shadowSplit;
    static bool logged;
    if (!logged) {
        logged = true;
        Log("graphics: shadow cascades: level asked for %d cascades, far %.1f, split %.2f; using %d, %.1f, %.2f",
            saveN, saveFar, saveSplit, *(int*)(s + 0x20), *(float*)(s + 0x28), *(float*)(s + 0x2c));
    }
    orig(cascades, s);
    *(int*)(s + 0x20) = saveN;
    *(float*)(s + 0x28) = saveFar;
    *(float*)(s + 0x2c) = saveSplit;
}

// shadow_cascades / shadow_far / shadow_split: if any is set, send both calls to
// ShadowCreateHook. RedirectCall checks each call site's target first.
static void PatchShadows() {
    if (!g_x.shadowCascades && g_x.shadowFar <= 0.0f && g_x.shadowSplit <= 0.0f) return;
    if (g_x.shadowCascades < 0 || g_x.shadowCascades > 4) { Log("graphics: shadow_cascades must be 0..4; ignored"); return; }
    if (g_x.shadowFar > 360.0f) g_x.shadowFar = 360.0f;    // the engine clamps above this anyway
    if (g_x.shadowSplit > 0.8f) g_x.shadowSplit = 0.8f;
    if (RedirectCall(RVA_SHADOW_CALL1, RVA_SHADOW_CREATE, (void*)ShadowCreateHook) &&
        RedirectCall(RVA_SHADOW_CALL2, RVA_SHADOW_CREATE, (void*)ShadowCreateHook))
        Log("graphics: shadow overrides armed (cascades=%d far=%.1f split=%.2f; 0 = level's value)",
            g_x.shadowCascades, g_x.shadowFar, g_x.shadowSplit);
    else
        Log("graphics: shadow overrides skipped: code doesn't match this build");
}

// The sun's shadow map is render target slot 0 ("shadowBuffer"), an atlas of
// three 640x640 cascade tiles stacked vertically (640x1920), created in
// FUN_1401f3b30 from a size written as one immediate. Each cascade's viewport
// is computed from the texture (height / cascade count, FUN_1401eb800), so
// scaling both sides keeps the layout and just gives every tile more texels.
static const DWORD RVA_SHADOW_SIZE = 0x1f3baf;   // C7 45 BB 80 02 80 07   mov dword [rbp-45h], 0x07800280
static uint32_t g_atlasW = 640, g_atlasH = 1920;  // the shadow map's actual size (shadow pancaking finds it by this)

// shadow_res: the immediate 0x07800280 is the size packed as height << 16 | width
// (1920, 640); it's rewritten with both multiplied. g_atlasW/H follow, so the
// pancaking code still recognises the atlas by its size.
static void PatchShadowRes() {
    if (g_x.shadowRes == 1) return;
    if (g_x.shadowRes < 1 || g_x.shadowRes > 4) { Log("graphics: shadow_res must be 1..4; ignored"); return; }
    static const BYTE sig[] = { 0xC7, 0x45, 0xBB, 0x80, 0x02, 0x80, 0x07, 0xE8 };   // + the call that creates it
    if (!BytesAt(RVA_SHADOW_SIZE, sig, sizeof sig)) {
        Log("graphics: shadow_res skipped: code doesn't match this build");
        return;
    }
    uint32_t w = 640u * g_x.shadowRes, h = 1920u * g_x.shadowRes;
    uint32_t packed = (h << 16) | w;
    if (WriteAbs(ExeBase() + RVA_SHADOW_SIZE + 3, &packed, 4, true)) {
        g_atlasW = w;
        g_atlasH = h;
        Log("graphics: shadow map %ux%u, %ux%u per cascade (game: 640x1920)", w, h, w, w);
    }
}

// ------------------------------------------------------------------ light overrides

// Every effect-parameter write goes through FUN_1402c6690(param, effect, ...,
// const float* values, first, count). A parameter handle starts with the key the
// engine interns for its name (Effect_FindParam compares exactly that), so names
// are resolved through the same table, lazily, because effects load later.
static const DWORD RVA_PARAM_SET   = 0x2c6690;
static const DWORD RVA_NAME_LOOKUP = 0x079440;   // FUN_140079440(table, name, len+1) -> slot
static const DWORD RVA_NAME_TABLE  = 0x948000;
typedef void (*PFN_ParamSet)(BYTE* param, void* effect, void* p3, const float* v, int first, unsigned count);
typedef void** (*PFN_NameLookup)(void* table, const char* name, int len);
static PFN_ParamSet g_paramSetOrig;

// One light_override entry: the parameter's name, up to 4 values (n of them), and the
// engine's interned key for the name once the name table has it (NULL until then).
struct LightOverride { char name[64]; float v[4]; int n; void* key; };
static LightOverride g_lo[16];
static int g_loCount;
static bool g_loLogged[16];
static unsigned g_loTries;      // names not interned yet are retried every 1024 writes, not on each

// Split g_x.lightOverride into g_lo (at most 16 entries). Entries with no '=', no
// name or nothing after the '=' are dropped.
static void ParseLightOverrides() {
    // name=v[,v,v,v]; name=v ...
    char buf[1024];
    strcpy_s(buf, g_x.lightOverride);
    char* ctx = NULL;
    for (char* item = strtok_s(buf, ";", &ctx); item && g_loCount < 16; item = strtok_s(NULL, ";", &ctx)) {
        while (*item == ' ') item++;
        char* eq = strchr(item, '=');
        if (!eq) continue;
        *eq = 0;
        LightOverride& o = g_lo[g_loCount];
        char* end = item + strlen(item);
        while (end > item && end[-1] == ' ') *--end = 0;
        strcpy_s(o.name, item);
        o.n = 0;
        for (char* p = eq + 1; *p && o.n < 4;) {
            o.v[o.n++] = (float)strtod(p, &p);
            while (*p == ',' || *p == ' ') p++;
        }
        if (o.n && o.name[0]) g_loCount++;
    }
}

// The detoured parameter setter. When the parameter's key is one we override, its
// values are copied, our components written over the first ones, and the copy passed
// on; anything else (or a fault reading the handle) goes through unchanged. Fields read
// from the handle: +8 its type (only 0 and 2 are overridden), +9 its element count
// (used when count is 0xFFFFFFFF, "all"), +0xa a per-element multiplier, +0xb its
// components (1..4).
static void ParamSetHook(BYTE* param, void* effect, void* p3, const float* v, int first, unsigned count) {
    __try {
        void* key = *(void**)param;
        bool retry = (g_loTries++ & 1023) == 0;
        for (int i = 0; i < g_loCount; i++) {
            LightOverride& o = g_lo[i];
            if (!o.key) {
                if (!retry) continue;
                void** slot = ((PFN_NameLookup)(ExeBase() + RVA_NAME_LOOKUP))(
                    ExeBase() + RVA_NAME_TABLE, o.name, (int)strlen(o.name) + 1);
                if (slot && *slot) o.key = *slot;
                else continue;
            }
            if (key != o.key) continue;
            BYTE kind = param[8], comps = param[0xb];
            if ((kind != 0 && kind != 2) || comps == 0 || comps > 4) break;
            float tmp[64];
            unsigned elems = param[0xa] * (count == 0xFFFFFFFFu ? param[9] : count);
            if (!v || elems * comps > 64) break;
            memcpy(tmp, v, elems * comps * sizeof(float));
            for (int c = 0; c < o.n && c < comps; c++) tmp[c] = o.v[c];
            if (!g_loLogged[i]) {
                g_loLogged[i] = true;
                Log("graphics: light_override %s: %.3f -> %.3f (%d component(s))", o.name, v[0], tmp[0], comps);
            }
            g_paramSetOrig(param, effect, p3, tmp, first, count);
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_paramSetOrig(param, effect, p3, v, first, count);
}

// light_override: parse the entries, check the name lookup we'll call, and detour the
// parameter setter.
static void PatchLightOverrides() {
    if (!g_x.lightOverride[0]) return;
    ParseLightOverrides();
    if (!g_loCount) { Log("graphics: light_override has no valid name=value entries; ignored"); return; }
    // push rbx; push rdi; push r14; sub rsp,30h; mov r14d,[rsp+78h]   (exactly 14 bytes)
    static const BYTE prologue[] = { 0x40, 0x53, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30,
                                     0x44, 0x8B, 0x74, 0x24, 0x78 };
    static const BYTE lookup[] = { 0x48, 0x89, 0x5C, 0x24 };   // FUN_140079440 starts mov [rsp+..],rbx
    if (!BytesAt(RVA_NAME_LOOKUP, lookup, sizeof lookup)) {
        Log("graphics: light_override skipped: name lookup doesn't match this build");
        return;
    }
    g_paramSetOrig = (PFN_ParamSet)Detour(RVA_PARAM_SET, prologue, sizeof prologue, (void*)ParamSetHook);
    if (g_paramSetOrig) Log("graphics: light_override armed for %d parameter(s)", g_loCount);
    else Log("graphics: light_override skipped: parameter setter doesn't match this build");
}

// ------------------------------------------------------------------ samplers

// anisotropy: make a linear sampler anisotropic at g_x.anisotropy; false if it's left
// alone. In D3D12_FILTER, bits 0x180 are the reduction (comparison / min / max: shadow
// map lookups and the like) and 0x10 / 0x04 are min / mag linear.
static bool UpgradeFilter(D3D12_FILTER* f, UINT* aniso) {
    // Bilinear or trilinear, non-comparison, non-min/max: min and mag linear.
    if ((*f & 0x180) != 0 || (*f & 0x14) != 0x14 || *f == D3D12_FILTER_ANISOTROPIC) return false;
    *f = D3D12_FILTER_ANISOTROPIC;
    *aniso = (UINT)g_x.anisotropy;
    return true;
}

typedef void (STDMETHODCALLTYPE* PFN_CreateSampler)(ID3D12Device*, const D3D12_SAMPLER_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
static PFN_CreateSampler g_createSampler;
static volatile LONG g_samplersUpgraded;

// ID3D12Device::CreateSampler (slot 22): samplers written into descriptor heaps.
static void STDMETHODCALLTYPE CreateSamplerHook(ID3D12Device* dev, const D3D12_SAMPLER_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    if (d) {
        D3D12_SAMPLER_DESC c = *d;
        if (UpgradeFilter(&c.Filter, &c.MaxAnisotropy)) {
            if (InterlockedIncrement(&g_samplersUpgraded) == 1) Log("graphics: anisotropic filtering x%d active", g_x.anisotropy);
            g_createSampler(dev, &c, h);
            return;
        }
    }
    g_createSampler(dev, d, h);
}

typedef HRESULT (WINAPI* PFN_Serialize)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
static PFN_Serialize g_serialize;

// D3D12SerializeRootSignature (the exe's import): static samplers, which are baked
// into root signatures instead. Up to 32 are copied and upgraded.
static HRESULT WINAPI SerializeHook(const D3D12_ROOT_SIGNATURE_DESC* d, D3D_ROOT_SIGNATURE_VERSION v, ID3DBlob** b, ID3DBlob** e) {
    if (d && d->NumStaticSamplers && d->NumStaticSamplers <= 32) {
        D3D12_STATIC_SAMPLER_DESC s[32];
        memcpy(s, d->pStaticSamplers, d->NumStaticSamplers * sizeof s[0]);
        bool any = false;
        for (UINT i = 0; i < d->NumStaticSamplers; i++) any |= UpgradeFilter(&s[i].Filter, &s[i].MaxAnisotropy);
        if (any) {
            D3D12_ROOT_SIGNATURE_DESC c = *d;
            c.pStaticSamplers = s;
            return g_serialize(&c, v, b, e);
        }
    }
    return g_serialize(d, v, b, e);
}

// ------------------------------------------------------------------ shader swap (soft shadows, better AO)

// Pixel shaders of the game's effects (assets\effects\fx_new.zip), by DXBC container
// size and header digest: the game creates its pipelines from these exact bytes.
// The replacements are src\shaders\*.hlsl, built by build_shaders.cmd.
struct ShaderSwap {
    const char* what;           // for the log
    UINT size;                  // the game shader's bytecode length
    BYTE digest[16];            // ... and the 16-byte digest at offset 4 of its DXBC container
    const BYTE* with;           // the replacement (a generated header's byte array)
    UINT withSize;
    bool* enabled;              // the g_x switch that turns this swap on
    volatile LONG count;        // pipelines made with it (the first is logged)
};
static ShaderSwap g_swaps[] = {
    // shadow_debug first: the first matching entry wins, so it takes priority over soft_shadows.
    { "shadowmask_4x4 (sun shadow filter) with cascade colours (shadow_debug)", 5076,
      { 0x9c, 0x9d, 0x69, 0x68, 0x22, 0x36, 0x03, 0x27, 0x5f, 0x2f, 0x2e, 0xee, 0xf7, 0xc7, 0x92, 0x83 },
      kShadowMaskDebug, sizeof kShadowMaskDebug, &g_x.shadowDebug },
    { "shadowmask_4x4_nodb (sun shadow filter, no depth buffer) with cascade colours (shadow_debug)", 4788,
      { 0x5e, 0xb9, 0x86, 0xee, 0x41, 0xc2, 0xdc, 0xe0, 0x49, 0xa8, 0x19, 0xb7, 0x9c, 0xda, 0xc6, 0x1e },
      kShadowMaskDebugNoDb, sizeof kShadowMaskDebugNoDb, &g_x.shadowDebug },
    { "shadowmask_4x4 (sun shadow filter) as its slope bias (shadow_debug=2)", 5076,
      { 0x9c, 0x9d, 0x69, 0x68, 0x22, 0x36, 0x03, 0x27, 0x5f, 0x2f, 0x2e, 0xee, 0xf7, 0xc7, 0x92, 0x83 },
      kShadowMaskBias, sizeof kShadowMaskBias, &g_x.shadowBiasView },
    // shadow_bias_fix: before soft_shadows, so it wins (it is the soft filter plus the fix).
    { "shadowmask_4x4 (sun shadow filter), soft, slope clamped to 5 texels per texel, cascades blended (shadow_bias_fix=2, shadow_cascade_blend)", 5076,
      { 0x9c, 0x9d, 0x69, 0x68, 0x22, 0x36, 0x03, 0x27, 0x5f, 0x2f, 0x2e, 0xee, 0xf7, 0xc7, 0x92, 0x83 },
      kShadowMaskTan5B, sizeof kShadowMaskTan5B, &g_x.shadowBiasTanBlend[0] },
    { "shadowmask_4x4 (sun shadow filter), soft, slope clamped to 8 texels per texel, cascades blended (shadow_bias_fix=2, shadow_cascade_blend)", 5076,
      { 0x9c, 0x9d, 0x69, 0x68, 0x22, 0x36, 0x03, 0x27, 0x5f, 0x2f, 0x2e, 0xee, 0xf7, 0xc7, 0x92, 0x83 },
      kShadowMaskTan8B, sizeof kShadowMaskTan8B, &g_x.shadowBiasTanBlend[1] },
    { "shadowmask_4x4 (sun shadow filter), soft, slope clamped to 12 texels per texel, cascades blended (shadow_bias_fix=2, shadow_cascade_blend)", 5076,
      { 0x9c, 0x9d, 0x69, 0x68, 0x22, 0x36, 0x03, 0x27, 0x5f, 0x2f, 0x2e, 0xee, 0xf7, 0xc7, 0x92, 0x83 },
      kShadowMaskTan12B, sizeof kShadowMaskTan12B, &g_x.shadowBiasTanBlend[2] },
    { "shadowmask_4x4 (sun shadow filter), soft, slope clamped to 5 texels per texel (shadow_bias_fix=2)", 5076,
      { 0x9c, 0x9d, 0x69, 0x68, 0x22, 0x36, 0x03, 0x27, 0x5f, 0x2f, 0x2e, 0xee, 0xf7, 0xc7, 0x92, 0x83 },
      kShadowMaskTan5, sizeof kShadowMaskTan5, &g_x.shadowBiasTan[0] },
    { "shadowmask_4x4 (sun shadow filter), soft, slope clamped to 8 texels per texel (shadow_bias_fix=2)", 5076,
      { 0x9c, 0x9d, 0x69, 0x68, 0x22, 0x36, 0x03, 0x27, 0x5f, 0x2f, 0x2e, 0xee, 0xf7, 0xc7, 0x92, 0x83 },
      kShadowMaskTan8, sizeof kShadowMaskTan8, &g_x.shadowBiasTan[1] },
    { "shadowmask_4x4 (sun shadow filter), soft, slope clamped to 12 texels per texel (shadow_bias_fix=2)", 5076,
      { 0x9c, 0x9d, 0x69, 0x68, 0x22, 0x36, 0x03, 0x27, 0x5f, 0x2f, 0x2e, 0xee, 0xf7, 0xc7, 0x92, 0x83 },
      kShadowMaskTan12, sizeof kShadowMaskTan12, &g_x.shadowBiasTan[2] },
    { "shadowmask_4x4 (sun shadow filter), soft, grazing-angle guard, slope cap 0.25 (shadow_bias_fix)", 5076,
      { 0x9c, 0x9d, 0x69, 0x68, 0x22, 0x36, 0x03, 0x27, 0x5f, 0x2f, 0x2e, 0xee, 0xf7, 0xc7, 0x92, 0x83 },
      kShadowMaskFix25, sizeof kShadowMaskFix25, &g_x.shadowBiasFix[0] },
    { "shadowmask_4x4 (sun shadow filter), soft, grazing-angle guard, slope cap 0.1 (shadow_bias_fix)", 5076,
      { 0x9c, 0x9d, 0x69, 0x68, 0x22, 0x36, 0x03, 0x27, 0x5f, 0x2f, 0x2e, 0xee, 0xf7, 0xc7, 0x92, 0x83 },
      kShadowMaskFix10, sizeof kShadowMaskFix10, &g_x.shadowBiasFix[1] },
    { "shadowmask_4x4 (sun shadow filter), soft, grazing-angle guard, slope cap 0.05 (shadow_bias_fix)", 5076,
      { 0x9c, 0x9d, 0x69, 0x68, 0x22, 0x36, 0x03, 0x27, 0x5f, 0x2f, 0x2e, 0xee, 0xf7, 0xc7, 0x92, 0x83 },
      kShadowMaskFix05, sizeof kShadowMaskFix05, &g_x.shadowBiasFix[2] },
    { "shadowmask_4x4 (sun shadow filter), soft, grazing-angle guard, slope cap 0.02 (shadow_bias_fix)", 5076,
      { 0x9c, 0x9d, 0x69, 0x68, 0x22, 0x36, 0x03, 0x27, 0x5f, 0x2f, 0x2e, 0xee, 0xf7, 0xc7, 0x92, 0x83 },
      kShadowMaskFix02, sizeof kShadowMaskFix02, &g_x.shadowBiasFix[3] },
    { "shadowmask_4x4 (sun shadow filter)", 5076,
      { 0x9c, 0x9d, 0x69, 0x68, 0x22, 0x36, 0x03, 0x27, 0x5f, 0x2f, 0x2e, 0xee, 0xf7, 0xc7, 0x92, 0x83 },
      kShadowMaskSoft, sizeof kShadowMaskSoft, &g_x.softShadows },
    { "shadowmask_4x4_nodb (sun shadow filter, no depth buffer)", 4788,
      { 0x5e, 0xb9, 0x86, 0xee, 0x41, 0xc2, 0xdc, 0xe0, 0x49, 0xa8, 0x19, 0xb7, 0x9c, 0xda, 0xc6, 0x1e },
      kShadowMaskSoftNoDb, sizeof kShadowMaskSoftNoDb, &g_x.softShadows },
    // ao_off first: the first matching entry wins, so it takes priority over better_ao.
    { "ssao (ambient occlusion) with no occlusion (ao_off test)", 3868,
      { 0x22, 0xbc, 0xf5, 0x20, 0x17, 0xc6, 0x13, 0x09, 0x74, 0xf8, 0xe8, 0xa5, 0x35, 0x15, 0x6b, 0xc4 },
      kSsaoOff, sizeof kSsaoOff, &g_x.aoOff },
    { "ssao (ambient occlusion)", 3868,
      { 0x22, 0xbc, 0xf5, 0x20, 0x17, 0xc6, 0x13, 0x09, 0x74, 0xf8, 0xe8, 0xa5, 0x35, 0x15, 0x6b, 0xc4 },
      kSsaoHq, sizeof kSsaoHq, &g_x.betterAo },
    { "shadowmaskchar (character blob shadow)", 3532,
      { 0x35, 0xc9, 0xd5, 0x6b, 0xe3, 0xd3, 0x01, 0xd0, 0x34, 0xa4, 0xa2, 0x22, 0x5b, 0x06, 0xb4, 0x30 },
      kShadowMaskCharSoft, sizeof kShadowMaskCharSoft, &g_x.softCharShadow },
};

// If bc is the game shader of an enabled swap, point it at the replacement and return
// that entry (the first match in g_swaps); else NULL and bc is untouched.
static ShaderSwap* SwapShader(D3D12_SHADER_BYTECODE* bc) {
    if (!bc->pShaderBytecode) return NULL;
    for (ShaderSwap& s : g_swaps) {
        if (!*s.enabled || bc->BytecodeLength != s.size) continue;
        __try {
            if (memcmp((const BYTE*)bc->pShaderBytecode + 4, s.digest, 16) != 0) continue;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return NULL;
        }
        bc->pShaderBytecode = s.with;
        bc->BytecodeLength = s.withSize;
        return &s;
    }
    return NULL;
}

// A pipeline was made with swap s: count it, and log the first.
static void NoteSwap(ShaderSwap* s, const char* api) {
    if (InterlockedIncrement(&s->count) == 1) Log("graphics: replaced %s (%s)", s->what, api);
}

// shadow_flash_probe: the pipelines that draw the sun shadow mask (every swap whose
// stock shader is shadowmask_4x4 or its _nodb twin), by identity.
static ID3D12PipelineState* g_maskPsos[32];
static volatile LONG g_maskPsoN;
static void NoteMaskPso(const ShaderSwap* s, void** out) {
    if (!g_x.shadowFlashProbe || !out || !*out || strncmp(s->what, "shadowmask_4x4", 14) != 0) return;
    ID3D12PipelineState* pso = NULL;
    if (FAILED(((IUnknown*)*out)->QueryInterface(__uuidof(ID3D12PipelineState), (void**)&pso)) || !pso) return;
    LONG n = g_maskPsoN;
    if (n < (LONG)_countof(g_maskPsos)) { g_maskPsos[n] = pso; InterlockedIncrement(&g_maskPsoN); }
    pso->Release();                                      // identity only; the game keeps it alive
}
// Is pso one of the sun shadow mask pipelines noted above?
static bool IsMaskPso(ID3D12PipelineState* pso) {
    LONG n = g_maskPsoN;
    for (LONG i = 0; pso && i < n && i < (LONG)_countof(g_maskPsos); i++) if (g_maskPsos[i] == pso) return true;
    return false;
}

// ------------------------------------------------------------------ shadow casters
//
// Stock bug (re\STOCK_BUGS.md, "Lead (2026-10-25)"): each sun-shadow cascade's light
// box is only the light-space bounds of the camera's view slice (FUN_1401d6d10 ->
// FUN_1401d5110 -> FUN_1401dabe0); nothing extends it toward the sun. A caster
// between the sun and the slice (a rock overhang above the view, the window frame
// over the board-game board) lies in front of the cascade's near plane and is
// depth-clipped, so its shadow vanishes or is cut along a straight line that slides
// with the camera. shadow_pancake=1 turns depth clipping off for draws into the
// shadow map only ("shadow pancaking"): such casters clamp to depth 0 and still
// cast. It is decided per draw, not per pipeline: a material's one "Depth"
// technique serves both the main view's depth pre-pass and the cascades, and no
// depth-only pipeline has a rasterizer depth bias (the step-1 log; decomp,
// re\STOCK_BUGS.md "Step-1 log result"). The pre-pass must keep clipping: there,
// geometry crossing the camera's near plane would write depth 0 and the colour
// pass, which still clips, would show holes. See "shadow pancaking, per draw"
// below. shadow_pso_log=1 logs each depth-only pipeline and depth view once.
static SRWLOCK  g_casterLock = SRWLOCK_INIT;
static uint64_t g_casterSeen[512];
static int      g_casterSeenN;

// True the first time key is seen (up to 512 keys; after that, never), so each kind of
// pipeline or depth view is logged once.
static bool CasterFirstSeen(uint64_t key) {
    bool fresh = true;
    AcquireSRWLockExclusive(&g_casterLock);
    for (int i = 0; i < g_casterSeenN; i++) if (g_casterSeen[i] == key) { fresh = false; break; }
    if (fresh && g_casterSeenN < _countof(g_casterSeen)) g_casterSeen[g_casterSeenN++] = key;
    else if (fresh) fresh = false;                       // (table full: stop logging)
    ReleaseSRWLockExclusive(&g_casterLock);
    return fresh;
}

// A depth-only pipeline being created: log it once (shadow_pso_log). r may be NULL
// (a stream without a rasterizer subobject: the defaults).
static void ShadowPsoLog(DXGI_FORMAT dsv, const D3D12_RASTERIZER_DESC* r, const D3D12_SHADER_BYTECODE* vs, bool ps, const char* api) {
    {
        // The "kind" is a hash of what the log line shows: VS identity, formats, bias,
        // culling and clipping.
        BYTE dig[8] = {};
        size_t vsLen = vs && vs->pShaderBytecode ? vs->BytecodeLength : 0;
        __try { if (vsLen >= 20) memcpy(dig, (const BYTE*)vs->pShaderBytecode + 4, 8); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        uint64_t key;
        memcpy(&key, dig, 8);
        key ^= (uint64_t)vsLen * 0x9E3779B97F4A7C15ull ^ (uint64_t)dsv << 52 ^ (ps ? 1ull << 51 : 0);
        if (r) {
            uint32_t slope;
            memcpy(&slope, &r->SlopeScaledDepthBias, 4);
            key ^= (uint64_t)(uint32_t)r->DepthBias << 20 ^ (uint64_t)slope * 0xC2B2AE3D27D4EB4Full ^
                   (uint64_t)r->CullMode << 4 ^ (uint64_t)r->DepthClipEnable;
        }
        if (CasterFirstSeen(key)) {
            char hex[17];
            for (int i = 0; i < 8; i++) sprintf_s(hex + i * 2, 3, "%02x", dig[i]);
            if (r)
                Log("graphics: depth-only pipeline (%s): DSV format %d, depth bias %d, slope bias %.3f, bias clamp %.3f, "
                    "cull %d, depth clip %d, VS %zu bytes %s, %s", api, (int)dsv, r->DepthBias, r->SlopeScaledDepthBias,
                    r->DepthBiasClamp, (int)r->CullMode, (int)r->DepthClipEnable, vsLen, hex, ps ? "with PS" : "no PS");
            else
                Log("graphics: depth-only pipeline (%s): DSV format %d, default rasterizer, VS %zu bytes %s, %s",
                    api, (int)dsv, vsLen, hex, ps ? "with PS" : "no PS");
        }
    }
}

static void AttachPancakeTwin(ID3D12Device* dev, void* pso, D3D12_GRAPHICS_PIPELINE_STATE_DESC c);   // (per draw, below)

// shadow_draw_audit=1: what each pipeline was made with, by pipeline pointer, so the
// audit can say what drew into the shadow map. (A freed pipeline's slot can be reused
// by a new one at the same address; the newer entry wins.)
struct PsoInfo { void* pso; uint32_t vsLen; BYTE vsDigest[8]; uint8_t numRt, depthClip, cull; bool stream; };
static PsoInfo g_psoInfo[16384];
static SRWLOCK g_psoInfoLock = SRWLOCK_INIT;

// Record a new pipeline in g_psoInfo (a hash table keyed by the pipeline pointer, linear
// probing). r NULL: the default rasterizer (depth clip on, back faces culled).
static void PsoInfoNote(void* obj, const D3D12_SHADER_BYTECODE* vs, UINT numRt, const D3D12_RASTERIZER_DESC* r, bool stream) {
    ID3D12PipelineState* pso = NULL;
    if (FAILED(((IUnknown*)obj)->QueryInterface(__uuidof(ID3D12PipelineState), (void**)&pso)) || !pso) return;
    PsoInfo e = { pso };
    e.vsLen = vs && vs->pShaderBytecode ? (uint32_t)vs->BytecodeLength : 0;
    __try { if (e.vsLen >= 20) memcpy(e.vsDigest, (const BYTE*)vs->pShaderBytecode + 4, 8); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    e.numRt = (uint8_t)numRt;
    e.depthClip = r ? (uint8_t)r->DepthClipEnable : 1;
    e.cull = r ? (uint8_t)r->CullMode : (uint8_t)D3D12_CULL_MODE_BACK;
    e.stream = stream;
    AcquireSRWLockExclusive(&g_psoInfoLock);
    size_t h = ((uintptr_t)pso >> 6) % _countof(g_psoInfo);
    for (size_t i = 0; i < _countof(g_psoInfo); i++, h = (h + 1) % _countof(g_psoInfo))
        if (g_psoInfo[h].pso == pso || !g_psoInfo[h].pso) { g_psoInfo[h] = e; break; }
    ReleaseSRWLockExclusive(&g_psoInfoLock);
    pso->Release();
}

// What PsoInfoNote recorded for pso; false if the audit never saw it made.
static bool PsoInfoFor(void* pso, PsoInfo* out) {
    bool found = false;
    AcquireSRWLockShared(&g_psoInfoLock);
    size_t h = ((uintptr_t)pso >> 6) % _countof(g_psoInfo);
    for (size_t i = 0; i < _countof(g_psoInfo); i++, h = (h + 1) % _countof(g_psoInfo)) {
        if (g_psoInfo[h].pso == pso) { *out = g_psoInfo[h]; found = true; break; }
        if (!g_psoInfo[h].pso) break;
    }
    ReleaseSRWLockShared(&g_psoInfoLock);
    return found;
}
static void NoteCopyPso(void* obj, const D3D12_SHADER_BYTECODE& ps);                                  // (smaa, below)

typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateGfxPso)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
static PFN_CreateGfxPso g_createGfxPso;

// ID3D12Device::CreateGraphicsPipelineState (slot 10), how the game makes its pipelines
// (a pipeline state object, PSO: shaders plus fixed-function state, made once and bound
// per draw). In order: the shader swaps, then for the pipeline the game got: a no-clip
// twin if it's depth-only, the SMAA copy-pipeline check, the audit's record.
static HRESULT STDMETHODCALLTYPE CreateGfxPsoHook(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, REFIID riid, void** out) {
    if (!d) return g_createGfxPso(dev, d, riid, out);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC c = *d;
    bool depthOnly = c.NumRenderTargets == 0;            // no colour targets: shadow map or depth pre-pass
    if (depthOnly && g_x.shadowPsoLog)
        ShadowPsoLog(c.DSVFormat, &c.RasterizerState, &c.VS, c.PS.pShaderBytecode && c.PS.BytecodeLength, "CreateGraphicsPipelineState");
    // A rejected replacement falls back to the game's own shader.
    HRESULT hr;
    if (ShaderSwap* s = SwapShader(&c.PS)) {
        hr = g_createGfxPso(dev, &c, riid, out);
        if (SUCCEEDED(hr)) {
            NoteSwap(s, "CreateGraphicsPipelineState");
            NoteMaskPso(s, out);
        } else {
            Log("graphics: replacement for %s rejected (0x%08lx); using the game's", s->what, (unsigned long)hr);
            c = *d;
            hr = g_createGfxPso(dev, d, riid, out);
        }
    } else {
        hr = g_createGfxPso(dev, d, riid, out);
    }
    if (SUCCEEDED(hr) && depthOnly && (g_x.shadowPancake || g_x.shadowNoClampVs || g_x.shadowCullNone) && out && *out) AttachPancakeTwin(dev, *out, c);
    if (SUCCEEDED(hr) && g_x.smaa && out && *out) NoteCopyPso(*out, d->PS);
    if (SUCCEEDED(hr) && g_x.shadowDrawAudit && out && *out) PsoInfoNote(*out, &d->VS, d->NumRenderTargets, &d->RasterizerState, false);
    return hr;
}

// ID3D12Device2::CreatePipelineState takes a stream of subobjects, each starting
// with its type on a pointer-aligned boundary. For the shader swaps, look for a PS
// entry (type 2) whose bytecode is the shader.
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreatePso)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
static PFN_CreatePso g_createPso;

// The shadow-caster check needs the whole stream (rasterizer, formats, VS), so it
// walks it subobject by subobject. Each one is laid out like d3dx12's
// CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT: alignas(void*) { type; inner }.
template <class T> struct alignas(void*) PsoSub { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type; T inner; };
#define PSO_SUB(T) { *innerOff = offsetof(PsoSub<T>, inner); return sizeof(PsoSub<T>); }
// A subobject's size in the stream and (*innerOff) where its payload starts; 0 for a
// type this doesn't know.
static size_t PsoSubSize(UINT type, size_t* innerOff) {
    switch (type) {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: PSO_SUB(ID3D12RootSignature*)
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: PSO_SUB(D3D12_SHADER_BYTECODE)
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: PSO_SUB(D3D12_STREAM_OUTPUT_DESC)
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: PSO_SUB(D3D12_BLEND_DESC)
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: PSO_SUB(UINT)
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: PSO_SUB(D3D12_RASTERIZER_DESC)
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: PSO_SUB(D3D12_DEPTH_STENCIL_DESC)
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: PSO_SUB(D3D12_INPUT_LAYOUT_DESC)
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: PSO_SUB(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE)
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: PSO_SUB(D3D12_PRIMITIVE_TOPOLOGY_TYPE)
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: PSO_SUB(D3D12_RT_FORMAT_ARRAY)
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: PSO_SUB(DXGI_FORMAT)
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: PSO_SUB(DXGI_SAMPLE_DESC)
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: PSO_SUB(UINT)
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: PSO_SUB(D3D12_CACHED_PIPELINE_STATE)
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: PSO_SUB(D3D12_PIPELINE_STATE_FLAGS)
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: PSO_SUB(D3D12_DEPTH_STENCIL_DESC1)
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: PSO_SUB(D3D12_VIEW_INSTANCING_DESC)
    }
    return 0;
}
#undef PSO_SUB

// Logs a depth-only stream pipeline (shadow_pso_log). The game made none in the
// step-1 log, so streams get no pancake twin. A subobject this doesn't know ends the walk.
static void ShadowPsoLogStream(BYTE* s, SIZE_T n) {
    D3D12_RASTERIZER_DESC* rast = NULL;
    const D3D12_SHADER_BYTECODE* vs = NULL;
    DXGI_FORMAT dsv = DXGI_FORMAT_UNKNOWN;
    UINT rts = 0;
    bool ps = false;
    for (SIZE_T o = 0; o < n;) {
        if (o + sizeof(UINT) > n) return;
        UINT type = *(UINT*)(s + o);
        size_t inner = 0, size = PsoSubSize(type, &inner);
        if (!size || o + size > n) {
            static LONG warned;
            if (!InterlockedExchange(&warned, 1))
                Log("graphics: shadow_pso_log: a pipeline stream has a subobject of type %u it doesn't know; not logged", type);
            return;
        }
        BYTE* p = s + o + inner;
        switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: rast = (D3D12_RASTERIZER_DESC*)p; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: dsv = *(DXGI_FORMAT*)p; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: rts = ((D3D12_RT_FORMAT_ARRAY*)p)->NumRenderTargets; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: vs = (const D3D12_SHADER_BYTECODE*)p; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: ps = ((const D3D12_SHADER_BYTECODE*)p)->BytecodeLength != 0; break;
        }
        o += size;
    }
    if (rts == 0) {
        static LONG noted;
        if (g_x.shadowPancake && !InterlockedExchange(&noted, 1))
            Log("graphics: shadow pancaking: the game made a depth-only pipeline through CreatePipelineState; it isn't pancaked");
        ShadowPsoLog(dsv, rast, vs, ps, "CreatePipelineState");
    }
}

static HRESULT CreatePsoHookInner(ID3D12Device2* dev, const D3D12_PIPELINE_STATE_STREAM_DESC* d, REFIID riid, void** out);

// The audit's view of a stream pipeline: its VS, render-target count and rasterizer.
static void PsoInfoNoteStream(void* obj, BYTE* s, SIZE_T n) {
    const D3D12_RASTERIZER_DESC* rast = NULL;
    const D3D12_SHADER_BYTECODE* vs = NULL;
    UINT rts = 0;
    for (SIZE_T o = 0; o + sizeof(UINT) <= n;) {
        UINT type = *(UINT*)(s + o);
        size_t inner = 0, size = PsoSubSize(type, &inner);
        if (!size || o + size > n) break;
        BYTE* p = s + o + inner;
        if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER) rast = (const D3D12_RASTERIZER_DESC*)p;
        else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS) vs = (const D3D12_SHADER_BYTECODE*)p;
        else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS) rts = ((D3D12_RT_FORMAT_ARRAY*)p)->NumRenderTargets;
        o += size;
    }
    PsoInfoNote(obj, vs, rts, rast, true);
}

// ID3D12Device2::CreatePipelineState (slot 47). The work is in CreatePsoHookInner;
// this adds the audit's record of the pipeline made.
static HRESULT STDMETHODCALLTYPE CreatePsoHook(ID3D12Device2* dev, const D3D12_PIPELINE_STATE_STREAM_DESC* d, REFIID riid, void** out) {
    HRESULT hr = CreatePsoHookInner(dev, d, riid, out);
    if (SUCCEEDED(hr) && g_x.shadowDrawAudit && out && *out && d && d->pPipelineStateSubobjectStream && d->SizeInBytes <= 4096) {
        alignas(8) BYTE copy[4096];
        memcpy(copy, d->pPipelineStateSubobjectStream, d->SizeInBytes);
        PsoInfoNoteStream(*out, copy, d->SizeInBytes);
    }
    return hr;
}

// The shader swaps for a stream pipeline. The stream is copied (the game passes it as
// const) and the PS entry changed in the copy. The PS is found by stepping 8 bytes at a
// time (each subobject starts pointer-aligned) to a type-2 tag, its D3D12_SHADER_BYTECODE
// 8 bytes after it. Streams over 4 KB go through untouched.
static HRESULT CreatePsoHookInner(ID3D12Device2* dev, const D3D12_PIPELINE_STATE_STREAM_DESC* d, REFIID riid, void** out) {
    if (d && d->pPipelineStateSubobjectStream && d->SizeInBytes >= 24 && d->SizeInBytes <= 4096) {
        alignas(8) BYTE copy[4096];
        memcpy(copy, d->pPipelineStateSubobjectStream, d->SizeInBytes);
        if (g_x.shadowPsoLog || g_x.shadowPancake) ShadowPsoLogStream(copy, d->SizeInBytes);
        ShaderSwap* s = NULL;
        for (SIZE_T o = 0; o + 8 + sizeof(D3D12_SHADER_BYTECODE) <= d->SizeInBytes; o += 8) {
            if (*(UINT*)(copy + o) != D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS) continue;
            if ((s = SwapShader((D3D12_SHADER_BYTECODE*)(copy + o + 8))) != NULL) break;
        }
        if (s) {
            D3D12_PIPELINE_STATE_STREAM_DESC c = { d->SizeInBytes, copy };
            HRESULT hr = g_createPso(dev, &c, riid, out);
            if (SUCCEEDED(hr)) { NoteSwap(s, "CreatePipelineState"); NoteMaskPso(s, out); return hr; }
            Log("graphics: replacement for %s rejected (0x%08lx); using the game's", s->what, (unsigned long)hr);
        }
    }
    return g_createPso(dev, d, riid, out);
}

// ------------------------------------------------------------------ shadow pancaking, per draw
//
// shadow_pancake=1. Every depth-only pipeline gets a twin with DepthClipEnable off,
// attached to it as private data, so it lives and dies with the original (no stale
// pointer if the game frees a pipeline). Each depth-stencil view is noted when it's
// created: is it the shadow atlas (shadowBuffer: 640 x 1920 times shadow_res,
// nothing else has that shape)? Command lists: while the atlas is the bound depth
// target, a pipeline with a twin is swapped for its twin, in SetPipelineState, and
// in OMSetRenderTargets for a pipeline set before the target. So only draws into
// the shadow map lose depth clipping; the main view's pre-pass, made with the same
// pipelines, keeps it.
static const GUID kTwinPtr = { 0x6b1f3c2a, 0x9d47, 0x4e61, { 0x8a, 0x13, 0x52, 0x7c, 0x0e, 0x94, 0xd2, 0x31 } };   // raw pointer
static const GUID kTwinRef = { 0x6b1f3c2b, 0x9d47, 0x4e61, { 0x8a, 0x13, 0x52, 0x7c, 0x0e, 0x94, 0xd2, 0x31 } };   // keeps it alive
static LONG g_twins, g_twinBinds;

// shadow_noclamp_vs=1: the twins of the static caster pipelines also get a vertex shader
// without the per-vertex FMax(z, 0) (shadowdepth_noclamp.hlsl; see its header). Matched by
// size and the first 8 bytes of the DXIL hash, as logged by shadow_pso_log.
struct NoClampVs { SIZE_T size; BYTE digest[8]; const unsigned char* with; SIZE_T withSize; const char* what; };
static const NoClampVs kNoClampVs[] = {
    { 2652, { 0x8a, 0x7e, 0x6d, 0x1b, 0x76, 0x8c, 0x0f, 0x25 }, kShadowDepthNoClamp, sizeof kShadowDepthNoClamp, "shadowdepth_" },
    { 2900, { 0x43, 0x82, 0x98, 0xd6, 0xa7, 0x4b, 0x93, 0x83 }, kShadowDepthNoClampAk, sizeof kShadowDepthNoClampAk, "shadowdepth_ak_" },
    // The material "Depth" technique (*_skin.tfx, nearly every world caster: rocks, cliffs,
    // buildings): depth_skin_noclamp.hlsl, one define set per original.
    { 3940, { 0xa4, 0x6a, 0x66, 0xfa, 0x5d, 0x20, 0x2e, 0xd7 }, kDskA46a, sizeof kDskA46a, "material depth (a46a66fa)" },
    { 3944, { 0x4c, 0x70, 0x71, 0x52, 0x7e, 0xc1, 0xa4, 0xf4 }, kDsk4c70, sizeof kDsk4c70, "material depth (4c707152)" },
    { 4036, { 0xe6, 0xf8, 0xad, 0x75, 0xc9, 0xc7, 0x28, 0x7e }, kDskE6f8, sizeof kDskE6f8, "material depth (e6f8ad75)" },
    { 4128, { 0x1f, 0x78, 0xd0, 0x30, 0x77, 0x32, 0xa9, 0xa6 }, kDsk1f78, sizeof kDsk1f78, "material depth (1f78d030)" },
    { 4136, { 0xa6, 0xaf, 0x0f, 0x02, 0x77, 0xec, 0x97, 0x0a }, kDskA6af, sizeof kDskA6af, "material depth (a6af0f02)" },
    { 4320, { 0xf6, 0x81, 0x4a, 0x0c, 0xe5, 0xdc, 0x2d, 0x1b }, kDskF681, sizeof kDskF681, "material depth (f6814a0c)" },
    { 4096, { 0x43, 0x7a, 0x63, 0x34, 0x1e, 0xe2, 0xe6, 0x56 }, kDsk437a, sizeof kDsk437a, "material depth, alpha-tested (437a6334)" },
    { 4136, { 0xe0, 0xf2, 0x3b, 0xe1, 0xa3, 0x10, 0x0d, 0x83 }, kDskE0f2, sizeof kDskE0f2, "material depth, scrolling (e0f23be1)" },
    { 4336, { 0x81, 0x55, 0x77, 0x17, 0xd5, 0x70, 0xe5, 0x54 }, kDsk8155, sizeof kDsk8155, "material depth, glass (81557717)" },
};
static LONG g_noClampTwins;

// The no-clamp replacement for caster vertex shader vs, or NULL if it isn't one.
static const NoClampVs* NoClampFor(const D3D12_SHADER_BYTECODE& vs) {
    if (!vs.pShaderBytecode) return NULL;
    for (const NoClampVs& n : kNoClampVs) {
        if (vs.BytecodeLength != n.size) continue;
        __try {
            if (memcmp((const BYTE*)vs.pShaderBytecode + 4, n.digest, 8) == 0) return &n;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return NULL;
        }
    }
    return NULL;
}

// Make the twin of depth-only pipeline obj from its description c (depth clip off, plus
// the no-clamp VS and no culling when those are on), through the real creation function
// so it isn't hooked again, and attach it to obj. From CreateGfxPsoHook.
static void AttachPancakeTwin(ID3D12Device* dev, void* obj, D3D12_GRAPHICS_PIPELINE_STATE_DESC c) {
    const NoClampVs* nc = g_x.shadowNoClampVs ? NoClampFor(c.VS) : NULL;
    // A pipeline the game already made without depth clipping still needs the no-clamp
    // VS (and cull_none); only one with nothing to change is left alone.
    if (!c.RasterizerState.DepthClipEnable && !nc && !g_x.shadowCullNone) return;
    c.RasterizerState.DepthClipEnable = FALSE;
    c.CachedPSO = {};                                    // a cached blob only fits the original
    if (nc) c.VS = { nc->with, nc->withSize };
    // shadow_cull_none=1 (test): most caster pipelines cull FRONT faces, so only faces
    // turned away from the sun cast. Single-sided pieces (cliff and overhang shells)
    // then leak light in triangle-shaped patches; here both faces cast.
    if (g_x.shadowCullNone) c.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    ID3D12PipelineState* pso = NULL;
    ID3D12PipelineState* twin = NULL;
    if (FAILED(((IUnknown*)obj)->QueryInterface(__uuidof(ID3D12PipelineState), (void**)&pso)) || !pso) return;
    HRESULT hr = g_createGfxPso(dev, &c, __uuidof(ID3D12PipelineState), (void**)&twin);
    if (SUCCEEDED(hr) && twin) {
        pso->SetPrivateData(kTwinPtr, sizeof twin, &twin);
        pso->SetPrivateDataInterface(kTwinRef, twin);   // holds the twin's reference now
        twin->Release();
        LONG n = InterlockedIncrement(&g_twins);
        if (n <= 2 || n % 25 == 0) Log("graphics: shadow pancaking: %ld depth-only pipelines have a no-clip twin", n);
        if (nc) {
            LONG k = InterlockedIncrement(&g_noClampTwins);
            if (k <= 4 || k % 10 == 0)
                Log("graphics: shadow_noclamp_vs: %ld shadow-map pipelines use the no-clamp %s vertex shader", k, nc->what);
        }
    } else {
        static LONG warned;
        if (!InterlockedExchange(&warned, 1))
            Log("graphics: shadow pancaking: a no-clip twin%s was rejected (0x%08lx)", nc ? " (no-clamp vertex shader)" : "",
                (unsigned long)hr);
    }
    pso->Release();
}

// pso's twin, or NULL. No reference is added: pso's private data holds one.
static ID3D12PipelineState* TwinOf(ID3D12PipelineState* pso) {
    if (!pso) return NULL;
    ID3D12PipelineState* twin = NULL;
    UINT size = sizeof twin;
    if (FAILED(pso->GetPrivateData(kTwinPtr, &size, &twin)) || size != sizeof twin) return NULL;
    return twin;
}

// A twin was bound for a shadow-map draw: log the first, then the count every 10 s
// (at most 30 lines).
static void NoteTwinBind() {
    LONG n = InterlockedIncrement(&g_twinBinds);
    static ULONGLONG last;
    static LONG lastN, lines;
    ULONGLONG now = GetTickCount64();
    if (n == 1) { last = now; Log("graphics: shadow pancaking: first draw into the shadow map without depth clipping"); return; }
    if (now - last >= 10000 && lines < 30) {
        lines++;
        Log("graphics: shadow pancaking: %ld shadow-map pipeline binds without depth clipping in the last %llu s",
            n - lastN, (now - last) / 1000);
        last = now;
        lastN = n;
    }
}

// Depth-stencil views: descriptor address -> is it the shadow atlas.
// (A hash table, linear probing; a descriptor rewritten with another view updates its slot.)
struct DsvSlot { SIZE_T ptr; bool atlas; };
static DsvSlot g_dsvs[1024];
static SRWLOCK g_dsvLock = SRWLOCK_INIT;

static void DsvNote(SIZE_T ptr, bool atlas) {
    AcquireSRWLockExclusive(&g_dsvLock);
    size_t h = (ptr >> 5) % _countof(g_dsvs);
    for (size_t i = 0; i < _countof(g_dsvs); i++, h = (h + 1) % _countof(g_dsvs)) {
        if (g_dsvs[h].ptr == ptr || !g_dsvs[h].ptr) { g_dsvs[h].ptr = ptr; g_dsvs[h].atlas = atlas; break; }
    }
    ReleaseSRWLockExclusive(&g_dsvLock);
}

static bool DsvIsAtlas(SIZE_T ptr) {
    bool atlas = false;
    AcquireSRWLockShared(&g_dsvLock);
    size_t h = (ptr >> 5) % _countof(g_dsvs);
    for (size_t i = 0; i < _countof(g_dsvs); i++, h = (h + 1) % _countof(g_dsvs)) {
        if (g_dsvs[h].ptr == ptr) { atlas = g_dsvs[h].atlas; break; }
        if (!g_dsvs[h].ptr) break;
    }
    ReleaseSRWLockShared(&g_dsvLock);
    return atlas;
}

typedef void (STDMETHODCALLTYPE* PFN_CreateDsv)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
static PFN_CreateDsv g_createDsv;

// ID3D12Device::CreateDepthStencilView (slot 21): note whether this descriptor now views
// the shadow atlas (recognised by its exact size, g_atlasW x g_atlasH).
static void STDMETHODCALLTYPE CreateDsvHook(ID3D12Device* dev, ID3D12Resource* res, const D3D12_DEPTH_STENCIL_VIEW_DESC* d,
                                             D3D12_CPU_DESCRIPTOR_HANDLE h) {
    g_createDsv(dev, res, d, h);
    bool atlas = false;
    if (res) {
        D3D12_RESOURCE_DESC rd = res->GetDesc();
        atlas = rd.Width == g_atlasW && rd.Height == g_atlasH;
        if (g_x.shadowPsoLog && CasterFirstSeen(0xD5F0000000000000ull ^ rd.Width << 24 ^ (uint64_t)rd.Height << 4 ^ (uint64_t)rd.Format))
            Log("graphics: depth view %llux%u, format %d%s", (unsigned long long)rd.Width, rd.Height, (int)rd.Format,
                atlas ? " (the shadow map: draws into it get pancaked when shadow_pancake=1)" : "");
    }
    DsvNote(h.ptr, atlas);
}

// smaa=1: the state the game set on a command list that our SMAA passes overwrite
// in the middle of its frame, so it can be put back exactly afterwards. A D3D12
// command list has no state beyond what was set through these calls, so re-issuing
// the last values restores it (and the engine's own dirty-bit cache stays right).
// Vertex/index buffers, blend factor and stencil ref aren't touched by our passes.
// shadow_draw_audit (the viewport) and shadow_flash_probe (the root CBVs) read the
// same record.
// Root arguments: the values bound to the root signature's parameter slots (descriptor
// tables, inline constants, buffer addresses); a new root signature clears them.
struct RootArg { uint8_t kind; UINT64 v; uint64_t constMask; uint32_t consts[64]; };   // kind: 0 none, 1 table, 2 consts, 3 CBV, 4 SRV, 5 UAV
enum { ARG_NONE, ARG_TABLE, ARG_CONSTS, ARG_CBV, ARG_SRV, ARG_UAV };
struct ListShadow {
    UINT nHeaps; ID3D12DescriptorHeap* heaps[2];
    ID3D12RootSignature* rootSig;
    UINT maxArg; RootArg args[64];
    bool hasTopo; D3D12_PRIMITIVE_TOPOLOGY topo;
    UINT nVp; D3D12_VIEWPORT vp[16];
    UINT nSc; D3D12_RECT sc[16];
    bool hasOm; UINT nRt; BOOL single; D3D12_CPU_DESCRIPTOR_HANDLE rt[8]; bool hasDsv; D3D12_CPU_DESCRIPTOR_HANDLE dsv;
};

// Per command list (recorded by one thread at a time): the pipeline the game set,
// whether the shadow atlas is the bound depth target, and (smaa) the state record.
struct ListState { void* list; ID3D12PipelineState* cur; bool atlas; bool isCopy; ListShadow* sh; bool smaaRan;
                   D3D12_VERTEX_BUFFER_VIEW vb0; };   // vb0: slot 0's vertex buffer (shadow_flash_probe)
static ListState g_lists[256];
static SRWLOCK   g_listLock = SRWLOCK_INIT;
static bool g_smaaRanAny;                // SMAA has run on a game list at least once
static thread_local bool g_inGfxPass;   // our own SMAA passes / restore: hooks pass straight through
static bool g_listHooksOk;              // the command-list hooks went in

// list's ListState, added on first sight (lookup under the shared lock, insert under
// the exclusive one). NULL when all 256 slots are taken. Slots are never freed; a
// list's Reset clears its state instead.
static ListState* ListFor(void* list) {
    size_t h0 = ((uintptr_t)list >> 6) % _countof(g_lists);
    AcquireSRWLockShared(&g_listLock);
    for (size_t i = 0, h = h0; i < _countof(g_lists); i++, h = (h + 1) % _countof(g_lists)) {
        if (g_lists[h].list == list) { ReleaseSRWLockShared(&g_listLock); return &g_lists[h]; }
        if (!g_lists[h].list) break;
    }
    ReleaseSRWLockShared(&g_listLock);
    ListState* s = NULL;
    AcquireSRWLockExclusive(&g_listLock);
    for (size_t i = 0, h = h0; i < _countof(g_lists); i++, h = (h + 1) % _countof(g_lists)) {
        if (g_lists[h].list == list || !g_lists[h].list) {
            if (!g_lists[h].list) g_lists[h] = { list, NULL, false, false, NULL, false };
            s = &g_lists[h];
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_listLock);
    if (!s) {
        static LONG warned;
        if (!InterlockedExchange(&warned, 1)) Log("graphics: shadow pancaking: too many command lists; the extra ones aren't pancaked");
    }
    return s;
}

typedef void (STDMETHODCALLTYPE* PFN_SetPso)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
typedef void (STDMETHODCALLTYPE* PFN_OMSetRT)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
                                              const D3D12_CPU_DESCRIPTOR_HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_ListReset)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
typedef void (STDMETHODCALLTYPE* PFN_ClearState)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
static PFN_SetPso     g_setPso;
static PFN_OMSetRT    g_omSetRT;
static PFN_ListReset  g_listReset;
static PFN_ClearState g_clearState;

// smaa=1: the game's simpleCopy pipelines (x360DepthRestore; the "Copy Post" that puts
// the finished 3D image on the back buffer before the HUD), noted at creation.
static ID3D12PipelineState* g_copyPsos[32];
static volatile LONG g_copyPsoN;
static bool IsCopyPso(ID3D12PipelineState* pso) {
    if (!pso) return false;
    LONG n = g_copyPsoN;
    for (LONG i = 0; i < n && i < (LONG)_countof(g_copyPsos); i++) if (g_copyPsos[i] == pso) return true;
    return false;
}

// x360DepthRestore technique simpleCopy, PS (DXBC hash 049ccc76..., 2124 bytes): reads t3/s3,
// writes SV_Target0. Its pipelines draw "Copy Post" (and "Capture Scene", to another target).
static void NoteCopyPso(void* obj, const D3D12_SHADER_BYTECODE& ps) {
    static const BYTE kCopyPs[16] = { 0x04, 0x9c, 0xcc, 0x76, 0xfc, 0x2b, 0x70, 0xad, 0x29, 0x56, 0x0b, 0x67, 0x1d, 0x39, 0x40, 0x52 };
    if (!ps.pShaderBytecode || ps.BytecodeLength != 2124) return;
    __try {
        if (memcmp((const BYTE*)ps.pShaderBytecode + 4, kCopyPs, 16) != 0) return;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    ID3D12PipelineState* pso = NULL;
    if (FAILED(((IUnknown*)obj)->QueryInterface(__uuidof(ID3D12PipelineState), (void**)&pso)) || !pso) return;
    LONG n = g_copyPsoN;
    if (n < (LONG)_countof(g_copyPsos)) {
        g_copyPsos[n] = pso;                             // identity only; the game keeps it alive
        InterlockedIncrement(&g_copyPsoN);
        if (n < 4) Log("graphics: smaa: found the game's post-copy pipeline (%ld so far)", n + 1);
    }
    pso->Release();
}

static UINT Cap(UINT n, UINT most) { return n < most ? n : most; }

// The list's state record, allocated on first use (never freed; one per list slot).
static ListShadow* ShadowFor(ListState* s) {
    if (!s->sh) s->sh = (ListShadow*)calloc(1, sizeof(ListShadow));
    return s->sh;
}

// Forget the recorded state: after Reset / ClearState the list has none.
static void ResetShadow(ListState* s) {
    if (s->sh) memset(s->sh, 0, sizeof *s->sh);
}

// SetPipelineState (slot 25): remember the game's pipeline (and whether it's the Copy
// Post one); while the shadow atlas is the depth target, bind its twin instead.
static void STDMETHODCALLTYPE SetPsoHook(ID3D12GraphicsCommandList* l, ID3D12PipelineState* pso) {
    ListState* s = g_inGfxPass ? NULL : ListFor(l);
    if (s) {
        s->cur = pso;
        s->isCopy = g_x.smaa && IsCopyPso(pso);
        if (s->atlas)
            if (ID3D12PipelineState* twin = TwinOf(pso)) { g_setPso(l, twin); NoteTwinBind(); return; }
    }
    g_setPso(l, pso);
}

// OMSetRenderTargets (slot 46): record the targets (smaa), and when the depth target
// changes to or from the shadow atlas, swap the bound pipeline for its twin or back.
static void STDMETHODCALLTYPE OMSetRTHook(ID3D12GraphicsCommandList* l, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts,
                                           BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    g_omSetRT(l, n, rts, single, dsv);
    if (g_inGfxPass) return;
    ListState* s = ListFor(l);
    if (!s) return;
    if (g_x.smaa)
        if (ListShadow* sh = ShadowFor(s)) {
            sh->hasOm = true;
            sh->single = single;
            sh->nRt = Cap(n, 8u);
            UINT copy = single ? (n ? 1u : 0u) : sh->nRt;
            for (UINT i = 0; i < copy; i++) sh->rt[i] = rts[i];
            sh->hasDsv = dsv != NULL;
            if (dsv) sh->dsv = *dsv;
        }
    bool atlas = dsv && DsvIsAtlas(dsv->ptr);
    if (atlas == s->atlas) return;
    s->atlas = atlas;
    if (ID3D12PipelineState* twin = TwinOf(s->cur)) {        // the pipeline was set before the target
        g_setPso(l, atlas ? twin : s->cur);
        if (atlas) NoteTwinBind();
    }
}

// Reset (slot 10) and ClearState (slot 11): the list starts over with only the initial
// pipeline, so everything recorded for it is dropped.
static HRESULT STDMETHODCALLTYPE ListResetHook(ID3D12GraphicsCommandList* l, ID3D12CommandAllocator* a, ID3D12PipelineState* pso) {
    HRESULT hr = g_listReset(l, a, pso);
    if (!g_inGfxPass)
        if (ListState* s = ListFor(l)) { s->cur = pso; s->atlas = false; s->isCopy = g_x.smaa && IsCopyPso(pso); s->smaaRan = false; ResetShadow(s); }
    return hr;
}

static void STDMETHODCALLTYPE ClearStateHook(ID3D12GraphicsCommandList* l, ID3D12PipelineState* pso) {
    g_clearState(l, pso);
    if (!g_inGfxPass)
        if (ListState* s = ListFor(l)) { s->cur = pso; s->atlas = false; s->isCopy = g_x.smaa && IsCopyPso(pso); s->smaaRan = false; ResetShadow(s); }
}

// --- the rest of the smaa=1 state record ---
typedef void (STDMETHODCALLTYPE* PFN_Draw)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
typedef void (STDMETHODCALLTYPE* PFN_Topo)(ID3D12GraphicsCommandList*, D3D12_PRIMITIVE_TOPOLOGY);
typedef void (STDMETHODCALLTYPE* PFN_Vp)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
typedef void (STDMETHODCALLTYPE* PFN_Sc)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*);
typedef void (STDMETHODCALLTYPE* PFN_Heaps)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
typedef void (STDMETHODCALLTYPE* PFN_RootSig)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
typedef void (STDMETHODCALLTYPE* PFN_Table)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
typedef void (STDMETHODCALLTYPE* PFN_Const1)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
typedef void (STDMETHODCALLTYPE* PFN_ConstN)(ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT);
typedef void (STDMETHODCALLTYPE* PFN_View)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
static PFN_Draw    g_drawInst;
static PFN_Topo    g_setTopo;
static PFN_Vp      g_setVp;
static PFN_Sc      g_setSc;
static PFN_Heaps   g_setHeaps;
static PFN_RootSig g_setRootSig;
static PFN_Table   g_setTable;
static PFN_Const1  g_setConst1;
static PFN_ConstN  g_setConstN;
static PFN_View    g_setCbv, g_setSrv, g_setUav;

// Recording is cheap (no lock beyond ListFor's shared one) and skipped inside our passes.
static ListShadow* RecFor(ID3D12GraphicsCommandList* l) {
    if (g_inGfxPass || !(g_x.smaa || g_x.shadowDrawAudit || g_x.shadowFlashProbe)) return NULL;
    ListState* s = ListFor(l);
    return s ? ShadowFor(s) : NULL;
}

// Root argument i's record (NULL past 64); maxArg grows to cover it.
static RootArg* ArgFor(ListShadow* sh, UINT i) {
    if (i >= _countof(sh->args)) return NULL;
    if (i + 1 > sh->maxArg) sh->maxArg = i + 1;
    return &sh->args[i];
}

// The recording hooks: each passes the call on, then notes what the game set.
static void STDMETHODCALLTYPE TopoHook(ID3D12GraphicsCommandList* l, D3D12_PRIMITIVE_TOPOLOGY t) {
    g_setTopo(l, t);
    if (ListShadow* sh = RecFor(l)) { sh->hasTopo = true; sh->topo = t; }
}
static void STDMETHODCALLTYPE VpHook(ID3D12GraphicsCommandList* l, UINT n, const D3D12_VIEWPORT* v) {
    g_setVp(l, n, v);
    if (ListShadow* sh = RecFor(l)) { sh->nVp = Cap(n, 16u); if (v) memcpy(sh->vp, v, sh->nVp * sizeof *v); else sh->nVp = 0; }
}
static void STDMETHODCALLTYPE ScHook(ID3D12GraphicsCommandList* l, UINT n, const D3D12_RECT* r) {
    g_setSc(l, n, r);
    if (ListShadow* sh = RecFor(l)) { sh->nSc = Cap(n, 16u); if (r) memcpy(sh->sc, r, sh->nSc * sizeof *r); else sh->nSc = 0; }
}
static void STDMETHODCALLTYPE HeapsHook(ID3D12GraphicsCommandList* l, UINT n, ID3D12DescriptorHeap* const* h) {
    g_setHeaps(l, n, h);
    if (ListShadow* sh = RecFor(l)) { sh->nHeaps = Cap(n, 2u); for (UINT i = 0; i < sh->nHeaps; i++) sh->heaps[i] = h[i]; }
}
static void STDMETHODCALLTYPE RootSigHook(ID3D12GraphicsCommandList* l, ID3D12RootSignature* rs) {
    g_setRootSig(l, rs);
    if (ListShadow* sh = RecFor(l)) {
        if (sh->rootSig != rs) { memset(sh->args, 0, sh->maxArg * sizeof(RootArg)); sh->maxArg = 0; }   // the arguments die with it
        sh->rootSig = rs;
    }
}
static void STDMETHODCALLTYPE TableHook(ID3D12GraphicsCommandList* l, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    g_setTable(l, i, h);
    if (ListShadow* sh = RecFor(l)) if (RootArg* a = ArgFor(sh, i)) { a->kind = ARG_TABLE; a->v = h.ptr; }
}
static void STDMETHODCALLTYPE Const1Hook(ID3D12GraphicsCommandList* l, UINT i, UINT v, UINT off) {
    g_setConst1(l, i, v, off);
    if (ListShadow* sh = RecFor(l)) if (RootArg* a = ArgFor(sh, i)) if (off < 64) {
        if (a->kind != ARG_CONSTS) { a->kind = ARG_CONSTS; a->constMask = 0; }
        a->consts[off] = v;
        a->constMask |= 1ull << off;
    }
}
static void STDMETHODCALLTYPE ConstNHook(ID3D12GraphicsCommandList* l, UINT i, UINT n, const void* d, UINT off) {
    g_setConstN(l, i, n, d, off);
    if (ListShadow* sh = RecFor(l)) if (RootArg* a = ArgFor(sh, i)) {
        if (a->kind != ARG_CONSTS) { a->kind = ARG_CONSTS; a->constMask = 0; }
        for (UINT k = 0; k < n && off + k < 64; k++) {
            a->consts[off + k] = ((const uint32_t*)d)[k];
            a->constMask |= 1ull << (off + k);
        }
    }
}
static void STDMETHODCALLTYPE CbvHook(ID3D12GraphicsCommandList* l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS v) {
    g_setCbv(l, i, v);
    if (ListShadow* sh = RecFor(l)) if (RootArg* a = ArgFor(sh, i)) { a->kind = ARG_CBV; a->v = v; }
}
static void STDMETHODCALLTYPE SrvHook(ID3D12GraphicsCommandList* l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS v) {
    g_setSrv(l, i, v);
    if (ListShadow* sh = RecFor(l)) if (RootArg* a = ArgFor(sh, i)) { a->kind = ARG_SRV; a->v = v; }
}
static void STDMETHODCALLTYPE UavHook(ID3D12GraphicsCommandList* l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS v) {
    g_setUav(l, i, v);
    if (ListShadow* sh = RecFor(l)) if (RootArg* a = ArgFor(sh, i)) { a->kind = ARG_UAV; a->v = v; }
}

// Re-issue everything our passes changed, through the original functions.
static void RestoreListState(ID3D12GraphicsCommandList* l, const ListState* s) {
    const ListShadow* sh = s->sh;
    if (!sh) return;
    if (sh->nHeaps) g_setHeaps(l, sh->nHeaps, sh->heaps);
    if (sh->rootSig) {
        g_setRootSig(l, sh->rootSig);
        for (UINT i = 0; i < sh->maxArg; i++) {
            const RootArg& a = sh->args[i];
            switch (a.kind) {
            case ARG_TABLE: g_setTable(l, i, D3D12_GPU_DESCRIPTOR_HANDLE{ a.v }); break;
            case ARG_CBV: g_setCbv(l, i, a.v); break;
            case ARG_SRV: g_setSrv(l, i, a.v); break;
            case ARG_UAV: g_setUav(l, i, a.v); break;
            case ARG_CONSTS:
                for (UINT k = 0; k < 64;) {                         // contiguous runs of set constants
                    if (!(a.constMask >> k & 1)) { k++; continue; }
                    UINT e = k;
                    while (e < 64 && (a.constMask >> e & 1)) e++;
                    g_setConstN(l, i, e - k, &a.consts[k], k);
                    k = e;
                }
                break;
            }
        }
    }
    if (s->cur) g_setPso(l, s->cur);
    if (sh->hasTopo) g_setTopo(l, sh->topo);
    if (sh->nVp) g_setVp(l, sh->nVp, sh->vp);
    if (sh->nSc) g_setSc(l, sh->nSc, sh->sc);
    if (sh->hasOm) g_omSetRT(l, sh->nRt, sh->nRt ? sh->rt : NULL, sh->single, sh->hasDsv ? &sh->dsv : NULL);
}

static void SmaaAfterCopy(ID3D12GraphicsCommandList* l, ListState* s);   // (SMAA section, below)

// --- shadow_draw_audit=1: per draw into the shadow map, by cascade (the atlas's tiles are
// stacked top to bottom, near first, each as tall as the atlas is wide): which game
// pipeline drew, and whether it had its twin at the time.
struct AuditSlot { ID3D12PipelineState* pso; uint32_t draws[4]; bool twin; };   // draws: near, middle, far, other
static AuditSlot g_audit[1024];
static SRWLOCK   g_auditLock = SRWLOCK_INIT;
static uint32_t  g_auditDraws[4], g_auditLost;
static LONG      g_auditBundles;
static D3D12_VIEWPORT g_auditVp0;
static bool      g_auditVp0Set;

// One draw into the shadow map: counted against its cascade (the tile its viewport's
// top edge is in; tiles are atlasW tall) and against the pipeline that drew it.
static void AuditDraw(ListState* s) {
    const ListShadow* sh = s->sh;
    int tile = 3;
    if (sh && sh->nVp && g_atlasW) {
        float t = sh->vp[0].TopLeftY / (float)g_atlasW;
        tile = t >= 0.0f && t < 3.0f ? (int)t : 3;
    }
    bool twin = TwinOf(s->cur) != NULL;
    AcquireSRWLockExclusive(&g_auditLock);
    if (tile == 0 && !g_auditVp0Set) { g_auditVp0 = sh->vp[0]; g_auditVp0Set = true; }
    g_auditDraws[tile]++;
    size_t h = ((uintptr_t)s->cur >> 6) % _countof(g_audit), i = 0;
    for (; i < _countof(g_audit); i++, h = (h + 1) % _countof(g_audit))
        if (g_audit[h].pso == s->cur || !g_audit[h].pso) {
            g_audit[h].pso = s->cur;
            g_audit[h].draws[tile]++;
            g_audit[h].twin = twin;
            break;
        }
    if (i == _countof(g_audit)) g_auditLost++;
    ReleaseSRWLockExclusive(&g_auditLock);
}

typedef void (STDMETHODCALLTYPE* PFN_ExecBundle)(ID3D12GraphicsCommandList*, ID3D12GraphicsCommandList*);
static PFN_ExecBundle g_execBundle;

// A bundle's draws are recorded on the bundle, which never sees the shadow map bound,
// so they'd get no twin: count any run while it is.
static void STDMETHODCALLTYPE ExecBundleHook(ID3D12GraphicsCommandList* l, ID3D12GraphicsCommandList* b) {
    g_execBundle(l, b);
    if (g_inGfxPass || !g_x.shadowDrawAudit) return;
    ListState* s = ListFor(l);
    if (s && s->atlas) InterlockedIncrement(&g_auditBundles);
}

// --- shadow_flash_probe=1 (re\SUN_FLASH.md). On the CPU every input of the sun shadow mask
// comes from one per-frame view, so the one-frame full-sun flash on camera cuts is left to
// data the GPU reads after the CPU has moved on: the mask quad's per-vertex rays (a vertex
// buffer; its VS is a pass-through) and the PerView constants. If either buffer is reused
// across frames instead of versioned, a late GPU frame reads the next frame's camera
// against its own depth, and on a cut every pixel falls outside the cascades: lit.
// At each mask draw the probe copies the bound vertex buffer and constant buffers (from
// the CPU pointers the game mapped them at), then compares them with memory again at
// ExecuteCommandLists and at the next Present. A change means the buffer was rewritten
// while the GPU may still read it. Also counted: mask draws whose buffers sit at the same
// GPU address as the previous frame's.
typedef HRESULT (STDMETHODCALLTYPE* PFN_Map)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**);
typedef void (STDMETHODCALLTYPE* PFN_SetVbs)(ID3D12GraphicsCommandList*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*);
static PFN_Map    g_map;
static PFN_SetVbs g_setVbs;
struct MapRange { UINT64 va, size; BYTE* cpu; };   // a mapped buffer: GPU address range -> CPU pointer
static MapRange g_maps[8192];
static int      g_mapN;
static SRWLOCK  g_mapLock = SRWLOCK_INIT;

// ID3D12Resource::Map (slot 8): note each buffer's GPU range and CPU pointer.
static HRESULT STDMETHODCALLTYPE MapHook(ID3D12Resource* r, UINT sub, const D3D12_RANGE* range, void** data) {
    HRESULT hr = g_map(r, sub, range, data);
    if (FAILED(hr) || !data || !*data || sub != 0) return hr;
    D3D12_RESOURCE_DESC d = r->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return hr;
    UINT64 va = r->GetGPUVirtualAddress();
    if (!va) return hr;
    AcquireSRWLockExclusive(&g_mapLock);
    int i = 0;
    while (i < g_mapN && g_maps[i].va != va) i++;
    if (i < (int)_countof(g_maps)) { g_maps[i] = { va, d.Width, (BYTE*)*data }; if (i == g_mapN) g_mapN++; }
    ReleaseSRWLockExclusive(&g_mapLock);
    return hr;
}
// The CPU address of GPU address va, and how many bytes follow it in that buffer (0: unknown).
static BYTE* CpuFor(UINT64 va, UINT64* avail) {
    BYTE* p = NULL;
    *avail = 0;
    AcquireSRWLockShared(&g_mapLock);
    for (int i = 0; i < g_mapN; i++)
        if (va >= g_maps[i].va && va < g_maps[i].va + g_maps[i].size) { p = g_maps[i].cpu + (va - g_maps[i].va); *avail = g_maps[i].va + g_maps[i].size - va; break; }
    ReleaseSRWLockShared(&g_mapLock);
    return p;
}
// IASetVertexBuffers (slot 44): remember slot 0's buffer (the mask quad's rays).
static void STDMETHODCALLTYPE SetVbsHook(ID3D12GraphicsCommandList* l, UINT start, UINT n, const D3D12_VERTEX_BUFFER_VIEW* v) {
    g_setVbs(l, start, n, v);
    if (g_inGfxPass || start != 0 || !n) return;
    if (ListState* s = ListFor(l)) s->vb0 = v ? v[0] : D3D12_VERTEX_BUFFER_VIEW{};
}

// One buffer the mask draw reads: where, and what it held when the draw was recorded.
struct ProbeBuf { UINT64 va; BYTE* cpu; UINT len; BYTE data[36 * 16]; };   // 36 float4: the whole PerView block
struct MaskSnap { void* list; uint32_t frame; bool used, submitted, changed; int nBuf; ProbeBuf buf[5]; };   // buf[0] = vertices
static MaskSnap g_snaps[16];                 // recorded mask draws, until checked at a Present
static SRWLOCK  g_snapLock = SRWLOCK_INIT;
static uint32_t g_probeFrame;
static LONG     g_probeDraws, g_probeSameVb, g_probeSameCb, g_probeNoCpu, g_probeAtSubmit, g_probeByPresent, g_probeLines;
static UINT64   g_probeLastVb, g_probeLastCb;

// Snapshot up to 'want' bytes (at most 576) at GPU address va, read through the buffer's
// CPU mapping. False if no mapping is known or it can't be read.
static bool ProbeTake(ProbeBuf& b, UINT64 va, UINT want) {
    UINT64 avail;
    b.va = va;
    b.cpu = CpuFor(va, &avail);
    if (!b.cpu) return false;
    b.len = (UINT)(avail < want ? avail : want);
    if (b.len > sizeof b.data) b.len = sizeof b.data;
    __try { memcpy(b.data, b.cpu, b.len); } __except (EXCEPTION_EXECUTE_HANDLER) { b.cpu = NULL; return false; }
    return true;
}
// First differing 16-byte row of a buffer against its snapshot, or -1 (-2: unreadable now).
static int ProbeDiffRow(const ProbeBuf& b, int* rows) {
    int first = -1;
    *rows = 0;
    __try {
        for (UINT o = 0; o < b.len; o += 16) {
            UINT n = b.len - o < 16 ? b.len - o : 16;
            if (memcmp(b.cpu + o, b.data + o, n) != 0) { if (first < 0) first = (int)(o / 16); (*rows)++; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
    return first;
}
// Log each of m's buffers that no longer matches its snapshot (400 lines at most in all).
static void ProbeReport(const MaskSnap& m, const char* when) {
    for (int k = 0; k < m.nBuf; k++) {
        int rows, first = ProbeDiffRow(m.buf[k], &rows);
        if (first == -1) continue;
        if (InterlockedIncrement(&g_probeLines) > 400) return;
        if (first == -2)
            Log("graphics: flash probe: frame %u mask draw: %s %llx unreadable %s", m.frame, k ? "constants" : "vertices",
                (unsigned long long)m.buf[k].va, when);
        else
            Log("graphics: flash probe: frame %u mask draw: %s %d at %llx REWRITTEN %s (%d of %u rows differ, first row %d)",
                m.frame, k ? "constants" : "vertices", k, (unsigned long long)m.buf[k].va, when, rows, (m.buf[k].len + 15) / 16, first);
    }
}
// Has any of m's buffers changed (or become unreadable) since the snapshot?
static bool ProbeChanged(const MaskSnap& m) {
    for (int k = 0; k < m.nBuf; k++) { int rows; if (ProbeDiffRow(m.buf[k], &rows) != -1) return true; }
    return false;
}

// At a draw with a mask pipeline: snapshot its vertices and every bound root CBV.
static void FlashProbeDraw(ListState* s) {
    if (!IsMaskPso(s->cur)) return;
    InterlockedIncrement(&g_probeDraws);
    MaskSnap m = {};
    m.list = s->list;
    m.frame = g_probeFrame;
    m.used = true;
    bool cpu = true;
    if (s->vb0.BufferLocation) {
        cpu &= ProbeTake(m.buf[m.nBuf++], s->vb0.BufferLocation, s->vb0.SizeInBytes < 256 ? s->vb0.SizeInBytes : 256);
        if (s->vb0.BufferLocation == g_probeLastVb) InterlockedIncrement(&g_probeSameVb);
        g_probeLastVb = s->vb0.BufferLocation;
    }
    if (const ListShadow* sh = s->sh)
        for (UINT i = 0; i < sh->maxArg && m.nBuf < (int)_countof(m.buf); i++) {
            if (sh->args[i].kind != ARG_CBV) continue;
            cpu &= ProbeTake(m.buf[m.nBuf++], sh->args[i].v, 36 * 16);
            if (m.nBuf == 2) {                                  // the first CBV: same address as last frame's?
                if (sh->args[i].v == g_probeLastCb) InterlockedIncrement(&g_probeSameCb);
                g_probeLastCb = sh->args[i].v;
            }
        }
    if (!cpu) InterlockedIncrement(&g_probeNoCpu);
    static LONG first;
    if (!InterlockedExchange(&first, 1)) {
        Log("graphics: flash probe: first mask draw: vertex buffer %llx (%u bytes, stride %u), %d constant buffer(s), CPU copies %s",
            (unsigned long long)s->vb0.BufferLocation, s->vb0.SizeInBytes, s->vb0.StrideInBytes, m.nBuf - (s->vb0.BufferLocation ? 1 : 0),
            cpu ? "found" : "NOT all found (mapped before the hook, or another resource type)");
        if (s->vb0.BufferLocation && m.buf[0].cpu && m.buf[0].len >= 16) {
            const float* f = (const float*)m.buf[0].data;
            Log("graphics: flash probe: first vertex floats: %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f", f[0], f[1], f[2], f[3],
                m.buf[0].len >= 32 ? f[4] : 0.f, m.buf[0].len >= 32 ? f[5] : 0.f, m.buf[0].len >= 32 ? f[6] : 0.f, m.buf[0].len >= 32 ? f[7] : 0.f);
        }
    }
    AcquireSRWLockExclusive(&g_snapLock);
    int slot = 0;
    for (int i = 0; i < (int)_countof(g_snaps); i++) {
        if (!g_snaps[i].used) { slot = i; break; }
        if (g_snaps[i].frame < g_snaps[slot].frame) slot = i;   // full: drop the oldest
    }
    g_snaps[slot] = m;
    ReleaseSRWLockExclusive(&g_snapLock);
}
// ExecuteCommandLists: the recorded mask draws in these lists, checked as they're handed to the GPU.
static void FlashProbeSubmit(UINT n, ID3D12CommandList* const* lists) {
    AcquireSRWLockExclusive(&g_snapLock);
    for (MaskSnap& m : g_snaps) {
        if (!m.used || m.submitted) continue;
        for (UINT i = 0; i < n; i++) {
            if ((void*)lists[i] != m.list) continue;
            m.submitted = true;
            if (ProbeChanged(m)) { m.changed = true; InterlockedIncrement(&g_probeAtSubmit); ProbeReport(m, "before it was submitted"); }
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_snapLock);
}
// Present: submitted mask draws checked once more (the GPU may still be reading them), then dropped.
static void FlashProbeFrame() {
    AcquireSRWLockExclusive(&g_snapLock);
    for (MaskSnap& m : g_snaps) {
        if (!m.used) continue;
        if (m.submitted) {
            if (!m.changed && ProbeChanged(m)) { InterlockedIncrement(&g_probeByPresent); ProbeReport(m, "after submit, by the next Present"); }
            m.used = false;
        } else if (g_probeFrame - m.frame > 4) {
            m.used = false;                                     // never seen submitted (the list was a bundle or reused)
        }
    }
    ReleaseSRWLockExclusive(&g_snapLock);
    if (++g_probeFrame % 600 == 0)
        Log("graphics: flash probe: last 600 frames: %ld mask draws, %ld with last frame's vertex buffer address, %ld with its "
            "constant buffer address, %ld without CPU copies, %ld rewritten before submit, %ld rewritten after",
            InterlockedExchange(&g_probeDraws, 0), InterlockedExchange(&g_probeSameVb, 0), InterlockedExchange(&g_probeSameCb, 0),
            InterlockedExchange(&g_probeNoCpu, 0), InterlockedExchange(&g_probeAtSubmit, 0), InterlockedExchange(&g_probeByPresent, 0));
}

// After every game draw (DrawInstanced / DrawIndexedInstanced, slots 12 / 13): the
// flash probe, the audit, and SMAA when the draw was the Copy Post.
static void AfterDraw(ID3D12GraphicsCommandList* l) {
    if (g_inGfxPass) return;
    bool smaa = g_x.smaa && g_copyPsoN;
    if (!smaa && !g_x.shadowDrawAudit && !g_x.shadowFlashProbe) return;
    ListState* s = ListFor(l);
    if (!s) return;
    if (g_x.shadowFlashProbe && g_maskPsoN) FlashProbeDraw(s);
    if (g_x.shadowDrawAudit && s->atlas) AuditDraw(s);
    if (smaa && s->isCopy) SmaaAfterCopy(l, s);
}

static void STDMETHODCALLTYPE DrawHook(ID3D12GraphicsCommandList* l, UINT vc, UINT ic, UINT sv, UINT si) {
    g_drawInst(l, vc, ic, sv, si);
    AfterDraw(l);
}

typedef void (STDMETHODCALLTYPE* PFN_DrawIdx)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
static PFN_DrawIdx g_drawIdx;

// Our SetDescriptorHeaps also unbinds the list's compute tables, and only the graphics
// state is put back. The game rebinds before it dispatches, as far as we know; say so
// once if a Dispatch ever follows the before-HUD pass on the same list.
typedef void (STDMETHODCALLTYPE* PFN_Dispatch)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
static PFN_Dispatch g_dispatch;

static void STDMETHODCALLTYPE DispatchHook(ID3D12GraphicsCommandList* l, UINT x, UINT y, UINT z) {
    g_dispatch(l, x, y, z);
    if (g_inGfxPass || !g_x.smaa || !g_smaaRanAny) return;
    ListState* s = ListFor(l);
    static LONG warned;
    if (s && s->smaaRan && !InterlockedExchange(&warned, 1))
        Log("graphics: smaa: the game ran a compute dispatch after the before-HUD pass on the same list "
            "(its compute bindings aren't put back; if anything looks wrong, tell the decomp session)");
}

static void STDMETHODCALLTYPE DrawIdxHook(ID3D12GraphicsCommandList* l, UINT ic, UINT n, UINT si, INT bv, UINT sInst) {
    g_drawIdx(l, ic, n, si, bv, sInst);
    AfterDraw(l);
}

// Every Present: every 1200 frames (20 s at 60), what drew into the shadow map.
static void AuditFrameEnd() {
    static uint32_t frames, reports;
    if (!g_x.shadowDrawAudit || ++frames % 1200 || reports >= 30) return;
    AcquireSRWLockExclusive(&g_auditLock);
    uint32_t total = g_auditDraws[0] + g_auditDraws[1] + g_auditDraws[2] + g_auditDraws[3];
    if (!total) { ReleaseSRWLockExclusive(&g_auditLock); return; }
    reports++;
    // Each game pipeline's verdict: its twin carries the no-clamp VS if its VS is in the list.
    struct Row { AuditSlot a; PsoInfo info; bool known; int verdict; };   // 0 twin + no-clamp VS, 1 twin + game's VS, 2 no twin
    static Row rows[_countof(g_audit)];
    int n = 0;
    uint32_t nearBy[3] = {};
    for (AuditSlot& a : g_audit) {
        if (!a.pso) continue;
        Row& r = rows[n++];
        r.a = a;
        r.known = PsoInfoFor(a.pso, &r.info);
        bool nc = false;
        if (r.known && g_x.shadowNoClampVs)
            for (const NoClampVs& k : kNoClampVs) nc |= r.info.vsLen == k.size && !memcmp(r.info.vsDigest, k.digest, 8);
        r.verdict = !a.twin ? 2 : nc ? 0 : 1;
        nearBy[r.verdict] += a.draws[0];
    }
    Log("graphics: shadow audit, last 1200 frames: shadow-map draws near %u, middle %u, far %u, outside the tiles %u; "
        "near tile viewport y %.0f, %.0fx%.0f, depth %.2f..%.2f; bundles run into it %ld%s",
        g_auditDraws[0], g_auditDraws[1], g_auditDraws[2], g_auditDraws[3], g_auditVp0.TopLeftY, g_auditVp0.Width,
        g_auditVp0.Height, g_auditVp0.MinDepth, g_auditVp0.MaxDepth, InterlockedExchange(&g_auditBundles, 0),
        g_auditLost ? " (some pipelines not counted: table full)" : "");
    Log("graphics: shadow audit:   near cascade: %u draws with the no-clip twin and no-clamp VS, %u with the twin but the game's VS, "
        "%u with no twin", nearBy[0], nearBy[1], nearBy[2]);
    // The near cascade's draws without the full fix, biggest first.
    for (int shown = 0; shown < 16; shown++) {
        int best = -1;
        for (int i = 0; i < n; i++)
            if (rows[i].verdict && rows[i].a.draws[0] && (best < 0 || rows[i].a.draws[0] > rows[best].a.draws[0])) best = i;
        if (best < 0) break;
        Row& r = rows[best];
        char hex[17] = "?";
        if (r.known) for (int i = 0; i < 8; i++) sprintf_s(hex + i * 2, 3, "%02x", r.info.vsDigest[i]);
        if (r.known)
            Log("graphics: shadow audit:     %s: near %u, middle %u, far %u draws; VS %u bytes %s, %u render targets, "
                "depth clip %u, cull %u%s", r.verdict == 2 ? "NO TWIN" : "game's VS", r.a.draws[0], r.a.draws[1], r.a.draws[2],
                r.info.vsLen, hex, r.info.numRt, r.info.depthClip, r.info.cull, r.info.stream ? ", stream pipeline" : "");
        else
            Log("graphics: shadow audit:     %s: near %u, middle %u, far %u draws; pipeline made before the audit saw it",
                r.verdict == 2 ? "NO TWIN" : "game's VS", r.a.draws[0], r.a.draws[1], r.a.draws[2]);
        r.a.draws[0] = 0;
    }
    // Start the next period from zero.
    memset(g_audit, 0, sizeof g_audit);
    memset(g_auditDraws, 0, sizeof g_auditDraws);
    g_auditLost = 0;
    g_auditVp0Set = false;
    ReleaseSRWLockExclusive(&g_auditLock);
}

// smaa=1: which resource each render-target view points at, to tell the Copy Post draw
// into the frame (back-buffer-sized) from the same copy into a smaller target.
struct RtvSlot { SIZE_T ptr; ID3D12Resource* res; };
static RtvSlot g_rtvs[2048];
static SRWLOCK g_rtvLock = SRWLOCK_INIT;
typedef void (STDMETHODCALLTYPE* PFN_CreateRtv)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
static PFN_CreateRtv g_createRtv;

// ID3D12Device::CreateRenderTargetView (slot 20): note which resource the descriptor
// now views.
static void STDMETHODCALLTYPE CreateRtvHook(ID3D12Device* dev, ID3D12Resource* res, const D3D12_RENDER_TARGET_VIEW_DESC* d,
                                             D3D12_CPU_DESCRIPTOR_HANDLE h) {
    g_createRtv(dev, res, d, h);
    AcquireSRWLockExclusive(&g_rtvLock);
    size_t k = (h.ptr >> 5) % _countof(g_rtvs);
    for (size_t i = 0; i < _countof(g_rtvs); i++, k = (k + 1) % _countof(g_rtvs))
        if (g_rtvs[k].ptr == h.ptr || !g_rtvs[k].ptr) { g_rtvs[k].ptr = h.ptr; g_rtvs[k].res = res; break; }
    ReleaseSRWLockExclusive(&g_rtvLock);
}

// The resource render-target descriptor ptr views, or NULL if it was made before the hook.
static ID3D12Resource* RtvResource(SIZE_T ptr) {
    ID3D12Resource* r = NULL;
    AcquireSRWLockShared(&g_rtvLock);
    size_t k = (ptr >> 5) % _countof(g_rtvs);
    for (size_t i = 0; i < _countof(g_rtvs); i++, k = (k + 1) % _countof(g_rtvs)) {
        if (g_rtvs[k].ptr == ptr) { r = g_rtvs[k].res; break; }
        if (!g_rtvs[k].ptr) break;
    }
    ReleaseSRWLockShared(&g_rtvLock);
    return r;
}

// ID3D12Device: CreateDepthStencilView is slot 21. ID3D12GraphicsCommandList: Reset 10,
// ClearState 11, SetPipelineState 25, OMSetRenderTargets 46. Command lists share one
// vtable, so hooking a throwaway list covers the game's.
// ID3D12Device: CreateRenderTargetView 20, CreateDepthStencilView 21. ID3D12GraphicsCommandList:
// DrawInstanced 12, DrawIndexedInstanced 13, Dispatch 14, ExecuteBundle 27, IASetPrimitiveTopology 20, RSSetViewports 21, RSSetScissorRects 22,
// SetDescriptorHeaps 28, SetGraphicsRootSignature 30, SetGraphicsRootDescriptorTable 32,
// SetGraphicsRoot32BitConstant 34, SetGraphicsRoot32BitConstants 36, SetGraphicsRoot
// ConstantBufferView/ShaderResourceView/UnorderedAccessView 38/40/42 (smaa=1,
// shadow_draw_audit=1 or shadow_flash_probe=1 only).
// Despite the name, this installs every device and command-list hook the features
// need: pancaking, SMAA, the audit and the flash probe. Called from CreateDeviceHook
// with the game's new device.
static void InstallPancakeHooks(ID3D12Device* dev) {
    bool ok = HookVtable(dev, 21, (void*)CreateDsvHook, (void**)&g_createDsv);   // also feeds shadow_pso_log
    bool shadows = g_x.shadowPancake || g_x.shadowNoClampVs || g_x.shadowCullNone;
    if (!shadows && !g_x.smaa && !g_x.shadowDrawAudit && !g_x.shadowFlashProbe) return;
    if (g_x.smaa) ok &= HookVtable(dev, 20, (void*)CreateRtvHook, (void**)&g_createRtv);
    // A throwaway direct command list, only to reach the command-list vtable.
    ID3D12CommandAllocator* alloc = NULL;
    ID3D12GraphicsCommandList* list = NULL;
    if (SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&alloc)) &&
        SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, __uuidof(ID3D12GraphicsCommandList),
                                         (void**)&list))) {
        ok &= HookVtable(list, 10, (void*)ListResetHook, (void**)&g_listReset);
        ok &= HookVtable(list, 11, (void*)ClearStateHook, (void**)&g_clearState);
        ok &= HookVtable(list, 25, (void*)SetPsoHook, (void**)&g_setPso);
        ok &= HookVtable(list, 46, (void*)OMSetRTHook, (void**)&g_omSetRT);
        // The draw hooks and the state record.
        if (g_x.smaa || g_x.shadowDrawAudit || g_x.shadowFlashProbe) {
            ok &= HookVtable(list, 27, (void*)ExecBundleHook, (void**)&g_execBundle);
            ok &= HookVtable(list, 12, (void*)DrawHook, (void**)&g_drawInst);
            ok &= HookVtable(list, 13, (void*)DrawIdxHook, (void**)&g_drawIdx);
            ok &= HookVtable(list, 14, (void*)DispatchHook, (void**)&g_dispatch);
            ok &= HookVtable(list, 20, (void*)TopoHook, (void**)&g_setTopo);
            ok &= HookVtable(list, 21, (void*)VpHook, (void**)&g_setVp);
            ok &= HookVtable(list, 22, (void*)ScHook, (void**)&g_setSc);
            ok &= HookVtable(list, 28, (void*)HeapsHook, (void**)&g_setHeaps);
            ok &= HookVtable(list, 30, (void*)RootSigHook, (void**)&g_setRootSig);
            ok &= HookVtable(list, 32, (void*)TableHook, (void**)&g_setTable);
            ok &= HookVtable(list, 34, (void*)Const1Hook, (void**)&g_setConst1);
            ok &= HookVtable(list, 36, (void*)ConstNHook, (void**)&g_setConstN);
            ok &= HookVtable(list, 38, (void*)CbvHook, (void**)&g_setCbv);
            ok &= HookVtable(list, 40, (void*)SrvHook, (void**)&g_setSrv);
            ok &= HookVtable(list, 42, (void*)UavHook, (void**)&g_setUav);
        }
        if (g_x.shadowFlashProbe) {
            // IASetVertexBuffers is slot 44; ID3D12Resource::Map is slot 8, hooked through a
            // throwaway upload buffer (made now, before the game maps its own).
            bool probe = HookVtable(list, 44, (void*)SetVbsHook, (void**)&g_setVbs);
            D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD };
            D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
                                       D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
            ID3D12Resource* dummy = NULL;
            if (SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                                       __uuidof(ID3D12Resource), (void**)&dummy)) && dummy) {
                probe &= HookVtable(dummy, 8, (void*)MapHook, (void**)&g_map);
                dummy->Release();
            } else {
                probe = false;
            }
            Log(probe ? "graphics: flash probe: hooks in place (vertex buffers, buffer maps)"
                      : "graphics: flash probe: couldn't hook vertex buffers or buffer maps");
        }
    } else {
        ok = false;
    }
    if (list) { list->Close(); list->Release(); }
    if (alloc) alloc->Release();
    if (shadows)
        Log(ok ? "graphics: shadow pancaking: hooks in place (shadow map %ux%u)" : "graphics: shadow pancaking: couldn't hook the device or command lists",
            g_atlasW, g_atlasH);
    if (g_x.smaa)
        Log(ok ? "graphics: smaa: command-list hooks in place" : "graphics: smaa: couldn't hook the device or command lists");
    g_listHooksOk = ok;
}

// ------------------------------------------------------------------ FXAA

// The FXAA shaders as HLSL source, compiled at run time with d3dcompiler_47 (FxaaCompile).
// The VS makes one full-screen triangle from SV_VertexID, so no vertex buffer is needed;
// the PS reads a copy of the back buffer (t0) and writes the smoothed pixel.
static const char kFxaaHlsl[] = R"(
Texture2D tex : register(t0);
SamplerState samp : register(s0);
cbuffer C : register(b0) { float2 rcpFrame; float subpix; float edge; float edgeMin; float3 pad; };

struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V VS(uint id : SV_VertexID) {
    V o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float L(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float LA(float2 p) { return L(tex.SampleLevel(samp, p, 0).rgb); }

// FXAA 3.11 (Timothy Lottes), PC quality path, preset 20-ish.
float4 PS(V i) : SV_Target {
    float2 posM = i.uv;
    float4 rgbM = tex.SampleLevel(samp, posM, 0);
    float lumaM = L(rgbM.rgb);
    float lumaS = L(tex.SampleLevel(samp, posM, 0, int2( 0, 1)).rgb);
    float lumaE = L(tex.SampleLevel(samp, posM, 0, int2( 1, 0)).rgb);
    float lumaN = L(tex.SampleLevel(samp, posM, 0, int2( 0,-1)).rgb);
    float lumaW = L(tex.SampleLevel(samp, posM, 0, int2(-1, 0)).rgb);
    float rangeMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));
    float rangeMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));
    float range = rangeMax - rangeMin;
    if (range < max(edgeMin, rangeMax * edge)) return rgbM;

    float lumaNW = L(tex.SampleLevel(samp, posM, 0, int2(-1,-1)).rgb);
    float lumaSE = L(tex.SampleLevel(samp, posM, 0, int2( 1, 1)).rgb);
    float lumaNE = L(tex.SampleLevel(samp, posM, 0, int2( 1,-1)).rgb);
    float lumaSW = L(tex.SampleLevel(samp, posM, 0, int2(-1, 1)).rgb);
    float lumaNS = lumaN + lumaS, lumaWE = lumaW + lumaE;
    float subpixRcpRange = 1.0 / range;
    float subpixNSWE = lumaNS + lumaWE;
    float edgeHorz1 = -2.0 * lumaM + lumaNS, edgeVert1 = -2.0 * lumaM + lumaWE;
    float lumaNESE = lumaNE + lumaSE, lumaNWNE = lumaNW + lumaNE;
    float edgeHorz2 = -2.0 * lumaE + lumaNESE, edgeVert2 = -2.0 * lumaN + lumaNWNE;
    float lumaNWSW = lumaNW + lumaSW, lumaSWSE = lumaSW + lumaSE;
    float edgeHorz4 = abs(edgeHorz1) * 2.0 + abs(edgeHorz2);
    float edgeVert4 = abs(edgeVert1) * 2.0 + abs(edgeVert2);
    float edgeHorz3 = -2.0 * lumaW + lumaNWSW, edgeVert3 = -2.0 * lumaS + lumaSWSE;
    float edgeHorz = abs(edgeHorz3) + edgeHorz4, edgeVert = abs(edgeVert3) + edgeVert4;
    float subpixNWSWNESE = lumaNWSW + lumaNESE;
    float lengthSign = rcpFrame.x;
    bool horzSpan = edgeHorz >= edgeVert;
    float subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;
    if (!horzSpan) { lumaN = lumaW; lumaS = lumaE; }
    if (horzSpan) lengthSign = rcpFrame.y;
    float subpixB = subpixA * (1.0 / 12.0) - lumaM;
    float gradientN = lumaN - lumaM, gradientS = lumaS - lumaM;
    float lumaNN = lumaN + lumaM, lumaSS = lumaS + lumaM;
    bool pairN = abs(gradientN) >= abs(gradientS);
    float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN) lengthSign = -lengthSign;
    float subpixC = saturate(abs(subpixB) * subpixRcpRange);

    float2 posB = posM;
    float2 offNP = float2(horzSpan ? rcpFrame.x : 0.0, horzSpan ? 0.0 : rcpFrame.y);
    if (!horzSpan) posB.x += lengthSign * 0.5;
    if (horzSpan) posB.y += lengthSign * 0.5;
    float2 posN = posB - offNP;
    float2 posP = posB + offNP;
    float subpixD = -2.0 * subpixC + 3.0;
    float lumaEndN = LA(posN);
    float subpixE = subpixC * subpixC;
    float lumaEndP = LA(posP);
    if (!pairN) lumaNN = lumaSS;
    float gradientScaled = gradient * 0.25;
    float lumaMM = lumaM - lumaNN * 0.5;
    float subpixF = subpixD * subpixE;
    bool lumaMLTZero = lumaMM < 0.0;
    lumaEndN -= lumaNN * 0.5;
    lumaEndP -= lumaNN * 0.5;
    bool doneN = abs(lumaEndN) >= gradientScaled;
    bool doneP = abs(lumaEndP) >= gradientScaled;
    if (!doneN) posN -= offNP * 1.5;
    if (!doneP) posP += offNP * 1.5;
    static const float steps[8] = { 2.0, 2.0, 2.0, 2.0, 4.0, 8.0, 8.0, 8.0 };
    [unroll] for (int k = 0; k < 8; k++) {
        if (doneN && doneP) break;
        if (!doneN) lumaEndN = LA(posN) - lumaNN * 0.5;
        if (!doneP) lumaEndP = LA(posP) - lumaNN * 0.5;
        doneN = abs(lumaEndN) >= gradientScaled;
        doneP = abs(lumaEndP) >= gradientScaled;
        if (!doneN) posN -= offNP * steps[k];
        if (!doneP) posP += offNP * steps[k];
    }
    float dstN = horzSpan ? posM.x - posN.x : posM.y - posN.y;
    float dstP = horzSpan ? posP.x - posM.x : posP.y - posM.y;
    bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero;
    bool goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
    float spanLengthRcp = 1.0 / (dstP + dstN);
    bool directionN = dstN < dstP;
    float dst = min(dstN, dstP);
    bool goodSpan = directionN ? goodSpanN : goodSpanP;
    float subpixG = subpixF * subpixF;
    float pixelOffset = dst * -spanLengthRcp + 0.5;
    float subpixH = subpixG * subpix;
    float pixelOffsetSubpix = max(goodSpan ? pixelOffset : 0.0, subpixH);
    if (!horzSpan) posM.x += pixelOffsetSubpix * lengthSign;
    if (horzSpan) posM.y += pixelOffsetSubpix * lengthSign;
    return float4(tex.SampleLevel(samp, posM, 0).rgb, rgbM.a);
}
)";

// Present passes that can be in flight on the GPU at once: one command allocator each,
// reused only after the fence says the GPU is done with it.
static const UINT kFrames = 3;

// Our own pass at Present, on its own command list: FXAA and/or the overlay
// (debug menu). The base objects (allocators, list, fence, RTV heap) serve both.
struct Fxaa {
    bool                       failed;       // FXAA pipeline failed (the overlay can still run)
    bool                       baseReady, baseFailed, fxaaReady;
    ID3D12Device*              dev;
    ID3D12CommandQueue*        queue;
    IDXGISwapChain*            sc;           // the swap chain these resources belong to
    IDXGISwapChain3*           sc3;
    ID3D12RootSignature*       rs;
    ID3D12PipelineState*       pso;
    DXGI_FORMAT                psoFormat;
    ID3D12DescriptorHeap*      rtvHeap;
    ID3D12DescriptorHeap*      srvHeap;
    ID3D12Resource*            copy;         // copy of the back buffer the shader reads
    UINT                       copyW, copyH;
    DXGI_FORMAT                copyFormat;
    ID3D12CommandAllocator*    alloc[kFrames];
    ID3D12GraphicsCommandList* list;
    ID3D12Fence*               fence;
    UINT64                     fenceValue, allocFence[kFrames];
    HANDLE                     event;
    UINT                       frame;
    ID3DBlob*                  vs;
    ID3DBlob*                  ps;
};
static Fxaa g_fx;
static CRITICAL_SECTION g_fxLock;       // Present, ResizeBuffers, swap-chain creation and SmaaAfterCopy

// Wait (2 s at most) for the GPU to finish our last Present pass, before something it
// uses is released.
static void FxaaWaitIdle() {
    if (!g_fx.fence || !g_fx.fenceValue) return;
    if (g_fx.fence->GetCompletedValue() < g_fx.fenceValue) {
        g_fx.fence->SetEventOnCompletion(g_fx.fenceValue, g_fx.event);
        WaitForSingleObject(g_fx.event, 2000);
    }
}

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = NULL; } }

// Log what failed and turn FXAA off for the session (the overlay still runs).
static void FxaaFail(const char* what, HRESULT hr) {
    Log("graphics: FXAA disabled: %s failed (0x%08lx)", what, (unsigned long)hr);
    g_fx.failed = true;
}

// Compile kFxaaHlsl's VS and PS into g_fx.vs / g_fx.ps, with d3dcompiler_47.dll loaded
// on demand. On failure FXAA is off for the session.
static bool FxaaCompile() {
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    pD3DCompile compile = dc ? (pD3DCompile)GetProcAddress(dc, "D3DCompile") : NULL;
    if (!compile) { Log("graphics: FXAA disabled: d3dcompiler_47.dll not available"); g_fx.failed = true; return false; }
    ID3DBlob* err = NULL;
    HRESULT hr = compile(kFxaaHlsl, sizeof kFxaaHlsl - 1, "fxaa", NULL, NULL, "VS", "vs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &g_fx.vs, &err);
    if (SUCCEEDED(hr)) {
        SafeRelease(err);
        hr = compile(kFxaaHlsl, sizeof kFxaaHlsl - 1, "fxaa", NULL, NULL, "PS", "ps_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &g_fx.ps, &err);
    }
    if (FAILED(hr)) {
        Log("graphics: FXAA disabled: shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        SafeRelease(err);
        g_fx.failed = true;
        return false;
    }
    return true;
}

// What every Present pass needs (FXAA, SMAA at Present, the overlay): one RTV slot, an
// allocator per in-flight frame, our command list (closed until used), a fence and its
// event. On failure the whole Present pass is off.
static bool PresentInitBase() {
    HRESULT hr;
    const char* what = NULL;
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    if (FAILED(hr = g_fx.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&g_fx.rtvHeap))) what = "RTV heap";
    for (UINT i = 0; !what && i < kFrames; i++)
        if (FAILED(hr = g_fx.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&g_fx.alloc[i]))) what = "allocator";
    if (!what && FAILED(hr = g_fx.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_fx.alloc[0], NULL, __uuidof(ID3D12GraphicsCommandList), (void**)&g_fx.list))) what = "command list";
    if (!what) g_fx.list->Close();
    if (!what && FAILED(hr = g_fx.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_fx.fence))) what = "fence";
    if (what) {
        Log("graphics: Present pass disabled (FXAA and overlay): %s failed (0x%08lx)", what, (unsigned long)hr);
        g_fx.baseFailed = true;
        return false;
    }
    g_fx.event = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_fx.baseReady = true;
    return true;
}

// FXAA's root signature (an SRV table at t0, 8 constants at b0, a linear clamp sampler
// at s0) and its shader-visible SRV heap. The pipeline itself waits for the back
// buffer's format (FxaaPso).
static bool FxaaInitPipeline() {
    if (!FxaaCompile()) return false;
    HRESULT hr;
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 8;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, params, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob* blob = NULL;
    ID3DBlob* err = NULL;
    // Our own sampler must stay linear: call the real serializer, not the hook.
    PFN_Serialize ser = g_serialize ? g_serialize : (PFN_Serialize)GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature");
    hr = ser(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    SafeRelease(err);
    if (FAILED(hr)) { FxaaFail("root signature", hr); return false; }
    hr = g_fx.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), (void**)&g_fx.rs);
    SafeRelease(blob);
    if (FAILED(hr)) { FxaaFail("CreateRootSignature", hr); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    if (FAILED(hr = g_fx.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&g_fx.srvHeap))) { FxaaFail("SRV heap", hr); return false; }
    g_fx.fxaaReady = true;
    return true;
}

// FXAA's pipeline for render-target format fmt, made again if the format changes.
static bool FxaaPso(DXGI_FORMAT fmt) {
    if (g_fx.pso && g_fx.psoFormat == fmt) return true;
    SafeRelease(g_fx.pso);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = g_fx.rs;
    d.VS = { g_fx.vs->GetBufferPointer(), g_fx.vs->GetBufferSize() };
    d.PS = { g_fx.ps->GetBufferPointer(), g_fx.ps->GetBufferSize() };
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = fmt;
    d.SampleDesc.Count = 1;
    HRESULT hr = g_fx.dev->CreateGraphicsPipelineState(&d, __uuidof(ID3D12PipelineState), (void**)&g_fx.pso);
    if (FAILED(hr)) { FxaaFail("pipeline state", hr); return false; }
    g_fx.psoFormat = fmt;
    return true;
}

// Typeless/sRGB back buffers: view as the plain UNORM format.
static DXGI_FORMAT ViewFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return f;
    }
}

// The texture FXAA reads: a copy of the back buffer (a shader can't read the target it
// writes), with its SRV. Made again when the back buffer's size or format changes.
static bool FxaaCopyTarget(const D3D12_RESOURCE_DESC& bb) {
    if (g_fx.copy && g_fx.copyW == bb.Width && g_fx.copyH == bb.Height && g_fx.copyFormat == bb.Format) return true;
    FxaaWaitIdle();
    SafeRelease(g_fx.copy);
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC d = bb;
    d.Flags = D3D12_RESOURCE_FLAG_NONE;
    d.MipLevels = 1;
    d.DepthOrArraySize = 1;
    d.SampleDesc = { 1, 0 };
    HRESULT hr = g_fx.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                   NULL, __uuidof(ID3D12Resource), (void**)&g_fx.copy);
    if (FAILED(hr)) { FxaaFail("copy texture", hr); return false; }
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = ViewFormat(bb.Format);
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    g_fx.dev->CreateShaderResourceView(g_fx.copy, &sv, g_fx.srvHeap->GetCPUDescriptorHandleForHeapStart());
    g_fx.copyW = (UINT)bb.Width;
    g_fx.copyH = bb.Height;
    g_fx.copyFormat = bb.Format;
    Log("graphics: FXAA active at %ux%u", g_fx.copyW, g_fx.copyH);
    return true;
}

// A barrier moving all of r's subresources from state a to state b (D3D12 makes the
// app declare each change of use: copy source, render target, shader input...).
static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER t = {};
    t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    t.Transition.pResource = r;
    t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    t.Transition.StateBefore = a;
    t.Transition.StateAfter = b;
    return t;
}

// debug_menu.cpp's overlay (GfxExt_InstallOverlay): records its drawing on cl into the
// back buffer through rtv, at the end of our Present pass.
typedef void (*GfxOverlayFn)(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                             DXGI_FORMAT fmt, UINT w, UINT h);
static GfxOverlayFn g_overlay;

// ------------------------------------------------------------------ frame submission probe
//
// For AA before the HUD: if the game submitted the 3D scene and the UI as separate
// command lists, an FXAA pass could run between them on our own list, with no
// engine state to preserve. Count the game's ExecuteCommandLists calls per Present
// and log the pattern once, on a steady in-game frame.
typedef void (STDMETHODCALLTYPE* PFN_Execute)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
static PFN_Execute   g_execute;
static volatile LONG g_execCalls, g_execLists;
static int           g_execFrames;

// ID3D12CommandQueue::ExecuteCommandLists (slot 10), hooked in CreateSwapChainHook:
// counts submissions, and the flash probe checks the lists being handed to the GPU.
static void STDMETHODCALLTYPE ExecuteHook(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    InterlockedIncrement(&g_execCalls);
    InterlockedAdd(&g_execLists, (LONG)n);
    if (g_x.shadowFlashProbe && lists) FlashProbeSubmit(n, lists);
    g_execute(q, n, lists);
}

// Every Present: the counts restart each frame; the 600th frame's are logged, then
// this stops.
static void ProbeFrame() {
    if (!g_execute || g_execFrames > 600) return;
    LONG calls = InterlockedExchange(&g_execCalls, 0), lists = InterlockedExchange(&g_execLists, 0);
    if (++g_execFrames == 600)
        Log("graphics: the game submits %ld command list(s) in %ld ExecuteCommandLists call(s) per frame", lists, calls);
}

// ------------------------------------------------------------------ SMAA
//
// smaa=1: SMAA 1x (iryoku/smaa, MIT; re\third_party\smaa_71c806a) in three passes:
// colour edges -> blending weights (AreaTex/SearchTex lookups) -> neighbourhood
// blend back into the back buffer (src\shaders\smaa_passes.hlsl). It runs right
// after the game's Copy Post draw puts the 3D image on the back buffer, before the
// HUD, on the game's own command list, whose state is then put back; on frames
// where that didn't happen, at Present on the whole frame (HUD included), like fxaa.
struct Smaa {
    bool                  failed, ready;
    ID3D12Device*         dev;
    ID3D12RootSignature*  rs;            // [0] b0 metrics (4 constants), [1] t0..t4; s0 linear, s1 point
    ID3D12PipelineState*  edgePso[5];    // per preset (1 low .. 4 ultra), made on first use: -> R8G8 edges
    ID3D12PipelineState*  blendPso[5];   //   -> RGBA8 blending weights
    volatile LONG         level;         // 0 off (debug menu), 1..4 the preset in use
    ID3D12PipelineState*  outPso[2];     // neighbourhood blend (or smaa_debug's edge view) -> the image: [0] through an
    DXGI_FORMAT           outFormat[2];  //   sRGB view, [1] as UNORM with the sRGB encode in the shader (the engine's
                                         //   typed frame target can't take an sRGB view)
    ID3D12DescriptorHeap* srvHeap;       // table A 0..4: colour, colour sRGB, edges, area, search; table B 5..9: ..., blend, ...
    ID3D12DescriptorHeap* rtvHeap;       // edges, blend, back buffer
    UINT                  srvStep, rtvStep;
    ID3D12Resource*       area;
    ID3D12Resource*       search;
    ID3D12Resource*       upload;        // their texels; the copy is recorded on the first list SMAA runs on
    bool                  uploaded;
    ID3D12Resource*       color;         // copy of the back buffer (typeless where it can be: read as UNORM and sRGB)
    ID3D12Resource*       edges;
    ID3D12Resource*       blend;
    UINT                  w, h;
    DXGI_FORMAT           bbFormat;
    UINT64                frame, lastFrame;  // Presents seen; the last one SMAA already covered the whole frame for
    LONG                  hudRuns, presentRuns;
};
// frame starts at 1 so that lastFrame's 0 doesn't read as "this frame is already covered".
static Smaa g_sm = [] { Smaa s = {}; s.frame = 1; return s; }();
static const char* const kSmaaPresetNames[5] = { "off", "low", "medium", "high", "ultra" };

// The preset-dependent shaders (EdgePS, BlendVS, BlendPS), by level 1..4.
struct SmaaPresetBlobs { const unsigned char* edgePs; size_t edgePsLen; const unsigned char* blendVs; size_t blendVsLen;
                         const unsigned char* blendPs; size_t blendPsLen; };
#define SMAA_BLOBS(e, bv, bp) { e, sizeof e, bv, sizeof bv, bp, sizeof bp }
static const SmaaPresetBlobs kSmaaPresets[5] = {
    {},
    SMAA_BLOBS(kSmaaEdgePSL, kSmaaBlendVSL, kSmaaBlendPSL),
    SMAA_BLOBS(kSmaaEdgePSM, kSmaaBlendVSM, kSmaaBlendPSM),
    SMAA_BLOBS(kSmaaEdgePS, kSmaaBlendVS, kSmaaBlendPS),
    SMAA_BLOBS(kSmaaEdgePSU, kSmaaBlendVSU, kSmaaBlendPSU),
};
#undef SMAA_BLOBS

// Log what failed and turn SMAA off for the session (fxaa=1, if set, takes over at Present).
static void SmaaFail(const char* what, HRESULT hr) {
    Log("graphics: smaa disabled: %s failed (0x%08lx)", what, (unsigned long)hr);
    g_sm.failed = true;
}

// 8-bit RGBA/BGRA back buffers: a typeless copy read as UNORM (edges are found in gamma
// space) and as sRGB (blending in linear space), written back through an sRGB view.
// Anything else is read and written as itself.
static void SmaaFormats(DXGI_FORMAT bb, DXGI_FORMAT* copy, DXGI_FORMAT* unorm, DXGI_FORMAT* srgb) {
    switch (bb) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        *copy = DXGI_FORMAT_R8G8B8A8_TYPELESS; *unorm = DXGI_FORMAT_R8G8B8A8_UNORM; *srgb = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        break;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        *copy = DXGI_FORMAT_B8G8R8A8_TYPELESS; *unorm = DXGI_FORMAT_B8G8R8A8_UNORM; *srgb = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        break;
    default:
        *copy = bb; *unorm = *srgb = ViewFormat(bb);
        break;
    }
}

// CPU handles of slot i in our SRV / RTV heaps.
static D3D12_CPU_DESCRIPTOR_HANDLE SmaaSrv(UINT i) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_sm.srvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)i * g_sm.srvStep;
    return h;
}

static D3D12_CPU_DESCRIPTOR_HANDLE SmaaRtv(UINT i) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_sm.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)i * g_sm.rtvStep;
    return h;
}

// A 2D shader view of r, read as format f, at SRV heap slot 'slot'.
static void SmaaTexSrv(ID3D12Resource* r, DXGI_FORMAT f, UINT slot) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = f;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    g_sm.dev->CreateShaderResourceView(r, &sv, SmaaSrv(slot));
}

// A w x h 2D texture in GPU memory, starting in 'state'. NULL (and SMAA off) on failure.
static ID3D12Resource* SmaaTexture(UINT w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                                   const char* what) {
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = f;
    d.SampleDesc = { 1, 0 };
    d.Flags = flags;
    D3D12_CLEAR_VALUE cv = { f };                       // black, what each pass clears to
    ID3D12Resource* r = NULL;
    HRESULT hr = g_sm.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state,
                                                   (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ? &cv : NULL,
                                                   __uuidof(ID3D12Resource), (void**)&r);
    if (FAILED(hr)) { SmaaFail(what, hr); return NULL; }
    return r;
}

// One SMAA pass's pipeline: a full-screen triangle, no blending, no depth, one target
// of format rt. NULL (and SMAA off) on failure.
static ID3D12PipelineState* SmaaPso(const void* vs, size_t vsLen, const void* ps, size_t psLen, DXGI_FORMAT rt) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = g_sm.rs;
    d.VS = { vs, vsLen };
    d.PS = { ps, psLen };
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = rt;
    d.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = NULL;
    HRESULT hr = g_sm.dev->CreateGraphicsPipelineState(&d, __uuidof(ID3D12PipelineState), (void**)&pso);
    if (FAILED(hr)) { SmaaFail("pipeline state", hr); return NULL; }
    return pso;
}

// Everything that doesn't depend on the back buffer or the preset: root signature,
// descriptor heaps, AreaTex/SearchTex and their upload buffer.
//
// The upload buffer's layout: each texture row starts on a 256-byte boundary and each
// texture on a 512-byte one (D3D12's copy alignment rules), so the rows are padded.
static const UINT kAreaPitch = (AREATEX_PITCH + 255) & ~255u, kSearchPitch = (SEARCHTEX_PITCH + 255) & ~255u;
static const UINT kSearchOffset = (kAreaPitch * AREATEX_HEIGHT + 511) & ~511u;

static bool SmaaInit() {
    // Root signature: 4 constants (the metrics) for every stage, a table of 5 SRVs for
    // the pixel shaders, and two static samplers (s0 linear, s1 point).
    HRESULT hr;
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0, 0, 0 };
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC samp[2] = {};
    for (UINT i = 0; i < 2; i++) {
        samp[i].Filter = i ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        samp[i].AddressU = samp[i].AddressV = samp[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp[i].MaxLOD = D3D12_FLOAT32_MAX;
        samp[i].ShaderRegister = i;
        samp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, params, 2, samp, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob* blob = NULL;
    ID3DBlob* err = NULL;
    // The real serializer: the anisotropy hook would turn our samplers anisotropic.
    PFN_Serialize ser = g_serialize ? g_serialize : (PFN_Serialize)GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature");
    hr = ser ? ser(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err) : E_FAIL;
    SafeRelease(err);
    if (FAILED(hr)) { SmaaFail("root signature", hr); return false; }
    hr = g_sm.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), (void**)&g_sm.rs);
    SafeRelease(blob);
    if (FAILED(hr)) { SmaaFail("CreateRootSignature", hr); return false; }

    // Descriptor heaps: 10 SRVs (two tables of 5), 3 RTVs.
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 10, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    if (FAILED(hr = g_sm.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&g_sm.srvHeap))) { SmaaFail("SRV heap", hr); return false; }
    D3D12_DESCRIPTOR_HEAP_DESC rd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    if (FAILED(hr = g_sm.dev->CreateDescriptorHeap(&rd, __uuidof(ID3D12DescriptorHeap), (void**)&g_sm.rtvHeap))) { SmaaFail("RTV heap", hr); return false; }
    g_sm.srvStep = g_sm.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_sm.rtvStep = g_sm.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // AreaTex and SearchTex (SMAA's precomputed lookup tables), and an upload buffer with
    // their texels; RunSmaa records the copy into the textures the first time it runs.
    g_sm.area = SmaaTexture(AREATEX_WIDTH, AREATEX_HEIGHT, DXGI_FORMAT_R8G8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, "AreaTex");
    g_sm.search = g_sm.area ? SmaaTexture(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_NONE,
                                          D3D12_RESOURCE_STATE_COPY_DEST, "SearchTex") : NULL;
    if (!g_sm.search) return false;
    D3D12_HEAP_PROPERTIES up = { D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = kSearchOffset + kSearchPitch * SEARCHTEX_HEIGHT;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc = { 1, 0 };
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(hr = g_sm.dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                                      __uuidof(ID3D12Resource), (void**)&g_sm.upload))) { SmaaFail("upload buffer", hr); return false; }
    BYTE* p = NULL;
    D3D12_RANGE none = { 0, 0 };
    if (FAILED(hr = g_sm.upload->Map(0, &none, (void**)&p))) { SmaaFail("upload map", hr); return false; }
    for (UINT y = 0; y < AREATEX_HEIGHT; y++) memcpy(p + y * kAreaPitch, areaTexBytes + y * AREATEX_PITCH, AREATEX_PITCH);
    for (UINT y = 0; y < SEARCHTEX_HEIGHT; y++) memcpy(p + kSearchOffset + y * kSearchPitch, searchTexBytes + y * SEARCHTEX_PITCH, SEARCHTEX_PITCH);
    g_sm.upload->Unmap(0, NULL);
    for (UINT t = 0; t < 2; t++) {                    // both tables
        SmaaTexSrv(g_sm.area, DXGI_FORMAT_R8G8_UNORM, t * 5 + 3);
        SmaaTexSrv(g_sm.search, DXGI_FORMAT_R8_UNORM, t * 5 + 4);
    }
    g_sm.ready = true;
    return true;
}

// The back-buffer-sized textures. Released (after the GPU is done) by ResizeBuffers
// and a new swap chain, and made again here at the new size.
static ID3D12Resource* g_smRetired[16];     // replaced while the GPU might still use them: never released
static UINT g_smRetiredN;

static void SmaaReleaseSized() {
    SafeRelease(g_sm.color);
    SafeRelease(g_sm.edges);
    SafeRelease(g_sm.blend);
    g_sm.w = g_sm.h = 0;
}

// Make (or keep) the textures for a w x h back buffer of format bbFormat, with their
// views: the colour copy in both tables (UNORM and sRGB), edges and blend.
static bool SmaaSized(UINT w, UINT h, DXGI_FORMAT bbFormat) {
    if (g_sm.color && g_sm.w == w && g_sm.h == h && g_sm.bbFormat == bbFormat) return true;
    if (g_sm.color) {
        // Size changed without ResizeBuffers: the game's lists in flight may still use them.
        ID3D12Resource* old[3] = { g_sm.color, g_sm.edges, g_sm.blend };
        for (ID3D12Resource* r : old)
            if (r && g_smRetiredN < _countof(g_smRetired)) g_smRetired[g_smRetiredN++] = r;   // kept alive (rare; a few MB)
        g_sm.color = g_sm.edges = g_sm.blend = NULL;
    }
    DXGI_FORMAT copy, unorm, srgb;
    SmaaFormats(bbFormat, &copy, &unorm, &srgb);
    g_sm.color = SmaaTexture(w, h, copy, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, "colour copy");
    g_sm.edges = g_sm.color ? SmaaTexture(w, h, DXGI_FORMAT_R8G8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, "edges texture") : NULL;
    g_sm.blend = g_sm.edges ? SmaaTexture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, "blend texture") : NULL;
    if (!g_sm.blend) { SmaaReleaseSized(); return false; }
    for (UINT t = 0; t < 2; t++) {
        SmaaTexSrv(g_sm.color, unorm, t * 5 + 0);
        SmaaTexSrv(g_sm.color, srgb, t * 5 + 1);
    }
    SmaaTexSrv(g_sm.edges, DXGI_FORMAT_R8G8_UNORM, 2);
    SmaaTexSrv(g_sm.blend, DXGI_FORMAT_R8G8B8A8_UNORM, 7);
    g_sm.dev->CreateRenderTargetView(g_sm.edges, NULL, SmaaRtv(0));
    g_sm.dev->CreateRenderTargetView(g_sm.blend, NULL, SmaaRtv(1));
    g_sm.w = w;
    g_sm.h = h;
    g_sm.bbFormat = bbFormat;
    Log("graphics: smaa: %ux%u, back buffer format %d, %s preset%s", w, h, (int)bbFormat, kSmaaPresetNames[g_sm.level],
        g_x.smaaDebug ? ", showing the detected edges (smaa_debug)" : "");
    return true;
}

// Ready to run on this back buffer? (Under g_fxLock.)
static bool SmaaPrepare(ID3D12Device* dev, const D3D12_RESOURCE_DESC& bd, int level, bool srgbOut) {
    if (g_sm.failed || level < 1 || level > 4) return false;
    if (bd.SampleDesc.Count != 1 || bd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return false;
    if (!g_sm.dev) { g_sm.dev = dev; dev->AddRef(); }
    if (!g_sm.ready && !SmaaInit()) return false;
    if (!SmaaSized((UINT)bd.Width, bd.Height, bd.Format)) return false;
    if (!g_sm.edgePso[level]) {                           // kept for the session: switching presets is instant
        const SmaaPresetBlobs& b = kSmaaPresets[level];
        g_sm.edgePso[level] = SmaaPso(kSmaaEdgeVS, sizeof kSmaaEdgeVS, b.edgePs, b.edgePsLen, DXGI_FORMAT_R8G8_UNORM);
        if (!g_sm.edgePso[level]) return false;
    }
    if (!g_sm.blendPso[level]) {
        const SmaaPresetBlobs& b = kSmaaPresets[level];
        g_sm.blendPso[level] = SmaaPso(b.blendVs, b.blendVsLen, b.blendPs, b.blendPsLen, DXGI_FORMAT_R8G8B8A8_UNORM);
        if (!g_sm.blendPso[level]) return false;
    }
    DXGI_FORMAT copy, unorm, srgb;
    SmaaFormats(bd.Format, &copy, &unorm, &srgb);
    int o = srgbOut ? 0 : 1;
    DXGI_FORMAT f = srgbOut ? srgb : unorm;
    if (!g_sm.outPso[o] || g_sm.outFormat[o] != f) {
        SafeRelease(g_sm.outPso[o]);
        if (g_x.smaaDebug)
            g_sm.outPso[o] = SmaaPso(kSmaaNbVS, sizeof kSmaaNbVS, kSmaaDebugPS, sizeof kSmaaDebugPS, f);
        else if (srgbOut || srgb == unorm)                // (no sRGB form: write it as it is)
            g_sm.outPso[o] = SmaaPso(kSmaaNbVS, sizeof kSmaaNbVS, kSmaaNbPS, sizeof kSmaaNbPS, f);
        else
            g_sm.outPso[o] = SmaaPso(kSmaaNbVS, sizeof kSmaaNbVS, kSmaaNbPSEnc, sizeof kSmaaNbPSEnc, f);
        if (!g_sm.outPso[o]) return false;
        g_sm.outFormat[o] = f;
    }
    return true;
}

// Record the three passes on cl. The image (the back buffer, or the engine's own frame
// target) goes in as bbState and comes out as a render target; only the rectangle 'out'
// of it is written: through an sRGB view if srgbOut, else UNORM with the encode in the
// shader. The caller sets g_inGfxPass.
static void RunSmaa(ID3D12GraphicsCommandList* cl, ID3D12Resource* bb, D3D12_RESOURCE_STATES bbState, const D3D12_RECT& out, int level,
                    bool srgbOut) {
    D3D12_RESOURCE_BARRIER b[2];
    // First run: copy AreaTex/SearchTex from the upload buffer into their textures.
    if (!g_sm.uploaded) {
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.pResource = g_sm.upload;
        dst.pResource = g_sm.area;
        src.PlacedFootprint = { 0, { DXGI_FORMAT_R8G8_UNORM, AREATEX_WIDTH, AREATEX_HEIGHT, 1, kAreaPitch } };
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);
        dst.pResource = g_sm.search;
        src.PlacedFootprint = { kSearchOffset, { DXGI_FORMAT_R8_UNORM, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1, kSearchPitch } };
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);
        b[0] = Transition(g_sm.area, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        b[1] = Transition(g_sm.search, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cl->ResourceBarrier(2, b);
        g_sm.uploaded = true;
    }
    // Copy the image: the passes read the copy and the last one writes the image itself.
    b[0] = Transition(bb, bbState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    b[1] = Transition(g_sm.color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->ResourceBarrier(2, b);
    cl->CopyResource(g_sm.color, bb);
    b[0] = Transition(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    b[1] = Transition(g_sm.color, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cl->ResourceBarrier(2, b);

    // State shared by the three passes: our heap and root signature, the metrics
    // (1/w, 1/h, w, h), a full-image viewport and scissor.
    D3D12_GPU_DESCRIPTOR_HANDLE tableA = g_sm.srvHeap->GetGPUDescriptorHandleForHeapStart(), tableB = tableA;
    tableB.ptr += 5ull * g_sm.srvStep;
    float metrics[4] = { 1.0f / g_sm.w, 1.0f / g_sm.h, (float)g_sm.w, (float)g_sm.h };
    D3D12_VIEWPORT vp = { 0, 0, (float)g_sm.w, (float)g_sm.h, 0, 1 };
    D3D12_RECT all = { 0, 0, (LONG)g_sm.w, (LONG)g_sm.h };
    static const float kBlack[4] = {};
    cl->SetDescriptorHeaps(1, &g_sm.srvHeap);
    cl->SetGraphicsRootSignature(g_sm.rs);
    cl->SetGraphicsRoot32BitConstants(0, 4, metrics, 0);
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &all);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = SmaaRtv(0);         // 1: edges
    b[0] = Transition(g_sm.edges, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl->ResourceBarrier(1, b);
    cl->ClearRenderTargetView(rtv, kBlack, 0, NULL);
    cl->OMSetRenderTargets(1, &rtv, FALSE, NULL);
    cl->SetPipelineState(g_sm.edgePso[level]);
    cl->SetGraphicsRootDescriptorTable(1, tableA);
    cl->DrawInstanced(3, 1, 0, 0);
    b[0] = Transition(g_sm.edges, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cl->ResourceBarrier(1, b);

    if (!g_x.smaaDebug) {                                 // 2: blending weights
        rtv = SmaaRtv(1);
        b[0] = Transition(g_sm.blend, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl->ResourceBarrier(1, b);
        cl->ClearRenderTargetView(rtv, kBlack, 0, NULL);
        cl->OMSetRenderTargets(1, &rtv, FALSE, NULL);
        cl->SetPipelineState(g_sm.blendPso[level]);
        cl->DrawInstanced(3, 1, 0, 0);
        b[0] = Transition(g_sm.blend, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cl->ResourceBarrier(1, b);
    }

    rtv = SmaaRtv(2);                                     // 3: neighbourhood blend into the back buffer
    D3D12_RENDER_TARGET_VIEW_DESC rd = {};
    rd.Format = g_sm.outFormat[srgbOut ? 0 : 1];
    rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g_sm.dev->CreateRenderTargetView(bb, &rd, rtv);
    cl->OMSetRenderTargets(1, &rtv, FALSE, NULL);
    cl->RSSetScissorRects(1, &out);
    cl->SetPipelineState(g_sm.outPso[srgbOut ? 0 : 1]);
    cl->SetGraphicsRootDescriptorTable(1, g_x.smaaDebug ? tableA : tableB);
    cl->DrawInstanced(3, 1, 0, 0);
}

// Count a run (hud: before the HUD; else at Present) and log the first of each kind.
static void SmaaCount(bool hud) {
    static LONG logged[2];
    InterlockedIncrement(hud ? &g_sm.hudRuns : &g_sm.presentRuns);
    if (!InterlockedExchange(&logged[hud], 1))
        Log(hud ? "graphics: smaa: running before the HUD (on the game's Copy Post)"
                : "graphics: smaa: running at Present on the whole frame, HUD included (no full-screen Copy Post this frame)");
}

// Diagnostic, the first few copy draws: what they drew into, against the swap chain.
static void SmaaCopyDiag(const ListShadow* sh, ID3D12Resource* rt) {
    static LONG n;
    LONG k = InterlockedIncrement(&n);
    if (k > 8) return;
    if (!sh || !sh->hasOm || !sh->nRt) { Log("graphics: smaa: copy draw %ld: no render target recorded on its list", k); return; }
    if (!rt) { Log("graphics: smaa: copy draw %ld: its render-target view (0x%llx) isn't in the RTV map", k, (unsigned long long)sh->rt[0].ptr); return; }
    D3D12_RESOURCE_DESC d = rt->GetDesc();
    int match = -1;
    UINT cur = 99, count = 0;
    if (g_fx.sc3) {
        DXGI_SWAP_CHAIN_DESC sd = {};
        g_fx.sc3->GetDesc(&sd);
        count = sd.BufferCount;
        cur = g_fx.sc3->GetCurrentBackBufferIndex();
        for (UINT i = 0; i < count && i < 8; i++) {
            ID3D12Resource* b = NULL;
            if (SUCCEEDED(g_fx.sc3->GetBuffer(i, __uuidof(ID3D12Resource), (void**)&b))) {
                if (b == rt) match = (int)i;
                b->Release();
            }
        }
    }
    const D3D12_VIEWPORT* v = sh->nVp ? &sh->vp[0] : NULL;
    Log("graphics: smaa: copy draw %ld: into %llux%u format %d, %s%d (current back buffer %u of %u), viewport %.0fx%.0f at %.0f,%.0f, "
        "%u targets, frame %llu (already covered %d)",
        k, (unsigned long long)d.Width, d.Height, (int)d.Format, match >= 0 ? "swap-chain buffer " : "not a swap-chain buffer ", match,
        cur, count, v ? v->Width : 0.0f, v ? v->Height : 0.0f, v ? v->TopLeftX : 0.0f, v ? v->TopLeftY : 0.0f, sh->nRt,
        (unsigned long long)g_sm.frame, g_sm.lastFrame == g_sm.frame);
}

// After the game's simpleCopy draw on list l (DrawHook): if it drew the whole frame,
// smooth the 3D image now, before the HUD goes on top, and put l back.
static void SmaaAfterCopy(ID3D12GraphicsCommandList* l, ListState* s) {
    const ListShadow* sh = s->sh;
    int level = g_sm.level;
    if (g_sm.failed || !level) return;
    ID3D12Resource* rt = sh && sh->hasOm && sh->nRt ? RtvResource(sh->rt[0].ptr) : NULL;
    EnterCriticalSection(&g_fxLock);
    IDXGISwapChain* sc = g_fx.sc;
    if (sc && !g_fx.sc3) sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&g_fx.sc3);
    SmaaCopyDiag(sh, rt);
    ID3D12Resource* bb = NULL;
    if (rt && g_fx.sc3) g_fx.sc3->GetBuffer(g_fx.sc3->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource), (void**)&bb);
    // The engine copies into its own frame target (g_Display+0x1c0, the size of the back
    // buffer; the HUD goes on that next, and Renderer_Present blits it to the swap chain),
    // or, on some paths, into the back buffer itself. The typed frame target can't take
    // an sRGB view, so there the shader does the sRGB encode (same result).
    D3D12_RESOURCE_DESC bd = rt ? rt->GetDesc() : D3D12_RESOURCE_DESC{};
    D3D12_RESOURCE_DESC bbd = bb ? bb->GetDesc() : D3D12_RESOURCE_DESC{};
    bool frameTarget = rt && bb && bd.Width == bbd.Width && bd.Height == bbd.Height && bd.SampleDesc.Count == 1;
    // (Own screen, net_own_screen.cpp: in online co-op each PC shows only its own player,
    // full screen, though the other player's view is still built.)
    // Only a copy that covers the whole target: with own screens the other player's
    // view also copies, into a 1x1 viewport, on a list that may never execute (its GPU
    // work is skipped), so running there would waste two full-screen passes and could
    // record the one-time AreaTex/SearchTex upload into a list that's thrown away.
    // Split-screen views (half each) are left to the Present pass as well.
    D3D12_RECT out = { 0, 0, (LONG)bd.Width, (LONG)bd.Height };
    if (sh && sh->nVp) {
        const D3D12_VIEWPORT& v = sh->vp[0];
        out = { (LONG)v.TopLeftX, (LONG)v.TopLeftY, (LONG)(v.TopLeftX + v.Width), (LONG)(v.TopLeftY + v.Height) };
    }
    bool whole = out.left <= 0 && out.top <= 0 && out.right >= (LONG)bd.Width && out.bottom >= (LONG)bd.Height;
    // At most once per frame. Our passes run with g_inGfxPass set so the hooks don't
    // record them, then the game's state is re-issued.
    if (frameTarget && whole && g_sm.lastFrame != g_sm.frame) {
        bool srgbOut = rt == bb || bd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || bd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
        ID3D12Device* dev = NULL;
        if (g_sm.dev || SUCCEEDED(l->GetDevice(__uuidof(ID3D12Device), (void**)&dev))) {
            if (SmaaPrepare(g_sm.dev ? g_sm.dev : dev, bd, level, srgbOut)) {
                g_inGfxPass = true;
                RunSmaa(l, rt, D3D12_RESOURCE_STATE_RENDER_TARGET, out, level, srgbOut);
                RestoreListState(l, s);
                s->smaaRan = true;
                g_smaaRanAny = true;
                g_inGfxPass = false;
                g_sm.lastFrame = g_sm.frame;
                SmaaCount(true);
            }
            SafeRelease(dev);
        }
    }
    SafeRelease(bb);
    LeaveCriticalSection(&g_fxLock);
}

// Every Present (under g_fxLock): a periodic count of where SMAA ran.
static void SmaaFrameEnd() {
    if (!g_x.smaa) return;
    if (++g_sm.frame % 3600 == 0 && !g_sm.failed) {
        LONG hud = InterlockedExchange(&g_sm.hudRuns, 0), pres = InterlockedExchange(&g_sm.presentRuns, 0);
        Log("graphics: smaa: last 3600 frames: %ld before the HUD, %ld at Present", hud, pres);
    }
}

// The before-HUD route. The view renderer (FUN_1401f6xxx, "Apply Post Processes" /
// "Copy Post") only draws post effects into an offscreen target and then copies them
// to the back buffer when the view has a full-screen effect (FUN_14046f340); otherwise
// the 3D image goes straight to the back buffer and the HUD follows on the same list.
// Saying "yes" for a view that covers the whole display makes the engine do the copy
// every frame, and that copy draw is where SmaaAfterCopy steps in. View 1's effect
// slot can then be empty, so its flag read (FUN_14046f500, slot+0xd8) gets a null check.
static const DWORD RVA_FS_EFFECT_CALL = 0x1f6e32;   // call FUN_14046f340(effects, view)
static const DWORD RVA_FS_EFFECT      = 0x46f340;
static const DWORD RVA_SLOT_FLAG_CALL = 0x1f6e71;   // call FUN_14046f500(slot)
static const DWORD RVA_SLOT_FLAG      = 0x46f500;
static const DWORD RVA_DISPLAY_GET    = 0x2d5fb0;   // returns g_Display + 0x1c0; full-screen rect (x, y, w, h shorts) at +0xac
static const DWORD RVA_VIEWPORTS      = 0x9aac40;   // g_Viewports: x, y, w, h shorts, 0x20 apart
typedef bool (*PFN_FsEffect)(void* effects, uint32_t view);
typedef BYTE (*PFN_SlotFlag)(void* slot);
typedef BYTE* (*PFN_DisplayGet)();

// Replaces the call to FUN_14046f340 ("does this view have a full-screen effect?"): the
// game's answer, or yes for a view (0..3) as big as the display while SMAA is on and its
// hooks are in. Viewport and display rects are 4 shorts: x, y, width, height.
static bool H_FsEffect(void* effects, uint32_t view) {
    bool r = ((PFN_FsEffect)(ExeBase() + RVA_FS_EFFECT))(effects, view);
    if (r || view >= 4 || !g_listHooksOk || g_sm.failed || !g_sm.level) return r;
    BYTE* disp = ((PFN_DisplayGet)(ExeBase() + RVA_DISPLAY_GET))();
    if ((uintptr_t)disp < 0x10000) return r;
    const short* vp = (const short*)(ExeBase() + RVA_VIEWPORTS + view * 0x20);
    const short* full = (const short*)(disp + 0xac);
    bool force = vp[2] == full[2] && vp[3] == full[3];    // this view fills the display (own_screen, single player)
    static LONG seen[4];
    if (!InterlockedExchange(&seen[view], 1))
        Log("graphics: smaa: view %u: viewport %dx%d at %d,%d, display %dx%d -> %s", view, vp[2], vp[3], vp[0], vp[1], full[2], full[3],
            force ? "Copy Post forced" : "left alone");
    return force;
}

// Replaces the call to FUN_14046f500: the same flag read, 0 for an empty slot.
static BYTE H_SlotFlag(void* slot) {
    return slot ? ((PFN_SlotFlag)(ExeBase() + RVA_SLOT_FLAG))(slot) : 0;
}

// smaa=1: arm the before-HUD route. The byte checks are the first instructions of the
// three functions: FUN_14046f340 (movsxd r8,edx; cmp edx,3; ja ...: views 0..3),
// FUN_14046f500 (movzx eax,byte [rcx+0d8h]; ret) and the display getter (mov rax,[rip+..];
// add rax,1c0h; ret). If any differs, SMAA still runs, at Present.
static void PatchSmaa() {
    if (!g_x.smaa) return;
    static const BYTE fs[] = { 0x4c, 0x63, 0xc2, 0x83, 0xfa, 0x03, 0x77, 0x0a };
    static const BYTE sf[] = { 0x0f, 0xb6, 0x81, 0xd8, 0x00, 0x00, 0x00, 0xc3 };
    static const BYTE dg0[] = { 0x48, 0x8b, 0x05 }, dg1[] = { 0x48, 0x05, 0xc0, 0x01, 0x00, 0x00, 0xc3 };
    if (!BytesAt(RVA_FS_EFFECT, fs, sizeof fs) || !BytesAt(RVA_SLOT_FLAG, sf, sizeof sf) ||
        !BytesAt(RVA_DISPLAY_GET, dg0, sizeof dg0) || !BytesAt(RVA_DISPLAY_GET + 7, dg1, sizeof dg1)) {
        Log("graphics: smaa: the before-HUD route doesn't match this build; SMAA runs at Present (HUD included)");
        return;
    }
    if (RedirectCall(RVA_SLOT_FLAG_CALL, RVA_SLOT_FLAG, (void*)H_SlotFlag) &&      // the null check first
        RedirectCall(RVA_FS_EFFECT_CALL, RVA_FS_EFFECT, (void*)H_FsEffect))
        Log("graphics: smaa: before-HUD route armed (full-screen views always go through the engine's Copy Post)");
    else
        Log("graphics: smaa: before-HUD route skipped: call sites don't match this build; SMAA runs at Present (HUD included)");
}

// Our pass at Present (under g_fxLock), recorded on our own command list and submitted
// on the game's queue just before the frame is shown: SMAA on the whole frame if it's on
// and the before-HUD run didn't happen this frame; FXAA only while SMAA is off; then the
// debug menu's overlay. Everything is made lazily, on the first Present that needs it.
static void PresentPass(IDXGISwapChain* sc) {
    if (!g_fx.queue || g_fx.sc != sc || g_fx.baseFailed) return;
    int smaaLevel = g_x.smaa && !g_sm.failed ? (int)g_sm.level : 0;
    bool doSmaa = smaaLevel && g_sm.lastFrame != g_sm.frame;   // not already run before the HUD
    bool doFxaa = g_x.fxaa && !smaaLevel && !g_fx.failed;      // SMAA wins; off in the debug menu lets FXAA back
    GfxOverlayFn overlay = g_overlay;
    if (!doSmaa && !doFxaa && !overlay) return;
    // Set up what's missing; a pass whose setup fails is dropped for this frame.
    if (!g_fx.dev) {
        if (FAILED(g_fx.queue->GetDevice(__uuidof(ID3D12Device), (void**)&g_fx.dev))) { g_fx.baseFailed = true; return; }
    }
    if (!g_fx.sc3 && FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&g_fx.sc3))) {
        Log("graphics: Present pass disabled: no IDXGISwapChain3");
        g_fx.baseFailed = true;
        return;
    }
    if (!g_fx.baseReady && !PresentInitBase()) return;
    if (doFxaa && !g_fx.fxaaReady && !FxaaInitPipeline()) doFxaa = false;
    UINT idx = g_fx.sc3->GetCurrentBackBufferIndex();
    ID3D12Resource* bb = NULL;
    if (FAILED(sc->GetBuffer(idx, __uuidof(ID3D12Resource), (void**)&bb))) return;
    D3D12_RESOURCE_DESC bd = bb->GetDesc();
    DXGI_FORMAT vf = ViewFormat(bd.Format);
    if (bd.SampleDesc.Count != 1) { bb->Release(); return; }
    if (doFxaa && (!FxaaCopyTarget(bd) || !FxaaPso(vf))) doFxaa = false;
    if (doSmaa && !SmaaPrepare(g_fx.dev, bd, smaaLevel, true)) doSmaa = false;
    if (!doSmaa && !doFxaa && !overlay) { bb->Release(); return; }
    UINT w = (UINT)bd.Width, h = bd.Height;

    // Reuse this frame's allocator once the GPU has finished with it (2 s at most).
    UINT f = g_fx.frame++ % kFrames;
    if (g_fx.fence->GetCompletedValue() < g_fx.allocFence[f]) {
        g_fx.fence->SetEventOnCompletion(g_fx.allocFence[f], g_fx.event);
        WaitForSingleObject(g_fx.event, 2000);
    }
    g_fx.alloc[f]->Reset();
    ID3D12GraphicsCommandList* cl = g_fx.list;
    cl->Reset(g_fx.alloc[f], NULL);

    // A render-target view of this frame's back buffer (FXAA and the overlay draw into it).
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_fx.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC rd = {};
    rd.Format = vf;
    rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g_fx.dev->CreateRenderTargetView(bb, &rd, rtv);

    D3D12_RESOURCE_BARRIER b[2];
    if (doSmaa) {
        D3D12_RECT all = { 0, 0, (LONG)w, (LONG)h };
        g_inGfxPass = true;
        RunSmaa(cl, bb, D3D12_RESOURCE_STATE_PRESENT, all, smaaLevel, true);   // leaves bb a render target
        g_inGfxPass = false;
        SmaaCount(false);
    } else if (doFxaa) {
        // Copy the back buffer, then draw FXAA from the copy over it.
        b[0] = Transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        b[1] = Transition(g_fx.copy, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->ResourceBarrier(2, b);
        cl->CopyResource(g_fx.copy, bb);
        b[0] = Transition(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        b[1] = Transition(g_fx.copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cl->ResourceBarrier(2, b);

        cl->SetPipelineState(g_fx.pso);
        cl->SetGraphicsRootSignature(g_fx.rs);
        cl->SetDescriptorHeaps(1, &g_fx.srvHeap);
        cl->SetGraphicsRootDescriptorTable(0, g_fx.srvHeap->GetGPUDescriptorHandleForHeapStart());
        float consts[8] = { 1.0f / g_fx.copyW, 1.0f / g_fx.copyH, g_x.fxaaSubpix, g_x.fxaaEdge, g_x.fxaaEdgeMin, 0, 0, 0 };
        cl->SetGraphicsRoot32BitConstants(1, 8, consts, 0);
        D3D12_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
        D3D12_RECT sr = { 0, 0, (LONG)w, (LONG)h };
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sr);
        cl->OMSetRenderTargets(1, &rtv, FALSE, NULL);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->DrawInstanced(3, 1, 0, 0);
    } else {
        b[0] = Transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl->ResourceBarrier(1, b);
    }
    if (overlay) overlay(g_fx.dev, cl, rtv, vf, w, h);   // sets all of its own state
    // Back to PRESENT, submit, and signal the fence so this allocator's reuse can wait on it.
    b[0] = Transition(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    cl->ResourceBarrier(1, b);
    if (SUCCEEDED(cl->Close())) {
        ID3D12CommandList* lists[] = { cl };
        if (g_execute) g_execute(g_fx.queue, 1, lists);   // not counted by the probe
        else g_fx.queue->ExecuteCommandLists(1, lists);
        g_fx.queue->Signal(g_fx.fence, ++g_fx.fenceValue);
        g_fx.allocFence[f] = g_fx.fenceValue;
    }
    bb->Release();
}

// ------------------------------------------------------------------ DXGI

static bool g_tearingOk;               // the swap chain was created with ALLOW_TEARING

typedef HRESULT (STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateSCHwnd)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
typedef HRESULT (WINAPI* PFN_CreateFactory2)(UINT, REFIID, void**);
static PFN_Present1      g_present1;
static PFN_Present       g_present;
static PFN_ResizeBuffers g_resize;
static PFN_CreateSCHwnd  g_createSC;
static PFN_CreateFactory2 g_createFactory2;

// vrr: present with sync interval 0 and ALLOW_TEARING, so a G-Sync/FreeSync display shows
// each frame when it's ready instead of waiting for vsync. Left alone for test presents,
// in exclusive fullscreen, or if the swap chain couldn't get tearing.
static void TearingFlags(IDXGISwapChain* sc, UINT* sync, UINT* flags) {
    if (!g_x.vrr || !g_tearingOk || (*flags & DXGI_PRESENT_TEST)) return;
    BOOL fs = FALSE;
    if (SUCCEEDED(sc->GetFullscreenState(&fs, NULL)) && fs) return;   // not allowed in exclusive fullscreen
    *sync = 0;
    *flags |= DXGI_PRESENT_ALLOW_TEARING;
}

// IDXGISwapChain1::Present1 (slot 22) and IDXGISwapChain::Present (slot 8), whichever the
// game calls: once per shown frame, the per-frame work of every feature here (our
// Present pass included), then the real Present with the vrr flags. DXGI_PRESENT_TEST
// shows nothing, so it's skipped.
static HRESULT STDMETHODCALLTYPE Present1Hook(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    if (!(flags & DXGI_PRESENT_TEST)) {
        EnterCriticalSection(&g_fxLock);
        ProbeFrame();
        if (g_x.shadowFlashProbe) FlashProbeFrame();
        PresentPass(sc);
        SmaaFrameEnd();
        AuditFrameEnd();
        LeaveCriticalSection(&g_fxLock);
    }
    TearingFlags(sc, &sync, &flags);
    return g_present1(sc, sync, flags, p);
}

static HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) {
        EnterCriticalSection(&g_fxLock);
        ProbeFrame();
        if (g_x.shadowFlashProbe) FlashProbeFrame();
        PresentPass(sc);
        SmaaFrameEnd();
        AuditFrameEnd();
        LeaveCriticalSection(&g_fxLock);
    }
    TearingFlags(sc, &sync, &flags);
    return g_present(sc, sync, flags);
}

// IDXGISwapChain::ResizeBuffers (slot 13): drop our back-buffer-sized resources first;
// the flags must keep ALLOW_TEARING if the swap chain was made with it.
static HRESULT STDMETHODCALLTYPE ResizeBuffersHook(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    EnterCriticalSection(&g_fxLock);
    FxaaWaitIdle();
    SafeRelease(g_fx.copy);                       // re-made at the new size on the next Present
    SmaaReleaseSized();                           // (the game has let the GPU finish to resize)
    LeaveCriticalSection(&g_fxLock);
    if (g_tearingOk) flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;   // must match creation
    return g_resize(sc, n, w, h, fmt, flags);
}

// IDXGIFactory2::CreateSwapChainForHwnd (slot 15): adds ALLOW_TEARING for vrr (if the
// system supports it and the swap chain is flip model), then takes the game's command
// queue (for a D3D12 swap chain the 'dev' argument is the queue it presents on), drops
// resources sized for an old swap chain, and hooks the new one.
static HRESULT STDMETHODCALLTYPE CreateSwapChainHook(IDXGIFactory2* f, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out, IDXGISwapChain1** sc) {
    DXGI_SWAP_CHAIN_DESC1 d = *desc;
    bool tearing = false;
    if (g_x.vrr) {
        IDXGIFactory5* f5 = NULL;
        BOOL allow = FALSE;
        if (SUCCEEDED(f->QueryInterface(__uuidof(IDXGIFactory5), (void**)&f5))) {
            f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow);
            f5->Release();
        }
        bool flip = d.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD || d.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        if (allow && flip) { d.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING; tearing = true; }
        else Log("graphics: vrr unavailable (tearing support %d, flip model %d)", allow, flip);
    }
    HRESULT hr = g_createSC(f, dev, hwnd, &d, fs, out, sc);
    if (FAILED(hr) && tearing) {                  // fall back to the game's own settings
        Log("graphics: swap chain with ALLOW_TEARING failed (0x%08lx); using the game's settings", (unsigned long)hr);
        tearing = false;
        hr = g_createSC(f, dev, hwnd, desc, fs, out, sc);
    }
    if (FAILED(hr) || !*sc) return hr;
    g_tearingOk = tearing;
    if (tearing) Log("graphics: vrr on: presenting without vsync, tearing allowed (cap FPS just below the refresh rate with fps_limit)");
    EnterCriticalSection(&g_fxLock);
    FxaaWaitIdle();
    SafeRelease(g_fx.copy);
    SmaaReleaseSized();
    SafeRelease(g_fx.sc3);                        // re-queried on the next Present (PresentPass)
    if (!g_fx.queue) dev->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&g_fx.queue);
    // ID3D12CommandQueue: ExecuteCommandLists is slot 10.
    if (g_fx.queue && !g_execute) HookVtable(g_fx.queue, 10, (void*)ExecuteHook, (void**)&g_execute);
    g_fx.sc = *sc;
    LeaveCriticalSection(&g_fxLock);
    // IDXGISwapChain: Present 8, ResizeBuffers 13; IDXGISwapChain1: Present1 22.
    HookVtable(*sc, 8, (void*)PresentHook, (void**)&g_present);
    HookVtable(*sc, 13, (void*)ResizeBuffersHook, (void**)&g_resize);
    HookVtable(*sc, 22, (void*)Present1Hook, (void**)&g_present1);
    return hr;
}

// CreateDXGIFactory2 (the exe's import): hook swap-chain creation on the factory the game gets.
static HRESULT WINAPI CreateFactory2Hook(UINT flags, REFIID riid, void** out) {
    HRESULT hr = g_createFactory2(flags, riid, out);
    if (SUCCEEDED(hr) && *out) {
        // Patch the vtable of the interface the game got (it calls through that one).
        // IDXGIFactory2 and later all keep CreateSwapChainForHwnd at slot 15.
        IDXGIFactory2* f2 = NULL;
        if (SUCCEEDED(((IUnknown*)*out)->QueryInterface(__uuidof(IDXGIFactory2), (void**)&f2))) {
            HookVtable(*out, 15, (void*)CreateSwapChainHook, (void**)&g_createSC);
            f2->Release();
        }
    }
    return hr;
}

// ------------------------------------------------------------------ D3D12 device

typedef HRESULT (WINAPI* PFN_CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
static PFN_CreateDevice g_createDevice;

// D3D12CreateDevice (the exe's import): hook the new device's methods for the features
// that are on: samplers (anisotropy), pipeline creation (shader swaps and everything that
// watches pipelines), then the view and command-list hooks (InstallPancakeHooks).
static HRESULT WINAPI CreateDeviceHook(IUnknown* adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void** out) {
    HRESULT hr = g_createDevice(adapter, fl, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        // The interface the game asked for (any ID3D12DeviceN keeps CreateSampler at slot 22,
        // CreateGraphicsPipelineState at 10, and from ID3D12Device2 on CreatePipelineState at 47).
        ID3D12Device* dev = NULL;
        if (SUCCEEDED(((IUnknown*)*out)->QueryInterface(__uuidof(ID3D12Device), (void**)&dev))) {
            if (g_x.anisotropy && HookVtable(*out, 22, (void*)CreateSamplerHook, (void**)&g_createSampler))
                Log("graphics: device created; sampler hook in place");
            dev->Release();
        }
        if (g_x.softShadows || g_x.betterAo || g_x.aoOff || g_x.shadowPsoLog || g_x.shadowPancake || g_x.shadowDebug || g_x.shadowBiasView || g_x.shadowBiasFix[0] || g_x.shadowBiasFix[1] || g_x.shadowBiasFix[2] || g_x.shadowBiasFix[3] || g_x.shadowBiasTan[0] || g_x.shadowBiasTan[1] || g_x.shadowBiasTan[2] || g_x.shadowBiasTanBlend[0] || g_x.shadowBiasTanBlend[1] || g_x.shadowBiasTanBlend[2] || g_x.shadowNoClampVs || g_x.shadowCullNone || g_x.smaa || g_x.shadowDrawAudit || g_x.shadowFlashProbe) {
            bool ok = HookVtable(*out, 10, (void*)CreateGfxPsoHook, (void**)&g_createGfxPso);
            ID3D12Device2* dev2 = NULL;
            if (SUCCEEDED(((IUnknown*)*out)->QueryInterface(__uuidof(ID3D12Device2), (void**)&dev2))) {
                ok &= HookVtable(*out, 47, (void*)CreatePsoHook, (void**)&g_createPso);
                dev2->Release();
            }
            Log(ok ? "graphics: shader swaps: pipeline hooks in place" : "graphics: shader swaps: couldn't hook pipeline creation");
        }
        if (g_x.shadowPancake || g_x.shadowNoClampVs || g_x.shadowCullNone || g_x.shadowPsoLog || g_x.smaa || g_x.shadowDrawAudit || g_x.shadowFlashProbe) {
            ID3D12Device* d12 = NULL;
            if (SUCCEEDED(((IUnknown*)*out)->QueryInterface(__uuidof(ID3D12Device), (void**)&d12))) {
                InstallPancakeHooks(d12);
                d12->Release();
            }
        }
    }
    return hr;
}

// Import address table slots in the exe (d3d12 #101 is D3D12CreateDevice).
static const DWORD RVA_IAT_CREATE_DEVICE  = 0x759b20;
static const DWORD RVA_IAT_SERIALIZE_RS   = 0x759b28;
static const DWORD RVA_IAT_CREATE_FACTORY = 0x759b38;

// Swap-chain hooks (VRR, FXAA, overlay). Safe to call more than once.
// SMAA's Present fallback, the audit's reports and the flash probe's checks also run
// from the Present hooks; the command-queue hook goes in with the swap chain.
static void InstallDxgiHooks() {
    if (g_createFactory2) return;
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    void* cf = dxgi ? (void*)GetProcAddress(dxgi, "CreateDXGIFactory2") : NULL;
    g_createFactory2 = (PFN_CreateFactory2)HookIat(RVA_IAT_CREATE_FACTORY, cf, (void*)CreateFactory2Hook);
    if (!g_createFactory2) Log("graphics: swap-chain hooks skipped: CreateDXGIFactory2 import doesn't match");
    else Log("graphics: swap-chain hooks armed (vrr=%d fxaa=%d overlay=%d)", g_x.vrr, g_x.fxaa, g_overlay != NULL);
}

// Swap the exe's import slots for D3D12CreateDevice, D3D12SerializeRootSignature and
// CreateDXGIFactory2, each only if a feature that's on needs it. At load, from
// GfxExt_Install, before the game makes its device. If a DLL isn't loaded or a slot
// doesn't hold the real function, that hook is skipped with a log line.
static void InstallApiHooks() {
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    if (g_x.anisotropy && (g_x.anisotropy < 2 || g_x.anisotropy > 16)) {
        Log("graphics: anisotropy must be 2..16; ignored");
        g_x.anisotropy = 0;
    }
    if (g_x.anisotropy || g_x.softShadows || g_x.betterAo || g_x.aoOff || g_x.shadowPsoLog || g_x.shadowPancake || g_x.shadowDebug || g_x.shadowBiasView || g_x.shadowBiasFix[0] || g_x.shadowBiasFix[1] || g_x.shadowBiasFix[2] || g_x.shadowBiasFix[3] || g_x.shadowBiasTan[0] || g_x.shadowBiasTan[1] || g_x.shadowBiasTan[2] || g_x.shadowBiasTanBlend[0] || g_x.shadowBiasTanBlend[1] || g_x.shadowBiasTanBlend[2] || g_x.shadowNoClampVs || g_x.shadowCullNone || g_x.smaa || g_x.shadowDrawAudit || g_x.shadowFlashProbe) {
        void* cd = d3d12 ? (void*)GetProcAddress(d3d12, "D3D12CreateDevice") : NULL;
        g_createDevice = (PFN_CreateDevice)HookIat(RVA_IAT_CREATE_DEVICE, cd, (void*)CreateDeviceHook);
        if (!g_createDevice) Log("graphics: D3D12CreateDevice import doesn't match; anisotropy and soft shadows off");
    }
    if (g_x.anisotropy) {
        void* sr = d3d12 ? (void*)GetProcAddress(d3d12, "D3D12SerializeRootSignature") : NULL;
        g_serialize = (PFN_Serialize)HookIat(RVA_IAT_SERIALIZE_RS, sr, (void*)SerializeHook);
        if (!g_serialize) Log("graphics: anisotropy: D3D12SerializeRootSignature import doesn't match; static samplers unchanged");
    }
    if (g_x.vrr || g_x.fxaa || g_x.smaa || g_x.shadowDrawAudit) InstallDxgiHooks();
}

// ------------------------------------------------------------------ entry

// Once, from whichever of GfxExt_Install / GfxExt_InstallOverlay runs first: the log
// function and the Present lock.
static void InitShared(GfxLogFn log) {
    static bool done;
    if (done) return;
    done = true;
    if (!Log) Log = log;
    InitializeCriticalSection(&g_fxLock);
}

// From ApplyGraphicsPatches (graphics_patches.cpp) at load, when [graphics] enabled=1:
// read this file's keys, log them, patch the exe, then hook the graphics API imports.
void GfxExt_Install(const wchar_t* iniPath, GfxLogFn log) {
    Log = log;
    InitShared(log);
    LoadExtConfig(iniPath);
    g_sm.level = g_x.smaa ? g_x.smaaPreset : 0;
    Log("graphics: anisotropy=%d vrr=%d fxaa=%d volumetric_res=%d shadow_res=%d soft_shadows=%d better_ao=%d ao_off=%d "
        "smaa=%d smaa_preset=%s smaa_debug=%d shadow_pancake=%d shadow_noclamp_vs=%d shadow_cull_none=%d shadow_draw_audit=%d shadow_pso_log=%d shadow_debug=%d shadow_bias_fix=%s shadow_cascades=%d shadow_far=%.1f shadow_split=%.2f light_override=\"%s\"",
        g_x.anisotropy, g_x.vrr, g_x.fxaa, g_x.volumetricRes, g_x.shadowRes, g_x.softShadows, g_x.betterAo, g_x.aoOff,
        g_x.smaa, kSmaaPresetNames[g_x.smaaPreset], g_x.smaaDebug, g_x.shadowPancake, g_x.shadowNoClampVs, g_x.shadowCullNone, g_x.shadowDrawAudit, g_x.shadowPsoLog, g_x.shadowDebug ? 1 : g_x.shadowBiasView ? 2 : 0,
        g_x.shadowBiasFix[0] ? "cap 0.25" : g_x.shadowBiasFix[1] ? "cap 0.1" : g_x.shadowBiasFix[2] ? "cap 0.05" : g_x.shadowBiasFix[3] ? "cap 0.02" : g_x.shadowBiasTan[0] ? "max tan 5" :
        g_x.shadowBiasTan[1] ? "max tan 8" : g_x.shadowBiasTan[2] ? "max tan 12" : g_x.shadowBiasTanBlend[0] ? "max tan 5 + cascade blend" :
        g_x.shadowBiasTanBlend[1] ? "max tan 8 + cascade blend" : g_x.shadowBiasTanBlend[2] ? "max tan 12 + cascade blend" : "off",
        g_x.shadowCascades, g_x.shadowFar, g_x.shadowSplit, g_x.lightOverride);
    PatchVolumetric();
    PatchShadowRes();
    PatchShadows();
    PatchLightOverrides();
    PatchSmaa();
    InstallApiHooks();
}

// For debug_menu.cpp: draw an overlay at Present (after FXAA). Works whether or
// not [graphics] is enabled; installs the swap-chain hooks if needed.
void GfxExt_InstallOverlay(GfxOverlayFn fn, GfxLogFn log) {
    InitShared(log);
    g_overlay = fn;
    InstallDxgiHooks();
}

// For debug_menu.cpp: the FXAA switch, read and set live.
bool GfxExt_GetFxaa() { return g_x.fxaa; }

// For debug_menu.cpp: SMAA 0 off, 1 low .. 4 ultra; -1 = not available (smaa=0 in the
// ini, so the hooks it needs weren't installed at start-up, or it failed).
int GfxExt_GetSmaa() { return g_x.smaa && !g_sm.failed ? (int)g_sm.level : -1; }
// Takes effect from the next SMAA run; each preset's pipelines are made the first time it's used.
void GfxExt_SetSmaa(int level) {
    if (!g_x.smaa || g_sm.failed || level < 0 || level > 4) return;
    if (InterlockedExchange(&g_sm.level, level) != level) Log("graphics: smaa: %s (debug menu)", kSmaaPresetNames[level]);
}
// Turning FXAA on needs the swap-chain hooks, which only go in at start-up.
void GfxExt_SetFxaa(bool on) {
    if (on && !g_createFactory2) return;           // swap chain isn't hooked: can't turn it on live
    if (on && g_x.fxaaEdge <= 0.0f) {              // [graphics] wasn't loaded: the template's defaults
        g_x.fxaaSubpix = 0.75f;
        g_x.fxaaEdge = 0.125f;
        g_x.fxaaEdgeMin = 0.0312f;
    }
    g_x.fxaa = on;
}
