// net_session.cpp -- netplay session state and plumbing: the session globals, rumble and the
// local pad, packet send and the network lock, the load profiler and keepalive thread, NetEnd, the
// settings sheet, NetHandle/NetPump, input diagnostics, aim settings, pad routing, the keyboard
// and mouse block every online session uses, and the exit hooks.
//
// Netplay is lockstep: both PCs run the whole game and only pad input crosses the network,
// so the game's frame f waits until both players' input for f is here (net_transport.cpp
// explains the protocol). This file holds what the other netplay modules share and the
// edges of a session; the per-frame driver is NetFrame in net_lockstep.cpp.
// Entry points:
//   NetGetState / NetGetCaps / NetSetState  the game's pad calls in netplay mode, from the
//       exports in xinput_proxy.cpp. The anchor poll (the poll that starts each frame) also
//       runs NetFrame, so the whole session advances on the game's main thread.
//   NetPump      reads every waiting packet into NetHandle; called by NetFrame each frame and
//       by the loops that block the game thread (movies, waits for the other PC).
//   NetEnd       ends the session, from anywhere that finds a reason to.
//   NetKeepaliveThread, LoadProfilerThread  background threads NetOpen starts.
//   InstallDesktopBlock (DllMain), InstallNetExitHooks (NetOpen).
// Terms: the settings fingerprint is a hash of the settings both PCs must share (the settings
// sheet below); the state fingerprint is a hash of the game's state, compared every
// hash_interval frames to catch a desync. Drop-in is a friend joining while the host is in a
// level (net_dropin.cpp). A publish is a level finishing loading; the load gate holds both
// PCs at that point until both are loaded (see below). Repair (net_lockstep.cpp) copies the
// host's values over the client's when they drift apart. Own screen: each PC shows only its
// own player, full screen (net_own_screen.cpp).

#include "proxy_shared.h"

bool g_netSettingsNotice;     // client: settings were replaced, show the restart notice
wchar_t g_netPeerNameW[64] = L"the other player";   // their Steam name once known, for on-screen messages
uint64_t g_netMyId;                                  // this player's Steam ID (Steam transport)
volatile LONG64 g_netJoinReq;   // Steam ID from a "Join Game" request, picked up on the game thread

// --- session state ---------------------------------------------------------
//
// Read and written on the game thread under the network lock (NetLockObj below), unless a
// comment says otherwise. Times are GetTickCount64 milliseconds; frames are lockstep frames.

NetPhase g_netPhase = NP_OFF;
uint32_t g_netSettings;                    // this PC's settings fingerprint (NetOpen), in every packet header
bool     g_netPeerHello, g_netPeerReady, g_netLocalReady;   // heard their HELLO / their READY / we're ready
uint32_t g_netFrame;                       // lockstep frame being answered
unsigned g_fpCsrRef;                       // MXCSR at the session's first frame; restored every frame
uint64_t g_fpCsrFixes;                     // frames where it had changed and was put back
// Input rings, indexed by frame % kNetRing: this PC's pad and the other PC's, per frame.
NetPad   g_netLocal[kNetRing], g_netRemote[kNetRing];
uint32_t g_netLocalTag[kNetRing], g_netRemoteTag[kNetRing];   // frame + 1, 0 = empty
NetPad   g_netCur[2];                      // this frame's pad 0 (host) and pad 1 (client)
// State fingerprints by frame, ours and theirs, tagged like the input rings.
uint64_t g_netHashLocal[kNetRing], g_netHashRemote[kNetRing];
uint32_t g_netHashLocalTag[kNetRing], g_netHashRemoteTag[kNetRing];
ULONGLONG g_netLastSend, g_netLastHeard, g_netPhaseSince;   // last packet out / last packet in / this phase began
int      g_netEndNeutralFrames;            // after the session: pad 1 neutral, then disconnected
uint64_t g_netStalls, g_netStallMs, g_netMaxStallMs, g_netHashesChecked, g_netPacketsIn, g_netPacketsOut;   // for the summary
char     g_netEndReason[128];
uint32_t g_netLastHashFrame;               // frame of our newest state fingerprint (sent with every input)
// The other PC said goodbye mid-session (NET_BYE): when, and the last frame it sent input for.
bool     g_netPeerBye;
ULONGLONG g_netPeerByeAt;
uint32_t g_netPeerLastFrame;
ULONGLONG g_netNextStatus;                 // next "still waiting" log line
uint64_t g_netPeerVus, g_netPeerKeys;      // from the other PC's READY
bool     g_netPeerValsKnown, g_netStartKnown;
uint64_t g_netStartVus, g_netStartKeys;    // this session's start values (host picks, client adopts)
bool     g_netRemoveP2;                    // session ended mid-play: remove player 2 on the next poll
ULONGLONG g_netEndedAt;
bool     g_netEndedRunning;                // the session that ended had started lockstep
// Drop-in (net_dropin.cpp; the steps run from NetFrame): where the host is in its save-and-
// reload sequence, whether this session is a drop-in and its world has been launched, and
// the host's save buffer (g_dropBuf, g_dropSize bytes) with the world to load from it.
DropStep g_dropStep;
bool     g_dropIn, g_dropLaunched, g_dropSawBusy;
ULONGLONG g_dropAt;
uint8_t* g_dropBuf;
uint32_t g_dropSize;
char     g_dropWorld[64];
int      g_dropSlot = -1;                  // the host's selected save slot (saveObj+0x64); publish applies "SLOT%d"
// The host's characters for the drop world: its world-database entry's "DefaultAvatar"
// (player 1) and "DefaultAvatar2" (player 2, "" when alone), as the avatar-select
// screen writes them (FUN_1400eb410). A PC that never went through that screen still
// has the file's default, so both PCs write the host's values before loading.
char     g_dropAvatar[2][16];
bool     g_dropPublished;
uint32_t g_dropPublishedFrame;             // lockstep frame the drop world was published
uint32_t g_dropHintAt;                     // show the "press Start to join" hint from this frame (0: none)
// Load gate. After a level publish the game stays paused (g_Display+0x1a8 bit 3: the PM
// and the world managers don't tick) until StandAloneLoop::vf24 has counted DAT_140b2ce48
// up to 4; then it posts LevelLoadComplete, unpauses and forces a PM tick. That came 2
// lockstep frames later on a PC that read the level's files from disk than on one that
// had them in memory (drop-in runs 81-87: the PM's first tick at frame 40 vs 42). In a
// session the counter is held below 4 until both PCs' inputs say they got there, then
// both release it on the same frame. Decomp session's fix point.
bool     g_loadGate;                       // holding the counter (armed at each publish in a session)
bool     g_loadGateReady;                  // this PC's counter reached the hold point
// The pause itself (g_Display+0x1a8 bit 3) wasn't the same on both PCs either: the
// game's pause (FUN_140477470) only sets it when its callback asks for a loading
// screen, so the PC with the town in memory ran the world managers from frame 40
// while the other stayed paused (run 88). While the gate is armed both PCs are held
// paused: the bit is set at the publish and the game's unpause (FUN_140477500) does
// nothing until the gate releases; then vf24's load-complete block unpauses both.
uint32_t (*g_unpauseOrig)();
uint32_t g_unpauseBlocked;                 // unpauses held back while armed (for the log)
// The world is frozen too while the gate is armed: actor updates, scripts and physics
// run outside the pause bit, and a script on the PC with the level in memory acted on
// data the other PC was still streaming (run 93: LP_Barber's DROP_TO_GROUND landed at
// frame 40 on the host, about 72 on the friend's, which then removed it). The world
// update (WilburStandAloneLoop vtable slot 11, called by StandAloneLoop::vf24) is skipped,
// and a PC only says it's ready once its load step is done AND its streaming is idle:
// loader queue empty, data pump idle, nothing in flight on the HogThread, and no file
// read for kStreamIdleFrames frames. Then both release together and start from the same
// fully loaded state.
void (*g_worldUpdateOrig)(void*);
uint32_t g_worldFrozen;                    // world updates skipped while armed
bool     g_loadStepDone;                   // the load-complete counter reached the hold point
uint32_t g_loadStepAt, g_streamIdleFrames;   // frame the load step finished; streaming idle this many frames in a row
LONG64   g_streamLastReads;                // the game's file-read count at the last check
bool     g_clientQuitPosted;               // drop-in client: was in a level, asked the game to quit to the front end
ULONGLONG g_clientFeSince;
uint32_t g_netHashHoldUntil;               // don't compare fingerprints up to this frame (title scene -> level)
// Full-motion movies play in a blocking loop with the game frozen. Movie n is
// the n-th movie of the session on both PCs; either player can skip it for both.
uint32_t g_movieIdx, g_movieAtFrame = 0xFFFFFFFF, g_moviePeerSkipIdx;
uint32_t g_moviePeerIdx;                   // highest movie index the other PC has reached (see H_BinkPlay)
bool     g_movieSkipLocal;                 // this player pressed skip during the current movie
WORD     g_moviePrevButtons;               // pad buttons at the last check, to see new presses
ULONGLONG g_movieLastSend, g_peerMovieAt;  // our last NET_MOVIE sent; their last one heard
bool     g_movieFixOn;                     // the movie patches are in (PatchMovieSkip)
int      g_netSessions;                    // sessions started in this run (2+ = rejoined from the title)
ULONGLONG g_feSeen;                        // last title-screen update (GetTickCount64)
uint64_t g_feState;                        // its page field (+0x290): 0 = "Press Start"
bool     g_feHooked;                       // the title-screen watch is in (HookFrontendWatch)
ULONGLONG g_netNextHint;                   // earliest time for the next repeated hint banner
// Is the game showing "Press Start"? The title screen's update hook (net_own_screen.cpp)
// stamps g_feSeen and g_feState every frame it runs, so a stamp under half a second old
// with page 0 means yes. Without the hook there's no way to tell, so it answers yes.
bool OnPressStart() { return !g_feHooked || (GetTickCount64() - g_feSeen < 500 && g_feState == 0); }
// This PC's entity-key counter (DAT_1409575c8): every entity the game creates takes the next
// key, so both PCs must agree on it. Sent in READY; the start values put it above both PCs'.
uint64_t LocalKeys() {
    __try { return *(volatile uint64_t*)((BYTE*)GetModuleHandleW(NULL) + 0x9575c8); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int      g_netLocalPadRes = -1;            // this player's pad before lockstep: 0 connected, 1 not, -1 unknown
static bool     g_netLocalPadSeen;                // a button was seen on this player's pad
static ULONGLONG g_netRumbleUntil;
static bool     g_netRumbling;

// The Steam connection's state (NetTransport::LinkState) in words, for the log.
const char* NetLinkName(int st) {
    switch (st) {
    case -1: return "n/a";
    case 0:  return "no session (nothing heard from them yet)";
    case 1:  return "connecting";
    case 2:  return "finding a route";
    case 3:  return "connected";
    case 4:  return "closed by the other player";
    case 5:  return "problem detected on this PC";
    default: return "unknown state";
    }
}

// A short buzz on this player's own controller, so they can feel the
// session's progress without reading the log.
void NetRumble(ULONGLONG ms, WORD strength) {
    XINPUT_VIBRATION v = { strength, strength };
    XInputSetState((DWORD)g_net.localPad, &v);
    g_netRumbleUntil = GetTickCount64() + ms;
    g_netRumbling = true;
}
// Every frame (NetFrame): stop our buzz once its time is up. NetSetState clears
// g_netRumbling when the game drives the motors itself, so this never cuts the game's rumble.
void NetRumbleTick(ULONGLONG now) {
    if (!g_netRumbling || now < g_netRumbleUntil) return;
    XINPUT_VIBRATION v = {};
    XInputSetState((DWORD)g_net.localPad, &v);
    g_netRumbling = false;
}

// Before lockstep the game sees a neutral pad, so prove here that this
// player's controller (Steam Input included) actually reaches the proxy.
void NetCheckLocalPad() {
    if (g_net.testInputSeed) return;
    XINPUT_STATE s = {};
    int res = XInputGetState((DWORD)g_net.localPad, &s) == ERROR_SUCCESS ? 0 : 1;
    DebugMenu_FilterPad(0, &s, true);                    // the menu's once-per-frame tick while waiting
    if (res != g_netLocalPadRes) {
        if (res == 0) Log("netplay: this player's controller (pad %d) is connected", g_net.localPad);
        else Log("netplay: no controller on pad %d (with Steam Input, check this game's controller setting in Steam)", g_net.localPad);
        g_netLocalPadRes = res;
    }
    const XINPUT_GAMEPAD& g = s.Gamepad;
    bool moved = g.wButtons || g.bLeftTrigger > 100 || g.bRightTrigger > 100 || abs(g.sThumbLX) > 16000 || abs(g.sThumbLY) > 16000;
    if (res == 0 && moved && !g_netLocalPadSeen) {
        g_netLocalPadSeen = true;
        Log("netplay: this player's controller works (buttons %04x)", g.wButtons);
        NetRumble(150, 20000);
    }
}
uint32_t g_netLocalSentUpTo = 0xFFFFFFFF;  // newest frame we've sent input for (0xFFFFFFFF: none yet)
uint64_t g_netLastHash;                    // our newest state fingerprint (frame g_netLastHashFrame)

// A packet header of the given type, from this PC.
NetHdr NetHeader(uint8_t type) { return { kNetMagic, type, (uint8_t)g_net.role, kNetVersion, g_netSettings }; }

// Send a packet (if a transport is open), counting it and noting the time for the keepalive.
void NetSend(const void* d, int n) { if (g_netT) { g_netT->Send(d, n); g_netPacketsOut++; g_netLastSend = GetTickCount64(); } }

// All game-thread network work (the per-frame step, the movie callback, the
// goodbye at exit) holds g_netLock. A long load or unload runs entirely inside
// one game frame, sending nothing, which would look like a disconnect; so a
// small thread says "still here, busy" whenever the game thread has been quiet
// for half a second and isn't in the network code itself.
CRITICAL_SECTION* NetLockObj() {
    // Made on first use, so it exists whichever module or thread asks for it first.
    static CRITICAL_SECTION cs;
    static bool init = (InitializeCriticalSection(&cs), true);
    (void)init;
    return &cs;
}
volatile ULONGLONG g_peerBusyAt;           // when the other PC last said it was busy (NET_BUSY)
static uint64_t g_netBusySent;             // NET_BUSY packets this PC sent
// Long-frame profiler. Level loads inside a session take 40-120 s against about
// a second outside one (the fixed-point loader). While a lockstep frame has run
// for more than 400 ms, the main thread is sampled every 10 ms: where it is,
// and the first exe return addresses on its stack. When the frame ends the
// busiest spots are logged, so the stall is read from the log, not guessed.
struct ProfBucket { uintptr_t key; uint32_t n; };
struct ProfMod { const wchar_t* name; uintptr_t base, size; };
static ProfBucket g_profRip[128], g_profRet[128];
static int        g_profRipN, g_profRetN, g_profModN;
static uint32_t   g_profSamples;
static ProfMod    g_profMods[12];
// Count one sample at address key; a table holds up to 128 different addresses.
static void ProfAdd(ProfBucket* b, int& n, uintptr_t key) {
    for (int i = 0; i < n; i++) if (b[i].key == key) { b[i].n++; return; }
    if (n < 128) { b[n].key = key; b[n].n = 1; n++; }
}
// Note where the exe (entry 0, GetModuleHandleW(NULL)) and the DLLs a stall is likely to be
// in sit in memory, so samples can be named by module.
static void ProfModules() {
    static const wchar_t* names[] = { NULL, L"ntdll.dll", L"kernelbase.dll", L"kernel32.dll", L"xinput1_4.dll", L"fmod.dll",
                                      L"fmodstudio.dll", L"d3d12.dll", L"dxgi.dll", L"steam_api64.dll", L"bink2w64.dll" };
    g_profModN = 0;
    for (const wchar_t* nm : names) {
        HMODULE m = GetModuleHandleW(nm);
        if (!m) continue;
        BYTE* b = (BYTE*)m;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
        g_profMods[g_profModN++] = { nm ? nm : L"exe", (uintptr_t)b, nt->OptionalHeader.SizeOfImage };
    }
}
// An address as the decomp shows it (0x14... for the exe), module+offset for a DLL, or ?addr.
static void ProfName(uintptr_t a, char* out, size_t n) {
    for (int i = 0; i < g_profModN; i++)
        if (a >= g_profMods[i].base && a < g_profMods[i].base + g_profMods[i].size) {
            if (i == 0) _snprintf_s(out, n, _TRUNCATE, "0x%llx", 0x140000000ULL + (a - g_profMods[i].base));
            else _snprintf_s(out, n, _TRUNCATE, "%S+%llx", g_profMods[i].name, (unsigned long long)(a - g_profMods[i].base));
            return;
        }
    _snprintf_s(out, n, _TRUNCATE, "?%llx", (unsigned long long)a);
}
// Log a table's 8 most-sampled addresses with their counts, then empty it.
static void ProfTop(ProfBucket* b, int& n, const char* what) {
    char line[900] = "";
    for (int k = 0; k < 8; k++) {
        int best = -1;
        for (int i = 0; i < n; i++) if (b[i].n && (best < 0 || b[i].n > b[best].n)) best = i;
        if (best < 0) break;
        char nm[64];
        ProfName(b[best].key, nm, sizeof nm);
        size_t len = strlen(line);
        _snprintf_s(line + len, sizeof line - len, _TRUNCATE, " %s:%u", nm, b[best].n);
        b[best].n = 0;
    }
    n = 0;
    Log("profile: %s:%s", what, line);
}
// The profiler's own thread, started by NetOpen; runs until the game exits. Only samples
// while lockstep runs (g_clockLockstep) and the current frame is over 400 ms old.
DWORD WINAPI LoadProfilerThread(LPVOID) {
    HANDLE mt = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, FALSE, g_mainThread);
    if (!mt) return 0;
    ProfModules();
    uint64_t lastFrame = g_engineFrames;
    ULONGLONG start = 0;
    for (;;) {
        Sleep(10);
        if (g_engineFrames != lastFrame || !g_clockLockstep) {
            if (g_profSamples) {                         // the long frame ended: report it
                Log("profile: lockstep frame of %llu ms, %u samples (where the main thread was, and the exe code it was called from)",
                    GetTickCount64() - start, g_profSamples);
                ProfTop(g_profRip, g_profRipN, "at");
                ProfTop(g_profRet, g_profRetN, "from");
                g_profSamples = 0;
            }
            lastFrame = g_engineFrames;
            if (!g_clockLockstep) continue;
        }
        if (GetTickCount64() - g_frameWallStart < 400) continue;
        if (!g_profSamples) start = g_frameWallStart;
        CONTEXT cx = {};
        cx.ContextFlags = CONTEXT_CONTROL;
        uintptr_t stack[96] = {};
        bool ok = false;
        if (SuspendThread(mt) != (DWORD)-1) {            // nothing that locks or allocates while it's suspended
            if (GetThreadContext(mt, &cx)) {
                ok = true;
                __try { memcpy(stack, (const void*)cx.Rsp, sizeof stack); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            ResumeThread(mt);
        }
        if (!ok) continue;
        // Where it is (Rip), and the first two exe addresses on its stack: likely return
        // addresses, i.e. the game code that called whatever it's stuck in.
        g_profSamples++;
        ProfAdd(g_profRip, g_profRipN, (uintptr_t)cx.Rip);
        int found = 0;
        for (int i = 0; i < 96 && found < 2; i++)
            if (stack[i] >= g_profMods[0].base && stack[i] < g_profMods[0].base + g_profMods[0].size) { ProfAdd(g_profRet, g_profRetN, stack[i]); found++; }
    }
}

// The keepalive thread (started by NetOpen; see NetLockObj above). Every 250 ms: if a session
// is open and nothing was sent for half a second, and the game thread isn't holding the
// network lock, send NET_BUSY at once so the other PC doesn't time out.
DWORD WINAPI NetKeepaliveThread(LPVOID) {
    for (;;) {
        Sleep(250);
        NetPhase ph = g_netPhase;
        if (!g_netT || ph == NP_OFF || ph == NP_ENDED) continue;
        if (GetTickCount64() - g_netLastSend < 500) continue;
        if (!TryEnterCriticalSection(NetLockObj())) continue;   // the game thread is in the network code: not stuck
        if (g_netT && g_netPhase != NP_ENDED && g_netPhase != NP_OFF && GetTickCount64() - g_netLastSend >= 500) {
            NetHdr h = NetHeader(NET_BUSY);
            g_netT->SendNow(&h, sizeof h);
            g_netT->Flush();
            g_netLastSend = GetTickCount64();
            g_netBusySent++;
        }
        LeaveCriticalSection(NetLockObj());
    }
}

// End the session (no-op if none is open): tell the other PC (NET_BYE, three times in case
// one is lost), say why on screen and in the log, and switch the game back to playing alone.
// If lockstep was running, player 2 is removed and the session-only changes (own-screen
// viewports, HUD, music hold) are undone.
void NetEnd(const char* reason) {
    if (g_netPhase == NP_ENDED || g_netPhase == NP_OFF) return;
    bool wasRunning = g_netPhase == NP_RUNNING;
    strncpy_s(g_netEndReason, reason, _TRUNCATE);
    g_netEndedRunning = wasRunning;
    g_netPhase = NP_ENDED;
    g_netEndedAt = GetTickCount64();
    if (g_netT) {
        g_netT->Flush();
        NetByePkt bye = { NetHeader(NET_BYE), g_netLocalSentUpTo };
        for (int i = 0; i < 3; i++) g_netT->SendNow(&bye, sizeof bye);
    }
    g_netEndNeutralFrames = wasRunning ? 30 : 0;
    Log("netplay: session ended at frame %u: %s. Carrying on single player.", g_netFrame, reason);
    DebugMenu_EnableSharedActions(false, false);
    DebugMenu_SetNetplay(false);                       // playing alone again: the debug menu's cheats work
    SetSyncLoader(false);                              // and the game's own, faster loading
    g_clockLockstep = false;
    if (g_feHooked)
        Status(15000, kOvRed, L"Online co-op ended: %S.\nPlaying on alone. To play together again, both hold Back + Start "
                              L"for a moment.", reason);
    else
        Status(15000, kOvRed, L"Online co-op ended: %S.\nPlaying on alone. To play together again, both restart the game.", reason);
    if (g_protectSave)
        Log("drop-in: this PC's own save was never written; restart the game to go back to it");
    if (wasRunning) {
        g_netRemoveP2 = true;                          // on the next poll, while pad 1 still reads connected
        NetViewportsRefresh();                         // back to normal split screen
        if (g_healthPlaced) HudPlaceHealthBars(false);
        HudHidePrompts(false);
        NetMusicSyncHold(false);
        g_zeroAllocs = false;
    }
}

// --- settings sheet ----------------------------------------------------------
//
// The settings fingerprint in every packet header is a hash of a short text
// sheet, one "key=value" line per thing both PCs must share (NetOpen writes it).
// When the fingerprints differ, each PC sends the other its sheet, so both can
// say which settings differ and where to change them instead of only "the two
// PCs don't match". Lines starting with '~' are information only (not hashed):
// they explain a difference without being one. Builds before 95 send no sheet;
// a few seconds without one, the message says the other build is probably older.
// A NET_SHEET packet: the header, the format byte, then the sheet's text.
enum { NET_SHEET = 16 };
static const uint8_t kSheetFormat = 1;
std::string g_netSheet;                     // this PC's sheet
static ULONGLONG g_sheetLastSent, g_sheetHeardAt;  // our last send; their last sheet
static ULONGLONG g_sheetWaitUntil;                 // a mismatch seen: when to stop waiting for their sheet
static std::string g_sheetLastShown;               // their sheet as last explained (logged once each)

// How each sheet line reads on screen. kind: 'b' on/off, 's' distance scale (1 = the
// game's own), 'f' frames, 'n' the number as is, 'x' "different" with no values shown.
// Keys this table lacks (from a newer build) show as their own name.
struct SheetKey { const char* key; const wchar_t* label; const wchar_t* where; char kind; };
static const SheetKey kSheetKeys[] = {
    { "build",              L"Yee Haw Edition build",       L"both need the same one", 'n' },
    { "proto",              L"Yee Haw Edition build",       L"both need the same one", 'n' },
    { "exe",                L"Game version",                L"Steam: verify the game's files", 'x' },
    { "files",              L"Game files",                  L"Steam: verify the game's files, remove other mods", 'x' },
    { "input_delay",        L"Input delay",                 L"launcher: Partner Up", 'f' },
    { "own_screen",         L"Full screen each",            L"launcher: Partner Up", 'b' },
    { "drop_in",            L"Drop-in",                     L"launcher: Partner Up", 'b' },
    { "hidden_levels",      L"Restore Wii-only levels",     L"launcher: Extras", 'b' },
    { "script_fixes",       L"2010 script fixes",           L"launcher: Extras", 'b' },
    { "~graphics",          L"Graphics improvements",       L"launcher: Dress It Up; the distances count only when it's on", 'b' },
    { "lod_scale",          L"Character detail distance",   L"launcher: Dress It Up", 's' },
    { "world_lod_scale",    L"World detail distance",       L"launcher: Dress It Up", 's' },
    { "object_fade_scale",  L"Object draw distance",        L"launcher: Dress It Up", 's' },
    { "cull_pop",           L"Keep small objects far away", L"launcher: Dress It Up", 'x' },
    { "cull_fade",          L"Keep small objects far away", L"launcher: Dress It Up", 'x' },
    { "shadow_cull_near",   L"Keep small objects far away", L"launcher: Dress It Up", 'x' },
    { "shadow_cull_mid",    L"Keep small objects far away", L"launcher: Dress It Up", 'x' },
    { "shadow_cull_far",    L"Keep small objects far away", L"launcher: Dress It Up", 'x' },
    { "shadow_no_contrib",  L"Keep small objects far away", L"launcher: Dress It Up", 'x' },
    { "shadow_all_zones",   L"shadow_all_zones",            L"xinput_harness.ini [graphics]", 'n' },
    { "shadow_near_extend", L"shadow_near_extend",          L"xinput_harness.ini [graphics]", 'n' },
    { "shadow_far",         L"shadow_far",                  L"xinput_harness.ini [graphics]", 'n' },
    { "shadow_cascades",    L"shadow_cascades",             L"xinput_harness.ini [graphics]", 'n' },
    { "shadow_split",       L"shadow_split",                L"xinput_harness.ini [graphics]", 'n' },
    { "fps",                L"fps",                         L"xinput_harness.ini [harness]", 'n' },
    { "rand_isolate",       L"rand_isolate",                L"xinput_harness.ini [harness]", 'n' },
    { "spawn_gate",         L"spawn_gate",                  L"xinput_harness.ini [harness]", 'n' },
    { "spawn_delay",        L"spawn_delay",                 L"xinput_harness.ini [harness]", 'n' },
    { "sync_loader",        L"sync_loader",                 L"xinput_harness.ini [harness]", 'n' },
    { "audio_wait",         L"audio_wait",                  L"xinput_harness.ini [harness]", 'n' },
    { "hash_interval",      L"hash_interval",               L"xinput_harness.ini [netplay]", 'n' },
    { "repair",             L"repair",                      L"xinput_harness.ini [netplay]", 'n' },
    { "repair_lead",        L"repair_lead",                 L"xinput_harness.ini [netplay]", 'n' },
    { "repair_followers",   L"repair_followers",            L"xinput_harness.ini [netplay]", 'n' },
    { "fixes",              L"Built-in game fixes",         L"one PC couldn't apply some; compare the two logs", 'x' },
};
// The table entry for a sheet key, or NULL for a key this build doesn't know.
static const SheetKey* SheetKeyFor(const std::string& k) {
    for (const SheetKey& s : kSheetKeys) if (k == s.key) return &s;
    return NULL;
}

// "key=value" lines in order; a key missing from one sheet reads as "".
// (Also used by NetOpen, which hashes the parsed lines into the settings fingerprint.)
std::vector<std::pair<std::string, std::string>> SheetParse(const std::string& s) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t e = s.find('\n', i);
        if (e == std::string::npos) e = s.size();
        std::string line = s.substr(i, e - i);
        size_t eq = line.find('=');
        if (eq != std::string::npos) out.push_back({ line.substr(0, eq), line.substr(eq + 1) });
        i = e + 1;
    }
    return out;
}
// The value for key k, "" if absent.
static std::string SheetGet(const std::vector<std::pair<std::string, std::string>>& v, const std::string& k) {
    for (auto& kv : v) if (kv.first == k) return kv.second;
    return "";
}
// A value as the player reads it, by its key's kind (see SheetKey).
static std::wstring SheetShow(char kind, const std::string& v) {
    if (v.empty()) return L"not in that build";
    std::wstring w(v.begin(), v.end());
    switch (kind) {
    case 'b': return v == "0" ? L"off" : L"on";
    case 's': return v == "1" ? L"game" : w + L"x";
    case 'f': return w + L" frames";
    default:  return w;
    }
}

// Send this PC's sheet in one NET_SHEET packet of at most 1 KB (reliable over Steam).
static void SheetSend() {
    if (!g_netT || g_netSheet.empty()) return;
    uint8_t pkt[1024];
    NetHdr h = NetHeader(NET_SHEET);
    size_t n = g_netSheet.size();
    if (n > sizeof pkt - sizeof h - 1) n = sizeof pkt - sizeof h - 1;   // NetOpen keeps it far below this
    memcpy(pkt, &h, sizeof h);
    pkt[sizeof h] = kSheetFormat;
    memcpy(pkt + sizeof h + 1, g_netSheet.data(), n);
    g_netT->SendReliable(pkt, (int)(sizeof h + 1 + n));
    g_sheetLastSent = GetTickCount64();
}

// The fingerprints differ: send ours (at most every 5 s) and, unless their sheet came
// already, wait a few seconds for theirs before saying they're probably on an older build.
static void SheetMismatchSeen() {
    ULONGLONG now = GetTickCount64();
    if (now - g_sheetLastSent > 5000) SheetSend();
    if (!g_sheetWaitUntil && now - g_sheetHeardAt > 10000) g_sheetWaitUntil = now + 5000;
}

// Their sheet: list what differs, on screen (the first few, by the names the launcher
// uses and where to change each) and in the log (every line).
static void SheetReceived(const uint8_t* p, int n) {
    if (n < (int)sizeof(NetHdr) + 1) return;
    if (p[sizeof(NetHdr)] != kSheetFormat) Log("netplay: the other player's settings sheet is format %u (ours %u); reading it anyway",
                                               p[sizeof(NetHdr)], kSheetFormat);
    std::string theirs((const char*)p + sizeof(NetHdr) + 1, n - sizeof(NetHdr) - 1);
    if (theirs == g_netSheet || g_netPhase == NP_RUNNING) return;   // same settings (a resend), or a stray mid-game
    ULONGLONG now = GetTickCount64();
    g_sheetHeardAt = now;
    g_sheetWaitUntil = 0;
    if (now - g_sheetLastSent > 5000) SheetSend();     // so their game can say what differs too
    if (g_net.transport != 0 && (g_netPhase == NP_CONNECTING || g_netPhase == NP_WAITSTART))
        NetEnd("the two PCs' settings differ");
    if (theirs == g_sheetLastShown) return;
    g_sheetLastShown = theirs;

    auto ours = SheetParse(g_netSheet), them = SheetParse(theirs);
    std::vector<std::string> keys;                     // ours in order, then any only they have
    for (auto& kv : ours) keys.push_back(kv.first);
    for (auto& kv : them) if (std::find(keys.begin(), keys.end(), kv.first) == keys.end()) keys.push_back(kv.first);
    std::vector<std::string> diff;
    bool gfxDiff = false;
    for (auto& k : keys) {
        std::string a = SheetGet(ours, k), b = SheetGet(them, k);
        if (a == b) continue;
        Log("netplay: settings differ: %s is %s here, %s on the other PC", k.c_str(), a.empty() ? "(none)" : a.c_str(),
            b.empty() ? "(none)" : b.c_str());
        if (k[0] == '~') continue;
        diff.push_back(k);
        const SheetKey* sk = SheetKeyFor(k);
        if (sk && wcsstr(sk->where, L"Dress It Up")) gfxDiff = true;
    }
    // A different build changes the sheet itself, so its other lines would only be noise.
    if (std::find(diff.begin(), diff.end(), "build") != diff.end() || std::find(diff.begin(), diff.end(), "proto") != diff.end()) {
        std::string a = SheetGet(ours, "build"), b = SheetGet(them, "build");
        Status(30000, kOvRed, L"Online co-op couldn't start: you and %s have different Yee Haw Edition builds\n"
                              L"(yours is %S, theirs is %S). Both need the same build.", g_netPeerNameW,
               a.c_str(), b.empty() ? "older" : b.c_str());
        return;
    }
    // The distances read "game" while Graphics improvements is off: name the switch first.
    if (gfxDiff && SheetGet(ours, "~graphics") != SheetGet(them, "~graphics")) diff.insert(diff.begin(), "~graphics");

    std::wstring msg = L"Online co-op couldn't start: these settings differ from ";
    msg += g_netPeerNameW;
    msg += L"'s:";
    std::vector<const wchar_t*> shown;                 // one line per label (the culls share one switch)
    int lines = 0, more = 0;
    for (auto& k : diff) {
        const SheetKey* sk = SheetKeyFor(k);
        const wchar_t* label = sk ? sk->label : NULL;
        if (label && std::find_if(shown.begin(), shown.end(), [&](const wchar_t* s) { return !wcscmp(s, label); }) != shown.end())
            continue;
        if (label) shown.push_back(label);
        if (lines == 4) { more++; continue; }
        lines++;
        std::wstring name = label ? label : std::wstring(k.begin(), k.end());
        msg += L"\n\x2022 " + name;
        if (!sk || sk->kind != 'x')
            msg += L": you " + SheetShow(sk ? sk->kind : 'n', SheetGet(ours, k)) + L", them " + SheetShow(sk ? sk->kind : 'n', SheetGet(them, k));
        else
            msg += L": different";
        if (sk) { msg += L" ("; msg += sk->where; msg += L")"; }
    }
    if (more) msg += L"\n...and " + std::to_wstring(more) + L" more (the log lists them all)";
    msg += g_feHooked ? L"\nMake them the same on both PCs, then both hold Back + Start."
                      : L"\nMake them the same on both PCs, then both restart the game.";
    if (diff.empty()) msg = L"Online co-op couldn't start: the two PCs don't match, but their settings read the same.\nThe log has both.";
    Status(30000, kOvRed, L"%s", msg.c_str());
}

// No sheet came back: the other PC is on a build from before sheets (94 or older).
static void SheetCheckWait() {
    if (!g_sheetWaitUntil || GetTickCount64() < g_sheetWaitUntil) return;
    g_sheetWaitUntil = 0;
    Log("netplay: no settings sheet from the other player; their build is probably older than %u", kYeeHawBuild);
    Status(30000, kOvRed, L"Online co-op couldn't start: %s's game doesn't match yours.\n"
                          L"Their game didn't say what differs, so it's probably an older Yee Haw Edition build.\n"
                          L"Both need build %u.", g_netPeerNameW, kYeeHawBuild);
}

// After a session ended, the other PC is saying HELLO or READY again: a hint banner,
// at most every 8 s.
static void NetPeerWantsRejoin() {
    ULONGLONG now = GetTickCount64();
    if (now < g_netNextHint) return;
    g_netNextHint = now + 8000;
    Status(6000, kOvBlue, L"%s wants to play again.\nHold Back + Start for a moment to join them.", g_netPeerNameW);
}

// One received packet (from NetPump, under the network lock). Packets that aren't ours or
// claim our own role are dropped; ones from a different build or settings start the sheet
// exchange (and over Steam end a session that hasn't started) but are otherwise ignored.
// The rest are handled by type: most only record what arrived, and the per-frame code
// (NetFrame) acts on it.
static void NetHandle(const uint8_t* p, int n) {
    if (n < (int)sizeof(NetHdr)) return;
    const NetHdr* h = (const NetHdr*)p;
    if (h->magic != kNetMagic || h->role == g_net.role) return;
    g_netPacketsIn++;
    g_netLastHeard = GetTickCount64();
    if (h->type == NET_SHEET) { SheetReceived(p, n); return; }   // read whatever the fingerprints say: it explains them
    if (h->version != kNetVersion || h->settings != g_netSettings) {
        SheetMismatchSeen();
        if (g_net.transport == 0) {
            // UDP: anything on this PC or network can reach the port (another test, an old
            // copy of the game). Ignore it rather than end the session over a stray packet.
            static ULONGLONG lastNote;
            ULONGLONG t = GetTickCount64();
            if (t - lastNote > 10000) {
                lastNote = t;
                Log("netplay: ignoring packets from a different build or settings (theirs %08x v%u, ours %08x v%u)",
                    h->settings, h->version, g_netSettings, kNetVersion);
            }
            return;
        }
        if (g_netPhase == NP_CONNECTING || g_netPhase == NP_WAITSTART) {
            Log("netplay: the other player's settings or game build differ (theirs %08x v%u, ours %08x v%u)",
                h->settings, h->version, g_netSettings, kNetVersion);
            NetEnd("the two PCs' settings differ");     // the sheets say which (SheetReceived / SheetCheckWait)
        }
        return;
    }
    switch (h->type) {
    case NET_HELLO:
        if (g_netPhase == NP_ENDED) { NetPeerWantsRejoin(); break; }
        if (!g_netPeerHello) Log("netplay: connected to the %s", h->role ? "client" : "host");
        g_netPeerHello = true;
        break;
    case NET_READY: {
        if (g_netPhase == NP_ENDED) { NetPeerWantsRejoin(); break; }
        if (!g_netPeerReady) Log("netplay: the other player is ready");
        g_netPeerReady = true;
        g_netPeerHello = true;
        if (n >= (int)sizeof(NetReadyPkt)) {
            const NetReadyPkt* r = (const NetReadyPkt*)p;
            g_netPeerVus = r->vus;
            g_netPeerKeys = r->keys;
            g_netPeerValsKnown = true;
            if (g_net.role == 1 && r->started && !g_netStartKnown) {
                g_netStartVus = r->startVus;
                g_netStartKeys = r->startKeys;
                g_netStartKnown = true;
            }
        }
        break;
    }
    case NET_BYE:
        if (g_netPhase == NP_WAITSTART || g_netPhase == NP_CONNECTING) {
            NetEnd("the other player left before the start (right after the save transfer, that means their game "
                   "settings were updated and they need to restart)");
            ShowNotice(L"The other player's game left before the session started.\n\n"
                       L"Right after connecting, this usually means their game settings were updated to match yours. "
                       L"Both of you: close the game, start it again, and connect again.");
            break;
        }
        if (g_netPhase == NP_RUNNING && !g_netPeerBye) {
            // Inputs they sent before leaving still count: play on through them.
            g_netPeerBye = true;
            g_netPeerByeAt = GetTickCount64();
            g_netPeerLastFrame = n >= (int)sizeof(NetByePkt) ? ((const NetByePkt*)p)->lastFrame : 0xFFFFFFFF;
        }
        break;
    case NET_BUSY:
        g_peerBusyAt = GetTickCount64();                 // loading or unloading; alive
        break;
    case NET_MOVIE:
        if (n >= (int)sizeof(NetMoviePkt)) {
            const NetMoviePkt* m = (const NetMoviePkt*)p;
            g_peerMovieAt = GetTickCount64();
            if (m->idx > g_moviePeerIdx) g_moviePeerIdx = m->idx;
            if (m->skip) g_moviePeerSkipIdx = m->idx;
        }
        break;
    case NET_INPUT: {
        if (n < (int)offsetof(NetInputPkt, pads)) return;
        const NetInputPkt* in = (const NetInputPkt*)p;
        int count = in->count;
        // count may be below kNetRedundancy, so the packet can be shorter than the struct.
        if (count > kNetRedundancy || n < (int)(offsetof(NetInputPkt, pads) + count * sizeof(NetPad))) return;
        if (g_net.role == 1 && in->startVus && !g_netStartKnown) {   // the host started: adopt its start values
            g_netStartVus = in->startVus;
            g_netStartKeys = in->startKeys;
            g_netStartKnown = true;
        }
        if (g_netPhase == NP_WAITSTART && g_netLocalReady) g_netPeerReady = true;   // they started; so can we
        if (in->hashFrame) NetRemoteHash(in->hashFrame, in->hash);
        if (in->rngCount && in->rngCount <= kNetRngTags) RngStoreRemote(in->rngLast, in->rngCount, in->rng);
        // Store each pad in the remote ring under its frame (tag = frame + 1 marks it present).
        for (int k = 0; k < count; k++) {
            uint32_t f = in->lastFrame - (count - 1) + k;
            if (f < g_netFrame) continue;                       // already used
            if (f >= g_netFrame + kNetRing - 1) continue;       // too far ahead (can't happen with sane delays)
            g_netRemote[f % kNetRing] = in->pads[k];
            g_netRemoteTag[f % kNetRing] = f + 1;
        }
        break;
    }
    case NET_SAVE_INFO: case NET_SAVE_CHUNK: case NET_SAVE_ACK:
        if (g_netPhase == NP_WAITSTART || g_netPhase == NP_CONNECTING) SaveHandle(p, n, h->type);
        break;
    case NET_HASH: {
        if (n < (int)sizeof(NetHashPkt)) return;
        const NetHashPkt* hp = (const NetHashPkt*)p;
        NetRemoteHash(hp->frame, hp->hash);
        break;
    }
    case NET_RDIGEST: case NET_RREQ: case NET_RVAL: case NET_RNAK: case NET_RMISSED:
        RepHandle(p, n, h->type);
        break;
    }
}

// Handle up to 64 waiting packets, then check whether a settings sheet is overdue.
void NetPump() {
    if (!g_netT) return;
    g_netT->Pump();
    uint8_t buf[1024];
    for (int i = 0; i < 64; i++) {
        int n = g_netT->Recv(buf, sizeof buf);
        if (n <= 0) break;
        NetHandle(buf, n);
    }
    SheetCheckWait();
}

// READY: this player is ready to start, with this PC's game time and entity-key counter, and
// (host, once it has picked them) the start values both PCs will use.
void NetSendReady() {
    NetReadyPkt r = {};
    r.h = NetHeader(NET_READY);
    r.vus = (uint64_t)g_vus;
    r.keys = LocalKeys();
    r.started = g_netStartKnown && g_net.role == 0;
    r.startVus = g_netStartVus;
    r.startKeys = g_netStartKeys;
    NetSend(&r, sizeof r);
}

// Back + Start held together: how a player says they're ready to start a session.
bool NetComboHeld(const NetPad& p) {
    return (p.pad.wButtons & (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START)) == (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START);
}

// g_netWanted: netplay mode, so NetFrame should open the session; g_netTried: it has tried
// (NetOpen), whether or not that worked.
bool g_netWanted, g_netTried;

// Diagnostic: the game's input devices (g_InputDriver+0x460, count +0x458; dev 0 is
// the keyboard, the rest XInput pads with their user index at +0x2d0) and its
// player->device table (+0x4b8, bound count +0x4e4), logged at lockstep start and
// whenever they change. Both PCs must show the same lines at the same frames;
// if not, the bindings need pinning at lockstep start. Read-only. Called every running
// session frame (NetFrame); at most 40 lines. g_InputDriver is the pointer at RVA 0xac5380.
void NetLogBindings(uint32_t f) {
    static uint64_t last;
    static int lines;
    if (lines >= 40) return;
    BYTE* drv = NULL;
    char buf[256];
    uint64_t sig = 0xcbf29ce484222325ull;
    int len = 0;
    __try {
        drv = *(BYTE**)(ExeBase() + 0xac5380);
        if (!drv) return;
        uint32_t count = *(uint32_t*)(drv + 0x458);
        len = sprintf_s(buf, "devices %u:", count);
        for (uint32_t i = 0; i < count && i < 11; i++) {
            BYTE* dev = *(BYTE**)(drv + 0x460 + i * 8);
            int user = !dev ? -2 : i == 0 ? -1 : *(int*)(dev + 0x2d0);
            if (user == -1) len += sprintf_s(buf + len, sizeof buf - len, "%s %u=keyboard", i ? "," : "", i);
            else len += sprintf_s(buf + len, sizeof buf - len, "%s %u=pad %d", i ? "," : "", i, user + 1);
            sig = (sig ^ (uint64_t)(user + 7)) * 0x100000001b3ull;
        }
        int p0 = *(int*)(drv + 0x4b8), p1 = *(int*)(drv + 0x4b8 + 4), bound = *(int*)(drv + 0x4e4);
        len += sprintf_s(buf + len, sizeof buf - len, "; player 1 -> device %d, player 2 -> device %d (%d bound)", p0, p1, bound);
        sig = (((sig ^ (uint64_t)(p0 + 7)) * 0x100000001b3ull ^ (uint64_t)(p1 + 7)) * 0x100000001b3ull ^ (uint64_t)bound) * 0x100000001b3ull;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (sig == last) return;
    last = sig;
    lines++;
    Log("input: frame %u: %s", f, buf);
}

// Every lockstep frame must poll the pads on both PCs. WindowsSystemInputDriver_Update
// spends a frame re-enumerating DirectInput instead of polling when DAT_140ac481a is
// set (a device change, on one PC only) unless g_ForceCreateAllXInputPads (0x140950321,
// 1 as shipped, otherwise read only at start-up when the pad devices are made) is 0.
// So it's 0 while a session runs, and its own value again after, when any pending
// re-enumeration happens (decomp session). Called after every NetFrame with whether lockstep
// is running; only a change does anything.
void NetHoldPadPoll(bool hold) {
    static bool held;
    static BYTE saved;
    if (hold == held) return;
    BYTE* flag = ExeBase() + 0x950321;
    __try {
        if (hold) { saved = *flag; *flag = 0; }
        else *flag = saved;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    held = hold;
    Log(hold ? "input: the pads are read every frame of the session (device-change re-enumeration held off)"
             : "input: device-change re-enumeration allowed again");
}

// Aim settings (decomp session, re\SYNC_VIABILITY.md "Aimed throws"). Of CGameSettings
// (*(0x140b2ef68)), only the per-player invert flags (P1 +0x4c..+0x51, P2 +0x68..+0x6d)
// reach the simulation: aiming, MiniTurret, cameras and flight read them live. They're
// this PC's own options, so two PCs can differ, and an aimed throw (the cow at the
// targets, the alien launcher) then flies differently. (+0x44/+0x48 pick a control scheme
// for a mapper named "Avatar", which doesn't exist: the player's is "Buzz". +0x3c is mouse
// sensitivity, blocked in netplay. The rest is rumble, UI and audio.)
// So the host sends its flags as a lockstep action every 120 frames, one player's six bytes
// per action (frames 0 and 1 mod 120), and both PCs write them on the frame it's applied.
// Only player 1 sends actions; this one rides in the debug-menu action slot, with its own
// id and length so the menu's handler (12-byte actions) never sees it.
static const uint8_t kActAimInvert = 0xA1;              // act: id, player (0/1), 6 flag bytes
static const int kActAimInvertLen = 8;
// Host, on a frame with no menu action to send: when frame "mine" is 0 or 1 mod 120, fill a
// with player 1's or player 2's flags (p = mine % 120) and return the action's length;
// otherwise 0 (nothing to send).
// CGameSettings is the pointer at RVA 0xb2ef68; each player's block is 0x1c bytes apart.
int NetAimInvertAction(uint8_t* a, uint32_t mine) {
    int p = mine % 120;
    if (p > 1) return 0;
    __try {
        BYTE* gs = *(BYTE**)(ExeBase() + 0xb2ef68);
        if (!gs) return 0;
        a[0] = kActAimInvert;
        a[1] = (uint8_t)p;
        memcpy(a + 2, gs + 0x4c + p * 0x1c, 6);
        return kActAimInvertLen;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
// Both PCs, on the frame player 1's action is applied: if it's an aim-invert action, write
// the host's flags into this PC's settings and return true (handled); false means it's a
// debug-menu action for DebugMenu_ApplyAction.
bool NetApplyAimInvert(const NetPad& pad, uint32_t f) {
    if (pad.actLen != kActAimInvertLen || pad.act[0] != kActAimInvert) return false;
    int p = pad.act[1];
    if (p > 1) return true;
    __try {
        BYTE* gs = *(BYTE**)(ExeBase() + 0xb2ef68);
        BYTE* dst = gs ? gs + 0x4c + p * 0x1c : NULL;
        if (dst && memcmp(dst, pad.act + 2, 6)) {
            static int logged;
            if (logged++ < 20)
                Log("aim settings %u: player %d's invert flags %02x%02x%02x%02x%02x%02x -> the host's %02x%02x%02x%02x%02x%02x",
                    f, p + 1, dst[0], dst[1], dst[2], dst[3], dst[4], dst[5],
                    pad.act[2], pad.act[3], pad.act[4], pad.act[5], pad.act[6], pad.act[7]);
            memcpy(dst, pad.act + 2, 6);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return true;
}
// Diagnostic, every running session frame: log the settings block when it changes (at most
// 20 lines), and once whether an "Avatar" input context exists. Read-only.
void NetLogAimSettings(uint32_t f) {
    static uint64_t lastHash;
    static int lines;
    BYTE* b = ExeBase();
    __try {
        BYTE* gs = *(BYTE**)(b + 0xb2ef68);
        if (!gs) return;
        static bool ctxLogged;                           // the decomp session expects NULL: there's no "Avatar" mapper
        if (!ctxLogged && !memcmp(b + 0x76d098, "Avatar", 7)) {
            ctxLogged = true;
            typedef void* (*PFN_GetCtx)(void* mgr, const char* name, int player);
            void* ctx = ((PFN_GetCtx)(b + 0x27d590))(b + 0xac61c0, (const char*)(b + 0x76d098), 0);
            Log("aim settings %u: \"Avatar\" input context for player 1: %p", f, ctx);
        }
        // Log the block (and the window's client size, which the alley-toss aim reads as
        // W/H < 16:9) whenever it changes, so two logs can be compared.
        uint64_t h = 0xcbf29ce484222325ull;
        for (int i = 8; i < 0x70; i++) h = (h ^ gs[i]) * 0x100000001b3ull;
        BYTE* disp = ((BYTE* (*)())(b + 0x2d5fb0))();
        int cw = disp ? *(int*)(disp - 0x1c0 + 0x138) : 0, ch = disp ? *(int*)(disp - 0x1c0 + 0x13c) : 0;
        h = (h ^ (uint64_t)cw) * 0x100000001b3ull;
        h = (h ^ (uint64_t)ch) * 0x100000001b3ull;
        if (h != lastHash && lines < 20) {
            lastHash = h;
            lines++;
            char hex[0x68 * 2 + 1];
            for (int i = 0; i < 0x68; i++) sprintf_s(hex + i * 2, 3, "%02x", gs[8 + i]);
            Log("aim settings %u: window client %dx%d; block +8 %s", f, cw, ch, hex);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Netplay's answer to every game pad poll (from HandleGetState, under g_cs). The anchor poll
// first runs the frame step (NetFrame), which may wait for the other PC's input. Then, by
// phase: running, pads 0 and 1 are this frame's lockstep pads (host, client); ended, this
// player's own pad as pad 0 and pad 1 neutral for a moment, then gone; netplay never opened,
// a plain single-PC game; otherwise (waiting for a friend) see below.
DWORD NetGetState(DWORD user, XINPUT_STATE* st, bool anchor) {
    if (anchor) NetFrame();
    ZeroMemory(st, sizeof *st);
    if (g_netPhase == NP_RUNNING) {
        if (user > 1) return ERROR_DEVICE_NOT_CONNECTED;
        st->Gamepad = g_netCur[user].pad;
        st->dwPacketNumber = g_netCur[user].packet;
        return ERROR_SUCCESS;
    }
    if (g_netPhase == NP_ENDED) {
        if (anchor && g_netRemoveP2) { g_netRemoveP2 = false; NetRemovePlayer2(); }
        if (anchor && g_netEndNeutralFrames > 0) g_netEndNeutralFrames--;
        if (user == 0) {                                // back to this player's own controller
            NetPad p = NetReadLocal(g_frame, anchor);
            // Back + Start rejoins: while Back is held, Start doesn't reach the game (no pause menu).
            if (p.pad.wButtons & XINPUT_GAMEPAD_BACK) p.pad.wButtons &= ~XINPUT_GAMEPAD_START;
            st->Gamepad = p.pad;
            st->dwPacketNumber = p.packet;
            return ERROR_SUCCESS;
        }
        if (user == 1 && g_netEndNeutralFrames > 0) return ERROR_SUCCESS;   // neutral first: a failed poll keeps the last state
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    // Netplay couldn't open at all: play as an ordinary single-PC game.
    if (g_netPhase == NP_OFF && g_netTried) {
        DWORD r = XInputGetState(user, st);
        if (r != ERROR_SUCCESS) ZeroMemory(st, sizeof *st);
        DebugMenu_FilterPad(user, st, anchor);
        return r;
    }
    // Before lockstep: this player plays on alone while waiting for a friend,
    // until they're ready. From then on the pad is neutral, so the game stays on
    // "Press Start" (where every session begins) while the other player gets ready.
    if (user == 0 && !g_netLocalReady && !g_net.testInputSeed && !(g_dropIn && (g_dropStep != DROP_NONE || g_net.role == 1))) {
        NetPad p = NetReadLocal(g_frame, false);       // NetCheckLocalPad gives the menu its frame tick here
        if (g_feHooked && OnPressStart() && (p.pad.wButtons & XINPUT_GAMEPAD_BACK)) p.pad.wButtons &= ~XINPUT_GAMEPAD_START;
        st->Gamepad = p.pad;
        st->dwPacketNumber = p.packet;
        return ERROR_SUCCESS;
    }
    return user == 0 ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

// Which pads exist, from the game's point of view: pad 0 always; pad 1 while lockstep runs
// (and for the neutral frames after it ends). If netplay couldn't open, the real answer.
DWORD NetGetCaps(DWORD user, XINPUT_CAPABILITIES* caps) {
    if (g_netPhase == NP_OFF && g_netTried) return XInputGetCapabilities(user, XINPUT_FLAG_GAMEPAD, caps);
    bool connected = user == 0 || (user == 1 && (g_netPhase == NP_RUNNING || (g_netPhase == NP_ENDED && g_netEndNeutralFrames > 0)));
    if (!connected || !caps) return ERROR_DEVICE_NOT_CONNECTED;
    // Identical on both PCs, whatever controllers the players own.
    ZeroMemory(caps, sizeof *caps);
    caps->Type = XINPUT_DEVTYPE_GAMEPAD;
    caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    caps->Gamepad.wButtons = 0xF3FF;
    caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 0xFF;
    caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY = (SHORT)0xFFC0;
    caps->Vibration.wLeftMotorSpeed = caps->Vibration.wRightMotorSpeed = 0xFF;
    return ERROR_SUCCESS;
}

// Rumble for pad 0 belongs to the host, pad 1 to the client: each PC only
// rumbles its own player's controller.
DWORD NetSetState(DWORD user, XINPUT_VIBRATION* v) {
    if (g_netPhase == NP_OFF && g_netTried) return XInputSetState(user, v);
    bool mine = g_netPhase == NP_RUNNING ? (int)user == g_net.role : user == 0;
    if (!mine) return ERROR_SUCCESS;
    g_netRumbling = false;                            // the game owns the motors now
    return XInputSetState((DWORD)g_net.localPad, v);
}

// ------------------------------------------------------------------ netplay: keyboard and mouse
//
// Only controller input goes through lockstep. Keyboard and mouse would
// reach just one PC (and make the game rebind players to the keyboard), so
// in netplay they're blocked at the source: DirectInput keyboard and mouse
// read as idle, DirectInput pads aren't listed, the cursor stands still and
// GetKeyState reports nothing held.

// Block now? Yes in netplay mode, from start-up (before a session) until a session ends;
// no once it has ended or if netplay couldn't open.
static bool NetBlocksDesktop() {
    if (g_cfg.mode != MODE_NETPLAY) return false;
    if (g_netPhase == NP_ENDED) return false;
    if (g_netPhase == NP_OFF && g_netTried) return false;          // netplay never started
    return true;
}

// DirectInput objects are COM objects: their methods are called through a vtable (a table of
// function pointers the object points to). These hooks overwrite vtable slots and keep the
// originals here, keyed by vtable and slot, so a hook can find the original for whichever
// object called it.
struct VHook { void** vt; int slot; void* orig; };
static VHook g_vh[16];
static int g_vhN;
static SRWLOCK g_vhLock = SRWLOCK_INIT;
// The original method in obj's vtable slot, as it was before VPatch (NULL if never patched).
static void* VOrig(void* obj, int slot) {
    void** vt = *(void***)obj;
    AcquireSRWLockShared(&g_vhLock);
    void* o = NULL;
    for (int i = 0; i < g_vhN; i++) if (g_vh[i].vt == vt && g_vh[i].slot == slot) { o = g_vh[i].orig; break; }
    ReleaseSRWLockShared(&g_vhLock);
    return o;
}
// Point obj's vtable slot at hook, once per vtable and slot (up to 16), saving the original.
static void VPatch(void* obj, int slot, void* hook) {
    void** vt = *(void***)obj;
    AcquireSRWLockExclusive(&g_vhLock);
    bool known = false;
    for (int i = 0; i < g_vhN; i++) if (g_vh[i].vt == vt && g_vh[i].slot == slot) known = true;
    if (!known && g_vhN < 16 && vt[slot] != hook) {
        g_vh[g_vhN++] = { vt, slot, vt[slot] };
        DWORD old;
        if (VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &old)) {
            vt[slot] = hook;
            VirtualProtect(&vt[slot], sizeof(void*), old, &old);
        }
    }
    ReleaseSRWLockExclusive(&g_vhLock);
}

typedef HRESULT (STDMETHODCALLTYPE* PFN_DIGetState)(void*, DWORD, void*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_DIGetData)(void*, DWORD, void*, DWORD*, DWORD);
typedef HRESULT (STDMETHODCALLTYPE* PFN_DICreateDevice)(void*, const GUID&, void**, void*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_DIEnum)(void*, DWORD, void*, void*, DWORD);
typedef HRESULT (WINAPI* PFN_DI8Create)(HINSTANCE, DWORD, const IID&, void**, void*);
static PFN_DI8Create Real_DI8Create;
static volatile LONG g_desktopBlockedLogged, g_padsHiddenLogged;

// IDirectInputDevice8::GetDeviceState (slot 9): while blocking, the device reads as idle.
static HRESULT STDMETHODCALLTYPE H_DIGetState(void* self, DWORD cb, void* data) {
    PFN_DIGetState o = (PFN_DIGetState)VOrig(self, 9);
    HRESULT hr = o ? o(self, cb, data) : E_FAIL;
    if (data && NetBlocksDesktop()) {
        memset(data, 0, cb);
        hr = S_OK;
        if (!InterlockedExchange(&g_desktopBlockedLogged, 1)) Log("netplay: keyboard/mouse input is blocked while online");
    }
    return hr;
}
// IDirectInputDevice8::GetDeviceData (slot 10): while blocking, no buffered events.
static HRESULT STDMETHODCALLTYPE H_DIGetData(void* self, DWORD cbObj, void* rgdod, DWORD* inOut, DWORD flags) {
    PFN_DIGetData o = (PFN_DIGetData)VOrig(self, 10);
    HRESULT hr = o ? o(self, cbObj, rgdod, inOut, flags) : E_FAIL;
    if (inOut && NetBlocksDesktop()) { *inOut = 0; hr = S_OK; }
    return hr;
}
// IDirectInput8::CreateDevice (slot 3): hook each new device's two read methods.
static HRESULT STDMETHODCALLTYPE H_DICreateDevice(void* self, const GUID& g, void** dev, void* outer) {
    PFN_DICreateDevice o = (PFN_DICreateDevice)VOrig(self, 3);
    HRESULT hr = o ? o(self, g, dev, outer) : E_FAIL;
    if (SUCCEEDED(hr) && dev && *dev) {
        VPatch(*dev, 9, (void*)H_DIGetState);
        VPatch(*dev, 10, (void*)H_DIGetData);
    }
    return hr;
}
// IDirectInput8::EnumDevices (slot 4): while blocking, list no game controllers.
static HRESULT STDMETHODCALLTYPE H_DIEnum(void* self, DWORD type, void* cb, void* ref, DWORD flags) {
    // DI8DEVCLASS_GAMECTRL (4) or a game-controller device type (0x14..0x18)
    if (NetBlocksDesktop() && (type == 4 || (type >= 0x14 && type <= 0x18))) {
        if (!InterlockedExchange(&g_padsHiddenLogged, 1))
            Log("netplay: DirectInput controllers hidden from the game (only XInput / Steam Input pads are synced)");
        return S_OK;
    }
    PFN_DIEnum o = (PFN_DIEnum)VOrig(self, 4);
    return o ? o(self, type, cb, ref, flags) : E_FAIL;
}
// DirectInput8Create, through the game's import: hook the DirectInput object it returns.
static HRESULT WINAPI H_DI8Create(HINSTANCE inst, DWORD ver, const IID& riid, void** out, void* outer) {
    HRESULT hr = Real_DI8Create(inst, ver, riid, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        VPatch(*out, 3, (void*)H_DICreateDevice);
        VPatch(*out, 4, (void*)H_DIEnum);
    }
    return hr;
}

typedef BOOL (WINAPI* PFN_GetCursorPos)(LPPOINT);
typedef BOOL (WINAPI* PFN_SetCursorPos)(int, int);
typedef SHORT (WINAPI* PFN_GetKeyState)(int);
static PFN_GetCursorPos Real_GetCursorPos;
static PFN_SetCursorPos Real_SetCursorPos;
static PFN_GetKeyState Real_GetKeyState;
static POINT g_cursorHold;
static bool g_cursorHoldSet;
// The cursor, as the game sees it while blocking: held where it was, or where the game last
// put it, so the mouse can't turn the camera on one PC only.
static BOOL WINAPI H_GetCursorPos(LPPOINT p) {
    if (!NetBlocksDesktop() || !p) return Real_GetCursorPos(p);
    if (!g_cursorHoldSet) {                                // first read: hold it where it is
        Real_GetCursorPos(&g_cursorHold);
        g_cursorHoldSet = true;
    }
    *p = g_cursorHold;
    return TRUE;
}
static BOOL WINAPI H_SetCursorPos(int x, int y) {
    g_cursorHold.x = x;                                    // the game's own warps (mouse look) stand still
    g_cursorHold.y = y;
    g_cursorHoldSet = true;
    return Real_SetCursorPos(x, y);
}
// GetKeyState: while blocking, no key reads as held.
static SHORT WINAPI H_GetKeyState(int vk) { return NetBlocksDesktop() ? 0 : Real_GetKeyState(vk); }

// From DllMain in netplay mode: hook DirectInput8Create and the cursor and key-state calls in
// the game's import table (PatchIAT). DirectInput is hooked only if dinput8.dll is already
// loaded by then.
void InstallDesktopBlock() {
    HMODULE exe = GetModuleHandleW(NULL);
    int n = 0;
    if (HMODULE di = GetModuleHandleW(L"dinput8.dll"))
        if ((Real_DI8Create = (PFN_DI8Create)GetProcAddress(di, "DirectInput8Create")))
            n += PatchIAT(exe, "DirectInput8Create", (void*)H_DI8Create);
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        Real_GetCursorPos = (PFN_GetCursorPos)GetProcAddress(u32, "GetCursorPos");
        Real_SetCursorPos = (PFN_SetCursorPos)GetProcAddress(u32, "SetCursorPos");
        Real_GetKeyState = (PFN_GetKeyState)GetProcAddress(u32, "GetKeyState");
        if (Real_GetCursorPos) n += PatchIAT(exe, "GetCursorPos", (void*)H_GetCursorPos);
        if (Real_SetCursorPos) n += PatchIAT(exe, "SetCursorPos", (void*)H_SetCursorPos);
        if (Real_GetKeyState) n += PatchIAT(exe, "GetKeyState", (void*)H_GetKeyState);
    }
    Log("netplay: keyboard/mouse block installed (%d imports hooked)", n);
}

// Say goodbye as the game quits, while networking still works: by the time
// this DLL is unloaded, Windows has already unloaded the socket helpers and
// the game has shut Steam down.
static void NetGoodbye() {
    if (!g_netT || g_netPhase == NP_OFF || g_netPhase == NP_ENDED) return;
    NetLockGuard lock;
    // Our last inputs first (they may still be queued), then the goodbye.
    g_netT->Flush();
    if (g_netPhase == NP_RUNNING && g_netLocalSentUpTo != 0xFFFFFFFF) { NetSendInputs(g_netLocalSentUpTo); g_netT->Flush(); }
    NetByePkt bye = { NetHeader(NET_BYE), g_netLocalSentUpTo };
    for (int i = 0; i < 3; i++) g_netT->SendNow(&bye, sizeof bye);
    Log("netplay: this player quit at frame %u; told the other player", g_netFrame);
    strncpy_s(g_netEndReason, "this player quit", _TRUNCATE);
    g_netPhase = NP_ENDED;
}
typedef void (WINAPI* PFN_ExitProcess)(UINT);
typedef void (*PFN_SteamShutdown)();
static PFN_ExitProcess Real_ExitProcess;
static PFN_SteamShutdown Real_SteamShutdown;
// Both say goodbye first, then do what the game asked.
static void WINAPI H_ExitProcess(UINT code) { NetGoodbye(); Real_ExitProcess(code); }
static void H_SteamShutdown() { NetGoodbye(); if (Real_SteamShutdown) Real_SteamShutdown(); }
// From NetOpen: hook ExitProcess (in the game's imports and the C runtime's, which exit()
// goes through) and SteamAPI_Shutdown.
void InstallNetExitHooks() {
    Real_ExitProcess = (PFN_ExitProcess)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ExitProcess");
    int n = 0;
    if (Real_ExitProcess) {
        n += PatchIAT(GetModuleHandleW(NULL), "ExitProcess", (void*)H_ExitProcess);
        if (HMODULE crt = GetModuleHandleW(L"ucrtbase.dll")) n += PatchIAT(crt, "ExitProcess", (void*)H_ExitProcess);
    }
    if (HMODULE st = GetModuleHandleW(L"steam_api64.dll")) {
        Real_SteamShutdown = (PFN_SteamShutdown)GetProcAddress(st, "SteamAPI_Shutdown");
        if (Real_SteamShutdown) n += PatchIAT(GetModuleHandleW(NULL), "SteamAPI_Shutdown", (void*)H_SteamShutdown);
    }
    Log("netplay: %d quit hooks installed (goodbye is sent when the game exits or shuts Steam down)", n);
}
