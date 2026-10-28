// net_determinism.cpp -- determinism across PCs: the per-frame RNG check and catch-up, plus every
// netplay determinism patch (fixed-point loader, data pump, frame limiter, voice-line waits with
// each line's real length from the dialog bank, script random, Bullet proxy arena, body/key/pair
// ordering, orphan body parking, wake logs, the session-only simulation fixes, static maths).
// InstallDeterminism is last; it also installs net_own_screen.cpp's split-test hooks (screen
// regions and the camera field of view).
//
// Why it exists. Online co-op runs in lockstep: both PCs run the whole game, feed it the same
// inputs on the same frame numbers, and send each other pads, not game state. That works only if
// the game computes exactly the same thing on both PCs, down to the last bit of every float.
// Every section here removes one way it didn't: something that depended on real time, disk
// speed, heap addresses, the CPU or the Windows version. Each section says what the decomp
// session (the reverse engineering of the game's exe; notes in ..\re) found and what the fix
// does. Addresses written 0x14... are in the game's exe; the code uses them as RVAs, offsets
// from the exe's base (0x14027ac30 = RVA 0x27ac30; see patch_util.cpp). Every patch checks
// the bytes it expects first (BytesAt, RipRefersTo, RedirectCall), so a different game build
// is left alone and logged as "NOT handled (code doesn't match this build)".
//
// Terms. A session is a connected co-op game (g_netPhase NP_RUNNING; g_clockLockstep is set
// while one runs); "in a session" fixes are off otherwise, so solo play is the game's own.
// "Netplay mode" ([harness] mode = netplay, g_cfg.mode MODE_NETPLAY) is the whole run of a
// copy that may host or join, session or not. The fingerprint is a hash of the simulation
// state both PCs compare every hash_interval frames; when they differ (a desync) the repair
// has the client copy the host's values for what differs (both in net_lockstep.cpp). A
// publish is a level finishing loading; the load gate holds both PCs at it until both are
// loaded (see net_session.cpp). The *Ok flags below also go into the settings sheet
// (net_transport.cpp), the list of settings both PCs must agree on, since a patch can apply
// on one PC only.
//
// Entry points: InstallDeterminism, from DllMain (xinput_proxy.cpp) in record, replay and
// netplay modes; RngStep, NetParkOrphanBodies, KeyLogFlush, NetVisSync, NetMusicSyncHold and
// KeepBtFreeHooked, from the per-frame driver NetFrameBody (net_lockstep.cpp); LogSilentWakes
// and LogActivationLists, from its NetLogPhysFrame; RngStoreRemote, from input packet handling
// (net_session.cpp); RngReset, at session start; SetSyncLoader, at session start and end;
// NetRejoinResetLate, from NetRejoin (net_dropin.cpp). The H_* functions are hooks the game
// itself calls.

#include "proxy_shared.h"

// --- per-frame RNG check ------------------------------------------------------
//
// The fingerprint compares the random-number state only every hash_interval
// frames, so an extra draw on one PC could run for up to a second before the
// repair even started. Meanwhile everything that drew from the shifted stream
// on that PC (an enemy's attack pick, a spawn) went its own way, and copying the
// RNG back can't undo that. So every frame, at the lockstep point and after any
// repair write, both PCs also hash just the random-number state (the engine RNG
// and the simulation's rand() streams, as in the fingerprint) and send the last
// few hashes with their inputs. The first frame where they differ is known
// within a few frames: both PCs log it, with who drew from the engine RNG that
// frame on this PC (the two logs side by side show the extra draw), and the
// client asks the host for its random-number state at once, with a short lead:
// the host can't be more than input_delay frames ahead of the client.
// [netplay] rng_check=0 turns it off; the fingerprint and digest run either way.
NetRngTag g_rngLocal[kNetRing], g_rngRemote[kNetRing];
uint32_t  g_rngLocalTag[kNetRing], g_rngRemoteTag[kNetRing];   // frame + 1 when filled
uint32_t  g_rngLocalNewest;                                    // newest frame + 1 (0: none yet)
static uint32_t  g_rngRemoteNewest;                            // the same for the other PC's tags
static uint32_t  g_rngNext;                                           // next frame to compare
static uint32_t  g_rngBadSince;                                       // first differing frame + 1 (0: matching)
int       g_rngEpisodeAsks;                                   // repair asks in the current slip (RngAskRepair caps them)
static int g_rngLogged;                                       // slips logged so far (the log stops after 20)
uint64_t  g_rngSlips, g_rngAsks;                              // counts for the session summary

// The simulation's rand() stream states, in site order (what the tag hashes).
static int RngSimRand(LONG* out, int cap) {
    int n = 0;
    for (int i = 0; i < g_randSiteCount && n < cap; i++)
        if (NetSimRandSite(i)) out[n++] = g_randSites[i].state;
    return n;
}

// The tag: the rand() streams, then the engine RNG's 624 state words and words left.
// (The engine RNG is a Mersenne Twister, MT19937: 624 words of state, refilled in one go
// when used up.) An FNV-1a hash; words NULL: only the rand() streams.
static uint32_t RngHashOf(const LONG* rnd, int nr, const uint8_t* words, const int32_t* left) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < nr; i++) h = RepFnv(&rnd[i], 4, h);
    if (words) {
        h = RepFnv(words, 624 * 4, h);
        h = RepFnv(left, 4, h);
    }
    return h;
}

// This PC's tag now: RngHashOf over the live state. The generator object (*g_mtGlobalPtr)
// holds the state words at +0, the next-word pointer at +0x9c0 and the words left at +0x9c8.
static uint32_t NetRngHash() {
    LONG rnd[64];
    uint32_t h = RngHashOf(rnd, RngSimRand(rnd, 64), NULL, NULL);
    if (g_mtGlobalPtr && *g_mtGlobalPtr) {
        const uint8_t* inst = (const uint8_t*)*g_mtGlobalPtr;
        __try {
            h = RepFnv(inst, 624 * 4, h);                   // the state words
            h = RepFnv(inst + 0x9c8, 4, h);                 // words left
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return h;
}

// --- local catch-up ---
//
// Most slips are one PC drawing k more engine random numbers than the other in one
// frame (Ctrl+F9 is exactly that). The repair fixes it by copying the host's RNG a
// dozen frames later, but whatever drew from the shifted stream meanwhile (an AI's
// pick) can't be undone: in a one-PC test two town people went their own way and the
// session desynced 8 s later. Both PCs already know each frame's tag and draw count
// on both sides, so the PC that's BEHIND can fix itself the moment it sees the slip:
// it takes its saved RNG state from that frame, advances a copy by the k draws it
// missed, and if that hashes to exactly the other PC's tag there (and every later
// frame both tags are in for agrees too) it steps its live RNG forward k draws. The
// PC that's ahead does nothing; a client that's ahead waits a few frames for the host
// to catch up before asking it for a copy. Anything that isn't a clean k-draw slip
// (a rand() stream, a different consumer) falls through to the repair as before.
// [netplay] rng_catch_up=0 turns it off.
struct RngSnap { uint32_t frame1; uint32_t idx; int32_t left; int nrand; LONG rnd[64]; uint32_t words[624]; };
static const int kRngSnaps = 32;
static RngSnap   g_rngSnaps[kRngSnaps];                // this PC's RNG at each recent lockstep frame
uint64_t  g_rngCatchUps;
static uint32_t  g_rngPeerFixUntil;                    // client: the host should catch up by then; don't ask before

// k draws of the engine's MT_Next on a generator, without the outputs (Rng_U32 0x14025ded0:
// refill when no words are left, then one word).
static void MtAdvance(uint8_t* inst, int k) {
    for (int i = 0; i < k; i++) {
        int32_t* left = (int32_t*)(inst + 0x9c8);
        if (*left == 0) g_mtRefill(inst);
        (*left)--;
        *(uint8_t**)(inst + 0x9c0) += 4;
    }
}

// Saves this PC's RNG state at lockstep frame f into the ring (frame1 = f + 1 only if the
// state read back sane), for RngTryCatchUp to replay from.
static void RngSnapshot(uint32_t f) {
    RngSnap& s = g_rngSnaps[f % kRngSnaps];
    s.frame1 = 0;
    s.nrand = RngSimRand(s.rnd, 64);
    if (!g_mtGlobalPtr || !*g_mtGlobalPtr) return;      // the stand-in tests have no engine RNG
    const uint8_t* inst = (const uint8_t*)*g_mtGlobalPtr;
    __try {
        memcpy(s.words, inst, sizeof s.words);
        s.idx = (uint32_t)((*(const uint8_t* const*)(inst + 0x9c0) - inst) / 4);
        s.left = *(const int32_t*)(inst + 0x9c8);
        if (s.idx <= 624 && s.left >= 0 && s.left <= 624) s.frame1 = f + 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// The tag a saved state would have had after k more draws.
// Rebuilt in a scratch copy laid out like the game's generator object, so MtAdvance and the
// game's refill can run on it.
static uint32_t RngSnapHashAfter(const RngSnap& s, int k) {
    alignas(16) static uint8_t scratch[0xa00];
    memcpy(scratch, s.words, sizeof s.words);
    *(uint8_t**)(scratch + 0x9c0) = scratch + s.idx * 4;
    *(int32_t*)(scratch + 0x9c8) = s.left;
    MtAdvance(scratch, k);
    return RngHashOf(s.rnd, s.nrand, scratch, (const int32_t*)(scratch + 0x9c8));
}

// Frame g differs; f is this frame. True if this PC was k draws behind from g on and has
// now caught up (the live RNG stepped forward k draws).
// Only small slips (1..8 draws) still inside the snapshot ring are tried.
static bool RngTryCatchUp(uint32_t g, uint32_t f, int k) {
    if (!g_net.rngCatchUp || !g_mtRefill || !g_mtGlobalPtr || !*g_mtGlobalPtr || k < 1 || k > 8) return false;
    if (f - g >= (uint32_t)kRngSnaps) return false;
    // Check every frame from g to f whose other-PC tag is in: our saved state plus k draws
    // must hash to their tag, and after g both PCs must have drawn alike.
    int checked = 0;
    for (uint32_t h = g; h <= f; h++) {
        int jh = h % kNetRing;
        const RngSnap& s = g_rngSnaps[h % kRngSnaps];
        if (s.frame1 != h + 1 || g_rngLocalTag[jh] != h + 1) return false;
        if (g_rngRemoteTag[jh] != h + 1) continue;      // the other PC's tag isn't here (yet)
        if (h > g && g_rngRemote[jh].draws != g_rngLocal[jh].draws) return false;   // not a single clean slip
        if (RngSnapHashAfter(s, k) != g_rngRemote[jh].hash) return false;
        checked++;
    }
    if (!checked) return false;
    // Verified: step the live generator forward, under the lock its draws take.
    AcquireSRWLockExclusive(&g_mtLock);
    __try { MtAdvance((uint8_t*)*g_mtGlobalPtr, k); } __except (EXCEPTION_EXECUTE_HANDLER) { checked = 0; }
    ReleaseSRWLockExclusive(&g_mtLock);
    if (!checked) return false;
    g_rngCatchUps++;
    Log("rng check: frame %u: this PC was %d engine RNG draw%s behind; caught up locally at frame %u "
        "(verified on %d frame%s, no copy from the other PC needed)", g, k, k == 1 ? "" : "s", f, checked, checked == 1 ? "" : "s");
    return true;
}

// Packets (under the net lock, any thread): only store.
// tags are the other PC's for frames last - count + 1 .. last, from one input packet;
// RngStep compares them on the game thread.
void RngStoreRemote(uint32_t last, int count, const NetRngTag* tags) {
    if ((uint32_t)count > last + 1) return;
    for (int k = 0; k < count; k++) {
        uint32_t f = last - (count - 1) + k;
        if (f < g_rngNext || f >= g_rngNext + kNetRing - 1) continue;   // compared already, or absurdly far ahead
        g_rngRemote[f % kNetRing] = tags[k];
        g_rngRemoteTag[f % kNetRing] = f + 1;
        if (f + 1 > g_rngRemoteNewest) g_rngRemoteNewest = f + 1;
    }
}

// Forgets every tag, snapshot and slip, for a new session (called from the repair's reset).
void RngReset() {
    memset(g_rngLocalTag, 0, sizeof g_rngLocalTag);
    memset(g_rngRemoteTag, 0, sizeof g_rngRemoteTag);
    g_rngLocalNewest = g_rngRemoteNewest = g_rngNext = g_rngBadSince = g_rngQuietUntil = g_rngPeerFixUntil = 0;
    g_rngLogged = g_rngEpisodeAsks = 0;
    for (RngSnap& s : g_rngSnaps) s.frame1 = 0;
    AcquireSRWLockExclusive(&g_mtLock);
    memset(g_rngDraws, 0, sizeof g_rngDraws);
    g_rngDrawCur = 0;
    g_rngDrawOn = false;                                  // on from this session's first lockstep frame
    ReleaseSRWLockExclusive(&g_mtLock);
}

// This PC's engine RNG callers in lockstep frame f (function return addresses, as in
// the decomp: drawing function < its caller).
static void RngLogDraws(uint32_t f) {
    char list[1100] = "";
    uint32_t n = 0;
    bool found = false;
    AcquireSRWLockShared(&g_mtLock);
    for (int i = 0; i < kRngDrawFrames && !found; i++) {
        const RngFrameDraws& b = g_rngDraws[i];
        if (b.frame != f + 1) continue;
        found = true;
        n = b.n;
        for (uint32_t k = 0; k < b.n && k < _countof(b.d); k++) {
            size_t len = strlen(list);
            _snprintf_s(list + len, sizeof list - len, _TRUNCATE, "%s%llx<%llx%s", k ? ", " : "",
                        0x140000000ULL + b.d[k].r0, b.d[k].r1 ? 0x140000000ULL + b.d[k].r1 : 0ULL,
                        b.d[k].main ? "" : " (other thread)");
        }
    }
    ReleaseSRWLockShared(&g_mtLock);
    if (!found) Log("rng check: frame %u: this PC's engine RNG callers for that frame are no longer recorded", f);
    else Log("rng check: frame %u: this PC's engine RNG draws that frame (%u%s): %s", f, n,
             n > 32 ? ", first 32 listed" : "", n ? list : "none");
}

// Once per lockstep frame, right after RepStep, at the same point on both PCs.
void RngStep(uint32_t f) {
    if (g_netPhase != NP_RUNNING) return;
    AcquireSRWLockExclusive(&g_mtLock);                   // close this frame's record of engine RNG draws
    RngFrameDraws& b = g_rngDraws[g_rngDrawCur];
    uint32_t draws = b.n;
    b.frame = f + 1;
    g_rngDrawCur = (g_rngDrawCur + 1) % kRngDrawFrames;
    g_rngDraws[g_rngDrawCur].frame = g_rngDraws[g_rngDrawCur].n = 0;
    g_rngDrawOn = true;
    ReleaseSRWLockExclusive(&g_mtLock);
    // This frame's own tag (hash and draw count), which the next input packets carry.
    int i = f % kNetRing;
    g_rngLocal[i] = { NetRngHash(), (uint16_t)(std::min)(draws, 0xFFFFu) };
    g_rngLocalTag[i] = f + 1;
    g_rngLocalNewest = f + 1;
    if (!g_net.rngCheck) return;
    RngSnapshot(f);
    if (g_rngNext <= g_netHashHoldUntil) g_rngNext = g_netHashHoldUntil + 1;   // drop-in: the title screens differ
    // Compare, in order, every frame up to f for which both tags are in: a match ends a
    // slip; a difference starts or continues one, and is fixed locally if this PC is
    // behind by a clean k draws, else handed to the repair (RngAskRepair).
    while (g_rngNext <= f) {
        uint32_t g = g_rngNext;
        int j = g % kNetRing;
        if (g_rngLocalTag[j] != g + 1) { g_rngNext++; continue; }   // before this session's first frame
        if (g_rngRemoteTag[j] != g + 1) {
            if (g_rngRemoteNewest > g + 1 + kNetRngTags) { g_rngNext++; continue; }   // its packets were lost: skip it
            break;                                                                    // not here yet
        }
        g_rngNext++;
        const NetRngTag &mine = g_rngLocal[j], &theirs = g_rngRemote[j];
        if (mine.hash == theirs.hash) {
            if (g_rngBadSince && g_rngLogged <= 20)
                Log("rng check: frame %u: the random numbers match again (they differed for %u frames)", g, g + 1 - g_rngBadSince);
            g_rngBadSince = 0;
            g_rngEpisodeAsks = 0;
            continue;
        }
        int k = (int)theirs.draws - (int)mine.draws;            // > 0: this PC drew fewer (it's behind)
        if (!g_rngBadSince) {                                   // a new slip
            g_rngBadSince = g + 1;
            g_rngSlips++;
            if (g_rngLogged++ < 20) {
                Log("rng check: frame %u: the random numbers differ (this PC: %08x after %u engine RNG draws that frame; "
                    "the other PC: %08x after %u)", g, mine.hash, mine.draws, theirs.hash, theirs.draws);
                RngLogDraws(g);
            }
            // The other PC drew fewer: it catches up on its side. A client gives the host
            // time to see the slip and fix itself before asking it for a copy.
            if (k < 0 && k >= -8 && g_net.rngCatchUp) g_rngPeerFixUntil = g + 2 * g_net.delay + 10;
        }
        if (g <= g_rngQuietUntil) continue;                     // a fix already covers this frame
        // Try within the slip's first few frames, while it's still one clean slip.
        if (k > 0 && g + 1 - g_rngBadSince < 4 && RngTryCatchUp(g, f, k)) {
            g_rngLocal[i].hash = NetRngHash();                  // frame f's tag, after the fix (not sent yet)
            RngSnapshot(f);
            g_rngQuietUntil = (std::max)(g_rngQuietUntil, f - 1);   // frames before f still differ: expected
            g_rngEpisodeAsks = 0;
            continue;
        }
        if (g < g_rngPeerFixUntil) continue;
        RngAskRepair(g);
    }
}

// ------------------------------------------------------------------ determinism across PCs
//
// From the decomp session's trace of the netplay_6 desync (re\SYNC_VIABILITY.md,
// "wall-clock decisions after a load" and "per-PC data"):
//  - the streaming loader gets a time budget measured with the real clock
//    (and a 31 ms gate on its low-priority fiber), so a faster PC finishes
//    loads on an earlier frame;
//  - ten scripts wait for a voice line to stop playing in real time;
//  - ucrtbase picks an FMA3 maths path per CPU.

// Which of those fixes applied: fixed-point loading verified (sync_loader), voice-line waits
// patched (audio_wait), the CRT's FMA3 path off.
bool g_syncLoaderOn, g_audioWaitOn, g_fma3Off;

// Engine_FrameTick: budget = 100000 or ((int)DAT_140ac66fc + 2) * 1000 us
// becomes 0x7fffffff, so the loader fiber drains its queue at the fixed slice
// point; the four "QPC - last > 31000 us" gates become "> -1" (always).
static const DWORD kLoaderGates[] = { 0x25b225, 0x25b2ea, 0x25b45d, 0x25b531 };
static BYTE g_loaderOrigA[5], g_loaderOrigBody[18], g_loaderOrigGate[4][4];   // the game's bytes, to switch back
static bool g_syncLoaderActive;                                                  // the patch is in right now
// Checks the budget code and the four gates are this build's, and saves the original
// bytes SetSyncLoader writes back. The patch itself isn't written here.
static bool VerifySyncLoader() {
    static const BYTE a[] = { 0xB8, 0xA0, 0x86, 0x01, 0x00, 0xEB, 0x12, 0xF3, 0x48, 0x0F, 0x2C, 0x05 };   // 0x140280bf1
    static const BYTE b[] = { 0x83, 0xC0, 0x02, 0x69, 0xC0, 0xE8, 0x03, 0x00, 0x00, 0x8B, 0xC8 };       // 0x140280c01
    static const BYTE gate[] = { 0x48, 0x3D, 0x18, 0x79, 0x00, 0x00, 0x7E, 0x28 };
    if (!BytesAt(0x280bf1, a, sizeof a) || !BytesAt(0x280c01, b, sizeof b)) return false;
    for (DWORD g : kLoaderGates) if (!BytesAt(g, gate, sizeof gate)) return false;
    memcpy(g_loaderOrigA, ExeBase() + 0x280bf1, 5);
    memcpy(g_loaderOrigBody, ExeBase() + 0x280bf8, 18);
    for (int i = 0; i < 4; i++) memcpy(g_loaderOrigGate[i], ExeBase() + kLoaderGates[i] + 2, 4);
    return true;
}
// On: the loader drains at a fixed point each frame. Off: the game's own
// time-budgeted loading. Flipped on the game thread (the loader's fibers run there).
void SetSyncLoader(bool on) {
    if (on == g_syncLoaderActive) return;
    if (on) {
        // "mov eax,7FFFFFFFh" over the 100000 constant; the other budget formula's 18 bytes
        // become the same load plus NOPs; each gate's 31000 compare becomes -1.
        static const BYTE big[] = { 0xB8, 0xFF, 0xFF, 0xFF, 0x7F };
        BYTE body[18];
        memcpy(body, big, 5);
        memset(body + 5, 0x90, 13);
        static const BYTE never[] = { 0xFF, 0xFF, 0xFF, 0xFF };
        WriteCode(0x280bf1, big, 5);
        WriteCode(0x280bf8, body, sizeof body);
        for (DWORD g : kLoaderGates) WriteCode(g + 2, never, 4);
    } else {
        WriteCode(0x280bf1, g_loaderOrigA, 5);
        WriteCode(0x280bf8, g_loaderOrigBody, 18);
        for (int i = 0; i < 4; i++) WriteCode(kLoaderGates[i] + 2, g_loaderOrigGate[i], 4);
    }
    g_syncLoaderActive = on;
    Log("determinism: fixed-point loading %s", on ? "on" : "off (the game's own loading)");
}
bool SyncLoaderOk() { return g_syncLoaderOn; }   // verified at start-up, so SetSyncLoader(true) may be used

// The data-file pump FUN_140511870(&DAT_140957280) runs once per Engine_FrameTick
// (call at 0x140280bd0, just before GameTimer_Tick). Async .var/.txt/.dbl/.oct
// reads finish whenever the disk does, so the frame a data file is applied
// depends on the PC. In a session, keep pumping until it's idle (+0x84 == 0), as
// the game's own flush does, so every file lands at this fixed point.
uint64_t g_pumpDrainFrames, g_pumpDrainMaxMs;
static void H_DataPump(void* mgr) {
    typedef void (*PFN_Pump)(void*);
    PFN_Pump pump = (PFN_Pump)(ExeBase() + 0x511870);
    pump(mgr);
    if (!g_clockLockstep) return;
    volatile char* busy = (volatile char*)((BYTE*)mgr + 0x84);
    if (!*busy) return;
    ULONGLONG t0 = GetTickCount64();
    while (*busy) {
        SwitchToThread();
        pump(mgr);
        if (GetTickCount64() - t0 > 30000) {           // never wait forever; this run may then drift
            Log("determinism: data-file pump still busy after 30 s; carrying on");
            break;
        }
    }
    uint64_t ms = GetTickCount64() - t0;
    g_pumpDrainFrames++;
    if (ms > g_pumpDrainMaxMs) g_pumpDrainMaxMs = ms;
}

// The loader slice, Engine_FrameTick's call to FUN_14025b610(budget) (-> Loader_Yield
// 0x14025b3e0 on the loader at 0x1409c2e40). The decomp session's trace: the
// fiber only keeps running past a yield while the PENDING count (ld+0x48) is
// non-zero; once the last queued task is the one running, every yield (each
// read) switches back to the main loop, and the frame limiter sleeps 1/60 s
// before the next step: one ~45 KB read per frame, 266 MB in 100 s. The budget
// is never consulted then. In a session the slice is repeated, with the data
// pump between (its clients feed the streamer), until the engine's own "loader
// has work" test is false: pending == 0 and the in-progress flag 0x1409c2e48
// clear. Loads then run at disk speed, and every frame's loader work completes
// inside that frame on both PCs. Fiber tasks wait only on the IO thread.
// The frame limiter FUN_140538650 spins (pause loop 0x140259100) until the frame
// stopwatch 0x140b2b410 reaches 1/60 s (1/30 s in menus). The loading screen
// steps the loader once per iteration of its own loop, so in a session that
// wait IS most of a load (profiled: 100-130 s for Toy Box, 45 KB per step).
// While the mini-load screen is up (DAT_140b2c5a4) the wait is skipped: it is
// pure real-time pacing, nothing the simulation reads. Solo play already runs
// without it when fps_limit is off, which is why solo loads were quick.
typedef void (*PFN_FrameLimit)();
static PFN_FrameLimit g_frameLimitOrig;
uint64_t g_frameLimitSkips;
static void H_FrameLimit() {
    // In play the limiter runs once per lockstep frame. A loading screen runs its
    // own render loop INSIDE one lockstep frame (profiled: hundreds of limiter
    // calls in a 10-20 s frame), so in a session every call after the first in
    // the same lockstep frame skips the wait. Play keeps its pacing; loads run
    // flat out, like solo with fps_limit off. No lockstep frame passes during
    // those calls, so nothing the simulation sees depends on it. (DAT_140b2c5a4
    // stayed 0 through level loads and the bytes-read detector starts late, so
    // neither is a reliable trigger.)
    static uint32_t lastFrame = 0xFFFFFFFF;
    static uint32_t callsThisFrame;
    if (g_clockLockstep) {
        if (g_netFrame != lastFrame) { lastFrame = g_netFrame; callsThisFrame = 0; }
        if (++callsThisFrame > 1) { g_frameLimitSkips++; return; }
    }
    g_frameLimitOrig();
}
// Detours the frame limiter FUN_140538650 to H_FrameLimit. Its first two instructions
// (6 bytes) are copied to a trampoline that jumps back after them, and g_frameLimitOrig
// calls that: the game's limiter, unchanged. The function's start becomes a jmp rel32 to a
// near stub holding H_FrameLimit's absolute address.
static bool HookFrameLimit() {
    static const BYTE expect[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x0D };   // push rbx; sub rsp,20h; mov ecx,[rip+..]
    if (!BytesAt(0x538650, expect, sizeof expect)) return false;
    BYTE* fn = ExeBase() + 0x538650;
    BYTE* tr = AllocNear(fn, 32);                        // the two moved instructions, then jmp rel32 back
    BYTE* stub = AllocNear(fn, 16);                      // jmp [rip+0] -> H_FrameLimit
    if (!tr || !stub) return false;
    memcpy(tr, expect, 6);
    tr[6] = 0xE9;
    *(int32_t*)(tr + 7) = (int32_t)((fn + 6) - (tr + 11));
    stub[0] = 0xFF; stub[1] = 0x25;
    *(int32_t*)(stub + 2) = 0;
    void* h = (void*)H_FrameLimit;
    memcpy(stub + 6, &h, 8);
    FlushInstructionCache(GetCurrentProcess(), tr, 32);
    FlushInstructionCache(GetCurrentProcess(), stub, 16);
    g_frameLimitOrig = (PFN_FrameLimit)tr;
    BYTE jmp[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
    *(int32_t*)(jmp + 1) = (int32_t)(stub - (fn + 5));
    return WriteCode(0x538650, jmp, sizeof jmp);
}

// The redirected loader slice call (see "The loader slice" above): the game's slice, then,
// in a session with fixed-point loading on, more slices and data pumps until the loader's
// pending count is 0. The counters are for the session summary.
uint64_t g_loaderDrainFrames, g_loaderDrainMaxMs;
uint32_t g_loaderDrainMaxIters;
static void H_LoaderSlice(uint32_t budgetUs) {
    typedef void (*PFN_Slice)(uint32_t);
    typedef void (*PFN_Pump)(void*);
    BYTE* b = ExeBase();
    PFN_Slice slice = (PFN_Slice)(b + 0x25b610);
    slice(budgetUs);
    if (!g_clockLockstep || !g_syncLoaderActive) return;
    BYTE* ld = *(BYTE**)(b + 0x9c2e40);
    if (!ld) return;
    volatile char* inProgress = (volatile char*)(b + 0x9c2e48);   // (the loader's STOP flag; logged only)
    auto busy = [&]() { return *(volatile int64_t*)(ld + 0x48) != 0; };   // queued + running tasks
    if (!busy()) return;
    ULONGLONG t0 = GetTickCount64();
    uint32_t iters = 0;
    while (busy()) {
        slice(0x7fffffff);
        ((PFN_Pump)(b + 0x511870))((void*)(b + 0x957280));
        SwitchToThread();
        iters++;
        if (GetTickCount64() - t0 > 30000) {           // never wait forever; this run may then drift
            Log("determinism: loader still busy after 30 s (pending %lld, in progress %d); carrying on",
                (long long)*(volatile int64_t*)(ld + 0x48), (int)*inProgress);
            break;
        }
    }
    uint64_t ms = GetTickCount64() - t0;
    g_loaderDrainFrames++;
    if (ms > g_loaderDrainMaxMs) g_loaderDrainMaxMs = ms;
    if (iters > g_loaderDrainMaxIters) g_loaderDrainMaxIters = iters;
}

// Script_SfxHandleIsPlayingOnActor (0x140479390) replaced: a voice line counts
// as playing for exactly its authored length in frames from the first query,
// not until the audio really stops (stream latency differs per PC).
static VoiceWait g_voice[64];
uint64_t g_voiceWaits;
static uint64_t g_voiceFallbacks;
typedef int* (*PFN_ScriptArgInt)(void* vm, void* tmp);
typedef void (*PFN_ScriptPop)(void* vm);
typedef void* (*PFN_SoundObj)(void* audio, int handle);
typedef uint64_t (*PFN_ScriptRetBool)(void* vm, int value);
typedef int (__stdcall* PFN_FmodGetDesc)(void* inst, void** desc);
typedef int (__stdcall* PFN_FmodGetLength)(void* desc, int* ms);
static PFN_FmodGetDesc g_fmodGetDesc;
static PFN_FmodGetLength g_fmodGetLength;
static const int kVoiceFallbackFrames = 180;         // unknown length: 3 s on both PCs

// --- voice line lengths (re\VOICE_OVERLAP.md) ---------------------------------
//
// Voice lines aren't FMOD Studio events: they're subsounds of one low-level stream bank,
// data_dx\sound\DIALOG<language>\dialog_str.fsb (opened by FUN_140267e00, played by
// FUN_140275d70). So the event-length lookup below never found them, and every line counted
// as the 3 s fallback. A third of the lines are longer: when the 3 s ran out the game took
// the speaker as finished (the Say component's update clears its line handle) and started the
// next line on top of the one still playing, most often at level starts.
// The fix reads every line's real length from the bank's FSB5 header (just the header, about
// 220 KB; no audio) into a table, once. The table is always the ENGLISH bank's: all 14
// languages hold the same 4,311 lines in the same order (their names only add a language
// prefix, "DE_LG001" for "LG001"), so a line's index means the same line in every language,
// and both co-op PCs time every line identically whatever language each one plays in.
static std::vector<uint16_t> g_voiceFrames;           // per line index: its length in 60 Hz frames, rounded up
static std::unordered_map<std::string, int> g_voiceByName;   // line name ("LG001") -> index
uint32_t g_voiceTableHash;                             // FNV-1a of the table, for the co-op settings sheet (0: not measured)
static void* volatile g_dialogBank;                    // the dialog bank's FMOD Sound*, once the game opens it
static const int kVoiceMarginFrames = 6;               // a line's wait outlasts the stream's start-up a little

// Reads DIALOGen-US\dialog_str.fsb's FSB5 header into g_voiceFrames / g_voiceByName. Layout
// (FSB5 version 1): a 0x3c-byte header (magic, version, line count, sample-header bytes,
// name-table bytes, ...), then one 64-bit header per line (bit 0: extra chunks follow; bits 1-4:
// sample-rate index; bits 34-63: the length in samples), each optionally followed by chunks
// (u32: bit 0 more chunks, bits 1-24 size, bits 25-31 type; type 2 overrides the rate), then
// the name table (one offset per line, then the names). False if anything doesn't fit.
static bool LoadVoiceTable() {
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\data_dx\\sound\\DIALOGen-US\\dialog_str.fsb", g_exeDir);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    uint32_t h[7] = {};                                  // magic, version, lines, sample-header bytes, name bytes, data bytes, mode
    BYTE head[0x3c];
    DWORD got = 0;
    bool ok = ReadFile(f, head, sizeof head, &got, NULL) && got == sizeof head;
    if (ok) memcpy(h, head, sizeof h);
    ok = ok && !memcmp(head, "FSB5", 4) && h[1] == 1 && h[2] > 0 && h[2] < 100000 && h[3] < (64u << 20) && h[4] < (64u << 20);
    std::vector<BYTE> sh, names;
    if (ok) {
        sh.resize(h[3]);
        names.resize(h[4]);
        ok = ReadFile(f, sh.data(), h[3], &got, NULL) && got == h[3] && ReadFile(f, names.data(), h[4], &got, NULL) && got == h[4];
    }
    CloseHandle(f);
    if (!ok) return false;
    static const uint32_t kRates[16] = { 4000, 8000, 11000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 96000 };
    std::vector<uint16_t> frames(h[2]);
    size_t pos = 0;
    for (uint32_t i = 0; i < h[2]; i++) {
        if (pos + 8 > sh.size()) return false;
        uint64_t v;
        memcpy(&v, &sh[pos], 8);
        pos += 8;
        uint32_t rate = kRates[(v >> 1) & 0xf];
        uint64_t samples = v >> 34;
        for (bool more = v & 1; more;) {                 // the optional chunks after this line's header
            if (pos + 4 > sh.size()) return false;
            uint32_t c;
            memcpy(&c, &sh[pos], 4);
            pos += 4;
            more = c & 1;
            uint32_t size = (c >> 1) & 0xffffff, type = c >> 25;
            if (pos + size > sh.size()) return false;
            if (type == 2 && size >= 4) memcpy(&rate, &sh[pos], 4);   // the sample rate, when it isn't one of the table's
            pos += size;
        }
        if (!rate) return false;
        uint64_t fr = (samples * 60 + rate - 1) / rate;
        frames[i] = (uint16_t)(fr > 60000 ? 60000 : fr);
    }
    std::unordered_map<std::string, int> byName;
    for (uint32_t i = 0; i < h[2]; i++) {                // the name table: offsets, then C strings
        if (4 * (i + 1) > names.size()) return false;
        uint32_t off;
        memcpy(&off, &names[4 * i], 4);
        if (off >= names.size()) return false;
        const char* s = (const char*)&names[off];
        size_t n = strnlen(s, names.size() - off);
        byName[std::string(s, n)] = (int)i;
    }
    uint32_t hash = 0x811C9DC5;
    for (uint16_t fr : frames) { hash = (hash ^ (fr & 0xff)) * 16777619u; hash = (hash ^ (fr >> 8)) * 16777619u; }
    g_voiceFrames.swap(frames);
    g_voiceByName.swap(byName);
    g_voiceTableHash = hash;
    return true;
}

// A line's length by name: as asked for, or without a two-letter language prefix
// ("DE_LG001"), plus the margin. -1 if the table doesn't know it.
static int VoiceFramesByName(const char* name) {
    if (!name || !*name || g_voiceFrames.empty()) return -1;
    auto it = g_voiceByName.find(name);
    if (it == g_voiceByName.end() && isupper((BYTE)name[0]) && isupper((BYTE)name[1]) && name[2] == '_')
        it = g_voiceByName.find(name + 3);
    return it == g_voiceByName.end() ? -1 : g_voiceFrames[it->second] + kVoiceMarginFrames;
}

// FUN_140267e00's createStream call (0x140267f90): the game opening a sound bank. When it's
// the dialog bank, remember its Sound*, so VoiceLengthFrames can tell a voice line from
// other low-level sounds.
typedef int (__stdcall* PFN_CreateStream)(void* system, const char* name, unsigned mode, void* exinfo, void** sound);
static int __stdcall H_CreateStream(void* system, const char* name, unsigned mode, void* exinfo, void** sound) {
    int r = ((PFN_CreateStream)(ExeBase() + 0x717880))(system, name, mode, exinfo, sound);
    __try {
        size_t n = name ? strlen(name) : 0;
        if (r == 0 && sound && *sound && n >= 14 && !_stricmp(name + n - 14, "dialog_str.fsb")) {
            g_dialogBank = *sound;
            Log("voice lines: the game opened its dialog bank (%s)", name);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return r;
}

// A playing sound's length, in 60 Hz frames rounded up: a voice line's from the table (by its
// index in the dialog bank, plus the margin), anything else from its FMOD Studio event
// description. Both are file data, the same on both PCs. -1 if neither applies.
static int VoiceLengthFrames(void* obj) {
    if (!obj) return -1;
    __try {
        void* inst = *(void**)((BYTE*)obj + 0x10);     // FMOD::Studio::EventInstance (0 for a low-level channel)
        if (!inst) {
            // A low-level subsound: +0xc0 points at its bank's Sound*, +0xc8 is its index in it.
            // Both are set when the line is started, before its audio is fetched.
            void** bankRef = *(void***)((BYTE*)obj + 0xc0);
            void* bank = bankRef ? *bankRef : NULL;
            int idx = *(int*)((BYTE*)obj + 0xc8);
            if (bank && bank == g_dialogBank && idx >= 0 && idx < (int)g_voiceFrames.size())
                return g_voiceFrames[idx] + kVoiceMarginFrames;
            static LONG misses;
            if (bank && InterlockedIncrement(&misses) <= 10)
                Log("voice lines: a low-level sound that isn't a known dialog line (bank %p, dialog bank %p, index %d)",
                    bank, (void*)g_dialogBank, idx);
            return -1;
        }
        void* desc = NULL;
        int ms = 0;
        if (!g_fmodGetDesc || !g_fmodGetLength || g_fmodGetDesc(inst, &desc) != 0 || !desc ||
            g_fmodGetLength(desc, &ms) != 0 || ms <= 0) return -1;
        return (ms * 60 + 999) / 1000;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// The AI speakers' lines (the Say component). FUN_14045b960(say, name, ?, priority) asks for a
// line by name and stores its sound handle at say+0x150; the per-frame update and the next
// request then ask whether that handle is still playing (H_SndIsPlaying). Whether a speaker
// gets a voice slot at all is decided in real time on each PC, so that answer can't come from
// the sound object: it comes from the requested NAME, which the simulation chose, the same on
// both PCs. Each request's handle and length are kept here for H_SndIsPlaying to look up.
struct SayLine { int handle; int frames; };
static SayLine g_sayLines[64];
static int g_sayNext;
typedef uint64_t (*PFN_SayRequest)(BYTE* say, const char* name, void* a3, int priority);
static PFN_SayRequest g_sayRequestOrig;
static uint64_t H_SayRequest(BYTE* say, const char* name, void* a3, int priority) {
    uint64_t r = g_sayRequestOrig(say, name, a3, priority);
    if (g_clockLockstep && g_audioWaitOn && GetCurrentThreadId() == g_mainThread) {
        int handle = 0;
        __try { handle = say ? *(int*)(say + 0x150) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        int frames = handle ? VoiceFramesByName(name) : -1;
        if (frames >= 0) {
            g_sayLines[g_sayNext] = { handle, frames };
            g_sayNext = (g_sayNext + 1) % (int)_countof(g_sayLines);
        } else if (handle) {
            static LONG misses;                          // to check names against the bank's (the spec's open question)
            if (InterlockedIncrement(&misses) <= 20)
                Log("voice lines: speaker line \"%s\" isn't in the dialog bank; it keeps the 3 s window", name ? name : "");
        }
    }
    return r;
}
static int SayLineFrames(int handle) {
    for (const SayLine& s : g_sayLines) if (handle && s.handle == handle) return s.frames;
    return -1;
}

// The frame rule: a handle counts as playing for its authored length in frames
// from the first query; a handle nobody asked about for 5 frames is forgotten.
// lineAudio: the audio object a conversation block resolves its handle through
// (FUN_140273070(*(block+8), handle), as FUN_140264780 does); NULL = the global sound list.
// known: a length the caller already has (an AI speaker's line, by name); -1 = none.
static bool FramePlaying(VoiceWait* tab, int n, int handle, bool learn, void* lineAudio = NULL, int known = -1) {
    if (!handle) return false;
    BYTE* b = ExeBase();
    uint64_t now = g_engineFrames;
    VoiceWait* e = NULL;
    VoiceWait* freeSlot = NULL;
    for (int i = 0; i < n; i++) {
        VoiceWait& v = tab[i];
        if (v.used && now - v.lastQuery > 5) v.used = false;   // the caller stopped asking: done with it
        if (v.used && v.handle == handle) e = &v;
        else if (!v.used && !freeSlot) freeSlot = &v;
    }
    if (!e && freeSlot) {
        e = freeSlot;
        *e = { handle, (uint32_t)now, now, -1, true };
        g_voiceWaits++;
    }
    if (!e) return false;
    e->lastQuery = (uint32_t)now;
    if (e->frames < 0) {
        if (known >= 0) {                                // the caller knows the length already
            e->frames = known;
        } else if (learn) {                              // learn the length once the sound object exists
            void* obj = NULL;
            __try {
                obj = lineAudio ? ((PFN_SoundObj)(b + 0x273070))(lineAudio, handle)
                                : ((PFN_SoundObj)(b + 0x26a6f0))(*(void**)(b + 0xac2ec0), handle);
            } __except (EXCEPTION_EXECUTE_HANDLER) { obj = NULL; }
            e->frames = VoiceLengthFrames(obj);
        }
        if (e->frames < 0 && (!learn || now - e->start >= 30)) { e->frames = kVoiceFallbackFrames; g_voiceFallbacks++; }
        if (e->frames >= 0 && g_clockLockstep)           // diagnostic: both logs must agree on every length
            Log("voice wait: handle %d counts as playing for %d frames from frame %llu (%s)", handle, e->frames,
                (unsigned long long)e->start, known >= 0 ? "speaker line, by name"
                : learn ? (e->frames == kVoiceFallbackFrames ? "fallback" : "authored") : "fixed window");
    }
    return e->frames < 0 || now < e->start + (uint64_t)e->frames;
}

// The replacement native: pops the script's handle argument, answers by the frame rule
// (learning the line's length) and pushes the bool, as the game's own native does.
static uint64_t H_SfxIsPlaying(void* vm) {
    BYTE* b = ExeBase();
    uint8_t tmp[16] = {};
    int handle = *((PFN_ScriptArgInt)(b + 0x544790))(vm, tmp);
    ((PFN_ScriptPop)(b + 0x544740))(vm);
    int playing = FramePlaying(g_voice, _countof(g_voice), handle, true);
    return ((PFN_ScriptRetBool)(b + 0x544d30))(vm, playing);
}

// FUN_140475580(int* handle), "is this sound still playing", asks FMOD's real
// state. The callers whose answer changes the simulation (decomp session's
// list) are redirected here: AI voice priority and last-spoke time
// (FUN_14045b960, FUN_14045b600), the talking flag (FUN_140444070), the reward
// jingle (ScreenReward vf38) and two list cleanups. In a session they get a
// FIXED window (kVoiceFallbackFrames from the first query), never the live
// line's length: these are low-priority AI lines, and whether one even gets a
// voice slot (so a sound object with a length exists) is decided in real time
// per PC. In the Toy Box two townspeople walked off a few frames apart that
// way while every other state matched. Audio-only callers keep the real answer.
static VoiceWait g_sndWait[128];
// 0x45f2a4: Say::vf44 (the AI dialogue component's per-frame update) zeroes the
// handle at +0x150 when the real sound ends, and FUN_14045b960/FUN_14045b600
// test that handle before their own (redirected) check (decomp session's audit).
static const DWORD kSndPlayingCalls[] = { 0x123ab2, 0x45b9cb, 0x45b61a, 0x444099, 0x4440bc, 0x4440cf, 0x18b37e, 0x6c4894, 0x45f2a4 };
// The redirected calls: on the game thread in a session, an AI speaker's line lasts its real
// length (looked up by the name it was requested with, H_SayRequest) and anything else the
// fixed window; the real FMOD answer otherwise.
static bool H_SndIsPlaying(int* h) {
    if (g_clockLockstep && g_audioWaitOn && GetCurrentThreadId() == g_mainThread) {
        int handle = h ? *h : 0;
        return FramePlaying(g_sndWait, _countof(g_sndWait), handle, false, NULL, SayLineFrames(handle));
    }
    return ((bool (*)(int*))(ExeBase() + 0x475580))(h);
}
// Conversation lines (decomp session, the Toy Box menu split). FUN_1401a9c80, "is the
// speaker's line still playing", gates the mission-giver's ConversationFinished
// (FUN_1400cac90), ConversationMgr's next-line pick (FUN_1401c2880, a shared-RNG roll) and
// ScreenAlertPrompt's advance and close (vf38, whose dequeue rolls too), which scripts poll.
// It asked FMOD directly (FUN_14055d680(block) -> FUN_140264780(..., block+0x10 handle)),
// past the redirected sites above, so a mission-giver resumed a step apart on the two PCs.
// Its one call, at 0x1401a9ca9, answers with the frame rule in a session, with the line's
// authored length (these are mission dialogue lines; the FMOD description is data).
static bool H_LineIsPlaying(BYTE* block) {
    if (g_clockLockstep && g_audioWaitOn && GetCurrentThreadId() == g_mainThread)
        // The real line length comes from the block's own audio object (*(block+8)); the global
        // lookup never found these lines, so every one counted as the 3 s fallback (run 74).
        return FramePlaying(g_voice, _countof(g_voice), block ? *(int*)(block + 0x10) : 0, true, block ? *(void**)(block + 8) : NULL);
    return ((bool (*)(BYTE*))(ExeBase() + 0x55d680))(block);
}

bool g_lineGateOk;                         // session: conversation lines end by the frame rule (H_LineIsPlaying)

// Replaces the script native Script_SfxHandleIsPlayingOnActor (0x140479390) outright with
// H_SfxIsPlaying (a 14-byte absolute jmp over its start), after checking its prologue and
// the helpers H_SfxIsPlaying calls in its place. Also looks up the two FMOD functions
// VoiceLengthFrames uses. Applies in record and replay as well as netplay (audio_wait=1).
static bool PatchAudioWait() {
    static const BYTE expect[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8D, 0x54, 0x24, 0x38 };
    if (!BytesAt(0x479390, expect, sizeof expect)) return false;
    // the helpers it calls, checked through its own call instructions
    if (!RipRefersTo(0x4793a2, 1, 5, 0x544790) || !RipRefersTo(0x4793ac, 1, 5, 0x544740) ||
        !RipRefersTo(0x4793c0, 1, 5, 0x26a6f0) || !RipRefersTo(0x4793e2, 1, 5, 0x544d30)) return false;
    HMODULE fm = GetModuleHandleW(L"fmodstudio.dll");
    if (fm) {
        g_fmodGetDesc = (PFN_FmodGetDesc)GetProcAddress(fm, "FMOD_Studio_EventInstance_GetDescription");
        g_fmodGetLength = (PFN_FmodGetLength)GetProcAddress(fm, "FMOD_Studio_EventDescription_GetLength");
    }
    BYTE jmp[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    void* target = (void*)H_SfxIsPlaying;
    memcpy(jmp + 6, &target, 8);
    if (!WriteCode(0x479390, jmp, sizeof jmp)) return false;
    // Real voice-line lengths (see "voice line lengths" above): the table, the hook that
    // recognises the dialog bank, and the one that notes each AI speaker line's name. Without
    // them every line keeps the 3 s fallback, as before; g_voiceTableHash = 0 says so in the
    // co-op settings sheet, so two PCs that differ here don't connect.
    // FUN_14045b960: mov [rsp+10h],rbx; mov [rsp+18h],rbp; mov [rsp+20h],rsi; push rdi
    static const BYTE say[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57 };
    bool table = LoadVoiceTable();
    bool bank = table && RedirectCall(0x267f90, 0x717880, (void*)H_CreateStream);
    g_sayRequestOrig = bank ? (PFN_SayRequest)Mod_Detour(0x45b960, say, sizeof say, (void*)H_SayRequest) : NULL;
    if (!g_sayRequestOrig) { g_voiceTableHash = 0; g_voiceFrames.clear(); }
    Log("voice lines: %s", g_sayRequestOrig ? "real lengths from the dialog bank's header" :
        !table ? "NOT measured (couldn't read DIALOGen-US\\dialog_str.fsb); every line counts 3 s" :
                 "NOT measured (the sound-bank or speaker code doesn't match this build); every line counts 3 s");
    if (g_sayRequestOrig)
        Log("voice lines: %zu lines, longest %u frames (table %08x)", g_voiceFrames.size(),
            (unsigned)*std::max_element(g_voiceFrames.begin(), g_voiceFrames.end()), g_voiceTableHash);
    return true;
}

// Diagnostic (plus the blink-timer fix below): the two script random-number natives (0x14049d4a0
// GetRandomReal and 0x14049d500 GetRandomRealExc) log the lockstep frame and
// the calling script, so two logs name the script that drew a frame apart.
// The VM (FUN_1405454f0) calls natives with its context: ctx+0 the loaded
// .sx, ctx+0x30 the instruction index (int, in code words). The header's path
// fields ([4] built .sx, [5] .sla source) are field-relative "+1" on disk; the
// loader rewrites some fields to self-relative + 0x4000000, so both are read.
// Both natives start with the same 14 relocatable bytes, moved to a trampoline.
typedef uint64_t (*PFN_ScriptNative)(BYTE* ctx);
static PFN_ScriptNative g_scriptRand[3];               // GetRandomReal, GetRandomRealExc, GetRandomInt
static LONG g_scriptRandLogs;
// A path field of a .sx header in either encoding (see above); NULL if empty.
static const char* SxField(BYTE* field) {
    int v = *(int*)field;
    if (!v) return NULL;
    return (const char*)field + (v > 0x2000000 ? v - 0x4000000 : v + 1);
}
// One "script rand" log line: lockstep frame, which native, the script's file name and the
// instruction index. Session only; at most 50000 lines.
static void LogScriptRand(int which, BYTE* ctx) {
    if (g_netPhase != NP_RUNNING || InterlockedIncrement(&g_scriptRandLogs) > 50000) return;
    char name[96] = "?";
    int pc = -1;
    __try { pc = *(int*)(ctx + 0x30); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try {
        // In memory the loader replaces header fields [4]/[5] with one 8-byte
        // pointer to the built .sx path (seen in the header dump: "9a0069c0 00000213").
        BYTE* prog = *(BYTE**)ctx;
        const char* s = prog ? *(const char**)(prog + 0x10) : NULL;
        if (s && s[0] > 0x20 && s[0] < 0x7f) strncpy_s(name, s, _TRUNCATE);
        else if (prog) for (int f = 4; f <= 5 && name[0] == '?'; f++)   // on-disk encoding, just in case
            if (const char* t = SxField(prog + f * 4)) strncpy_s(name, t, _TRUNCATE);
    } __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(name, "?"); }
    const char* tail = name;
    for (const char* p = name; *p; p++) if (*p == '/' || *p == '\\') tail = p + 1;
    Log("script rand %u: %s by %s at %04x", g_netFrame, which ? "exc" : "real", tail, pc);
    static bool dumped;                                  // name unreadable: show what ctx+0 points at, once
    if (name[0] == '?' && !dumped) {
        dumped = true;
        __try {
            BYTE* prog = *(BYTE**)ctx;
            const uint32_t* w = (const uint32_t*)prog;
            Log("script rand: ctx %p -> %p: %08x %08x %08x %08x | %08x %08x | +0x40 %08x; ctx words %08x %08x %08x %08x",
                ctx, prog, w[0], w[1], w[2], w[3], w[4], w[5], w[16], ((uint32_t*)ctx)[0], ((uint32_t*)ctx)[1], ((uint32_t*)ctx)[2], ((uint32_t*)ctx)[3]);
        } __except (EXCEPTION_EXECUTE_HANDLER) { Log("script rand: ctx %p unreadable", ctx); }
    }
}
// The blink script, recognised by its loaded header: file size 0x734 (+0xc)
// and 256 code words (+0x18); no other .sx in the game's archives matches both.
static bool IsBlinkDraw(BYTE* ctx, int wantPc) {
    __try {
        BYTE* prog = *(BYTE**)ctx;
        return prog && *(int*)(ctx + 0x30) == wantPc && *(uint32_t*)(prog + 0xc) == 0x734 && *(uint32_t*)(prog + 0x18) == 256;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Calls the game's native. In netplay mode a draw from the blink script at instruction
// blinkPc is flagged cosmetic (g_cosmeticDraw), so the engine RNG hook (sim_determinism.cpp)
// serves it from a separate generator and the shared one doesn't move.
static uint64_t CallScriptRand(int which, BYTE* ctx, int blinkPc) {
    bool cosmetic = g_cfg.mode == MODE_NETPLAY && IsBlinkDraw(ctx, blinkPc);
    if (cosmetic) g_cosmeticDraw = true;
    uint64_t r = g_scriptRand[which](ctx);
    g_cosmeticDraw = false;
    return r;
}
// The hooks: the two real-number natives are logged; GetRandomReal and GetRandomInt also
// keep the blink script's draws (at instructions 0xaf and 0x6b) off the shared generator.
static uint64_t H_ScriptRandReal(BYTE* ctx) { LogScriptRand(0, ctx); return CallScriptRand(0, ctx, 0xaf); }
static uint64_t H_ScriptRandD500(BYTE* ctx) { LogScriptRand(1, ctx); return g_scriptRand[1](ctx); }
static uint64_t H_ScriptRandInt(BYTE* ctx) { return CallScriptRand(2, ctx, 0x6b); }
// Detours one native: its prologue is copied to a trampoline (g_scriptRand[which], which
// then jumps back into the game's code) and its start becomes a jmp to hook.
static bool HookScriptRand(int which, DWORD rva, void* hook) {
    static const BYTE realExpect[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x0F, 0x29, 0x74, 0x24, 0x30, 0x48, 0x8B, 0xD9 };
    // GetRandomInt: mov [rsp+8],rbx; mov [rsp+18h],rsi; push rdi; sub rsp,20h
    static const BYTE intExpect[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20 };
    const BYTE* expect = which == 2 ? intExpect : realExpect;
    size_t expectLen = which == 2 ? sizeof intExpect : sizeof realExpect;
    if (!BytesAt(rva, expect, expectLen)) return false;
    BYTE* tr = (BYTE*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return false;
    memcpy(tr, expect, expectLen);                       // the moved prologue, then jmp [rip] back
    BYTE back[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    void* cont = ExeBase() + rva + expectLen;
    memcpy(back + 6, &cont, 8);
    memcpy(tr + expectLen, back, sizeof back);
    FlushInstructionCache(GetCurrentProcess(), tr, 64);
    g_scriptRand[which] = (PFN_ScriptNative)tr;
    BYTE jmp[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    memcpy(jmp + 6, &hook, 8);
    return WriteCode(rva, jmp, sizeof jmp);
}

static bool EnsureBtFreeHook();

// Broadphase proxy order. btHashedOverlappingPairCache add/find/remove order
// every pair by the proxies' ADDRESSES (lower address = proxy0), and every
// manifold for the pair takes body0/body1 from that: the solver then handles
// the same contact with swapped roles on two PCs, and the rounding differs.
// Seen as one body drifting in its low bits, then an extra contact, then an
// extra coin flip. Proxies (0x60 bytes) come from one bt_Alloc call in
// btDbvtBroadphase::createProxy and are freed by one bt_Free in destroyProxy;
// in netplay mode both go to an arena whose addresses only rise, so address
// order = creation order, which the simulation makes identical on both PCs.
static BYTE*  g_proxyArena;
static SIZE_T g_proxyArenaCommitted;
static volatile LONG64 g_proxyArenaUsed;
static const SIZE_T kProxyArenaReserve = 512u << 20, kProxyArenaChunk = 1u << 20;
static SRWLOCK g_proxyArenaLock = SRWLOCK_INIT;
static uint64_t g_proxyAllocs, g_proxyFallbacks;
static bool EnsureBtFreeHook();
// The redirected bt_Alloc calls (proxies and tree nodes): the next 16-byte-aligned block of
// the arena, zeroed, committing more of the reserve in 1 MB steps as it fills. Falls back
// to the game's bt_Alloc if the arena can't be used (no arena, a larger alignment, the free
// hook not in place, or the 512 MB reserve used up).
static void* H_ProxyAlloc(size_t size, int align) {
    typedef void* (*PFN_BtAlloc)(size_t, int);
    size_t need = (size + 15) & ~(size_t)15;
    if (g_proxyArena && align <= 16 && EnsureBtFreeHook()) {
        AcquireSRWLockExclusive(&g_proxyArenaLock);
        SIZE_T at = (SIZE_T)g_proxyArenaUsed;
        if (at + need <= kProxyArenaReserve) {
            if (at + need > g_proxyArenaCommitted) {
                SIZE_T grow = ((at + need - g_proxyArenaCommitted) + kProxyArenaChunk - 1) & ~(kProxyArenaChunk - 1);
                if (VirtualAlloc(g_proxyArena + g_proxyArenaCommitted, grow, MEM_COMMIT, PAGE_READWRITE)) g_proxyArenaCommitted += grow;
            }
            if (at + need <= g_proxyArenaCommitted) {
                g_proxyArenaUsed = at + need;
                g_proxyAllocs++;
                ReleaseSRWLockExclusive(&g_proxyArenaLock);
                memset(g_proxyArena + at, 0, need);
                return g_proxyArena + at;
            }
        }
        ReleaseSRWLockExclusive(&g_proxyArenaLock);
    }
    g_proxyFallbacks++;
    return ((PFN_BtAlloc)(ExeBase() + 0x713a60))(size, align);
}
// True for memory that came from the arena (never to be passed to the CRT's free).
static bool InArena(const void* p) {
    return g_proxyArena && (const BYTE*)p >= g_proxyArena && (const BYTE*)p < g_proxyArena + kProxyArenaReserve;
}
// destroyProxy's redirected bt_Free call.
static void H_ProxyFree(void* p) {
    if (InArena(p)) return;                              // arena: never reused
    ((void (*)(void*))(ExeBase() + 0x713ba0))(p);
}
// bt_Free (0x140713ba0) is "if (p) jmp [btAlignedFreeFunc]" with the function
// pointer at 0x14095a058; replacing that pointer makes EVERY Bullet free ignore
// arena memory, wherever it is called from (leaf removal, tree clears, rebuilds,
// broadphase teardown), so arena blocks can be handed out from any alloc site.
typedef void (*PFN_BtFreeFunc)(void*);
static PFN_BtFreeFunc g_btFreeOrig;
static void H_BtFreeFunc(void* p) {
    if (InArena(p)) return;
    g_btFreeOrig(p);
}
// The arena may only hand out memory while every Bullet free goes through
// H_BtFreeFunc; if anything replaced the pointer (btAlignedAllocSetCustom),
// hook it again first, or fall back to normal memory.
static bool EnsureBtFreeHook() {
    PFN_BtFreeFunc* slot = (PFN_BtFreeFunc*)(ExeBase() + 0x95a058);
    PFN_BtFreeFunc cur = *slot;
    if (cur == H_BtFreeFunc) return true;
    if (!cur) return false;
    g_btFreeOrig = cur;
    PFN_BtFreeFunc hook = H_BtFreeFunc;
    if (!WriteMem(0x95a058, &hook, sizeof hook, false)) return false;
    static int relogged;
    if (relogged++ < 4) Log("determinism: Bullet's free hook was replaced; hooked again");
    return true;
}
void KeepBtFreeHooked() { if (g_proxyArena) EnsureBtFreeHook(); }   // every lockstep frame (NetFrameBody)
// The dynamic AABB tree (btDbvt) rebalances a little every frame
// (optimizeIncremental), and its node "sort" compares NODE ADDRESSES to decide
// swaps, so the tree's shape, the order pairs are found in and the solver's
// order followed each process's heap layout. Seen: identical pair sets in a
// different order from a level's first frame. The broadphase trees' 0x40-byte
// nodes come from five bt_Alloc calls (decomp session's byte scan): btDbvt::
// insert 0x1406e13d8, insertleaf 0x1406e1520, and three inlined inserts in
// btDbvtBroadphase: createProxy 0x1406d93ff (every new proxy's leaf), collide
// 0x1406d6527 (stage -> fixed set) and setAabb 0x1406ef388 (fixed set -> set
// 0). All now come from the same rising-address arena, so node address order =
// creation order. (Compound-shape trees are never optimised; left alone.)
// Installed by InstallDeterminism in netplay mode, for the whole run (sessions or not); true only if every site
// was redirected. The arena is 512 MB of reserved address space, committed as used.
static bool InstallProxyArena() {
    static const BYTE size60[] = { 0xBA, 0x10, 0x00, 0x00, 0x00, 0xB9, 0x60, 0x00, 0x00, 0x00 };   // mov edx,10h; mov ecx,60h
    static const BYTE size40[] = { 0xBA, 0x10, 0x00, 0x00, 0x00, 0xB9, 0x40, 0x00, 0x00, 0x00 };   // mov edx,10h; mov ecx,40h
    static const BYTE freeJmp[] = { 0x48, 0x85, 0xC9, 0x74, 0x0D, 0xFF, 0x05 };                       // test rcx,rcx; je; inc [..]
    static const DWORD kNodeAllocCalls[] = { 0x6e13d8, 0x6e1520, 0x6d93ff, 0x6d6527, 0x6ef388 };
    // Every failure says which check, so a mismatch can't silently switch it all off.
    if (!BytesAt(0x6d9352, size60, sizeof size60)) { Log("determinism: arena: proxy alloc site differs"); return false; }
    if (!BytesAt(0x713ba0, freeJmp, sizeof freeJmp)) { Log("determinism: arena: bt_Free differs"); return false; }
    if (!RipRefersTo(0x713bab, 3, 7, 0x95a058)) { Log("determinism: arena: bt_Free's jmp [free hook] differs"); return false; }   // 48 FF 25 disp32
    for (DWORD c : kNodeAllocCalls)                      // each: mov edx,10h; mov ecx,40h; call bt_Alloc
        if (!BytesAt(c - 10, size40, sizeof size40)) { Log("determinism: arena: node alloc site 0x%x differs", c + 0x140000000u); return false; }
    g_proxyArena = (BYTE*)VirtualAlloc(NULL, kProxyArenaReserve, MEM_RESERVE, PAGE_NOACCESS);
    if (!g_proxyArena) { Log("determinism: arena: reserve failed"); return false; }
    // Frees first: an arena block must never reach the CRT.
    if (!EnsureBtFreeHook()) { Log("determinism: arena: Bullet's free hook not set yet"); return false; }
    if (!RedirectCall(0x6dc677, 0x713ba0, (void*)H_ProxyFree)) { Log("determinism: arena: destroyProxy free site differs"); return false; }
    if (!RedirectCall(0x6d935f, 0x713a60, (void*)H_ProxyAlloc)) { Log("determinism: arena: proxy alloc redirect failed"); return false; }
    int nodes = 0;
    for (DWORD c : kNodeAllocCalls) nodes += RedirectCall(c, 0x713a60, (void*)H_ProxyAlloc);
    Log("determinism: %d/%d AABB-tree node allocation sites use the arena", nodes, (int)_countof(kNodeAllocCalls));
    return nodes == (int)_countof(kNodeAllocCalls);
}

// Origin orphans. CAabbCollisionObject builds its body at the identity
// transform plus the object's AABB; objects whose AABB was still empty end up
// as invisible boxes AT THE WORLD ORIGIN, often a dynamic one inside a static
// twin, which pops out and tumbles (st_train) or falls out of the world for
// good (Toy Box). Nothing reads or moves them except physics (no component
// owner), but as moving bodies they amplify any last-bit difference into a
// desync. In netplay mode, PhysicsWorld_AddBody (FUN_140531da0) makes any
// fully dynamic body ((rec+0x10 & 3) == 3) with no owner whose origin
// (rec+0x60..0x68) is within 1 cm of (0,0,0) STATIC: record bit 0 cleared, so
// it's added with CF_STATIC and never integrated. Removal reads the same bit,
// so add/remove stay paired (decomp session's trace). Same rule on both PCs.
typedef uint64_t (*PFN_AddBody)(void* wrapper, uint32_t id);
static PFN_AddBody g_addBodyOrig;
static uint64_t g_orphansFrozen;
// Which bodies the level-package loader made (FUN_1402dbf00's Ctor_PhysicsBody call,
// return address 0x1402dc23f): only those can be the orphans NetParkOrphanBodies parks.
// Keyed by pool record; set or cleared at every add, so a reused record is re-judged.
static BYTE* g_pkgBodyRec[16384];                     // pool records are recycled, so distinct pointers stay bounded
static bool  g_pkgBodyIs[16384];
static int   g_pkgBodyN;
// Records whether pool record rec was made by the package loader: an open-addressing hash
// table keyed by the record's address. Once three quarters full it stops changing: marks
// already made stay, and records not in it read as not package-made.
static void PkgBodyNote(BYTE* rec, bool pkg) {
    if (g_pkgBodyN >= (int)(_countof(g_pkgBodyRec) * 3 / 4)) return;   // keep the table sparse; lookups stay short
    size_t h = ((uintptr_t)rec >> 4) % _countof(g_pkgBodyRec);
    for (size_t i = 0; i < _countof(g_pkgBodyRec); i++, h = (h + 1) % _countof(g_pkgBodyRec))
        if (g_pkgBodyRec[h] == rec || !g_pkgBodyRec[h]) {
            if (!g_pkgBodyRec[h]) g_pkgBodyN++;
            g_pkgBodyRec[h] = rec; g_pkgBodyIs[h] = pkg;
            return;
        }
}
// True if rec's latest add was by the package loader.
static bool PkgBodyMade(BYTE* rec) {
    size_t h = ((uintptr_t)rec >> 4) % _countof(g_pkgBodyRec);
    for (size_t i = 0; i < _countof(g_pkgBodyRec); i++, h = (h + 1) % _countof(g_pkgBodyRec)) {
        if (g_pkgBodyRec[h] == rec) return g_pkgBodyIs[h];
        if (!g_pkgBodyRec[h]) return false;
    }
    return false;
}

// The detoured PhysicsWorld_AddBody (see "Origin orphans" above). id is the body's handle
// in the physics manager's pool (*0x140b275c8); FUN_140418870 turns it into the pool record.
// In netplay mode: note whether the package loader made it, apply the origin-orphan rule,
// and log other dynamic bodies that appear near the origin. Then the game's own add.
static uint64_t H_AddBody(void* wrapper, uint32_t id) {
    if (g_cfg.mode == MODE_NETPLAY) {
        __try {
            BYTE* b = ExeBase();
            BYTE* mgr = *(BYTE**)(b + 0xb275c8);
            BYTE* rec = mgr ? ((BYTE* (*)(BYTE*, uint16_t))(b + 0x418870))(mgr, (uint16_t)id) : NULL;
            if (rec) {
                // The loader's body is added twice: by Ctor_PhysicsBody (def 0, no shape) and
                // again when the package's next record, Ctor_Association_CollisionShapeToPhysicsBody
                // (0x140533390..0x1405335bb), gives it its shape (FUN_14040eb70 re-adds it). Both
                // count as package-made; otherwise the re-add cleared the mark and the train
                // level's never-placed box (def 442) escaped parking (decomp session).
                void* frames[12];
                USHORT nf = RtlCaptureStackBackTrace(1, 12, frames, NULL);
                bool pkg = false;
                for (USHORT i = 0; i < nf; i++) {
                    BYTE* r = (BYTE*)frames[i];
                    if (r == b + 0x2dc23f || (r > b + 0x533390 && r <= b + 0x5335bb)) pkg = true;
                }
                PkgBodyNote(rec, pkg);
            }
            if (rec && (*(BYTE*)(rec + 0x10) & 3) == 3) {
                // Owner: the record's link type (+0x14 & 3) is 1 and its component (+8 -> +0x10) is set.
                bool owner = false;
                if ((*(BYTE*)(rec + 0x14) & 3) == 1)
                    if (BYTE* h = *(BYTE**)(rec + 8)) owner = *(BYTE**)(h + 0x10) != NULL;
                float x = *(float*)(rec + 0x60), y = *(float*)(rec + 0x64), z = *(float*)(rec + 0x68);
                if (!owner && fabsf(x) < 0.01f && fabsf(y) < 0.01f && fabsf(z) < 0.01f) {
                    *(BYTE*)(rec + 0x10) &= ~1;
                    if (++g_orphansFrozen <= 20)
                        Log("determinism: origin orphan body %u (slot %u, def %u) added as static", id, *(uint16_t*)rec, *(uint16_t*)(rec + 2));
                } else if (fabsf(x) < 1.0f && fabsf(y) < 1.0f && fabsf(z) < 1.0f) {
                    // Diagnostic: a free-moving body appearing near the origin that
                    // the rule above left alone (Toy Box's first body, a character-
                    // shaped btMultiSphereShape, is flung from here and falls forever).
                    static int seen;
                    if (seen++ < 20) {
                        uint64_t shapeVt = 0, compVt = 0, key = 0;
                        char nm[48] = "?", dn[48] = "?";
                        if (BYTE* shape = *(BYTE**)(rec + 0x20 + 0xb8)) shapeVt = *(uint64_t*)shape - (uint64_t)b + 0x140000000ULL;
                        if ((*(BYTE*)(rec + 0x14) & 3) == 1)
                            if (BYTE* h = *(BYTE**)(rec + 8))
                                if (BYTE* comp = *(BYTE**)(h + 0x10)) {
                                    compVt = *(uint64_t*)comp - (uint64_t)b + 0x140000000ULL;
                                    key = *(uint64_t*)(comp + 0x58);
                                    BYTE* actor = ((BYTE* (*)(uint64_t))(b + 0x558400))(key);
                                    if (!actor) {
                                        // Not live: look in the registry directly (bucket key & 0xff,
                                        // node {object +0, key +8, next +0x18}) and say why it's not live.
                                        for (BYTE* node = ((BYTE**)(b + 0xb2bb90))[key & 0xff]; node; node = *(BYTE**)(node + 0x18))
                                            if (*(uint64_t*)(node + 8) == key) {
                                                actor = *(BYTE**)node;
                                                strcpy_s(nm, "(not live)");
                                                if (actor) Log("phys: owner key %llu is registered but not live: type flags %08x (live needs (flags & %08x) == %08x)",
                                                               (unsigned long long)key, *(uint32_t*)(actor + 0x10), *(uint32_t*)(b + 0x94841c),
                                                               *(uint32_t*)(b + 0x948418));
                                                break;
                                            }
                                        if (!actor) strcpy_s(nm, "(not registered)");
                                    }
                                    if (actor) {
                                        if (const char* s = *(const char**)(actor + 0x148)) strncpy_s(nm, s, _TRUNCATE);
                                        if (const char* s = *(const char**)(actor + 0x150)) strncpy_s(dn, s, _TRUNCATE);
                                    }
                                }
                        Log("phys: dynamic body %u (slot %u, def %u) added near the origin at (%.3f %.3f %.3f): shape %llx, "
                            "owner %s comp vt %llx key %llu, actor %s / %s", id, *(uint16_t*)rec, *(uint16_t*)(rec + 2), x, y, z,
                            (unsigned long long)shapeVt, owner ? "yes" : "no", (unsigned long long)compVt, (unsigned long long)key, nm, dn);
                        // Who adds it: the game's return addresses (exe only), to name with functions.tsv.
                        // The train-level resolution desync starts on one of these bodies.
                        void* frames[20];
                        USHORT nf = RtlCaptureStackBackTrace(1, 20, frames, NULL);
                        char st[400];
                        int sl = 0;
                        st[0] = 0;
                        for (USHORT i = 0; i < nf && sl < (int)sizeof st - 20; i++) {
                            uintptr_t a = (uintptr_t)frames[i];
                            if (a >= (uintptr_t)b && a < (uintptr_t)b + 0x1000000)
                                sl += sprintf_s(st + sl, sizeof st - sl, " %llx", (unsigned long long)(a - (uintptr_t)b + 0x140000000ULL));
                        }
                        Log("phys:   body %u added by%s", id, st[0] ? st : " (no game frames)");
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return g_addBodyOrig(wrapper, id);
}
// Physics::RigidBody, the game's 200-byte wrapper around each Bullet body, comes from
// Mem_Alloc (0x140241aa0) at three constructors (Ctor_PhysicsBody 0x1405349c2,
// FUN_140534be0 0x140534c11, FUN_140534da0 0x140534dcf), and they leave +0x74, +0x84,
// +0xbc and +0xc4 unwritten (decomp session): whatever the heap held there, which
// differs per process (different resolutions mean different allocation histories).
// Bodies then split in their last bits on the two PCs (train level: an orphan box,
// the board game's dice). Those three allocations are zero-filled; every field the
// constructors write is unchanged.
typedef void* (*PFN_RbMemAlloc)(uint64_t n, uint64_t a2, uint64_t a3, uint64_t a4);
static volatile LONG g_rbZeroed;
// Mem_Alloc, then the block zeroed (when it's at most 4 KB, as these wrappers are).
static void* H_RigidBodyAlloc(uint64_t n, uint64_t a2, uint64_t a3, uint64_t a4) {
    void* p = ((PFN_RbMemAlloc)(ExeBase() + 0x241aa0))(n, a2, a3, a4);
    if (p && n && n <= 0x1000) { memset(p, 0, (size_t)n); InterlockedIncrement(&g_rbZeroed); }
    return p;
}
bool g_rbZeroOn;
// Redirects the three constructors' Mem_Alloc calls to H_RigidBodyAlloc (every run in
// netplay mode, session or not). True only if all three were redirected.
static bool PatchRigidBodyAlloc() {
    static const DWORD kSites[] = { 0x5349c2, 0x534c11, 0x534dcf };
    static const BYTE kCallOp = 0xE8;
    for (DWORD site : kSites)                            // all or none: check every call first
        if (!BytesAt(site, &kCallOp, 1)) return false;
    int n = 0;
    for (DWORD site : kSites) n += RedirectCall(site, 0x241aa0, (void*)H_RigidBodyAlloc);
    return n == (int)_countof(kSites);
}

// Detours PhysicsWorld_AddBody (0x140531da0) to H_AddBody: the usual pattern in this file.
// The function's first instructions are copied to a trampoline that jumps back after them
// (g_addBodyOrig calls the game's function through it), and its start becomes a 14-byte
// "jmp [rip+0]" plus the hook's address. Only possible when the copied bytes have no
// RIP-relative operands, which the byte check confirms.
static bool HookAddBody() {
    static const BYTE expect[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20 };
    if (!BytesAt(0x531da0, expect, sizeof expect)) return false;
    BYTE* fn = ExeBase() + 0x531da0;
    BYTE* tr = (BYTE*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return false;
    memcpy(tr, expect, sizeof expect);                   // the moved prologue (no RIP-relative operands), then jmp back
    BYTE back[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    void* cont = fn + sizeof expect;
    memcpy(back + 6, &cont, 8);
    memcpy(tr + sizeof expect, back, sizeof back);
    FlushInstructionCache(GetCurrentProcess(), tr, 64);
    g_addBodyOrig = (PFN_AddBody)tr;
    BYTE jmp[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    void* hook = (void*)H_AddBody;
    memcpy(jmp + 6, &hook, 8);
    return WriteCode(0x531da0, jmp, sizeof jmp);
}

// Orphan physics bodies (stock content bug; decomp session). A streamed package can
// hold a PhysicsBody that no PhysicsBodyToSceneTreeNode association ever binds: it
// never gets its pose, motion state or inertia set up, so it drops at the world origin
// running on whatever the heap held, which differs per process (different resolutions
// mean different allocation histories). In the train level such an invisible box at
// the origin was the first body to split, and its contacts then fed the overlap
// callback's RNG. Every lockstep frame, the same on both PCs: a dynamic body whose
// PhysicsBody (record +8 -> +0x10, Physics::RigidBody) has no bound node (+0xc0) and
// was made by the package loader, has no owner actor and has only fallen straight down
// from the origin is parked at the origin, its velocities zeroed, its simulation
// disabled (activation state 5) and its broadphase proxy filtered out of all pairs
// (group/mask +8/+0xa). Bodies spawned at run time (crates, thrown props) never go
// through the package loader, so they're never candidates, even in levels around the origin.
// (The train level's box, def 442, is added twice, the second time when its shape is
// attached; see H_AddBody.) A parked body that gets a node later is given its collision
// filter and activation state back.
void NetParkOrphanBodies(uint32_t f) {
    struct Parked { BYTE* o; BYTE* rec; int act; int16_t group, mask; };   // collision object, its pool record, what parking changed
    static Parked parked[64];
    static int parkedN, parkedSession = -1;
    if (parkedSession != g_netSessions) { parkedSession = g_netSessions; parkedN = 0; }
    BYTE* b = ExeBase();
    // The Bullet collision world (pointer at +0x1168 in the physics manager, *0x140b275c8)
    // and its array of collision objects (count +0xc, array +0x18), walked in array order.
    __try {
        BYTE* phys = *(BYTE**)(b + 0xb275c8);
        BYTE* world = phys ? *(BYTE**)(phys + 0x1168) : NULL;
        if (!world) return;
        int n = *(int*)(world + 0x0c);
        BYTE** arr = *(BYTE***)(world + 0x18);
        if (n <= 0 || n > 200000 || !arr) return;
        for (int i = 0; i < n; i++) {
            BYTE* o = arr[i];
            if (!o || (*(uint32_t*)(o + 0xc8) & 3) || *(int*)(o + 0xf0) != 2) continue;   // static, kinematic, or not a rigid body
            BYTE* rec = *(BYTE**)(o + 0xe8);
            {
                // Diagnostic: once per body, why a dynamic body near the origin isn't parked.
                static BYTE* told[32];
                static int toldN;
                const float* q = (const float*)(o + 0x40);
                bool atOrigin = fabsf(q[0]) < 1.0f && fabsf(q[1]) < 8.0f && fabsf(q[2]) < 1.0f;
                bool seen = false;
                for (int k = 0; k < toldN; k++) seen = seen || told[k] == o;
                if (atOrigin && !seen && toldN < (int)_countof(told)) {
                    told[toldN++] = o;
                    int link = rec ? (*(BYTE*)(rec + 0x14) & 3) : -1;
                    BYTE* hh = rec && link == 1 ? *(BYTE**)(rec + 8) : NULL;
                    BYTE* bd = hh ? *(BYTE**)(hh + 0x10) : NULL;
                    bool rb = bd && *(uint64_t*)bd == (uint64_t)(b + 0x7edca8);
                    Log("physics: frame %u: body #%d slot %d def %d near the origin: link %d, %s, node %p, owner key %llx, package-made %d",
                        f, i, rec ? *(uint16_t*)rec : -1, rec ? *(uint16_t*)(rec + 2) : -1, link,
                        bd ? (rb ? "Physics::RigidBody" : "other component") : "no component", rb ? *(BYTE**)(bd + 0xc0) : NULL,
                        rb ? (unsigned long long)*(uint64_t*)(bd + 0x58) : 0ULL, rec ? (int)PkgBodyMade(rec) : -1);
                }
            }
            // Is it parked already, and is it (still) an orphan?
            int known = -1;
            for (int k = 0; k < parkedN; k++) if (parked[k].o == o && parked[k].rec == rec) known = k;
            bool orphan = rec && (*(BYTE*)(rec + 0x14) & 3) == 1;
            BYTE* h = orphan ? *(BYTE**)(rec + 8) : NULL;
            BYTE* body = h ? *(BYTE**)(h + 0x10) : NULL;
            orphan = body && *(uint64_t*)body == (uint64_t)(b + 0x7edca8) && !*(BYTE**)(body + 0xc0)   // no node bound: never placed
                     && !*(uint64_t*)(body + 0x58) && PkgBodyMade(rec);       // no owner actor; made by the package loader
            if (!orphan) {
                if (known >= 0) {
                    // Placed after all (a late binding): give it its collision and simulation back.
                    // Its pose then comes from the node, as for any placed body.
                    if (BYTE* proxy = *(BYTE**)(o + 0xb0)) { *(int16_t*)(proxy + 8) = parked[known].group; *(int16_t*)(proxy + 0xa) = parked[known].mask; }
                    *(int*)(o + 0xd4) = parked[known].act;
                    Log("physics: frame %u: parked body slot %u (def %u) was placed after all; collision and simulation back on",
                        f, rec ? *(uint16_t*)rec : 0, rec ? *(uint16_t*)(rec + 2) : 0);
                    parked[known] = parked[--parkedN];
                }
                continue;
            }
            // A new orphan: park it only if it is still within a fall of the origin, and
            // remember what parking changes so it can be put back.
            if (known < 0) {
                float* p = (float*)(o + 0x40);
                if (fabsf(p[0]) > 1.0f || fabsf(p[1]) > 8.0f || fabsf(p[2]) > 1.0f) continue;   // not just dropped from the origin
                if (parkedN < (int)_countof(parked)) {
                    Parked& e = parked[parkedN++];
                    e.o = o; e.rec = rec; e.act = *(int*)(o + 0xd4);
                    BYTE* proxy = *(BYTE**)(o + 0xb0);
                    e.group = proxy ? *(int16_t*)(proxy + 8) : 0;
                    e.mask = proxy ? *(int16_t*)(proxy + 0xa) : 0;
                }
                Log("physics: frame %u: orphan body slot %u (def %u) at (%.3f %.3f %.3f) was never placed by the level; parked at the origin, "
                    "collision and simulation off", f, *(uint16_t*)rec, *(uint16_t*)(rec + 2), p[0], p[1], p[2]);
            }
            // Re-applied every frame: identity transform and interpolation transform,
            // no velocities, simulation disabled, out of every broadphase pair.
            static const float kIdentity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0 };
            memcpy(o + 0x10, kIdentity, 0x40);
            memcpy(o + 0x50, kIdentity, 0x40);
            memset(o + 0x90, 0, 0x20);                                           // interpolation velocities
            memset(o + 0x140, 0, 0x20);                                          // linear, angular velocity
            *(int*)(o + 0xd4) = 5;                                               // DISABLE_SIMULATION
            if (BYTE* proxy = *(BYTE**)(o + 0xb0)) { *(int16_t*)(proxy + 8) = 0; *(int16_t*)(proxy + 0xa) = 0; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Diagnostic: who creates each entity key in a session. Keys come from one counter
// (DAT_1409575c8) at two places: FUN_140023cc0, the registry node every registered
// object gets (node {object +0, key +8, prev +0x10, next +0x18}, bucket key & 0xff at
// 0x140b2bb90), and CStageEntity::ctor (0x1400253c0, key at +0x38). The online cow
// desync ended on a counter one higher on the client: a key made on one PC only. Each
// lockstep frame gets one "newkeys" line with every key made, its caller (return
// address) and the object's vtable, so two logs name the extra one.
static SRWLOCK g_keyLogLock = SRWLOCK_INIT;
static char    g_keyLog[1600];
static int     g_keyLogLen, g_keyLogCount;
// Adds " key<caller:vtable>" to this frame's line (both as 0x14... addresses when they're in
// the exe). Session only; any thread, so under g_keyLogLock.
static void KeyNote(uint64_t key, void* caller, const void* obj) {
    if (g_netPhase != NP_RUNNING) return;
    uint64_t vt = 0;
    __try { if (obj) vt = *(const uint64_t*)obj; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    BYTE* b = ExeBase();
    uint64_t c = (uint64_t)caller, v = vt;
    if (c >= (uint64_t)b && c < (uint64_t)b + 0x1000000) c = c - (uint64_t)b + 0x140000000ULL;
    if (v >= (uint64_t)b && v < (uint64_t)b + 0x1000000) v = v - (uint64_t)b + 0x140000000ULL;
    AcquireSRWLockExclusive(&g_keyLogLock);
    g_keyLogCount++;
    if (g_keyLogLen < (int)sizeof g_keyLog - 48)
        g_keyLogLen += sprintf_s(g_keyLog + g_keyLogLen, sizeof g_keyLog - g_keyLogLen, " %llu<%llx:%llx",
                                 (unsigned long long)key, (unsigned long long)c, (unsigned long long)v);
    ReleaseSRWLockExclusive(&g_keyLogLock);
}
// Once per lockstep frame (NetFrameBody): writes and clears frame f's "newkeys" line.
void KeyLogFlush(uint32_t f) {
    // Also the Meridian surge counter (DAT_140956ec4, +2 per new surge; ActorCarried keeps
    // one as its handle; decomp session): logged when it moves, to rule it out.
    static uint32_t lastSurge;
    uint32_t surge = 0;
    __try { surge = *(volatile uint32_t*)(ExeBase() + 0x956ec4); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (surge != lastSurge) { Log("surge %u: %u", f, surge); lastSurge = surge; }
    AcquireSRWLockExclusive(&g_keyLogLock);
    if (g_keyLogCount) Log("newkeys %u: %d%s%s", f, g_keyLogCount, g_keyLog, g_keyLogLen >= (int)sizeof g_keyLog - 48 ? " ..." : "");
    g_keyLogLen = 0;
    g_keyLogCount = 0;
    g_keyLog[0] = 0;
    ReleaseSRWLockExclusive(&g_keyLogLock);
}
// FUN_140023cc0, rewritten the same (its 4th instruction is RIP-relative, so no trampoline):
// node[0] = object, node[1] = key (counter post-increment, skipping 0), node[2] = 0, then
// pushed onto bucket (key & mask), mask = the qword at 0x1407eee70.
static uint64_t* H_KeyNode(uint64_t* node, uint64_t obj) {
    BYTE* b = ExeBase();
    volatile uint64_t* ctr = (volatile uint64_t*)(b + 0x9575c8);
    uint64_t* buckets = (uint64_t*)(b + 0xb2bb90);
    uint64_t mask = *(const uint64_t*)(b + 0x7eee70);          // the bucket mask the original reads (rip-relative)
    node[0] = obj;
    uint64_t k = *ctr;
    node[1] = k;
    *ctr = k + 1;
    if (*ctr == 0) *ctr = 1;
    node[2] = 0;
    node[3] = buckets[node[1] & mask];
    buckets[node[1] & mask] = (uint64_t)node;
    if (node[3]) *(uint64_t*)(node[3] + 0x10) = (uint64_t)node;
    KeyNote(k, _ReturnAddress(), (const void*)obj);
    return node;
}
// The game's unpause, held back while the load gate is armed (see g_loadGate).
static uint32_t H_Unpause() {
    if (g_loadGate && g_netPhase == NP_RUNNING) {
        if (g_unpauseBlocked++ == 0) Log("load gate: an unpause at frame %u held back until both PCs are loaded", g_netFrame);
        __try {
            if (BYTE* disp = *(BYTE**)(ExeBase() + kDisplayPtr)) return (*(uint32_t*)(disp + 0x1a8) >> 3) & 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 1;
    }
    return g_unpauseOrig();
}
// FUN_140477500 starts: sub rsp,28h; mov rax,[rip+g_Display]; test byte [rax+1a8h],8; jz ...
// The second instruction is RIP-relative, so the trampoline re-encodes it with the
// absolute address (mov rax,imm64; mov rax,[rax]) and jumps back to the jz (an absolute
// jmp keeps the flags from the test).
static bool HookUnpause() {
    static const BYTE head[] = { 0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x05 };
    static const BYTE test[] = { 0xF6, 0x80, 0xA8, 0x01, 0x00, 0x00, 0x08, 0x74 };
    if (!BytesAt(0x477500, head, sizeof head) || !BytesAt(0x47750b, test, sizeof test) ||
        !RipRefersTo(0x477504, 3, 7, kDisplayPtr)) return false;
    BYTE* fn = ExeBase() + 0x477500;
    BYTE* tr = (BYTE*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return false;
    int n = 0;
    memcpy(tr + n, head, 4); n += 4;                                   // sub rsp,28h
    tr[n++] = 0x48; tr[n++] = 0xB8;                                    // mov rax, &g_Display
    uint64_t gd = (uint64_t)(ExeBase() + kDisplayPtr);
    memcpy(tr + n, &gd, 8); n += 8;
    tr[n++] = 0x48; tr[n++] = 0x8B; tr[n++] = 0x00;                    // mov rax,[rax]
    memcpy(tr + n, test, 7); n += 7;                                   // test byte [rax+1a8h],8
    BYTE back[14] = { 0xFF, 0x25, 0, 0, 0, 0 };                        // jmp to the original jz
    void* cont = fn + 0x12;
    memcpy(back + 6, &cont, 8);
    memcpy(tr + n, back, sizeof back);
    FlushInstructionCache(GetCurrentProcess(), tr, 64);
    g_unpauseOrig = (uint32_t (*)())tr;
    BYTE jmp[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    void* hook = (void*)H_Unpause;
    memcpy(jmp + 6, &hook, 8);
    if (!WriteCode(0x477500, jmp, sizeof jmp)) { g_unpauseOrig = NULL; return false; }
    return true;
}

// The world update, skipped while the load gate is armed (see g_worldUpdateOrig).
static void H_WorldUpdate(void* loop) {
    if (g_loadGate && g_netPhase == NP_RUNNING) {
        if (g_worldFrozen++ == 0) Log("load gate: world frozen from frame %u until both PCs are loaded and streamed", g_netFrame);
        return;
    }
    g_worldUpdateOrig(loop);
}
// WilburStandAloneLoop's vtable (0x1407b2e00) slot 11 = WilburStandAloneLoop::vf11, which
// StandAloneLoop::vf24 calls with "mov rax,[rbx]; mov rcx,rbx; call [rax+58h]" (0x1405dd436).
static bool HookWorldFreeze() {
    static const BYTE call[] = { 0x48, 0x8B, 0x03, 0x48, 0x8B, 0xCB, 0xFF, 0x50, 0x58 };
    BYTE* slot = ExeBase() + 0x7b2e58;
    if (!BytesAt(0x5dd438, call, sizeof call) || *(BYTE**)slot != ExeBase() + 0x21ed70) return false;
    g_worldUpdateOrig = (void (*)(void*))(ExeBase() + 0x21ed70);
    void* hook = (void*)H_WorldUpdate;
    if (!WriteCode(0x7b2e58, &hook, sizeof hook)) { g_worldUpdateOrig = NULL; return false; }
    return true;
}

typedef void* (*PFN_StageCtor)(BYTE* self, uint64_t a2, uint64_t a3, uint64_t a4);
static PFN_StageCtor g_stageCtorOrig;
// CStageEntity::ctor, detoured: the game's constructor, then its new key (+0x38) is noted.
static void* H_StageCtor(BYTE* self, uint64_t a2, uint64_t a3, uint64_t a4) {
    void* r = g_stageCtorOrig(self, a2, a3, a4);
    uint64_t key = 0;
    __try { key = *(uint64_t*)(self + 0x38); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    KeyNote(key, _ReturnAddress(), self);
    return r;
}
// Hooks both key makers for the "newkeys" log, in netplay mode: CStageEntity::ctor through a
// trampoline (H_StageCtor), and FUN_140023cc0 replaced outright by H_KeyNode. The whole of
// FUN_140023cc0 is byte-checked first, since H_KeyNode does its entire job.
static bool HookKeyMakers() {
    // FUN_140023cc0, all 106 bytes (counter at 0x1409575c8, buckets at 0x140b2bb90).
    static const BYTE node[] = {
        0x45, 0x33, 0xc9, 0x48, 0x89, 0x11, 0x4c, 0x89, 0x49, 0x08, 0x4c, 0x8b, 0xc1, 0x48, 0x8b, 0x15, 0xf4, 0x38, 0x93, 0x00,
        0x48, 0x8d, 0x42, 0x01, 0x48, 0x89, 0x05, 0xe9, 0x38, 0x93, 0x00, 0x48, 0x89, 0x51, 0x08, 0x4c, 0x39, 0x0d, 0xde, 0x38,
        0x93, 0x00, 0x75, 0x0b, 0x48, 0xc7, 0x05, 0xd1, 0x38, 0x93, 0x00, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x0d, 0x72, 0xb1,
        0x7c, 0x00, 0x48, 0x8d, 0x15, 0x8b, 0x7e, 0xb0, 0x00, 0x49, 0x23, 0x48, 0x08, 0x4d, 0x89, 0x48, 0x10, 0x48, 0x8b, 0x04,
        0xca, 0x49, 0x89, 0x40, 0x18, 0x4c, 0x89, 0x04, 0xca, 0x49, 0x8b, 0x40, 0x18, 0x48, 0x85, 0xc0, 0x74, 0x04, 0x4c, 0x89,
        0x40, 0x10, 0x49, 0x8b, 0xc0, 0xc3 };            // all 106 bytes, so the C++ copy below is exact
    static const BYTE stage[] = { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x4c, 0x24, 0x08, 0x55, 0x56, 0x57,
                                  0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x20 };
    if (!BytesAt(0x23cc0, node, sizeof node) || !BytesAt(0x253c0, stage, sizeof stage)) return false;
    BYTE* fn = ExeBase() + 0x253c0;
    BYTE* tr = (BYTE*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return false;
    memcpy(tr, stage, sizeof stage);                     // no RIP-relative operands in these 21 bytes
    BYTE back[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    void* cont = fn + sizeof stage;
    memcpy(back + 6, &cont, 8);
    memcpy(tr + sizeof stage, back, sizeof back);
    FlushInstructionCache(GetCurrentProcess(), tr, 64);
    g_stageCtorOrig = (PFN_StageCtor)tr;
    BYTE jmp[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    void* h1 = (void*)H_StageCtor;
    memcpy(jmp + 6, &h1, 8);
    if (!WriteCode(0x253c0, jmp, sizeof jmp)) return false;
    void* h2 = (void*)H_KeyNode;
    memcpy(jmp + 6, &h2, 8);
    return WriteCode(0x23cc0, jmp, sizeof jmp);
}

// Game collision callbacks in a fixed pair order. After Bullet's narrowphase,
// Physics::DynamicsWorld::vf4 (0x14053d230) walks the pair cache once more with
// the game's own callbacks (btHashedOverlappingPairCache::vf10, 0x1406e6ec0,
// vtable 0x14081f200 slot 10; vf11 slot 11 is the one-callback form).
// CollisionDetectAcceptCallback::processOverlap (0x14053d270) draws a global-MT
// coin per touching actor pair to pick which actor is handled first, then sends
// "ActorsCollided" both ways. The pair ARRAY order drifted between PCs (Toy Box
// build 34: same pairs, different order in 482 frames from frame 2950), so the
// same coins landed on different pairs. In netplay mode, when every callback is
// one of the game's two (accept 0x1407ee1f8, audio 0x1407ee1c8; both always
// return false), the pairs are walked sorted by the two proxies' unique ids.
// Each pair is looked up again (findPair, slot 12) right before its callbacks,
// so a pair removed by an event handler mid-walk is skipped, not read stale.
// Pair: proxy0 +0, proxy1 +8 (0x20 bytes); cache: count +0x14, pairs +0x20.
typedef void (*PFN_PairWalkMulti)(BYTE* cache, BYTE* cbs, void* disp);
typedef void (*PFN_PairWalk)(BYTE* cache, BYTE* cb, void* disp);
typedef BYTE* (*PFN_FindPair)(BYTE* cache, BYTE* p0, BYTE* p1);
typedef void (*PFN_RemovePair)(BYTE* cache, BYTE* p0, BYTE* p1, void* disp);
static uint64_t g_pairWalkSorted;
// True if cb is one of the game's two overlap callbacks (by its vtable).
static bool IsGameOverlapCallback(BYTE* cb) {
    if (!cb) return false;
    uintptr_t vt = *(uintptr_t*)cb - (uintptr_t)ExeBase();
    return vt == 0x7ee1f8 || vt == 0x7ee1c8;
}
struct SortedPair { uint32_t u0, u1; BYTE* p0; BYTE* p1; };   // the proxies' unique ids (+0x18) and the proxies
static bool SortedPairLess(const SortedPair& a, const SortedPair& b) { return a.u0 != b.u0 ? a.u0 < b.u0 : a.u1 < b.u1; }
// The game's pair walk, redone in a fixed order: snapshot the pairs, sort them by proxy ids,
// then run each callback over them; a callback returning true removes the pair, as Bullet's
// own walk does.
static void WalkPairsSorted(BYTE* cache, BYTE** cbs, int ncb, void* disp) {
    int n = *(int*)(cache + 0x14);
    BYTE* data = *(BYTE**)(cache + 0x20);
    std::vector<SortedPair> v;
    v.reserve(n > 0 ? n : 0);
    for (int i = 0; i < n; i++) {
        BYTE* p0 = *(BYTE**)(data + i * 0x20);
        BYTE* p1 = *(BYTE**)(data + i * 0x20 + 8);
        if (p0 && p1) v.push_back({ *(uint32_t*)(p0 + 0x18), *(uint32_t*)(p1 + 0x18), p0, p1 });
    }
    std::sort(v.begin(), v.end(), SortedPairLess);
    void** vt = *(void***)cache;
    PFN_FindPair find = (PFN_FindPair)vt[12];
    PFN_RemovePair remove = (PFN_RemovePair)vt[2];
    if (g_pairWalkSorted++ == 0) Log("determinism: first fixed-order collision callback walk (%d pairs, %d callbacks)", n, ncb);
    for (int c = 0; c < ncb; c++)                        // same nesting as the original: callback outer, pairs inner
        for (const SortedPair& sp : v) {
            BYTE* pair = find(cache, sp.p0, sp.p1);
            if (!pair) continue;
            BYTE* cb = cbs[c];
            if ((*(bool (**)(BYTE*, BYTE*))(*(BYTE**)cb + 8))(cb, pair)) {
                remove(cache, sp.p0, sp.p1, disp);
                (*(volatile int*)(ExeBase() + 0xb30f18))--;   // gOverlappingPairs, as the original does
            }
        }
}
// vtable slot 10 (a list of callbacks: count +4, array +0x10). Sorted only when every
// callback is one of the game's two; anything else gets Bullet's own walk.
static void H_PairWalkMulti(BYTE* cache, BYTE* cbs, void* disp) {
    if (g_cfg.mode == MODE_NETPLAY && cbs) {
        int ncb = *(int*)(cbs + 4);
        BYTE** data = *(BYTE***)(cbs + 0x10);
        bool ours = ncb > 0 && data;
        for (int c = 0; ours && c < ncb; c++) ours = IsGameOverlapCallback(data[c]);
        if (ours) { WalkPairsSorted(cache, data, ncb, disp); return; }
    }
    ((PFN_PairWalkMulti)(ExeBase() + 0x6e6ec0))(cache, cbs, disp);
}
// vtable slot 11 (one callback), the same rule.
static void H_PairWalk(BYTE* cache, BYTE* cb, void* disp) {
    if (g_cfg.mode == MODE_NETPLAY && IsGameOverlapCallback(cb)) { WalkPairsSorted(cache, &cb, 1, disp); return; }
    ((PFN_PairWalk)(ExeBase() + 0x6e6f70))(cache, cb, disp);
}
// Swaps vtable slots 10 and 11 of the pair cache's vtable for the hooks, after checking
// those slots and the two the sorted walk calls (findPair 12, the pair removal 2).
static bool HookPairWalks() {
    void** slots = (void**)(ExeBase() + 0x81f200);
    if ((BYTE*)slots[10] != ExeBase() + 0x6e6ec0 || (BYTE*)slots[11] != ExeBase() + 0x6e6f70 ||
        (BYTE*)slots[12] != ExeBase() + 0x6dcbb0 || (BYTE*)slots[2] != ExeBase() + 0x6ed860) return false;
    void* hooks[2] = { (void*)H_PairWalkMulti, (void*)H_PairWalk };
    return WriteCode(0x81f200 + 10 * 8, hooks, sizeof hooks);
}

// Diagnostic: who wakes a body. Toy Box build 34 showed the stagecoach's sleep
// timer (+0xd8) reset on both PCs about 13 frames apart, and a townsperson
// asleep on one PC and awake on the other. Bullet's activate (FUN_1406cedf0,
// 22 callers) is detoured: a call that actually changes the body (state or
// timer) is logged with its uid and three return addresses, once per run of
// consecutive frames per (body, caller). NetLogPhysFrame then reports timer
// resets and wake-ups that came WITHOUT an activate call (velocity above the
// sleep threshold, or the island manager waking a whole island).
typedef void (*PFN_Activate)(BYTE* obj, char force);
static PFN_Activate g_activateOrig;
static std::unordered_map<uint64_t, uint32_t> g_wakeRuns;   // (uid << 32 | caller rva) -> last frame
static std::unordered_map<uint32_t, uint32_t> g_wakeFrame;  // uid -> last frame an activate changed it
static int g_wakeLogs, g_wakeSession = -1;
// The name (actor +0x148) of the actor that owns collision object o, for log lines; "?" if
// it has none. Also used by the repair's logs (net_lockstep.cpp).
void BodyActorName(BYTE* o, char* nm, size_t cap) {
    strcpy_s(nm, cap, "?");
    __try {
        if (BYTE* rec = *(BYTE**)(o + 0xe8))
            if ((*(BYTE*)(rec + 0x14) & 3) == 1)
                if (BYTE* h = *(BYTE**)(rec + 8))
                    if (BYTE* comp = *(BYTE**)(h + 0x10))
                        if (BYTE* actor = ((BYTE* (*)(uint64_t))(ExeBase() + 0x558400))(*(uint64_t*)(comp + 0x58)))
                            if (const char* s = *(const char**)(actor + 0x148)) strncpy_s(nm, cap, s, _TRUNCATE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// An id for a body that's the same on both PCs: its broadphase proxy's unique id.
static uint32_t BodyUid(BYTE* o) {
    BYTE* proxy = *(BYTE**)(o + 0xb0);
    if (proxy) return *(uint32_t*)(proxy + 0x18);
    BYTE* rec = *(BYTE**)(o + 0xe8);                     // not in the broadphase: 100000 + its pool slot
    return rec ? 100000u + *(uint16_t*)rec : 0;
}
// Bullet's activate, detoured: the activation state (+0xd4) and sleep timer (+0xd8) are
// read before and after the game's call; a change, on the game thread in a session, is
// recorded for LogSilentWakes and logged once per run of consecutive frames.
static void H_Activate(BYTE* obj, char force) {
    uint32_t st0 = 0, dt0 = 0;
    bool watch = g_clockLockstep && obj && GetCurrentThreadId() == g_mainThread;
    if (watch) __try { st0 = *(uint32_t*)(obj + 0xd4); dt0 = *(uint32_t*)(obj + 0xd8); } __except (EXCEPTION_EXECUTE_HANDLER) { watch = false; }
    g_activateOrig(obj, force);
    if (!watch) return;
    __try {
        uint32_t st1 = *(uint32_t*)(obj + 0xd4), dt1 = *(uint32_t*)(obj + 0xd8);
        if (st0 == st1 && dt0 == dt1) return;               // already awake with a zero timer: nothing changed
        if (g_wakeSession != g_netSessions) { g_wakeSession = g_netSessions; g_wakeRuns.clear(); g_wakeFrame.clear(); g_wakeLogs = 0; }
        uint32_t f = g_netFrame, uid = BodyUid(obj);
        g_wakeFrame[uid] = f;
        void* stack[12] = {};
        USHORT got = RtlCaptureStackBackTrace(1, 12, stack, NULL);  // [0] = the game's call site
        uint32_t r[12] = {};
        for (int k = 0; k < got && k < 12; k++) {
            uintptr_t a = (uintptr_t)stack[k];
            r[k] = a >= (uintptr_t)ExeBase() && a < (uintptr_t)ExeBase() + 0x1000000 ? Rva(a) : 0;   // 0 = outside the exe
        }
        auto ins = g_wakeRuns.emplace((uint64_t)uid << 32 | r[0], 0xffffffffu);
        uint32_t& last = ins.first->second;
        bool fresh = last != f && last + 1 != f;
        last = f;
        if (fresh && g_wakeLogs++ < 30000) {
            char nm[48];
            BodyActorName(obj, nm, sizeof nm);
            Log("wake %u: uid %u %s force %d state %u->%u deact %08x->%08x by %x < %x < %x < %x < %x < %x < %x < %x < %x < %x < %x < %x",
                f, uid, nm, force ? 1 : 0, st0, st1, dt0, dt1, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// Detours Bullet's activate (0x1406cedf0) to H_Activate, through a trampoline (as HookAddBody).
static bool HookActivate() {
    static const BYTE expect[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8D, 0x81, 0xD4, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD9 };
    if (!BytesAt(0x6cedf0, expect, sizeof expect)) return false;
    BYTE* fn = ExeBase() + 0x6cedf0;
    BYTE* tr = (BYTE*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return false;
    memcpy(tr, expect, sizeof expect);                   // moved prologue (no RIP-relative operands), then jmp back
    BYTE back[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    void* cont = fn + sizeof expect;
    memcpy(back + 6, &cont, 8);
    memcpy(tr + sizeof expect, back, sizeof back);
    FlushInstructionCache(GetCurrentProcess(), tr, 64);
    g_activateOrig = (PFN_Activate)tr;
    BYTE jmp[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    void* hook = (void*)H_Activate;
    memcpy(jmp + 6, &hook, 8);
    return WriteCode(0x6cedf0, jmp, sizeof jmp);
}
// Once per lockstep frame (NetLogPhysFrame): timer resets and wake-ups with no
// activate call behind them this frame.
void LogSilentWakes(uint32_t f, BYTE** arr, int n) {
    static std::vector<std::pair<uint32_t, uint64_t>> prev;     // uid -> state << 32 | timer bits
    static int sess = -1, logs;
    if (sess != g_netSessions) { sess = g_netSessions; prev.clear(); logs = 0; }
    if (g_wakeSession != g_netSessions) { g_wakeSession = g_netSessions; g_wakeRuns.clear(); g_wakeFrame.clear(); g_wakeLogs = 0; }
    static std::vector<std::pair<uint32_t, uint64_t>> cur;
    cur.clear();
    __try {
        for (int i = 0; i < n; i++) {
            BYTE* o = arr[i];
            if (!o) continue;
            uint32_t uid = BodyUid(o), st = *(uint32_t*)(o + 0xd4), dt = *(uint32_t*)(o + 0xd8);
            cur.push_back({ uid, (uint64_t)st << 32 | dt });
            const std::pair<uint32_t, uint64_t>* p = NULL;
            if (i < (int)prev.size() && prev[i].first == uid) p = &prev[i];
            else for (auto& e : prev) if (e.first == uid) { p = &e; break; }
            if (!p) continue;
            uint32_t st0 = (uint32_t)(p->second >> 32), dt0 = (uint32_t)p->second;
            float t0, t1;
            memcpy(&t0, &dt0, 4); memcpy(&t1, &dt, 4);
            bool reset = t1 < t0, woke = st0 == 2 && st != 2;
            if (!reset && !woke) continue;
            auto wf = g_wakeFrame.find(uid);
            if (wf != g_wakeFrame.end() && (wf->second == f - 1 || wf->second == f)) continue;   // an activate call (logged) did it
            if (logs++ < 30000) {
                char nm[48];
                BodyActorName(o, nm, sizeof nm);
                Log("wake %u: uid %u %s by %s: state %u->%u deact %08x->%08x", f, uid, nm,
                    woke ? "island (no activate call)" : "velocity or direct write (no activate call)", st0, st, dt0, dt);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    prev.swap(cur);
}

// Behaviour range draws in a fixed order. FUN_140641850 fills a behaviour's
// parameter hash by walking the behaviour's variable HashTable (buckets +0x28,
// count +0x38; node: next +0, hash +8, key +0x10, ScriptVariable embedded at
// +0x18 whose +0 is the name). The key is the interned name's POINTER, so the
// bucket order (and with it the order of the "_MinMax" global-MT draws) followed
// each process's string-pool addresses: two PCs handed a townsperson's speed
// and duration draws out swapped. In netplay mode the two advance calls inside
// FUN_140641850 (0x14064188a first, 0x140641cae loop) walk a snapshot sorted by
// name instead. The loop only reads the table and never re-enters, so one
// static snapshot is safe (main thread). (Decomp session's trace.)
typedef bool (*PFN_VarNext)(int64_t* it);
static std::vector<BYTE*> g_varNodes;      // the snapshot, sorted by name
static size_t g_varIdx;                    // next one to hand out
// A table node's variable name ("" if none).
static const char* VarName(BYTE* node) {
    const char* s = *(const char**)(node + 0x18);
    return s ? s : "";
}
static bool VarNameLess(BYTE* a, BYTE* b) { return strcmp(VarName(a), VarName(b)) < 0; }
// The redirected iterator advance (FUN_14044ede0). it[0] is the table, it[1] the current
// node (0 before the first call). Returns false when there are no more nodes.
static bool H_SortedVarNext(int64_t* it) {
    if (g_cfg.mode != MODE_NETPLAY) return ((PFN_VarNext)(ExeBase() + 0x44ede0))(it);
    BYTE* t = (BYTE*)it[0];
    if (!t) return false;
    if (it[1] == 0) {                                    // first call: snapshot the table, sorted by name
        g_varNodes.clear();
        g_varIdx = 0;
        BYTE** buckets = *(BYTE***)(t + 0x28);
        uint32_t count = *(uint32_t*)(t + 0x38);
        for (uint32_t k = 0; buckets && k < count && k < 65536; k++)
            for (BYTE* n = buckets[k]; n && g_varNodes.size() < 4096; n = *(BYTE**)n) g_varNodes.push_back(n);
        std::sort(g_varNodes.begin(), g_varNodes.end(), VarNameLess);
    }
    it[1] = g_varIdx < g_varNodes.size() ? (int64_t)g_varNodes[g_varIdx++] : 0;
    return it[1] != 0;
}
// Every place that switches the Performance Manager's async loading (+0x41) on
// is neutralised in netplay mode, so a load can never complete in the background
// on one PC only:
//  - the level publish FUN_1405d81a0 at 0x1405d840d and LevelLoadComplete in
//    StandAloneLoop::vf24 at 0x1405dd533 ("mov byte [rax+41h],1") store 0;
//  - the setter FUN_1405f0c40 ("mov [rcx+41h],dl; ret") returns at once. The
//    building-lot selector (FUN_1401ba9b0) and choreography resource release
//    (FUN_1406c2b80) call it with 1, which left a host async through a drop-in
//    reload: its town buildings loaded 2 frames before the friend's (runs 81);
//  - the constructor (0x14059a114, "mov word [rcx+40h],0101h") starts it at 0.
// NetFrameBody's per-frame reset stays as a backstop. Returns the patches made (of 4).
static int PatchPmAsyncOff() {
    static const BYTE movOne[] = { 0xC6, 0x40, 0x41, 0x01 };
    static const BYTE setter[] = { 0x88, 0x51, 0x41, 0xC3 };
    static const BYTE ctor[] = { 0x66, 0xC7, 0x41, 0x40, 0x01, 0x01 };
    static const BYTE zero = 0, ret = 0xC3;
    int n = 0;
    for (DWORD at : { 0x5d840du, 0x5dd533u })
        if (BytesAt(at, movOne, sizeof movOne) && WriteCode(at + 3, &zero, 1)) n++;
    if (BytesAt(0x5f0c40, setter, sizeof setter) && WriteCode(0x5f0c40, &ret, 1)) n++;
    if (BytesAt(0x59a114, ctor, sizeof ctor) && WriteCode(0x59a119, &zero, 1)) n++;
    PmAsyncClear();                                     // in case the manager was already built
    return n;
}

// Diagnostic: behaviour starts and their random parameters (decomp session's
// trace). A townsperson's walk speed is a global-MT draw taken when a
// behaviour starts (FUN_140663010 -> FUN_140641850 -> FUN_140446eb0 per
// "_MinMax" variable), so two actors starting in a different order on each PC
// get the same draws swapped. Logged in a session: every behaviour start
// (frame, actor key, method), every MinMax draw (range, result), a per-frame
// hash of the order actors are updated in (FUN_140552220), and once per level
// the order of the entity world's activation lists.
typedef uint64_t (*PFN_BehavStart)(BYTE* ctrl, const char* method, int a3, int* a4);
typedef float (*PFN_MinMax)(BYTE* var);
typedef void (*PFN_ActorUpdate)(BYTE* world, BYTE* actor);
static LONG g_behavLogs;                   // behav + minmax lines so far (40000 at most)
// The actor update order, per lockstep frame: which frame, how many updates, and an FNV-1a
// hash of the actor keys in order (written to the log by net_lockstep.cpp).
uint32_t g_updFrame = 0xFFFFFFFF, g_updCount;
uint64_t g_updHash;
// The redirected behaviour start: logs "behav frame: actor key starts method", then starts it.
static uint64_t H_BehavStart(BYTE* ctrl, const char* method, int a3, int* a4) {
    if (g_clockLockstep && InterlockedIncrement(&g_behavLogs) <= 40000) {
        uint64_t key = 0;
        char m[48] = "?";
        __try {
            if (BYTE* actor = *(BYTE**)(ctrl + 8)) key = *(uint64_t*)(actor + 0x38);
            if (method) strncpy_s(m, method, _TRUNCATE);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Log("behav %u: actor %llu starts %s", g_netFrame, (unsigned long long)key, m);
    }
    return ((PFN_BehavStart)(ExeBase() + 0x663010))(ctrl, method, a3, a4);
}
// The redirected MinMax draw: the game's draw, then a log line with the range and the
// result (and its bits, so two logs compare exactly).
static float H_MinMax(BYTE* var) {
    float r = ((PFN_MinMax)(ExeBase() + 0x446eb0))(var);
    if (g_clockLockstep && InterlockedIncrement(&g_behavLogs) <= 40000) {
        float lo = 0, hi = 0;
        __try { if (const float* d = *(const float**)(var + 8)) { lo = d[0]; hi = d[1]; } } __except (EXCEPTION_EXECUTE_HANDLER) {}
        uint32_t bits;
        memcpy(&bits, &r, 4);
        Log("minmax %u: [%.3f, %.3f] -> %.4f (%08x)", g_netFrame, lo, hi, r, bits);
    }
    return r;
}
// The redirected per-actor update: folds the actor's key into this frame's order hash.
static void H_ActorUpdate(BYTE* world, BYTE* actor) {
    if (g_clockLockstep) {
        if (g_updFrame != g_netFrame) { g_updFrame = g_netFrame; g_updCount = 0; g_updHash = 1469598103934665603ULL; }
        uint64_t key = 0;
        __try { key = *(uint64_t*)(actor + 0x38); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        for (int k = 0; k < 8; k++) { g_updHash ^= (key >> (k * 8)) & 0xff; g_updHash *= 1099511628211ULL; }
        g_updCount++;
    }
    ((PFN_ActorUpdate)(ExeBase() + 0x552220))(world, actor);
}
// One "actlist" line for each of four actor lists in the entity world (*0x140b2bb70, lists
// at +0x110/+0x158/+0x188/+0x1a8: first node at +8, next +0x30, actor +8): count, hash and
// the keys in order. Called from net_lockstep.cpp's per-frame physics log when a level's
// physics comes up, and again 60 and 600 frames later.
void LogActivationLists(uint32_t f) {
    BYTE* w = NULL;
    __try { w = *(BYTE**)(ExeBase() + 0xb2bb70); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!w) return;
    static const int offs[] = { 0x158, 0x188, 0x110, 0x1a8 };
    for (int off : offs) {
        char line[1800] = "";
        int n = 0;
        uint64_t h = 1469598103934665603ULL;
        __try {
            for (BYTE* node = *(BYTE**)(w + off + 8); node && n < 5000; node = *(BYTE**)(node + 0x30)) {
                BYTE* actor = *(BYTE**)(node + 8);
                uint64_t key = actor ? *(uint64_t*)(actor + 0x38) : 0;
                for (int k = 0; k < 8; k++) { h ^= (key >> (k * 8)) & 0xff; h *= 1099511628211ULL; }
                size_t len = strlen(line);
                if (len < sizeof line - 24) _snprintf_s(line + len, sizeof line - len, _TRUNCATE, " %llu", (unsigned long long)key);
                n++;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Log("actlist %u: W+0x%x n %d hash %08x:%s", f, off, n, (uint32_t)h, line);
    }
}

// Diagnostic: the physics collision callback (CollisionDetectAcceptCallback::vf1,
// 0x14053d270) draws the MT coin flip per accepted hit and then sends
// "ActorsCollided" to both actors (EventMgr_SendToActor at 0x14053d704 and
// 0x14053d727: 4th arg the other actor's key, 5th the target). Logging the two
// keys per frame on both PCs names the pair behind an extra draw (the manifold
// contact history isn't in any hash).
typedef uint64_t (*PFN_SendToActor)(void* mgr, const char* name, uint64_t a3, uint64_t other, uint64_t target);
static LONG g_collidedLogs;
// The two redirected sends: one "collided" line (target, other actor), then the send.
static uint64_t H_SendCollided(void* mgr, const char* name, uint64_t a3, uint64_t other, uint64_t target) {
    if (g_clockLockstep && InterlockedIncrement(&g_collidedLogs) <= 60000)
        Log("collided %u: %llu with %llu", g_netFrame, (unsigned long long)target, (unsigned long long)other);
    return ((PFN_SendToActor)(ExeBase() + 0x49e0b0))(mgr, name, a3, other, target);
}

// Part of NetRejoin (net_dropin.cpp), when an ended session goes back to "connecting" for a
// new one without restarting the game:
// clears the per-session state of this file's voice waits and of the drop-in, load gate,
// own-screen HUD and movie-sync code, so nothing carries over from the last session.
void NetRejoinResetLate() {
    memset(g_voice, 0, sizeof g_voice);
    memset(g_sndWait, 0, sizeof g_sndWait);
    g_vpOwnLogged = false;
    g_dropPublished = false;
    g_dropPublishedFrame = g_dropHintAt = 0;
    g_loadGate = g_loadGateReady = g_loadStepDone = false;
    g_netHashHoldUntil = 0;
    g_hudHiddenN = 0;
    memset(g_promptEl, 0, sizeof g_promptEl);
    g_movieIdx = 0;
    g_movieAtFrame = 0xFFFFFFFF;
    g_moviePeerSkipIdx = 0;
    g_moviePeerIdx = 0;
    g_movieSkipLocal = false;
}

// --- session-only simulation fixes ---------------------------------------------
//
// Zeroed heap blocks, music-synced choreography, visibility tree rebuilds, bone -> body
// pushes, window shape, script screen z and the contact coin flip. InstallDeterminism
// verifies and installs them; NetMusicSyncHold turns the code patches on for a session.

// Uninitialised memory, as a class (decomp session's sweep, E). The game's heaps don't
// clear what they hand out, and constructors that leave a field unset (the RigidBody
// wrapper, build 57) then compute from whatever this process's heap held there before:
// different on every PC. In a session every block from the game's one heap allocator,
// FUN_140242630 (heap, size, align, ?, kind; behind Mem_Alloc, Mem_Alloc2 and
// Mem_AllocFromHeapAligned), is zero-filled before it's returned, so a forgotten field
// reads 0 on both PCs. Costs a memset per allocation.
volatile bool g_zeroAllocs;                // session: heap blocks are zero-filled (H_HeapAlloc)
typedef void* (*PFN_HeapAlloc)(void* heap, uint64_t size, uint64_t align, uint64_t a4, int kind);
static PFN_HeapAlloc g_heapAllocOrig;
// The detoured allocator: the game's allocation, zero-filled while g_zeroAllocs is set
// (NetFrameBody sets it each session frame; NetEnd clears it).
static void* H_HeapAlloc(void* heap, uint64_t size, uint64_t align, uint64_t a4, int kind) {
    void* p = g_heapAllocOrig(heap, size, align, a4, kind);
    if (p && size && g_zeroAllocs) memset(p, 0, (size_t)size);
    return p;
}
// Detours FUN_140242630 to H_HeapAlloc through a trampoline (as HookAddBody); in netplay
// mode for the whole run, doing nothing extra outside a session.
static bool PatchHeapAlloc() {
    // mov r11,rsp; mov [r11+20h],rbx; mov [r11+18h],r8; push rbp/rsi/rdi/r12/r13 (18 bytes, no RIP operands)
    static const BYTE expect[] = { 0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x20, 0x4D, 0x89, 0x43, 0x18,
                                   0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55 };
    if (!BytesAt(0x242630, expect, sizeof expect)) return false;
    BYTE* fn = ExeBase() + 0x242630;
    BYTE* tr = (BYTE*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return false;
    memcpy(tr, expect, sizeof expect);
    BYTE back[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    void* cont = fn + sizeof expect;
    memcpy(back + 6, &cont, 8);
    memcpy(tr + sizeof expect, back, sizeof back);
    FlushInstructionCache(GetCurrentProcess(), tr, 64);
    g_heapAllocOrig = (PFN_HeapAlloc)tr;
    BYTE jmp[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    void* hook = (void*)H_HeapAlloc;
    memcpy(jmp + 6, &hook, 8);
    return WriteCode(0x242630, jmp, sizeof jmp);
}
bool g_zeroAllocOk;

// Music-synced choreography (decomp session's class sweep, C1). A choreography with audio
// starts its clip paused and waits (+0x161) until its music segment is really playing:
// FUN_140275fa0 posts "InteractiveMusicSegmentStart" the first time FMOD's
// getPlaybackState says PLAYING, and ChoreographyNode::Message::vf3 (its only subscriber)
// then un-pauses the clip. That's real time, so the clip started on a different step on
// each PC. In a session that post is dropped, and the node's own give-up (3 s since the
// play, read through the pinned frame clock: ChoreographyNode::vf0's cmp eax, 3000 at
// 0x1406c29ab) is cut to 40 ms, so every such clip starts 3 steps after it's played on both
// PCs. Only the sync between the music and the animation changes.
bool g_musicSyncOk;
// FUN_140275fa0's redirected post (the event poster at 0x14049e160): dropped in a session.
static uint64_t H_MusicSegPost(void* mgr, const char* name, uint64_t arg, uint64_t z) {
    if (g_clockLockstep && g_netPhase == NP_RUNNING) return 0;
    return ((uint64_t (*)(void*, const char*, uint64_t, uint64_t))(ExeBase() + 0x49e160))(mgr, name, arg, z);
}
// Visibility tree rebuilds (class sweep, B; decomp session). FUN_1402e6c10 inserts moving
// objects into the visibility BVH; when a leaf's pending list needs splitting it rebuilds
// it on the main thread if DAT_140b22eb3 is 0, else marks the node (+0xb0 = 1) for the
// VisibilityThread, which polls every 16 ms of real time (FUN_1402e8b30). The load sets the
// flag to 0, LevelPublishDone (store at 0x1405d835a) to 1, so in play the tree changed shape
// on a real-time frame per PC, and with it the cull order and the "became visible" events
// animation reads (player 2's roll camera and Jessie drifted that way). In a session it's
// held at 0, each lockstep step and right after a publish: rebuilds happen synchronously,
// as during a load.
bool g_visSyncOk;
// Puts DAT_140b22eb3 back to 0 if anything set it. Called every lockstep frame
// (NetFrameBody) and right after a level publish (net_own_screen.cpp's publish hook).
void NetVisSync() {
    if (!g_visSyncOk) return;
    volatile BYTE* flag = ExeBase() + 0xb22eb3;
    if (*flag) {
        static int logged;
        if (logged++ < 3) Log("determinism: visibility tree rebuilds moved back onto the main thread (session)");
        *flag = 0;
    }
}

// Bone -> body pushes (decomp session). An animated actor's collision is a compound shape,
// one child per bone, and a bone's physics part follows its animated scene node only when
// that node is refreshed: lazily, by the first main-thread getter that asks where it is
// (FUN_1402b0870: node+0xac & 0x100 -> node->vf(0x70) -> FUN_1402ce090 -> FUN_14041f950).
// So whether a bone's new pose reached its body before a physics step depended on who
// asked first, and code that only runs on one PC (own-screen HUD/camera/listener paths)
// could ask on one PC only: in Andy's room a switched-out avatar's body (Woody, after an
// LB swap) moved a step apart on the two PCs. In a session, right before every physics
// step (the call to FUN_1405324f0 in the world update, 0x14055155e), every dirty node of
// every live actor is refreshed, in registry order (bucket 0..255, chain order; keys are
// the same on both PCs), each actor's tree pre-order (first child +0x28, next sibling
// +0x30), after reading the actor's position (its own transform cache, which audio refreshes
// per PC). Every pending push then lands at the same point on both PCs.
bool g_boneFlushOk;
uint64_t g_boneFlushNodes;                 // nodes refreshed so far (session summary)
// The flush described above. FUN_140558400 (live) answers a key with its actor only while
// that actor is live; registry entries whose object isn't the live actor are skipped. An
// actor's scene tree is reached through actor +0x380 -> +0x18 (its root node).
static void NetFlushDirtyNodes() {
    BYTE* b = ExeBase();
    typedef BYTE* (*PFN_Live)(uint64_t);
    typedef void* (*PFN_Refresh)(BYTE*);
    PFN_Live live = (PFN_Live)(b + 0x558400);
    PFN_Refresh refresh = (PFN_Refresh)(b + 0x2b0870);
    typedef float* (*PFN_GetPos)(BYTE*, float*);
    PFN_GetPos getPos = (PFN_GetPos)(b + 0x5561c0);                    // Actor_GetPosition
    BYTE** buckets = (BYTE**)(b + 0xb2bb90);
    uint32_t refreshed = 0;
    __try {
        for (int k = 0; k < 256; k++)
            for (BYTE* e = buckets[k]; e; e = *(BYTE**)(e + 0x18)) {
                BYTE* actor = *(BYTE**)e;
                if (!actor || live(*(uint64_t*)(e + 8)) != actor) continue;
                // The actor-level transform cache too (actor+0x200: stamp vs parent stamp, owner
                // vf 0x78), which audio refreshes per PC (3D events read their actors' positions
                // while they're alive in real time): read it here like any reader would.
                float pos[3];
                getPos(actor, pos);
                BYTE* blk = *(BYTE**)(actor + 0x380);
                BYTE* root = blk ? *(BYTE**)(blk + 0x18) : NULL;
                if (!root) continue;
                BYTE* stack[512];
                int sp = 0, guard = 0;
                stack[sp++] = root;
                while (sp && guard++ < 20000) {
                    BYTE* n = stack[--sp];
                    if (*(uint32_t*)(n + 0xac) & 0x100) { refresh(n); refreshed++; }
                    BYTE* kids[64];
                    int nk = 0;
                    for (BYTE* c = *(BYTE**)(n + 0x28); c && nk < (int)_countof(kids); c = *(BYTE**)(c + 0x30)) kids[nk++] = c;
                    while (nk && sp < (int)_countof(stack)) stack[sp++] = kids[--nk];   // reversed: popped in sibling order
                }
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_boneFlushNodes += refreshed;
}
// The world update's redirected call to the physics step (FUN_1405324f0): flush first, on
// the game thread in a session, then step.
static void H_PhysStep(BYTE* world) {
    if (g_boneFlushOk && g_clockLockstep && g_netPhase == NP_RUNNING && GetCurrentThreadId() == g_mainThread)
        NetFlushDirtyNodes();
    ((void (*)(BYTE*))(ExeBase() + 0x5324f0))(world);
}

// Window shape (class sweep, D): gameplay's only reader of the window size is the alley
// toss aim, FUN_140075b80, which scales the reticle's screen offset by 1.0 when the
// client area is narrower than 16:9 and 1.33 otherwise, so a 16:10 or 4:3 window aimed
// differently. In a session its "jb" (0x140075cac, after comiss w/h, 16/9) is two NOPs:
// every PC takes the 16:9 path.
bool g_aspectOk;
// Script_GetScreenToWorld (0x1405e9a00; pixar_ball.sx uses it) pops only x and y, then
// reads z from [rsp+68h], its unused second parameter's home slot, which nothing writes:
// whatever the script VM's previous call left there, which differs per process. The Pixar
// ball in Andy's room was the first body to split online. In a session that load
// (movss xmm3,[rsp+68h] at 0x1405e9a34) becomes xorps xmm3,xmm3: z = 0 on both PCs.
bool g_screenZOk;
// The contact coin flip (decomp session, Toy Box split). processOverlap draws one engine
// random number per touching actor pair per frame (0x14053d678: call Rng_U32; test al,1;
// jne) only to pick which actor is A and which is B for ActorsCollided. One contact on one
// PC (two followers posed slightly differently) then shifted every later random draw. In a
// session the call is xor eax,eax + a 3-byte nop: always the even branch, A/B follow the
// pair's own (deterministic) order, and no draw is made. Damage limitation, not the root.
bool g_coinFlipOk;
// Writes (on) or undoes (off) the four code patches above that are verified at start-up:
// the choreography give-up (3000 -> 40 ms), the toss aim's jb, the screen-to-world z load
// and the coin flip's call. On every session frame (NetFrameBody) and off at NetEnd; it
// only writes when the state changes. A failed write stops there and is retried next call.
void NetMusicSyncHold(bool on) {
    static bool held;
    if (on == held) return;
    if (g_musicSyncOk) {
        uint32_t ms = on ? 40 : 3000;
        if (!WriteMem(0x6c29ac, &ms, sizeof ms, true)) return;
    }
    if (g_aspectOk) {
        static const BYTE nops[] = { 0x90, 0x90 }, jb[] = { 0x72, 0x0A };
        if (!WriteMem(0x75cac, on ? nops : jb, 2, true)) return;
    }
    if (g_screenZOk) {
        static const BYTE zero[] = { 0x0F, 0x57, 0xDB, 0x90, 0x90, 0x90 }, load[] = { 0xF3, 0x0F, 0x10, 0x5C, 0x24, 0x68 };
        if (!WriteMem(0x5e9a34, on ? zero : load, 6, true)) return;
    }
    if (g_coinFlipOk) {
        static const BYTE even[] = { 0x31, 0xC0, 0x0F, 0x1F, 0x00 }, flip[] = { 0xE8, 0x53, 0x08, 0xD2, 0xFF };   // xor eax,eax; nop3 / call Rng_U32
        if (!WriteMem(0x53d678, on ? even : flip, 5, true)) return;
    }
    held = on;
    if (g_musicSyncOk)
        Log(on ? "determinism: choreographies waiting for their music start 3 frames after they're played (session)"
               : "determinism: choreographies wait for their music again");
}

// The game's transcendental maths imports (sinf ... log10) come from each PC's
// own ucrtbase.dll, a Windows component whose version nothing compares, and
// those functions aren't guaranteed to round identically across versions. In
// netplay mode they're pointed at this DLL's statically linked copies (built
// /MT), with the FMA3 path off there too, so both PCs run the same code.
// sqrt/floor/ceil/fmod are exact under IEEE and stay as they are.
#pragma function(sinf, cosf, tanf, asinf, acosf, atanf, atan2f, expf, powf, pow, log10)
extern "C" int __cdecl _set_FMA3_enable(int);
// Points each of the 11 imports at this DLL's copy (the casts pick the float or double
// overload). Returns how many import entries were patched.
static int InstallStaticMaths() {
    _set_FMA3_enable(0);                                 // this module's static CRT
    HMODULE exe = GetModuleHandleW(NULL);
    struct { const char* name; void* fn; } maths[] = {
        { "sinf", (void*)(float (*)(float))sinf }, { "cosf", (void*)(float (*)(float))cosf },
        { "tanf", (void*)(float (*)(float))tanf }, { "asinf", (void*)(float (*)(float))asinf },
        { "acosf", (void*)(float (*)(float))acosf }, { "atanf", (void*)(float (*)(float))atanf },
        { "atan2f", (void*)(float (*)(float, float))atan2f }, { "expf", (void*)(float (*)(float))expf },
        { "powf", (void*)(float (*)(float, float))powf }, { "pow", (void*)(double (*)(double, double))pow },
        { "log10", (void*)(double (*)(double))log10 },
    };
    int n = 0;
    for (auto& m : maths) n += PatchIAT(exe, m.name, m.fn);
    return n;
}

// Called once from DllMain (xinput_proxy.cpp), on the game's initial thread, in record,
// replay and netplay modes, after the engine clock and rand() isolation are in. Verifies and
// installs everything in this file and logs one line per fix saying whether it applied.
// Most patches are installed for the whole run but act only in a session (they test
// g_clockLockstep or g_netPhase); the code patches that must be off in solo play are
// switched by NetMusicSyncHold and SetSyncLoader instead. Nearly all of it needs
// sync_loader=1 (it sits inside that block) and netplay mode.
void InstallDeterminism() {
    // Maths: same CRT code on both PCs, and the reference floating-point mode.
    if (g_cfg.mode == MODE_NETPLAY)
        Log("determinism: %d/11 maths imports use this DLL's own copies (same code on every PC, whatever its Windows version)",
            InstallStaticMaths());
    // The reference floating-point mode: taken here, at start-up on the game
    // thread, before any movie or library could change it; both PCs run the
    // same exe, so they agree. Restored every lockstep frame (NetFrameBody).
    g_fpCsrRef = _mm_getcsr();
    Log("determinism: floating-point mode (MXCSR) at start %08x; restored every frame in a session", g_fpCsrRef);
    if (HMODULE u = GetModuleHandleW(L"ucrtbase.dll")) {
        typedef int (*PFN_SetFma3)(int);
        if (PFN_SetFma3 f = (PFN_SetFma3)GetProcAddress(u, "_set_FMA3_enable")) { f(0); g_fma3Off = true; }
    }
    Log("determinism: CRT maths %s", g_fma3Off ? "forced to the SSE2 path on every CPU (FMA3 off)" : "NOT changed (ucrtbase lacks _set_FMA3_enable)");
    if (g_cfg.syncLoader) {
        g_syncLoaderOn = VerifySyncLoader();
        if (g_syncLoaderOn && g_cfg.mode == MODE_NETPLAY) {
            // Loading and pacing: the data pump, the loader slice, the frame limiter, and
            // the movie and fade timers.
            static const BYTE leaMgr[] = { 0x48, 0x8D, 0x0D };   // lea rcx, [DAT_140957280]
            bool pumpOk = BytesAt(0x280bc9, leaMgr, 3) && RipRefersTo(0x280bc9, 3, 7, 0x957280) &&
                          RedirectCall(0x280bd0, 0x511870, (void*)H_DataPump);
            Log("determinism: data-file pump %s", pumpOk ? "drains at a fixed point while a session runs"
                                                         : "NOT hooked (code doesn't match this build)");
            bool sliceOk = RedirectCall(0x280c0c, 0x25b610, (void*)H_LoaderSlice);
            Log("determinism: loader slice %s", sliceOk ? "repeats until the loader is idle while a session runs"
                                                        : "NOT hooked (code doesn't match this build)");
            Log("determinism: frame limiter %s", HookFrameLimit() ? "skipped while a loading screen is up in a session (fast loads)"
                                                                  : "NOT hooked (code doesn't match this build)");
            if (RipRefersTo(0x479169, 1, 5, 0x25a5e0)) g_fmvTimerRet = (uintptr_t)(ExeBase() + 0x47916e);
            Log("determinism: post-movie timer reset %s", g_fmvTimerRet ? "uses the frame's start time in a session"
                                                                       : "NOT handled (code doesn't match this build)");
            if (RipRefersTo(0x473414, 1, 5, 0x25a5e0)) g_fadeClockRet = (uintptr_t)(ExeBase() + 0x473419);
            Log("determinism: screen fades %s", g_fadeClockRet ? "are timed from the frame's start time in a session"
                                                               : "NOT handled (code doesn't match this build)");
            g_fadeOnMain = g_fadeClockRet && RedirectCall(0x1ef399, 0x473320, (void*)H_FadeStepRender);
            // The session-only simulation fixes (see that section): each block verifies its
            // code and sets its *Ok flag; NetMusicSyncHold writes the in-place ones later.
            {
                static const BYTE giveUp[] = { 0x2B, 0x87, 0x24, 0x01, 0x00, 0x00, 0x3D, 0xB8, 0x0B, 0x00, 0x00, 0x72, 0x10 };   // sub eax,[rdi+124h]; cmp eax,3000; jb
                static const BYTE postArgs[] = { 0x45, 0x33, 0xC9 };                                                          // xor r9d,r9d before the post
                g_musicSyncOk = BytesAt(0x6c29a5, giveUp, sizeof giveUp) && RipRefersTo(0x6c29a0, 1, 5, 0x25a5e0) &&
                                BytesAt(0x276013, postArgs, sizeof postArgs) && RipRefersTo(0x276005, 3, 7, 0x7bb978) &&
                                RedirectCall(0x276016, 0x49e160, (void*)H_MusicSegPost);
                Log("determinism: music-synced choreography %s", g_musicSyncOk ? "starts a fixed 3 frames after it's played in a session"
                                                                                : "NOT handled (code doesn't match this build)");
            }
            {
                static const BYTE aspect[] = { 0x0F, 0x2F, 0x0D, 0x00, 0x12, 0x70, 0x00, 0x72, 0x0A, 0xF3, 0x0F, 0x10, 0x15 };   // comiss xmm1,[16/9]; jb; movss xmm2,[1.33]
                g_aspectOk = BytesAt(0x75ca5, aspect, sizeof aspect);
                Log("determinism: alley toss aim %s", g_aspectOk ? "uses the 16:9 scale whatever the window shape, in a session"
                                                                 : "NOT handled (code doesn't match this build)");
            }
            {
                static const BYTE setAsync[] = { 0xC6, 0x05 };                     // mov byte [DAT_140b22eb3], 1
                g_visSyncOk = BytesAt(0x5d835a, setAsync, sizeof setAsync) && RipRefersTo(0x5d835a, 2, 7, 0xb22eb3) &&
                              *(ExeBase() + 0x5d835a + 6) == 1 && RipRefersTo(0x2e6f27, 2, 7, 0xb22eb3);
                Log("determinism: visibility tree rebuilds %s", g_visSyncOk ? "run on the main thread in a session (not the VisibilityThread)"
                                                                           : "NOT handled (code doesn't match this build)");
            }
            {
                static const BYTE zLoad[] = { 0xF3, 0x0F, 0x10, 0x5C, 0x24, 0x68, 0x48, 0x8D, 0x54, 0x24, 0x20 };   // movss xmm3,[rsp+68h]; lea rdx,[rsp+20h]
                static const BYTE frame[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x50 };                               // push rbx; sub rsp,50h
                g_screenZOk = BytesAt(0x5e9a00, frame, sizeof frame) && BytesAt(0x5e9a34, zLoad, sizeof zLoad);
                Log("determinism: script screen-to-world %s", g_screenZOk ? "uses z = 0 in a session (the game read an unset stack slot)"
                                                                         : "NOT handled (code doesn't match this build)");
            }
            {
                static const BYTE leaWorld[] = { 0x48, 0x81, 0xC1, 0x60, 0x11, 0x00, 0x00 };   // add rcx,1160h (the physics world)
                static const BYTE refresh[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9 };
                static const BYTE getPos[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x99, 0x00, 0x02, 0x00, 0x00 };   // ...; mov rbx,[rcx+200h]
                g_boneFlushOk = BytesAt(0x551557, leaWorld, sizeof leaWorld) && BytesAt(0x2b0870, refresh, sizeof refresh) &&
                                BytesAt(0x5561c0, getPos, sizeof getPos) &&
                                RipRefersTo(0x558403, 3, 7, 0xb2bb90) && RedirectCall(0x55155e, 0x5324f0, (void*)H_PhysStep);
                Log("determinism: animated bones %s", g_boneFlushOk ? "push their pose into physics at a fixed point before each step, in a session"
                                                                   : "NOT flushed (code doesn't match this build)");
            }
            {
                // Own screen (net_own_screen.cpp's hooks, installed here): the screen-region
                // helpers and the private-camera draws.
                static const DWORD kVertCalls[] = { 0x1c85aa, 0x1d0c20, 0x1e7c1c, 0x1e8610, 0x1e8710 };
                static const DWORD kHorzCalls[] = { 0x1c85c8, 0x1d0c52, 0x1e7c4e, 0x1e8645, 0x1e8742 };
                int n = 0;
                for (DWORD c : kVertCalls) n += RedirectCall(c, 0x5ceca0, (void*)H_SplitIsVertical);
                for (DWORD c : kHorzCalls) n += RedirectCall(c, 0x5cdf40, (void*)H_SplitIsHorizontal);
                g_regionFixOk = n == 10;
                Log("determinism: %d/10 screen-region helper calls use the full screen while own screen is on", n);
                // The camera field of view (re\SPLITSCREEN_FOV.md). The normal player camera
                // (its FOV callback FUN_14021fd70) and CTetherWTVC::Update (0x14021a5b0) pick 65
                // degrees for one view, 52 for a top/bottom split and 87.75 for side by side, from
                // the same two split tests. Own screen gives each player a full-screen view, so
                // those four calls answer "not split" too: 65 degrees, as in single player. The
                // FOV also shapes the cull frustum, which feeds the simulation, so like the calls
                // above this follows NetOwnScreenActive(), the same on both PCs on every frame.
                int fov = 0;
                fov += RedirectCall(0x21fd74, 0x5ceca0, (void*)H_SplitIsVertical);     // player camera: side by side?
                fov += RedirectCall(0x21fd8a, 0x5cdf40, (void*)H_SplitIsHorizontal);   // player camera: top/bottom?
                fov += RedirectCall(0x21a6ee, 0x5ceca0, (void*)H_SplitIsVertical);     // tether camera: side by side?
                fov += RedirectCall(0x21a701, 0x5cdf40, (void*)H_SplitIsHorizontal);   // tether camera: top/bottom?
                g_camFovFixOk = fov == 4;
                Log("determinism: %d/4 camera field-of-view calls use the single-player 65 degrees while own screen is on", fov);
                int draws = 0;
                if (RipRefersTo(0x3416a, 3, 7, 0xb2ef10))
                    for (DWORD c : { 0x1225caUL, 0x123fc7UL, 0x205b22UL }) draws += RedirectCall(c, 0x1f7d10, (void*)H_PreviewDraw);
                g_previewFixOk = draws > 0;
                Log("netplay: own screen: %d/3 private-camera draws (customization preview, wanted poster, compass arrow) "
                    "only on the owning player's PC", draws);
            }
            {
                static const BYTE flip[] = { 0xE8, 0x5B, 0xF0, 0xD1, 0xFF, 0x48, 0x8B, 0xC8, 0xE8, 0x53, 0x08, 0xD2, 0xFF, 0xA8, 0x01, 0x75, 0x15 };
                g_coinFlipOk = BytesAt(0x53d670, flip, sizeof flip) && RipRefersTo(0x53d670, 1, 5, 0x25c6d0) && RipRefersTo(0x53d678, 1, 5, 0x25ded0);
                Log("determinism: contact coin flip %s", g_coinFlipOk ? "replaced by a fixed A/B order in a session (no engine RNG draw)"
                                                                     : "NOT handled (code doesn't match this build)");
            }
            g_zeroAllocOk = PatchHeapAlloc();
            Log("determinism: heap blocks %s", g_zeroAllocOk ? "are zero-filled in a session (no leftover memory in unset fields)"
                                                             : "NOT zero-filled (code doesn't match this build)");
            Log("determinism: screen fades %s", g_fadeOnMain ? "step on the main thread once per lockstep frame in a session"
                                                             : "still step on the render thread (code doesn't match this build)");
            // Gameplay timers that read the engine clock (FUN_14025a5e0) mid-frame: their return
            // addresses go on sim_determinism.cpp's pinned list, which answers them with the
            // frame's start time in a session.
            static const DWORD kPinnedClockCalls[] = { 0x695f2a, 0x6c29a0, 0x27e068, 0x5fa767, 0x5fa7cf, 0x5fa823, 0x5fa880, 0x5fa8e0, 0x2070ac,
                                                       0x55eb24, 0x55eb3d };   // the last two: the streamer's parse slice (see EngineNow)
            for (DWORD c : kPinnedClockCalls)
                if (RipRefersTo(c, 1, 5, 0x25a5e0) && g_pinnedClockN < _countof(g_pinnedClockRets))
                    g_pinnedClockRets[g_pinnedClockN++] = (uintptr_t)(ExeBase() + c + 5);
            Log("determinism: %d/%d gameplay timers (choreography give-up, Buzz direction latch, per-view time, camera minigame) "
                "and the streamer's parse slice read the frame's start time in a session", g_pinnedClockN, (int)_countof(kPinnedClockCalls));
            // Physics, entity keys and the load gate.
            Log("determinism: broadphase proxies and AABB-tree nodes %s", InstallProxyArena()
                ? "come from an arena (address order = creation order on both PCs)" : "NOT handled (code doesn't match this build)");
            Log("determinism: origin orphan bodies %s", HookAddBody()
                ? "are added as static (they can no longer tumble or fall)" : "NOT handled (code doesn't match this build)");
            g_rbZeroOn = PatchRigidBodyAlloc();
            Log("determinism: entity-key log %s", HookKeyMakers() ? "on (one \"newkeys\" line per session frame: key<caller:vtable>)"
                                                                 : "NOT available (code doesn't match this build)");
            Log("determinism: load gate holds both PCs paused until both are loaded: %s",
                HookUnpause() ? "yes" : "NO (unpause code doesn't match; only the load-complete step is held)");
            Log("determinism: load gate freezes the world until both PCs have streamed the level: %s",
                HookWorldFreeze() ? "yes" : "NO (world update code doesn't match)");
            Log("determinism: physics body wrappers %s", g_rbZeroOn ? "start zero-filled (no heap leftovers in fields the game skips)"
                                                                    : "NOT zero-filled (allocation calls don't match this build)");
            // Behaviours, the Performance Manager, collision callbacks, and the diagnostics.
            int bh = RedirectCall(0x66f159, 0x663010, (void*)H_BehavStart) + RedirectCall(0x66f18b, 0x663010, (void*)H_BehavStart) +
                     RedirectCall(0x66f1e4, 0x663010, (void*)H_BehavStart);
            int mm = RedirectCall(0x60ff20, 0x446eb0, (void*)H_MinMax) + RedirectCall(0x60ffbe, 0x446eb0, (void*)H_MinMax) +
                     RedirectCall(0x64191e, 0x446eb0, (void*)H_MinMax);
            int up = RedirectCall(0x5514b3, 0x552220, (void*)H_ActorUpdate) + RedirectCall(0x5515d1, 0x552220, (void*)H_ActorUpdate);
            Log("diagnostic: behaviour starts %d/3, MinMax draws %d/3, actor update order %d/2 call sites logged in a session", bh, mm, up);
            int vs = RedirectCall(0x64188a, 0x44ede0, (void*)H_SortedVarNext) + RedirectCall(0x641cae, 0x44ede0, (void*)H_SortedVarNext);
            Log("determinism: behaviour parameter draws %s (%d/2 call sites)", vs == 2 ? "follow the parameters' names, not memory order"
                                                                                  : "NOT fully handled (code doesn't match this build)", vs);
            Log("determinism: Performance Manager async loading never switched on in netplay mode "
                "(%d/4 patched: 2 stores, the setter, the constructor)", PatchPmAsyncOff());
            Log("determinism: game collision callbacks %s", HookPairWalks() ? "walk the touching pairs in a fixed order (by proxy id)"
                                                                            : "NOT handled (code doesn't match this build)");
            Log("diagnostic: body wake-ups %s", HookActivate() ? "logged with their callers in a session" : "NOT logged (code doesn't match this build)");
            int col = RedirectCall(0x53d704, 0x49e0b0, (void*)H_SendCollided) + RedirectCall(0x53d727, 0x49e0b0, (void*)H_SendCollided);
            Log("diagnostic: physics \"ActorsCollided\" pairs logged in a session: %d/2 call sites", col);
            bool sr0 = HookScriptRand(0, 0x49d4a0, (void*)H_ScriptRandReal), sr1 = HookScriptRand(1, 0x49d500, (void*)H_ScriptRandD500);
            Log("determinism: the player's blink timer %s", HookScriptRand(2, 0x49d440, (void*)H_ScriptRandInt) && sr0
                ? "draws from its own generator (cosmetic, kept off the shared one)" : "NOT handled (code doesn't match this build)");
            Log("diagnostic: script random natives logged in a session: %s/%s", sr0 ? "yes" : "NO", sr1 ? "yes" : "NO");
        }
        if (g_syncLoaderOn && g_cfg.mode != MODE_NETPLAY) SetSyncLoader(true);   // record/replay: the whole run
        Log("determinism: loader %s", !g_syncLoaderOn ? "NOT patched (code doesn't match this build)"
                                      : g_cfg.mode == MODE_NETPLAY ? "drains at a fixed point while a co-op session runs; normal otherwise"
                                                                   : "finishes each frame's loading at a fixed point (sync_loader=1)");
    }
    // Voice-line waits (any mode with audio_wait=1), and in netplay mode the gameplay
    // "is this sound playing" checks and conversation lines too.
    if (g_cfg.audioWait && !g_engineClockReady) {
        Log("determinism: voice-line waits NOT patched (they count engine frames, which need engine_clock=1 and fixed_timestep=1)");
    } else if (g_cfg.audioWait) {
        g_audioWaitOn = PatchAudioWait();
        Log("determinism: scripts waiting on voice lines %s", g_audioWaitOn
            ? "wait the line's authored length in frames (audio_wait=1)" : "NOT patched (code doesn't match this build)");
        if (g_audioWaitOn && g_cfg.mode == MODE_NETPLAY) {
            int ok = 0;
            for (DWORD rva : kSndPlayingCalls) ok += RedirectCall(rva, 0x475580, (void*)H_SndIsPlaying);
            Log("determinism: gameplay checks of \"is this sound playing\" use the frame rule in a session (%d/%d call sites)",
                ok, (int)_countof(kSndPlayingCalls));
            g_lineGateOk = RedirectCall(0x1a9ca9, 0x55d680, (void*)H_LineIsPlaying);
            Log("determinism: conversation lines (mission givers, alert prompts) %s",
                g_lineGateOk ? "end by their authored length in frames in a session" : "NOT handled (code doesn't match this build)");
        }
        if (g_audioWaitOn && g_cfg.mode == MODE_NETPLAY && g_net.ownScreen)
            Log("netplay: own-screen audio: %d listener imports hooked (each PC hears from its own player)",
                InstallAudioListenerHooks());
    }
}
