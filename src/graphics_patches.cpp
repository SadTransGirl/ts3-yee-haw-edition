// graphics_patches.cpp -- patches to the game's own render code (frame limit, cull sizes,
// half-res targets, object fade, world/actor LOD, free-cam culling, shadow cull/zones/near plane).
// The code-patch helpers they use are in patch_util.cpp; the D3D12 work is in gfx_d3d12.cpp.
//
// Entry points:
//   ApplyGraphicsPatches  DllMain, when [graphics] enabled=1: every static patch below, then
//                         GfxExt_Install (gfx_d3d12.cpp).
//   InstallFreeCamCull    DllMain, when the debug menu is on (after ApplyGraphicsPatches).
//   Gfx_FreeCamCulling    the debug menu, every frame, on the game thread.
//   GfxNewFrame           the per-frame poll (NoteCall in harness_core.cpp), once the actor
//                         LOD scaling is verified (g_lodReady).
//   OpenGraphicsLog       DllMain: harness\graphics.log, when no run log is open.
// LOD = level of detail: a model swaps to simpler versions as it gets farther away, at
// distances the game's data sets; "cull" here means not drawing an object at all.

#include "proxy_shared.h"

bool g_lodReady;           // actor LOD scaling verified and waiting for the LOD object

// ------------------------------------------------------------------ graphics patches
//
// Engine-side quality settings from the [graphics] ini section, found by
// decompiling the game. Every patch first checks both the value it expects and
// the instruction that reads it (RIP-relative operand decoded back to the
// address), and is skipped with a log line if either differs, so a game update
// can't make it write to the wrong place. Static patches are applied in
// DllMain, before the game creates its render targets. The actor LOD object
// only exists after a level loads, so that one is scaled from the poll hook.

GfxConfig g_gfx;

// Addresses for this game build (RVAs from the image base). Each was found in the decomp
// session; add 0x140000000 for the address the decompiler shows. A "_READ"/"_LEA"/"_STORE"
// entry is the instruction that uses the value above it, checked before the value is changed.
static const DWORD RVA_FRAME_LIMIT_60   = 0x7ee610;  // double 1/60, read only by FrameLimiter_Wait
static const DWORD RVA_FRAME_LIMIT_READ = 0x53868c;  //   movsd xmm1, [rip+d]
static const DWORD RVA_CULL_POP         = 0x94e348;  // float 15, copied to the main-view cull every frame
static const DWORD RVA_CULL_FADE        = 0x94e34c;  // float 25
static const DWORD RVA_CULL_POP_READ    = 0x1e880a;  //   movss xmm0, [rip+d] in Shadow_SetupCascades
static const DWORD RVA_CULL_FADE_READ   = 0x1e881a;  //   movss xmm1, [rip+d]
static const DWORD RVA_SHADOW_CULL      = 0x94e4c8;  // float[4] { 5, 15, 25, 0 }, shadow cull per cascade
static const DWORD RVA_SHADOW_CULL_LEA  = 0x1e8962;  //   lea rax, [rip+d] in Shadow_SetupCascades
static const DWORD RVA_ACTOR_LOD_PTR    = 0xb29a50;  // CCameraLOD* built from actorlod.var
static const DWORD RVA_ACTOR_LOD_STORE  = 0x4cef33;  //   mov [rip+d], rcx in its loader
static const DWORD RVA_REFL_SHR_W       = 0x1f3893;  // RT_ReflectionPlane_Setup: width >> 1
static const DWORD RVA_REFL_SHR_H       = 0x1f389a;  //   height >> 1
static const DWORD RVA_GLOW_SHR_W       = 0x1f2fd0;  // RT_Glow_Setup: width >> 1
static const DWORD RVA_GLOW_SHR_H       = 0x1f2fdd;  //   height >> 1
// RT_Glow_Setup binds Glow0/Glow1 with the "Depth 1/2" depth buffer
// (g_RenderTargets+0x700), and "Draw Glow Objects" clears that buffer (pass
// clear flags 0x1f). Making only the glow targets full size trips the engine's
// size check (RT_ValidateBindings, int3 at 0x1402d3bdf). Borrowing the main
// scene depth instead would get it cleared mid-frame. So RT_DepthHalf_Setup's
// 0x700 depth and its 0x7e0 colour partner (the only other pass using 0x700)
// are made full size too, and glow keeps its own buffer as designed. The
// reflection plane makes its own depth buffer from the same size, so it
// needs nothing extra.
static const DWORD RVA_GLOW_DEPTH0      = 0x1f3041;  //   add rdx, 0x700 (Glow0): checked, not changed
static const DWORD RVA_GLOW_DEPTH1      = 0x1f3078;  //   add rdx, 0x700 (Glow1): checked, not changed
static const DWORD RVA_HALFD_SHR_W      = 0x1f2c6b;  // RT_DepthHalf_Setup: 0x700 depth width >> 1
static const DWORD RVA_HALFD_SHR_H      = 0x1f2c72;  //   height >> 1
static const DWORD RVA_HALFD_ADD        = 0x1f2c80;  //   add rcx, 0x700 (creates it): checked
static const DWORD RVA_HALFC_SHR_W      = 0x1f2cdd;  // RT_DepthHalf_Setup: 0x7e0 colour width >> 1
static const DWORD RVA_HALFC_SHR_H      = 0x1f2ce4;  //   height >> 1
static const DWORD RVA_HALFC_ADD        = 0x1f2cf2;  //   add rcx, 0x7e0 (creates it): checked
static const DWORD RVA_VIEWDEPTH_CALL   = 0x2e5f48;  // call Cull_ObjectViewDepth, its only caller (Cull_ContributionTest)
static const DWORD RVA_VIEWDEPTH_FN     = 0x2e4c90;  // Cull_ObjectViewDepth: depth used for per-model fade distances
static const DWORD RVA_LODSWAP_VT10     = 0x8060b0;  // LODEntitySwapper::vftable slot 10 (reads LODDistance)
static const DWORD RVA_LODSWAP_VF10     = 0x65fed0;  //   LODEntitySwapper::vf10

// fps_limit: the game's frame limiter waits for a frame time it reads from a constant (1/60 s).
// That constant is replaced by 1/fps_limit, or 0 (no wait) for a negative fps_limit.
static void PatchFrameLimit() {
    if (g_gfx.fpsLimit == 0) return;
    if (g_cfg.mode != MODE_PASSTHROUGH && g_cfg.fixedTimestep) {
        Log("graphics: fps_limit ignored: with fixed_timestep on, the game's own limiter is what keeps "
            "the fixed clock in real time");
        return;
    }
    // Check the reading instruction (movsd xmm1, [rip+d]), that its operand points at the
    // constant, and that the constant is still 1/60 before writing it.
    static const BYTE kMovsdXmm1[] = { 0xF2, 0x0F, 0x10, 0x0D };
    const double expect = 1.0 / 60.0;
    if (!BytesAt(RVA_FRAME_LIMIT_READ, kMovsdXmm1, sizeof kMovsdXmm1) ||
        !RipRefersTo(RVA_FRAME_LIMIT_READ, 4, 8, RVA_FRAME_LIMIT_60) ||
        !BytesAt(RVA_FRAME_LIMIT_60, &expect, sizeof expect)) {
        Log("graphics: fps_limit skipped: frame limiter code doesn't match this build");
        return;
    }
    double v = g_gfx.fpsLimit > 0 ? 1.0 / g_gfx.fpsLimit : 0.0;
    if (WriteMem(RVA_FRAME_LIMIT_60, &v, sizeof v, false))
        Log("graphics: frame limiter %s (vsync still applies; the menu's 30 FPS cap is unchanged)",
            g_gfx.fpsLimit > 0 ? "raised" : "disabled");
}

// cull_pop / cull_fade and shadow_cull_*: the game skips objects that look smaller than a size
// threshold (pop) and fades them in up to a second one (fade); shadow casters have one
// threshold per cascade (the sun's shadow is drawn as cascades: nested shadow maps, the
// smallest and sharpest around the camera). These overwrite the constant thresholds, after
// checking the code that reads them; a value the ini leaves out keeps the game's.
static void PatchCull() {
    if (g_gfx.hasCullPop || g_gfx.hasCullFade) {
        static const BYTE kMovssXmm0[] = { 0xF3, 0x0F, 0x10, 0x05 };
        static const BYTE kMovssXmm1[] = { 0xF3, 0x0F, 0x10, 0x0D };
        const float expect[2] = { 15.0f, 25.0f };
        if (!BytesAt(RVA_CULL_POP_READ, kMovssXmm0, 4) || !RipRefersTo(RVA_CULL_POP_READ, 4, 8, RVA_CULL_POP) ||
            !BytesAt(RVA_CULL_FADE_READ, kMovssXmm1, 4) || !RipRefersTo(RVA_CULL_FADE_READ, 4, 8, RVA_CULL_FADE) ||
            !BytesAt(RVA_CULL_POP, expect, sizeof expect)) {
            Log("graphics: cull_pop/cull_fade skipped: code doesn't match this build");
        } else {
            float v[2] = { g_gfx.hasCullPop ? (float)g_gfx.cullPop : expect[0],
                           g_gfx.hasCullFade ? (float)g_gfx.cullFade : expect[1] };
            if (v[1] < v[0]) v[1] = v[0];   // fade below pop would make objects flicker
            if (WriteMem(RVA_CULL_POP, v, sizeof v, false))
                Log("graphics: main-view small-object cull pop/fade %.1f/%.1f (game: 15/25)", v[0], v[1]);
        }
    }
    if (g_gfx.hasShadowCull[0] || g_gfx.hasShadowCull[1] || g_gfx.hasShadowCull[2]) {
        static const BYTE kLeaRax[] = { 0x48, 0x8D, 0x05 };
        const float expect[3] = { 5.0f, 15.0f, 25.0f };
        if (!BytesAt(RVA_SHADOW_CULL_LEA, kLeaRax, 3) || !RipRefersTo(RVA_SHADOW_CULL_LEA, 3, 7, RVA_SHADOW_CULL) ||
            !BytesAt(RVA_SHADOW_CULL, expect, sizeof expect)) {
            Log("graphics: shadow_cull skipped: code doesn't match this build");
        } else {
            float v[3];
            for (int i = 0; i < 3; i++) v[i] = g_gfx.hasShadowCull[i] ? (float)g_gfx.shadowCull[i] : expect[i];
            if (WriteMem(RVA_SHADOW_CULL, v, sizeof v, false))
                Log("graphics: shadow-caster cull per cascade %.1f/%.1f/%.1f (game: 5/15/25; ignored with --hiresshadows)",
                    v[0], v[1], v[2]);
        }
    }
}

// Remove a "shr reg, 1" so a render target is created at full instead of
// half resolution. Both the width and height sites are checked before either
// is patched; sig includes the following instruction for extra certainty.
static void PatchHalfRes(const char* what, DWORD rvaW, const BYTE* sigW, size_t sigWLen,
                         DWORD rvaH, const BYTE* sigH, size_t sigHLen, size_t shrLen) {
    if (!BytesAt(rvaW, sigW, sigWLen) || !BytesAt(rvaH, sigH, sigHLen)) {
        Log("graphics: full-res %s skipped: code doesn't match this build", what);
        return;
    }
    static const BYTE kNops[4] = { 0x90, 0x90, 0x90, 0x90 };
    if (WriteMem(rvaW, kNops, shrLen, true) && WriteMem(rvaH, kNops, shrLen, true))
        Log("graphics: %s render target now full resolution", what);
}

// full_res_glow: glow, its "Depth 1/2" depth buffer and that buffer's colour partner at full
// resolution (see RVA_GLOW_DEPTH0 above for why all three). Every site is checked, including
// the unchanged "add 0x700" ones that prove the buffers are the expected ones, before any write.
static void PatchGlow() {
    static const BYTE w[] = { 0xD1, 0xE8, 0x66, 0x89, 0x44, 0x24, 0x44 };   // shr eax,1 ; mov [rsp+44h],ax
    static const BYTE h[] = { 0xD1, 0xE8, 0x66, 0x89 };                     // shr eax,1 ; (mov ...)
    static const BYTE addRdx700[] = { 0x48, 0x81, 0xC2, 0x00, 0x07, 0x00, 0x00 };  // add rdx, 0x700
    static const BYTE dW[] = { 0x66, 0xD1, 0xE9, 0x66, 0x89, 0x4D, 0xDB };   // shr cx,1 ; mov [rbp-25h],cx
    static const BYTE dH[] = { 0x66, 0xD1, 0xEA, 0x66, 0x89, 0x55, 0xDD };   // shr dx,1 ; mov [rbp-23h],dx
    static const BYTE dAdd[] = { 0x48, 0x81, 0xC1, 0x00, 0x07, 0x00, 0x00 }; // add rcx, 0x700
    static const BYTE cW[] = { 0x66, 0xD1, 0xE9, 0x66, 0x89, 0x4D, 0xFB };   // shr cx,1 ; mov [rbp-5],cx
    static const BYTE cH[] = { 0x66, 0xD1, 0xEA, 0x66, 0x89, 0x55, 0xFD };   // shr dx,1 ; mov [rbp-3],dx
    static const BYTE cAdd[] = { 0x48, 0x81, 0xC1, 0xE0, 0x07, 0x00, 0x00 }; // add rcx, 0x7e0
    if (!BytesAt(RVA_GLOW_SHR_W, w, sizeof w) || !BytesAt(RVA_GLOW_SHR_H, h, sizeof h) ||
        !BytesAt(RVA_GLOW_DEPTH0, addRdx700, sizeof addRdx700) || !BytesAt(RVA_GLOW_DEPTH1, addRdx700, sizeof addRdx700) ||
        !BytesAt(RVA_HALFD_SHR_W, dW, sizeof dW) || !BytesAt(RVA_HALFD_SHR_H, dH, sizeof dH) ||
        !BytesAt(RVA_HALFD_ADD, dAdd, sizeof dAdd) ||
        !BytesAt(RVA_HALFC_SHR_W, cW, sizeof cW) || !BytesAt(RVA_HALFC_SHR_H, cH, sizeof cH) ||
        !BytesAt(RVA_HALFC_ADD, cAdd, sizeof cAdd)) {
        Log("graphics: full-res glow skipped: code doesn't match this build");
        return;
    }
    static const BYTE kNops[3] = { 0x90, 0x90, 0x90 };
    if (WriteMem(RVA_HALFD_SHR_W, kNops, 3, true) && WriteMem(RVA_HALFD_SHR_H, kNops, 3, true) &&
        WriteMem(RVA_HALFC_SHR_W, kNops, 3, true) && WriteMem(RVA_HALFC_SHR_H, kNops, 3, true) &&
        WriteMem(RVA_GLOW_SHR_W, kNops, 2, true) && WriteMem(RVA_GLOW_SHR_H, kNops, 2, true))
        Log("graphics: glow and its depth buffer (the \"Depth 1/2\" pass) now full resolution");
}

// Per-model fade-out distances (model data) are checked against the depth this
// returns. Shrinking that depth by 1/k makes every model fade k times farther
// away. Only the main view uses it; shadows and the other culling are separate.
typedef float (*PFN_ViewDepth)(void* view, const float* box);
static PFN_ViewDepth g_viewDepth;
static float         g_invObjectFade = 1.0f;

// Replaces the one call to Cull_ObjectViewDepth: the game's own depth, times 1/k.
static float ScaledViewDepth(void* view, const float* box) {
    return g_viewDepth(view, box) * g_invObjectFade;
}

// object_fade_scale = k: check Cull_ObjectViewDepth's first bytes, then redirect its only call
// to ScaledViewDepth.
static void PatchObjectFade() {
    double k = g_gfx.objectFadeScale;
    if (k == 1.0) return;
    if (k < 0.25 || k > 16.0) { Log("graphics: object_fade_scale %.2f out of range (0.25..16); ignored", k); return; }
    static const BYTE prologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20 };
    if (!BytesAt(RVA_VIEWDEPTH_FN, prologue, sizeof prologue)) {
        Log("graphics: object_fade_scale skipped: code doesn't match this build");
        return;
    }
    g_viewDepth = (PFN_ViewDepth)(ExeBase() + RVA_VIEWDEPTH_FN);
    g_invObjectFade = (float)(1.0 / k);
    if (RedirectCall(RVA_VIEWDEPTH_CALL, RVA_VIEWDEPTH_FN, (void*)ScaledViewDepth))
        Log("graphics: per-model fade-out distances x%.2f", k);
    else
        Log("graphics: object_fade_scale skipped: couldn't redirect the call");
}

// World chunks swap between low and high detail models at LODDistance, read
// from level data by LODEntitySwapper::vf10. Wrap that vtable slot and scale
// the value it stores at +0x3c. Only scale when the original actually wrote a
// new value, so a repeat call can't compound the factor.
typedef void (*PFN_LodSwapInit)(BYTE* self);
static PFN_LodSwapInit g_lodSwapInit;
static float           g_worldLod = 1.0f;

// Scale LODDistance (+0x3c) if vf10 changed it. ScaleLodSwap and ReadLodSwap guard their
// memory access (__try) in case self isn't the object expected; a bad read gives 0.
static void ScaleLodSwap(BYTE* self, float before) {
    __try {
        float* d = (float*)(self + 0x3c);
        if (*d != before && *d > 0.0f) *d *= g_worldLod;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static float ReadLodSwap(BYTE* self) {
    __try {
        return *(float*)(self + 0x3c);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.0f;
    }
}

// The hook in vf10's vtable slot: note LODDistance, run the original, scale what it wrote.
static void LodSwapInit(BYTE* self) {
    float before = ReadLodSwap(self);
    g_lodSwapInit(self);
    ScaleLodSwap(self, before);
}

// world_lod_scale = k: check that the vtable slot still points at vf10, then point it at
// LodSwapInit. (A vtable is a class's table of virtual-function pointers: every call to
// vf10 on any LODEntitySwapper goes through this one slot.)
static void PatchWorldLod() {
    double k = g_gfx.worldLodScale;
    if (k == 1.0) return;
    if (k < 0.25 || k > 8.0) { Log("graphics: world_lod_scale %.2f out of range (0.25..8); ignored", k); return; }
    void* expect = ExeBase() + RVA_LODSWAP_VF10;
    if (!BytesAt(RVA_LODSWAP_VT10, &expect, sizeof expect)) {
        Log("graphics: world_lod_scale skipped: LODEntitySwapper vtable doesn't match this build");
        return;
    }
    g_lodSwapInit = (PFN_LodSwapInit)expect;
    g_worldLod = (float)k;
    void* hook = (void*)LodSwapInit;
    if (WriteMem(RVA_LODSWAP_VT10, &hook, sizeof hook, false))
        Log("graphics: world-chunk LOD swap distances x%.2f", k);
}

// Debug menu free camera, offline (debug_menu.cpp's CullFollows): everything it looks at
// is drawn however small or far. While it's on, the small-object culls (main view and
// shadows) are 0, as with the launcher's "Keep small objects far away", and the
// per-model fade-outs never fire (view depth x0); turning it off puts back whatever was
// there. The far plane is debug_menu.cpp's. The view-depth redirect goes in at start-up
// even with object_fade_scale 1, where it multiplies by exactly 1.
static bool  g_fcCullOk, g_fcShadowOk, g_fcFadeOk;
// From DllMain: check which of the three can be switched (the same code checks as the
// [graphics] patches) and put the view-depth redirect in if object_fade_scale didn't.
void InstallFreeCamCull() {
    static const BYTE kMovssXmm0[] = { 0xF3, 0x0F, 0x10, 0x05 };
    static const BYTE kMovssXmm1[] = { 0xF3, 0x0F, 0x10, 0x0D };
    static const BYTE kLeaRax[] = { 0x48, 0x8D, 0x05 };
    g_fcCullOk = BytesAt(RVA_CULL_POP_READ, kMovssXmm0, 4) && RipRefersTo(RVA_CULL_POP_READ, 4, 8, RVA_CULL_POP) &&
                 BytesAt(RVA_CULL_FADE_READ, kMovssXmm1, 4) && RipRefersTo(RVA_CULL_FADE_READ, 4, 8, RVA_CULL_FADE);
    g_fcShadowOk = BytesAt(RVA_SHADOW_CULL_LEA, kLeaRax, 3) && RipRefersTo(RVA_SHADOW_CULL_LEA, 3, 7, RVA_SHADOW_CULL);
    if (g_viewDepth) {
        g_fcFadeOk = true;                             // object_fade_scale already put it in
    } else {
        static const BYTE prologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20 };
        if (BytesAt(RVA_VIEWDEPTH_FN, prologue, sizeof prologue)) {
            g_viewDepth = (PFN_ViewDepth)(ExeBase() + RVA_VIEWDEPTH_FN);
            g_invObjectFade = 1.0f;
            g_fcFadeOk = RedirectCall(RVA_VIEWDEPTH_CALL, RVA_VIEWDEPTH_FN, (void*)ScaledViewDepth);
            if (!g_fcFadeOk) g_viewDepth = NULL;
        }
    }
    Log("debug menu: free camera draws everything offline: small-object cull %s, shadow cull %s, fade-outs %s",
        g_fcCullOk ? "yes" : "NO (code doesn't match)", g_fcShadowOk ? "yes" : "NO (code doesn't match)",
        g_fcFadeOk ? "yes" : "NO (code doesn't match)");
}
// Called by the debug menu every frame with whether the free camera wants it (on the game thread).
void Gfx_FreeCamCulling(bool off) {
    static bool isOff;
    static float savedCull[2], savedShadow[3], savedFade;
    if (off == isOff) return;
    isOff = off;
    BYTE* b = ExeBase();
    if (g_fcCullOk) {
        if (off) { memcpy(savedCull, b + RVA_CULL_POP, sizeof savedCull); const float z[2] = {}; WriteMem(RVA_CULL_POP, z, sizeof z, false); }
        else WriteMem(RVA_CULL_POP, savedCull, sizeof savedCull, false);
    }
    if (g_fcShadowOk) {
        if (off) { memcpy(savedShadow, b + RVA_SHADOW_CULL, sizeof savedShadow); const float z[3] = {}; WriteMem(RVA_SHADOW_CULL, z, sizeof z, false); }
        else WriteMem(RVA_SHADOW_CULL, savedShadow, sizeof savedShadow, false);
    }
    if (g_fcFadeOk) {
        if (off) { savedFade = g_invObjectFade; g_invObjectFade = 0.0f; }
        else g_invObjectFade = savedFade;
    }
    Log("debug menu: free camera culling %s", off ? "off: everything draws, however small or far"
                                                   : "back to normal (small-object culls and fade-outs restored)");
}

// lod_scale: actor (character) LOD distances. The LOD object doesn't exist yet at start-up, so
// this only checks the loader instruction that stores its pointer; GfxNewFrame does the
// scaling once it exists (g_lodReady).
static void VerifyActorLod() {
    if (g_gfx.lodScale == 1.0) return;
    if (g_gfx.lodScale < 0.25 || g_gfx.lodScale > 8.0) {
        Log("graphics: lod_scale %.2f out of range (0.25..8); ignored", g_gfx.lodScale);
        return;
    }
    static const BYTE kMovRipRcx[] = { 0x48, 0x89, 0x0D };
    if (!BytesAt(RVA_ACTOR_LOD_STORE, kMovRipRcx, 3) || !RipRefersTo(RVA_ACTOR_LOD_STORE, 3, 7, RVA_ACTOR_LOD_PTR)) {
        Log("graphics: lod_scale skipped: actor LOD loader doesn't match this build");
        return;
    }
    g_lodReady = true;
    Log("graphics: actor LOD distances will be scaled x%.2f once the LOD settings load", g_gfx.lodScale);
}

// Called once per polled frame on the game's main thread. The LOD object holds
// six distances already multiplied by actorlod.var's LODScale (+0x0c..+0x20)
// and their squares (+0x24..+0x38). Scale them whenever they aren't the values
// we last wrote, i.e. after every (re)load.
static uintptr_t g_lodObj;
static float     g_lodWritten;
static bool      g_lodBadLogged;

// Scale obj's six distances by k (and their squares by k*k), unless they're the values we
// last wrote or don't look freshly loaded. Returns true when it scaled, with the nearest
// distance before and after.
static bool ScaleLodObject(BYTE* obj, float k, float* before, float* after) {
    __try {
        float* lin = (float*)(obj + 0x0c);
        float* sq  = (float*)(obj + 0x24);
        if ((uintptr_t)obj == g_lodObj && lin[0] == g_lodWritten) return false;   // already ours
        // Freshly loaded values satisfy sq == lin^2; anything else isn't the object we expect.
        if (!(lin[0] > 0.0f) || fabsf(sq[0] - lin[0] * lin[0]) > 1e-3f * (1.0f + sq[0])) {
            if (!g_lodBadLogged) { g_lodBadLogged = true; Log("graphics: actor LOD object looks wrong; not scaling it"); }
            return false;
        }
        *before = lin[0];
        for (int i = 0; i < 6; i++) { lin[i] *= k; sq[i] *= k * k; }
        g_lodObj = (uintptr_t)obj;
        g_lodWritten = lin[0];
        *after = lin[0];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The per-frame part of lod_scale (see the comment above g_lodObj): scale the actor LOD
// object once it exists, and again after each reload.
void GfxNewFrame() {
    BYTE* obj = *(BYTE**)(ExeBase() + RVA_ACTOR_LOD_PTR);
    if (!obj) return;
    float before, after;
    if (ScaleLodObject(obj, (float)g_gfx.lodScale, &before, &after))
        Log("graphics: actor LOD distances x%.2f (nearest band %.1f -> %.1f)", g_gfx.lodScale, before, after);
}

// harness\graphics.log, for when no run log is open (passthrough mode has none). Called from
// DllMain when the graphics patches or the debug menu need somewhere to log.
void OpenGraphicsLog() {
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\harness", g_exeDir);
    CreateDirectoryW(path, NULL);
    wcscat_s(path, L"\\graphics.log");
    g_log = _wfsopen(path, L"w", _SH_DENYWR);   // others may read it while the game runs
}

// Stock bug (re\STOCK_BUGS.md, "Pancaking result"): which shadow casters a cascade
// draws depends on visibility ZONES. Shadow_SetupCascades -> FUN_1402d5d40 (call
// at 0x1401e8a47) -> FUN_1402d1560 -> FUN_1402e4130(cullData) (call at 0x1402d172a)
// picks the zones to frustum-test from the cascade camera cam = *(cullData+0x18):
// a linked camera's zone list (cam+0x228), else the zones around cam's position
// (cam+0x234 set), else all of them. For a cascade that follows the player's
// camera, whole groups of casters (the room's window over the board-game board,
// a cliff overhang) drop in and out as the view moves: the wedges and stripes.
// shadow_all_zones=1: while a cascade is being culled, that camera's two fields are
// cleared around the call, so it takes the "all zones" branch. Per camera, not the
// global switch (DAT_140b22eb1): other cameras and threads are untouched. The
// other two callers of FUN_1402d5d40 are other passes and are left alone.
// Render-only: which casters reach the shadow map.
typedef void* (*PFN_ShadowCull)(void* camNode, void* a2, void* a3, void* a4);
typedef void  (*PFN_ZoneSelect)(void* cullData);
static PFN_ShadowCull g_shadowCull;
static PFN_ZoneSelect g_zoneSelect;
static thread_local bool g_inShadowCull;

// shadow_no_contrib=1 (decomp, re\STOCK_BUGS.md): the cascade cull also drops casters
// by projected size (Cull_ContributionTest 0x1402e5db0, and FUN_1402d1560's per-view
// fade weight), both gated by the byte at 0x140953b21 (1 by default). An ortho
// cascade's "projected size" is the object's area over the cascade rect, and cascade
// 0's rect follows the view slice, so small casters near a threshold pop in and out
// of the shadow map as the player moves or turns: a camera-following square of
// missing shadows. The byte is cleared only around the cascade cull; the main view
// keeps its size culling.
static const DWORD RVA_CONTRIB_FLAG = 0x953b21;

// shadow_caster_log=1: each cascade's caster count (FUN_1402d5d40's result header:
// [0] flags, [1] entries) logged when it changes, with a millisecond time to line up
// with a recording.
// Up to 8 cascade cameras are told apart by their node pointer; log lines stop after 3000.
static void ShadowCasterLog(void* camNode, void* header) {
    static void* cams[8];
    static uint32_t last[8];
    static LONG lines;
    uint32_t n = 0;
    __try { if (!header) return; n = ((uint32_t*)header)[1]; } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    int i = 0;
    while (i < 8 && cams[i] && cams[i] != camNode) i++;
    if (i == 8) return;
    if (!cams[i]) { cams[i] = camNode; last[i] = ~0u; }
    if (n == last[i]) return;
    if (InterlockedIncrement(&lines) <= 3000)
        Log("shadow casters: cascade %d: %u casters (was %d) at %llu ms", i, n, last[i] == ~0u ? -1 : (int)last[i], GetTickCount64());
    last[i] = n;
}

// Replaces Shadow_SetupCascades' call to the cascade cull (FUN_1402d5d40). Marks this thread
// as inside a cascade cull (for H_ZoneSelect), clears the size-cull switch around the call
// for shadow_no_contrib, and logs caster counts for shadow_caster_log.
static void* H_ShadowCull(void* camNode, void* a2, void* a3, void* a4) {
    bool was = g_inShadowCull;
    g_inShadowCull = true;
    volatile BYTE* contrib = ExeBase() + RVA_CONTRIB_FLAG;
    BYTE saved = 0;
    if (g_gfx.shadowNoContrib) { saved = *contrib; *contrib = 0; }
    void* r = g_shadowCull(camNode, a2, a3, a4);
    if (g_gfx.shadowNoContrib) *contrib = saved;
    g_inShadowCull = was;
    if (g_gfx.shadowCasterLog) ShadowCasterLog(camNode, r);
    return r;
}

// Replaces the zone pick's call to FUN_1402e4130 (shadow_all_zones). Inside a cascade cull,
// the cascade camera's linked camera (+0x228) and "zones by position" flag (+0x234) are
// cleared around the call, so it tests every zone; outside one it's the plain call.
static void H_ZoneSelect(void* cullData) {
    BYTE* cam = NULL;
    if (g_inShadowCull && cullData) __try { cam = *(BYTE**)((BYTE*)cullData + 0x18); } __except (EXCEPTION_EXECUTE_HANDLER) { cam = NULL; }
    if (!cam) { g_zoneSelect(cullData); return; }
    void* linked = *(void**)(cam + 0x228);
    BYTE byPos = *(cam + 0x234);
    *(void**)(cam + 0x228) = NULL;
    *(cam + 0x234) = 0;
    g_zoneSelect(cullData);
    *(void**)(cam + 0x228) = linked;
    *(cam + 0x234) = byPos;
    static LONG logs;
    if (InterlockedIncrement(&logs) == 1)
        Log("shadow casters: all visibility zones (shadow_all_zones): %u zones tested for the first cascade "
            "(camera had %s)", *(uint32_t*)(cam + 0x230),
            linked ? "a linked camera's zone list" : byPos ? "zones around its position" : "all zones already");
}

// shadow_near_extend=E (metres). Stock bug (decomp, re\STOCK_BUGS.md): the shadow
// caster vertex shaders clamp clip-space z at 0 PER VERTEX (FMax(z, 0) in shadowdepth_
// and the skinned Depth VS). A big caster triangle with a vertex above a cascade's near
// plane gets that vertex lifted to depth 0, so the whole triangle's interpolated depth
// is too deep and its shadow vanishes in a polygon-shaped patch. The near plane is
// the padded box's top (light space max z), refitted to the view slice every frame,
// and lowest for the small cascade 0: patches that jump as the player moves or turns,
// inside a square around them (cascade 0's footprint). Bedroom walls and the window
// frame over the board-game board are exactly such triangles.
// FUN_1401dabe0 (cascade projection, called only at 0x1401eb8ab from FUN_1401eb800)
// returns the padded near and far distances in out[0]/out[1]; the depth terms derived
// from the near distance are recomputed here with near moved E metres toward the sun
// (min 0.1): proj[10], proj[14], cascade+0x170, out[0], out[2], out[3]. The x/y rect,
// texel snapping and the cascade camera (CPU culling) stay the game's. Render-only.
typedef float* (*PFN_CascadeProj)(void* self, float* proj, BYTE* cascade, float* boxMin, float* boxMax, float* out);
static PFN_CascadeProj g_cascadeProj;

// Replaces the call to FUN_1401dabe0: run it, then redo the near-plane terms as described
// above when moving near toward the sun actually changes it.
static float* H_CascadeProj(void* self, float* proj, BYTE* cascade, float* boxMin, float* boxMax, float* out) {
    float* r = g_cascadeProj(self, proj, cascade, boxMin, boxMax, out);
    __try {
        float nearD = out[0], farD = out[1];
        float n2 = (std::max)(nearD - (float)g_gfx.shadowNearExtend, 0.1f);
        if (n2 < nearD && farD > n2) {
            float k = 1.0f / (n2 - farD);
            proj[10] = k;
            proj[14] = k * n2;
            *(float*)(cascade + 0x170) = -k;
            float z = farD / (k * n2);
            out[0] = n2;
            out[2] = z;
            out[3] = z * k;
            static LONG logs;
            if (InterlockedIncrement(&logs) <= 3)
                Log("shadow cascade depth: near %.2f m -> %.2f m (far %.2f m; shadow_near_extend=%.1f)", nearD, n2, farD,
                    g_gfx.shadowNearExtend);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return r;
}

// shadow_near_extend: check FUN_1401dabe0's first bytes (RVA 0x1dabe0), then redirect its
// one call (at RVA 0x1eb8ab) to H_CascadeProj.
static void PatchShadowNear() {
    if (g_gfx.shadowNearExtend <= 0) return;
    g_cascadeProj = (PFN_CascadeProj)(ExeBase() + 0x1dabe0);
    static const BYTE prologue[] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x68, 0x18 };
    bool ok = BytesAt(0x1dabe0, prologue, sizeof prologue) && RedirectCall(0x1eb8ab, 0x1dabe0, (void*)H_CascadeProj);
    Log(ok ? "graphics: sun shadow cascades reach %.1f m further toward the sun (shadow_near_extend)"
           : "graphics: shadow_near_extend skipped: code doesn't match this build", g_gfx.shadowNearExtend);
}

// shadow_all_zones / shadow_no_contrib / shadow_caster_log: all three need H_ShadowCull on
// the cascade cull call (RVA 0x1e8a47 -> 0x2d5d40); shadow_all_zones also needs H_ZoneSelect
// on the zone pick call (RVA 0x2d172a -> 0x2e4130). RedirectCall checks each call's target
// first. The size-cull switch is read here only to log its starting value.
static void PatchShadowZones() {
    if (!g_gfx.shadowAllZones && !g_gfx.shadowNoContrib && !g_gfx.shadowCasterLog) return;
    g_shadowCull = (PFN_ShadowCull)(ExeBase() + 0x2d5d40);
    g_zoneSelect = (PFN_ZoneSelect)(ExeBase() + 0x2e4130);
    BYTE flag = 0;
    __try { flag = *(ExeBase() + RVA_CONTRIB_FLAG); } __except (EXCEPTION_EXECUTE_HANDLER) { flag = 0xFF; }
    if (!RedirectCall(0x1e8a47, 0x2d5d40, (void*)H_ShadowCull)) {
        Log("graphics: shadow cascade cull hooks skipped: code doesn't match this build");
        return;
    }
    if (g_gfx.shadowAllZones)
        Log(RedirectCall(0x2d172a, 0x2e4130, (void*)H_ZoneSelect)
                ? "graphics: shadow casters are culled against every visibility zone (shadow_all_zones=1)"
                : "graphics: shadow_all_zones skipped: code doesn't match this build");
    if (g_gfx.shadowNoContrib)
        Log("graphics: shadow casters aren't dropped for their size (shadow_no_contrib=1; the size-cull switch reads %u at start)", flag);
    if (g_gfx.shadowCasterLog) Log("graphics: shadow caster counts are logged when they change (shadow_caster_log=1)");
}

// From DllMain when [graphics] enabled=1, before the game creates its render targets: log the
// settings, apply each static patch (each checks the game's code and skips itself if it
// doesn't match), then install gfx_d3d12.cpp's D3D12 hooks.
void ApplyGraphicsPatches() {
    Log("graphics: fps_limit=%.1f cull_pop=%s cull_fade=%s lod_scale=%.2f object_fade_scale=%.2f "
        "world_lod_scale=%.2f full_res_reflection=%d full_res_glow=%d shadow_all_zones=%d shadow_no_contrib=%d "
        "shadow_caster_log=%d shadow_near_extend=%.1f",
        g_gfx.fpsLimit, g_gfx.hasCullPop ? "set" : "game", g_gfx.hasCullFade ? "set" : "game",
        g_gfx.lodScale, g_gfx.objectFadeScale, g_gfx.worldLodScale, g_gfx.fullResReflection, g_gfx.fullResGlow,
        g_gfx.shadowAllZones, g_gfx.shadowNoContrib, g_gfx.shadowCasterLog, g_gfx.shadowNearExtend);
    PatchFrameLimit();
    PatchCull();
    PatchShadowZones();
    PatchShadowNear();
    if (g_gfx.fullResReflection) {
        static const BYTE w[] = { 0x66, 0xD1, 0xE9, 0x66, 0x89, 0x4D, 0xCB };   // shr cx,1 ; mov [rbp-35h],cx
        static const BYTE h[] = { 0x66, 0xD1, 0xEA, 0x66 };                     // shr dx,1 ; (mov ...)
        PatchHalfRes("reflection plane", RVA_REFL_SHR_W, w, sizeof w, RVA_REFL_SHR_H, h, sizeof h, 3);
    }
    if (g_gfx.fullResGlow) PatchGlow();
    PatchObjectFade();
    PatchWorldLod();
    VerifyActorLod();
    if (g_cfg.mode != MODE_PASSTHROUGH &&
        (g_lodReady || g_gfx.hasCullPop || g_gfx.hasCullFade || g_gfx.worldLodScale != 1.0))
        Log("NOTE: lod_scale, world_lod_scale and the cull settings may change what the game simulates. "
            "A recording and its replays (and both players in co-op) must use the same [graphics] values.");
    wchar_t ini[MAX_PATH];
    swprintf_s(ini, L"%s\\xinput_harness.ini", g_exeDir);
    GfxExt_Install(ini, Log);
}
