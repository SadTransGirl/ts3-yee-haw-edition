// harness_core.cpp -- the record/replay harness core: the config and log globals, Log(), the
// frame and anchor bookkeeping, the file I/O hooks and file event log, the loading detector,
// recording replay, screenshots and the virtual QueryPerformanceCounter clock.
//
// The harness began as a test rig for determinism: mode=record writes every XInput poll the
// game makes to input.bin, and mode=replay feeds that file back so a second run gets the
// same controller input. If the game is deterministic, both runs then play out the same;
// screenshots (frames.csv), per-frame I/O (io.csv), file events (files.csv) and the engine
// RNG trace (sim_determinism.cpp) show where two runs part ways. Online co-op (the net_*
// modules) reuses the same frame counting and clocks.
//
// Words used below: a "poll" is one XInput call; a "frame" is one polling cycle (see
// NoteCall); a "logical" frame is a frame outside loads, the unit recordings are indexed by.
//
// Entry points:
//  - Log(): every module logs through it.
//  - NoteCall() and ReplayState(): called by HandleGetState (xinput_proxy.cpp) under g_cs,
//    NoteCall on every XInput poll, ReplayState for replay's live frames. NoteCall's anchor
//    poll runs the per-frame work (BeginFrame).
//  - LoadRecording(), OpenFilesLog(), CloseFilesLog(): from OpenRun / CloseRun in
//    xinput_proxy.cpp, at DllMain attach and detach.
//  - InstallClockHooks(): from DllMain when fixed_timestep=1 but the engine clock override
//    (sim_determinism.cpp) isn't used.

#include "proxy_shared.h"

// ------------------------------------------------------------------ config

// Filled from xinput_harness.ini by LoadConfig (xinput_proxy.cpp). g_inDir is the run
// folder a replay reads (harness\runs\<input>); g_outDir is where this run writes.
Config  g_cfg;
wchar_t g_exeDir[MAX_PATH];
wchar_t g_inDir[MAX_PATH];
wchar_t g_outDir[MAX_PATH];

// ------------------------------------------------------------------ logging

FILE*            g_log;
FILE*            g_framesCsv;
CRITICAL_SECTION g_cs;

// printf-style: one line to harness.log and to the debugger output, prefixed
// "[xinput-harness]". The file is flushed after every line.
void Log(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    char line[1100];
    snprintf(line, sizeof line, "[xinput-harness] %s\n", msg);
    OutputDebugStringA(line);
    if (g_log) { fputs(line, g_log); fflush(g_log); }
}

// ------------------------------------------------------------------ recording

FILE*    g_recFile;        // record: input.bin being written (opened by OpenRun)
static Rec*     g_replay;         // replay: the whole recording, read into memory by LoadRecording
size_t   g_replayCount;
size_t   g_replayPos;      // replay: next record to hand out
bool     g_userSeen[4];    // replay: pads that were connected in the recording
// Last state/result the replay handed the game, per pad. While loading, while
// holding at a recorded load and after the recording ends, the replay keeps
// returning these, exactly as a hands-off controller keeps reporting its
// resting position and packet number. (Returning zeros there made the game
// see the sticks move and a new packet when the replay resumed.)
XINPUT_STATE g_lastState[4];
DWORD        g_lastResult[4] = { ERROR_DEVICE_NOT_CONNECTED, ERROR_DEVICE_NOT_CONNECTED,
                                       ERROR_DEVICE_NOT_CONNECTED, ERROR_DEVICE_NOT_CONNECTED };

uint32_t g_seq;            // XInput calls so far (HandleGetState counts them)
uint32_t g_frame;          // read lock-free by the clock hooks
int      g_anchorKind = -1;
int      g_anchorUser = -1;
static DWORD    g_pollThread;     // the thread that made the first poll
static bool     g_multiThreadWarned;

uint32_t g_mismatches;
uint32_t g_firstMismatchFrame;
static bool     g_frameDriftLogged;
static bool     g_endLogged;

bool     g_clockHooked;    // fixed_timestep hooks are live
static void     ClockNewFrame();

// Loading detection state.
//
// The recording stores every poll, but polls made while a load was in
// progress carry REC_LOADING, and a KIND_LOAD marker is written when each load
// finishes. Replay never consumes recorded input during its own loads, and on
// reaching a marker it holds neutral input until it has finished a load of
// its own. So the k-th load of the replay lines up with the k-th load of the
// recording no matter how long either one took.
static const char* kSrcName[4] = { "exe", "crt", "bink", "fmod" };

volatile LONG64 g_ioReads[4];
static volatile LONG64 g_ioBytes[4], g_ioOpens[4];
static LONG64   g_ioPrevBytes[4], g_ioPrevReads[4], g_ioPrevOpens[4];
static bool     g_ioHooked;
FILE*    g_ioCsv;
LONGLONG g_realFreq;
static LONGLONG g_lastFrameQpc;

uint32_t g_logical;        // frames outside loads (record) / that consumed input (replay)
bool     g_loading;        // detector says a load is in progress
bool     g_frameLive = true; // this frame's polls use real (record) or recorded (replay) input
static bool     g_waiting;        // replay: holding at a marker for our own load
static uint32_t g_waitFrames;
static uint32_t g_loadStartPoll;
static LONG64   g_loadBytes;
uint32_t g_loadsCompleted, g_loadsConsumed, g_loadTimeouts, g_unmatchedLoads;
static uint64_t g_ring[64];
static int      g_ringPos;
static int      g_quietRun;

static void MaybeScreenshot();
static void BeginFrame();
static void InstallIoHooks();

// A "frame" is one polling cycle. The first (kind,user) the game asks for
// becomes the anchor; every later call matching the anchor starts a new frame.
// This works for games polling pad 0 only, pads 0..3, or GetState+GetStateEx.
// kind: 0 XInputGetState, 1 XInputGetStateEx. The first poll also installs the
// file I/O hooks (io_log or load_detect) and retries the sound pop fix (FMOD may
// not have been loaded yet at DllMain).
void NoteCall(int kind, DWORD user) {
    DWORD tid = GetCurrentThreadId();
    if (g_anchorKind < 0) {
        g_anchorKind = kind;
        g_anchorUser = (int)user;
        g_pollThread = tid;
        Log("first poll: kind=%d user=%lu thread=%lu", kind, user, tid);
        TrySoundPopFix(true);
        if (g_cfg.ioLog || g_cfg.loadDetect) InstallIoHooks();
        LogXInputRouting("first poll");
    } else if (tid != g_pollThread && !g_multiThreadWarned) {
        g_multiThreadWarned = true;
        Log("WARNING: XInput polled from more than one thread (%lu and %lu); "
            "frame counting may be unreliable", g_pollThread, tid);
    }
    if (kind == g_anchorKind && (int)user == g_anchorUser) {
        // A new frame: everything that runs once per frame hangs off here.
        g_frame++;
        if (g_frame == 600) LogXInputRouting("poll 600, about 10 s in");
        if (g_clockHooked) ClockNewFrame();
        if (g_lodReady) GfxNewFrame();
        if (g_vocalOn) VocalTick();
        BeginFrame();
    }
}

// ------------------------------------------------------------------ file I/O hooks
//
// Each reader gets its own copy of the hook (template parameter = source) so
// the game's own reads can be told apart from video and audio streaming.

typedef BOOL   (WINAPI* PFN_ReadFile)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef HANDLE (WINAPI* PFN_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI* PFN_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static PFN_ReadFile    Real_ReadFile;
static PFN_CreateFileA Real_CreateFileA;
static PFN_CreateFileW Real_CreateFileW;

// ------------------------------------------------------------------ file event log
//
// files.csv: every file the game opens, and every read made during play (not
// during loads, which do thousands), with the poll and logical frame, the
// thread (named if the game named it) and the file. diff_files.py compares
// two runs event by event, so a streamed file arriving a frame early in one
// run shows up with its name and the thread that read it.

static FILE*   g_filesCsv;
static SRWLOCK g_filesLock = SRWLOCK_INIT;
static LONG    g_filesLogged;
static const LONG kMaxFileEvents = 500000;
struct HandleSlot { HANDLE h; wchar_t path[MAX_PATH]; };
static HandleSlot g_handles[4096];
static DWORD   g_seenThreads[256];
static int     g_seenThreadCount;
typedef HRESULT (WINAPI* PFN_GetThreadDescription)(HANDLE, PWSTR*);
static PFN_GetThreadDescription g_getThreadDescription;

// Handle -> path, so a read can be logged with its file name. Open addressing on
// the handle value; insert: return a free slot (or, after 64 full probes, overwrite
// the home slot); otherwise NULL when the handle isn't known.
static HandleSlot* HandleSlotFor(HANDLE h, bool insert) {
    size_t base = ((uintptr_t)h >> 2) & 4095;
    for (int i = 0; i < 64; i++) {
        HandleSlot& s = g_handles[(base + i) & 4095];
        if (s.h == h) return &s;
        if (!s.h) return insert ? &s : NULL;
    }
    return insert ? &g_handles[base] : NULL;
}

// One files.csv row (caller holds g_filesLock). The first event from a thread
// is preceded by a "thread" row giving its name. The state column is 0 play,
// 1 loading, 2 replay holding at a recorded load.
static void FileEventLocked(int src, const char* op, DWORD bytes, const wchar_t* path) {
    DWORD tid = GetCurrentThreadId();
    bool seen = false;
    for (int i = 0; i < g_seenThreadCount; i++) if (g_seenThreads[i] == tid) { seen = true; break; }
    if (!seen && g_seenThreadCount < 256) {
        g_seenThreads[g_seenThreadCount++] = tid;
        PWSTR desc = NULL;
        if (g_getThreadDescription && SUCCEEDED(g_getThreadDescription(GetCurrentThread(), &desc)) && desc) {
            fwprintf(g_filesCsv, L"%u,%u,%d,%lu,%d,%S,thread,0,\"%s\"\n", g_frame, g_logical,
                     g_waiting ? 2 : g_loading ? 1 : 0, tid, tid == g_pollThread, kSrcName[src], desc[0] ? desc : L"(unnamed)");
            LocalFree(desc);
        }
    }
    fwprintf(g_filesCsv, L"%u,%u,%d,%lu,%d,%S,%S,%lu,\"%s\"\n", g_frame, g_logical, g_waiting ? 2 : g_loading ? 1 : 0,
             tid, tid == g_pollThread, kSrcName[src], op, bytes, path ? path : L"?");
}

// Remember which file a new handle is, and log the open.
static void LogFileOpen(int src, HANDLE h, const wchar_t* path) {
    if (!g_filesCsv || h == INVALID_HANDLE_VALUE || !path) return;
    AcquireSRWLockExclusive(&g_filesLock);
    if (g_filesCsv) {
        HandleSlot* slot = HandleSlotFor(h, true);
        slot->h = h;
        wcsncpy_s(slot->path, path, _TRUNCATE);
        if (g_filesLogged++ < kMaxFileEvents) FileEventLocked(src, "open", 0, path);
    }
    ReleaseSRWLockExclusive(&g_filesLock);
}

// One "read" row, with the handle's file name if its open was seen; play only.
static void LogFileRead(int src, HANDLE h, DWORD bytes) {
    if (!g_filesCsv || g_loading || g_waiting) return;     // play only
    AcquireSRWLockExclusive(&g_filesLock);
    if (g_filesCsv && g_filesLogged++ < kMaxFileEvents) {
        HandleSlot* slot = HandleSlotFor(h, false);
        FileEventLocked(src, "read", bytes, slot ? slot->path : NULL);
    }
    ReleaseSRWLockExclusive(&g_filesLock);
}

// Create files.csv in the run folder. GetThreadDescription is looked up at run
// time: older Windows versions don't have it (threads are then not named).
void OpenFilesLog() {
    g_getThreadDescription = (PFN_GetThreadDescription)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\files.csv", g_outDir);
    if (!_wfopen_s(&g_filesCsv, path, L"w, ccs=UTF-8") && g_filesCsv) {
        setvbuf(g_filesCsv, NULL, _IOFBF, 1 << 16);
        fputws(L"poll,logical,state,thread,main,source,op,bytes,file\n", g_filesCsv);
    }
}

// At exit: a last row says how many events were dropped past kMaxFileEvents.
void CloseFilesLog() {
    AcquireSRWLockExclusive(&g_filesLock);
    if (g_filesCsv) {
        if (g_filesLogged > kMaxFileEvents)
            fwprintf(g_filesCsv, L"0,0,0,0,0,exe,truncated,%ld,\"event limit reached\"\n", g_filesLogged - kMaxFileEvents);
        fclose(g_filesCsv);
        g_filesCsv = NULL;
    }
    ReleaseSRWLockExclusive(&g_filesLock);
}

// The import hooks. S is the source (0 exe, 1 crt, 2 bink, 3 fmod: see kSrcName);
// the byte counts feed the loading detector (BeginFrame), the rest files.csv.
template <int S>
static BOOL WINAPI H_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    // Count the requested size: overlapped reads finish later, and the
    // request is what the game is waiting on either way.
    InterlockedAdd64(&g_ioBytes[S], n);
    InterlockedIncrement64(&g_ioReads[S]);
    LogFileRead(S, h, n);
    return Real_ReadFile(h, buf, n, got, ov);
}
// Drop-in client: this PC plays the host's progress from memory, so its own
// "Saved Game" must never be overwritten. Writes go to "Saved Game (co-op session)".
// (g_protectSave is set by SaveApply in net_dropin.cpp when the host's save buffer
// arrives.) RedirectSaveW: true, with the new path in out, for an open of a file
// named "Saved Game" with write access while protected; false otherwise.
volatile bool g_protectSave;
static volatile LONG g_protectLogged;
static bool RedirectSaveW(LPCWSTR p, DWORD access, wchar_t* out, size_t cap) {
    if (!g_protectSave || !p || !(access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA | GENERIC_ALL))) return false;
    static const wchar_t kTail[] = L"\\Saved Game";
    size_t n = wcslen(p), tl = wcslen(kTail);
    if (n < tl || _wcsicmp(p + n - tl, kTail) != 0) return false;
    swprintf_s(out, cap, L"%s (co-op session)", p);
    if (!InterlockedExchange(&g_protectLogged, 1)) Log("drop-in: this PC's own save is protected; the game's saves go to %ls", out);
    return true;
}

// CreateFileA/W hooks: count the open, send a protected save write elsewhere,
// and log the file's name for files.csv.
template <int S>
static HANDLE WINAPI H_CreateFileA(LPCSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) {
    InterlockedIncrement64(&g_ioOpens[S]);
    if (g_protectSave && p) {
        wchar_t w[MAX_PATH], redirected[MAX_PATH + 32];
        if (MultiByteToWideChar(CP_ACP, 0, p, -1, w, MAX_PATH) && RedirectSaveW(w, a, redirected, _countof(redirected)))
            return Real_CreateFileW(redirected, a, s, sa, c, f, t);
    }
    HANDLE r = Real_CreateFileA(p, a, s, sa, c, f, t);
    if (g_filesCsv && p) {
        wchar_t w[MAX_PATH];
        if (MultiByteToWideChar(CP_ACP, 0, p, -1, w, MAX_PATH)) LogFileOpen(S, r, w);
    }
    return r;
}
template <int S>
static HANDLE WINAPI H_CreateFileW(LPCWSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) {
    InterlockedIncrement64(&g_ioOpens[S]);
    wchar_t redirected[MAX_PATH + 32];
    if (RedirectSaveW(p, a, redirected, _countof(redirected))) return Real_CreateFileW(redirected, a, s, sa, c, f, t);
    HANDLE r = Real_CreateFileW(p, a, s, sa, c, f, t);
    LogFileOpen(S, r, p);
    return r;
}

// Point one module's ReadFile / CreateFileA / CreateFileW imports at the hooks
// for its source.
static void HookModuleIo(const wchar_t* name, HMODULE mod, int src, void* rf, void* cfa, void* cfw) {
    if (!mod) { Log("io hooks: %ls not loaded, skipped", name); return; }
    int n = PatchIAT(mod, "ReadFile", rf) + PatchIAT(mod, "CreateFileA", cfa) + PatchIAT(mod, "CreateFileW", cfw);
    Log("io hooks: %ls -> source '%s', %d imports patched", name, kSrcName[src], n);
}

// Called on the first poll rather than in DllMain: by then every DLL the game
// links against is loaded, so their import tables can be patched too.
static void InstallIoHooks() {
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    Real_ReadFile    = (PFN_ReadFile)   GetProcAddress(k32, "ReadFile");
    Real_CreateFileA = (PFN_CreateFileA)GetProcAddress(k32, "CreateFileA");
    Real_CreateFileW = (PFN_CreateFileW)GetProcAddress(k32, "CreateFileW");
    if (!Real_ReadFile || !Real_CreateFileA || !Real_CreateFileW) { Log("io hooks: kernel32 lookup failed"); return; }
    HookModuleIo(L"game exe",       GetModuleHandleW(NULL),                0, (void*)H_ReadFile<0>, (void*)H_CreateFileA<0>, (void*)H_CreateFileW<0>);
    HookModuleIo(L"ucrtbase.dll",   GetModuleHandleW(L"ucrtbase.dll"),     1, (void*)H_ReadFile<1>, (void*)H_CreateFileA<1>, (void*)H_CreateFileW<1>);
    HookModuleIo(L"bink2w64.dll",   GetModuleHandleW(L"bink2w64.dll"),     2, (void*)H_ReadFile<2>, (void*)H_CreateFileA<2>, (void*)H_CreateFileW<2>);
    HookModuleIo(L"fmod.dll",       GetModuleHandleW(L"fmod.dll"),         3, (void*)H_ReadFile<3>, (void*)H_CreateFileA<3>, (void*)H_CreateFileW<3>);
    HookModuleIo(L"fmodstudio.dll", GetModuleHandleW(L"fmodstudio.dll"),   3, (void*)H_ReadFile<3>, (void*)H_CreateFileA<3>, (void*)H_CreateFileW<3>);
    g_ioHooked = true;
}

// ------------------------------------------------------------------ loading detector

// A load just finished: count it, and when recording, write a KIND_LOAD marker
// (result = the load's number) so a replay can line its own loads up with it.
static void OnLoadEnd() {
    g_loadsCompleted++;
    Log("LOAD #%u end at poll %u (logical %u): %u polls, %.1f MB read",
        g_loadsCompleted, g_frame, g_logical, g_frame - g_loadStartPoll, g_loadBytes / 1048576.0);
    if (g_cfg.mode == MODE_RECORD && g_recFile) {
        Rec m = {};
        m.seq = g_seq; m.frame = g_logical; m.kind = KIND_LOAD; m.user = 0;
        m.result = g_loadsCompleted;
        fwrite(&m, sizeof m, 1, g_recFile);
        fflush(g_recFile);
    }
}

// Hysteresis: enter when loadWindow frames together read loadEnterKb, leave
// after loadQuietFrames consecutive frames each reading under loadQuietKb.
// Called once per frame by BeginFrame with that frame's bytes (from the sources
// use_src counts) and its real duration in ms (load_hitch_ms).
static void UpdateDetector(uint64_t bytes, double ms) {
    int w = g_cfg.loadWindow < 1 ? 1 : g_cfg.loadWindow > 64 ? 64 : g_cfg.loadWindow;
    g_ring[g_ringPos++ % 64] = bytes;
    uint64_t sum = 0;
    for (int i = 1; i <= w && i <= g_ringPos; i++) sum += g_ring[(g_ringPos - i) % 64];
    bool hitch = g_cfg.loadHitchMs > 0 && ms >= g_cfg.loadHitchMs;

    if (!g_loading) {
        if (sum >= (uint64_t)g_cfg.loadEnterKb * 1024 || hitch) {
            g_loading = true;
            g_quietRun = 0;
            g_loadStartPoll = g_frame;
            g_loadBytes = (LONG64)sum;
            Log("LOAD #%u begin at poll %u (logical %u): %.0f KB in last %d frames%s",
                g_loadsCompleted + 1, g_frame, g_logical, sum / 1024.0, w, hitch ? ", frame hitch" : "");
        }
    } else {
        g_loadBytes += (LONG64)bytes;
        bool quiet = bytes < (uint64_t)g_cfg.loadQuietKb * 1024 && !hitch;
        g_quietRun = quiet ? g_quietRun + 1 : 0;
        if (g_quietRun >= g_cfg.loadQuietFrames) {
            g_loading = false;
            OnLoadEnd();
        }
    }
}

// Replay: decide whether this frame consumes recorded input.
// Sets g_frameLive. Not while this run is loading. At a recorded load marker:
// pass it if this run has finished its matching load, else hold (g_waiting)
// until it does, or give up after load_wait_frames.
static void ResolveReplayFrame() {
    if (g_loading) { g_frameLive = false; return; }
    for (;;) {
        // Polls that happened during a load of the recording are never replayed.
        while (g_replayPos < g_replayCount && (g_replay[g_replayPos].flags & REC_LOADING)) g_replayPos++;
        if (g_replayPos >= g_replayCount || g_replay[g_replayPos].kind != KIND_LOAD) break;
        if (!g_cfg.loadDetect) { g_replayPos++; continue; }   // detection off: ignore markers
        if (g_loadsCompleted > g_loadsConsumed) {             // our matching load already finished
            g_loadsConsumed++;
            g_replayPos++;
            if (g_waiting) Log("replay: load #%u matched after holding %u polls", g_loadsConsumed, g_waitFrames);
            g_waiting = false;
            g_waitFrames = 0;
            continue;
        }
        if (!g_waiting) {
            g_waiting = true;
            g_waitFrames = 0;
            Log("replay: reached recorded load #%u at logical %u, holding neutral input until this run's load finishes",
                g_loadsConsumed + 1, g_logical);
        }
        if (++g_waitFrames > (uint32_t)g_cfg.loadWaitFrames) {
            g_loadTimeouts++;
            Log("WARNING: replay: waited %u polls for load #%u and none came; skipping the marker. "
                "Replay is probably out of sync from here.", g_waitFrames, g_loadsConsumed + 1);
            g_loadsConsumed++;
            g_replayPos++;
            g_waiting = false;
            g_waitFrames = 0;
            continue;
        }
        g_frameLive = false;
        return;
    }
    g_frameLive = true;
}

// Per-frame bookkeeping, at the anchor poll (NoteCall): this frame's I/O, the
// loading detector, whether the frame is live, io.csv, and maybe a screenshot.
static void BeginFrame() {
    // Real elapsed time since the previous frame (our own IAT is never patched,
    // so this is the true clock even with fixed_timestep on).
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double ms = g_lastFrameQpc ? (now.QuadPart - g_lastFrameQpc) * 1000.0 / g_realFreq : 0.0;
    g_lastFrameQpc = now.QuadPart;

    // What each source read and opened since the last frame.
    LONG64 dBytes[4] = {}, dReads = 0, dOpens = 0;
    uint64_t counted = 0;
    if (g_ioHooked) {
        for (int s = 0; s < 4; s++) {
            LONG64 b = g_ioBytes[s], r = g_ioReads[s], o = g_ioOpens[s];
            dBytes[s] = b - g_ioPrevBytes[s];
            dReads += r - g_ioPrevReads[s];
            dOpens += o - g_ioPrevOpens[s];
            g_ioPrevBytes[s] = b; g_ioPrevReads[s] = r; g_ioPrevOpens[s] = o;
            if (g_cfg.useSrc[s]) counted += (uint64_t)dBytes[s];
        }
    }

    bool wasLoading = g_loading;
    if (g_cfg.loadDetect && g_ioHooked) UpdateDetector(counted, ms);

    if (g_cfg.mode == MODE_REPLAY) {
        ResolveReplayFrame();
        // A load in the replay with no recorded load waiting for it.
        if (wasLoading && !g_loading && g_loadsCompleted > g_loadsConsumed + 1) {
            g_unmatchedLoads++;
            Log("WARNING: replay: load #%u finished but the recording has not reached a load yet "
                "(extra load in this run, or the previous one never matched)", g_loadsCompleted);
        }
    } else {
        g_frameLive = !g_loading;                      // record (and the other modes): live unless loading
    }
    if (g_frameLive) g_logical++;

    // One io.csv row per frame.
    if (g_ioCsv) {
        static uint64_t prevTicks;
        static LONG64 prevMt;
        LONG64 mt = g_mtMainDraws - prevMt;              // engine RNG draws since the last poll
        prevMt = g_mtMainDraws;
        uint64_t ticks = g_engineFrames - prevTicks;   // game frames since the last poll
        prevTicks = g_engineFrames;
        fprintf(g_ioCsv, "%u,%u,%d,%.2f,%.1f,%.1f,%.1f,%.1f,%lld,%lld,%llu,%lld\n", g_frame, g_logical,
                g_waiting ? 2 : g_loading ? 1 : 0, ms, dBytes[0] / 1024.0, dBytes[1] / 1024.0,
                dBytes[2] / 1024.0, dBytes[3] / 1024.0, dReads, dOpens, ticks, mt);
        if ((g_frame % 60) == 0) fflush(g_ioCsv);
    }
    if (g_frameLive) MaybeScreenshot();
}

// Replay: read <input folder>\input.bin into memory. Returns false (and the run
// falls back to passthrough) if it's missing or not a recording. Each pad starts
// from its first recorded state. The header's flags say how it was recorded
// (bit 0 fixed_timestep, 1 freeze_wallclock, 2 load_detect, 3 and 5 engine clock,
// 4 rand_isolate, 7 spawn gate; reserved[0..1] the spawn delay); a mismatch with
// this run is logged.
bool LoadRecording() {
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\input.bin", g_inDir);
    FILE* f = NULL;
    if (_wfopen_s(&f, path, L"rb") || !f) { Log("FATAL: cannot open recording %ls", path); return false; }
    FileHeader h;
    if (fread(&h, sizeof h, 1, f) != 1 || memcmp(h.magic, "XIH1", 4) != 0 || h.recSize != sizeof(Rec)) {
        Log("FATAL: %ls is not a harness recording", path);
        fclose(f);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, sizeof h, SEEK_SET);
    g_replayCount = (size_t)(size - (long)sizeof h) / sizeof(Rec);
    g_replay = (Rec*)malloc((g_replayCount + 1) * sizeof(Rec));
    if (!g_replay) { fclose(f); return false; }
    g_replayCount = fread(g_replay, sizeof(Rec), g_replayCount, f);
    fclose(f);
    // Count markers and loading polls, and prime each pad's last state.
    uint32_t markers = 0, loadingPolls = 0;
    bool primed[4] = {};
    for (size_t i = 0; i < g_replayCount; i++) {
        const Rec& r = g_replay[i];
        if (r.kind == KIND_LOAD) { markers++; continue; }
        if (r.flags & REC_LOADING) loadingPolls++;
        if (r.user < 4 && !primed[r.user]) {          // start from the pad's first recorded state
            primed[r.user] = true;
            g_lastState[r.user] = r.state;
            g_lastResult[r.user] = r.result;
        }
        if (r.result == ERROR_SUCCESS && r.user < 4) g_userSeen[r.user] = true;
    }
    uint32_t lastFrame = g_replayCount ? g_replay[g_replayCount - 1].frame : 0;
    Log("replaying %ls: %zu records, %u logical frames, %u loads, %u polls during loads "
        "(recorded with fixed_timestep=%u load_detect=%u fps=%.2f)",
        path, g_replayCount, lastFrame, markers, loadingPolls, h.flags & 1, (h.flags >> 2) & 1, h.fps);
    bool engineNow = g_engineClockReady;
    if (((h.flags & 128) != 0) != g_spawnGateOn ||
        (g_spawnGateOn && *(const uint16_t*)h.reserved != (uint16_t)g_cfg.spawnDelay))
        Log("WARNING: recording used spawn_gate=%u spawn_delay=%u, this replay uses %d/%d. "
            "The runs are not comparable.", (h.flags >> 7) & 1, *(const uint16_t*)h.reserved,
            g_spawnGateOn, g_cfg.spawnDelay);
    if (((h.flags & 32) != 0) != engineNow || ((h.flags & 16) != 0) != (g_cfg.randIsolate != 0))
        Log("WARNING: recording used engine_clock=%u rand_isolate=%u, this replay uses %d/%d. "
            "The runs are not comparable.", (h.flags >> 3) & 1, (h.flags >> 4) & 1, engineNow, g_cfg.randIsolate);
    if (!(h.flags & 4) && g_cfg.loadDetect)
        Log("NOTE: this recording has no load markers. Loads in this replay will still pause input, "
            "but nothing lines them up with the recording.");
    if (((h.flags & 1) != 0) != (g_cfg.fixedTimestep != 0))
        Log("WARNING: recording was made with fixed_timestep=%u but this replay uses %d. "
            "The runs are not comparable.", h.flags & 1, g_cfg.fixedTimestep);
    return true;
}

// Replay, on a live frame: hand the game the next recorded poll and return its
// result. Past the end, each pad keeps its last state. If the game asks for a
// different call or pad than the recording has next, look up to 16 records
// ahead for a match; without one, return a neutral pad.
DWORD ReplayState(int kind, DWORD user, XINPUT_STATE* st) {
    if (g_replayPos >= g_replayCount) {
        if (!g_endLogged) { g_endLogged = true; Log("replay: end of recording reached at frame %u", g_frame); }
        if (user < 4) { *st = g_lastState[user]; return g_lastResult[user]; }
        ZeroMemory(st, sizeof *st);
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    Rec* r = &g_replay[g_replayPos];
    if (r->kind != kind || r->user != user) {
        // The game is asking for a different pad/call than it did when
        // recording: the call pattern itself has diverged. Try to resync.
        g_mismatches++;
        if (g_mismatches == 1) g_firstMismatchFrame = g_frame;
        size_t j = g_replayPos;
        for (; j < g_replayCount && j < g_replayPos + 16; j++)
            if (g_replay[j].kind == kind && g_replay[j].user == user) break;
        bool found = j < g_replayCount && j < g_replayPos + 16;
        if (g_mismatches <= 20)
            Log("replay: CALL PATTERN MISMATCH #%u at seq %u frame %u: recorded kind=%u user=%u, "
                "game asked kind=%d user=%lu -> %s",
                g_mismatches, g_seq, g_frame, r->kind, r->user, kind, user,
                found ? "resynced" : "returned neutral state");
        if (!found) { ZeroMemory(st, sizeof *st); return ERROR_SUCCESS; }
        g_replayPos = j;
        r = &g_replay[g_replayPos];
    }
    if (r->frame != g_logical && !g_frameDriftLogged) {
        g_frameDriftLogged = true;
        Log("replay: frame numbering drifted at seq %u (recorded logical frame %u, now %u)", g_seq, r->frame, g_logical);
    }
    *st = r->state;
    g_replayPos++;
    return r->result;
}

// ------------------------------------------------------------------ screenshots

static HWND g_hwnd;
int  g_shotsTaken;

// EnumWindows callback: the first visible top-level window of this process
// at least 320x200 is the game's; it's stored in *(HWND*)lp.
static BOOL CALLBACK FindGameWindow(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetParent(h)) return TRUE;
    RECT rc;
    if (!GetClientRect(h, &rc) || rc.right < 320 || rc.bottom < 200) return TRUE;
    *(HWND*)lp = h;
    return FALSE;
}

// FNV-1a over the RGB bytes only; the X byte of BGRX is undefined after BitBlt.
static uint64_t HashPixels(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        if ((i & 3) == 3) continue;
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// Every shot_interval logical frames (up to shot_max): capture the game window
// to shot_<logical>.bmp and add its pixel hash to frames.csv, so two runs can be
// compared at the same logical frames.
static void MaybeScreenshot() {
    if (g_cfg.mode == MODE_PASSTHROUGH) return;   // no run folder to write into
    if (g_cfg.shotInterval <= 0 || g_shotsTaken >= g_cfg.shotMax) return;
    if (g_logical == 0 || g_logical % (uint32_t)g_cfg.shotInterval) return;
    if (!g_hwnd || !IsWindow(g_hwnd)) {
        g_hwnd = NULL;
        EnumWindows(FindGameWindow, (LPARAM)&g_hwnd);
        if (!g_hwnd) return;
        DWORD wndThread = GetWindowThreadProcessId(g_hwnd, NULL);
        Log("game window %p found; window thread=%lu poll thread=%lu (%s)", (void*)g_hwnd, wndThread,
            g_pollThread, wndThread == g_pollThread ? "input polled on the window thread" : "input polled on another thread");
    }
    RECT rc;
    if (!GetClientRect(g_hwnd, &rc)) return;
    POINT tl = { 0, 0 };
    ClientToScreen(g_hwnd, &tl);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;

    // A 32-bit top-down DIB section to draw the window into.
    HDC sdc = GetDC(NULL);
    HDC mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(mdc, bmp);
        // PrintWindow with PW_RENDERFULLCONTENT asks DWM for the window's own
        // composed surface (works for D3D swap chains in windowed/borderless
        // mode) even if another window is on top. Fall back to a screen blit.
        if (!PrintWindow(g_hwnd, mdc, 2 /* PW_RENDERFULLCONTENT */))
            BitBlt(mdc, 0, 0, w, h, sdc, tl.x, tl.y, SRCCOPY | CAPTUREBLT);
        size_t n = (size_t)w * h * 4;
        uint64_t hash = HashPixels((const uint8_t*)bits, n);

        // Write it as a .bmp: file header, info header, pixels.
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\shot_%06u.bmp", g_outDir, g_logical);
        FILE* f = NULL;
        if (!_wfopen_s(&f, path, L"wb") && f) {
            BITMAPFILEHEADER fh = {};
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof fh + sizeof(BITMAPINFOHEADER);
            fh.bfSize = fh.bfOffBits + (DWORD)n;
            BITMAPINFOHEADER ih = bi.bmiHeader;
            ih.biSizeImage = (DWORD)n;
            fwrite(&fh, sizeof fh, 1, f);
            fwrite(&ih, sizeof ih, 1, f);
            fwrite(bits, 1, n, f);
            fclose(f);
        }
        if (g_framesCsv) { fprintf(g_framesCsv, "%u,%016llx,%d,%d\n", g_logical, (unsigned long long)hash, w, h); fflush(g_framesCsv); }
        g_shotsTaken++;
        SelectObject(mdc, old);
        DeleteObject(bmp);
    }
    DeleteDC(mdc);
    ReleaseDC(NULL, sdc);
}

// ------------------------------------------------------------------ virtual clock
//
// fixed_timestep=1 without the engine clock override (sim_determinism.cpp): the
// game's clock imports (QueryPerformanceCounter, Sleep, GetTickCount...) are
// replaced so time depends only on what the game does, not on how fast this PC is.

LONGLONG  g_freq;                 // QPC ticks per second
static LONGLONG  g_base;          // real QPC reading at install: virtual time 0
static ULONGLONG g_wallBase;      // freeze_wallclock: 2026-01-01 as a FILETIME
int              g_hooksInstalled;   // PatchIAT (patch_util.cpp) counts every import it hooks

// Virtual time is one monotonic counter, g_vt, in QPC ticks since g_base:
//
//  - every clock read on the main/poll thread advances it by 1 us, uncapped,
//    so a spin-wait ("loop until 16.7 ms have passed") always terminates, and
//    terminates after the same number of reads every run;
//  - Sleep(ms) on that thread advances it by exactly ms;
//  - each new input frame raises it to at least previous frame start + 1/fps.
//
// Reads from other threads see the current value without advancing it, so
// worker-thread timing can't leak into the main thread's view of time. If a
// worker spins on the clock while the main thread is stuck waiting for it,
// after a million fruitless reads the worker is allowed to advance time
// (counted as a "stall assist": that run may not be reproducible).
volatile LONG64 g_vt;
static LONG64          g_frameStartVt;
static LONG64          g_tick = 1;
static volatile LONG64 g_workerReads;
volatile LONG   g_stallAssists;
DWORD           g_mainThread;
static LONGLONG        g_paceQpc;

// The threads whose clock reads move virtual time: the one that loaded this DLL
// (DllMain) and the one that polls XInput.
bool IsClockThread() {
    DWORD t = GetCurrentThreadId();
    return t == g_mainThread || t == g_pollThread;
}

// The current virtual QPC value, by the rules above.
static LONGLONG VirtualQpc() {
    if (IsClockThread()) {
        InterlockedExchange64(&g_workerReads, 0);
        return g_base + InterlockedAdd64(&g_vt, g_tick);
    }
    LONG64 n = InterlockedIncrement64(&g_workerReads);
    if (n > 1000000) {
        if (n == 1000001) InterlockedIncrement(&g_stallAssists);
        return g_base + InterlockedAdd64(&g_vt, g_tick);
    }
    return g_base + g_vt;
}

// Called at the start of every input frame (anchor poll).
static void ClockNewFrame() {
    LONG64 perFrame = (LONG64)((double)g_freq / g_cfg.fps);
    LONG64 target = g_frameStartVt + perFrame;
    for (;;) {
        LONG64 cur = g_vt;
        if (cur >= target || InterlockedCompareExchange64(&g_vt, target, cur) == cur) break;
    }
    g_frameStartVt = g_vt;

    // Pace to real time so the game stays playable: virtual time no longer
    // depends on the real clock, so nothing here affects determinism.
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);   // our own import: the real clock
    if (g_paceQpc) {
        // Coarse Sleep until ~2 ms before the deadline (timer resolution is
        // 1 ms, see InstallClockHooks), then spin on the real clock.
        LONGLONG deadline = g_paceQpc + (LONGLONG)(g_freq / g_cfg.fps);
        LONGLONG left = deadline - now.QuadPart;
        if (left > 0) {
            DWORD coarse = (DWORD)(left * 1000 / g_freq);
            if (coarse > 2) Sleep(coarse - 2);
            do { QueryPerformanceCounter(&now); } while (now.QuadPart < deadline);
        }
        // Fell behind by more than a frame (slow frame, load): don't try to catch up.
        if (now.QuadPart - deadline > g_freq / g_cfg.fps) deadline = now.QuadPart;
        g_paceQpc = deadline;
        return;
    }
    g_paceQpc = now.QuadPart;
}

static BOOL WINAPI H_QueryPerformanceCounter(LARGE_INTEGER* li) {
    li->QuadPart = VirtualQpc();
    return TRUE;
}
// On the main thread, Sleep only moves virtual time and yields. Really sleeping
// would be both pointless (ClockNewFrame paces each frame to real time) and
// harmful: Windows rounds Sleep(1) up to ~15.6 ms, so a "Sleep(1) until a frame
// has passed" limiter would run 15x slower than real time.
static void WINAPI H_Sleep(DWORD ms) {
    if (ms != INFINITE && IsClockThread()) {
        InterlockedAdd64(&g_vt, (LONG64)ms * g_freq / 1000);
        Sleep(0);
        return;
    }
    Sleep(ms);
}
// The millisecond clocks: virtual time from 100000 ms (the engine clock's kStartMs
// is the same value). GetSystemTimeAsFileTime (freeze_wallclock): 2026-01-01 plus
// virtual time.
static ULONGLONG VirtualMs() { return (ULONGLONG)((VirtualQpc() - g_base) * 1000 / g_freq) + 100000; }
static DWORD     WINAPI H_GetTickCount()   { return (DWORD)VirtualMs(); }
static ULONGLONG WINAPI H_GetTickCount64() { return VirtualMs(); }
static DWORD     WINAPI H_timeGetTime()    { return (DWORD)VirtualMs(); }
static void WINAPI H_GetSystemTimeAsFileTime(LPFILETIME ft) {
    ULONGLONG v = g_wallBase + (ULONGLONG)((VirtualQpc() - g_base) * 10000000 / g_freq);
    ft->dwLowDateTime = (DWORD)v;
    ft->dwHighDateTime = (DWORD)(v >> 32);
}

// From DllMain (fixed_timestep=1, engine clock override not used): start virtual
// time at 0 and hook the exe's clock imports. g_hooksInstalled counts every PatchIAT
// hook so far (in netplay the keyboard/mouse block hooks imports first), so only the
// ones made here count towards g_clockHooked and the "no clock imports" warning.
void InstallClockHooks() {
    int before = g_hooksInstalled;
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    g_freq = li.QuadPart;
    QueryPerformanceCounter(&li);
    g_base = li.QuadPart;
    SYSTEMTIME st = {};
    st.wYear = 2026; st.wMonth = 1; st.wDay = 1;
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    g_wallBase = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;

    g_tick = g_freq / 1000000;
    if (g_tick < 1) g_tick = 1;
    timeBeginPeriod(1);   // 1 ms Sleep granularity for frame pacing (per-process on Windows 10 2004+)
    g_vt = 0;
    g_frameStartVt = 0;

    HMODULE exe = GetModuleHandleW(NULL);
    PatchIAT(exe, "QueryPerformanceCounter", (void*)H_QueryPerformanceCounter);
    PatchIAT(exe, "Sleep",                   (void*)H_Sleep);
    PatchIAT(exe, "GetTickCount",            (void*)H_GetTickCount);
    PatchIAT(exe, "GetTickCount64",          (void*)H_GetTickCount64);
    PatchIAT(exe, "timeGetTime",             (void*)H_timeGetTime);
    if (g_cfg.freezeWallclock) {
        PatchIAT(exe, "GetSystemTimeAsFileTime",        (void*)H_GetSystemTimeAsFileTime);
        PatchIAT(exe, "GetSystemTimePreciseAsFileTime", (void*)H_GetSystemTimeAsFileTime);
    }
    int hooked = g_hooksInstalled - before;
    Log("fixed timestep: %d clock imports hooked, fps=%.2f (perFrame=%lld ticks)",
        hooked, g_cfg.fps, (LONGLONG)((double)g_freq / g_cfg.fps));
    if (hooked == 0)
        Log("WARNING: no clock imports found in the exe IAT; the game may read time another way (rdtsc, delay-load, static CRT)");
    g_clockHooked = hooked > 0;
}
