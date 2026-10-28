// proxy_shared.h -- what the harness modules (src\*.cpp except gfx_d3d12.cpp and debug_menu.cpp)
// share: the system includes, the types and constants more than one module uses, extern
// declarations of the shared globals and prototypes of the shared functions, grouped by the
// module that defines them. Include it first in every module. Declarations only: no function
// bodies, no mutable statics.
//
// The whole harness is one DLL that the game loads as xinput1_4.dll (see xinput_proxy.cpp).
// Code runs from three kinds of place: DllMain at load (reads the ini, patches the game),
// the game's own input poll (every XInputGetState call; each poll of the "anchor" pad, the
// first one the game ever asked for, starts a new frame, so this is the per-frame hook), and
// hooks planted in the game's code; plus a few threads of its own (the status banner, the
// netplay keepalive and load profiler, the vocal theme's file check). The modules:
//   xinput_proxy.cpp      DllMain, the ini, every export, which mode handles each pad poll
//   harness_core.cpp      Log(), frame counting, record/replay, the loading detector, the
//                         virtual QueryPerformanceCounter clock
//   patch_util.cpp        verify-then-patch helpers for the game's code (BytesAt, RedirectCall...)
//   sim_determinism.cpp   one-PC determinism: engine clock, engine RNG, rand() isolation, spawn gate
//   graphics_patches.cpp  [graphics] patches to the game's own render code
//   levels_scripts.cpp    restored levels, script fixes, vocal theme
//   status_overlay.cpp    the on-screen status banner
//   net_transport.cpp     the netplay connection: UDP and Steam transports, opening a session
//   net_session.cpp       netplay session state, packet handling, pad routing during a session
//   net_lockstep.cpp      the per-frame lockstep driver, fingerprint check and repair
//   net_dropin.cpp        drop-in: the save transfer and the host's level reload
//   net_determinism.cpp   fixes that keep two PCs' simulations identical
//   net_own_screen.cpp    own screen, level publish and movie sync, HUD
//   local_test.cpp, test_save.cpp   the one-PC co-op test and the separate test save
//
// Hex numbers like 0x14xxxxxxx are addresses in the game's exe, found by reverse engineering
// it (the "decomp session"; FUN_/DAT_ names are the decompiler's). Those addresses assume the
// exe's preferred base, 0x140000000, so an RVA (offset from the exe's base) is the address
// minus that, and ExeBase() + RVA finds it wherever Windows actually loaded the exe. Every
// patch first checks the bytes it expects there (BytesAt), so a different game build is left
// alone rather than patched in the wrong place.
//
// Netplay words used throughout:
//   lockstep       both PCs run the whole game and only pad input crosses the network; the
//                  game's frame f waits until both players' input for f is here, so two
//                  identical simulations stay identical. A "lockstep frame" is one such step.
//   fingerprint    a hash. The settings fingerprint (in every packet header) must match to
//                  connect; the state fingerprint (every hash_interval frames) catches a desync.
//   settings sheet the "key=value" text the settings fingerprint is a hash of; swapped on a
//                  mismatch so each PC can say which setting differs (net_session.cpp).
//   drop-in        a friend joins while the host is in a level: the host's save is sent over
//                  and both PCs reload that level together (net_dropin.cpp).
//   publish        a level finishing loading (the game's level publish, FUN_1405d81a0, which
//                  applies the save and posts LoadedSavedGame; hooked in net_own_screen.cpp).
//   repair         in-place desync repair: the client copies the host's values that drifted.
//   load gate      after a publish, both PCs stay paused until both have finished loading.
//   own screen     each PC shows only its own player, full screen, instead of split screen.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>

// We define the XInput entry points ourselves, so hide the SDK's declarations
// (they carry a different exception specification under /EHsc).
#define XInputGetState               sdk_XInputGetState
#define XInputSetState               sdk_XInputSetState
#define XInputGetCapabilities        sdk_XInputGetCapabilities
#define XInputEnable                 sdk_XInputEnable
#define XInputGetBatteryInformation  sdk_XInputGetBatteryInformation
#define XInputGetKeystroke           sdk_XInputGetKeystroke
#define XInputGetAudioDeviceIds      sdk_XInputGetAudioDeviceIds
#include <xinput.h>
#undef XInputGetState
#undef XInputSetState
#undef XInputGetCapabilities
#undef XInputEnable
#undef XInputGetBatteryInformation
#undef XInputGetKeystroke
#undef XInputGetAudioDeviceIds

// The named XInput exports are plain pass-throughs to Windows' xinput1_4.dll
// that Steam Input hooks by name; the proxy reads and rumbles the local pad
// through them (see "Named exports" at the end of xinput_proxy.cpp).
extern "C" DWORD WINAPI XInputGetState(DWORD dwUserIndex, XINPUT_STATE* pState);
extern "C" DWORD WINAPI XInputSetState(DWORD dwUserIndex, XINPUT_VIBRATION* pVibration);
extern "C" DWORD WINAPI XInputGetCapabilities(DWORD dwUserIndex, DWORD dwFlags, XINPUT_CAPABILITIES* pCaps);
#include <stdio.h>
#include <share.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <timeapi.h>
#include <intrin.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <vector>
#include <algorithm>
#include <ctype.h>
#include <unordered_map>
#include <wctype.h>

#include <bcrypt.h>

#pragma warning(disable : 4995) // XInputEnable deprecated

// ================================================================== shared types and constants

// [harness] mode: what the proxy does with the game's pad polls. Passthrough hands them to
// Windows untouched; record writes every poll to input.bin; replay feeds a recording back;
// netplay is online co-op (each PC's pad crosses the network, see net_transport.cpp).
enum Mode { MODE_PASSTHROUGH = 0, MODE_RECORD = 1, MODE_REPLAY = 2, MODE_NETPLAY = 3 };

struct NetPad;

// The [harness] ini settings (LoadConfig in xinput_proxy.cpp reads them into g_cfg). The
// command line can override some: a Steam join or the one-PC co-op test forces netplay.
struct Config {
    Mode    mode            = MODE_PASSTHROUGH;
    wchar_t input[64]       = L"run1";
    wchar_t output[64]      = L"";
    int     fixedTimestep   = 0;
    double  fps             = 60.0;
    int     freezeWallclock = 0;
    int     shotInterval    = 300;
    int     shotMax         = 200;
    int     logCalls        = 0;

    // Loading detection (see BeginFrame / UpdateDetector).
    int     ioLog           = 1;     // write io.csv: per-frame file I/O
    int     loadDetect      = 1;     // pause the input stream while loading
    int     loadWindow      = 8;     // frames summed for the "enter" test
    int     loadEnterKb     = 2048;  // KB read within loadWindow frames -> loading
    int     loadQuietKb     = 16;    // a frame reading less than this is "quiet"
    int     loadQuietFrames = 30;    // this many quiet frames in a row -> load over
    int     loadHitchMs     = 0;     // a frame taking this long counts as loading (0 = off)
    int     loadWaitFrames  = 3600;  // replay: give up waiting for a load after this
    int     useSrc[4]       = { 1, 1, 0, 0 };  // which readers count: exe, crt, bink, fmod

    // Save games: record snapshots the save folder; replay restores that
    // snapshot so every run starts from the same progress (coins, items).
    int     saveRestore     = 1;
    int     testSave        = 0;   // [harness] test_save: a separate save folder, achievements off
    int     syncLoader      = 0;  // loading finishes inside the frame, whatever the PC's speed
    int     audioWait       = 0;   // scripts wait a fixed number of frames for voice lines

    // fixed_timestep implementation. 1 = drive the engine's own clock
    // override (falls back to virtualising QueryPerformanceCounter if the
    // game code doesn't match); 0 = always virtualise QueryPerformanceCounter.
    int     engineClock     = 1;
    // 1 = every rand() call site in the game gets its own random stream, so
    // one caller (e.g. audio) using more numbers can't shift another's.
    int     randIsolate     = 1;
    // 1 = trace every draw from the engine's Mersenne Twister (diagnostic;
    // doesn't change what the game does).
    int     mtTrace         = 1;
    // 1 = spawns are placed exactly spawnDelay frames after they're queued
    // (record/replay only; see the spawn gate section).
    int     spawnGate       = 1;
    int     spawnDelay      = 15;
    wchar_t saveDir[MAX_PATH] = L"";
};

// A recording, input.bin in the run folder: one FileHeader, then one Rec per XInputGetState
// call the game made, with a KIND_LOAD marker Rec after each load (OpenRun writes the header,
// HandleGetState the polls, OnLoadEnd the markers). Replay checks
// the header's flags against its own settings, since a run recorded with a different clock
// or RNG setup can't be reproduced.
#pragma pack(push, 1)
struct FileHeader {
    char     magic[4];     // "XIH1"
    uint32_t version;      // 1
    uint32_t recSize;      // sizeof(Rec)
    uint32_t flags;        // bit0 fixed_timestep, bit1 freeze_wallclock, bit2 load_detect, bit3 and bit5
                           // engine clock, bit4 rand_isolate, bit7 spawn gate
    double   fps;
    uint8_t  reserved[8];  // with the spawn gate on, the first two bytes hold spawn_delay
};
// One recorded pad poll.
struct Rec {
    uint32_t     seq;      // call index within the run
    uint32_t     frame;    // logical frame: counts only frames outside loads
    uint8_t      kind;     // 0 = XInputGetState, 1 = XInputGetStateEx, KIND_LOAD = marker
    uint8_t      user;     // dwUserIndex
    uint16_t     flags;    // REC_LOADING: polled while a load was in progress
    uint32_t     result;   // return value of the real call
    XINPUT_STATE state;
};
#pragma pack(pop)
static_assert(sizeof(FileHeader) == 32, "header size");
static_assert(sizeof(Rec) == 32, "record size");

enum { KIND_LOAD = 0xF0, REC_LOADING = 1 };

// The engine clock's reading (ms) at virtual time 0: published game time is kStartMs + elapsed.
static const uint32_t kStartMs      = 100000;

// The engine's Mersenne Twister (MT19937) random-number generator: its refill function,
// called when an instance has used up its block of numbers (sim_determinism.cpp).
typedef void (*PFN_MTRefill)(void* inst);

// Who drew from the engine's global RNG in each lockstep frame, for netplay's
// per-frame RNG check: when the two PCs' random numbers part ways at frame f,
// each PC logs its own callers for f, and the two logs side by side show the
// extra draw. Filled under g_mtLock; RngStep closes each frame's record at the
// lockstep point (frame = lockstep frame + 1, 0 = open/unused).
// One draw: the RVAs of the code that called the RNG (r0) and, when that was a small helper,
// of the game function above it (r1, 0 if none); main = drawn on the game's main thread.
struct RngDrawRec { uint32_t r0, r1; uint8_t main; };
struct RngFrameDraws { uint32_t frame, n; RngDrawRec d[32]; };
static const int kRngDrawFrames = 64;

static const uint32_t kMtEngineSeed = 0x5e5450;     // the seed MT19937_CreateGlobal uses
typedef void (*PFN_MTSeed)(void*, uint32_t);

// One rand() call site in the game (rand_isolate, sim_determinism.cpp): the return address
// that identifies it, the state of its own random stream, and how many calls it made from
// the main thread and from other threads.
struct RandSite { uintptr_t ret; LONG state; volatile LONG mainCalls, otherCalls; };

// The [graphics] ini settings (read in LoadConfig, applied by graphics_patches.cpp). The
// has* flags say a value was given; without one the game's own value stays.
struct GfxConfig {
    int    enabled     = 0;
    double fpsLimit    = 0;      // 0 = leave the game's 60, > 0 = new cap, < 0 = no cap
    bool   hasCullPop  = false, hasCullFade = false;
    double cullPop     = 15.0;   // main view: objects smaller than this are dropped
    double cullFade    = 25.0;   //   ... and faded in up to this size
    bool   hasShadowCull[3] = {};
    double shadowCull[3] = { 5.0, 15.0, 25.0 };   // same, for shadow casters, per cascade
    double lodScale    = 1.0;    // multiplier on actor LOD distances
    double objectFadeScale = 1.0;  // multiplier on per-model fade-out distances
    double worldLodScale = 1.0;  // multiplier on world-chunk LOD swap distances
    int    fullResReflection = 0;
    int    fullResGlow = 0;
    int    shadowAllZones = 0;   // shadow cascades test every visibility zone, not just the camera's
    int    shadowNoContrib = 0;  // no size (contribution) culling of shadow casters
    int    shadowCasterLog = 0;  // diagnostic: log each cascade's caster count when it changes
    double shadowNearExtend = 0; // metres each sun cascade's depth range is extended toward the sun
    double shadowFar = 0, shadowCascades = 0, shadowSplit = 0;   // read by gfx_d3d12.cpp; here only for the fingerprint
};

// A spawn the spawn gate is tracking (sim_determinism.cpp): the game's queued spawn task and
// its actor, the frame it was first seen in the queue (stamp), its queue-order number (seq),
// whether the slot is in use, and whether the task was still in the queue this frame.
struct SpawnSlot { uint8_t* task; void* actor; uint64_t stamp; uint32_t seq; bool used, live; };

// Netplay packet types (NetHdr::type) and protocol constants. Two PCs only talk when their
// kNetMagic, kNetVersion and settings fingerprint all match.
enum { NET_HELLO = 1, NET_READY = 2, NET_INPUT = 3, NET_HASH = 4, NET_BYE = 5, NET_MOVIE = 9, NET_BUSY = 10 };   // 6-8: save transfer
static const uint32_t kNetMagic = 0x4E335354;   // "TS3N"
static const uint16_t kNetVersion = 5;              // 2: start values in READY/INPUT; 3: a debug-menu action per input frame;
                                                    // 4: per-frame RNG tags in INPUT; 5: per-frame flags (load gate)
static const unsigned kYeeHawBuild = 104;            // bump every build: the settings sheet's "build" line, so two builds never connect
// Inputs and fingerprints are kept in rings of kNetRing frames (index frame % kNetRing); an
// input packet carries the newest kNetRedundancy frames, so the next packet covers a lost one.
static const int kNetRing = 256, kNetRedundancy = 8;

#pragma pack(push, 1)

// One frame of a player's input, plus at most one debug-menu action (player 1's
// menu, applied on both PCs on this frame; travels with the input, so it can't
// arrive without it).
// flags bit 0: this PC's level load has reached its load-complete point (the load gate).
struct NetPad { XINPUT_GAMEPAD pad; uint32_t packet; uint8_t actLen; uint8_t act[12]; uint8_t flags; };   // 30 bytes
// The start of every packet: kNetMagic, the packet type, the sender's role (0 host, 1 client;
// a packet claiming our own role is ignored), kNetVersion and the settings fingerprint.
struct NetHdr { uint32_t magic; uint8_t type, role; uint16_t version; uint32_t settings; };

// Per-frame RNG check (see RngStep): a hash of the random-number state at a lockstep
// frame and how many engine RNG draws that frame made. The newest few ride along with
// every input packet (frames rngLast - rngCount + 1 .. rngLast).
struct NetRngTag { uint32_t hash; uint16_t draws; };
static const int kNetRngTags = 4;
// INPUT: the sender's pads for frames lastFrame - count + 1 .. lastFrame, its newest state
// fingerprint (hashFrame 0 = none) and RNG tags, and this session's start values (startVus
// 0 = not known yet), which a client that doesn't have them yet adopts from the host's.
struct NetInputPkt { NetHdr h; uint32_t hashFrame; uint64_t hash; uint64_t startVus, startKeys; uint32_t lastFrame;
                     uint32_t rngLast; uint8_t rngCount; NetRngTag rng[kNetRngTags];
                     uint8_t count; NetPad pads[kNetRedundancy]; };

// READY: this PC's game time and entity-key counter; once the host has started,
// also the start values both PCs use (the host picks them, the client adopts them).
struct NetReadyPkt { NetHdr h; uint64_t vus, keys; uint8_t started; uint64_t startVus, startKeys; };
struct NetHashPkt { NetHdr h; uint32_t frame; uint64_t hash; };
struct NetByePkt { NetHdr h; uint32_t lastFrame; };   // last frame we sent input for (0xFFFFFFFF: none)
struct NetMoviePkt { NetHdr h; uint32_t idx; uint8_t skip; };   // "watching movie idx" keepalive; skip = 1: skip it
#pragma pack(pop)

// The [netplay] ini settings (LoadConfig in xinput_proxy.cpp reads them into g_net).
struct NetCfg {
    int      role = 0;                 // 0 host (player 1), 1 client (player 2)
    int      transport = 0;            // 0 udp, 1 steam
    int      localPort = 47001;
    wchar_t  peerHost[64] = L"127.0.0.1";
    int      peerPort = 47002;
    uint64_t peerSteamId = 0;
    int      delay = 4;
    int      timeoutMs = 10000;
    int      startImmediate = 0;      // 1 = start as soon as both say hello (tests)
    int      hashInterval = 60;
    int      repair = 1;               // in-place desync repair: the client copies the host's drifted state
    int      repairLead = 30;          // frames between finding a difference and copying it
    int      testDesyncKeys = 0;       // 1: Ctrl+F9 / Ctrl+F10 knock this PC slightly out of step (to see the repair work)
    int      repairFollowers = 0;      // 1: also copy kinematic bodies and actor transforms on a real desync (experimental)
    int      rngCheck = 1;             // compare the random-number state every frame (catches an extra draw within a few frames)
    int      rngCatchUp = 1;           // the PC that drew fewer engine random numbers steps forward itself, at once
    int      localPad = 0;             // which real XInput pad is this player's controller
    int      simLatencyMs = 0, simLossPct = 0;   // UDP test transport only
    uint32_t testInputSeed = 0;        // non-zero: synthesize local input (tests)
    int      testRejoinMs = 0;         // tests: rejoin this long after a session ends (no title screen in stand-ins)
    int      dropIn = 1;               // a friend joining while the host is in a level reloads it for both
    int      testDrop = 0;             // tests: host fakes a captured save (stand-ins have no game to save)
    int      testActionFrame = 0;      // tests: player 1 sends a dummy debug-menu action on this frame
    int      ownScreen = 0;            // each PC shows only its own player, full screen (off: changes camera streaming)
    int      skipHidden = 1;           // own screen: the other player's view builds but doesn't render (GPU)
};

// Accent colours for the status banner (Status() in status_overlay.cpp): blue for progress
// and hints, green for success, amber for a warning, red for a failure.
static const COLORREF kOvBlue = RGB(80, 160, 255), kOvGreen = RGB(90, 210, 110), kOvAmber = RGB(255, 190, 60),
                      kOvRed = RGB(240, 90, 80);

// How packets reach the other PC: plain UDP (tests, the one-PC co-op test) or Steam's
// networking (online play). Both are in net_transport.cpp; the rest of netplay only sees this.
struct NetTransport {
    virtual ~NetTransport() {}
    virtual bool Open() = 0;
    virtual void Send(const void* data, int len) = 0;
    virtual int  Recv(void* buf, int cap) = 0;       // 0 = nothing waiting
    virtual void Pump() {}
    virtual void SendNow(const void* data, int len) { Send(data, len); }   // skip any queue
    virtual void Flush() {}                                                  // send anything queued, now
    virtual void SendReliable(const void* data, int len) { Send(data, len); }   // the save transfer resends anyway
    virtual const char* Name() = 0;
    virtual int  LinkState(char* why, size_t cap) { if (why && cap) why[0] = 0; return -1; }   // Steam only
    virtual void RelayStatus(char* out, size_t cap) { if (out && cap) out[0] = 0; }               // Steam only
    virtual void SetPeer(uint64_t id) {}                                                           // Steam only
    virtual void SessionStarted() {}                                                               // Steam only
    virtual void Advertise() {}                       // Steam only: friends see "Join Game" on this host again
    virtual bool OpenInvite() { return false; }       // Steam only: the overlay's "invite a friend" dialog
};

// Where the session is: off (not opened yet, or it couldn't open), connecting (saying HELLO
// until the other PC answers), waiting for both players to be ready, running lockstep, or
// ended (back to playing alone; a new session can start from the title screen).
enum NetPhase { NP_OFF, NP_CONNECTING, NP_WAITSTART, NP_RUNNING, NP_ENDED };

// Drop-in: the host is in a level when a friend connects. The host saves, sends
// its in-memory save buffer, returns to the title, and on lockstep frame
// kDropLaunchFrame both PCs install that buffer and load the saved world.
enum DropStep { DROP_NONE, DROP_SAVE, DROP_WAITSAVE, DROP_QUIT, DROP_WAITTITLE, DROP_SEND };
static const uint32_t kDropLaunchFrame = 30;

// Load gate (see the session state in net_session.cpp): the game's load-complete counter,
// its display object (pause bit, front-end flag), and how long a PC's streaming must stay
// idle, at most waited for, before it says it's loaded.
// The two addresses are RVAs of game globals (counter at 0x140b2ce48, pointer at 0x140ac66a0).
static const DWORD kLoadDoneCounter = 0xb2ce48;   // DAT_140b2ce48, ++ per vf24 call below 4
static const DWORD kDisplayPtr = 0xac66a0;        // g_Display*
static const uint32_t kStreamIdleFrames = 30, kStreamWaitMax = 1800;

// The network lock (defined in net_session.cpp): held by all game-thread network work, so the
// keepalive thread can tell "busy in the network code" from "stuck in a long load". The
// guard takes it for the rest of a scope.
CRITICAL_SECTION* NetLockObj();
struct NetLockGuard {
    NetLockGuard() { EnterCriticalSection(NetLockObj()); }
    ~NetLockGuard() { LeaveCriticalSection(NetLockObj()); }
};

// More packet types: the save transfer (net_dropin.cpp) and the in-place repair
// (net_lockstep.cpp), and how often the per-frame RNG check asks for a repair.
enum { NET_SAVE_INFO = 6, NET_SAVE_CHUNK = 7, NET_SAVE_ACK = 8 };
enum { NET_RDIGEST = 11, NET_RREQ = 12, NET_RVAL = 13, NET_RNAK = 14, NET_RMISSED = 15 };
static const int kRngMaxAsks = 3;                                     // per slip; then the digest takes over

// A voice line's frame-rule wait (net_determinism.cpp).
struct VoiceWait { int handle; uint32_t lastQuery; uint64_t start; int frames; bool used; };

// The game's save apply (test_save.cpp) and the save folder lookup it redirects (test_save.cpp,
// local_test.cpp).
typedef void (*PFN_SaveApply)(void* self, char a2, void* a3, void* a4);
typedef HRESULT (WINAPI* PFN_SHGetFolderPathA)(HWND, int, HANDLE, DWORD, LPSTR);

// ================================================================== shared globals, by owning module
//
// Each is defined (and usually commented) in the module named above its group.

// harness_core.cpp
extern Config g_cfg;
extern wchar_t g_exeDir[MAX_PATH];
extern wchar_t g_inDir[MAX_PATH];
extern wchar_t g_outDir[MAX_PATH];
extern FILE* g_log;
extern FILE* g_framesCsv;
extern CRITICAL_SECTION g_cs;
extern FILE* g_recFile;
extern size_t g_replayCount;
extern size_t g_replayPos;
extern bool g_userSeen[4];
extern XINPUT_STATE g_lastState[4];
extern DWORD g_lastResult[4];
extern uint32_t g_seq;
extern uint32_t g_frame;
extern int g_anchorKind;
extern int g_anchorUser;
extern uint32_t g_mismatches;
extern uint32_t g_firstMismatchFrame;
extern bool g_clockHooked;
extern volatile LONG64 g_ioReads[4];
extern FILE* g_ioCsv;
extern LONGLONG g_realFreq;
extern uint32_t g_logical;
extern bool g_loading;
extern bool g_frameLive;
extern uint32_t g_loadsCompleted, g_loadsConsumed, g_loadTimeouts, g_unmatchedLoads;
extern volatile bool g_protectSave;
extern int g_shotsTaken;
extern LONGLONG g_freq;
extern int g_hooksInstalled;
extern volatile LONG64 g_vt;
extern volatile LONG g_stallAssists;
extern DWORD g_mainThread;

// sim_determinism.cpp
extern bool g_engineClockReady;
extern bool g_spawnGateOn;
extern uint64_t g_engineFrames;
extern LONG64 g_mtMainDraws;
extern DWORD RVA_CLOCK_FN;
extern DWORD RVA_OVR_FLAG;
extern DWORD RVA_OVR_MS;
extern DWORD RVA_TICK_CALL;
extern volatile LONG64 g_vus;
extern LONG64 g_frameStartUs;
extern volatile ULONGLONG g_frameWallStart;
extern volatile bool g_clockLockstep;
extern uintptr_t g_fmvTimerRet;
extern uintptr_t g_fadeClockRet;
extern bool g_fadeOnMain;
extern uintptr_t g_pinnedClockRets[16];
extern int g_pinnedClockN;
extern uint32_t g_readsThisFrame, g_maxReadsPerFrame;
extern DWORD RVA_MT_NEXT;
extern DWORD RVA_MT_REFILL;
extern DWORD RVA_MT_GLOBAL;
extern DWORD RVA_MT_HELPERS_LO;
extern DWORD RVA_MT_HELPERS_HI;
extern PFN_MTRefill g_mtRefill;
extern void* volatile* g_mtGlobalPtr;
extern bool g_mtHooked;
extern LONG64 g_mtGlobalDraws;
extern FILE* g_mtDrawFile;
extern RngFrameDraws g_rngDraws[kRngDrawFrames];
extern int g_rngDrawCur;
extern bool g_rngDrawOn;
extern SRWLOCK g_mtLock;
extern uint8_t* g_uiMt;
extern uint64_t g_uiMtDraws;
extern uint8_t* g_dlgMt;
extern uint64_t g_dlgMtDraws;
extern bool g_cosmeticDraw;
extern PFN_MTSeed g_mtSeed;
extern uint8_t* g_cosMt;
extern bool g_attractOff, g_mtReseedOk;
extern bool g_noDesktopInput;
extern RandSite g_randSites[64];
extern int g_randSiteCount;
extern volatile LONG g_randFallbackState;
extern DWORD RVA_SPAWN_CALL;
extern DWORD RVA_SPAWN_UPDATE;
extern DWORD RVA_SPAWN_FORCE;
extern DWORD RVA_SPAWN_LOADER;
extern SpawnSlot g_spawnSlots[512];
extern uint32_t g_spawnSeq;

// graphics_patches.cpp
extern bool g_lodReady;
extern GfxConfig g_gfx;

// levels_scripts.cpp
extern bool g_vocalOn;
extern bool g_levelsWanted, g_levelsFileListOk, g_levelsWorldDbOk;
extern bool g_galleryButtonsOk;
extern bool g_bankListOk;
extern bool g_bankRenameOk;
extern bool g_scriptFixesWanted, g_scriptFixesOk;

// net_transport.cpp
extern NetCfg g_net;
extern bool g_netLaunchedByJoin;
extern wchar_t g_dropLaunchHow[16];
extern int g_localTest;
extern int g_localControllers;
extern bool g_sharedPad, g_sharedActive, g_sharedComboWas;
extern int g_localHostPad, g_localClientPad;
extern NetTransport* g_netT;

// net_session.cpp
extern bool g_netSettingsNotice;
extern wchar_t g_netPeerNameW[64];
extern uint64_t g_netMyId;
extern volatile LONG64 g_netJoinReq;
extern NetPhase g_netPhase;
extern uint32_t g_netSettings;
extern bool g_netPeerHello, g_netPeerReady, g_netLocalReady;
extern uint32_t g_netFrame;
extern unsigned g_fpCsrRef;
extern uint64_t g_fpCsrFixes;
extern NetPad g_netLocal[kNetRing], g_netRemote[kNetRing];
extern uint32_t g_netLocalTag[kNetRing], g_netRemoteTag[kNetRing];
extern NetPad g_netCur[2];
extern uint64_t g_netHashLocal[kNetRing], g_netHashRemote[kNetRing];
extern uint32_t g_netHashLocalTag[kNetRing], g_netHashRemoteTag[kNetRing];
extern ULONGLONG g_netLastSend, g_netLastHeard, g_netPhaseSince;
extern int g_netEndNeutralFrames;
extern uint64_t g_netStalls, g_netStallMs, g_netMaxStallMs, g_netHashesChecked, g_netPacketsIn, g_netPacketsOut;
extern char g_netEndReason[128];
extern uint32_t g_netLastHashFrame;
extern bool g_netPeerBye;
extern ULONGLONG g_netPeerByeAt;
extern uint32_t g_netPeerLastFrame;
extern ULONGLONG g_netNextStatus;
extern uint64_t g_netPeerVus, g_netPeerKeys;
extern bool g_netPeerValsKnown, g_netStartKnown;
extern uint64_t g_netStartVus, g_netStartKeys;
extern bool g_netRemoveP2;
extern ULONGLONG g_netEndedAt;
extern bool g_netEndedRunning;
extern DropStep g_dropStep;
extern bool g_dropIn, g_dropLaunched, g_dropSawBusy;
extern ULONGLONG g_dropAt;
extern uint8_t* g_dropBuf;
extern uint32_t g_dropSize;
extern char g_dropWorld[64];
extern int g_dropSlot;
extern char g_dropAvatar[2][16];
extern bool g_dropPublished;
extern uint32_t g_dropPublishedFrame;
extern uint32_t g_dropHintAt;
extern bool g_loadGate;
extern bool g_loadGateReady;
extern uint32_t (*g_unpauseOrig)();
extern uint32_t g_unpauseBlocked;
extern void (*g_worldUpdateOrig)(void*);
extern uint32_t g_worldFrozen;
extern bool g_loadStepDone;
extern uint32_t g_loadStepAt, g_streamIdleFrames;
extern LONG64 g_streamLastReads;
extern bool g_clientQuitPosted;
extern ULONGLONG g_clientFeSince;
extern uint32_t g_netHashHoldUntil;
extern uint32_t g_movieIdx, g_movieAtFrame, g_moviePeerSkipIdx;
extern uint32_t g_moviePeerIdx;
extern bool g_movieSkipLocal;
extern WORD g_moviePrevButtons;
extern ULONGLONG g_movieLastSend, g_peerMovieAt;
extern bool g_movieFixOn;
extern int g_netSessions;
extern ULONGLONG g_feSeen;
extern uint64_t g_feState;
extern bool g_feHooked;
extern ULONGLONG g_netNextHint;
extern uint32_t g_netLocalSentUpTo;
extern uint64_t g_netLastHash;
extern volatile ULONGLONG g_peerBusyAt;
extern std::string g_netSheet;
extern bool g_netWanted, g_netTried;

// net_dropin.cpp
extern int g_netSaveSync;

// net_lockstep.cpp
extern bool g_repairOn;
extern uint64_t g_repRounds, g_repItemsFixed, g_repMissedCount, g_repUnrepairable;
extern uint32_t g_rngQuietUntil;

// net_determinism.cpp
extern NetRngTag g_rngLocal[kNetRing];
extern uint32_t g_rngLocalTag[kNetRing];
extern uint32_t g_rngLocalNewest;
extern int g_rngEpisodeAsks;
extern uint64_t g_rngSlips, g_rngAsks;
extern uint64_t g_rngCatchUps;
extern bool g_syncLoaderOn, g_audioWaitOn, g_fma3Off;
extern uint64_t g_pumpDrainFrames, g_pumpDrainMaxMs;
extern uint64_t g_frameLimitSkips;
extern uint64_t g_loaderDrainFrames, g_loaderDrainMaxMs;
extern uint32_t g_loaderDrainMaxIters;
extern uint64_t g_voiceWaits;
extern bool g_rbZeroOn;
extern uint32_t g_updFrame, g_updCount;
extern uint64_t g_updHash;
extern bool g_lineGateOk;
extern volatile bool g_zeroAllocs;
extern bool g_zeroAllocOk;
extern bool g_musicSyncOk;
extern bool g_visSyncOk;
extern bool g_boneFlushOk;
extern uint64_t g_boneFlushNodes;
extern bool g_aspectOk;
extern bool g_screenZOk;
extern bool g_coinFlipOk;

// net_own_screen.cpp
extern bool g_popFixWanted;
extern bool g_ownScreenReady;
extern bool g_vpOwnLogged;
extern int g_hudHiddenN;
extern BYTE* g_promptEl[6];
extern bool g_healthPlaced;
extern bool g_regionFixOk;
extern bool g_previewFixOk;

// test_save.cpp
extern PFN_SHGetFolderPathA Real_SHGetFolderPathA;
extern const wchar_t* kLocalTestRoot;
extern PFN_SaveApply g_saveApply;

// ================================================================== shared functions, by owning module
//
// As with the globals, each function's comment is at its definition in the named module.

// xinput_proxy.cpp
void LogXInputRouting(const char* when);
int CopyDirFiles(const wchar_t* src, const wchar_t* dst);

// harness_core.cpp
void Log(const char* fmt, ...);
void NoteCall(int kind, DWORD user);
void OpenFilesLog();
void CloseFilesLog();
bool LoadRecording();
DWORD ReplayState(int kind, DWORD user, XINPUT_STATE* st);
bool IsClockThread();
void InstallClockHooks();

// patch_util.cpp
BYTE* ExeBase();
bool BytesAt(DWORD rva, const void* expect, size_t n);
bool RipRefersTo(DWORD insnRva, size_t dispOff, size_t insnLen, DWORD targetRva);
bool WriteMem(DWORD rva, const void* src, size_t n, bool code);
BYTE* AllocNear(BYTE* site, size_t size);
bool RedirectCall(DWORD callRva, DWORD expectTargetRva, void* hook);
bool WriteCode(DWORD rva, const void* bytes, size_t n);
int PatchIAT(HMODULE mod, const char* name, void* hook);

// sim_determinism.cpp
bool VerifyEngineClock();
uint32_t PublishMs();
bool PatchEngineClock();
void LogEngineClockSummary();
uint32_t Rva(uintptr_t a);
__declspec(noinline) uint32_t H_MTNext(uint8_t* inst);
bool VerifyMt();
bool PatchMt();
uint8_t* DialogMt(bool reseed);
uint8_t* CosmeticMt(bool reseed);
bool VerifyMtSeed();
bool PatchAttractVideo();
bool PatchNoDesktopInput();
void OpenMtDrawFile();
void WriteMtSummary();
void InstallRandIsolation();
void LogRandSummary();
bool VerifySpawnGate();
bool PatchSpawnGate();
void OpenSpawnLog();
void CloseSpawnLog();

// graphics_patches.cpp
void InstallFreeCamCull();
void GfxNewFrame();
void OpenGraphicsLog();
void ApplyGraphicsPatches();

// levels_scripts.cpp
void InstallHiddenLevels();
void VocalTick();
void InstallVocalTheme(const wchar_t* ini);
void InstallScriptFixes();

// status_overlay.cpp
void Status(DWORD ms, COLORREF accent, const wchar_t* fmt, ...);
void ShowNotice(const wchar_t* text);

// net_transport.cpp
void NetOpen();
void NetSummary();

// net_session.cpp
bool OnPressStart();
uint64_t LocalKeys();
const char* NetLinkName(int st);
void NetRumble(ULONGLONG ms, WORD strength);
void NetRumbleTick(ULONGLONG now);
void NetCheckLocalPad();
NetHdr NetHeader(uint8_t type);
void NetSend(const void* d, int n);
DWORD WINAPI LoadProfilerThread(LPVOID);
DWORD WINAPI NetKeepaliveThread(LPVOID);
void NetEnd(const char* reason);
std::vector<std::pair<std::string, std::string>> SheetParse(const std::string& s);
void NetPump();
void NetSendReady();
bool NetComboHeld(const NetPad& p);
void NetLogBindings(uint32_t f);
void NetHoldPadPoll(bool hold);
int NetAimInvertAction(uint8_t* a, uint32_t mine);
bool NetApplyAimInvert(const NetPad& pad, uint32_t f);
void NetLogAimSettings(uint32_t f);
DWORD NetGetState(DWORD user, XINPUT_STATE* st, bool anchor);
DWORD NetGetCaps(DWORD user, XINPUT_CAPABILITIES* caps);
DWORD NetSetState(DWORD user, XINPUT_VIBRATION* v);
void InstallNetExitHooks();
void InstallDesktopBlock();

// net_dropin.cpp
void SaveHandle(const uint8_t* p, int n, uint8_t type);
bool SaveSyncReady();
int StorageState();
BYTE* SaveGameObj();
bool DropCodeOk();
bool PostGameEvent(const char* ev, const void* arg);
bool DropSaveNow();
void LotSelectorReset(const char* when);
bool PmAsyncClear();
int DropWorldIndex(void** dbOut);
bool DropWriteAvatars(void* db, int idx);
bool DropCapture();
bool DropLevelUp();
bool DropInstall();
void DropQuitToTitle();
void DropAlignBindings();
void DropPrepareSave();
void NetRejoin();
void H_FadeStepRender();
void NetFadeStep(uint32_t f);

// net_lockstep.cpp
bool NetSimRandSite(int i);
void NetRemoteHash(uint32_t f, uint64_t h);
uint32_t RepFnv(const void* p, size_t n, uint32_t h);
void RepHandle(const uint8_t* p, int n, int type);
void RngAskRepair(uint32_t g);
NetPad NetReadLocal(uint32_t frame, bool tick);
void NetSendInputs(uint32_t upTo);
void NetFrame();

// net_determinism.cpp
void RngStoreRemote(uint32_t last, int count, const NetRngTag* tags);
void RngReset();
void RngStep(uint32_t f);
void SetSyncLoader(bool on);
bool SyncLoaderOk();
void KeepBtFreeHooked();
void KeyLogFlush(uint32_t f);
void BodyActorName(BYTE* o, char* nm, size_t cap);
void LogSilentWakes(uint32_t f, BYTE** arr, int n);
void LogActivationLists(uint32_t f);
void NetParkOrphanBodies(uint32_t f);
void NetVisSync();
void NetMusicSyncHold(bool on);
void InstallDeterminism();
void NetRejoinResetLate();

// net_own_screen.cpp
void NetViewportsRefresh();
bool PatchViewportSetAll();
bool HookPublish();
bool PatchMovieSkip();
void NetRemovePlayer2();
bool HookFrontendWatch();
bool NetOwnScreenActive();
void HudPlaceHealthBars(bool on);
bool PatchJobExec();
int PatchOwnedScreens();
bool PatchReticleShow();
void HudHidePrompts(bool on);
bool PatchHudLayout();
int InstallAudioListenerHooks();
void TrySoundPopFix(bool last);
bool H_SplitIsVertical();
bool H_SplitIsHorizontal();
void H_PreviewDraw(BYTE* node, void* rect, void* view, void* proj);

// local_test.cpp
void InstallLocalTestHooks();

// test_save.cpp
HRESULT WINAPI H_SHGetFolderPathA(HWND w, int csidl, HANDLE tok, DWORD flags, LPSTR path);
bool InstallTestSave();

// ================================================================== defined in debug_menu.cpp and gfx_d3d12.cpp

// debug_menu.cpp: in-game debug menu (decomp session). FilterPad sees this PC's
// player pad before it's recorded or sent, and neutralises it while the menu is open.
void DebugMenu_Install(const wchar_t* iniPath, void (*log)(const char* fmt, ...), bool netplay);
bool DebugMenu_FilterPad(unsigned user, XINPUT_STATE* st, bool frameTick);
// Netplay always on: the menu's cheats work while playing alone, and are reset
// and locked from the moment a friend connects until the session ends.
void DebugMenu_SetNetplay(bool on);
void DebugMenu_SetTestSave(bool on);   // test_save is on: offer "make this test save 100%"
void DebugMenu_FrontendUpdate(void* screenFrontend);   // the menu's title-screen work, called from our vf37 hook
// Shared menu actions in a session: player 1's actions are queued (not done
// locally), sent with that frame's input, and applied on both PCs on that frame.
void DebugMenu_EnableSharedActions(bool enable, bool canAct);
int  DebugMenu_PollAction(void* out, int cap);
void DebugMenu_ApplyAction(const void* data, int len);
// Detour the game function at rva if its first len bytes (>= 14) are prologue: its start is
// overwritten with a jump to hook. Returns a trampoline that calls the original, or NULL.
void* Mod_Detour(DWORD rva, const BYTE* prologue, size_t len, void* hook);   // debug_menu.cpp
// gfx_d3d12.cpp: D3D12/DXGI hooks (anisotropy, vrr, fxaa), volumetric light,
// shadow cascade and light-parameter overrides. Reads its own [graphics] keys.
void GfxExt_Install(const wchar_t* iniPath, void (*log)(const char* fmt, ...));
// debug_menu.cpp, called inside a level load's save apply (test_save.cpp): true, with a line
// for the screen in msg, when it applied the armed "make this test save 100%".
bool DebugMenu_SaveApplied(char* msg, size_t cap);
