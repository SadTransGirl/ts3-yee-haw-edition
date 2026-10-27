// test_save.cpp -- test_save mode: the game plays on a separate save folder (a copy of the real
// save the first time) with its Steam achievement awards dropped, plus the hook for the debug
// menu's "make this test save 100%". Also the AppData save-folder redirect (SHGetFolderPathA),
// shared with the one-PC co-op test in local_test.cpp, which puts player 2's save apart the same way.
//
// Entry points: InstallTestSave, called once from DllMain (xinput_proxy.cpp) when the ini's
// [harness] test_save=1 and this isn't the one-PC test, before the game reads its save;
// H_SHGetFolderPathA, which the game calls through its import table once InstallTestSave or
// InstallLocalTestHooks has put it there; H_SaveApply, called by the game during a level
// load. Addresses written 0x14... are in the game's exe as the decomp session (the reverse
// engineering notes in ..\re) found them; the code uses them as RVAs (0x14027ac30 = RVA
// 0x27ac30, see patch_util.cpp), and checks the bytes there before patching.

#include "proxy_shared.h"

// ------------------------------------------------------------------ save folder redirect

PFN_SHGetFolderPathA Real_SHGetFolderPathA;
// The game finds its save folder under SHGetFolderPathA(CSIDL_APPDATA); these put an
// AppData sub-folder in between, so the save lives apart from the real one: player 2's
// in the one-PC co-op test, the test save in test_save mode.
static const wchar_t* kTestSaveRoot = L"\\TS3 test save";
const wchar_t* kLocalTestRoot = L"\\TS3 local co-op test\\player 2";   // client's AppData sub-folder
static const wchar_t* g_appDataSub = kLocalTestRoot;   // the one in use; InstallTestSave switches it
// The game's SHGetFolderPathA. For CSIDL_APPDATA (low byte; the high bits are flags) the
// sub-folder is appended to the real answer and every level of it is created, so the
// game's own "\Disney\..." path is built inside it. Any other folder passes through.
HRESULT WINAPI H_SHGetFolderPathA(HWND w, int csidl, HANDLE tok, DWORD flags, LPSTR path) {
    HRESULT hr = Real_SHGetFolderPathA(w, csidl, tok, flags, path);
    if (SUCCEEDED(hr) && path && (csidl & 0xff) == 0x1a /* CSIDL_APPDATA */) {
        char sub[128];
        WideCharToMultiByte(CP_ACP, 0, g_appDataSub, -1, sub, sizeof sub, NULL, NULL);
        strcat_s(path, MAX_PATH, sub);
        char dir[MAX_PATH];                                // create each level
        for (char* q = path + 3; *q; q++)                  // (from past the drive's "C:\")
            if (*q == '\\') { memcpy(dir, path, q - path); dir[q - path] = 0; CreateDirectoryA(dir, NULL); }
        CreateDirectoryA(path, NULL);
    }
    return hr;
}

// ------------------------------------------------------------------ test save

// Every Steam achievement goes through one queue (decomp session, re\TEST_SAVE.md): the
// game pushes an id with FUN_14027ac30(queue, id) and FUN_140282f90 hands one a frame to
// ISteamUserStats (GetAchievement, SetAchievement, StoreStats). The exe has no other
// user-stats calls, no SetStat, so no stat that Steam could unlock from. The push
// becomes "xor eax,eax; ret": every award is dropped and nothing builds up to fire later.
// Returns false, with nothing written, if the push's first bytes aren't this build's.
static bool BlockAchievements() {
    // mov [rsp+10h],edx; sub rsp,28h; add rcx,8; lea rdx,[rsp+..]
    static const BYTE push[] = { 0x89, 0x54, 0x24, 0x10, 0x48, 0x83, 0xEC, 0x28, 0x48, 0x83, 0xC1, 0x08, 0x48, 0x8D, 0x54, 0x24 };
    static const BYTE ret0[] = { 0x31, 0xC0, 0xC3 };
    if (!BytesAt(0x27ac30, push, sizeof push) || !WriteMem(0x27ac30, ret0, sizeof ret0, true)) return false;
    Log("test save: achievements off (the game's award queue push at 0x14027ac30 drops every award)");
    return true;
}

// The debug menu's "make this test save 100%" is applied inside a level load's save apply:
// FUN_140492900 calls the apply FUN_1404a4ca0 at 0x140492c08 and delivers LoadedSavedGame
// after it, so the menu's raw writes land where the level graph's lock monitors re-read
// them without firing their cinematics (re\TEST_SAVE_100.md). Test save only.
// g_saveApply: the game's apply (NULL if the redirect didn't fit this build, which also
// tells DllMain not to offer the menu item).
PFN_SaveApply g_saveApply;
// The redirected call: the game applies the save as usual, then the debug menu makes its
// writes if the player asked for them (DebugMenu_SaveApplied returns true, with a summary,
// only then), and the banner tells the player what was done.
static void H_SaveApply(void* self, char a2, void* a3, void* a4) {
    g_saveApply(self, a2, a3, a4);
    char msg[160];
    if (DebugMenu_SaveApplied(msg, sizeof msg))
        Status(20000, kOvAmber, L"Test save made 100%%: %S.\nPlay a few seconds, then leave the level so it saves.", msg);
}

// test_save=1: the game saves under %APPDATA%\TS3 test save\... instead of its real,
// Steam Cloud-synced folder, so testing (a 100% save, cheats) never touches the real
// save or the cloud copy, and achievements are off. The first time, the test folder
// starts as a copy of the real save. Turning test_save off goes back to the real save
// as it was. Without the achievement block it doesn't turn on at all: loading or making
// a 100% save can fire lock-watcher awards (re\TEST_SAVE.md).
// Returns false when test_save couldn't be turned on (the caller then clears g_cfg.testSave).
bool InstallTestSave() {
    // 1. Achievements off, or nothing.
    if (!BlockAchievements()) {
        Log("test save: the achievement code doesn't match this build; test_save NOT on (the real save is in use)");
        Status(15000, kOvRed, L"Test save is NOT on: this game build's achievement code doesn't match.\nYou're on your real save.");
        return false;
    }
    // 2. The game's save-folder lookup goes through H_SHGetFolderPathA, with the test save's
    // sub-folder.
    HMODULE sh = GetModuleHandleW(L"shell32.dll");     // the game imports it; no loading inside DllMain
    Real_SHGetFolderPathA = sh ? (PFN_SHGetFolderPathA)GetProcAddress(sh, "SHGetFolderPathA") : NULL;
    g_appDataSub = kTestSaveRoot;
    if (!Real_SHGetFolderPathA || !PatchIAT(GetModuleHandleW(NULL), "SHGetFolderPathA", (void*)H_SHGetFolderPathA)) {
        Log("test save: couldn't hook the game's save folder lookup; test_save NOT on (the real save is in use)");
        return false;
    }
    // 3. The harness's own idea of the save folder (g_cfg.saveDir, from [harness] save_dir)
    // follows it to where the game will now save. The first time, the folder is made and
    // filled with a copy of the real save.
    wchar_t real[MAX_PATH], app[MAX_PATH];
    wcscpy_s(real, g_cfg.saveDir);
    ExpandEnvironmentStringsW(L"%APPDATA%", app, MAX_PATH);
    swprintf_s(g_cfg.saveDir, L"%s%s\\Disney\\Toy Story 3 Complete Edition\\savedata", app, kTestSaveRoot);
    wchar_t probe[MAX_PATH];
    swprintf_s(probe, L"%s\\Saved Game", g_cfg.saveDir);
    if (GetFileAttributesW(probe) == INVALID_FILE_ATTRIBUTES) {
        for (wchar_t* q = g_cfg.saveDir + 3; *q; q++)    // create each level
            if (*q == L'\\') { *q = 0; CreateDirectoryW(g_cfg.saveDir, NULL); *q = L'\\'; }
        int n = CopyDirFiles(real, g_cfg.saveDir);
        Log("test save: new test save folder %ls, %s", g_cfg.saveDir,
            n > 0 ? "starting from a copy of the real save" : "starting empty (no real save found)");
    } else {
        Log("test save: using the test save in %ls; the real save in %ls is left alone", g_cfg.saveDir, real);
    }
    // 4. FUN_140492900's call to the save apply (0x140492c08) goes through H_SaveApply, for the menu's
    // "make this test save 100%". Optional: test_save stays on without it.
    g_saveApply = (PFN_SaveApply)(ExeBase() + 0x4a4ca0);
    if (!RedirectCall(0x492c08, 0x4a4ca0, (void*)H_SaveApply)) {
        g_saveApply = NULL;
        Log("test save: the save apply call doesn't match this build; no \"make this test save 100%%\"");
    }
    Status(12000, kOvAmber, L"Test save: playing on a separate save folder, with achievements off.\nYour real save isn't touched.");
    return true;
}
