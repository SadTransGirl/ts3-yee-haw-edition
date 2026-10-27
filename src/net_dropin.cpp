// net_dropin.cpp -- drop-in join support: the save transfer between PCs and the game-side
// helpers that capture, install, quit, prepare and reload the host's level, plus rejoin and the
// fade step.
//
// A co-op session runs both games in lockstep (net_lockstep.cpp), so both must start from
// the same world: the same save, the same level, loaded the same way. Without drop-in,
// both players go to the title screen and start together. Drop-in is the friendlier path:
// a friend connects while the host is playing; the host saves, takes its in-memory save
// buffer, quits to the title and sends the buffer; then on lockstep frame kDropLaunchFrame
// both PCs install it and load that world together (DropLaunch in net_lockstep.cpp). The
// level's "publish" (it has finished loading and applied the save; H_Publish in
// net_own_screen.cpp) then resets what must match.
//
// Addresses (0x14...) are in the game's exe, found by reverse engineering (the "decomp
// session", notes in re\); code uses RVAs, offsets from the exe's base 0x140000000.
// BytesAt / RipRefersTo check the game build before any game function is called.
//
// Entry points:
//  - SaveHandle(): net_session.cpp's packet handler, for the save packets, while
//    connecting or waiting to start. SaveSyncReady(): NetFrame, each frame while waiting
//    to start.
//  - The game-side helpers (DropSaveNow ... DropPrepareSave): the drop-in steps and
//    DropLaunch in net_lockstep.cpp; LotSelectorReset also from H_Publish.
//  - NetRejoin(): net_lockstep.cpp, when a finished session goes back to "connecting".
//  - H_FadeStepRender(): the game's render thread (InstallDeterminism redirects a call
//    to it); NetFadeStep(): each lockstep frame.

#include "proxy_shared.h"

// --- save transfer ------------------------------------------------------------
//
// Both games must start from the host's save. The game reads "Saved Game"
// only after the title screen (files.csv: first open after the first load),
// so during the handshake the host sends its save folder and the client backs
// up its own save and writes the host's before either side can start. The
// settings file (newsettings.bin) is read before the title, so if the client's
// differ, they're replaced and the client is asked to restart once.

// The transfer: one NET_SAVE_INFO (file names and sizes, total, hash), then the files
// back to back as one blob, in kSaveChunk-byte NET_SAVE_CHUNKs; the client answers
// NET_SAVE_ACK with the hash once it has all of it.
static const int kSaveChunk = 960, kSaveMaxFiles = 8, kSaveMaxBytes = 256 * 1024;
#pragma pack(push, 1)
struct NetSaveFile { char name[64]; uint32_t size; };
struct NetSaveInfoPkt { NetHdr h; uint32_t total; uint8_t count; NetSaveFile files[kSaveMaxFiles]; uint64_t hash; };
struct NetSaveChunkPkt { NetHdr h; uint32_t offset; uint16_t len; uint8_t data[kSaveChunk]; };
struct NetSaveAckPkt { NetHdr h; uint64_t hash; };
#pragma pack(pop)

int      g_netSaveSync = 1;                 // 0: no save transfer (switched off, or nothing to send)
static uint8_t* g_saveBlob;                 // host: what we send; client: what we receive
static uint8_t* g_saveHave;                 // client: which chunks arrived
static NetSaveInfoPkt g_saveInfo;
static bool     g_saveInfoKnown, g_saveDone, g_saveAcked;
static ULONGLONG g_saveLastSend;

// FNV-1a over the blob: the client applies it only once its copy hashes the same.
static uint64_t Fnv64(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

// Host: read the save folder into one blob (skipping .bak files).
static bool DropLoadForSending();
static bool SaveLoadForSending() {
    if (g_dropIn) return DropLoadForSending();
    wchar_t pat[MAX_PATH];
    swprintf_s(pat, L"%s\\*", g_cfg.saveDir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { Log("netplay: no save folder at %ls to send", g_cfg.saveDir); return false; }
    g_saveBlob = (uint8_t*)malloc(kSaveMaxBytes);
    NetSaveInfoPkt& in = g_saveInfo;
    in = {};
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        size_t len = wcslen(fd.cFileName);
        if (len > 4 && !_wcsicmp(fd.cFileName + len - 4, L".bak")) continue;
        if (in.count >= kSaveMaxFiles || in.total + fd.nFileSizeLow > (uint32_t)kSaveMaxBytes) continue;
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\%s", g_cfg.saveDir, fd.cFileName);
        FILE* f = NULL;
        if (_wfopen_s(&f, path, L"rb") || !f) continue;
        size_t got = fread(g_saveBlob + in.total, 1, fd.nFileSizeLow, f);
        fclose(f);
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, in.files[in.count].name, 64, NULL, NULL);
        in.files[in.count].size = (uint32_t)got;
        in.total += (uint32_t)got;
        in.count++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    in.h = NetHeader(NET_SAVE_INFO);
    in.hash = Fnv64(g_saveBlob, in.total);
    Log("netplay: sending this save to the other player: %u files, %u bytes", in.count, in.total);
    return true;
}

// Drop-in host: newsettings.bin from disk; the save buffer, world name, save slot
// and characters from memory ("@buffer", "@world", "@slot", "@avatars"). No
// "Saved Game" file: the friend's is left alone.
static bool DropLoadForSending() {
    if (!g_dropBuf || !g_dropSize) return false;
    g_saveBlob = (uint8_t*)malloc(kSaveMaxBytes);
    NetSaveInfoPkt& in = g_saveInfo;
    in = {};
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\newsettings.bin", g_cfg.saveDir);
    FILE* f = NULL;
    if (!_wfopen_s(&f, path, L"rb") && f) {
        size_t got = fread(g_saveBlob, 1, 4096, f);
        fclose(f);
        strcpy_s(in.files[in.count].name, "newsettings.bin");
        in.files[in.count].size = (uint32_t)got;
        in.total += (uint32_t)got;
        in.count++;
    }
    if (in.total + g_dropSize + sizeof g_dropWorld + 4 + sizeof g_dropAvatar > (uint32_t)kSaveMaxBytes) return false;
    memcpy(g_saveBlob + in.total, g_dropBuf, g_dropSize);
    strcpy_s(in.files[in.count].name, "@buffer");
    in.files[in.count].size = g_dropSize;
    in.total += g_dropSize;
    in.count++;
    uint32_t wl = (uint32_t)strlen(g_dropWorld) + 1;
    memcpy(g_saveBlob + in.total, g_dropWorld, wl);
    strcpy_s(in.files[in.count].name, "@world");
    in.files[in.count].size = wl;
    in.total += wl;
    in.count++;
    memcpy(g_saveBlob + in.total, &g_dropSlot, 4);
    strcpy_s(in.files[in.count].name, "@slot");
    in.files[in.count].size = 4;
    in.total += 4;
    in.count++;
    memcpy(g_saveBlob + in.total, g_dropAvatar, sizeof g_dropAvatar);
    strcpy_s(in.files[in.count].name, "@avatars");
    in.files[in.count].size = sizeof g_dropAvatar;
    in.total += sizeof g_dropAvatar;
    in.count++;
    in.h = NetHeader(NET_SAVE_INFO);
    in.hash = Fnv64(g_saveBlob, in.total);
    Log("drop-in: sending the save buffer (%u bytes), world \"%s\", save slot %d and characters \"%s\" / \"%s\"",
        g_dropSize, g_dropWorld, g_dropSlot, g_dropAvatar[0], g_dropAvatar[1]);
    return true;
}

// Host: send the info packet and every chunk (repeated by SaveSyncReady until acked).
static void SaveSendAll() {
    g_netT->SendReliable(&g_saveInfo, sizeof g_saveInfo);
    for (uint32_t off = 0; off < g_saveInfo.total; off += kSaveChunk) {
        NetSaveChunkPkt c = { NetHeader(NET_SAVE_CHUNK), off, (uint16_t)min((uint32_t)kSaveChunk, g_saveInfo.total - off) };
        memcpy(c.data, g_saveBlob + off, c.len);
        g_netT->SendReliable(&c, (int)offsetof(NetSaveChunkPkt, data) + c.len);
    }
}

// Client: everything arrived; back up our save and write the host's.
// Drop-in's "@" entries stay in memory ("@buffer" also switches on g_protectSave, see
// harness_core.cpp); real files are written into the save folder unless identical.
// Then the client is told what happens next.
static void SaveApply() {
    wchar_t backup[MAX_PATH];
    swprintf_s(backup, L"%s\\save_backup", g_outDir);
    Log("netplay: received the host's save; backing up this PC's save to %ls", backup);
    CopyDirFiles(g_cfg.saveDir, backup);
    CreateDirectoryW(g_cfg.saveDir, NULL);
    bool settingsChanged = false;
    uint32_t off = 0;
    for (int i = 0; i < g_saveInfo.count; i++) {
        const NetSaveFile& sf = g_saveInfo.files[i];
        wchar_t wname[64], path[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, sf.name, -1, wname, 64);
        if (wcschr(wname, L'\\') || wcschr(wname, L'/') || wcsstr(wname, L"..")) { off += sf.size; continue; }   // names only
        if (sf.name[0] == '@') {                            // drop-in: kept in memory, never written to disk
            if (!strcmp(sf.name, "@buffer") && sf.size && sf.size <= 0x32000) {
                free(g_dropBuf);
                g_dropBuf = (uint8_t*)malloc(sf.size);
                memcpy(g_dropBuf, g_saveBlob + off, sf.size);
                g_dropSize = sf.size;
                g_dropIn = true;
                g_protectSave = true;
            } else if (!strcmp(sf.name, "@slot") && sf.size == 4) {
                memcpy(&g_dropSlot, g_saveBlob + off, 4);
            } else if (!strcmp(sf.name, "@world") && sf.size) {
                size_t n = sf.size < sizeof g_dropWorld ? sf.size : sizeof g_dropWorld - 1;
                memcpy(g_dropWorld, g_saveBlob + off, n);
                g_dropWorld[n] = 0;
            } else if (!strcmp(sf.name, "@avatars") && sf.size == sizeof g_dropAvatar) {
                memcpy(g_dropAvatar, g_saveBlob + off, sizeof g_dropAvatar);
                g_dropAvatar[0][sizeof g_dropAvatar[0] - 1] = g_dropAvatar[1][sizeof g_dropAvatar[1] - 1] = 0;
            }
            Log("  %s (%u bytes): kept in memory", sf.name, sf.size);
            off += sf.size;
            continue;
        }
        swprintf_s(path, L"%s\\%s", g_cfg.saveDir, wname);
        if (!_wcsicmp(wname, L"newsettings.bin")) {          // read before the title: compare first
            FILE* f = NULL;
            uint8_t cur[4096];
            size_t n = 0;
            if (!_wfopen_s(&f, path, L"rb") && f) { n = fread(cur, 1, sizeof cur, f); fclose(f); }
            // newsettings.bin (0x2c0 bytes, magic 0x0cf9da7a): +0x10c windowed, +0x10d EULA, +0x110/+0x114
            // volumes, +0x118/+0x11c resolution, +0x120, +0x124 custom resolution are this PC's own.
            // Everything else (subtitles, FPS cap, language, button bindings) comes from the host.
            if (n == 0x2c0 && sf.size == 0x2c0 && *(const uint32_t*)cur == 0x0cf9da7a &&
                *(const uint32_t*)(g_saveBlob + off) == 0x0cf9da7a) {
                memcpy(g_saveBlob + off + 0x10c, cur + 0x10c, 0x128 - 0x10c);
                Log("netplay: kept this PC's screen, window and volume settings");
            }
            settingsChanged = n != sf.size || memcmp(cur, g_saveBlob + off, sf.size) != 0;
        }
        // Skip files that are already identical: no needless rewrites of a save.
        bool same = false;
        {
            FILE* f = NULL;
            if (!_wfopen_s(&f, path, L"rb") && f) {
                uint8_t* cur = (uint8_t*)malloc(sf.size + 1);
                size_t n = cur ? fread(cur, 1, sf.size + 1, f) : 0;
                same = cur && n == sf.size && memcmp(cur, g_saveBlob + off, sf.size) == 0;
                free(cur);
                fclose(f);
            }
        }
        if (!same) {
            FILE* f = NULL;
            if (!_wfopen_s(&f, path, L"wb") && f) { fwrite(g_saveBlob + off, 1, sf.size, f); fclose(f); }
        }
        Log("  %s (%u bytes)%s", sf.name, sf.size, same ? ": already identical, left alone" : "");
        off += sf.size;
    }
    // What the player is told next.
    g_saveDone = true;
    if (settingsChanged) {
        NetEnd("this PC's game settings differed from the host's. They've been copied over: restart the game and connect again");
        g_netSettingsNotice = true;
    } else if (g_dropIn) {                              // the host is mid-game: we're brought in automatically
        Status(0, kOvGreen, L"Joining %s's game...\nLoading their world, hang on.", g_netPeerNameW);
    } else if (g_feHooked && !OnPressStart()) {
        Status(0, kOvGreen, L"Got %s's save!\nGo to the Press Start screen, then hold Back + Start.", g_netPeerNameW);
    } else {
        Status(0, kOvGreen, L"Got %s's save!\nHold Back + Start to begin.", g_netPeerNameW);
    }
}

// One save packet (p, n bytes, of the given type). The host only takes the ack.
// The client stores the info and chunks, re-acks a repeat once done, and when the
// whole blob is in and its hash matches, acks and applies it.
void SaveHandle(const uint8_t* p, int n, uint8_t type) {
    if (type == NET_SAVE_ACK && g_net.role == 0 && n >= (int)sizeof(NetSaveAckPkt)) {
        if (((const NetSaveAckPkt*)p)->hash == g_saveInfo.hash && !g_saveAcked) {
            g_saveAcked = true;
            Log("netplay: the other player has this save");
        }
        return;
    }
    if (g_net.role != 1 || g_saveDone) {
        if (g_net.role == 1 && g_saveDone && type == NET_SAVE_INFO) {    // they missed our ack
            NetSaveAckPkt a = { NetHeader(NET_SAVE_ACK), g_saveInfo.hash };
            g_netT->SendReliable(&a, sizeof a);
        }
        return;
    }
    // Client, still receiving.
    if (type == NET_SAVE_INFO && n >= (int)sizeof(NetSaveInfoPkt) && !g_saveInfoKnown) {
        const NetSaveInfoPkt* in = (const NetSaveInfoPkt*)p;
        if (in->total > (uint32_t)kSaveMaxBytes || in->count > kSaveMaxFiles) return;
        g_saveInfo = *in;
        g_saveBlob = (uint8_t*)calloc(1, kSaveMaxBytes);
        g_saveHave = (uint8_t*)calloc(1, kSaveMaxBytes / kSaveChunk + 1);
        g_saveInfoKnown = true;
    } else if (type == NET_SAVE_CHUNK && g_saveInfoKnown && n >= (int)offsetof(NetSaveChunkPkt, data)) {
        const NetSaveChunkPkt* c = (const NetSaveChunkPkt*)p;
        if (c->offset % kSaveChunk || c->offset + c->len > g_saveInfo.total || n < (int)offsetof(NetSaveChunkPkt, data) + c->len) return;
        memcpy(g_saveBlob + c->offset, c->data, c->len);
        g_saveHave[c->offset / kSaveChunk] = 1;
    }
    // Complete and intact?
    if (!g_saveInfoKnown) return;
    for (uint32_t i = 0; i * kSaveChunk < g_saveInfo.total; i++) if (!g_saveHave[i]) return;
    if (Fnv64(g_saveBlob, g_saveInfo.total) != g_saveInfo.hash) return;       // wait for good copies
    NetSaveAckPkt a = { NetHeader(NET_SAVE_ACK), g_saveInfo.hash };
    g_netT->SendReliable(&a, sizeof a);
    SaveApply();
}

// Host: keep sending until acknowledged. Returns true once save sync is done.
// (Every 500 ms. With nothing to send, the transfer is switched off.) Client: true
// once the host's save is applied.
bool SaveSyncReady() {
    if (!g_netSaveSync) return true;
    if (g_net.role == 1) return g_saveDone;
    if (g_saveAcked) return true;
    if (!g_saveBlob && !SaveLoadForSending()) { g_netSaveSync = 0; return true; }
    if (GetTickCount64() - g_saveLastSend > 500) { SaveSendAll(); g_saveLastSend = GetTickCount64(); }
    return false;
}

// Game calls for drop-in (decomp session, re\SYNC_VIABILITY.md "drop-in via a quick reload").
// The game objects behind them: the storage (0x140ac6270; +0xa4 "ready", +0x28 its inner
// object, which holds the in-memory save buffer at +0x258, its size at +0x260 and the
// save state at +0x278) and the save-game object (0x140b29a60; +0x64 the save slot,
// +0x160 the world name, +0x1a0 the parsed save tree). Each getter returns NULL (or -1)
// while the game hasn't created the object.
static BYTE* StorageObj() { __try { return *(BYTE**)(ExeBase() + 0xac6270); } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; } }
static BYTE* StorageInner() { BYTE* st = StorageObj(); return st ? *(BYTE**)(st + 0x28) : NULL; }
int StorageState() { BYTE* in = StorageInner(); return in ? *(int*)(in + 0x278) : -1; }
BYTE* SaveGameObj() { __try { return *(BYTE**)(ExeBase() + 0xb29a60); } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; } }
// The game functions drop-in calls match this build (checked once, then cached):
// the save, the buffer allocator, EventMgr_Post and the audio stop.
bool DropCodeOk() {
    static int ok = -1;
    if (ok < 0) {
        static const BYTE save[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x05 };       // FUN_14049c930
        static const BYTE alloc[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x83, 0xB9, 0x60, 0x02, 0x00, 0x00, 0x00 };   // FUN_14027d3c0
        static const BYTE post[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x48, 0x83, 0xEC, 0x20 };  // EventMgr_Post
        static const BYTE stop[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x0F, 0x28, 0xDA };      // FUN_140273910
        ok = BytesAt(0x49c930, save, sizeof save) && RipRefersTo(0x49c936, 3, 7, 0xac6270) &&
             BytesAt(0x27d3c0, alloc, sizeof alloc) && BytesAt(0x49e160, post, sizeof post) && BytesAt(0x273910, stop, sizeof stop);
    }
    return ok == 1;
}
// Post a named game event (QuitGame, SwitchLevel, SetupLaunch...) through the game's
// event manager (0x140b29a70), as game code does. False if it doesn't exist yet.
bool PostGameEvent(const char* ev, const void* arg) {
    void* mgr = NULL;
    __try { mgr = *(void**)(ExeBase() + 0xb29a70); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!mgr) return false;
    typedef void (*PFN_Post)(void*, const char*, const void*, void*);
    ((PFN_Post)(ExeBase() + 0x49e160))(mgr, ev, arg, NULL);
    return true;
}
// Host: start the game's own save (FUN_14049c930). It finishes later: the drop-in
// step (net_lockstep.cpp) watches StorageState until the storage is idle again.
bool DropSaveNow() {
    BYTE* st = StorageObj();
    BYTE* so = SaveGameObj();
    if (!st || !so || !*(BYTE*)(st + 0xa4)) return false;          // storage not ready yet
    ((void (*)(void*))(ExeBase() + 0x49c930))(so);
    return true;
}
// The building-lot selector (DAT_14099b8c0, updated by FUN_1401ba9b0 at the start of
// WilburStandAloneLoop::vf11, before its pause check) outlives a level. A pending
// enter/exit transition (+0x33e / +0x340 with the countdown +0x344) forces a PM tick
// (FUN_1405fb830(PM,1)) when the countdown runs out. In drop-in run 90 the host's
// selector still had one from before the reload and forced the PM at frame 40, while
// held paused; the friend's fresh one didn't. Both PCs clear the pending state at the
// drop-in launch and at each level publish in a session, so both start idle.
void LotSelectorReset(const char* when) {
    static const DWORD kSelector = 0x99b8c0;
    if (!RipRefersTo(0x21ed85, 3, 7, kSelector) || !RipRefersTo(0x21ed8c, 1, 5, 0x1ba9b0)) return;
    __try {
        BYTE* s = *(BYTE**)(ExeBase() + kSelector);
        if (!s) return;
        BYTE enter = s[0x33e], fadeWait = s[0x33f], exitP = s[0x340], b348 = s[0x348], areaWait = s[0x349], fading = s[0x2c2];
        int count = *(int*)(s + 0x344);
        if (enter | fadeWait | exitP | b348 | areaWait | fading | count) {
            s[0x33e] = s[0x33f] = s[0x340] = s[0x348] = s[0x349] = s[0x2c2] = 0;
            *(int*)(s + 0x344) = 0;
            Log("lot selector: pending state cleared at %s (enter %u, fade wait %u, exit %u, +348 %u, area wait %u, "
                "fading %u, countdown %d)", when, enter, fadeWait, exitP, b348, areaWait, fading, count);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// The Performance Manager's async-loading byte (PerformanceManagerTS3 at
// 0x140b2ef20, +0x41) off now; true if it was on. See PatchPmAsyncOff.
bool PmAsyncClear() {
    __try {
        if (BYTE* pmgr = *(BYTE**)(ExeBase() + 0xb2ef20))
            if (*(volatile BYTE*)(pmgr + 0x41)) { *(volatile BYTE*)(pmgr + 0x41) = 0; return true; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// The drop world's entry in the world database (0x140b2c760), found by its
// "filename" field the way the character-select screen does (FUN_14012e800); -1
// if the code doesn't match or the world isn't listed.
int DropWorldIndex(void** dbOut) {
    BYTE* b = ExeBase();
    static const char kFilename[] = "filename";
    *dbOut = NULL;
    if (!BytesAt(0x7745d8, kFilename, sizeof kFilename) || !RipRefersTo(0x12e85a, 1, 5, 0x5b53a0)) return -1;
    void* db = *(void**)(b + 0xb2c760);
    if (!db) return -1;
    *dbOut = db;
    return ((int (*)(void*, const char*, const char*, int))(b + 0x5b53a0))(db, (const char*)(b + 0x7745d8), g_dropWorld, -1);
}

// Read or write the drop world's "DefaultAvatar" / "DefaultAvatar2" fields
// (getter FUN_1405be2b0 + FUN_140477200, setter FUN_1405f35b0, which copies the text).
static const BYTE kWdbGet[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xDA, 0x41, 0x83, 0xF9, 0xFE, 0x75, 0x07, 0x44 };
static const BYTE kWdbText[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x41, 0x10, 0x48, 0x8B };
static const BYTE kWdbSet[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x49, 0x8B, 0xD8, 0x41, 0x83, 0xF9, 0xFE, 0x75, 0x07, 0x44 };
static const char* const kAvatarField[2] = { "DefaultAvatar", "DefaultAvatar2" };

// Host: read both fields into g_dropAvatar (empty where unset); false if the world
// isn't found or the getters don't match.
static bool DropReadAvatars() {
    g_dropAvatar[0][0] = g_dropAvatar[1][0] = 0;
    void* db;
    int idx = DropWorldIndex(&db);
    if (idx < 0 || !BytesAt(0x5be2b0, kWdbGet, sizeof kWdbGet) || !BytesAt(0x477200, kWdbText, sizeof kWdbText)) return false;
    BYTE* b = ExeBase();
    for (int p = 0; p < 2; p++) {
        __try {
            void* field = NULL;
            ((void** (*)(void*, void**, const char*, int))(b + 0x5be2b0))(db, &field, kAvatarField[p], idx);
            const char* v = field ? ((const char* (*)(void*, int))(b + 0x477200))(field, 0) : NULL;
            if (v) strncpy_s(g_dropAvatar[p], v, _TRUNCATE);
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_dropAvatar[p][0] = 0; }
    }
    return true;
}

// Both PCs, at the launch: write g_dropAvatar into the world's entry idx.
bool DropWriteAvatars(void* db, int idx) {
    if (!BytesAt(0x5f35b0, kWdbSet, sizeof kWdbSet)) return false;
    for (int p = 0; p < 2; p++)
        ((void (*)(void*, const char*, const char*, int))(ExeBase() + 0x5f35b0))(db, kAvatarField[p], g_dropAvatar[p], idx);
    return true;
}

// Host: copy what drop-in sends from the game's memory: the save buffer (at most
// 0x32000 bytes, the size the game allocates), the world name, the save slot and
// the characters. True if there is a world to load.
bool DropCapture() {
    BYTE* in = StorageInner();
    BYTE* so = SaveGameObj();
    if (!in || !so) return false;
    BYTE* buf = *(BYTE**)(in + 0x258);
    uint32_t size = *(uint32_t*)(in + 0x260);
    if (!buf || !size || size > 0x32000) return false;
    free(g_dropBuf);
    g_dropBuf = (uint8_t*)malloc(size);
    memcpy(g_dropBuf, buf, size);
    g_dropSize = size;
    strncpy_s(g_dropWorld, (const char*)(so + 0x160), _TRUNCATE);
    g_dropSlot = *(int*)(so + 0x64);
    if (g_dropWorld[0]) {
        bool ok = DropReadAvatars();
        Log("drop-in: characters for \"%s\": player 1 \"%s\", player 2 \"%s\"%s", g_dropWorld, g_dropAvatar[0],
            g_dropAvatar[1], ok ? "" : " (world database code doesn't match: not read)");
    }
    return g_dropWorld[0] != 0;
}
// A level is really loaded: the physics world holds a level's bodies (menus and
// the boot screens have a handful) and the storage holds a loaded save.
// (Physics at 0x140b275c8, its world at +0x1168, the body count at world+0x0c.)
bool DropLevelUp() {
    __try {
        int n = -1;
        if (BYTE* phys = *(BYTE**)(ExeBase() + 0xb275c8))
            if (BYTE* world = *(BYTE**)(phys + 0x1168)) n = *(int*)(world + 0x0c);
        BYTE* in = StorageInner();
        if (!in) return false;
        return n > 100 && *(BYTE**)(in + 0x258) && *(uint32_t*)(in + 0x260) > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Put the session's save buffer into the storage, the way a load does.
bool DropInstall() {
    BYTE* in = StorageInner();
    if (!in || !g_dropBuf || !g_dropSize) return false;
    *(uint32_t*)(in + 0x260) = g_dropSize;
    BYTE* buf = ((BYTE* (*)(void*))(ExeBase() + 0x27d3c0))(in);   // existing buffer, or a new 0x32000 one
    if (!buf) return false;
    memcpy(buf, g_dropBuf, g_dropSize);
    return true;
}
// Leave the level: the audio stop call (FUN_140273910 on the audio object at
// 0x140ac2ec0), then the game's own QuitGame event.
void DropQuitToTitle() {
    void* audio = NULL;
    __try { audio = *(void**)(ExeBase() + 0xac2ec0); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (audio) ((void (*)(void*, int, float))(ExeBase() + 0x273910))(audio, 0, 0.0f);
    PostGameEvent("QuitGame", NULL);
}
// Player 1 on controller 1 on both PCs. Press Start at the title binds the
// pressing device (FUN_1405f1ee0 -> table 0x140b2eb08, FUN_1405f1e90 -> the
// settings block's binding words); the host did that at boot, the friend's game
// never did. Also the input driver's own player->device table.
void DropAlignBindings() {
    BYTE* b = ExeBase();
    BYTE* drv = NULL;
    __try { drv = *(BYTE**)(b + 0xac5380); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!drv) { Log("drop-in: no input driver; bindings not aligned"); return; }
    // Find controller 1's device: the driver's device list (count +0x458, pointers
    // from +0x460; entry 0 is skipped) holds its controller index at +0x2d0.
    int pad0 = -1;
    uint32_t count = *(uint32_t*)(drv + 0x458);
    for (uint32_t i = 1; i < count && i < 11 && pad0 < 0; i++) {
        BYTE* dev = *(BYTE**)(drv + 0x460 + i * 8);
        __try { if (dev && *(int*)(dev + 0x2d0) == 0) pad0 = (int)i; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (pad0 < 0) { Log("drop-in: controller 1's device not found; bindings not aligned"); return; }
    // (drv+0x4b8: the driver's player -> device table, logged before the change.)
    int before0 = *(int*)(drv + 0x4b8), before1 = *(int*)(drv + 0x4b8 + 4);
    uint32_t tbl0 = *(uint32_t*)(b + 0xb2eb08);
    // As Press Start does: profile object 0x140b2c5a0, slot 0 = this device, second per-slot value 0.
    ((void (*)(void*, int, int))(b + 0x5f1ee0))(b + 0xb2c5a0, 0, pad0);
    ((void (*)(void*, int, int))(b + 0x5f1e90))(b + 0xb2c5a0, 0, 0);
    void** vt = *(void***)drv;
    ((void (*)(void*, int))vt[0x88 / 8])(drv, 1);        // UnbindPlayer(1)
    ((void (*)(void*, int))vt[0x88 / 8])(drv, 0);        // UnbindPlayer(0), then bind it fresh: same on both PCs
    ((void (*)(void*, int, int))vt[0x80 / 8])(drv, 0, pad0);
    Log("drop-in: player 1 bound to device %d (was %d / %d, press-start table was %d)", pad0, before0, before1, (int)tbl0);
}

// Load the saved world on both PCs, same lockstep frame, from the title the way
// the game's own character-select screen does (FUN_14012e800): select the
// world in the world database, post SetupLaunch, close the frontend screens.
// [netplay] dropin_launch=switchlevel posts SwitchLevel instead (in-level loop).
// Both PCs take the same path through the level publish: the host's slot
// selected (a fresh client has -1, so "SLOT-1" would find no progress) and the
// parsed save tree emptied, so publish re-parses the installed buffer on both.
// The publish's load (FUN_140492900) only re-parses when the tree's root is
// completely empty: no data blocks (+0x08, count +0x10) and no child sections
// (+0x28, count +0x30). StartMenu::vf10's clear (FUN_14049c0d0) empties SLOT0-2
// and drops GLOBAL but keeps the root's blocks, so a game that loaded a save at
// boot skipped the re-parse and played on with empty slots (default town, player
// at the origin). So the root is then emptied too (FUN_1404881e0, as the save
// object's own teardown does), leaving it as a fresh game's.
// (The launch itself is DropLaunch in net_lockstep.cpp; this is its save part, run
// first, before DropInstall puts the buffer in.)
void DropPrepareSave() {
    BYTE* so = SaveGameObj();
    static const BYTE clr[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18,
                                0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF1 };
    static const BYTE empty[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18,
                                  0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x79, 0x10 };
    if (!so) return;
    int before = *(int*)(so + 0x64);
    if (g_dropSlot >= 0) *(int*)(so + 0x64) = g_dropSlot;
    bool cleared = false;
    if (BytesAt(0x49c0d0, clr, sizeof clr)) { ((void (*)(void*))(ExeBase() + 0x49c0d0))(so); cleared = true; }
    Log("drop-in: save slot %d (was %d); save tree %s", *(int*)(so + 0x64), before, cleared ? "cleared" : "NOT cleared (code doesn't match)");
    BYTE* root = *(BYTE**)(so + 0x1a0);
    if (!root) { Log("drop-in: no save tree root; nothing more to empty"); return; }
    uint32_t blocks = *(uint32_t*)(root + 0x10), sections = *(uint32_t*)(root + 0x30);
    if (blocks || sections) {
        if (!BytesAt(0x4881e0, empty, sizeof empty)) {
            Log("drop-in: save tree root still holds %u blocks, %u sections, and its emptying code doesn't match: "
                "this PC may not load the shared save", blocks, sections);
            return;
        }
        ((void (*)(void*))(ExeBase() + 0x4881e0))(root);
    }
    Log("drop-in: save tree root had %u blocks, %u sections; now %u, %u (the level load re-parses the shared save)",
        blocks, sections, *(uint32_t*)(root + 0x10), *(uint32_t*)(root + 0x30));
}

// Back to "connecting" for a new session, without restarting the game (from
// anywhere: with drop-in the next session reloads the host's level). Everything
// session-specific starts over; the start
// values the host picks put game time and entity keys above both PCs'.
// Called from net_lockstep.cpp after a session ended: when this player holds Back +
// Start for 20 frames (anywhere: with drop-in the host's world is reloaded), by itself
// for a host whose session never started, or in tests. NetRejoinResetLate resets
// net_determinism.cpp's state.
void NetRejoin() {
    memset(g_netLocal, 0, sizeof g_netLocal);           memset(g_netRemote, 0, sizeof g_netRemote);
    memset(g_netLocalTag, 0, sizeof g_netLocalTag);     memset(g_netRemoteTag, 0, sizeof g_netRemoteTag);
    memset(g_netHashLocal, 0, sizeof g_netHashLocal);   memset(g_netHashRemote, 0, sizeof g_netHashRemote);
    memset(g_netHashLocalTag, 0, sizeof g_netHashLocalTag); memset(g_netHashRemoteTag, 0, sizeof g_netHashRemoteTag);
    memset(g_netCur, 0, sizeof g_netCur);
    g_netFrame = 0;
    g_netLocalSentUpTo = 0xFFFFFFFF;
    g_netLastHash = 0;
    g_netLastHashFrame = 0;
    g_netPeerHello = g_netPeerReady = g_netLocalReady = false;
    g_netPeerBye = false;
    g_netPeerLastFrame = 0;
    g_netEndReason[0] = 0;
    g_netEndNeutralFrames = 0;
    g_netPeerValsKnown = g_netStartKnown = false;
    g_netStartVus = g_netStartKeys = 0;
    g_netRemoveP2 = false;
    g_dropIn = g_dropLaunched = false;
    g_dropStep = DROP_NONE;
    g_clientQuitPosted = false;
    g_clientFeSince = 0;
    free(g_saveBlob); g_saveBlob = NULL;                  // the host re-reads its save (it may have saved since)
    free(g_saveHave); g_saveHave = NULL;
    g_saveInfo = {};
    g_saveInfoKnown = g_saveDone = g_saveAcked = false;
    g_saveLastSend = 0;
    g_sharedActive = g_net.role == 0;
    g_sharedComboWas = false;
    NetRejoinResetLate();
    // Back to connecting, and tell the player.
    ULONGLONG now = GetTickCount64();
    g_netPhase = NP_CONNECTING;
    g_netPhaseSince = g_netLastHeard = now;
    g_netNextStatus = 0;
    Log("netplay: back to connecting for a new session (session %d next)", g_netSessions + 1);
    if (g_net.role == 0 && g_netT) g_netT->Advertise();   // joinable again (a session in progress isn't)
    if (g_net.dropIn)                                      // they're brought in as soon as they connect
        Status(8000, kOvBlue, L"Waiting for %s to join...\nKeep playing; they'll be brought in as soon as they connect. "
               L"Hold Back + Y to invite a friend.", g_netPeerNameW);
    else
        Status(8000, kOvBlue, L"Waiting for %s to join...\nKeep playing; they hold Back + Start, or Join Game in Steam.",
               g_netPeerNameW);
}

// Screen fades (see g_fadeOnMain): the render list's call does nothing in a session;
// NetFadeStep runs the FadeSystem update on the main thread at every lockstep step, before
// the update that polls it. Its clock read is pinned to the frame's start time, so a fade
// finishes on the same step on both PCs. Logs each change of the all-screens fade
// (FadeSystem_Get(4) = DAT_140b27d00[4]: [0] running, [1] level) so two logs can be compared.
// H_FadeStepRender stands in for the render-list item's call to the FadeSystem update
// (FUN_140473320, call at 0x1401ef399, redirected by InstallDeterminism); outside a
// session it runs the update as before. NetFadeStep: f is the lockstep frame.
void H_FadeStepRender() {
    if (g_clockLockstep && g_netPhase == NP_RUNNING) return;
    ((void (*)())(ExeBase() + 0x473320))();
}
void NetFadeStep(uint32_t f) {
    if (!g_fadeOnMain || !g_clockLockstep) return;
    ((void (*)())(ExeBase() + 0x473320))();
    static int lastRunning = -1, logged;
    __try {
        const int* fade = *(int**)(ExeBase() + 0xb27d00 + 4 * 8);
        int running = fade ? fade[0] : -1;
        if (running != lastRunning && logged < 60) {
            logged++;
            Log("fade %u: all-screens fade %s (level %.3f)", f, running == 1 ? "running" : running == 0 ? "done" : "?",
                fade ? *(const float*)(fade + 1) : 0.0f);
        }
        lastRunning = running;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
