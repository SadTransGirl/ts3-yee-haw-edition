// levels_scripts.cpp -- content restores: the hidden and Wii-only levels (file list and
// WorldDatabase patches), shooting-gallery buttons, extra sound banks, the 2010 script fixes and
// the vocal theme song.
//
// Entry points. DllMain (xinput_proxy.cpp) calls the three installers once, at process
// attach and only when xinput_harness.ini exists, so they're in place before the game reads
// its file lists and scripts: InstallHiddenLevels ([harness] restore_hidden_levels, which also
// installs the gallery buttons and the sound-bank fixes), InstallScriptFixes ([harness]
// script_fixes_2010) and InstallVocalTheme ([audio] vocal_theme). After that everything here
// runs inside the game's own calls, through hooks: VocalTick is the one per-frame entry,
// called from the pad poll (NoteCall in harness_core.cpp).
//
// Reading the addresses. A hex address like 0x1404979de is an address in the game's exe
// (image base 0x140000000) found by reverse engineering it (the decomp session); FUN_... and
// DAT_... are the decompiler's names for functions and data it has no real name for. The
// patch helpers take an RVA instead: the offset from the exe's base (0x4979de for the
// address above). RedirectCall(callRva, expectTargetRva, hook) rewrites one "call"
// instruction in the game to call our hook instead, but only if it still calls the expected
// function; BytesAt and the prologue arrays compare the game's bytes with what this build
// is known to hold. Both verify the game build before patching: on any other build the
// feature is skipped and logged, never patched blindly.

#include "proxy_shared.h"

bool g_vocalOn;            // [audio] vocal_theme (below, with the script fixes)

// ------------------------------------------------------------------ hidden and Wii-only levels
//
// [harness] restore_hidden_levels=1 (default). The CE ships the full HD data of
// three Wii-only story levels (ST_AndysBasement, TrainStub, ST_BuzzFlight) and of
// Trendy Town, the Skate Park and the Shooting Gallery, but their preload lists
// and database entries are missing or flagged, so they load "buggy" (no sounds,
// empty dialogue boxes, story-mode avatar in Trendy). Two text files are edited in
// memory as the game reads them at boot; no game file changes. Spec and data:
// re\wii_levels\ (README.md, filelist_extra.txt, worlddb_patch.txt), embedded in
// wii_levels.h by re\tools\bin2h.py (kFileListExtra, kWorldDbPatch).
//   FileList.txt: read once by FUN_1404979a0 (GameCommon::vf2) through the call at
//     0x1404979de; the extra sections are appended.
//   WorldDatabase.txt: read by the generic ini loader FUN_140474f10 through the call
//     at 0x140474fd3 (other ini files pass there too: matched by name); each listed
//     section gets its flags line replaced and "+" keys inserted after its header.
// Both callers free the buffer with the game's allocator, so the edited text goes
// into a Mem_Alloc block and the original is Mem_Free'd. Simulation-relevant (what a
// level preloads, story mode): part of the co-op settings fingerprint (the hash of the
// settings sheet, the list of settings both PCs of an online session must share; see
// NetOpen in net_transport.cpp: two PCs with different values refuse to connect).
#include "wii_levels.h"
#include "sx_fixes_2010.h"

// The game's whole-file readers: return a buffer holding the file (the caller frees it) and
// its size through *size. a3..a5 are passed through untouched. The scripts' reader below has
// the same shape.
typedef char* (*PFN_ReadTextFile)(const char* path, uint32_t* size, void* a3, uint8_t a4, void* a5);
typedef void* (*PFN_MemAlloc)(size_t size);
typedef void  (*PFN_MemFree)(void* p);
static PFN_ReadTextFile g_readFileList, g_readIniFile;   // the original readers our hooks call first
bool g_levelsWanted, g_levelsFileListOk, g_levelsWorldDbOk;   // wanted; each hook installed

// True if the last component of path (after any '\' or '/') is name, ignoring case.
static bool PathNameIs(const char* path, const char* name) {
    if (!path) return false;
    const char* base = path;
    for (const char* p = path; *p; p++) if (*p == '\\' || *p == '/') base = p + 1;
    return _stricmp(base, name) == 0;
}

// The edited text into a game-allocated buffer, replacing buf. Returns the new buffer
// (or buf unchanged if the game's allocator failed).
static char* LevelsSwapBuffer(char* buf, uint32_t* size, const std::string& text) {
    PFN_MemAlloc memAlloc = (PFN_MemAlloc)(ExeBase() + 0x241aa0);   // the game's Mem_Alloc
    PFN_MemFree memFree = (PFN_MemFree)(ExeBase() + 0x2422c0);      // and Mem_Free
    char* out = (char*)memAlloc(text.size() + 1);
    if (!out) return buf;
    memcpy(out, text.data(), text.size());
    out[text.size()] = 0;
    memFree(buf);
    *size = (uint32_t)text.size();
    return out;
}

// The file the game read, as a string, without any trailing NUL bytes.
static std::string LevelsOriginal(const char* buf, uint32_t size) {
    std::string s(buf, size);
    while (!s.empty() && s.back() == 0) s.pop_back();
    return s;
}

// Hook on the call that reads FileList.txt (the per-level preload lists): reads it as
// usual, then appends kFileListExtra's sections (the restored levels' lists) on a new line.
// Any other file passes through unchanged.
static char* H_ReadFileList(const char* path, uint32_t* size, void* a3, uint8_t a4, void* a5) {
    char* buf = g_readFileList(path, size, a3, a4, a5);
    if (!buf || !size || !PathNameIs(path, "FileList.txt")) return buf;
    std::string text = LevelsOriginal(buf, *size);
    size_t before = text.size();
    text += "\r\n";
    text.append((const char*)kFileListExtra, sizeof kFileListExtra);
    // Count the "[Section]" headers added, for the log line.
    int sections = 0;
    for (size_t i = 0; i < sizeof kFileListExtra; i++)
        if (kFileListExtra[i] == '[' && (i == 0 || kFileListExtra[i - 1] == '\n')) sections++;
    char* out = LevelsSwapBuffer(buf, size, text);
    if (out == buf) Log("levels: FileList.txt: the game's allocator failed; left as it was");
    else Log("levels: FileList.txt +%d sections (+%zu bytes, %zu -> %zu)", sections, text.size() - before, before, text.size());
    return out;
}

// Applies kWorldDbPatch to WorldDatabase.txt's text: for each [Section] it names
// (case-insensitive), the section's "flags =" line is replaced and the "+" lines are
// inserted right after the header. A section ends at the next line starting with '['.
// One section's edit: its name, the replacement "flags =" line (empty: leave flags alone),
// the lines to insert after its header, and whether the section and its flags line were met.
struct LevelsDbEdit { std::string name, flags; std::vector<std::string> add; bool found = false, flagsDone = false; };

// s without leading and trailing spaces, tabs and line endings.
static std::string LevelsTrim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// A "flags = ..." line (any case, any spacing before the '=').
static bool LevelsIsFlagsLine(const std::string& line) {
    std::string t = LevelsTrim(line);
    if (t.size() < 5 || _strnicmp(t.c_str(), "flags", 5) != 0) return false;
    size_t i = 5;
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) i++;
    return i < t.size() && t[i] == '=';
}

// Parses the embedded kWorldDbPatch into one LevelsDbEdit per "[Section]": "+key" lines go
// to add (without the '+'), a flags line becomes the replacement. Blank lines and '#'
// comments are skipped, as is anything before the first section.
static std::vector<LevelsDbEdit> LevelsParseDbPatch() {
    std::vector<LevelsDbEdit> edits;
    std::string patch((const char*)kWorldDbPatch, sizeof kWorldDbPatch);
    size_t pos = 0;
    while (pos < patch.size()) {
        size_t nl = patch.find('\n', pos);
        std::string line = LevelsTrim(patch.substr(pos, (nl == std::string::npos ? patch.size() : nl) - pos));
        pos = nl == std::string::npos ? patch.size() : nl + 1;
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[' && line.back() == ']') { edits.emplace_back(); edits.back().name = line.substr(1, line.size() - 2); continue; }
        if (edits.empty()) continue;
        if (line[0] == '+') edits.back().add.push_back(LevelsTrim(line.substr(1)));
        else if (LevelsIsFlagsLine(line)) edits.back().flags = line;
    }
    return edits;
}

// Returns text with the edits applied, line by line, keeping every untouched line byte for
// byte. Inserted lines use the file's own line ending. Marks each edit's found / flagsDone
// so the caller can log what wasn't there. Only the first section with a given name is edited.
static std::string LevelsPatchWorldDb(const std::string& text, std::vector<LevelsDbEdit>& edits) {
    std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::string out;
    out.reserve(text.size() + 1024);
    LevelsDbEdit* cur = NULL;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        size_t end = nl == std::string::npos ? text.size() : nl + 1;
        std::string line = text.substr(pos, end - pos);          // with its own line ending
        pos = end;
        std::string body = line;
        while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
        std::string ending = line.substr(body.size());
        std::string t = LevelsTrim(body);
        // A section header: is it one we edit? If so, copy it and insert its "+" lines.
        if (!t.empty() && t[0] == '[') {
            cur = NULL;
            size_t close = t.find(']');
            std::string name = close == std::string::npos ? std::string() : t.substr(1, close - 1);
            for (auto& e : edits) if (_stricmp(e.name.c_str(), name.c_str()) == 0 && !e.found) { cur = &e; break; }
            out += line;
            if (cur) {
                cur->found = true;
                if (ending.empty()) out += eol;
                for (auto& a : cur->add) out += a + eol;
            }
            continue;
        }
        // Inside an edited section: its flags line is swapped for ours (keeping the line ending).
        if (cur && !cur->flags.empty() && LevelsIsFlagsLine(body)) {
            out += cur->flags + (ending.empty() ? std::string() : ending);
            cur->flagsDone = true;
            continue;
        }
        out += line;
    }
    return out;
}

// Hook on the generic ini loader's file read: WorldDatabase.txt (the table of worlds and
// their settings) gets kWorldDbPatch applied; every other ini passes through unchanged.
static char* H_ReadIniFile(const char* path, uint32_t* size, void* a3, uint8_t a4, void* a5) {
    char* buf = g_readIniFile(path, size, a3, a4, a5);
    if (!buf || !size || !PathNameIs(path, "WorldDatabase.txt")) return buf;
    std::vector<LevelsDbEdit> edits = LevelsParseDbPatch();
    std::string text = LevelsPatchWorldDb(LevelsOriginal(buf, *size), edits);
    // Tally the result for the log: sections edited, flags lines replaced, keys added, names not found.
    int found = 0, flags = 0, keys = 0;
    std::string missing;
    for (auto& e : edits) {
        if (e.found) { found++; keys += (int)e.add.size(); } else missing += (missing.empty() ? "" : ", ") + e.name;
        if (e.flagsDone) flags++;
    }
    char* out = LevelsSwapBuffer(buf, size, text);
    if (out == buf) Log("levels: WorldDatabase.txt: the game's allocator failed; left as it was");
    else Log("levels: WorldDatabase.txt: %d of %zu sections edited (%d flags lines replaced, %d keys added)%s%s",
             found, edits.size(), flags, keys, missing.empty() ? "" : "; not found: ", missing.c_str());
    return out;
}

// The shooting gallery (the PS Move mode) reads control-map slots 0, 1 and 2 as Start,
// Calibrate and Exit on its title menu, and the last one pressed wins. On PC, slots 0
// and 2 are both X (and both 0x13 on the keyboard), so X starts and exits at once: the
// player just leaves (re\wii_levels\SHOOTING_GALLERY.md). BuzzControlMapper::vf7
// (0x1402150b0, vtable 0x1407aff60 entry 7) sets the default bindings, and the gallery
// calls it again whenever its menu opens, so the gallery's three slots are rebound
// right after it. Pad: A starts (and fires), Y calibrates, B exits. Keyboard: slot 2's
// copy of slot 0's key is cleared; exit through the pause menu. Input ids are
// XInputInputDriver_Poll's: X 5, A 6, B 7, Y 8.
typedef void (*PFN_MapperDefaults)(void* mapper, int mode);
static PFN_MapperDefaults g_mapperDefaults;   // the original vf7
bool g_galleryButtonsOk;

// Is the loaded world the shooting gallery? Asks the game's WorldDb_Get (0x1405bb6a0) for
// the current world's name; false if there's none yet or the read faults.
static bool InShootingGallery() {
    __try {
        const char* world = ((const char* (*)())(ExeBase() + 0x5bb6a0))();   // WorldDb_Get: g_World, name inline at +0
        return world && _stricmp(world, "shootinggallery") == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Rewrites the gallery's three menu slots in the mapper's binding tables (see above). The
// mapper holds its input driver at +8 and pointers to two int tables of input ids, indexed
// by slot: the primary bindings at +0xb0, the secondary ones at +0xc0. Logs once per input
// kind. (__try: a bad pointer skips the rebind instead of crashing the game.)
static void GalleryRebind(BYTE* mapper) {
    __try {
        BYTE* driver = *(BYTE**)(mapper + 8);
        int* primary = *(int**)(mapper + 0xb0);
        int* secondary = *(int**)(mapper + 0xc0);
        if (!driver || !primary || !secondary) return;
        bool pad = *(int*)(driver + 0x28c) != 0;          // FUN_1402838c0 false: the pad table
        static int logged;
        if (pad) {
            primary[0] = 6;                                // A: start / fire
            primary[1] = 8;                                // Y: calibrate
            primary[2] = 7;                                // B: exit
            secondary[2] = 0;                              // no longer the same as slot 0's 0x13
        } else if (primary[2] == primary[0]) {
            primary[2] = 0;                                // keyboard: Start no longer exits too
        }
        if (!(logged & (pad ? 1 : 2))) {
            logged |= pad ? 1 : 2;
            Log(pad ? "shooting gallery: menu buttons rebound (pad: A start, Y calibrate, B exit)"
                    : "shooting gallery: menu buttons rebound (keyboard: Start no longer also exits; exit from the pause menu)");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Our vf7: the game's default bindings, then the gallery's fix if the gallery is loaded.
static void H_MapperDefaults(void* mapper, int mode) {
    g_mapperDefaults(mapper, mode);
    if (mapper && InShootingGallery()) GalleryRebind((BYTE*)mapper);
}

// Swaps entry 7 of BuzzControlMapper's vtable for H_MapperDefaults, so calls made through
// the vtable (the gallery's included) reach us. Checks first that the slot still holds vf7
// and that WorldDb_Get is the expected "mov rax, [rip+...]" followed by ret at +7.
// Returns false, patching nothing, on any other build.
static bool InstallGalleryButtons() {
    static const BYTE worldDbGet[] = { 0x48, 0x8B, 0x05 };   // mov rax, [rip+g_World]; ret
    BYTE* slot = ExeBase() + 0x7aff60 + 7 * 8;               // 8 bytes per vtable entry
    __try {
        if (*(BYTE**)slot != ExeBase() + 0x2150b0 || !BytesAt(0x5bb6a0, worldDbGet, sizeof worldDbGet) ||
            *(ExeBase() + 0x5bb6a0 + 7) != 0xC3)
            return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    g_mapperDefaults = (PFN_MapperDefaults)(ExeBase() + 0x2150b0);
    void* hook = (void*)H_MapperDefaults;
    return WriteMem(0x7aff60 + 7 * 8, &hook, sizeof hook, false);
}

// Sound banks: the CE maps each old .fev name to a Studio bank through
// data_dx\sound\bank_name_replacement.txt, read line by line by FUN_14026e290 with
// FUN_14025b950(handle, buf, cap): 1 per line (with its '\n'), 0 at the end. Two
// names a level asks for aren't in it, so their banks never load though they ship
// complete (re\wii_levels\SHOOTING_GALLERY.md, "Missing sound effects"):
// shooting_gallery.fev (every gallery sound effect) and jessie.fev (preloaded by
// four story levels; the list only has "jesse.fev"). Once the file runs out, the
// reader hands back the gallery's extra line, in the file's own "name,bank" form.
// The reader returns a char: at the end it returns via xor al,al (0x14025ba02) with
// the rest of RAX still holding an offset, so only the low byte is the answer (build
// 51-70 read all of RAX, never saw the end, and the line was never added).
// After loading a bank, FUN_14026db40 opens SOUND\<the name it was called with, minus
// ".fev">.TXT and registers every "Name : /path" line in it; an effect missing from
// that table plays nothing. So the gallery keeps its own name (the line above maps it
// to the bank and shooting_gallery.txt then loads), and Jessie's levels' "jessie.fev"
// becomes "jesse.fev" (H_LoadBank), the name the stock list and jesse.txt use.
typedef int (*PFN_ReadLine)(void* handle, char* buf, int cap);
static PFN_ReadLine g_readLine;
bool g_bankListOk;
static const char* const kExtraBanks[] = {
    "shooting_gallery.fev,shooting_gallery_shooting_gallery_se.bank",
};
static int g_extraBankNext;                            // next kExtraBanks line to hand out in this pass

// Hook on the reader's line calls: the file's own lines first; once it reports the end, one
// kExtraBanks line per call (returning 1, "a line"), then the real end.
static int H_BankListNext(void* handle, char* buf, int cap) {
    BYTE r = (BYTE)g_readLine(handle, buf, cap);
    if (r || !buf || g_extraBankNext >= (int)_countof(kExtraBanks)) return r;
    _snprintf_s(buf, cap, _TRUNCATE, "%s\r\n", kExtraBanks[g_extraBankNext++]);
    if (g_extraBankNext == _countof(kExtraBanks)) {
        static bool logged;
        if (!logged) { logged = true; Log("sounds: bank list +%d entry (shooting_gallery)", (int)_countof(kExtraBanks)); }
    }
    return 1;
}

// Hook on the first line read of a pass over the file (a separate call site): restarts the
// extra lines, then reads as H_BankListNext does.
static int H_BankListFirst(void* handle, char* buf, int cap) {
    g_extraBankNext = 0;                                // a new pass over the file
    static bool logged;
    if (!logged) { logged = true; Log("sounds: the game is reading its bank list (bank_name_replacement.txt)"); }
    return H_BankListNext(handle, buf, cap);
}

// Where a level preloads its banks (FUN_140498bb0's call to the bank loader
// FUN_14026db40(manager, name, flag, useList=1, 1) at 0x140498e26), "jessie.fev" is
// renamed to "jesse.fev": the stock list maps that to the bank and jesse.txt names its
// effects. (Builds 52-70 passed the bank file names here instead; the loader then
// looked for a .TXT named after the bank, found none, and registered no effects.)
typedef uint64_t (*PFN_LoadBank)(void* mgr, const char* name, char a3, char a4, char a5);
static PFN_LoadBank g_loadBank;
bool g_bankRenameOk;

// Replaces the call at 0x140498e26: a name in kRename's left column loads under the right one;
// anything else is passed on unchanged.
static uint64_t H_LoadBank(void* mgr, const char* name, char a3, char a4, char a5) {
    static const char* const kRename[][2] = {
        { "jessie.fev", "jesse.fev" },
    };
    static bool logged[_countof(kRename)];
    if (name)
        for (int i = 0; i < (int)_countof(kRename); i++)
            if (PathNameIs(name, kRename[i][0])) {
                if (!logged[i]) { logged[i] = true; Log("sounds: %s loads as %s", name, kRename[i][1]); }
                return g_loadBank(mgr, kRename[i][1], a3, a4, a5);
            }
    return g_loadBank(mgr, name, a3, a4, a5);
}

// [harness] restore_hidden_levels=1, from DllMain before GameCommon reads FileList.txt:
// installs everything above (sound-bank list and renames, the two text-file hooks, the
// gallery buttons). Each part is checked and logged on its own; a failed check leaves that
// part of the game untouched.
void InstallHiddenLevels() {
    // Sound banks: FUN_14026e290's two calls to the line reader (the one that starts a pass, and the other).
    g_readLine = (PFN_ReadLine)(ExeBase() + 0x25b950);
    g_bankListOk = RedirectCall(0x26e310, 0x25b950, (void*)H_BankListFirst) &&
                   RedirectCall(0x26ea42, 0x25b950, (void*)H_BankListNext);
    if (!g_bankListOk) Log("sounds: bank list NOT extended: code doesn't match this build");
    g_loadBank = (PFN_LoadBank)(ExeBase() + 0x26db40);
    g_bankRenameOk = RedirectCall(0x498e26, 0x26db40, (void*)H_LoadBank);
    if (!g_bankRenameOk) Log("sounds: level bank names NOT translated: code doesn't match this build");
    // The level lists: both calls go to readers whose originals are kept for the hooks.
    g_readFileList = (PFN_ReadTextFile)(ExeBase() + 0x250b60);
    g_readIniFile = (PFN_ReadTextFile)(ExeBase() + 0x25bc10);
    g_levelsFileListOk = RedirectCall(0x4979de, 0x250b60, (void*)H_ReadFileList);
    g_levelsWorldDbOk = RedirectCall(0x474fd3, 0x25bc10, (void*)H_ReadIniFile);
    g_galleryButtonsOk = InstallGalleryButtons();
    Log("levels: hidden and Wii-only levels %s (FileList.txt hook %s, WorldDatabase.txt hook %s, shooting gallery buttons %s)",
        g_levelsFileListOk && g_levelsWorldDbOk ? "restored as the game reads its lists" : "NOT fully restored",
        g_levelsFileListOk ? "in" : "FAILED: code doesn't match", g_levelsWorldDbOk ? "in" : "FAILED: code doesn't match",
        g_galleryButtonsOk ? "in" : "FAILED: code doesn't match");
}

// [harness] script_fixes_2010=1 (default off): the fixes the 2010 PC release made to four
// behaviour scripts (ground pound in co-op on the Buzz board game, aiming after landing in
// target mode, the reticle and ball throw, TNT in water, Zurgbots falling out of the
// world), applied to the CE's own scripts as the game reads them; the builder and its data
// are in sx_fixes_2010.h (re\SCRIPT_FIXES_2010.md). Both script read paths call the file
// reader FUN_140250b60: scripts loaded by name (FUN_140542090, call at 0x1405420ea) and
// the modifier preload (ModifierLoaderTask::Execute, call at 0x1401e264e). Only the four
// retail files (size and SHA-256) are swapped for a patched copy in a Mem_Alloc block; the
// original is freed with FUN_140245170, as the callers free script buffers. The player
// fix calls GetIgnoreEntity, which the CE exe lacks: it's registered with the script VM
// before the first patched player.sx is returned. Changes the simulation: a settings-sheet
// line, so both co-op PCs must match.
bool g_scriptFixesWanted, g_scriptFixesOk;
static PFN_ReadTextFile g_readScript;
static volatile LONG g_scriptPatched[4];                 // per kSxPatches entry: logged once

// SHA-256 of data as 64 lowercase hex digits (plus NUL) in hex; false if BCrypt fails.
static bool Sha256Hex(const void* data, size_t n, char hex[65]) {
    UCHAR d[32];
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, NULL, 0, (PUCHAR)data, (ULONG)n, d, sizeof d) != 0) return false;
    for (int i = 0; i < 32; i++) sprintf_s(hex + i * 2, 3, "%02x", d[i]);
    return true;
}

// Pops an entity and pushes the key of the actor it ignores for collisions, or 0: the field
// SetIgnoreEntity (0x1405675e0) writes, at *(actor + 0x368) + 0x38.
typedef BYTE* (*PFN_PopEntity)(void* ctx);
typedef void (*PFN_PushValue)(void* ctx, uint64_t value, uint32_t type);
static void Native_GetIgnoreEntity(void* ctx) {
    uint64_t key = 0;
    __try {
        if (BYTE* actor = ((PFN_PopEntity)(ExeBase() + 0x544740))(ctx))
            if (BYTE* coll = *(BYTE**)(actor + 0x368)) key = *(uint64_t*)(coll + 0x38);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    ((PFN_PushValue)(ExeBase() + 0x544cd0))(ctx, key, 0);         // type 0: an int, as entities are
}
// Adds GetIgnoreEntity to the script VM's natives, once. False while the VM doesn't exist
// yet (the caller then leaves the script unpatched rather than give it a call that fails).
static bool RegisterGetIgnoreEntity() {
    static bool done;
    if (done) return true;
    void* vm = *(void**)(ExeBase() + 0xb2b430);                   // g_ScriptVM
    if (!vm) return false;
    typedef void (*PFN_RegisterNative)(void* ctx, const char* name, void* fn);
    ((PFN_RegisterNative)(ExeBase() + 0x544f60))(vm, "GetIgnoreEntity", (void*)Native_GetIgnoreEntity);
    done = true;
    Log("script fixes: GetIgnoreEntity registered with the script VM");
    return true;
}

// Hook on both script-reading calls: reads the file as usual; if it's one of kSxPatches'
// retail scripts, returns a patched copy instead (new buffer, new *size). Anything else,
// or anything that doesn't check out, is returned exactly as read.
static char* H_ReadScript(const char* path, uint32_t* size, void* a3, uint8_t a4, void* a5) {
    char* buf = g_readScript(path, size, a3, a4, a5);
    // Compiled scripts only: "SLNG", 0x12345678 as the third int, and room for the 26-int header.
    if (!buf || !size || *size < 26 * 4 || memcmp(buf, "SLNG", 4) != 0 || ((int32_t*)buf)[2] != 0x12345678) return buf;
    for (size_t k = 0; k < _countof(kSxPatches); k++) {
        const SxPatchSpec& s = kSxPatches[k];
        if (*size != s.size || !PathNameIs(path, s.name)) continue;
        // The right name and size: patch it only if it's byte for byte the retail file.
        char hex[65];
        if (!Sha256Hex(buf, *size, hex) || strcmp(hex, s.sha256) != 0) {
            if (!InterlockedExchange(&g_scriptPatched[k], 1))
                Log("script fixes: %s isn't the retail version (sha256 %.16s...); left as it is", s.name, hex);
            return buf;
        }
        if (s.nNatives && !RegisterGetIgnoreEntity()) {
            Log("script fixes: no script VM yet when %s loaded; left as it is (its fix calls a native)", s.name);
            return buf;
        }
        // Build the patched copy, hand it over in a game-allocated block, free the original.
        std::vector<uint8_t> out = SxBuildPatched((const uint8_t*)buf, *size, s);
        char* nb = (char*)((PFN_MemAlloc)(ExeBase() + 0x241aa0))(out.size());
        if (!nb) return buf;
        memcpy(nb, out.data(), out.size());
        ((PFN_MemFree)(ExeBase() + 0x245170))(buf);
        if (!InterlockedExchange(&g_scriptPatched[k], 1))
            Log("script fixes: %s patched with the 2010 fixes (%u -> %zu bytes, %zu cave words)", s.name, *size, out.size(), s.nCave);
        *size = (uint32_t)out.size();
        return nb;
    }
    return buf;
}

// [audio] vocal_theme=1: Randy Newman's vocal "You've Got a Friend in Me" on the Story Mode
// game board and in Al's Toy Barn, as the 2010 PC game had it, instead of the Complete
// Edition's instrumental (a licensing cut). Played from the user's OWN 2010 install
// ([audio] legacy_install, default C:\Program Files (x86)\Disney Interactive Studios\Toy
// Story 3): nothing is copied, and without it (or if the file doesn't check out) the
// instrumental plays. Spec: re\audio_check\VOCAL_THEME.md.
//   The 2010 bank data_dxw\sound\imusic_str.fsb holds both as plain 16-bit stereo PCM, which
//   the CE's fmod.dll streams straight out of the file (FMOD_OPENRAW at an offset). Its size
//   and each slice's SHA-1 are checked once, on a thread at start-up.
//   FUN_1402657c0(cue, ...) starts every music cue (event:/imusic_new/<cue+0>, the instance
//   at cue+0x158). For "gameboard" and "toybarn" the Studio event keeps running, so the music
//   system's state, fades, pause and volume are all stock; each frame its own tracks (the
//   child groups of its master channel group) are muted and the 2010 stream plays as a
//   channel in that master group, until the event stops. Render-only: each co-op PC may
//   differ.
struct VocalSample { const char* cue; uint32_t offset, length, rate; const char* sha1; volatile LONG ok; };   // ok: 1 good, -1 bad
static VocalSample g_vocalSamples[] = {
    { "gameboard", 0xde73560, 21792800, 44100, "dfca5ce463f1", 0 },   // 2010 cue 42: "01 - You've Got a Friend In Me"
    { "toybarn", 0x104a9d60, 23720064, 48000, "fc66a89f707a", 0 },    // 2010 cue 43: Toybarn.wav, the same recording
};
static char g_vocalFsb[MAX_PATH];                     // the 2010 bank, ANSI (FMOD takes char paths)
static const uint64_t kVocalFsbSize = 1223825728ull;

// Started by InstallVocalTheme: checks the 2010 bank's size, then hashes each sample's slice
// (over 20 MB each, so not on the game thread) and sets its ok to 1 or -1. A cue that starts
// before its check is done plays the instrumental.
static DWORD WINAPI VocalVerifyThread(void*) {
    HANDLE f = CreateFileA(g_vocalFsb, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    LARGE_INTEGER sz = {};
    if (f == INVALID_HANDLE_VALUE || !GetFileSizeEx(f, &sz) || (uint64_t)sz.QuadPart != kVocalFsbSize) {
        Log("vocal theme: %s %s; the instrumental plays", g_vocalFsb, f == INVALID_HANDLE_VALUE ? "not found (is the 2010 game "
            "installed there? [audio] legacy_install)" : "isn't the expected 2010 file (size differs)");
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        for (VocalSample& s : g_vocalSamples) s.ok = -1;
        return 0;
    }
    // SHA-1 of each slice, read 1 MB at a time; only the first 12 hex digits are compared.
    std::vector<BYTE> buf(1 << 20);
    for (VocalSample& s : g_vocalSamples) {
        BCRYPT_HASH_HANDLE h = NULL;
        bool ok = BCryptCreateHash(BCRYPT_SHA1_ALG_HANDLE, &h, NULL, 0, NULL, 0, 0) == 0;
        LARGE_INTEGER at;
        at.QuadPart = s.offset;
        ok = ok && SetFilePointerEx(f, at, NULL, FILE_BEGIN);
        for (uint32_t left = s.length; ok && left;) {
            DWORD want = left < buf.size() ? left : (DWORD)buf.size(), got = 0;
            ok = ReadFile(f, buf.data(), want, &got, NULL) && got == want && BCryptHashData(h, buf.data(), got, 0) == 0;
            left -= got;
        }
        UCHAR d[20];
        char hex[41] = {};
        if (ok && BCryptFinishHash(h, d, sizeof d, 0) == 0)
            for (int i = 0; i < 20; i++) sprintf_s(hex + i * 2, 3, "%02x", d[i]);
        if (h) BCryptDestroyHash(h);
        s.ok = ok && strncmp(hex, s.sha1, strlen(s.sha1)) == 0 ? 1 : -1;
        Log("vocal theme: %s: the 2010 recording %s", s.cue, s.ok > 0 ? "checks out; it plays instead of the instrumental"
                                                                    : "doesn't match; the instrumental plays");
    }
    CloseHandle(f);
    return 0;
}

// The FMOD C API, from the game's own fmod.dll / fmodstudio.dll (2.03), looked up on first use.
struct VocalFmod {
    bool tried, ok;
    int (__stdcall* instIsValid)(void*);
    int (__stdcall* instGetGroup)(void*, void**);
    int (__stdcall* instGetState)(void*, int*);
    int (__stdcall* groupGetSystem)(void*, void**);
    int (__stdcall* groupNumGroups)(void*, int*);
    int (__stdcall* groupGetGroup)(void*, int, void**);
    int (__stdcall* groupSetMute)(void*, int);
    int (__stdcall* createSound)(void*, const char*, unsigned, void*, void**);
    int (__stdcall* playSound)(void*, void*, void*, int, void**);
    int (__stdcall* chanStop)(void*);
    int (__stdcall* chanSetPos)(void*, unsigned, unsigned);
    int (__stdcall* soundRelease)(void*);
};
static VocalFmod g_fm;
// Looks the functions up the first time (VocalTick, game thread) and remembers the answer:
// true if both DLLs are loaded and export everything.
static bool VocalFmodReady() {
    if (g_fm.tried) return g_fm.ok;
    g_fm.tried = true;
    HMODULE core = GetModuleHandleW(L"fmod.dll"), studio = GetModuleHandleW(L"fmodstudio.dll");
    if (!core || !studio) { Log("vocal theme: fmod.dll / fmodstudio.dll not loaded; the instrumental plays"); return false; }
    auto get = [](HMODULE m, const char* n, auto& fn) { fn = (std::remove_reference_t<decltype(fn)>)GetProcAddress(m, n); return fn != NULL; };
    g_fm.ok = get(studio, "FMOD_Studio_EventInstance_IsValid", g_fm.instIsValid) &&
              get(studio, "FMOD_Studio_EventInstance_GetChannelGroup", g_fm.instGetGroup) &&
              get(studio, "FMOD_Studio_EventInstance_GetPlaybackState", g_fm.instGetState) &&
              get(core, "FMOD_ChannelGroup_GetSystemObject", g_fm.groupGetSystem) &&
              get(core, "FMOD_ChannelGroup_GetNumGroups", g_fm.groupNumGroups) &&
              get(core, "FMOD_ChannelGroup_GetGroup", g_fm.groupGetGroup) &&
              get(core, "FMOD_ChannelGroup_SetMute", g_fm.groupSetMute) &&
              get(core, "FMOD_System_CreateSound", g_fm.createSound) && get(core, "FMOD_System_PlaySound", g_fm.playSound) &&
              get(core, "FMOD_Channel_Stop", g_fm.chanStop) && get(core, "FMOD_Channel_SetPosition", g_fm.chanSetPos) &&
              get(core, "FMOD_Sound_Release", g_fm.soundRelease);
    if (!g_fm.ok) Log("vocal theme: an FMOD function is missing from the game's FMOD; the instrumental plays");
    return g_fm.ok;
}

// One replaced cue: its Studio instance and, once its channel group exists, our stream.
struct VocalPlay { void* inst; void* sound; void* chan; int sample, resumeMs, frames; };
static VocalPlay g_vocalPlays[4];
static CRITICAL_SECTION g_vocalLock;                  // guards g_vocalPlays (cue starts and VocalTick)

// Detour of FUN_1402657c0, the music cue start: runs the original, then, if it was a fresh
// start of "gameboard" or "toybarn" whose 2010 slice checked out, records the cue's Studio
// instance in a free g_vocalPlays slot. VocalTick does the FMOD work from there.
typedef bool (*PFN_CueStart)(BYTE* cue, void* a2, char keepPrevious, char a4);
static PFN_CueStart g_cueStart;
static bool H_CueStart(BYTE* cue, void* a2, char keepPrevious, char a4) {
    bool fresh = false;
    __try { fresh = cue && !cue[0x104]; } __except (EXCEPTION_EXECUTE_HANDLER) {}   // +0x104: already started
    bool r = g_cueStart(cue, a2, keepPrevious, a4);
    if (!fresh) return r;                                     // (the game names the cues in capitals: "GAMEBOARD")
    __try {
        for (int k = 0; k < (int)_countof(g_vocalSamples); k++) {
            if (_stricmp((const char*)cue, g_vocalSamples[k].cue) != 0) continue;
            if (g_vocalSamples[k].ok <= 0) { Log("vocal theme: %s started before its check finished (or it failed)", g_vocalSamples[k].cue); break; }
            void* inst = *(void**)(cue + 0x158);
            if (!inst) { Log("vocal theme: %s started without an instance", g_vocalSamples[k].cue); break; }
            EnterCriticalSection(&g_vocalLock);
            for (VocalPlay& p : g_vocalPlays)
                if (!p.inst) { p = { inst, NULL, NULL, k, *(int*)(cue + 0x148), 0 }; break; }   // +0x148: resume position (ms)
            LeaveCriticalSection(&g_vocalLock);
            break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return r;
}

// Stops and releases our stream (if it got that far) and frees the slot.
static void VocalStop(VocalPlay& p, const char* why) {
    if (p.chan) g_fm.chanStop(p.chan);
    if (p.sound) g_fm.soundRelease(p.sound);
    if (p.sound) Log("vocal theme: %s stopped (%s)", g_vocalSamples[p.sample].cue, why);
    p = {};
}
// Once a frame (the pad poll), on the game thread.
void VocalTick() {
    EnterCriticalSection(&g_vocalLock);
    for (VocalPlay& p : g_vocalPlays) {
        if (!p.inst) continue;
        if (!VocalFmodReady()) { p = {}; continue; }
        // Has the Studio event stopped or gone? Then so does our stream.
        int state = 2, valid = g_fm.instIsValid(p.inst), rs = valid ? g_fm.instGetState(p.inst, &state) : -1;
        // Until Studio's next update processes start(), the state can still read STOPPED: give it a second.
        if (!valid || rs != 0 || (state == 2 /* STOPPED */ && (p.sound || ++p.frames > 60))) {
            if (!p.sound) Log("vocal theme: %s: the event %s before the vocal started (valid %d, result %d, state %d)",
                              g_vocalSamples[p.sample].cue, valid ? "stopped" : "went away", valid, rs, state);
            VocalStop(p, "the event stopped");
            continue;
        }
        if (state == 2) continue;
        void* group = NULL;
        if (g_fm.instGetGroup(p.inst, &group) != 0 || !group) {      // exists once Studio has processed the start
            if (++p.frames > 600) { Log("vocal theme: %s: no channel group after 10 s; the instrumental plays", g_vocalSamples[p.sample].cue); p = {}; }
            continue;
        }
        int n = 0;                                                   // the event's own tracks: silent (every frame:
        g_fm.groupNumGroups(group, &n);                              // a track can be created later)
        for (int i = 0; i < n; i++) { void* g = NULL; if (g_fm.groupGetGroup(group, i, &g) == 0 && g) g_fm.groupSetMute(g, 1); }
        if (p.sound) continue;
        // First frame with a channel group: open the 2010 slice as a raw PCM stream and play
        // it in that group, so the event's own volume, fades and pause apply to it.
        const VocalSample& s = g_vocalSamples[p.sample];
        void* sys = NULL;
        BYTE ex[224] = {};                                           // FMOD_CREATESOUNDEXINFO (2.03, x64)
        int32_t cb = sizeof ex, ch = 2, rate = (int32_t)s.rate, fmt = 2;   // FMOD_SOUND_FORMAT_PCM16
        memcpy(ex + 0, &cb, 4); memcpy(ex + 4, &s.length, 4); memcpy(ex + 8, &s.offset, 4);
        memcpy(ex + 12, &ch, 4); memcpy(ex + 16, &rate, 4); memcpy(ex + 20, &fmt, 4);
        const unsigned mode = 0x2 | 0x80 | 0x1000;                   // FMOD_LOOP_NORMAL | FMOD_CREATESTREAM | FMOD_OPENRAW
        int r1 = g_fm.groupGetSystem(group, &sys);
        int r2 = sys ? g_fm.createSound(sys, g_vocalFsb, mode, ex, &p.sound) : -1;
        int r3 = p.sound ? g_fm.playSound(sys, p.sound, group, 0, &p.chan) : -1;
        if (r1 || r2 || r3) {
            Log("vocal theme: %s couldn't play (FMOD %d/%d/%d); the instrumental plays", s.cue, r1, r2, r3);
            for (int i = 0; i < n; i++) { void* g = NULL; if (g_fm.groupGetGroup(group, i, &g) == 0 && g) g_fm.groupSetMute(g, 0); }
            if (p.sound) g_fm.soundRelease(p.sound);
            p = {};
            continue;
        }
        // Resume where the cue says (+0x148), wrapped to the loop's length (4 bytes per stereo frame).
        unsigned lenMs = (unsigned)((uint64_t)s.length / 4 * 1000 / s.rate);
        if (p.resumeMs > 0 && lenMs) g_fm.chanSetPos(p.chan, (unsigned)p.resumeMs % lenMs, 1 /* FMOD_TIMEUNIT_MS */);
        Log("vocal theme: %s: playing the 2010 vocal recording (%u ms, from %d ms)", s.cue, lenMs, p.resumeMs > 0 ? p.resumeMs % (int)lenMs : 0);
    }
    LeaveCriticalSection(&g_vocalLock);
}

// [audio] vocal_theme=1, from DllMain: builds the 2010 bank's path from [audio]
// legacy_install, detours the music cue start (checked against its prologue) and starts the
// check thread. g_vocalOn (which lets VocalTick run) is set only if the detour went in.
void InstallVocalTheme(const wchar_t* ini) {
    wchar_t dir[MAX_PATH];
    GetPrivateProfileStringW(L"audio", L"legacy_install", L"C:\\Program Files (x86)\\Disney Interactive Studios\\Toy Story 3",
                             dir, MAX_PATH, ini);
    char a[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, dir, -1, a, sizeof a, NULL, NULL);
    sprintf_s(g_vocalFsb, "%s\\data_dxw\\sound\\imusic_str.fsb", a);
    InitializeCriticalSection(&g_vocalLock);
    // FUN_1402657c0: mov rax,rsp; mov [rax+18h],rbx; push rbp; push rsi; push rdi; sub rsp,130h
    static const BYTE prologue[] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57, 0x48, 0x81, 0xEC, 0x30, 0x01, 0x00, 0x00 };
    g_cueStart = (PFN_CueStart)Mod_Detour(0x2657c0, prologue, sizeof prologue, (void*)H_CueStart);
    if (!g_cueStart) { Log("vocal theme: the music cue code doesn't match this build; the instrumental plays"); return; }
    g_vocalOn = true;
    if (HANDLE t = CreateThread(NULL, 0, VocalVerifyThread, NULL, 0, NULL)) CloseHandle(t);
    Log("vocal theme: on; checking %s", g_vocalFsb);
}

// [harness] script_fixes_2010=1, from DllMain before the first script loads: checks the
// three script-VM functions Native_GetIgnoreEntity and RegisterGetIgnoreEntity call, then
// redirects both script-reading calls to H_ReadScript. g_scriptFixesOk (the settings-sheet
// line) is true only if every step worked.
void InstallScriptFixes() {
    static const BYTE pop[] = { 0x48, 0x8B, 0x51, 0x38, 0x4C, 0x63, 0x02, 0x41, 0x8D, 0x40, 0xFF };   // ScriptVM_PopEntity
    static const BYTE push[] = { 0x4C, 0x8B, 0x49, 0x38, 0x4D, 0x63, 0x11, 0x41, 0x8D, 0x42, 0x01 };  // ScriptVM_PushValue
    static const BYTE reg[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57 };  // Script_RegisterNative
    g_readScript = (PFN_ReadTextFile)(ExeBase() + 0x250b60);
    g_scriptFixesOk = BytesAt(0x544740, pop, sizeof pop) && BytesAt(0x544cd0, push, sizeof push) &&
                      BytesAt(0x544f60, reg, sizeof reg) && RedirectCall(0x5420ea, 0x250b60, (void*)H_ReadScript) &&
                      RedirectCall(0x1e264e, 0x250b60, (void*)H_ReadScript);
    Log(g_scriptFixesOk ? "script fixes: the 2010 fixes to player, player2, carriable and zurgbot.sx are applied as they load "
                          "(a line per script follows; one missing in a level that uses it means another read path)"
                        : "script fixes: the script loading code doesn't match this build; NOT applied");
}
