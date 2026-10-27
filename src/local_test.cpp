// local_test.cpp -- the one-PC co-op test hooks: two copies of the game on one PC (single-instance
// mutex, Steam relaunch check, cursor clip) and player 2's save folder, through the save-folder
// redirect in test_save.cpp.
//
// Entry point: InstallLocalTestHooks, called once from DllMain (xinput_proxy.cpp) when the
// game was started with +tsnet_local host or +tsnet_local client (g_localTest 0 or 1; -1
// otherwise). Each copy then plays one side of an online co-op session over the loopback
// network. Every hook here is an import-table hook (PatchIAT, patch_util.cpp): the game's
// calls to that Windows or Steam function come to us instead.

#include "proxy_shared.h"

// ------------------------------------------------------------------ one-PC co-op test
//
// Lets two copies of the game run on one PC: the "OctaneApp" single-instance
// mutex gets a per-process name, Steam's relaunch-through-Steam check is
// skipped (SteamAppId is set instead), and player 2's copy keeps its save
// under %APPDATA%\TS3 local co-op test\player 2 so the real save is never shared
// (H_SHGetFolderPathA, test_save.cpp).

typedef HANDLE (WINAPI* PFN_CreateMutexA)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
typedef bool (*PFN_RestartApp)(uint32_t);
static PFN_CreateMutexA Real_CreateMutexA;
// The game's CreateMutexA: "OctaneApp" (the game's single-instance check) gets a name of
// this process's own, so the second copy doesn't find the first's and quit. Other names
// pass through.
static HANDLE WINAPI H_CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL own, LPCSTR name) {
    if (name && !strcmp(name, "OctaneApp")) {
        char unique[64];
        snprintf(unique, sizeof unique, "OctaneApp_localtest_%lu", GetCurrentProcessId());
        return Real_CreateMutexA(sa, own, unique);
    }
    return Real_CreateMutexA(sa, own, name);
}
// SteamAPI_RestartAppIfNecessary: always "no restart needed", so a copy started directly
// (not through Steam) keeps running instead of relaunching itself through Steam.
static bool H_RestartApp(uint32_t) { return false; }
// The game clips the cursor to its window (ClipCursor) every frame it has focus,
// which makes dragging the two test windows side by side a fight. In the
// one-PC test the clip is dropped; the game never reads it back.
static BOOL WINAPI H_ClipCursor(const RECT*) { return TRUE; }
// n counts the import entries hooked, for the log line.
void InstallLocalTestHooks() {
    HMODULE exe = GetModuleHandleW(NULL);
    int n = 0;
    // Both copies: the mutex, the Steam relaunch check and the cursor clip.
    if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll"))
        if ((Real_CreateMutexA = (PFN_CreateMutexA)GetProcAddress(k32, "CreateMutexA")))
            n += PatchIAT(exe, "CreateMutexA", (void*)H_CreateMutexA);
    n += PatchIAT(exe, "SteamAPI_RestartAppIfNecessary", (void*)H_RestartApp);
    n += PatchIAT(exe, "ClipCursor", (void*)H_ClipCursor);
    // Player 2 (client) only: its save folder moves under kLocalTestRoot; the host's copy
    // keeps the real save.
    if (g_localTest == 1)
        if (HMODULE sh = GetModuleHandleW(L"shell32.dll"))
            if ((Real_SHGetFolderPathA = (PFN_SHGetFolderPathA)GetProcAddress(sh, "SHGetFolderPathA")))
                n += PatchIAT(exe, "SHGetFolderPathA", (void*)H_SHGetFolderPathA);
    // With no relaunch through Steam, the Steam API takes the game's app id from the
    // environment, as Steam itself would have set it; set here unless it's already there.
    wchar_t id[16];
    if (!GetEnvironmentVariableW(L"SteamAppId", id, 16)) {
        SetEnvironmentVariableW(L"SteamAppId", L"4049180");
        SetEnvironmentVariableW(L"SteamGameId", L"4049180");
    }
    Log("local co-op test: this copy is player %d (%s) on pad %d%s; %d imports hooked; save folder %ls",
        g_localTest + 1, g_localTest ? "client" : "host", g_net.localPad,
        g_net.testInputSeed ? " driven by the input bot" : "", n, g_cfg.saveDir);
}
