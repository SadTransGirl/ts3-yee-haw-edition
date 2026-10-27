// net_own_screen.cpp -- each PC on its own full screen in a session: viewports and camera swap,
// publish/movie sync and skip, the title-screen watch, HUD hiding and placement, the screen-region
// helpers and private-camera draws, owned-screen menus, reticle and prompts, per-player audio
// listeners and the sound pop fix.
//
// In a co-op session both PCs run the whole two-player game in lockstep (each steps the
// same frame with both players' input; net_lockstep.cpp), so each would show the stock
// split screen. "Own screen" ([netplay] own_screen=1) instead shows each PC only its own
// player, full screen. Everything it changes must be render-only: anything the
// simulation reads (cameras, culling, the screen manager, g_SplitScreenMode) has to stay
// identical on both PCs, or the two worlds drift apart. This file also holds the other
// session hooks that sit on the same game paths: the level publish (a level finishing
// loading and applying its save, FUN_1405d81a0), movies, and the title-screen watch.
//
// Addresses (0x14...) are in the game's exe, found by reverse engineering (the "decomp
// session", notes in re\); code uses RVAs, offsets from the exe's base 0x140000000.
// BytesAt / RipRefersTo and the "expect" arrays check the game build before patching.
//
// Entry points:
//  - DllMain (xinput_proxy.cpp), netplay mode: HookFrontendWatch, PatchMovieSkip,
//    HookPublish (drop_in=1), and with own_screen=1 PatchHudLayout, PatchReticleShow,
//    PatchOwnedScreens, PatchViewportSetAll, PatchJobExec. TrySoundPopFix in every mode.
//  - InstallDeterminism (net_determinism.cpp): redirects the split tests to
//    H_SplitIsVertical / H_SplitIsHorizontal and the private-camera draws to
//    H_PreviewDraw, and calls InstallAudioListenerHooks.
//  - The lockstep: NetStart re-applies the viewports (NetViewportsRefresh) and every
//    lockstep frame hides the other player's button prompts (HudHidePrompts); NetEnd
//    (net_session.cpp) restores the viewports, health bar and prompts. NetGetState
//    calls NetRemovePlayer2 after a session ends mid-play.
//  - Everything else runs inside the game, from the hooks installed above.

#include "proxy_shared.h"

bool     g_popFixWanted;                      // sound pop fix (audio section)
static bool g_popFixDone;

// ------------------------------------------------------------------ netplay: own full screen
//
// Viewport_SetAll(rects, count) copies count 0x18-byte viewports
// (x, y, w, h as shorts, then four floats) into g_Viewports (stride 0x20),
// stores the count, and sets a scale: 1.0 for one view, 2.0 for split. It's
// replaced here with an exact copy that, during a two-player netplay
// session, gives this PC's player the full-screen viewport and shrinks the
// other player's to one pixel. Both PCs get the single-view scale, so they
// still agree; only what each PC draws differs.

// RVA_VP_ONE / RVA_VP_TWO: the game's constant scales for one and for two views.
// RVA_DISPLAY_GET returns g_Display + 0x1c0; +0xac from there is the full-screen viewport.
static const DWORD RVA_VP_SETALL = 0x1f7bd0, RVA_VP_COUNT = 0x94e294, RVA_VP_SCALE = 0x953b38,
                   RVA_VP_ARRAY = 0x9aac40, RVA_VP_ONE = 0x776e84, RVA_VP_TWO = 0x776eb8, RVA_DISPLAY_GET = 0x2d5fb0;
bool g_ownScreenReady;                        // Viewport_SetAll verified and replaced
static uint8_t g_vpLast[4 * 0x18];            // the game's last layout, as it passed it
static int g_vpLastCount = -1;
bool g_vpOwnLogged;
static bool g_camSwapped;                     // client: viewport slots 0 and 1 have swapped cameras
static BYTE* g_hudHidden[64];                 // HUD roots hidden for the other player (see H_HudLayout)
static float g_hudScale[64][2];
int   g_hudHiddenN;
BYTE* g_promptEl[6];                   // the other player's button-prompt slots, hidden (see HudHidePrompts)
static float g_promptScale[6][2];
// Camera pointers live at g_Viewports[i]+0x18, written once by RenderTargets_Init;
// only the frame builder reads them, so swapping slot 0 and 1 is render-only.
static void SwapViewportCameras() {
    uint64_t* c0 = (uint64_t*)(ExeBase() + 0x9aac58);
    uint64_t* c1 = (uint64_t*)(ExeBase() + 0x9aac78);
    uint64_t t = *c0;
    *c0 = *c1;
    *c1 = t;
    g_camSwapped = !g_camSwapped;
}

// The replacement Viewport_SetAll, called by the game whenever its view layout
// changes. It keeps a copy of the game's layout (for NetViewportsRefresh), swaps in
// the own-screen layout while a session runs, then does what the original does.
static void H_ViewportSetAll(const uint8_t* rects, int count) {
    BYTE* b = ExeBase();
    if (count >= 0 && count <= 4 && rects != g_vpLast) { memcpy(g_vpLast, rects, count * 0x18); g_vpLastCount = count; }
    uint8_t buf[4 * 0x18];
    const uint8_t* src = rects;
    float scale = *(float*)(b + (count == 1 ? RVA_VP_ONE : RVA_VP_TWO));
    if (count == 2 && g_ownScreenReady && g_net.ownScreen && g_netPhase == NP_RUNNING) {
        // Own screen v2: both viewports are still built every frame, because the frame
        // builder updates each rendered camera (cam->vf(0x70)) and streaming reads the
        // camera positions. This PC's slot gets the display's full-screen viewport
        // (rect and single-view projection); the other slot shrinks to 1x1.
        typedef uint8_t* (*PFN_Display)();
        const uint8_t* full = ((PFN_Display)(b + RVA_DISPLAY_GET))() + 0xac;
        int local = g_net.role ? 1 : 0, other = 1 - local;
        memcpy(buf, rects, 2 * 0x18);
        memcpy(buf + local * 0x18, full, 0x18);
        short* o = (short*)(buf + other * 0x18);
        o[2] = 1; o[3] = 1;                              // w, h (position stays the game's)
        // The four floats after the rect are min/max depth and the two projection
        // scales (split screen halves one axis and doubles the other). The hidden
        // view gets the full-screen ones too, so BOTH views have the same frustum
        // on both PCs: each PC's local view is full screen, and with the other
        // view on split scales the two PCs culled each view differently. The
        // stagecoach's animated collision pieces then moved, and woke it, 14
        // frames apart (Toy Box build 35); split screen (own_screen=0) stayed in
        // sync for 26000 frames.
        memcpy(buf + other * 0x18 + 8, full + 8, 0x10);
        src = buf;
        scale = *(float*)(b + RVA_VP_ONE);
        if (g_camSwapped) SwapViewportCameras();         // (v1 swapped cameras; v2 never does)
        if (!g_vpOwnLogged) {
            g_vpOwnLogged = true;
            const short* f = (const short*)full;
            const uint32_t* fl = (const uint32_t*)(full + 8);    // min/max depth, projection scale, aspect: must match bit for bit across PCs
            Log("netplay: own screen: player %d's view full screen (%dx%d); the other view is 1x1 but still built, with the full-screen frustum "
                "(depth/scale floats %08x %08x %08x %08x)", local + 1, f[2], f[3], fl[0], fl[1], fl[2], fl[3]);
        }
    } else if (g_camSwapped) {
        SwapViewportCameras();                           // back to the game's own camera order
    }
    // The original's work: count, scale, then each 0x18-byte viewport into its 0x20 slot.
    *(int*)(b + RVA_VP_COUNT) = count;
    *(float*)(b + RVA_VP_SCALE) = scale;
    for (unsigned i = 0; i < (unsigned)count && i < 4; i++) memcpy(b + RVA_VP_ARRAY + i * 0x20, src + i * 0x18, 0x18);
}

// Re-apply the game's last layout (after lockstep starts or ends).
void NetViewportsRefresh() {
    if (!g_ownScreenReady || g_vpLastCount < 0) return;
    uint8_t copy[4 * 0x18];
    memcpy(copy, g_vpLast, sizeof copy);
    H_ViewportSetAll(copy, g_vpLastCount);
}

// From DllMain (own_screen=1): check Viewport_SetAll and the display getter, then
// jump from Viewport_SetAll's entry to H_ViewportSetAll (14-byte absolute jump).
bool PatchViewportSetAll() {
    static const BYTE expect[] = { 0x89, 0x15, 0xBE, 0x66, 0x75, 0x00, 0x83, 0xFA, 0x01, 0x75, 0x12,
                                   0xF3, 0x0F, 0x10, 0x05, 0xA1, 0xF2, 0x57, 0x00 };
    static const BYTE expectDisp[] = { 0x48, 0x8B, 0x05, 0xE9, 0x06, 0x7F, 0x00, 0x48, 0x05, 0xC0, 0x01, 0x00, 0x00, 0xC3 };
    if (!BytesAt(RVA_VP_SETALL, expect, sizeof expect) || !BytesAt(RVA_DISPLAY_GET, expectDisp, sizeof expectDisp)) return false;
    BYTE* fn = ExeBase() + RVA_VP_SETALL;
    BYTE jmp[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    void* target = (void*)H_ViewportSetAll;
    memcpy(jmp + 6, &target, 8);
    DWORD old;
    if (!VirtualProtect(fn, sizeof jmp, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(fn, jmp, sizeof jmp);
    VirtualProtect(fn, sizeof jmp, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, sizeof jmp);
    return true;
}

// FUN_1405d81a0 (level publish: applies the save buffer, posts LoadedSavedGame).
// Its first 15 bytes (three movs to the stack) are position-independent, so a
// trampoline runs them and jumps back.
// H_Publish runs first at every publish (a level finishing loading). In a session it
// (1) clears the "save already applied" flag once per session, (2) arms the load
// gate, (3) at the drop-in level's publish resets the random streams and entity
// keys on both PCs; then it calls the game's publish.
typedef uint64_t (*PFN_Publish)(void*);
static PFN_Publish g_publishOrig;
static uint64_t H_Publish(void* self) {
    // The save manager's "a save has been applied" byte (DAT_140b29a60 +0x158) is set at the
    // END of each save apply (FUN_140492900, "mov byte [rsi+158h],1" at 0x140492c5a) and never
    // cleared. Townsfolk bought in the Toy Box are created at the stagecoach DURING that apply
    // (stagecoach.sx's LoadedSavedGame handler) and only take their saved spot when the byte
    // is already 1 (CActor::vf7 -> FUN_1404a5a20). So a process that had applied a save before
    // put LP_Barber back where it stood, while a fresh one dropped it from the stagecoach, and
    // the PCs split (drop-in runs 81-98; decomp session). Both PCs clear it before the FIRST
    // publish of each session (a drop-in reload or a title-screen start: either PC may have
    // applied a save before the session): both take the stock first-load path, then the
    // apply sets it on both. Later publishes in the session keep the stock behaviour (bought
    // townsfolk back where they were on a return to the town), as both PCs share the history.
    static int clearedSession = -1;
    if (g_netPhase == NP_RUNNING && clearedSession != g_netSessions) {
        clearedSession = g_netSessions;
        static const BYTE set158[] = { 0xC6, 0x86, 0x58, 0x01, 0x00, 0x00, 0x01 };
        if (BytesAt(0x492c5a, set158, sizeof set158)) {
            if (BYTE* so = SaveGameObj()) {
                BYTE was = 0;
                __try { was = so[0x158]; so[0x158] = 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
                Log("save restore: 'save already applied' flag was %u at the session's first publish; cleared, so townsfolk start "
                    "the same way on both PCs", was);
            }
        } else {
            Log("save restore: the save apply's code doesn't match; townsfolk may start differently on the two PCs");
        }
    }
    // The load gate (net_session.cpp): from here the game's load-complete step is held
    // until both PCs have reached it, and the game is kept paused meanwhile
    // (g_Display+0x1a8 bit 3).
    // vf24's "cmp dword [DAT_140b2ce48],4" at 0x1405dd4d1 (see g_loadGate)
    if (g_netPhase == NP_RUNNING && RipRefersTo(0x5dd4d1, 2, 7, kLoadDoneCounter) && *(ExeBase() + 0x5dd4d7) == 4) {
        g_loadGate = true;
        g_loadGateReady = false;
        g_unpauseBlocked = 0;
        g_worldFrozen = 0;
        g_loadStepDone = false;
        g_loadStepAt = g_streamIdleFrames = 0;
        g_streamLastReads = g_ioReads[0] + g_ioReads[1];
        bool wasPaused = true;
        if (g_unpauseOrig) {                              // held paused on both PCs until the release
            __try {
                if (BYTE* disp = *(BYTE**)(ExeBase() + kDisplayPtr)) {
                    wasPaused = (*(volatile uint32_t*)(disp + 0x1a8) & 8) != 0;
                    *(volatile uint32_t*)(disp + 0x1a8) |= 8;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        Log("load gate: armed at frame %u (level publish)%s", g_netFrame,
            !g_unpauseOrig ? "; the pause can't be held (unpause code doesn't match)"
                           : wasPaused ? "; the game was paused" : "; the game wasn't paused: paused it");
        LotSelectorReset("the level publish");
    }
    // Drop-in (a friend joined while the host was in a level, and both PCs reloaded it):
    // the first publish after the launch is that level. Both PCs reset the engine RNG,
    // the rand() streams, the spawn gate's slots and the entity-key counter
    // (DAT_1409575c8) here, so the level starts out the same on both.
    if (g_dropIn && g_dropLaunched && !g_dropPublished && g_netPhase == NP_RUNNING) {
        g_dropPublished = true;
        if (g_mtReseedOk && g_mtGlobalPtr && *g_mtGlobalPtr) {
            AcquireSRWLockExclusive(&g_mtLock);
            g_mtSeed(*g_mtGlobalPtr, kMtEngineSeed);
            g_mtRefill(*g_mtGlobalPtr);
            ReleaseSRWLockExclusive(&g_mtLock);
        }
        for (int i = 0; i < g_randSiteCount; i++) { g_randSites[i].state = 1; g_randSites[i].mainCalls = g_randSites[i].otherCalls = 0; }
        g_randFallbackState = 1;
        for (auto& sl : g_spawnSlots) sl.used = false;
        g_spawnSeq = 0;
        if (RipRefersTo(0x23ccd, 3, 7, 0x9575c8)) *(volatile uint64_t*)(ExeBase() + 0x9575c8) = g_netStartKeys + 0x100000;
        g_netHashHoldUntil = g_netFrame + 120;           // compare again two seconds after the level is up
        Log("drop-in: level published at frame %u: engine RNG, rand streams and entity keys reset on both PCs", g_netFrame);
        g_dropPublishedFrame = g_netFrame;
        g_dropHintAt = g_netFrame + 60;                  // after the loading screen (see NetDropJoinHint)
    }
    uint64_t r = g_publishOrig(self);
    if (g_clockLockstep && g_netPhase == NP_RUNNING) NetVisSync();   // it just set the async flag
    return r;
}
// From DllMain (drop_in=1): build the trampoline (the 15 bytes, then a jump back to
// publish+15) as g_publishOrig, and jump from publish's entry to H_Publish.
bool HookPublish() {
    static const BYTE expect[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20,
                                   0x57, 0x48, 0x83, 0xEC, 0x20 };
    if (!BytesAt(0x5d81a0, expect, sizeof expect)) return false;
    BYTE* fn = ExeBase() + 0x5d81a0;
    BYTE* tramp = (BYTE*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    memcpy(tramp, fn, 15);                               // the three movs
    BYTE back[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    void* cont = fn + 15;
    memcpy(back + 6, &cont, 8);
    memcpy(tramp + 15, back, sizeof back);
    FlushInstructionCache(GetCurrentProcess(), tramp, 64);
    g_publishOrig = (PFN_Publish)tramp;
    BYTE jmp[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    void* target = (void*)H_Publish;
    memcpy(jmp + 6, &target, 8);
    return WriteCode(0x5d81a0, jmp, sizeof jmp);
}

// Bink_PlayMovie's skip callback (FUN_14047d5f0 -> FUN_14047d3b0) polls every
// input device on each movie frame. In a session that would count movie frames
// as lockstep frames (a number that depends on playback speed) and let each PC
// skip on its own. Replaced: no polling; this player's A/Start (newly pressed)
// skips the movie on both PCs; a keepalive tells the other PC we're in a movie.
// Tell the other PC we're in movie g_movieIdx (skip = 1: and we skip it).
static void NetSendMovie(uint8_t skip) {
    NetMoviePkt m = { NetHeader(NET_MOVIE), g_movieIdx, skip };
    NetSend(&m, sizeof m);
    g_movieLastSend = GetTickCount64();
}
// A new movie: number it (both PCs count the same movies) and take the buttons
// already held as the baseline, so only a new press skips.
static void MovieBegin() {                              // under NetLockGuard
    g_movieAtFrame = g_netFrame;
    g_movieIdx++;
    g_movieSkipLocal = false;
    XINPUT_STATE s0 = {};
    g_moviePrevButtons = XInputGetState((DWORD)g_net.localPad, &s0) == ERROR_SUCCESS ? s0.Gamepad.wButtons : 0;
    Log("netplay: movie %u started at frame %u", g_movieIdx, g_netFrame);
}

// Both players see every movie: the first PC to reach one holds BEFORE Bink
// starts (a stalled player would fast-forward to catch up) until the other PC
// reports the same movie, then both play it. Without this, the faster PC
// could play and skip a movie before the slower one arrived, and the skip
// (which counts for both) cut the slower player's copy at once. Bink_PlayMovie
// (0x14047f390) is called from the in-level script path (0x140480390) and the
// queued path story levels use after a load (0x14047ffe6); both are redirected.
typedef uint64_t (*PFN_BinkPlay)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t H_BinkPlay(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g) {
    if (g_netPhase == NP_RUNNING && g_netT) {
        // Announce the movie, then wait for the other PC to announce it too.
        {
            NetLockGuard lock;
            if (g_movieAtFrame != g_netFrame) MovieBegin();
            NetSendMovie(0);
        }
        // No time limit while the other PC is alive: a long load there keeps
        // sending "busy" keepalives. Silence for timeout_ms ends the session,
        // the same rule as every lockstep wait, and the movie then plays here.
        ULONGLONG t0 = GetTickCount64(), shown = 0;
        bool waited = false;
        while (g_netPhase == NP_RUNNING && g_moviePeerIdx < g_movieIdx) {
            waited = true;
            {
                NetLockGuard lock;
                NetPump();
                if (GetTickCount64() - g_movieLastSend > 100) NetSendMovie(0);
                if (GetTickCount64() - g_netLastHeard > (ULONGLONG)g_net.timeoutMs) {
                    NetEnd("the other player stopped responding");
                    break;
                }
            }
            ULONGLONG t = GetTickCount64();
            if (t - shown >= 1000) {
                shown = t;
                Status(1500, kOvBlue, L"Cutscene: waiting for %s to get here... (%llu s)", g_netPeerNameW, (t - t0) / 1000);
            }
            Sleep(10);
        }
        if (waited)
            Log("netplay: movie %u held %llu ms until %s", g_movieIdx, GetTickCount64() - t0,
                g_moviePeerIdx >= g_movieIdx ? "the other player reached it" : "the session ended");
        NetLockGuard lock;
        XINPUT_STATE s0 = {};                            // a press made while waiting isn't a skip
        g_moviePrevButtons = XInputGetState((DWORD)g_net.localPad, &s0) == ERROR_SUCCESS ? s0.Gamepad.wButtons : 0;
        Status(4000, kOvBlue, L"Cutscene: press A to skip it for both players.");
    }
    return ((PFN_BinkPlay)(ExeBase() + 0x47f390))(a, b, c, d, e, f, g);
}

// The replacement skip callback, called on every movie frame: true = skip now.
// Outside a session it's the game's own (FUN_14047d3b0).
static bool H_MovieSkip(void* ctx) {
    if (!ctx) return false;
    if (g_netPhase != NP_RUNNING || !g_netT) return ((bool (*)(void*))(ExeBase() + 0x47d3b0))(ctx);
    NetLockGuard lock;
    if (g_movieAtFrame != g_netFrame) {                  // a movie that didn't come through H_BinkPlay
        MovieBegin();
        Status(4000, kOvBlue, L"Movie: press A to skip it for both players.");
    }
    NetPump();
    XINPUT_STATE s = {};
    WORD b = XInputGetState((DWORD)g_net.localPad, &s) == ERROR_SUCCESS ? s.Gamepad.wButtons : 0;
    WORD pressed = b & ~g_moviePrevButtons;
    g_moviePrevButtons = b;
    if (!g_movieSkipLocal && (pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START))) {
        g_movieSkipLocal = true;
        for (int i = 0; i < 3; i++) NetSendMovie(1);
        Log("netplay: this player skipped movie %u", g_movieIdx);
    }
    if (GetTickCount64() - g_movieLastSend > 100) NetSendMovie(g_movieSkipLocal ? 1 : 0);
    bool skip = g_movieSkipLocal || g_moviePeerSkipIdx == g_movieIdx;
    if (skip && !g_movieSkipLocal) Log("netplay: the other player skipped movie %u", g_movieIdx);
    return skip;
}
// From DllMain (netplay): replace the skip callback FUN_14047d5f0 (a null check and a
// jump to FUN_14047d3b0) with a jump to H_MovieSkip, and send both Bink_PlayMovie
// calls through H_BinkPlay.
bool PatchMovieSkip() {
    static const BYTE expect[] = { 0x48, 0x85, 0xC9, 0x75, 0x03, 0x32, 0xC0, 0xC3, 0xE9 };    // test rcx,rcx; jne; xor al,al; ret; jmp
    static const BYTE pad[] = { 0xCC, 0xCC, 0xCC };
    if (!BytesAt(0x47d5f0, expect, sizeof expect) || !RipRefersTo(0x47d5f8, 1, 5, 0x47d3b0) || !BytesAt(0x47d5fd, pad, sizeof pad))
        return false;
    BYTE jmp[14] = { 0xFF, 0x25, 0, 0, 0, 0 };          // 14 bytes: the 13-byte function plus one byte of its padding
    void* target = (void*)H_MovieSkip;
    memcpy(jmp + 6, &target, 8);
    if (!WriteCode(0x47d5f0, jmp, sizeof jmp)) return false;
    int held = RedirectCall(0x480390, 0x47f390, (void*)H_BinkPlay) + RedirectCall(0x47ffe6, 0x47f390, (void*)H_BinkPlay);
    Log("netplay: cutscenes wait for both players before playing (%d/2 call sites)", held);
    return true;
}

// PlayerMgr_SetPlayerAvatar(mgr, 1, 0) (0x1405f1030): count--, player 2's view
// off, device unbound, one viewport. Called while pad 1 still reads connected,
// so the game doesn't stop for "Reconnect controller". Called by NetGetState
// (net_session.cpp) on the first poll after a session ended mid-play.
// The player manager is at 0x140b2ef50 (+4: player count).
void NetRemovePlayer2() {
    static const BYTE expect[] = { 0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x63, 0xDA, 0x49, 0x8B, 0xF0,
                                   0x48, 0x8B, 0xF9, 0x83, 0xFB, 0x01 };
    if (!BytesAt(0x5f1030, expect, sizeof expect)) { Log("netplay: player 2 not removed (code doesn't match this build)"); return; }
    BYTE* pm = NULL;
    __try { pm = *(BYTE**)(ExeBase() + 0xb2ef50); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!pm || *(int*)(pm + 4) < 2) return;
    typedef void (*PFN_SetAvatar)(void* mgr, int slot, void* avatar);
    ((PFN_SetAvatar)(ExeBase() + 0x5f1030))(pm, 1, NULL);
    Log("netplay: player 2 removed; this PC carries on as a one-player game");
}

// ------------------------------------------------------------------ netplay: title screen, HUD and audio
//
// Title screen: ScreenFrontend::vf37 (per-frame update, vtable slot 0x14077e220)
// records that the title is up and its page (+0x290, 0 = "Press Start"), so a
// session only starts, or restarts, from "Press Start" on both PCs.
typedef void (*PFN_FrontendUpdate)(BYTE* self);
static PFN_FrontendUpdate g_feOrig;
// The title screen's update: note when it last ran and its page, let the debug
// menu do its title-screen work, then run the game's own.
static void H_FrontendUpdate(BYTE* self) {
    g_feSeen = GetTickCount64();
    DebugMenu_FrontendUpdate(self);
    __try { g_feState = *(uint64_t*)(self + 0x290); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_feOrig(self);
}
// From DllMain (netplay): if vf37's vtable slot still holds the game's function,
// point it at H_FrontendUpdate.
bool HookFrontendWatch() {
    void** slot = (void**)(ExeBase() + 0x77e220);
    void* expect = ExeBase() + 0x126130;
    __try { if (*slot != expect) return false; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    g_feOrig = (PFN_FrontendUpdate)expect;
    void* hook = (void*)H_FrontendUpdate;
    DWORD old;
    if (!VirtualProtect(slot, sizeof hook, PAGE_READWRITE, &old)) return false;
    *slot = hook;
    VirtualProtect(slot, sizeof hook, old, &old);
    return true;
}

// Own screen is in effect right now: patched, switched on, and a session running.
bool NetOwnScreenActive() { return g_ownScreenReady && g_net.ownScreen && g_netPhase == NP_RUNNING; }

// HUD: every element lays itself out through FUN_1405f4af0(elem, playerIdx),
// which calls elem->vf(0x1d0)(elem, playerIdx, playerCount, g_SplitScreenMode).
// With own screens, this PC's player's elements get the one-player full-screen
// layout. g_SplitScreenMode itself is untouched (cameras and AI read it).
// The other player's HUD elements are hidden by scaling their root sprite to 0
// (sprite vtable +0x178; scale at +0x58/+0x5c): pure render state, nothing is
// closed, so both PCs' screen managers stay identical. Saved scales come back
// when an element is laid out again outside own-screen.
// HudRoot: an element's (or screen's) root sprite, elem+0x30 -> +0x30; NULL if unset.
static BYTE* HudRoot(BYTE* elem) {
    __try {
        BYTE* a = *(BYTE**)(elem + 0x30);
        return a ? *(BYTE**)(a + 0x30) : NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
// Set a root sprite's X / Y scale through its vtable (+0x178).
static void HudSetScale(BYTE* root, float x, float y) {
    __try {
        void** vt = *(void***)root;
        ((void (*)(void*, float, float))vt[0x178 / 8])(root, x, y);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// Scale a root to 0, remembering its scale the first time (up to 64 roots).
static void HudHide(BYTE* root) {
    for (int i = 0; i < g_hudHiddenN; i++) if (g_hudHidden[i] == root) { HudSetScale(root, 0.0f, 0.0f); return; }
    if (g_hudHiddenN >= 64) return;
    __try {
        g_hudScale[g_hudHiddenN][0] = *(float*)(root + 0x58);
        g_hudScale[g_hudHiddenN][1] = *(float*)(root + 0x5c);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    g_hudHidden[g_hudHiddenN++] = root;
    HudSetScale(root, 0.0f, 0.0f);
}
// Give a hidden root its scale back and forget it; nothing for one never hidden.
static void HudUnhide(BYTE* root) {
    for (int i = 0; i < g_hudHiddenN; i++)
        if (g_hudHidden[i] == root) {
            HudSetScale(root, g_hudScale[i][0], g_hudScale[i][1]);
            g_hudHidden[i] = g_hudHidden[g_hudHiddenN - 1];
            g_hudScale[i][0] = g_hudScale[g_hudHiddenN - 1][0];
            g_hudScale[i][1] = g_hudScale[g_hudHiddenN - 1][1];
            g_hudHiddenN--;
            return;
        }
}
// Replaces FUN_1405f4af0 (PatchHudLayout): picks the layout arguments, calls the
// element's vf(0x1d0) as the original would, then hides or unhides its root.
static uint64_t H_HudLayout(BYTE* elem, int idx) {
    BYTE* b = ExeBase();
    BYTE* pm = *(BYTE**)(b + 0xb2ef50);                   // player manager; +4 player count
    int count = pm ? *(int*)(pm + 4) : 1;
    int mode = *(int*)(b + 0xb2c5f8);                    // g_SplitScreenMode
    bool shared = false;                                 // owner 5/6: shared screens that follow UI focus
    int owner = -1;
    __try { owner = *(int*)(elem + 0x40); shared = owner == 5 || owner == 6; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    bool own = count == 2 && NetOwnScreenActive();
    // Two elements the decomp session traced (re\SYNC_VIABILITY.md, "Traced:
    // own-screen HUD"), both pure presentation:
    //  - ScreenRespawnTip (vtable 0x140781818, "you will respawn at the next
    //    safe zone") is laid out with the DYING player's index, so it hides on
    //    the other PC like a per-player element whatever its owner says;
    //  - HealthMeter (vtable 0x140788db0, P1HealthBarHud / P2HealthBarHud) are
    //    two shared elements whose side is elem+0x318 (0 = P1, 1 = P2). Both
    //    get the one-player layout; the partner's bar then moves under this
    //    player's and fills the same way (side = this player's).
    uintptr_t vt = 0;
    int side = -1;
    __try {
        vt = *(uintptr_t*)elem;
        if (vt == (uintptr_t)(b + 0x788db0)) side = *(BYTE*)(elem + 0x318);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    bool respawnTip = vt == (uintptr_t)(b + 0x781818), health = side >= 0;
    typedef uint64_t (*PFN_Layout)(BYTE*, int, int, int);
    PFN_Layout f = *(PFN_Layout*)(*(BYTE**)elem + 0x1d0);
    uint64_t r;
    bool hide = own && (idx == 0 || idx == 1) && idx != g_net.role && (!shared || respawnTip) && !health;
    if (own && !hide) r = f(elem, 0, 1, 0);              // this player's, or shared: the one-player layout
    else r = f(elem, idx, count, mode);
    BYTE* root = HudRoot(elem);
    if (root) {
        if (hide) HudHide(root);
        else HudUnhide(root);
    }
    if (root && own && health) {
        // Both bars at the same height, one per side (the user's choice): this
        // player's keeps the one-player (left) position and the left-side fill;
        // the partner's root is mirrored about the design-space centre (480
        // units high, aspect*480 wide) with the right-side fill, a mirror image.
        // +0x318 only picks the fill direction and the shine animation.
        __try {
            bool partner = side != g_net.role;
            *(BYTE*)(elem + 0x318) = partner ? 1 : 0;
            if (partner) {
                typedef const uint8_t* (*PFN_DisplayGet)();
                const uint8_t* full = ((PFN_DisplayGet)(b + RVA_DISPLAY_GET))() + 0xac;
                float w = *(const int16_t*)(full + 4), h = *(const int16_t*)(full + 6);
                float cx = h > 0 ? (w / h) * 240.0f : 426.7f;
                void** rvt = *(void***)root;
                float x = ((float (*)(void*))rvt[0x2c8 / 8])(root);
                ((void (*)(void*, float))rvt[0x148 / 8])(root, 2.0f * cx - x);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    static int logged;
    if (own && (health || respawnTip) && logged < 12) {
        logged++;
        Log("own-screen HUD: %s laid out: idx %d count %d mode %d owner %d side %d -> %s", health ? "HealthMeter" : "RespawnTip",
            idx, count, mode, owner, side, hide ? "hidden" : health && side != g_net.role ? "partner's, mirrored right" : "shown");
    }
    return r;
}
bool g_healthPlaced;                      // player 2's health bar is raised (HudPlaceHealthBars)
// The health meters (P1HealthBarHud / P2HealthBarHud, single-instance elements)
// are never laid out by code: their positions come from HealthMeters.sc, which
// is authored for split screen (a story-level recording showed both bars in
// those spots on both PCs). With own screens, player 1's bar keeps its place
// and player 2's root goes to the same height, mirrored to the right, on both
// PCs; it is put back when the session ends. The elements are found through the
// screen manager's own lookup (FUN_1404b2630, idx -1 = single instance), and
// +0x334/+0x338 hold each root's authored X/Y. Render state only.
// (Today only NetEnd calls it, with on = false, to undo an earlier raise: the
// per-frame raise was dropped, see NetFrame in net_lockstep.cpp.) Root vtable
// +0x148 / +0x150 set its X / Y. 0x140b2ef10 is the screen manager.
void HudPlaceHealthBars(bool on) {
    BYTE* b = ExeBase();
    if (!RipRefersTo(0x3416a, 3, 7, 0xb2ef10)) return;
    BYTE* mgr = *(BYTE**)(b + 0xb2ef10);
    if (!mgr) return;
    typedef BYTE* (*PFN_Find)(BYTE*, const char*, int);
    PFN_Find find = (PFN_Find)(b + 0x4b2630);
    __try {
        BYTE* p1 = find(mgr, "P1HealthBarHud", -1);
        BYTE* p2 = find(mgr, "P2HealthBarHud", -1);
        BYTE* r2 = p2 ? HudRoot(p2) : NULL;
        if (!p1 || !r2) { g_healthPlaced = false; return; }
        // Both roots are authored at the design centre (320, 240); the bars are
        // child sprites: player 1's top-left, player 2's already mirrored but at
        // mid height (the vertical-split layout). So only the height changes:
        // player 2's meter rises by the authored gap between the two bars.
        static const float kP2BarRaise = 187.5f;     // design units (480 high): 150 px of a 384 px capture
        float x = *(float*)(p2 + 0x334), y = *(float*)(p2 + 0x338);
        if (on) y -= kP2BarRaise;
        void** vt = *(void***)r2;
        ((void (*)(void*, float))vt[0x148 / 8])(r2, x);
        ((void (*)(void*, float))vt[0x150 / 8])(r2, y);
        static int logged;
        if (logged < 4 && (on != g_healthPlaced)) {
            logged++;
            Log("own-screen HUD: player 2's health bar root %s (%.1f, %.1f); player 1's is at (%.1f, %.1f)",
                on ? "raised to" : "restored to", x, y, *(float*)(p1 + 0x334), *(float*)(p1 + 0x338));
        }
        g_healthPlaced = on;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// Own screen: the other player's 1x1 view still builds all its passes every frame
// (its main, sun-cascade and reflection culls fire the "became visible" events that
// animation reads, so both PCs must keep them; decomp session), but its GPU work is
// dropped: the frame job's recorded command list is simply not executed (lists live in
// the per-frame arena, reset every frame, so there's nothing to free).
// That's the hidden view's depth, gbuffer, lighting and post, and its re-render of the
// whole sun shadow map (cascades are fitted and drawn per view). Render only. This
// replaces FUN_1402d2030, every frame job's default execute (render thread): sort the
// list's 0x18-byte items by key (FUN_1402cc290; pass begin/end markers and draws), then
// execute them (FUN_1402d6080). Frame jobs carry their 0x40-byte view entry at +8 (view
// index +0x38, view count +0x3a) and their build function at +0x18. (Build 54 still
// executed the hidden list, unsorted: draws ran before their pass's begin item, with no
// render target set (g_Display+0x160), and crashed in SubGeometryNode::vf16.)
static const DWORD RVA_JOB_EXEC = 0x2d2030, RVA_JOB_SORT = 0x2cc290, RVA_JOB_EXECUTE = 0x2d6080,
                   RVA_SETUP_FRAME_PASSES = 0x1f8d60;
static volatile LONG64 g_hiddenSkipped;
// job+0x28 is the job's command list; in the list, +0x18 is the first item and +8
// the end. Skips the list only for the other player's view of the frame setup job.
static void H_JobExec(BYTE* job) {
    BYTE* list = *(BYTE**)(job + 0x28);
    if (!list) return;
    BYTE* b = ExeBase();
    bool skip = false;
    if (g_net.skipHidden && *(BYTE**)(job + 0x18) == b + RVA_SETUP_FRAME_PASSES && NetOwnScreenActive()) {
        const BYTE* view = *(BYTE**)(job + 8);
        skip = view && *(uint16_t*)(view + 0x3a) == 2 && *(uint16_t*)(view + 0x38) == (g_net.role ? 0 : 1);
    }
    if (skip) {
        if (InterlockedIncrement64(&g_hiddenSkipped) == 1)
            Log("netplay: own screen: the other player's view (view %d of 2) is built but not rendered", g_net.role ? 1 : 2);
        return;
    }
    // What the original does: sort the items, then execute the list.
    BYTE scratch[64];
    BYTE* begin = *(BYTE**)(list + 0x18);
    int64_t n = (*(BYTE**)(list + 8) - begin) / 0x18;
    ((void (*)(void*, void*, int64_t))(b + RVA_JOB_SORT))(scratch, begin, n);
    ((void (*)(void*))(b + RVA_JOB_EXECUTE))(list);
}
// From DllMain (own_screen=1, own_screen_skip_hidden=1): jump from the default execute to
// H_JobExec.
bool PatchJobExec() {
    static const BYTE expect[] = {
        0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x59, 0x28, 0x48, 0x85, 0xdb, 0x74, 0x3e, 0x4c, 0x8b, 0x43, 0x08,
        0x48, 0x8d, 0x4c, 0x24, 0x30, 0x4c, 0x2b, 0x43, 0x18, 0x48, 0xb8, 0xab, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0x2a,
        0x49, 0xf7, 0xe8, 0x48, 0xc1, 0xfa, 0x02, 0x4c, 0x8b, 0xc2, 0x49, 0xc1, 0xe8, 0x3f, 0x4c, 0x03, 0xc2, 0x48, 0x8b,
        0x53, 0x18, 0xe8, 0x20, 0xa2, 0xff, 0xff, 0x48, 0x8b, 0xcb, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0xe9, 0x03, 0x40, 0x00,
        0x00, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3 };      // the whole function: sort(items, count), then execute(list)
    if (!BytesAt(RVA_JOB_EXEC, expect, sizeof expect)) return false;
    BYTE jmp[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    void* target = (void*)H_JobExec;
    memcpy(jmp + 6, &target, 8);
    return WriteCode(RVA_JOB_EXEC, jmp, sizeof jmp);
}

// Own screen: the aim reticle. There's one reticle screen per player (HudReticleManager
// 0x140993108, FUN_1400ea970(mgr, playerIdx)), laid out once when the level HUD opens, so
// H_HudLayout never sees it again; entering target mode only shows it, through
// FUN_140138fc0(mgr, show, playerIdx, tint) (TargetSelectDriver -> FUN_14008e750). After
// every show or hide, the other player's reticle root gets scale 0 on this PC (render only;
// visibility and scale are separate, so a later show doesn't undo it).
typedef void (*PFN_ReticleShow)(void* mgr, char show, int idx, char tint);
static PFN_ReticleShow g_reticleShowOrig;
// The game's show/hide first, then this PC's scale for that player's reticle.
static void H_ReticleShow(void* mgr, char show, int idx, char tint) {
    g_reticleShowOrig(mgr, show, idx, tint);
    if (!NetOwnScreenActive() || (idx != 0 && idx != 1)) return;
    __try {
        BYTE* screen = ((BYTE* (*)(void*, int))(ExeBase() + 0xea970))(mgr, idx);
        BYTE* root = screen ? HudRoot(screen) : NULL;
        if (!root) return;
        if (idx != g_net.role) HudHide(root);
        else HudUnhide(root);
        static int logged;
        if (logged < 4) {
            logged++;
            Log("own-screen HUD: player %d's aim reticle %s on this PC", idx + 1, idx != g_net.role ? "hidden" : "shown");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Own screen and the screen-region helpers (decomp session). Own screen gives both views a
// full-screen projection, but g_SplitScreenMode stays split (cameras and AI read it), so the
// helpers that map aim/reticle positions into "the player's part of the screen" still used
// half the screen: FUN_1401e7b80 (normalised -> 640x480 design coords), FUN_1401d0bb0 and
// FUN_1401e86a0 (clamp into the region), FUN_1401e8590 (region rect) and FUN_1401c8580
// (projection aspect, halved or doubled). Buzz's reticle sat in the middle of the top half,
// and turret/cannon aiming (MiniTurret, FUN_14005eef0's smoothing) kept that in its state.
// While own screen is active, their 10 calls to the split tests (FUN_1405ceca0 vertical,
// FUN_1405cdf40 horizontal) answer "not split" on both PCs: the full-screen region for both
// players, matching the projection. Everything else still sees the real split mode.
bool g_regionFixOk;
bool H_SplitIsVertical() { return NetOwnScreenActive() ? false : ((bool (*)())(ExeBase() + 0x5ceca0))(); }
bool H_SplitIsHorizontal() { return NetOwnScreenActive() ? false : ((bool (*)())(ExeBase() + 0x5cdf40))(); }

// Own screen: the customization preview. ScreenCustomization::vf38 draws the real
// townsperson's scene node a second time through a private camera into a pixel rect
// (FUN_1401f7d10(node, rect, view, proj), its one call at 0x1401225ca). Both PCs run the
// screen, so the non-owner PC drew a huge townsperson over the left of its full-screen view.
// Render only: the draw is skipped on the PC whose player doesn't own the screen (the screen
// holding that node, in any of the manager's lists; its ScreenInfo playerIndex +0xe0). The same
// draw job serves ScreenWantedPoster's portrait (node at +0x288, call 0x140123fc7) and the
// mission compass's 3D arrow (HudMissionCompass via FUN_140204be0, node at +0x330, call
// 0x140205b22); HudHide's scale 0 can't hide a draw job.
bool g_previewFixOk;                       // own screen: the customization preview draws only on its owner's PC
static int g_drawOwner = -1;                      // owner of the per-player menu being updated (H_OwnedScreenUpdate), else -1
// The playerIndex of the screen holding this node: the screen manager's three
// screen lists (+0x1c8/+0x1d0/+0x1d8, each linked through +0x98) are searched
// for a screen with it at one of the node offsets. -1 if none.
static int PreviewOwner(BYTE* node) {
    __try {
        BYTE* mgr = *(BYTE**)(ExeBase() + 0xb2ef10);
        static const int lists[3] = { 0x1c8, 0x1d0, 0x1d8 };
        static const int nodes[3] = { 0x5c8, 0x288, 0x330 };   // customization, wanted poster, compass
        for (int l = 0; mgr && l < 3; l++)
            for (BYTE* sc = *(BYTE**)(mgr + lists[l]); sc; sc = *(BYTE**)(sc + 0x98))
                for (int k = 0; k < 3; k++)
                    if (*(BYTE**)(sc + nodes[k]) == node) {
                        BYTE* info = *(BYTE**)(sc + 0x30);
                        return info ? *(int*)(info + 0xe0) : -1;
                    }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return -1;
}
// Stands in for FUN_1401f7d10 at the three call sites: draw unless the screen
// belongs to the other player.
void H_PreviewDraw(BYTE* node, void* rect, void* view, void* proj) {
    if (NetOwnScreenActive()) {
        int owner = g_drawOwner >= 0 ? g_drawOwner : PreviewOwner(node);   // set while the owning menu updates
        if ((owner == 0 || owner == 1) && owner != g_net.role) return;
    }
    ((void (*)(BYTE*, void*, void*, void*))(ExeBase() + 0x1f7d10))(node, rect, view, proj);
}

// Own screen: per-player menus that draw a model through a private camera (customization,
// wanted poster, mission compass). The manager's lists don't hold these screen objects, so
// their owner is taken while their own update runs: vf38 (vtable slot +0x130) is wrapped,
// the owner (ScreenInfo +0x30 -> playerIndex +0xe0) is published for H_PreviewDraw, and
// afterwards the screen's root sprite is scaled to 0 on the PC whose player doesn't own it
// (its paint/hat pickers showed on the other player's screen), or given its scale back.
// Render only. One remembered root per screen type: a new instance replaces the old one.
typedef void (*PFN_ScreenUpdate)(BYTE* screen);
// slotRva: the vf38 slot in the screen type's vtable; fnRva: the game's vf38 it must hold.
struct OwnedScreen { DWORD slotRva; DWORD fnRva; const char* name; PFN_ScreenUpdate orig; BYTE* hiddenRoot; float scale[2]; };
static OwnedScreen g_ownedScreens[3] = {
    { 0x77fa98, 0x121f90, "customization" }, { 0x786278, 0x123d10, "wanted poster" }, { 0x7884b8, 0x210d80, "mission compass" } };
// A screen's player: ScreenInfo (+0x30) -> playerIndex (+0xe0); -1 if none.
static int ScreenOwner(BYTE* screen) {
    __try {
        BYTE* info = *(BYTE**)(screen + 0x30);
        return info ? *(int*)(info + 0xe0) : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
// The customization preview's pixel rect (+0x5e0 x, +0x5e4 y, +0x5e8 w, +0x5ec h; +0x5f0 is
// player 2's x) is set once by FUN_140136c80. With two players (and +0x658 clear) it's the
// split version: the "IDS_SLICE5" box's design position and size scaled to HALF the window
// width, and player 2's x moved into the right half. Own screen draws the menu across the
// whole window, so the model landed left of the box (player 1) or outside it (player 2).
// While own screen is on, the rect is recomputed each update: the box's size over the full
// width, centred horizontally, and player 2 gets the same x. Render only (nothing but the draw reads it).
static void FixPreviewRect(BYTE* screen) {
    __try {
        if (*(BYTE*)(screen + 0x658)) return;                      // the game already uses its full-screen layout
        BYTE* b = ExeBase();
        typedef BYTE* (*PFN_FindChild)(BYTE*, const char*, uint64_t);
        BYTE* box = ((PFN_FindChild)(b + 0x4b2aa0))(screen, "IDS_SLICE5", *(uint64_t*)(screen + 0x660));
        if (!box) return;
        float px = 0, py = 0, sw = 0, sh = 0;
        void** vt = *(void***)box;
        ((void (*)(BYTE*, float*, float*))vt[0x160 / 8])(box, &px, &py);       // design position
        ((void (*)(BYTE*, float*, float*))vt[0x2e8 / 8])(box, &sw, &sh);       // design size
        BYTE* disp = ((BYTE* (*)())(b + 0x2d5fb0))();
        if (!disp) return;
        int W = *(int*)(disp - 0x1c0 + 0x138), H = *(int*)(disp - 0x1c0 + 0x13c);
        int w = (int)((sw / 640.0f) * (float)W), h = (int)((sh / 480.0f) * (float)H);
        // Own screen lays the menu out centred in the window, so the box is centred too: the
        // split branch's x (w/3 + px scaled to the window) put the model a third of the window
        // to its right. y keeps the split branch's formula, which lines up (screenshots, run 69d).
        int x = (W - w) / 2;
        int y = (-2 * h) / 3 + (int)((py / 480.0f) * (float)H);
        static BYTE* logged;
        if (logged != screen) {
            logged = screen;
            Log("own-screen HUD: customization preview: box design pos (%.1f, %.1f) size (%.1f x %.1f), window %dx%d -> rect (%d, %d) %dx%d",
                px, py, sw, sh, W, H, x, y, w, h);
        }
        *(int*)(screen + 0x5e0) = x;
        *(int*)(screen + 0x5e4) = y;
        *(int*)(screen + 0x5e8) = w;
        *(int*)(screen + 0x5ec) = h;
        *(int*)(screen + 0x5f0) = x;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// The vf38 wrapper for g_ownedScreens[K] (one instantiation per screen type).
template <int K> static void H_OwnedScreenUpdate(BYTE* screen) {
    OwnedScreen& s = g_ownedScreens[K];
    if (K == 0 && NetOwnScreenActive()) FixPreviewRect(screen);
    int owner = ScreenOwner(screen);
    int prev = g_drawOwner;
    g_drawOwner = owner;
    s.orig(screen);
    g_drawOwner = prev;
    // Then hide the root on the other player's PC, or give it back its scale.
    if (owner != 0 && owner != 1) return;
    BYTE* root = HudRoot(screen);
    if (!root) return;
    __try {
        if (NetOwnScreenActive() && owner != g_net.role) {
            if (s.hiddenRoot != root) { s.hiddenRoot = root; s.scale[0] = *(float*)(root + 0x58); s.scale[1] = *(float*)(root + 0x5c); }
            HudSetScale(root, 0.0f, 0.0f);
        } else if (s.hiddenRoot == root) {
            HudSetScale(root, s.scale[0], s.scale[1]);
            s.hiddenRoot = NULL;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// From DllMain (own_screen=1): wrap each vtable slot that still holds the game's
// vf38. Returns how many of the 3 were hooked.
int PatchOwnedScreens() {
    static const PFN_ScreenUpdate hooks[3] = { H_OwnedScreenUpdate<0>, H_OwnedScreenUpdate<1>, H_OwnedScreenUpdate<2> };
    int n = 0;
    for (int k = 0; k < 3; k++) {
        OwnedScreen& s = g_ownedScreens[k];
        BYTE* slot = ExeBase() + s.slotRva;
        __try { if (*(BYTE**)slot != ExeBase() + s.fnRva) continue; } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        s.orig = (PFN_ScreenUpdate)(ExeBase() + s.fnRva);
        void* hook = (void*)hooks[k];
        if (WriteMem(s.slotRva, &hook, sizeof hook, false)) n++;
    }
    return n;
}

// From DllMain (own_screen=1): trampoline for FUN_140138fc0's first 15 bytes, then
// a jump from its entry to H_ReticleShow.
bool PatchReticleShow() {
    static const BYTE expect[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20 };
    if (!BytesAt(0x138fc0, expect, sizeof expect)) return false;
    BYTE* fn = ExeBase() + 0x138fc0;
    BYTE* tr = (BYTE*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return false;
    memcpy(tr, expect, sizeof expect);                   // no RIP-relative operands in these 15 bytes
    BYTE back[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    void* cont = fn + sizeof expect;
    memcpy(back + 6, &cont, 8);
    memcpy(tr + sizeof expect, back, sizeof back);
    FlushInstructionCache(GetCurrentProcess(), tr, 64);
    g_reticleShowOrig = (PFN_ReticleShow)tr;
    BYTE jmp[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    void* hook = (void*)H_ReticleShow;
    memcpy(jmp + 6, &hook, 8);
    return WriteCode(0x138fc0, jmp, sizeof jmp);
}

// Own screen: the carry / throw / drop button prompts. Both players' prompts live in one
// shared screen, "ControllerLayoutHUD" (HudControllerLayout, vtable 0x140787e00): player 1's
// six slot elements slide in on the left, player 2's on the right (decomp session,
// re\SYNC_VIABILITY.md "Carry and throw button prompts"), so its root can't be hidden.
// Every lockstep frame the other player's six slot sprites get scale 0 instead. Render only:
// FUN_140218770 still fills and empties the slots on both PCs, and a slide-in only moves a
// slot (FUN_140219150 sets positions), it never rescales it. A level load makes new
// sprites; the old pointers are just forgotten.
// on = NetOwnScreenActive(), every lockstep frame (net_lockstep.cpp); on = false at
// the session's end puts the scales back. kPromptSlots: each player's six slot
// element ids, looked up in the screen with FUN_1404b2a00.
static const int kPromptSlots[2][6] = { { 0x10e, 0x10d, 0x10c, 0x10f, 0x10b, 0x102 },
                                        { 0x116, 0x117, 0x114, 0x115, 0x113, 0x112 } };
void HudHidePrompts(bool on) {
    bool any = false;
    for (int i = 0; i < 6; i++) any = any || g_promptEl[i];
    if (!on && !any) return;
    BYTE* b = ExeBase();
    if (!RipRefersTo(0x3416a, 3, 7, 0xb2ef10)) return;
    BYTE* mgr = *(BYTE**)(b + 0xb2ef10);
    if (!mgr) return;
    typedef BYTE* (*PFN_Find)(BYTE*, const char*, int);
    typedef BYTE* (*PFN_FindId)(BYTE*, int);
    static int logged;
    __try {
        BYTE* screen = ((PFN_Find)(b + 0x4b2630))(mgr, "ControllerLayoutHUD", -1);
        if (!screen) return;
        if (*(uintptr_t*)screen != (uintptr_t)(b + 0x787e00)) {
            if (logged < 1) { logged = 1; Log("own-screen HUD: ControllerLayoutHUD has an unexpected vtable; prompts not hidden"); }
            return;
        }
        int other = g_net.role ? 0 : 1, found = 0;
        bool fresh = false;
        for (int i = 0; i < 6; i++) {
            BYTE* el = ((PFN_FindId)(b + 0x4b2a00))(screen, kPromptSlots[other][i]);
            if (!el) continue;
            found++;
            if (on) {
                if (g_promptEl[i] != el) {
                    fresh = true;
                    g_promptEl[i] = el;
                    g_promptScale[i][0] = *(float*)(el + 0x58);
                    g_promptScale[i][1] = *(float*)(el + 0x5c);
                }
                HudSetScale(el, 0.0f, 0.0f);
            } else if (g_promptEl[i] == el) {
                HudSetScale(el, g_promptScale[i][0], g_promptScale[i][1]);
            }
        }
        if (!on) memset(g_promptEl, 0, sizeof g_promptEl);
        if (logged < 4 && found && (fresh || !on)) {
            logged++;
            Log("own-screen HUD: player %d's button prompts (%d of 6 slots) %s on this PC", other + 1, found,
                on ? "hidden" : "shown again");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// From DllMain (own_screen=1): FUN_1405f4af0 is checked whole (load the player count
// and g_SplitScreenMode, jump to elem->vf(0x1d0)) and replaced by a jump to H_HudLayout.
bool PatchHudLayout() {
    static const BYTE expect[] = { 0x4C, 0x8B, 0x05, 0x59, 0xA4, 0x53, 0x00, 0x48, 0x8B, 0x01, 0x44, 0x8B, 0x0D, 0xF7, 0x7A,
                                   0x53, 0x00, 0x45, 0x8B, 0x40, 0x04, 0x48, 0xFF, 0xA0, 0xD0, 0x01, 0x00, 0x00 };
    if (!BytesAt(0x5f4af0, expect, sizeof expect)) return false;
    BYTE jmp[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    void* target = (void*)H_HudLayout;
    memcpy(jmp + 6, &target, 8);
    return WriteCode(0x5f4af0, jmp, sizeof jmp);
}

// Audio: with own screens each PC hears only from its own player's camera.
// Only mixing changes: the voice-line wait (audio_wait) no longer asks the
// audio system, so this needs it on.
typedef int (*PFN_FmodSetNum)(void* self, int n);
typedef int (*PFN_Fmod3DAttr)(void* self, int l, const void* pos, const void* vel, const void* fwd, const void* up);
typedef int (*PFN_StudioAttr)(void* self, int l, const void* attr, const void* atten);
static PFN_FmodSetNum Real_Core3DNum, Real_StudioNum;
static PFN_Fmod3DAttr Real_Core3DAttr;
static PFN_StudioAttr Real_StudioAttr;
// Listener i follows player i's view (player 2's is added second, as 1). Until player 2
// has joined there is only listener 0, and player 2's PC hears from it too.
static volatile LONG g_audioListeners = 1;                 // how many the game asked for
static bool AudioOneListener() { return g_audioWaitOn && NetOwnScreenActive(); }   // mixing changed right now?
static int AudioMine() { return g_net.role == 1 && g_audioListeners >= 2 ? 1 : 0; } // the game's listener for this PC's player
// The hooks on both FMOD APIs (Core and Studio): the game still sets up one listener
// per player, but FMOD is given one, and only this player's position, as listener 0.
static int H_Core3DNum(void* self, int n) {
    g_audioListeners = n;
    return Real_Core3DNum(self, AudioOneListener() && n > 1 ? 1 : n);
}
static int H_StudioNum(void* self, int n) {
    g_audioListeners = n;
    return Real_StudioNum(self, AudioOneListener() && n > 1 ? 1 : n);
}
static int H_Core3DAttr(void* self, int l, const void* pos, const void* vel, const void* fwd, const void* up) {
    if (AudioOneListener()) { if (l != AudioMine()) return 0; l = 0; }
    return Real_Core3DAttr(self, l, pos, vel, fwd, up);
}
static int H_StudioAttr(void* self, int l, const void* attr, const void* atten) {
    if (AudioOneListener()) { if (l != AudioMine()) return 0; l = 0; }
    return Real_StudioAttr(self, l, attr, atten);
}
// From InstallDeterminism (audio_wait on, own_screen=1, netplay): hook the exe's
// imports of the four FMOD calls (by their C++ mangled names). Returns how many
// imports were patched; 0 if FMOD isn't loaded or an export is missing.
int InstallAudioListenerHooks() {
    HMODULE exe = GetModuleHandleW(NULL), core = GetModuleHandleW(L"fmod.dll"), studio = GetModuleHandleW(L"fmodstudio.dll");
    if (!core || !studio) return 0;
    const char* nNum = "?set3DNumListeners@System@FMOD@@QEAA?AW4FMOD_RESULT@@H@Z";
    const char* nAttr = "?set3DListenerAttributes@System@FMOD@@QEAA?AW4FMOD_RESULT@@HPEBUFMOD_VECTOR@@000@Z";
    const char* sAttr = "?setListenerAttributes@System@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@HPEBUFMOD_3D_ATTRIBUTES@@PEBUFMOD_VECTOR@@@Z";
    const char* sNum = "?setNumListeners@System@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@H@Z";
    Real_Core3DNum = (PFN_FmodSetNum)GetProcAddress(core, nNum);
    Real_Core3DAttr = (PFN_Fmod3DAttr)GetProcAddress(core, nAttr);
    Real_StudioAttr = (PFN_StudioAttr)GetProcAddress(studio, sAttr);
    Real_StudioNum = (PFN_FmodSetNum)GetProcAddress(studio, sNum);
    if (!Real_Core3DNum || !Real_Core3DAttr || !Real_StudioAttr || !Real_StudioNum) return 0;
    return PatchIAT(exe, nNum, (void*)H_Core3DNum) + PatchIAT(exe, nAttr, (void*)H_Core3DAttr) +
           PatchIAT(exe, sAttr, (void*)H_StudioAttr) + PatchIAT(exe, sNum, (void*)H_StudioNum);
}

// Sound pop fix ([harness] audio_start_paused=1, default on). A stock-game bug
// (re\STOCK_BUGS.md, decomp session): FUN_140275d70, the FMOD Core path for
// raw-FSB voices and efforts, calls System::playSound(..., paused = false, ...),
// then sets volume and pitch, and only then switches the channel to 3D with its
// min/max distance (FUN_140273390). The mixer thread can mix one block in
// between: a ~10 ms, centre-panned burst at full volume, the "crack" heard when
// e.g. Jessie charges into a crate. Here playSound always starts the channel
// paused; the game's next System::update (or Studio::System::update, whichever
// comes first) unpauses every channel started since, before the real update
// runs. The sound starts at most a frame later; isPlaying is true while paused,
// so nothing the game reads changes (no effect on the simulation).
typedef int (*PFN_PlaySound)(void* sys, void* sound, void* group, bool paused, void** channel);
typedef int (*PFN_FmodUpdate)(void* sys);
typedef int (*PFN_SetPaused)(void* ctl, bool paused);
static PFN_PlaySound g_realPlaySound;
static PFN_FmodUpdate g_realCoreUpdate, g_realStudioUpdate;
static PFN_SetPaused g_setPaused;
static SRWLOCK g_popLock = SRWLOCK_INIT;
static void* g_popPending[256];
static int g_popCount;
static volatile LONG64 g_popStarted, g_popOverflow;
// Unpause every channel started since the last FMOD update (any thread may queue).
static void PopFlush() {
    void* list[256];
    int n;
    AcquireSRWLockExclusive(&g_popLock);
    n = g_popCount;
    memcpy(list, g_popPending, n * sizeof(void*));
    g_popCount = 0;
    ReleaseSRWLockExclusive(&g_popLock);
    for (int i = 0; i < n; i++) g_setPaused(list[i], false);   // stopped or stolen meanwhile: ERR_INVALID_HANDLE, ignored
}
// System::playSound: start the channel paused and queue it for PopFlush.
static int H_PlaySound(void* sys, void* sound, void* group, bool paused, void** channel) {
    if (paused || !channel) return g_realPlaySound(sys, sound, group, paused, channel);   // already paused: the game unpauses it
    int r = g_realPlaySound(sys, sound, group, true, channel);
    if (r == 0 && *channel) {
        bool queued = false;
        AcquireSRWLockExclusive(&g_popLock);
        if (g_popCount < (int)_countof(g_popPending)) { g_popPending[g_popCount++] = *channel; queued = true; }
        ReleaseSRWLockExclusive(&g_popLock);
        if (queued) InterlockedIncrement64(&g_popStarted);
        else { InterlockedIncrement64(&g_popOverflow); g_setPaused(*channel, false); }   // list full: start it now, as the game would
    }
    return r;
}
static int H_CoreUpdate(void* sys) { PopFlush(); return g_realCoreUpdate(sys); }
static int H_StudioUpdate(void* sys) { PopFlush(); return g_realStudioUpdate(sys); }
// Hook the exe's FMOD imports. -1: FMOD not loaded or an export missing; 0: the
// update imports weren't found (playSound then left alone); else imports patched.
static int InstallSoundPopFix() {
    HMODULE exe = GetModuleHandleW(NULL), core = GetModuleHandleW(L"fmod.dll"), studio = GetModuleHandleW(L"fmodstudio.dll");
    if (!core || !studio) return -1;
    const char* nPlay = "?playSound@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAVSound@2@PEAVChannelGroup@2@_NPEAPEAVChannel@2@@Z";
    const char* nCoreUpd = "?update@System@FMOD@@QEAA?AW4FMOD_RESULT@@XZ";
    const char* nStudioUpd = "?update@System@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@XZ";
    const char* nPause = "?setPaused@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@_N@Z";
    g_realPlaySound = (PFN_PlaySound)GetProcAddress(core, nPlay);
    g_realCoreUpdate = (PFN_FmodUpdate)GetProcAddress(core, nCoreUpd);
    g_realStudioUpdate = (PFN_FmodUpdate)GetProcAddress(studio, nStudioUpd);
    g_setPaused = (PFN_SetPaused)GetProcAddress(core, nPause);
    if (!g_realPlaySound || !g_realCoreUpdate || !g_realStudioUpdate || !g_setPaused) return -1;
    // The unpause hooks first: a paused channel must never be left without one.
    int upd = PatchIAT(exe, nCoreUpd, (void*)H_CoreUpdate) + PatchIAT(exe, nStudioUpd, (void*)H_StudioUpdate);
    if (!upd) return 0;
    return upd + PatchIAT(exe, nPlay, (void*)H_PlaySound);
}
// From DllMain (last = false) and again at the first XInput poll (NoteCall,
// last = true), the last try: by then FMOD should be loaded. Installs once.
void TrySoundPopFix(bool last) {
    if (!g_popFixWanted || g_popFixDone) return;
    int n = InstallSoundPopFix();
    if (n < 0 && !last) return;                           // FMOD not loaded yet: the first poll tries again
    g_popFixDone = true;
    Log("audio: sound pop fix %s", n >= 3 ? "on: voices and efforts start paused and are unpaused at the next FMOD update"
                                          : n < 0 ? "NOT available (FMOD calls not found)" : "NOT installed (imports not found)");
}
