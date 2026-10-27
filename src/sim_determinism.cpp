// sim_determinism.cpp -- determinism on one PC: the engine clock override, the engine Mersenne
// Twister trace and the private UI/dialog/cosmetic streams, the attract-video and desktop-input
// patches, rand() isolation and the spawn gate.
//
// "Deterministic" here means: given the same inputs, the game simulates the same thing,
// frame for frame. Record/replay (harness_core.cpp) needs it to compare two runs; online
// co-op needs it because each PC runs its own copy of the game in lockstep (both PCs step
// the same frame with the same two players' input, and only the input is sent), so any
// difference in time, random numbers or spawn timing makes the two worlds drift apart.
// net_determinism.cpp holds the fixes that only matter in a co-op session.
//
// Game-specific numbers: an address like 0x14025a5e0 is a function or variable in the
// game's exe, found by reverse engineering it (the "decomp session", notes in re\). The
// exe loads at 0x140000000, so code uses RVAs (offsets from the exe's base, e.g. 0x25a5e0).
// Before patching, each Verify* / Patch* function checks the exact bytes there (the
// "expect" arrays), so a different game build is left alone instead of corrupted.
//
// Entry points: DllMain (xinput_proxy.cpp) verifies and installs everything here at
// startup; after that the hooks are called by the game itself (EngineNow for every clock
// read, H_MTNext for every engine random number, H_rand for rand(), GateUpdate once per
// frame). net_lockstep.cpp reseeds the generators and resets the clock at session start;
// CloseRun logs the summaries at exit.

#include "proxy_shared.h"

bool     g_engineClockReady;  // game clock function verified and replaced
bool     g_spawnGateOn;       // spawn gate verified and installed (see spawn gate)
uint64_t g_engineFrames;      // frames ticked by GameTimer_Tick
LONG64   g_mtMainDraws;       // engine RNG draws on the main thread (see engine RNG tracing)

// ------------------------------------------------------------------ engine clock
//
// Found by decompiling the game (see re\SYNC_VIABILITY.md). Clock_GetTimeMs
// (0x14025a5e0) is the game's clock. About 60 places call it, including
// GameTimer_Tick, which turns it into each frame's dt. We replace the whole
// function with EngineNow() via a 14-byte absolute jump at its entry:
//
//  - called from GameTimer_Tick (recognised by return address): a new frame.
//    Time moves to exactly one frame after the previous frame started.
//  - any other call on the main thread: +1 us, or +1 ms once this frame has
//    made more than kSpinReads reads. Busy-waits on game time, such as the
//    350 ms music fade before a level change (FUN_140269c30), so end after
//    the same number of reads in every run instead of hanging.
//  - other threads: the current time, unchanged.
//
// Time starts at the same fixed value in every run. The engine's override
// flag is set and its value kept current too, because Clock_FrameTick reads
// that value directly rather than calling the function.

// Addresses for this game build. Overridable from the ini (engine_rva_*) only
// so the test stand-in can point them at its own copy of the code pattern.
DWORD          RVA_CLOCK_FN  = 0x25a5e0;
DWORD          RVA_OVR_FLAG  = 0x9c27b9;   // byte: nonzero = the clock returns the override value
DWORD          RVA_OVR_MS    = 0x9c2918;   // the override value (ms)
DWORD          RVA_TICK_CALL = 0x4779df;   // inside GameTimer_Tick: call Clock_GetTimeMs
static const uint32_t kSpinReads    = 5000;

static volatile uint8_t*  g_ovrFlag;
static volatile uint32_t* g_ovrMs;
static uintptr_t          g_tickRet;       // return address of GameTimer_Tick's call: "a new frame"
volatile LONG64    g_vus;                  // virtual microseconds since start
LONG64             g_frameStartUs;
volatile ULONGLONG g_frameWallStart;       // real time the current engine frame began (long-frame profiler)
// Set while a co-op session's lockstep runs: each frame then starts exactly
// 1/fps after the last, and time a busy-wait pushed forward inside a frame is
// NOT carried into the next one (how far a spin pushes depends on real time,
// which differs between PCs).
volatile bool      g_clockLockstep;
// FUN_140479160 (run after every movie) stores Clock_GetTimeMs() into the game
// timer as "last frame time"; the next tick's dt is measured from it. In a
// session it gets the frame's start time, never the drifted in-frame clock.
uintptr_t          g_fmvTimerRet;
// The FadeSystem update (FUN_140473320) times every screen fade from its one
// Clock_GetTimeMs read. Main-thread reads move the clock 1 us each, and the
// number of reads before it differs between PCs (rendering reads the clock
// too), so near a millisecond boundary a fade ended a frame apart: the level
// graph's "Fade Out End" then started a story movie on different frames. In a
// session this read also gets the frame's start time.
uintptr_t          g_fadeClockRet;
// That pin wasn't enough: the FadeSystem update doesn't run on the main thread at all.
// Its only caller, FUN_1401ef390, handles a render-list item that the frame's setup job
// records, so it runs when the render thread executes the list, concurrently with the
// main thread's next update. Whether frame N's step lands before the level graph polls
// "fade done" (FadeAllPlayerScreens -> Fade Out End -> 02_WoodyTrain; Respawn's state
// machine; AlleyToss, PlaceAvatarFadeScreen...) was a race: a story movie started, or an
// avatar respawned, an update apart on two PCs with identical sim state (decomp session,
// re\SYNC_VIABILITY.md "Screen fades are advanced by a render-list item"). In a session
// that call is skipped and NetFadeStep runs the update on the main thread once per
// lockstep step instead. Its overlay colour write (FUN_1402c5af0) then races the
// renderer instead: visual only.
bool               g_fadeOnMain;      // the render-list call is redirected (H_FadeStepRender)
// More gameplay reads pinned to the frame's start time in a session (decomp
// session's review): the choreography node's 3 s give-up (stamp FUN_140695ae0,
// check FUN_1406c2850), Buzz's stick-release direction latch (FUN_14027e050),
// the screen manager's per-view time (FUN_1405fa740, 5 reads) and the
// camera-shoot minigame timer (FUN_140206f70). Each is a "now + x" deadline
// later compared with "now", so with frame-start time it's an exact frame count.
uintptr_t          g_pinnedClockRets[16];
int                g_pinnedClockN;
static bool IsPinnedClockRet(uintptr_t ra) {
    for (int i = 0; i < g_pinnedClockN; i++) if (g_pinnedClockRets[i] == ra) return true;
    return false;
}
// Clock reads from code whose read count depends on real time (audio fades
// and timers, 0x140265000..0x140276000; the data-file flush FUN_14056cd40) get
// their own in-frame offset in a session: time moves for them, so their loops
// end, but every other reader (script timers, cutscene starts, timeouts) sees a
// clock that only deterministic reads have moved.
static uintptr_t          g_clockExe;
static LONG64             g_privateUs;
static uint32_t           g_privateReads;
uint32_t           g_readsThisFrame, g_maxReadsPerFrame;
static uint64_t           g_spinReads;
static volatile LONG64    g_otherReads;
static volatile LONG      g_engineStallAssists;
static DWORD              g_tickThread;
static bool               g_tickThreadWarned;

// Check the exact instructions before trusting any address: a game update
// would move them, and patching a stale address would corrupt the game.
// Also checks that the function's RIP-relative operands point at the override
// flag and value, and that GameTimer_Tick's call targets it. Called from DllMain.
bool VerifyEngineClock() {
    BYTE* base = (BYTE*)GetModuleHandleW(NULL);
    BYTE* fn = base + RVA_CLOCK_FN;
    BYTE* call = base + RVA_TICK_CALL;
    __try {
        if (fn[0] != 0x48 || fn[1] != 0x83 || fn[2] != 0xEC) return false;      // sub rsp, imm8
        if (fn[4] != 0x80 || fn[5] != 0x3D || fn[10] != 0x00) return false;     // cmp byte ptr [rip+d], 0
        BYTE* flag = fn + 4 + 7 + *(int32_t*)(fn + 6);
        if (fn[0xB] != 0x74) return false;                                       // je
        if (fn[0xD] != 0x8B || fn[0xE] != 0x05) return false;                    // mov eax, [rip+d]
        BYTE* ms = fn + 0xD + 6 + *(int32_t*)(fn + 0xF);
        if (flag != base + RVA_OVR_FLAG || ms != base + RVA_OVR_MS) return false;
        if (call[0] != 0xE8 || call + 5 + *(int32_t*)(call + 1) != fn) return false;  // call Clock_GetTimeMs
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    g_ovrFlag = base + RVA_OVR_FLAG;
    g_ovrMs   = (volatile uint32_t*)(base + RVA_OVR_MS);
    g_tickRet = (uintptr_t)(call + 5);
    return true;
}

// Store the current virtual time (kStartMs + g_vus in ms) in the engine's override
// value, where Clock_FrameTick reads it, and return it. "Published" time is the
// clock every thread sees; a private spinner's offset (below) never is.
uint32_t PublishMs() {
    uint32_t ms = kStartMs + (uint32_t)(g_vus / 1000);
    *g_ovrMs = ms;
    return ms;
}

// The replacement Clock_GetTimeMs, by the rules at the top of this section. The
// caller is told apart by _ReturnAddress() (noinline keeps that the game's address).
static __declspec(noinline) uint32_t EngineNow() {
    // In a session: the pinned readers get the frame's start time.
    if (g_clockLockstep) {
        uintptr_t ra = (uintptr_t)_ReturnAddress();
        if ((g_fmvTimerRet && ra == g_fmvTimerRet) || (g_fadeClockRet && ra == g_fadeClockRet) || IsPinnedClockRet(ra))
            return kStartMs + (uint32_t)(g_frameStartUs / 1000);
    }
    if (g_clockLockstep && GetCurrentThreadId() == g_mainThread) {
        if (!g_clockExe) g_clockExe = (uintptr_t)GetModuleHandleW(NULL);
        uintptr_t r = (uintptr_t)_ReturnAddress() - g_clockExe;
        // Spinners that need time to move: the audio module and the data-file
        // flush (FUN_14056cd40). The streamer's parse slice (FUN_14055eb00) is
        // NOT one of them: it steps a definition-file parser for "3 ms of clock",
        // so with any advancing clock the records parsed per frame depend on the
        // read count, and a file finishes a frame apart on two PCs (and only a
        // few records per frame once reads cost 1 ms each: 40-120 s loads). Its
        // two reads are pinned to the frame start instead, so each slice parses
        // the file to completion, as the blocking dblLoad does.
        if ((r >= 0x265000 && r < 0x276000) || (r >= 0x56cd40 && r < 0x56ce40)) {
            g_privateUs += ++g_privateReads > kSpinReads ? 1000 : 1;
            return kStartMs + (uint32_t)((g_vus + g_privateUs) / 1000);   // not published, not shared
        }
    }
    if ((uintptr_t)_ReturnAddress() == g_tickRet) {
        // GameTimer_Tick: start the next frame exactly 1/fps after the last.
        DWORD t = GetCurrentThreadId();
        if (!g_tickThread) g_tickThread = t;
        else if (t != g_tickThread) g_tickThreadWarned = true;
        LONG64 a = (LONG64)((double)g_engineFrames * 1e6 / g_cfg.fps);
        LONG64 b = (LONG64)((double)(g_engineFrames + 1) * 1e6 / g_cfg.fps);
        g_engineFrames++;
        g_frameWallStart = GetTickCount64();
        g_privateUs = 0;
        g_privateReads = 0;
        if (g_readsThisFrame > g_maxReadsPerFrame) g_maxReadsPerFrame = g_readsThisFrame;
        g_readsThisFrame = 0;
        LONG64 next = g_frameStartUs + (b - a);
        if (!g_clockLockstep && next < g_vus) next = g_vus;   // solo: a spin already pushed time past it
        InterlockedExchange64(&g_vus, next);
        g_frameStartUs = next;
        InterlockedExchange64(&g_otherReads, 0);
        return PublishMs();
    }
    // Other threads read without moving time (except the stall assist).
    if (GetCurrentThreadId() != g_mainThread) {
        // Another thread spinning on game time while the main thread is
        // blocked would never see it move. After a million fruitless reads,
        // let it move time itself, and count it: the run may not reproduce.
        LONG64 n = InterlockedIncrement64(&g_otherReads);
        if (n > 1000000) {
            if (n == 1000001) InterlockedIncrement(&g_engineStallAssists);
            InterlockedAdd64(&g_vus, 1000);
            return PublishMs();
        }
        return kStartMs + (uint32_t)(g_vus / 1000);
    }
    // Any other main-thread read: +1 us, or +1 ms past kSpinReads in this frame.
    InterlockedExchange64(&g_otherReads, 0);
    g_readsThisFrame++;
    if (g_readsThisFrame > kSpinReads) {
        g_spinReads++;
        InterlockedAdd64(&g_vus, 1000);
    } else {
        InterlockedAdd64(&g_vus, 1);
    }
    return PublishMs();
}

// Overwrite Clock_GetTimeMs's entry with a 14-byte absolute jump to EngineNow
// (jmp [rip+0], then the 8-byte target), and switch the engine's override on
// at kStartMs. Called from DllMain after VerifyEngineClock.
bool PatchEngineClock() {
    BYTE* fn = (BYTE*)GetModuleHandleW(NULL) + RVA_CLOCK_FN;
    BYTE jmp[14] = { 0xFF, 0x25, 0, 0, 0, 0 };      // jmp qword ptr [rip+0]
    void* target = (void*)EngineNow;
    memcpy(jmp + 6, &target, 8);
    *g_ovrMs = kStartMs;
    *g_ovrFlag = 1;
    DWORD old;
    if (!VirtualProtect(fn, sizeof jmp, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(fn, jmp, sizeof jmp);
    VirtualProtect(fn, sizeof jmp, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, sizeof jmp);
    return true;
}

// At exit (CloseRun): game time and frames, and any warning that a run may not
// reproduce.
void LogEngineClockSummary() {
    Log("summary: engine clock %.1f s of game time over %llu frames; most clock reads in one frame %u; "
        "%llu reads took the spin path", (double)g_vus / 1e6, g_engineFrames, g_maxReadsPerFrame, g_spinReads);
    if (g_tickThread && g_tickThread != g_mainThread)
        Log("WARNING: GameTimer_Tick ran on thread %lu, not the main thread %lu", g_tickThread, g_mainThread);
    if (g_tickThreadWarned)
        Log("WARNING: GameTimer_Tick ran on more than one thread");
    if (g_engineStallAssists)
        Log("WARNING: %ld stall assist(s): a background thread waited on game time while the main thread was "
            "blocked. This run may not be reproducible.", g_engineStallAssists);
}

// ------------------------------------------------------------------ engine RNG tracing
//
// The engine's own random generator (see re\SYNC_VIABILITY.md) is a
// Mersenne Twister. Every draw, from the global instance or a private one,
// goes through one routine, MT_Next (0x14025ded0): take the next state word,
// refill the 624-word state via MT_Refill (0x14025e020) when it runs out,
// temper, return. We replace MT_Next with a bit-exact copy that also records
// who asked:
//  - per caller totals -> mt_callers.csv
//  - draws per frame   -> io.csv "mt" column
//  - every main-thread draw (frame, caller, caller's caller, which generator,
//    during a load or not) -> mt_draws.bin, so diff_mt.py can find the first
//    draw that differs between two runs.
// When the immediate caller is a tiny wrapper (e.g. the float helper
// 0x14012be40), its caller is recorded too, found with the unwind tables.

DWORD RVA_MT_NEXT   = 0x25ded0;
DWORD RVA_MT_REFILL = 0x25e020;
DWORD RVA_MT_GLOBAL = 0x9c2e58;   // pointer to the global generator instance
// Code range of the random-number module (MT_Next, seeding, float/range
// helpers). A caller inside it is a helper, not the game code asking.
DWORD RVA_MT_HELPERS_LO = 0x25c000;
DWORD RVA_MT_HELPERS_HI = 0x25f000;

PFN_MTRefill g_mtRefill;
void* volatile* g_mtGlobalPtr;
bool           g_mtHooked;
static uintptr_t      g_exeBase;
static volatile LONG64 g_mtOtherDraws;
LONG64         g_mtGlobalDraws;
static LONG64  g_mtPrivateDraws;
FILE*          g_mtDrawFile;
static bool           g_mtUnwindWarned;

RngFrameDraws g_rngDraws[kRngDrawFrames];
int  g_rngDrawCur;
bool g_rngDrawOn;

struct MtSlot { uintptr_t r0, r1; uint32_t global, used; LONG64 main, other; };
static MtSlot  g_mtTable[8192];
static LONG64  g_mtOverflow;
struct WrapSlot { uintptr_t ret; int tiny; };   // tiny: 0 unknown, 1 yes, 2 no
static WrapSlot g_mtWrap[2048];
SRWLOCK g_mtLock = SRWLOCK_INIT;

uint32_t Rva(uintptr_t a) { return a ? (uint32_t)(a - g_exeBase) : 0; }   // exe address -> RVA (0 stays 0)

// Is ret inside a function of at most 0x40 bytes (a thin wrapper around MT_Next)?
// Function sizes come from the exe's x64 unwind tables; answers are cached in
// g_mtWrap by return address.
static bool IsTinyCaller(uintptr_t ret) {
    size_t h = (ret >> 2) & 2047;
    for (int i = 0; i < 2048; i++, h = (h + 1) & 2047) {
        WrapSlot& s = g_mtWrap[h];
        if (s.ret == ret) return s.tiny == 1;
        if (!s.ret) {
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ret, &imageBase, NULL);
            s.ret = ret;
            s.tiny = (rf && rf->EndAddress - rf->BeginAddress <= 0x40) ? 1 : 2;
            return s.tiny == 1;
        }
    }
    return false;
}

// A helper, not the game code that wanted the number: inside the RNG module, or tiny.
static bool IsMtHelper(uintptr_t ret) {
    uintptr_t rva = ret - g_exeBase;
    if (ret >= g_exeBase && rva >= RVA_MT_HELPERS_LO && rva < RVA_MT_HELPERS_HI) return true;
    return IsTinyCaller(ret);
}

// Count one draw for (caller, real caller, generator) in g_mtTable (open addressing),
// for mt_callers.csv. Under g_mtLock. A full table only counts the overflow.
static void MtRecord(uintptr_t r0, uintptr_t r1, bool global, bool main) {
    size_t h = ((r0 >> 2) ^ (r1 * 31) ^ (global ? 0x5bd1e995 : 0)) & 8191;
    for (int i = 0; i < 8192; i++, h = (h + 1) & 8191) {
        MtSlot& s = g_mtTable[h];
        if (s.used && s.r0 == r0 && s.r1 == r1 && s.global == (uint32_t)global) {
            if (main) s.main++; else s.other++;
            return;
        }
        if (!s.used) {
            s.used = 1; s.r0 = r0; s.r1 = r1; s.global = global;
            if (main) s.main = 1; else s.other = 1;
            return;
        }
    }
    g_mtOverflow++;
}

#pragma pack(push, 1)
struct MtDraw { uint32_t logical, r0, r1, flags; };   // flags: bit0 global, bit1 not live (loading/holding)
#pragma pack(pop)

// UI code that draws from the global engine RNG on a real-time schedule gets a
// private generator in netplay, so it can't move the simulation's stream:
// FUN_1401305d0, the loading-tip picker (ScreenMiniLoad calls it every few
// real seconds while a load runs).
uint8_t* g_uiMt;
uint64_t g_uiMtDraws;
// Also FUN_140271af0, the ambient sound emitter: when its sound has really
// ended (real-time FMOD state) it replays it and draws the next interval; and
// FUN_1402704e0, the interval picker of FUN_1402711c0's replay of a sound once an
// earlier one has ended (decomp session's class sweep, C3).
static bool IsUiMtCaller(uintptr_t addr) {
    if (!addr) return false;
    uintptr_t r = addr - g_exeBase;
    return (r >= 0x1305d0 && r < 0x130790) || (r >= 0x271af0 && r < 0x271d06) || (r >= 0x2704e0 && r < 0x270522);
}
static uint8_t* UiMt();                           // defined after g_mtSeed
// The dialog take picker in the AudioEvent constructor (FUN_140260440) draws
// take = draw % N for every dialog line, also for lines that audio emitters
// replay in real time. It only picks the recording and subtitle, so it gets
// its own generator, reseeded at session start: both PCs usually play the
// same take, and when they don't, only the audio differs.
uint8_t* g_dlgMt;
uint64_t g_dlgMtDraws;
static bool IsDialogMtCaller(uintptr_t addr) {
    if (!addr) return false;
    uintptr_t r = addr - g_exeBase;
    return r >= 0x260440 && r < 0x2610b5;
}
// Cosmetic script draws: the player's eye-blink loop (player4.sx, function
// Blink) picks a blink animation with GetRandomInt (pc 0x6b) and the pause
// with GetRandomReal (pc 0xaf), timed by MotionEnded of the blink animation.
// Purely visual, but on the shared MT it shifted every later draw when the
// timing differed. The script-native hooks raise this flag for exactly those
// two calls; H_MTNext then draws from a private generator, reseeded at session
// start like the dialogue one.
bool g_cosmeticDraw;
static uint64_t g_cosmeticDraws;

// The replacement MT_Next (PatchMt jumps here), called on any thread for every
// engine random number. inst is the generator. Finds who asked, moves netplay's
// UI, dialog and cosmetic draws to their private generators, records the draw,
// then draws exactly as the game would.
__declspec(noinline) uint32_t H_MTNext(uint8_t* inst) {
    uintptr_t r0 = (uintptr_t)_ReturnAddress(), r1 = 0;
    bool global = g_mtGlobalPtr && *g_mtGlobalPtr == inst;
    bool main = GetCurrentThreadId() == g_mainThread;

    AcquireSRWLockExclusive(&g_mtLock);
    if (IsMtHelper(r0)) {
        // Walk up past helpers (tiny wrappers, or anything in the RNG module)
        // to the first real game function: that's the caller we record.
        void* fr[8] = {};
        USHORT n = RtlCaptureStackBackTrace(1, 8, fr, NULL);
        if (n >= 1 && (uintptr_t)fr[0] == r0) {
            for (USHORT i = 1; i < n; i++)
                if (!IsMtHelper((uintptr_t)fr[i])) { r1 = (uintptr_t)fr[i]; break; }
        }
        if (!r1) g_mtUnwindWarned = true;
    }
    // Draws that must not touch the shared global generator.
    if (global && g_cfg.mode == MODE_NETPLAY && (IsUiMtCaller(r0) || IsUiMtCaller(r1)))
        if (uint8_t* ui = UiMt()) { inst = ui; global = false; g_uiMtDraws++; }
    if (global && g_cfg.mode == MODE_NETPLAY && (IsDialogMtCaller(r0) || IsDialogMtCaller(r1)))
        if (uint8_t* dm = DialogMt(false)) { inst = dm; global = false; g_dlgMtDraws++; }
    if (global && g_cosmeticDraw && main)
        if (uint8_t* cm = CosmeticMt(false)) { inst = cm; global = false; g_cosmeticDraws++; }
    // Record it: caller table, the per-frame RNG check's draw list, mt_draws.bin.
    MtRecord(r0, r1, global, main);
    if (global) g_mtGlobalDraws++; else g_mtPrivateDraws++;
    if (global && g_rngDrawOn) {
        RngFrameDraws& b = g_rngDraws[g_rngDrawCur];
        if (b.n < _countof(b.d)) b.d[b.n] = { Rva(r0), Rva(r1), (uint8_t)main };
        b.n++;
    }
    if (main) {
        g_mtMainDraws++;
        if (g_mtDrawFile) {
            MtDraw d = { g_logical, Rva(r0), Rva(r1), (global ? 1u : 0u) | (g_frameLive ? 0u : 2u) };
            fwrite(&d, sizeof d, 1, g_mtDrawFile);
        }
    } else {
        g_mtOtherDraws++;
    }
    ReleaseSRWLockExclusive(&g_mtLock);

    // Bit-exact copy of the game's MT_Next.
    int32_t* left = (int32_t*)(inst + 0x9c8);            // state words left before a refill
    if (*left == 0) g_mtRefill(inst);
    uint32_t** cur = (uint32_t**)(inst + 0x9c0);         // next state word
    (*left)--;
    uint32_t y = **cur;
    (*cur)++;
    y ^= y >> 11;
    y ^= (y & 0xFF3A58ADu) << 7;
    y ^= (y & 0xFFFFDF8Cu) << 15;
    y ^= y >> 18;
    return y;
}

// Byte check of MT_Next's entry (and that its call goes to MT_Refill); on success
// remember MT_Refill and the global generator's pointer. From DllMain: for the
// trace (mt_trace=1), and in netplay always, since the reseed needs them.
bool VerifyMt() {
    BYTE* base = (BYTE*)GetModuleHandleW(NULL);
    BYTE* fn = base + RVA_MT_NEXT;
    static const BYTE expect[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x83, 0xB9, 0xC8, 0x09, 0x00, 0x00, 0x00,
                                   0x48, 0x8B, 0xD9, 0x75, 0x05, 0xE8 };
    __try {
        if (memcmp(fn, expect, sizeof expect) != 0) return false;
        if (fn + 23 + *(int32_t*)(fn + 19) != base + RVA_MT_REFILL) return false;   // call MT_Refill
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    g_exeBase = (uintptr_t)base;
    g_mtRefill = (PFN_MTRefill)(base + RVA_MT_REFILL);
    g_mtGlobalPtr = (void* volatile*)(base + RVA_MT_GLOBAL);
    return true;
}

// MT_Next's entry becomes a 14-byte absolute jump to H_MTNext.
bool PatchMt() {
    BYTE* fn = (BYTE*)GetModuleHandleW(NULL) + RVA_MT_NEXT;
    BYTE jmp[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    void* target = (void*)H_MTNext;
    memcpy(jmp + 6, &target, 8);
    DWORD old;
    if (!VirtualProtect(fn, sizeof jmp, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(fn, jmp, sizeof jmp);
    VirtualProtect(fn, sizeof jmp, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, sizeof jmp);
    return true;
}

// MT19937_Seed (rcx = generator, edx = seed): the engine's own init, used
// with MT_Refill to reseed the global generator identically on both PCs
// when lockstep starts. The title screen draws from it a different number
// of times on each PC, depending on how long each player waited.
static DWORD RVA_MT_SEED = 0x25cbd0;
PFN_MTSeed g_mtSeed;
// The private generators below are 0xa00-byte blocks laid out like the engine's,
// so the engine's own seed and refill (and H_MTNext's draw) work on them. Each is
// created on first use with its own fixed seed. UiMt is never reseeded; DialogMt
// and CosmeticMt are, with reseed = true, at lockstep start (net_lockstep.cpp).
static uint8_t* UiMt() {
    if (!g_uiMt && g_mtSeed && g_mtRefill) {
        g_uiMt = (uint8_t*)_aligned_malloc(0xa00, 16);
        if (g_uiMt) {
            memset(g_uiMt, 0, 0xa00);
            g_mtSeed(g_uiMt, 0x00C0FFEE);
            g_mtRefill(g_uiMt);
        }
    }
    return g_uiMt;
}
uint8_t* DialogMt(bool reseed) {               // reseed: under g_mtLock
    if (!g_dlgMt && g_mtSeed && g_mtRefill) {
        g_dlgMt = (uint8_t*)_aligned_malloc(0xa00, 16);
        if (g_dlgMt) { memset(g_dlgMt, 0, 0xa00); reseed = true; }
    }
    if (g_dlgMt && reseed) {
        g_mtSeed(g_dlgMt, 0x00D1A106);
        g_mtRefill(g_dlgMt);
    }
    return g_dlgMt;
}
uint8_t* g_cosMt;
uint8_t* CosmeticMt(bool reseed) {             // reseed: under g_mtLock
    if (!g_cosMt && g_mtSeed && g_mtRefill) {
        g_cosMt = (uint8_t*)_aligned_malloc(0xa00, 16);
        if (g_cosMt) { memset(g_cosMt, 0, 0xa00); reseed = true; }
    }
    if (g_cosMt && reseed) {
        g_mtSeed(g_cosMt, 0x00B11C);
        g_mtRefill(g_cosMt);
    }
    return g_cosMt;
}
// Byte check of MT19937_Seed's first instructions; on success g_mtSeed is usable.
bool VerifyMtSeed() {
    static const BYTE expect[] = { 0x89, 0x11, 0x48, 0x8D, 0x41, 0x04 };   // mov [rcx],edx; lea rax,[rcx+4]
    BYTE* fn = (BYTE*)GetModuleHandleW(NULL) + RVA_MT_SEED;
    __try {
        if (memcmp(fn, expect, sizeof expect) != 0) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    g_mtSeed = (PFN_MTSeed)fn;
    return true;
}

// The title screen (ScreenFrontend::vf37) starts the attract video after 15 s
// of engine time at "Press Start". In netplay the game sees a neutral pad
// while players connect, so it would always fire. Turn the jbe into a jmp.
static const DWORD RVA_ATTRACT_CMP = 0x126852;
bool PatchAttractVideo() {
    // cmp eax,15000 (0x3a98 ms); jbe +0x18; mov qword [r15+290h],... The jbe (byte 5) becomes jmp (0xEB).
    static const BYTE expect[] = { 0x3D, 0x98, 0x3A, 0x00, 0x00, 0x76, 0x18, 0x49, 0xC7, 0x87, 0x90, 0x02, 0x00, 0x00, 0x08 };
    BYTE* p = (BYTE*)GetModuleHandleW(NULL) + RVA_ATTRACT_CMP;
    __try {
        if (memcmp(p, expect, sizeof expect) != 0) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    DWORD old;
    if (!VirtualProtect(p + 5, 1, PAGE_EXECUTE_READWRITE, &old)) return false;
    p[5] = 0xEB;
    VirtualProtect(p + 5, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p + 5, 1);
    return true;
}
bool g_attractOff, g_mtReseedOk;

// WindowsSystemInputDriver init sets driver+0x501 = 1 (C6 83 01 05 00 00 01 at
// 0x14027efa5). With it 0, Update skips DirectInput keyboard and mouse
// entirely: they never become "active" (no rebinding a player to them), and
// an unfocused game keeps polling its pads (a failed keyboard read in the
// background otherwise returns before the pad loop, stalling lockstep).
bool g_noDesktopInput;
bool PatchNoDesktopInput() {
    static const BYTE expect[] = { 0xC6, 0x83, 0x01, 0x05, 0x00, 0x00, 0x01 };
    BYTE* p = (BYTE*)GetModuleHandleW(NULL) + 0x27efa5;
    __try {
        if (memcmp(p, expect, sizeof expect) != 0) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    DWORD old;
    if (!VirtualProtect(p + 6, 1, PAGE_EXECUTE_READWRITE, &old)) return false;
    p[6] = 0x00;
    VirtualProtect(p + 6, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p + 6, 1);
    return true;
}

// mt_draws.bin in the run folder (MtDraw records, 1 MB write buffer).
void OpenMtDrawFile() {
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\mt_draws.bin", g_outDir);
    if (!_wfopen_s(&g_mtDrawFile, path, L"wb") && g_mtDrawFile)
        setvbuf(g_mtDrawFile, NULL, _IOFBF, 1 << 20);
}

// At exit (CloseRun): close mt_draws.bin, write mt_callers.csv (busiest callers
// first) and log the totals and the top 12 callers.
void WriteMtSummary() {
    if (!g_mtHooked) return;
    AcquireSRWLockExclusive(&g_mtLock);
    if (g_mtDrawFile) { fclose(g_mtDrawFile); g_mtDrawFile = NULL; }
    Log("summary: engine RNG draws: %lld global, %lld private; %lld on the main thread, %lld on others",
        g_mtGlobalDraws, g_mtPrivateDraws, g_mtMainDraws, (LONG64)g_mtOtherDraws);
    if (g_mtOverflow) Log("WARNING: engine RNG caller table full; %lld draws not attributed", g_mtOverflow);
    if (g_mtUnwindWarned) Log("NOTE: some draws came through helpers whose real caller couldn't be found (chain over 8 "
                              "deep or unwind failed); their caller is blank");

    // Sort by total, write the CSV, log the top callers.
    static int order[8192];
    int n = 0;
    for (int i = 0; i < 8192; i++) if (g_mtTable[i].used) order[n++] = i;
    for (int i = 1; i < n; i++)                 // insertion sort: n is small
        for (int j = i; j > 0; j--) {
            const MtSlot& a = g_mtTable[order[j - 1]];
            const MtSlot& b = g_mtTable[order[j]];
            if (a.main + a.other >= b.main + b.other) break;
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\mt_callers.csv", g_outDir);
    FILE* f = NULL;
    if (!_wfopen_s(&f, path, L"w") && f) {
        fputs("caller,via,generator,main,other\n", f);
        for (int i = 0; i < n; i++) {
            const MtSlot& s = g_mtTable[order[i]];
            fprintf(f, "%llx,%llx,%s,%lld,%lld\n", 0x140000000ULL + Rva(s.r0), s.r1 ? 0x140000000ULL + Rva(s.r1) : 0ULL,
                    s.global ? "global" : "private", s.main, s.other);
        }
        fclose(f);
    }
    Log("top engine RNG callers (immediate caller, then the real game caller behind helpers; main / other threads):");
    for (int i = 0; i < n && i < 12; i++) {
        const MtSlot& s = g_mtTable[order[i]];
        Log("  mt@%llx via %llx %s: %lld / %lld", 0x140000000ULL + Rva(s.r0),
            s.r1 ? 0x140000000ULL + Rva(s.r1) : 0ULL, s.global ? "global" : "private", s.main, s.other);
    }
    ReleaseSRWLockExclusive(&g_mtLock);
}

// ------------------------------------------------------------------ rand() isolation
//
// The game calls the C runtime's rand() from 12 places, including audio,
// all sharing one sequence. If audio draws a different number of values in
// two runs, every later gameplay value shifts. Here each call site, found by
// scanning the game's code for calls through the rand import, gets its own
// generator (the same LCG as MSVC's rand, seeded with rand's default of 1).

RandSite         g_randSites[64];
int              g_randSiteCount;
volatile LONG    g_randFallbackState = 1;
static volatile LONG    g_randFallbackCalls;
static bool             g_randHooked;

// MSVC's rand() step on one stream's state (atomic, as threads may share a site).
static int NextRand(volatile LONG* state) {
    LONG cur, next;
    do {
        cur = *state;
        next = (LONG)((ULONG)cur * 214013u + 2531011u);
    } while (InterlockedCompareExchange(state, next, cur) != cur);
    return (int)(((ULONG)next >> 16) & 0x7FFF);
}

// The game's rand(): find the call site by return address (binary search of the
// sorted g_randSites) and draw from its stream. Callers not found by the scan
// share one fallback stream.
static int __cdecl H_rand() {
    uintptr_t ra = (uintptr_t)_ReturnAddress();
    int lo = 0, hi = g_randSiteCount - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_randSites[mid].ret == ra) {
            RandSite& s = g_randSites[mid];
            InterlockedIncrement(IsClockThread() ? &s.mainCalls : &s.otherCalls);
            return NextRand(&s.state);
        }
        if (g_randSites[mid].ret < ra) lo = mid + 1; else hi = mid - 1;
    }
    InterlockedIncrement(&g_randFallbackCalls);
    return NextRand(&g_randFallbackState);
}

// The import address table slot through which mod calls the named import, or NULL.
static ULONG_PTR* FindIatSlot(HMODULE mod, const char* name) {
    BYTE* base = (BYTE*)mod;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return NULL;
    for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (!imp->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA* funcs = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, funcs++) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            if (!strcmp(((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name, name))
                return (ULONG_PTR*)&funcs->u1.Function;
        }
    }
    return NULL;
}

// From DllMain (rand_isolate=1): list the call sites, then hook the rand import.
void InstallRandIsolation() {
    HMODULE exe = GetModuleHandleW(NULL);
    BYTE* base = (BYTE*)exe;
    ULONG_PTR* slot = FindIatSlot(exe, "rand");
    if (!slot) { Log("rand isolation: the game doesn't import rand; nothing to do"); return; }

    // Every "call qword ptr [rip+disp32]" (FF 15) whose target is the slot.
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        BYTE* p = base + sec->VirtualAddress;
        DWORD n = sec->Misc.VirtualSize;
        for (DWORD j = 0; j + 6 <= n; j++) {
            if (p[j] != 0xFF || p[j + 1] != 0x15) continue;
            BYTE* next = p + j + 6;
            if (next + *(int32_t*)(p + j + 2) != (BYTE*)slot) continue;
            if (g_randSiteCount < 64) {
                g_randSites[g_randSiteCount].ret = (uintptr_t)next;
                g_randSites[g_randSiteCount].state = 1;
                g_randSiteCount++;
            }
        }
    }
    // Sections are scanned in address order, so the list is already sorted.
    if (PatchIAT(exe, "rand", (void*)H_rand) > 0) g_randHooked = true;
    Log("rand isolation: %d call sites, each with its own stream", g_randSiteCount);
}

// At exit (CloseRun): calls per site. A site's address is its call instruction's
// (6 bytes before the return address).
void LogRandSummary() {
    if (!g_randHooked) return;
    BYTE* base = (BYTE*)GetModuleHandleW(NULL);
    Log("rand calls per call site (address as in Ghidra; main thread / other threads):");
    for (int i = 0; i < g_randSiteCount; i++) {
        const RandSite& s = g_randSites[i];
        Log("  rand@%llx: %ld / %ld", 0x140000000ULL + (s.ret - 6 - (uintptr_t)base), s.mainCalls, s.otherCalls);
    }
    if (g_randFallbackCalls) Log("  unrecognised callers: %ld", g_randFallbackCalls);
}

// ------------------------------------------------------------------ spawn gate
//
// Characters and objects are streamed in by a background loader whose
// per-frame time slice is measured in real time, so a spawn can finish
// loading a frame earlier or later from one run to the next (run12/13:
// pigbrown.zip), and the game places it on that frame. Traced in
// re\SYNC_VIABILITY.md ("Traced: async loading and spawns").
//
// WilburStandAloneLoop::vf11 calls AsyncActorCreator_Update(creator) once per
// frame; it places at most one finished spawn, the first DONE task in queue
// order. We redirect that one call (through a near stub) to GateUpdate:
//   1. note the sim frame each queued task is first seen;
//   2. if the first task that has reached first-seen + spawn_delay frames is
//      not loaded yet, make the game finish it now (FUN_14025b150, the
//      engine's own "run the loader until this task is done");
//   3. hide DONE on tasks that finished early;
//   4. call the real Update;  5. restore the hidden DONE flags.
// Every spawn is then placed a fixed number of frames after it was queued,
// whatever the disk or GPU did. Placements go to spawns.csv, numbered in
// queue order (addresses change every launch).

DWORD RVA_SPAWN_CALL   = 0x21ed80;   // in WilburStandAloneLoop::vf11: call AsyncActorCreator_Update
DWORD RVA_SPAWN_UPDATE = 0x5f9630;   // AsyncActorCreator_Update(creator)
DWORD RVA_SPAWN_FORCE  = 0x25b150;   // run the loader until a task is done (loader, task)
DWORD RVA_SPAWN_LOADER = 0x9c2e40;   // global: the loader

typedef void (*PFN_SpawnUpdate)(void* creator);
typedef void (*PFN_SpawnForce)(void* loader, void* task);
static PFN_SpawnUpdate g_spawnUpdate;
static PFN_SpawnForce  g_spawnForce;
static void**          g_spawnLoaderPtr;
static FILE*           g_spawnCsv;

SpawnSlot g_spawnSlots[512];
uint32_t  g_spawnSeq;
static uint64_t  g_spawnPlaced, g_spawnForced, g_spawnGuardTrips, g_spawnHides, g_spawnMaxQueue;

// The frame number spawns are timed in: game frames when the engine clock is
// replaced, else XInput frames.
static uint64_t SimFrame() { return g_engineClockReady ? g_engineFrames : g_frame; }

// One spawns.csv row; extra is per event (frames waited, or the loader's queue).
static void SpawnLog(uint64_t now, const char* ev, const SpawnSlot& s, long long extra) {
    if (g_spawnCsv) fprintf(g_spawnCsv, "%llu,%u,%s,%u,%llu,%lld\n", now, g_logical, ev, s.seq, s.stamp, extra);
}

// The slot tracking this (task, actor), or a new one stamped with now (*fresh =
// true); NULL if all 512 are in use.
static SpawnSlot* SpawnSlotFor(uint8_t* task, void* actor, uint64_t now, bool* fresh) {
    *fresh = false;
    for (auto& s : g_spawnSlots) if (s.used && s.task == task && s.actor == actor) return &s;
    for (auto& s : g_spawnSlots)
        if (!s.used) {
            s = { task, actor, now, ++g_spawnSeq, true, false };
            *fresh = true;
            return &s;
        }
    return NULL;
}

// Stands in for the game's one AsyncActorCreator_Update call per frame (steps
// 1-5 above). Fields used: creator+0x28 the queue's first node; node+0 the next
// node, node+0x10 its task; task+0x20 the actor, task[0x0d] DONE, task[0x90]
// cancelled; loader+0x48 how much the loader has pending.
static void GateUpdate(uint8_t* creator) {
    const uint64_t now = SimFrame(), delay = (uint64_t)g_cfg.spawnDelay;
    struct Item { uint8_t* task; SpawnSlot* slot; bool cancelled, stillQueued; };
    Item items[512];
    int n = 0;

    // 1. Walk the queue in order; note new tasks.
    for (auto& s : g_spawnSlots) s.live = false;
    for (uint8_t* node = *(uint8_t**)(creator + 0x28); node && n < 512; node = *(uint8_t**)node) {
        uint8_t* task = *(uint8_t**)(node + 0x10);
        if (!task) continue;
        bool fresh;
        SpawnSlot* slot = SpawnSlotFor(task, *(void**)(task + 0x20), now, &fresh);
        if (!slot) continue;
        slot->live = true;
        if (fresh) SpawnLog(now, "queued", *slot, 0);
        items[n++] = { task, slot, task[0x90] != 0, false };
    }
    if ((uint64_t)n > g_spawnMaxQueue) g_spawnMaxQueue = n;

    // 2. Force the first due task that isn't loaded yet.
    for (int i = 0; i < n; i++) {
        uint8_t* t = items[i].task;
        if (t[0x90] || t[0x0d] || now < items[i].slot->stamp + delay) continue;
        uint8_t* loader = g_spawnLoaderPtr ? (uint8_t*)*g_spawnLoaderPtr : NULL;
        long long pending = loader ? *(long long*)(loader + 0x48) : 0;
        if (pending > 0) {
            g_spawnForce(loader, t);
            g_spawnForced++;
            SpawnLog(now, "forced", *items[i].slot, pending);
        } else {
            g_spawnGuardTrips++;
            SpawnLog(now, "guard", *items[i].slot, pending);
        }
        break;
    }

    // 3. Hide tasks that finished early (never cancelled ones).
    uint8_t* hidden[512];
    int h = 0;
    for (int i = 0; i < n; i++) {
        uint8_t* t = items[i].task;
        if (!t[0x90] && t[0x0d] && now < items[i].slot->stamp + delay) { t[0x0d] = 0; hidden[h++] = t; }
    }
    g_spawnHides += h;

    // 4. The real update: places at most one DONE task.
    g_spawnUpdate(creator);

    // 5. Restore (hidden tasks weren't DONE, so Update can't have freed them),
    //    then see which tasks left the queue. Don't touch tasks that left:
    //    Update may have freed them.
    for (int i = 0; i < h; i++) hidden[i][0x0d] = 1;
    for (uint8_t* node = *(uint8_t**)(creator + 0x28); node; node = *(uint8_t**)node) {
        uint8_t* task = *(uint8_t**)(node + 0x10);
        for (int i = 0; i < n; i++)
            if (items[i].task == task) { items[i].stillQueued = true; break; }
    }
    for (int i = 0; i < n; i++) {
        if (items[i].stillQueued) continue;
        SpawnSlot& s = *items[i].slot;
        if (items[i].cancelled) {
            SpawnLog(now, "cancelled", s, 0);
        } else {
            SpawnLog(now, "placed", s, (long long)(now - s.stamp));
            g_spawnPlaced++;
        }
        s.used = false;
    }
    for (auto& s : g_spawnSlots) if (s.used && !s.live) {   // left the queue between frames (shouldn't happen)
        SpawnLog(now, "vanished", s, 0);
        s.used = false;
    }
    if (g_spawnCsv && (now % 60) == 0) fflush(g_spawnCsv);
}

// Byte checks: the call goes to AsyncActorCreator_Update, both functions' entries
// match, and the force function loads the loader global. From DllMain (spawn_gate=1).
bool VerifySpawnGate() {
    BYTE* base = (BYTE*)GetModuleHandleW(NULL);
    BYTE* call = base + RVA_SPAWN_CALL;
    BYTE* upd = base + RVA_SPAWN_UPDATE;
    BYTE* force = base + RVA_SPAWN_FORCE;
    static const BYTE updEntry[] = { 0x40, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x48, 0x48, 0x8B, 0x79, 0x28, 0x48, 0x8B, 0xF1 };
    static const BYTE forceEntry[] = { 0x40, 0x53, 0x41, 0x54, 0x48, 0x83, 0xEC, 0x38, 0x65, 0x48, 0x8B, 0x04, 0x25,
                                       0x20, 0x00, 0x00, 0x00 };
    __try {
        if (call[0] != 0xE8 || call + 5 + *(int32_t*)(call + 1) != upd) return false;
        if (memcmp(upd, updEntry, sizeof updEntry) != 0) return false;
        if (memcmp(force, forceEntry, sizeof forceEntry) != 0) return false;
        if (force[20] != 0x48 || force[21] != 0x8B || force[22] != 0x0D) return false;          // mov rcx,[rip+d]
        if (force + 27 + *(int32_t*)(force + 23) != base + RVA_SPAWN_LOADER) return false;     // ... the loader
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    g_spawnUpdate = (PFN_SpawnUpdate)upd;
    g_spawnForce = (PFN_SpawnForce)force;
    g_spawnLoaderPtr = (void**)(base + RVA_SPAWN_LOADER);
    return true;
}

// Executable memory within +-2 GB of the game, for a rel32 call to reach.
static BYTE* AllocNearExe() {
    BYTE* base = (BYTE*)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    uintptr_t end = (uintptr_t)base + nt->OptionalHeader.SizeOfImage;
    for (uintptr_t step = 0; step < 0x70000000; step += 0x10000) {
        uintptr_t up = (end + 0xFFFF + step) & ~(uintptr_t)0xFFFF;
        if (void* p = VirtualAlloc((void*)up, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) return (BYTE*)p;
        if ((uintptr_t)base > step + 0x20000) {
            uintptr_t down = ((uintptr_t)base - 0x10000 - step) & ~(uintptr_t)0xFFFF;
            if (void* p = VirtualAlloc((void*)down, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) return (BYTE*)p;
        }
    }
    return NULL;
}

// The 5-byte call can only reach +-2 GB, so it's pointed at a stub near the exe
// that jumps (absolute) to GateUpdate.
bool PatchSpawnGate() {
    BYTE* call = (BYTE*)GetModuleHandleW(NULL) + RVA_SPAWN_CALL;
    BYTE* stub = AllocNearExe();
    if (!stub) return false;
    BYTE jmp[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    void* target = (void*)GateUpdate;
    memcpy(jmp + 6, &target, 8);
    memcpy(stub, jmp, sizeof jmp);
    FlushInstructionCache(GetCurrentProcess(), stub, sizeof jmp);
    int64_t rel = stub - (call + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) return false;
    DWORD old;
    if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    *(int32_t*)(call + 1) = (int32_t)rel;
    VirtualProtect(call, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    return true;
}

// spawns.csv in the run folder (from DllMain once the gate is on).
void OpenSpawnLog() {
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\spawns.csv", g_outDir);
    if (!_wfopen_s(&g_spawnCsv, path, L"w") && g_spawnCsv)
        fputs("frame,logical,event,spawn,queued_frame,extra\n", g_spawnCsv);
}

// At exit (CloseRun): the gate's totals, then close spawns.csv.
void CloseSpawnLog() {
    if (!g_spawnGateOn) return;
    Log("summary: spawn gate (delay %d frames): %llu spawns placed, %llu forced, %llu guard trips, "
        "%llu early finishes held back, longest queue %llu", g_cfg.spawnDelay, g_spawnPlaced, g_spawnForced,
        g_spawnGuardTrips, g_spawnHides, g_spawnMaxQueue);
    if (g_spawnGuardTrips)
        Log("WARNING: %llu spawn(s) were due and not loaded while the loader had nothing queued; they were "
            "left to finish on their own. This run may not be reproducible.", g_spawnGuardTrips);
    if (g_spawnCsv) { fclose(g_spawnCsv); g_spawnCsv = NULL; }
}
