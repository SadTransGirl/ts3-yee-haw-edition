// xinput_proxy.cpp -- determinism test harness.
//
// A drop-in xinput1_4.dll. The game loads this instead of the system copy
// because the game folder is first in the DLL search order. Every export is
// forwarded to the real System32\xinput1_4.dll; XInputGetState (and the
// undocumented XInputGetStateEx, ordinal 100) are additionally recorded to,
// or replayed from, a file. Optionally the game's clock is replaced by a
// virtual clock that advances a fixed amount per polled frame, and the game
// window is captured every N frames so two runs can be diffed.
//
// This module: loading the real System32 xinput1_4.dll, the XInputGetState dispatch (record,
// replay, netplay, passthrough), the XInput routing diagnostic, the ini, opening and closing a
// run, DllMain and every export. The rest of the harness lives in the other src\*.cpp modules,
// which share their declarations through proxy_shared.h.
//
// The same DLL is also the whole Yee Haw Edition mod: online co-op (mode=netplay), graphics
// patches, the debug menu and the content restores are all switched on from DllMain here,
// according to xinput_harness.ini. Entry points:
//   DllMain                  at load: reads the ini, applies every patch the settings ask for
//                            (before the game's own code runs), and at unload closes the run.
//   Game_XInputGetState etc. the game's own XInput calls (by ordinal). Each poll of the anchor
//                            pad (the first one the game ever polled) is the harness's
//                            per-frame hook: NoteCall in harness_core.cpp counts the frame,
//                            and in netplay NetGetState runs NetFrame.
//   XInputGetState etc.      named pass-throughs the proxy itself reads the real pad through.

#include "proxy_shared.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "bcrypt.lib")

// ------------------------------------------------------------------ real DLL
//
// R holds Windows' own xinput1_4.dll and its exports. Ordinals 100-109 are undocumented
// (100 is XInputGetStateEx, which also reports the Guide button); the proxy passes them
// through as four opaque arguments.

typedef DWORD (WINAPI* PFN_GetState)(DWORD, XINPUT_STATE*);
typedef DWORD (WINAPI* PFN_SetState)(DWORD, XINPUT_VIBRATION*);
typedef DWORD (WINAPI* PFN_GetCaps)(DWORD, DWORD, XINPUT_CAPABILITIES*);
typedef void  (WINAPI* PFN_Enable)(BOOL);
typedef DWORD (WINAPI* PFN_Battery)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*);
typedef DWORD (WINAPI* PFN_Keystroke)(DWORD, DWORD, PXINPUT_KEYSTROKE);
typedef DWORD (WINAPI* PFN_AudioIds)(DWORD, LPWSTR, UINT*, LPWSTR, UINT*);
typedef DWORD (WINAPI* PFN_Generic4)(UINT_PTR, UINT_PTR, UINT_PTR, UINT_PTR);

static struct {
    HMODULE       mod;
    PFN_GetState  GetState;
    PFN_SetState  SetState;
    PFN_GetCaps   GetCaps;
    PFN_Enable    Enable;
    PFN_Battery   Battery;
    PFN_Keystroke Keystroke;
    PFN_AudioIds  AudioIds;
    PFN_Generic4  ord100, ord101, ord102, ord103, ord104, ord108, ord109;
} R;

static INIT_ONCE g_realOnce = INIT_ONCE_STATIC_INIT;

// Load the real xinput1_4.dll by its full System32 path (a bare name would find this proxy
// again, first in the search order) and look up every export. Always returns TRUE, so a
// failed load isn't retried: R's pointers stay NULL and each export says "not connected".
static BOOL CALLBACK InitReal(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\xinput1_4.dll");
    R.mod = LoadLibraryW(path);
    if (!R.mod) { Log("FATAL: cannot load real %ls (err %lu)", path, GetLastError()); return TRUE; }
    R.GetState  = (PFN_GetState) GetProcAddress(R.mod, "XInputGetState");
    R.SetState  = (PFN_SetState) GetProcAddress(R.mod, "XInputSetState");
    R.GetCaps   = (PFN_GetCaps)  GetProcAddress(R.mod, "XInputGetCapabilities");
    R.Enable    = (PFN_Enable)   GetProcAddress(R.mod, "XInputEnable");
    R.Battery   = (PFN_Battery)  GetProcAddress(R.mod, "XInputGetBatteryInformation");
    R.Keystroke = (PFN_Keystroke)GetProcAddress(R.mod, "XInputGetKeystroke");
    R.AudioIds  = (PFN_AudioIds) GetProcAddress(R.mod, "XInputGetAudioDeviceIds");
    R.ord100 = (PFN_Generic4)GetProcAddress(R.mod, MAKEINTRESOURCEA(100));
    R.ord101 = (PFN_Generic4)GetProcAddress(R.mod, MAKEINTRESOURCEA(101));
    R.ord102 = (PFN_Generic4)GetProcAddress(R.mod, MAKEINTRESOURCEA(102));
    R.ord103 = (PFN_Generic4)GetProcAddress(R.mod, MAKEINTRESOURCEA(103));
    R.ord104 = (PFN_Generic4)GetProcAddress(R.mod, MAKEINTRESOURCEA(104));
    R.ord108 = (PFN_Generic4)GetProcAddress(R.mod, MAKEINTRESOURCEA(108));
    R.ord109 = (PFN_Generic4)GetProcAddress(R.mod, MAKEINTRESOURCEA(109));
    Log("real xinput1_4.dll loaded at %p", (void*)R.mod);
    return TRUE;
}

// Every export calls this first: InitReal runs once, on whichever thread gets here first,
// and later callers wait for it to finish.
static void EnsureReal() { InitOnceExecuteOnce(&g_realOnce, InitReal, NULL, NULL); }

// Every pad poll the game makes lands here (kind 0 = XInputGetState, 1 = XInputGetStateEx).
// Under g_cs it counts the call (NoteCall: the anchor poll starts a new frame), then fills
// st by mode: netplay's pads (NetGetState), the recording's, or the real pad (recorded in
// record mode). Returns the XInput result the game sees.
static DWORD HandleGetState(int kind, DWORD user, XINPUT_STATE* st) {
    EnsureReal();
    if (!st) return ERROR_BAD_ARGUMENTS;
    EnterCriticalSection(&g_cs);
    NoteCall(kind, user);
    DWORD res;
    if (g_cfg.mode == MODE_NETPLAY) {
        // The anchor poll also runs the lockstep frame step (NetFrame, via NetGetState).
        res = NetGetState(user, st, kind == g_anchorKind && (int)user == g_anchorUser);
    } else if (g_cfg.mode == MODE_REPLAY && !g_frameLive) {
        // Loading, or holding at a recorded load: nothing consumed; the pad
        // keeps reporting its last state, as a controller left alone does.
        if (user < 4) {
            *st = g_lastState[user];
            res = g_lastResult[user];
        } else {
            ZeroMemory(st, sizeof *st);
            res = ERROR_DEVICE_NOT_CONNECTED;
        }
    } else if (g_cfg.mode == MODE_REPLAY) {
        res = ReplayState(kind, user, st);
        if (user < 4) { g_lastState[user] = *st; g_lastResult[user] = res; }
    } else {
        // Passthrough and record: the real pad, then the debug menu's filter (it hides the
        // pad from the game while the menu is open). Record appends what the game got.
        if (kind == 1 && R.ord100)   res = R.ord100((UINT_PTR)user, (UINT_PTR)st, 0, 0);
        else                         res = XInputGetState(user, st);     // via Steam Input if it hooked us
        if (res != ERROR_SUCCESS) ZeroMemory(st, sizeof *st);
        DebugMenu_FilterPad(user, st, kind == g_anchorKind && (int)user == g_anchorUser);
        if (g_cfg.mode == MODE_RECORD && g_recFile) {
            Rec r = {};
            r.seq = g_seq; r.frame = g_logical; r.kind = (uint8_t)kind; r.user = (uint8_t)user;
            r.flags = g_frameLive ? 0 : REC_LOADING;
            r.result = res; r.state = *st;
            fwrite(&r, sizeof r, 1, g_recFile);
            if ((g_frame % 60) == 0) fflush(g_recFile);
        }
    }
    if (g_cfg.logCalls)
        Log("seq=%u frame=%u kind=%d user=%lu res=%lu pkt=%lu btn=%04x lt=%u rt=%u lx=%d ly=%d rx=%d ry=%d",
            g_seq, g_frame, kind, user, res, st->dwPacketNumber, st->Gamepad.wButtons,
            st->Gamepad.bLeftTrigger, st->Gamepad.bRightTrigger, st->Gamepad.sThumbLX,
            st->Gamepad.sThumbLY, st->Gamepad.sThumbRX, st->Gamepad.sThumbRY);
    g_seq++;
    LeaveCriticalSection(&g_cs);
    return res;
}

// ------------------------------------------------------------------ XInput routing diagnostic
//
// Steam Input (and overlays) can hook XInput inside the game in different
// ways, and only some of them leave this proxy in the path. This logs where
// the game's XInput calls actually go: the game's import slots, whether this
// proxy's own exports were patched, and whether Windows' xinput1_4.dll was.

static HMODULE g_self;
static BYTE    g_selfEntry[6][16];
static const char* kRouteNames[3] = { "XInputGetState", "XInputSetState", "XInputGetCapabilities" };

// Describe a code address for the log: "address in module.dll", marked when it's this proxy.
static void ModuleOf(const void* addr, char* out, size_t cap) {
    HMODULE m = NULL;
    if (!addr || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     (LPCWSTR)addr, &m)) {
        snprintf(out, cap, "%p (not inside any loaded module: generated code)", addr);
        return;
    }
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(m, path, MAX_PATH);
    const wchar_t* base = wcsrchr(path, L'\\');
    snprintf(out, cap, "%p in %ls%s", addr, base ? base + 1 : path, m == g_self ? " (this proxy)" : "");
}

// If the code at p starts with a jump, where does it go?
static const void* JumpTarget(const BYTE* p) {
    __try {
        if (p[0] == 0xE9) return p + 5 + *(int32_t*)(p + 1);                              // jmp rel32
        if (p[0] == 0xFF && p[1] == 0x25) return *(void**)(p + 6 + *(int32_t*)(p + 2));   // jmp [rip+d]
        if (p[0] == 0x48 && p[1] == 0xB8 && p[10] == 0xFF && p[11] == 0xE0) return *(void**)(p + 2);   // mov rax,imm; jmp rax
        if (p[0] == 0x49 && p[1] == 0xBB && p[10] == 0x41 && p[11] == 0xFF && p[12] == 0xE3) return *(void**)(p + 2);  // mov r11; jmp r11
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return NULL;
}

// From DllMain, before anything can hook us: keep the first 16 bytes of this proxy's six
// XInput entry points (the three named exports, then ordinals 2-4), so LogXInputRouting can
// tell later whether someone patched them.
static void CaptureSelfEntries(HINSTANCE inst) {
    g_self = inst;
    for (int i = 0; i < 3; i++) {
        if (const void* f = (const void*)GetProcAddress(inst, kRouteNames[i])) memcpy(g_selfEntry[i], f, 16);
        if (const void* f = (const void*)GetProcAddress(inst, MAKEINTRESOURCEA(2 + i))) memcpy(g_selfEntry[3 + i], f, 16);
    }
}

// Logged at the first poll, about 10 s in (poll 600) and at exit; "when" says which.
void LogXInputRouting(const char* when) {
    char a[MAX_PATH + 64], b[MAX_PATH + 64];
    Log("xinput routing (%s):", when);
    // 1. The game's import slots for xinput1_4.dll (by ordinal: 2 GetState, 3 SetState, 4 GetCapabilities).
    // Walks the exe's import table (PE format): OriginalFirstThunk lists what was imported,
    // FirstThunk holds the addresses actually called.
    BYTE* base = (BYTE*)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); dir.VirtualAddress && imp->Name; imp++) {
        if (_stricmp((const char*)(base + imp->Name), "xinput1_4.dll") || !imp->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA* funcs = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, funcs++) {
            ULONGLONG ord = IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal) ? IMAGE_ORDINAL(names->u1.Ordinal) : 0;
            ModuleOf((const void*)funcs->u1.Function, a, sizeof a);
            Log("  game import xinput1_4 #%llu -> %s", ord, a);
        }
    }
    // 2. This proxy's own exports: patched since startup?
    for (int k = 0; k < 6; k++) {
        int i = k % 3;
        const BYTE* f = (const BYTE*)(k < 3 ? GetProcAddress(g_self, kRouteNames[i]) : GetProcAddress(g_self, MAKEINTRESOURCEA(2 + i)));
        if (!f) continue;
        char what[96];
        if (k < 3) snprintf(what, sizeof what, "named %s (Steam Input's hook point)", kRouteNames[i]);
        else       snprintf(what, sizeof what, "game-facing #%d handler (%s)", 2 + i, kRouteNames[i]);
        if (!memcmp(f, g_selfEntry[k], 16)) { Log("  proxy %s: unchanged", what); continue; }
        ModuleOf(JumpTarget(f), a, sizeof a);
        Log("  proxy %s: PATCHED since startup; now jumps to %s", what, a);
    }
    // 3. Windows' own xinput1_4.dll, which this proxy calls for the real pad.
    if (R.GetState) {
        const void* t = JumpTarget((const BYTE*)R.GetState);
        ModuleOf((const void*)R.GetState, a, sizeof a);
        if (t) { ModuleOf(t, b, sizeof b); Log("  system XInputGetState %s: PATCHED, jumps to %s", a, b); }
        else Log("  system XInputGetState %s: not patched", a);
    }
    // 4. Is Steam's overlay / input code loaded?
    const wchar_t* steamMods[] = { L"GameOverlayRenderer64.dll", L"steaminput64.dll", L"SteamInput.dll" };
    for (auto m : steamMods) if (GetModuleHandleW(m)) Log("  loaded: %ls", m);
}

// ------------------------------------------------------------------ setup

// Read xinput_harness.ini (beside the game's exe) into g_cfg, g_net and g_gfx; called first
// thing in DllMain. A missing ini leaves every default (passthrough, no patches), except
// that Steam's "Join Game" and the one-PC co-op test override some values from the command line.
static void LoadConfig() {
    GetModuleFileNameW(NULL, g_exeDir, MAX_PATH);
    wchar_t* slash = wcsrchr(g_exeDir, L'\\');
    if (slash) *slash = 0;

    wchar_t ini[MAX_PATH];
    swprintf_s(ini, L"%s\\xinput_harness.ini", g_exeDir);
    wchar_t buf[64];
    GetPrivateProfileStringW(L"harness", L"mode", L"passthrough", buf, 64, ini);
    if (!_wcsicmp(buf, L"record"))      g_cfg.mode = MODE_RECORD;
    else if (!_wcsicmp(buf, L"replay")) g_cfg.mode = MODE_REPLAY;
    else if (!_wcsicmp(buf, L"netplay")) g_cfg.mode = MODE_NETPLAY;
    else                                g_cfg.mode = MODE_PASSTHROUGH;
    GetPrivateProfileStringW(L"harness", L"input",  L"run1", g_cfg.input,  64, ini);
    GetPrivateProfileStringW(L"harness", L"output", L"",     g_cfg.output, 64, ini);
    g_cfg.fixedTimestep   = GetPrivateProfileIntW(L"harness", L"fixed_timestep",      0,   ini);
    g_cfg.freezeWallclock = GetPrivateProfileIntW(L"harness", L"freeze_wallclock",    0,   ini);
    g_cfg.shotInterval    = GetPrivateProfileIntW(L"harness", L"screenshot_interval", 300, ini);
    g_cfg.shotMax         = GetPrivateProfileIntW(L"harness", L"screenshot_max",      200, ini);
    g_cfg.logCalls        = GetPrivateProfileIntW(L"harness", L"log_calls",           0,   ini);
    GetPrivateProfileStringW(L"harness", L"fps", L"60", buf, 64, ini);
    g_cfg.fps = _wtof(buf);
    if (g_cfg.fps < 1.0) g_cfg.fps = 60.0;

    g_cfg.ioLog           = GetPrivateProfileIntW(L"harness", L"io_log",            1,    ini);
    g_cfg.loadDetect      = GetPrivateProfileIntW(L"harness", L"load_detect",       1,    ini);
    g_cfg.loadWindow      = GetPrivateProfileIntW(L"harness", L"load_window",       8,    ini);
    g_cfg.loadEnterKb     = GetPrivateProfileIntW(L"harness", L"load_enter_kb",     2048, ini);
    g_cfg.loadQuietKb     = GetPrivateProfileIntW(L"harness", L"load_quiet_kb",     16,   ini);
    g_cfg.loadQuietFrames = GetPrivateProfileIntW(L"harness", L"load_quiet_frames", 30,   ini);
    g_cfg.loadHitchMs     = GetPrivateProfileIntW(L"harness", L"load_hitch_ms",     0,    ini);
    g_cfg.loadWaitFrames  = GetPrivateProfileIntW(L"harness", L"load_wait_frames",  3600, ini);
    g_cfg.useSrc[0]       = GetPrivateProfileIntW(L"harness", L"load_use_exe",      1,    ini);
    g_cfg.useSrc[1]       = GetPrivateProfileIntW(L"harness", L"load_use_crt",      1,    ini);
    g_cfg.useSrc[2]       = GetPrivateProfileIntW(L"harness", L"load_use_bink",     0,    ini);
    g_cfg.useSrc[3]       = GetPrivateProfileIntW(L"harness", L"load_use_fmod",     0,    ini);
    g_cfg.saveRestore     = GetPrivateProfileIntW(L"harness", L"save_restore",      1,    ini);
    g_cfg.testSave        = GetPrivateProfileIntW(L"harness", L"test_save",         0,    ini);
    g_cfg.engineClock     = GetPrivateProfileIntW(L"harness", L"engine_clock",      1,    ini);
    g_cfg.randIsolate     = GetPrivateProfileIntW(L"harness", L"rand_isolate",      1,    ini);
    g_cfg.mtTrace         = GetPrivateProfileIntW(L"harness", L"mt_trace",          1,    ini);
    g_cfg.spawnGate       = GetPrivateProfileIntW(L"harness", L"spawn_gate",        1,    ini);
    // [netplay]: online co-op (see NetCfg in proxy_shared.h for what each value does).
    {
        wchar_t v[64];
        GetPrivateProfileStringW(L"netplay", L"role", L"host", v, 64, ini);
        g_net.role = !_wcsicmp(v, L"client") ? 1 : 0;
        GetPrivateProfileStringW(L"netplay", L"transport", L"steam", v, 64, ini);
        g_net.transport = !_wcsicmp(v, L"udp") ? 0 : 1;
        GetPrivateProfileStringW(L"netplay", L"peer", L"127.0.0.1", g_net.peerHost, 64, ini);
        GetPrivateProfileStringW(L"netplay", L"peer_steamid", L"0", v, 64, ini);
        g_net.peerSteamId = _wcstoui64(v, NULL, 10);
        GetPrivateProfileStringW(L"netplay", L"start", L"combo", v, 64, ini);
        g_net.startImmediate = !_wcsicmp(v, L"immediate");
        g_net.localPort     = GetPrivateProfileIntW(L"netplay", L"local_port", 47001, ini);
        g_net.peerPort      = GetPrivateProfileIntW(L"netplay", L"peer_port", 47002, ini);
        g_net.delay         = GetPrivateProfileIntW(L"netplay", L"input_delay", 4, ini);
        g_net.timeoutMs     = GetPrivateProfileIntW(L"netplay", L"timeout_ms", 10000, ini);
        g_net.hashInterval  = GetPrivateProfileIntW(L"netplay", L"hash_interval", 60, ini);
        g_net.repair        = GetPrivateProfileIntW(L"netplay", L"repair", -1, ini);       // -1: default (on; off for stand-in tests)
        g_net.repairLead    = GetPrivateProfileIntW(L"netplay", L"repair_lead", 30, ini);
        g_net.rngCheck      = GetPrivateProfileIntW(L"netplay", L"rng_check", 1, ini);
        g_net.rngCatchUp    = GetPrivateProfileIntW(L"netplay", L"rng_catch_up", 1, ini);
        if (g_net.repairLead < 8) g_net.repairLead = 8;
        if (g_net.repairLead > 240) g_net.repairLead = 240;
        g_net.testDesyncKeys = GetPrivateProfileIntW(L"netplay", L"test_desync_keys", 0, ini);
        g_net.repairFollowers = GetPrivateProfileIntW(L"netplay", L"repair_followers", 0, ini);
        g_net.localPad      = GetPrivateProfileIntW(L"netplay", L"local_pad", 0, ini);
        g_net.simLatencyMs  = GetPrivateProfileIntW(L"netplay", L"sim_latency_ms", 0, ini);
        g_net.simLossPct    = GetPrivateProfileIntW(L"netplay", L"sim_loss_pct", 0, ini);
        g_net.testInputSeed = (uint32_t)GetPrivateProfileIntW(L"netplay", L"test_input_seed", 0, ini);
        g_net.testRejoinMs  = GetPrivateProfileIntW(L"netplay", L"test_rejoin_ms", 0, ini);
        g_net.dropIn        = GetPrivateProfileIntW(L"netplay", L"drop_in", 1, ini);
        g_net.testDrop      = GetPrivateProfileIntW(L"netplay", L"test_drop", 0, ini);
        g_net.testActionFrame = GetPrivateProfileIntW(L"netplay", L"test_action_frame", 0, ini);
        GetPrivateProfileStringW(L"netplay", L"dropin_launch", L"auto", g_dropLaunchHow, 16, ini);
        g_netSaveSync       = GetPrivateProfileIntW(L"netplay", L"save_sync", 1, ini);
        g_net.ownScreen     = GetPrivateProfileIntW(L"netplay", L"own_screen", 0, ini);   // off until cameras stay identical
        g_net.skipHidden    = GetPrivateProfileIntW(L"netplay", L"own_screen_skip_hidden", 1, ini);
        // Started from Steam's "Join Game" or an invite: join that host, in online co-op whatever
        // the ini's mode (a friend set to solo still gets in), with the fixed timestep it needs.
        if (const wchar_t* k = wcsstr(GetCommandLineW(), L"+tsnet_host")) {
            uint64_t host = _wcstoui64(k + 11, NULL, 10);        // the host's Steam ID, after the 11-character key
            if (host) {
                g_net.role = 1; g_net.transport = 1; g_net.peerSteamId = host; g_netLaunchedByJoin = true;
                g_cfg.mode = MODE_NETPLAY;
                g_cfg.fixedTimestep = 1;
            }
        }
        // One-PC co-op test: two copies of the game on this PC, over the loopback.
        if (const wchar_t* k = wcsstr(GetCommandLineW(), L"+tsnet_local")) {
            g_localTest = wcsstr(k, L"client") ? 1 : 0;
            g_cfg.mode = MODE_NETPLAY;
            g_cfg.fixedTimestep = 1; g_cfg.engineClock = 1; g_cfg.randIsolate = 1; g_cfg.spawnGate = 1;
            g_net.role = g_localTest;
            g_net.transport = 0;
            wcscpy_s(g_net.peerHost, L"127.0.0.1");
            int base = GetPrivateProfileIntW(L"netplay", L"local_test_port", 47101, ini);
            g_net.localPort = g_localTest ? base + 1 : base;
            g_net.peerPort  = g_localTest ? base : base + 1;
            g_net.peerSteamId = 0;
            g_net.simLatencyMs = GetPrivateProfileIntW(L"netplay", L"local_test_latency_ms", 0, ini);
            g_net.simLossPct = 0;
            g_localHostPad = GetPrivateProfileIntW(L"netplay", L"local_test_host_pad", 0, ini) & 3;
            g_localClientPad = GetPrivateProfileIntW(L"netplay", L"local_test_client_pad", 1, ini) & 3;
            g_net.localPad = g_localTest ? g_localClientPad : g_localHostPad;
            g_net.testInputSeed = 0;
            g_net.startImmediate = 0;
            GetPrivateProfileStringW(L"netplay", L"local_test_controllers", L"auto", v, 64, ini);
            g_localControllers = !_wcsicmp(v, L"1") ? 1 : !_wcsicmp(v, L"2") ? 2 : 0;
            GetPrivateProfileStringW(L"netplay", L"local_test_client", L"pad", v, 64, ini);
            if (g_localTest == 1 && !_wcsicmp(v, L"bot")) {
                g_net.testInputSeed = 0x5EED;              // random presses instead of a second controller
                g_net.startImmediate = 1;                  // the bot is ready at once; the host still holds Back+Start
            }
            wcscpy_s(g_cfg.input, g_localTest ? L"local_client" : L"local_host");   // run folder name
        }
        if (g_net.repair < 0) g_net.repair = (g_net.testInputSeed && g_localTest < 0) ? 0 : 1;   // stand-in tests expect a desync to end the session
        if (g_net.delay < 1) g_net.delay = 1;
        if (g_net.delay > 60) g_net.delay = 60;
        if (g_net.localPad < 0 || g_net.localPad > 3) g_net.localPad = 0;
    }
    g_cfg.spawnDelay      = GetPrivateProfileIntW(L"harness", L"spawn_delay",       15,   ini);
    // On by default in netplay (two PCs differ in speed), off otherwise; the ini can force either.
    g_cfg.syncLoader      = GetPrivateProfileIntW(L"harness", L"sync_loader", g_cfg.mode == MODE_NETPLAY ? 1 : 0, ini);
    g_cfg.audioWait       = GetPrivateProfileIntW(L"harness", L"audio_wait",  g_cfg.mode == MODE_NETPLAY ? 1 : 0, ini);
    if (g_cfg.spawnDelay < 1) g_cfg.spawnDelay = 1;
    if (g_cfg.spawnDelay > 600) g_cfg.spawnDelay = 600;
    // Optional address overrides (hex RVAs) for the spawn gate, the engine RNG and the engine
    // clock; an empty value keeps the built-in address.
    {
        struct { const wchar_t* key; DWORD* rva; } sp[] = {
            { L"spawn_rva_call", &RVA_SPAWN_CALL }, { L"spawn_rva_update", &RVA_SPAWN_UPDATE },
            { L"spawn_rva_force", &RVA_SPAWN_FORCE }, { L"spawn_rva_loader", &RVA_SPAWN_LOADER } };
        for (auto& r : sp) {
            wchar_t v[32];
            GetPrivateProfileStringW(L"harness", r.key, L"", v, 32, ini);
            if (v[0]) *r.rva = wcstoul(v, NULL, 16);
        }
    }
    {
        struct { const wchar_t* key; DWORD* rva; } mt[] = {
            { L"mt_rva_next", &RVA_MT_NEXT }, { L"mt_rva_refill", &RVA_MT_REFILL }, { L"mt_rva_global", &RVA_MT_GLOBAL },
            { L"mt_rva_helpers_lo", &RVA_MT_HELPERS_LO }, { L"mt_rva_helpers_hi", &RVA_MT_HELPERS_HI } };
        for (auto& r : mt) {
            wchar_t v[32];
            GetPrivateProfileStringW(L"harness", r.key, L"", v, 32, ini);
            if (v[0]) *r.rva = wcstoul(v, NULL, 16);
        }
    }
    struct { const wchar_t* key; DWORD* rva; } rvas[] = {
        { L"engine_rva_clock", &RVA_CLOCK_FN }, { L"engine_rva_flag", &RVA_OVR_FLAG },
        { L"engine_rva_ms", &RVA_OVR_MS },      { L"engine_rva_tick_call", &RVA_TICK_CALL } };
    for (auto& r : rvas) {
        wchar_t v[32];
        GetPrivateProfileStringW(L"harness", r.key, L"", v, 32, ini);
        if (v[0]) *r.rva = wcstoul(v, NULL, 16);
    }
    // The game's save folder, which record snapshots and replay restores (HandleSaves).
    wchar_t raw[MAX_PATH];
    GetPrivateProfileStringW(L"harness", L"save_dir",
                             L"%APPDATA%\\Disney\\Toy Story 3 Complete Edition\\savedata", raw, MAX_PATH, ini);
    if (!raw[0]) wcscpy_s(raw, L"%APPDATA%\\Disney\\Toy Story 3 Complete Edition\\savedata");
    ExpandEnvironmentStringsW(raw, g_cfg.saveDir, MAX_PATH);
    if (g_localTest == 1) {                               // player 2's own save folder (see SHGetFolderPathA)
        ExpandEnvironmentStringsW(L"%APPDATA%", raw, MAX_PATH);
        swprintf_s(g_cfg.saveDir, L"%s%s\\Disney\\Toy Story 3 Complete Edition\\savedata", raw, kLocalTestRoot);
    }
    if (g_localTest >= 0) g_cfg.saveRestore = 0;          // two copies share nothing; leave saves alone
    if (g_cfg.loadQuietFrames < 1) g_cfg.loadQuietFrames = 1;

    // [graphics]: an empty or missing value leaves the game's own setting.
    auto readNum = [&](const wchar_t* key, double* out) {
        wchar_t v[32];
        GetPrivateProfileStringW(L"graphics", key, L"", v, 32, ini);
        if (!v[0]) return false;
        *out = _wtof(v);
        return true;
    };
    g_gfx.enabled = GetPrivateProfileIntW(L"graphics", L"enabled", 0, ini);
    readNum(L"fps_limit", &g_gfx.fpsLimit);
    g_gfx.hasCullPop  = readNum(L"cull_pop",  &g_gfx.cullPop);
    g_gfx.hasCullFade = readNum(L"cull_fade", &g_gfx.cullFade);
    g_gfx.hasShadowCull[0] = readNum(L"shadow_cull_near", &g_gfx.shadowCull[0]);
    g_gfx.hasShadowCull[1] = readNum(L"shadow_cull_mid",  &g_gfx.shadowCull[1]);
    g_gfx.hasShadowCull[2] = readNum(L"shadow_cull_far",  &g_gfx.shadowCull[2]);
    readNum(L"lod_scale", &g_gfx.lodScale);
    readNum(L"object_fade_scale", &g_gfx.objectFadeScale);
    readNum(L"world_lod_scale", &g_gfx.worldLodScale);
    g_gfx.fullResReflection = GetPrivateProfileIntW(L"graphics", L"full_res_reflection", 0, ini);
    g_gfx.fullResGlow       = GetPrivateProfileIntW(L"graphics", L"full_res_glow",       0, ini);
    g_gfx.shadowAllZones    = GetPrivateProfileIntW(L"graphics", L"shadow_all_zones",    0, ini);
    g_gfx.shadowNoContrib   = GetPrivateProfileIntW(L"graphics", L"shadow_no_contrib",   0, ini);
    g_gfx.shadowCasterLog   = GetPrivateProfileIntW(L"graphics", L"shadow_caster_log",   0, ini);
    readNum(L"shadow_near_extend", &g_gfx.shadowNearExtend);
    readNum(L"shadow_far", &g_gfx.shadowFar);
    readNum(L"shadow_cascades", &g_gfx.shadowCascades);
    readNum(L"shadow_split", &g_gfx.shadowSplit);
}

// Copy the files (not subfolders) of src into dst, creating dst. Returns the
// number copied, or -1 if src doesn't exist. Plain kernel32 calls only, so it
// is safe this early (DllMain, before the game has read its save).
int CopyDirFiles(const wchar_t* src, const wchar_t* dst) {
    wchar_t pat[MAX_PATH];
    swprintf_s(pat, L"%s\\*", src);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    CreateDirectoryW(dst, NULL);
    int n = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        wchar_t a[MAX_PATH], b[MAX_PATH];
        swprintf_s(a, L"%s\\%s", src, fd.cFileName);
        swprintf_s(b, L"%s\\%s", dst, fd.cFileName);
        if (CopyFileW(a, b, FALSE)) {
            n++;
            Log("  %ls (%lu bytes)", fd.cFileName, fd.nFileSizeLow);
        } else {
            Log("  FAILED to copy %ls (error %lu)", fd.cFileName, GetLastError());
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

// Make record and replay start from the same save. Record copies the game's save folder into
// the run (save_start); replay backs up the current save to its output folder (save_backup),
// then puts the recording's save in its place. Called from OpenRun, before the game reads it.
static void HandleSaves() {
    if (!g_cfg.saveRestore) { Log("save snapshot/restore off (save_restore=0)"); return; }
    wchar_t snap[MAX_PATH];
    if (g_cfg.mode == MODE_RECORD) {
        swprintf_s(snap, L"%s\\save_start", g_outDir);
        Log("saves: snapshotting %ls into the recording:", g_cfg.saveDir);
        int n = CopyDirFiles(g_cfg.saveDir, snap);
        if (n < 0) Log("saves: %ls not found; nothing to snapshot (fresh install?)", g_cfg.saveDir);
    } else if (g_cfg.mode == MODE_REPLAY) {
        swprintf_s(snap, L"%s\\save_start", g_inDir);
        if (GetFileAttributesW(snap) == INVALID_FILE_ATTRIBUTES) {
            Log("NOTE: this recording has no save snapshot (made before save restore existed). "
                "The replay starts from whatever save is on disk, so progress made since the "
                "recording (coins, items) will make it diverge.");
            return;
        }
        wchar_t backup[MAX_PATH];
        swprintf_s(backup, L"%s\\save_backup", g_outDir);
        Log("saves: backing up the current save to %ls:", backup);
        CopyDirFiles(g_cfg.saveDir, backup);
        CreateDirectoryW(g_cfg.saveDir, NULL);
        Log("saves: restoring the recording's starting save into %ls:", g_cfg.saveDir);
        CopyDirFiles(snap, g_cfg.saveDir);
    }
}

// A run folder is taken once it has a harness.log.
static bool DirHasResults(const wchar_t* dir) {
    wchar_t p[MAX_PATH];
    swprintf_s(p, L"%s\\harness.log", dir);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

// Set up this run's files (from DllMain, any mode but passthrough): pick the run folders under
// harness\runs (g_inDir is the one input= names, g_outDir a fresh one for this run's
// results), open harness.log, then create input.bin (record) or load the recording (replay),
// handle the saves and open frames.csv and io.csv. Falls back to passthrough if the
// recording can't be written or read.
static void OpenRun() {
    wchar_t runs[MAX_PATH];
    swprintf_s(runs, L"%s\\harness", g_exeDir);
    CreateDirectoryW(runs, NULL);
    wcscat_s(runs, L"\\runs");
    CreateDirectoryW(runs, NULL);
    swprintf_s(g_inDir, L"%s\\%s", runs, g_cfg.input);

    // Replay writes to <input>_replay (or output=), numbered _2, _3 ... if that's taken.
    if (g_cfg.mode == MODE_REPLAY) {
        wchar_t name[80];
        if (g_cfg.output[0]) wcscpy_s(name, g_cfg.output);
        else swprintf_s(name, L"%s_replay", g_cfg.input);
        swprintf_s(g_outDir, L"%s\\%s", runs, name);
        for (int n = 2; DirHasResults(g_outDir) && n < 1000; n++)
            swprintf_s(g_outDir, L"%s\\%s_%d", runs, name, n);
    } else {
        // Never overwrite a recording: launching again with mode=record
        // (e.g. meaning to replay) goes to <input>_2, _3 ... instead.
        wcscpy_s(g_outDir, g_inDir);
        for (int n = 2; DirHasResults(g_outDir) && n < 1000; n++)
            swprintf_s(g_outDir, L"%s_%d", g_inDir, n);
    }
    CreateDirectoryW(g_outDir, NULL);

    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\harness.log", g_outDir);
    _wfopen_s(&g_log, path, L"w");
    Log("mode=%s input=%ls output=%ls fixed_timestep=%d fps=%.2f freeze_wallclock=%d screenshots every %d frames (max %d)",
        g_cfg.mode == MODE_RECORD ? "record" : g_cfg.mode == MODE_REPLAY ? "replay" :
        g_cfg.mode == MODE_NETPLAY ? "netplay" : "passthrough",
        g_cfg.input, g_outDir, g_cfg.fixedTimestep, g_cfg.fps, g_cfg.freezeWallclock, g_cfg.shotInterval, g_cfg.shotMax);
    Log("load detection=%d: enter >= %d KB in %d frames, leave after %d frames under %d KB each, hitch_ms=%d, "
        "sources exe=%d crt=%d bink=%d fmod=%d, replay wait limit %d polls",
        g_cfg.loadDetect, g_cfg.loadEnterKb, g_cfg.loadWindow, g_cfg.loadQuietFrames, g_cfg.loadQuietKb,
        g_cfg.loadHitchMs, g_cfg.useSrc[0], g_cfg.useSrc[1], g_cfg.useSrc[2], g_cfg.useSrc[3], g_cfg.loadWaitFrames);

    if (g_cfg.mode == MODE_RECORD) {
        if (wcscmp(g_outDir, g_inDir) != 0)
            Log("NOTE: %ls already holds a recording, so this one goes to %ls. "
                "To replay it, set input= to that folder's name.", g_inDir, g_outDir);
        swprintf_s(path, L"%s\\input.bin", g_outDir);
        if (!_wfopen_s(&g_recFile, path, L"wb") && g_recFile) {
            FileHeader h = {};
            memcpy(h.magic, "XIH1", 4);
            h.version = 1;
            h.recSize = sizeof(Rec);
            h.flags = (g_cfg.fixedTimestep ? 1u : 0u) | (g_cfg.freezeWallclock ? 2u : 0u) | (g_cfg.loadDetect ? 4u : 0u)
                    | (g_engineClockReady ? 8u | 32u : 0u) | (g_cfg.randIsolate ? 16u : 0u)
                    | (g_spawnGateOn ? 128u : 0u);
            h.fps = g_cfg.fps;
            if (g_spawnGateOn) *(uint16_t*)h.reserved = (uint16_t)g_cfg.spawnDelay;
            fwrite(&h, sizeof h, 1, g_recFile);
            fflush(g_recFile);
        } else {
            Log("FATAL: cannot create %ls; falling back to passthrough", path);
            g_cfg.mode = MODE_PASSTHROUGH;
        }
    } else if (g_cfg.mode == MODE_REPLAY) {
        if (!LoadRecording()) g_cfg.mode = MODE_PASSTHROUGH;
    }
    if (g_cfg.mode != MODE_PASSTHROUGH) HandleSaves();
    if (g_cfg.mode != MODE_PASSTHROUGH && g_cfg.shotInterval > 0) {
        swprintf_s(path, L"%s\\frames.csv", g_outDir);
        if (!_wfopen_s(&g_framesCsv, path, L"w") && g_framesCsv) { fputs("frame,hash,width,height\n", g_framesCsv); fflush(g_framesCsv); }
    }
    if (g_cfg.mode != MODE_PASSTHROUGH && g_cfg.ioLog) {
        swprintf_s(path, L"%s\\io.csv", g_outDir);
        if (!_wfopen_s(&g_ioCsv, path, L"w") && g_ioCsv)
            fputs("poll,logical,state,ms,exe_kb,crt_kb,bink_kb,fmod_kb,reads,opens,ticks,mt\n", g_ioCsv);
        OpenFilesLog();
    }
}

// At unload (DLL_PROCESS_DETACH): log the run's summary (polls, frames, loads, clock, RNG,
// netplay) and close every file.
static void CloseRun() {
    EnterCriticalSection(&g_cs);
    Log("summary: %u calls, %u frames, %d screenshots, %u call-pattern mismatches",
        g_seq, g_frame, g_shotsTaken, g_mismatches);
    if (g_mismatches) Log("summary: first mismatch at frame %u", g_firstMismatchFrame);
    if (g_cfg.mode == MODE_REPLAY)
        Log("summary: consumed %zu of %zu recorded calls", g_replayPos, g_replayCount);
    Log("summary: %u logical frames, %u loads detected%s", g_logical, g_loadsCompleted,
        g_loading ? " (still loading at exit)" : "");
    if (g_engineClockReady) LogEngineClockSummary();
    LogRandSummary();
    WriteMtSummary();
    if (g_clockHooked) {
        timeEndPeriod(1);
        Log("summary: virtual clock at %.1f s, main thread %lu", (double)g_vt / g_freq, g_mainThread);
        if (g_stallAssists)
            Log("WARNING: virtual clock needed %ld stall assist(s): a worker thread waited on time while the "
                "main thread was blocked. This run may not be reproducible.", g_stallAssists);
    }
    if (g_cfg.mode == MODE_REPLAY)
        Log("summary: %u recorded loads matched, %u wait timeouts, %u unmatched loads",
            g_loadsConsumed - g_loadTimeouts, g_loadTimeouts, g_unmatchedLoads);
    if (g_ioCsv)     { fclose(g_ioCsv);     g_ioCsv = NULL; }
    if (g_anchorKind >= 0) LogXInputRouting("exit");
    NetSummary();
    CloseSpawnLog();
    CloseFilesLog();
    if (g_recFile)   { fclose(g_recFile);   g_recFile = NULL; }
    if (g_framesCsv) { fclose(g_framesCsv); g_framesCsv = NULL; }
    if (g_log)       { fclose(g_log);       g_log = NULL; }
    LeaveCriticalSection(&g_cs);
}

// The game imports this DLL statically, so DLL_PROCESS_ATTACH runs before any game code:
// the one moment every patch can go in before the code it changes first runs. In order:
// the ini, the run's files and the mode's patches (clock, spawn gate, netplay), then the
// graphics patches, then (only with an ini) the debug menu and content restores.
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_cs);
        CaptureSelfEntries(inst);
        LoadConfig();
        g_mainThread = GetCurrentThreadId();   // static imports load on the initial thread
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_realFreq = f.QuadPart;
        // Without an ini the proxy is a silent passthrough: no files, no hooks.
        if (g_cfg.mode == MODE_PASSTHROUGH) { g_cfg.ioLog = 0; g_cfg.loadDetect = 0; }
        if (g_cfg.mode != MODE_PASSTHROUGH) {
            // Decide before OpenRun so the recording header says which clock was used.
            g_engineClockReady = g_cfg.fixedTimestep && g_cfg.engineClock && VerifyEngineClock() && PatchEngineClock();
            g_spawnGateOn = g_cfg.spawnGate && VerifySpawnGate() && PatchSpawnGate();
            OpenRun();
            if (g_spawnGateOn) {
                OpenSpawnLog();
                Log("spawn gate: AsyncActorCreator_Update call at 0x%llx verified and redirected; spawns are placed "
                    "%d frames after they're queued (spawns.csv)", 0x140000000ULL + RVA_SPAWN_CALL, g_cfg.spawnDelay);
            } else if (g_cfg.spawnGate) {
                Log("spawn gate: the game's spawn code doesn't match this build; not gated");
            }
            // Netplay: every game patch a session needs goes in now. The connection itself
            // opens later, at the first poll (g_netWanted, picked up by NetFrame).
            if (g_cfg.mode == MODE_NETPLAY) {
                g_netWanted = true;
                if (!g_mtHooked) VerifyMt();
                g_mtReseedOk = g_mtRefill && g_mtGlobalPtr && VerifyMtSeed();
                g_attractOff = PatchAttractVideo();
                if (g_localTest >= 0) InstallLocalTestHooks();
                InstallDesktopBlock();
                g_noDesktopInput = PatchNoDesktopInput();
                Log("netplay: game's keyboard/mouse driver %s", g_noDesktopInput
                    ? "switched off (pads keep polling even when the window loses focus)"
                    : "NOT switched off (code doesn't match this build)");
                g_feHooked = HookFrontendWatch();
                g_movieFixOn = PatchMovieSkip();
                Log("netplay: movies %s", g_movieFixOn ? "kept in step (no input polls while one plays; either player skips for both)"
                                                       : "NOT handled (movie code doesn't match this build)");
                if (g_net.dropIn) {
                    bool pub = HookPublish();
                    Log("netplay: drop-in %s", pub ? "ready: a friend joining mid-level reloads it for both"
                                                   : "NOT available (level code doesn't match this build)");
                    if (!pub) g_net.dropIn = 0;
                }
                Log("netplay: title screen %s", g_feHooked ? "watched: sessions start (and restart) only from \"Press Start\""
                                                            : "NOT watched (code doesn't match this build); restart both games to play again");
                if (g_net.ownScreen) {
                    bool hud = PatchHudLayout();
                    Log("netplay: own-screen HUD %s", hud ? "ready (this player's HUD uses the full-screen layout)"
                                                          : "NOT available (HUD code doesn't match this build)");
                    Log("netplay: own-screen aim reticle %s", PatchReticleShow() ? "ready (the other player's is hidden on this PC)"
                                                                               : "NOT available (reticle code doesn't match this build)");
                    Log("netplay: own screen: %d/3 per-player menus (customization, wanted poster, compass) hidden on the other player's PC",
                        PatchOwnedScreens());
                }
                if (g_net.ownScreen) {
                    g_ownScreenReady = PatchViewportSetAll();
                    Log("netplay: own screen %s", g_ownScreenReady ? "ready: each PC will show only its own player"
                                                                    : "NOT available (viewport code doesn't match this build)");
                    if (g_ownScreenReady && g_net.skipHidden) {
                        bool skip = PatchJobExec();
                        if (!skip) g_net.skipHidden = 0;
                        Log("netplay: own screen: the other player's view %s", skip ? "will be built (culling, animation, streaming) but not rendered"
                                                                                    : "renders as before (frame-job code doesn't match this build)");
                    }
                }
                Log("netplay: title-screen attract video %s; engine RNG %s at lockstep start",
                    g_attractOff ? "turned off" : "NOT turned off (code doesn't match this build)",
                    g_mtReseedOk ? "will be reseeded" : "can't be reseeded (code doesn't match this build)");
                Log("netplay: %s, transport %s, input delay %d, connects on the first input poll",
                    g_net.role ? "client" : "host", g_net.transport ? "steam" : "udp", g_net.delay);
            }
            // fixed_timestep: the engine clock override chosen above, or else the virtual
            // QueryPerformanceCounter clock (harness_core.cpp).
            if (g_cfg.fixedTimestep) {
                if (g_engineClockReady) {
                    Log("engine clock: Clock_GetTimeMs at 0x%llx verified and replaced; GameTimer_Tick advances it "
                        "%.3f ms per frame, starting at %u ms", 0x140000000ULL + RVA_CLOCK_FN, 1000.0 / g_cfg.fps, kStartMs);
                } else {
                    if (g_cfg.engineClock)
                        Log("engine clock: the game's code doesn't match the expected override (different build?); "
                            "virtualising QueryPerformanceCounter instead");
                    InstallClockHooks();
                }
            }
            // The rest of determinism: per-site rand() streams, the patches in
            // net_determinism.cpp, and the engine RNG trace.
            if (g_cfg.randIsolate) InstallRandIsolation();
            InstallDeterminism();
            if (g_cfg.mtTrace) {
                if (VerifyMt() && PatchMt()) {
                    g_mtHooked = true;
                    OpenMtDrawFile();
                    Log("engine RNG: MT_Next at 0x%llx verified and traced (draws -> mt_draws.bin, mt_callers.csv)",
                        0x140000000ULL + RVA_MT_NEXT);
                } else {
                    Log("engine RNG: MT_Next doesn't match this build; not traced");
                }
            }
        }
        // Graphics patches work in every mode, including passthrough. They must
        // run here, before the game creates its render targets.
        if (g_gfx.enabled) {
            if (!g_log) OpenGraphicsLog();
            ApplyGraphicsPatches();
        }
        // The debug menu, the test save and the content restores (levels, scripts, vocal theme).
        // Each must go in before the game first reads what it changes, as noted per line.
        {
            wchar_t ini[MAX_PATH];
            swprintf_s(ini, L"%s\\xinput_harness.ini", g_exeDir);
            if (GetFileAttributesW(ini) != INVALID_FILE_ATTRIBUTES) {     // no ini: stay a silent passthrough
                if (!g_log) OpenGraphicsLog();
                DebugMenu_Install(ini, Log, g_cfg.mode == MODE_NETPLAY);
                if (GetPrivateProfileIntW(L"debug_menu", L"enabled", 1, ini)) InstallFreeCamCull();   // after the graphics patches
                if (g_cfg.testSave && g_localTest < 0 && !InstallTestSave()) g_cfg.testSave = 0;     // before the game reads its save
                DebugMenu_SetTestSave(g_cfg.testSave != 0 && g_localTest < 0 && g_saveApply);
                g_popFixWanted = GetPrivateProfileIntW(L"harness", L"audio_start_paused", 1, ini) != 0;
                TrySoundPopFix(false);                    // FMOD may not be loaded yet: retried at the first poll
                g_levelsWanted = GetPrivateProfileIntW(L"harness", L"restore_hidden_levels", 1, ini) != 0;
                if (g_levelsWanted) InstallHiddenLevels();   // before GameCommon reads FileList.txt
                g_scriptFixesWanted = GetPrivateProfileIntW(L"harness", L"script_fixes_2010", 0, ini) != 0;
                if (g_scriptFixesWanted) InstallScriptFixes();   // before the first script loads
                if (GetPrivateProfileIntW(L"audio", L"vocal_theme", 0, ini)) InstallVocalTheme(ini);
            }
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        CloseRun();
    }
    return TRUE;
}

// ------------------------------------------------------------------ exports

extern "C" {

// Game-facing handlers: exported by ordinal only (2/3/4, which is how the game
// imports them), so name-based hookers like Steam Input leave them alone and
// the proxy always sees the game's calls.
DWORD WINAPI Game_XInputGetState(DWORD dwUserIndex, XINPUT_STATE* pState) {
    return HandleGetState(0, dwUserIndex, pState);
}

// Ordinal 100, the undocumented XInputGetState that also reports the Guide button.
DWORD WINAPI XInputGetStateEx(DWORD dwUserIndex, XINPUT_STATE* pState) {
    return HandleGetState(1, dwUserIndex, pState);
}

// Rumble. In netplay each PC drives only its own player's motors (NetSetState).
DWORD WINAPI Game_XInputSetState(DWORD dwUserIndex, XINPUT_VIBRATION* pVibration) {
    EnsureReal();
    if (g_cfg.mode == MODE_NETPLAY) return NetSetState(dwUserIndex, pVibration);
    return XInputSetState(dwUserIndex, pVibration);
}

// Which pads are plugged in, and what they can do. In netplay both PCs answer the same way
// (NetGetCaps), so the game sees the same players on both.
DWORD WINAPI Game_XInputGetCapabilities(DWORD dwUserIndex, DWORD dwFlags, XINPUT_CAPABILITIES* pCaps) {
    EnsureReal();
    if (g_cfg.mode == MODE_NETPLAY) return NetGetCaps(dwUserIndex, pCaps);
    DWORD res = XInputGetCapabilities(dwUserIndex, dwFlags, pCaps);
    // During replay a pad that was present when recording must look present
    // even if nothing is plugged in now.
    if (res != ERROR_SUCCESS && g_cfg.mode == MODE_REPLAY && dwUserIndex < 4 && g_userSeen[dwUserIndex] && pCaps) {
        ZeroMemory(pCaps, sizeof *pCaps);
        pCaps->Type = XINPUT_DEVTYPE_GAMEPAD;
        pCaps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        pCaps->Gamepad.wButtons = 0xF3FF;
        pCaps->Gamepad.bLeftTrigger = pCaps->Gamepad.bRightTrigger = 0xFF;
        pCaps->Gamepad.sThumbLX = pCaps->Gamepad.sThumbLY = pCaps->Gamepad.sThumbRX = pCaps->Gamepad.sThumbRY = (SHORT)0xFFC0;
        pCaps->Vibration.wLeftMotorSpeed = pCaps->Vibration.wRightMotorSpeed = 0xFF;
        res = ERROR_SUCCESS;
    }
    return res;
}

// The remaining exports pass straight through to the real DLL (or say "not connected" if it
// didn't load). The undocumented ordinals 101-109 take their arguments as four opaque values.
void WINAPI XInputEnable(BOOL enable) {
    EnsureReal();
    if (R.Enable) R.Enable(enable);
}

DWORD WINAPI XInputGetBatteryInformation(DWORD dwUserIndex, BYTE devType, XINPUT_BATTERY_INFORMATION* pInfo) {
    EnsureReal();
    return R.Battery ? R.Battery(dwUserIndex, devType, pInfo) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetKeystroke(DWORD dwUserIndex, DWORD dwReserved, PXINPUT_KEYSTROKE pKeystroke) {
    EnsureReal();
    // Not recorded. In replay mode report "no event" so a plugged-in pad
    // cannot leak input into the run.
    if (g_cfg.mode == MODE_REPLAY) return ERROR_EMPTY;
    return R.Keystroke ? R.Keystroke(dwUserIndex, dwReserved, pKeystroke) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetAudioDeviceIds(DWORD dwUserIndex, LPWSTR pRenderId, UINT* pRenderCount, LPWSTR pCaptureId, UINT* pCaptureCount) {
    EnsureReal();
    return R.AudioIds ? R.AudioIds(dwUserIndex, pRenderId, pRenderCount, pCaptureId, pCaptureCount) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputWaitForGuideButton(UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d) {
    EnsureReal();
    return R.ord101 ? R.ord101(a, b, c, d) : ERROR_DEVICE_NOT_CONNECTED;
}
DWORD WINAPI XInputCancelGuideButtonWait(UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d) {
    EnsureReal();
    return R.ord102 ? R.ord102(a, b, c, d) : ERROR_DEVICE_NOT_CONNECTED;
}
DWORD WINAPI XInputPowerOffController(UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d) {
    EnsureReal();
    return R.ord103 ? R.ord103(a, b, c, d) : ERROR_DEVICE_NOT_CONNECTED;
}
DWORD WINAPI XInputGetBaseBusInformation(UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d) {
    EnsureReal();
    return R.ord104 ? R.ord104(a, b, c, d) : ERROR_DEVICE_NOT_CONNECTED;
}
DWORD WINAPI XInputGetCapabilitiesEx(UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d) {
    EnsureReal();
    return R.ord108 ? R.ord108(a, b, c, d) : ERROR_DEVICE_NOT_CONNECTED;
}
DWORD WINAPI XInputOrdinal109(UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d) {
    EnsureReal();
    return R.ord109 ? R.ord109(a, b, c, d) : ERROR_DEVICE_NOT_CONNECTED;
}

// Named exports: plain pass-throughs to Windows' xinput1_4.dll. Steam Input
// hooks XInput functions by name in the module the game loaded as
// xinput1_4.dll (this proxy), so its hooks land here, in front of Windows'
// library, and the proxy reads the local pad through them: with Steam Input
// on that's Steam's virtual controller, otherwise the real pad. Compiled
// without optimisation so each starts with an ordinary, hookable prologue.
#pragma optimize("", off)
__declspec(noinline) DWORD WINAPI XInputGetState(DWORD dwUserIndex, XINPUT_STATE* pState) {
    EnsureReal();
    DWORD res = R.GetState ? R.GetState(dwUserIndex, pState) : ERROR_DEVICE_NOT_CONNECTED;
    return res;
}
__declspec(noinline) DWORD WINAPI XInputSetState(DWORD dwUserIndex, XINPUT_VIBRATION* pVibration) {
    EnsureReal();
    DWORD res = R.SetState ? R.SetState(dwUserIndex, pVibration) : ERROR_DEVICE_NOT_CONNECTED;
    return res;
}
__declspec(noinline) DWORD WINAPI XInputGetCapabilities(DWORD dwUserIndex, DWORD dwFlags, XINPUT_CAPABILITIES* pCaps) {
    EnsureReal();
    DWORD res = R.GetCaps ? R.GetCaps(dwUserIndex, dwFlags, pCaps) : ERROR_DEVICE_NOT_CONNECTED;
    return res;
}
#pragma optimize("", on)

} // extern "C"
