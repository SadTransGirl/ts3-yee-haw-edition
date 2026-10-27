// In-game debug menu, drawn with the game's own UI fonts (DIN Condensed for
// items, Gill Sans Bold for the title, read from assets\ui\fonts\fonts.zip) on
// top of the finished frame, through gfx_d3d12.cpp's Present pass.
//
// Open/close: hold LB + RB and press Y on pad 1 (or F1). D-pad / left stick to
// move, A to toggle, Left/Right to change values, B to close. While it's open
// the game sees a neutral pad 1.
//
// Everything that changes the game runs on the game's main thread, from the
// pad poll (DebugMenu_FilterPad). Cheats are toggled the game's own way: the
// cheat's button sequence is written into the cheat code buffer, so the next
// CheatCodes_Update matches it, toggles it and plays cheat_on/cheat_off. In
// online co-op (netplay) everything that changes the simulation is locked, except
// through shared actions: player 1's menu actions travel with that frame's input
// and both PCs perform them on the same frame (lockstep: both PCs run the same
// frames from the same inputs; see "shared menu actions" at the end).
//
// How it fits with the rest. Unlike the harness modules this file doesn't include
// proxy_shared.h: it has its own ExeBase/BytesAt/RipRefersTo, and its Log is the
// proxy's, handed in at install. Entry points (declared at the end of proxy_shared.h):
//   DebugMenu_Install         DllMain (xinput_proxy.cpp), once, when xinput_harness.ini exists.
//   DebugMenu_FilterPad       every pad poll (xinput_proxy.cpp and the netplay modules' own
//                             pad paths); the once-per-frame poll runs MenuTick.
//   DrawOverlay               gfx_d3d12.cpp's Present pass, on the render thread (registered
//                             with GfxExt_InstallOverlay).
//   DebugMenu_SetNetplay, DebugMenu_EnableSharedActions, DebugMenu_PollAction and
//   DebugMenu_ApplyAction     the online session (net_transport, net_session, net_lockstep).
//   DebugMenu_SaveApplied     test_save.cpp's save-apply hook ("make this test save 100%").
//   DebugMenu_FrontendUpdate  net_own_screen.cpp's title-screen hook, when it owns the slot.
//   Mod_Detour                the proxy's modules (levels_scripts.cpp's vocal theme).
//
// Reading the addresses. An RVA is an offset from the exe's base (image base 0x140000000,
// so RVA 0xb291c0 is 0x140b291c0). The 0x14... addresses and the FUN_/DAT_ names are the
// game's functions and data as found by reverse engineering it (the decomp session); the
// other names (CheatCodes_Update, g_World...) were given there. Nothing is patched or
// called before its address is confirmed for this build: the Verify* functions check that
// a known instruction still refers to it (RipRefersTo, CallsTo), and detours compare the
// function's first bytes with a prologue array (BytesAt). A feature whose check fails is
// logged and left out of the menu.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "tinyzip.h"

// The proxy's Log, set by DebugMenu_Install. It stays NULL while the menu is off (no ini,
// enabled=0, or the fonts couldn't be read): the entry points check it and do nothing.
typedef void (*MenuLogFn)(const char* fmt, ...);
static MenuLogFn Log;

// gfx_d3d12.cpp's side: drawing the overlay (DrawOverlay) and the anti-aliasing switches
// the GRAPHICS rows drive.
typedef void (*GfxOverlayFn)(ID3D12Device*, ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, DXGI_FORMAT, UINT, UINT);
void GfxExt_InstallOverlay(GfxOverlayFn fn, void (*log)(const char* fmt, ...));
bool GfxExt_GetFxaa();
void GfxExt_SetFxaa(bool on);
int GfxExt_GetSmaa();                 // 0 off, 1 low .. 4 ultra; -1 not available (smaa=0 in the ini)
void GfxExt_SetSmaa(int level);

// ------------------------------------------------------------------ game addresses (image base 0x140000000)

static const DWORD RVA_CHEAT_TABLE  = 0xb291c0;  // 16 x 0x68: +0 id, +8 name, +0x14 on, +0x18 callback, +0x20 code[16], +0x60 code length
static const DWORD RVA_CHEAT_COUNT  = 0xb28c34;  // registered cheats (set at level init)
static const DWORD RVA_CHEAT_BUF    = 0xb28c40;  // int[16] recent button codes
static const DWORD RVA_CHEAT_BUFN   = 0xb28c80;  //   how many
static const DWORD RVA_CHEAT_LASTMS = 0xb28c84;  //   time of the last press (cleared after 500 ms)
static const DWORD RVA_NOW_MS       = 0x95482c;  // g_GameTimer+0xc: current ms
static const DWORD RVA_TIME_MODE    = 0x9562d8;  // game speed mode: 9 normal, 5 x2, 4 x1/2, 3 x1/4, 2 x1/8, 0 stopped
static const DWORD RVA_TIME_DIRTY   = 0xb28c2a;  //   set to make CheatCodes_Update post TimeSpeed
static const DWORD RVA_GAME_FRAME   = 0x954820;  // g_GameTimer: frame counter
static const DWORD RVA_PLAYER_MGR   = 0xb2ef50;  // PlayerManager*: +4 count, +0x1a0 + slot*0x58 = actor
static const DWORD RVA_COMPONENT    = 0x558fb0;  // Component* (Actor*, const char* name)
// The Ammo and Stealth cheats only apply to the actor whose id is stored here,
// and nothing in the shipped game ever stores one, so both did nothing. The
// menu keeps it set to player 1's actor id (actor+0x38).
static const DWORD RVA_CHEAT_TARGET = 0xb291b0;
// Progress counters are stats: g_Stats holds 0x60-byte records (array +8, count
// +0x10; name at +4, current value at +0x4c). Coins are Stat_GOLD_COLLECTED (what coin
// pickups report and bandits steal from). (Gold stars, Stat_GOLDSTAR_COLLECTED, are
// earned from the Toy Box's mini objectives, so the menu doesn't hand them out.)
static const DWORD RVA_STATS        = 0xb2fdb8;  // g_Stats*
static const DWORD RVA_STAT_FIND    = 0x617c40;  // int (stats, name) -> index or -1
static const DWORD RVA_STAT_REPORT  = 0x632130;  // (stats, index, amount): adds for counter stats
// Unlockables are "locks" (assets\data.zip playerdb: 728 of them). The lock manager
// holds lock* at +0x10, count in a global; a lock's name is *(char**)(lock+0x10), its
// state +0xa0, its database unlock state +0x9c. States: 0 LOCKED, 1 PURCHASABLE_NEW,
// 2 PURCHASABLE, 3 PURCHASED, 4 DELIVERED, 5 AVAILABLE, 6 ACTIVE. Setting a state
// (FUN_1405f2480) posts "LockUnlocked" when it leaves LOCKED; the level graphs' Lock
// Monitors react to the states.
static const DWORD RVA_LOCKS        = 0xb2fde0;  // lock manager*
static const DWORD RVA_LOCK_COUNT   = 0xb2c838;  // int
static const DWORD RVA_LOCK_FIND    = 0x614c40;  // lock* (mgr, const char* name), case-insensitive
static const DWORD RVA_LOCK_SET     = 0x5f2480;  // (lock*, state)
static const DWORD RVA_EVENT_POST   = 0x49e160;  // EventMgr_Post(mgr, event, arg, 0)
static const DWORD RVA_EVENT_MGR    = 0xb29a70;  // g_EventManager*
static const DWORD RVA_WORLD        = 0xb2c760;  // g_World* (null until a level is loaded)
// Title screen: ScreenFrontend::vf37 (per-frame update) through its vtable slot.
// Its page field +0x290 = 0x10000000000 shows the developers' "Debug Options" page.
static const DWORD RVA_FRONTEND_VT37 = 0x77e220;
static const DWORD RVA_FRONTEND_VF37 = 0x126130;
// Level loading: WorldDb_SelectEntry turns a world row into "worlds\<name>\" through
// World_SetWorldPath(world, name, slot); slot 0 is the main world.
static const DWORD RVA_SET_WORLD_PATH = 0x4cf850;
// After a level loads, FUN_1405ae570 spawns an avatar at each Meridian_Player_N in
// the scene. With none it calls PlayerMgr_SpawnAvatar(mgr, NULL, &origin, 0, 0).
static const DWORD RVA_SPAWN_AVATAR = 0x5af7e0;
// Named-locator lookup, as FUN_1405ae570 does for "AvatarNewLocation_%d|Transform":
static const DWORD RVA_STR_MAKE   = 0x023fb0;   // engine string (out, cstr, allocator)
static const DWORD RVA_STR_ALLOC  = 0x9bb6d8;   //   allocator*, or FUN_1402215c0() if null
static const DWORD RVA_STR_ALLOC0 = 0x2215c0;
static const DWORD RVA_SCENES     = 0xb28c38;   // scene manager*
static const DWORD RVA_GET_SCENE  = 0x48ff20;   //   (mgr, 0) = the world's scene
static const DWORD RVA_FIND_NODE  = 0x022e40;   //   (scene, &string) = node; position at +0x48
// Teleport, the way Script_ActorSetPosition does it: write the position (actor+0x80),
// mark the transform (actor+0x50) dirty, then actor->vf(0xa8)(0) and vf(0xb8).
static const DWORD RVA_XFORM_DIRTY  = 0x56dab0;
static const DWORD RVA_ACTOR_SETPOS = 0x566550;   // Script_ActorSetPosition (checked only)
// Field of view: every perspective projection is built by FUN_1402e43c0(out, near, far,
// fovDegrees, aspect), called from the camera render update with the camera's values.
static const DWORD RVA_PROJ_SETUP   = 0x2e43c0;
// Free camera: the frame builder calls FUN_1402d86f0(cameraNode, aspect) for every
// viewport in its table. The node's update (vf 0x70) computes its world matrix at
// node+0xd0 (rows right, up, forward, position; 16 floats, w = 0,0,0,1) and hands it to
// the render view with FUN_1402e3fd0(*(*(node+0x260)+0x28), matrix). Handing it our own
// matrix afterwards moves only what's drawn: the game's camera, which activation and
// the audio listener read, is untouched.
static const DWORD RVA_CAM_UPDATE   = 0x2d86f0;
static const DWORD RVA_CAM_SETWORLD = 0x2e3fd0;
static const DWORD RVA_VP_NODES     = 0x99bd08;   // camera node per viewport, stride 0x40
static const DWORD RVA_VP_COUNT     = 0x99bf00;   // int

// The game exe's base address in memory (add an RVA to get a game address).
static BYTE* ExeBase() { return (BYTE*)GetModuleHandleW(NULL); }

// True if the game's bytes at rva are expect (n bytes); false if they differ or can't be read.
static bool BytesAt(DWORD rva, const void* expect, size_t n) {
    __try {
        return memcmp(ExeBase() + rva, expect, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The "call rel32" at callRva goes to targetRva.
static bool CallsTo(DWORD callRva, DWORD targetRva) {
    __try {
        BYTE* p = ExeBase() + callRva;
        return p[0] == 0xE8 && p + 5 + *(int32_t*)(p + 1) == ExeBase() + targetRva;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Writes a 14-byte absolute jump to target at 'at': jmp [rip+0], then the 8-byte address.
static void WriteAbsJmp(BYTE* at, void* target) {
    at[0] = 0xFF; at[1] = 0x25;                    // jmp qword ptr [rip+0]
    *(int32_t*)(at + 2) = 0;
    memcpy(at + 6, &target, 8);
}

// Detour a function whose first 'len' bytes (>= 14, whole instructions, nothing
// RIP-relative) are 'prologue'. Returns a callable trampoline to the original.
static void* Detour(DWORD rva, const BYTE* prologue, size_t len, void* hook) {
    if (len < 14 || !BytesAt(rva, prologue, len)) return NULL;
    BYTE* fn = ExeBase() + rva;
    // The trampoline: the saved prologue, then a jump back into the function after it.
    BYTE* tramp = (BYTE*)VirtualAlloc(NULL, 64, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!tramp) return NULL;
    memcpy(tramp, prologue, len);
    WriteAbsJmp(tramp + len, fn + len);
    FlushInstructionCache(GetCurrentProcess(), tramp, len + 14);
    // Then the function's first 14 bytes become a jump to the hook.
    DWORD old;
    if (!VirtualProtect(fn, 14, PAGE_EXECUTE_READWRITE, &old)) return NULL;
    WriteAbsJmp(fn, hook);
    VirtualProtect(fn, 14, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 14);
    return tramp;
}
// The same for the proxy's modules (patch_util.cpp has call-site redirects but no detours).
void* Mod_Detour(DWORD rva, const BYTE* prologue, size_t len, void* hook) { return Detour(rva, prologue, len, hook); }

// True if the instruction at insnRva starts with opcode (opLen bytes) and the RIP-relative
// operand right after it (insnLen: the whole instruction's length) addresses targetRva. This
// is how a global's address is confirmed: by an instruction in the game that uses it.
static bool RipRefersTo(DWORD insnRva, const BYTE* opcode, size_t opLen, size_t insnLen, DWORD targetRva) {
    __try {
        BYTE* p = ExeBase() + insnRva;
        if (memcmp(p, opcode, opLen) != 0) return false;
        return p + insnLen + *(int32_t*)(p + opLen) == ExeBase() + targetRva;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Checked once at install: each address is confirmed by an instruction that uses it.
static bool VerifyGame() {
    static const BYTE cmpEbp[] = { 0x3B, 0x2D };        // cmp ebp, [cheat count]      (CheatCodes_Update)
    static const BYTE movEbx[] = { 0x89, 0x1D };        // mov [cheat buffer count], ebx
    static const BYTE movR8[]  = { 0x4C, 0x8B, 0x05 };  // mov r8, [player manager]
    static const BYTE cmpEax[] = { 0x39, 0x05 };        // cmp [time mode], eax        (time cheat callback)
    static const BYTE movB[]   = { 0xC6, 0x05 };        // mov byte [time dirty], 1
    static const BYTE comp[]   = { 0x48, 0x8B, 0x89, 0x70, 0x03, 0x00, 0x00 };   // mov rcx, [rcx+370h]
    static const BYTE movRax[] = { 0x48, 0x8B, 0x05 };  // mov rax, [cheat target]      (ammo use)
    bool ok = RipRefersTo(0x48a783, cmpEbp, 2, 6, RVA_CHEAT_COUNT) &&
              RipRefersTo(0x48a777, movEbx, 2, 6, RVA_CHEAT_BUFN) &&
              RipRefersTo(0x2187f0, movR8, 3, 7, RVA_PLAYER_MGR) &&
              RipRefersTo(0x4a49b9, cmpEax, 2, 6, RVA_TIME_MODE) &&
              RipRefersTo(0x4a49bf, movB, 2, 7, RVA_TIME_DIRTY) &&
              RipRefersTo(0x595ec0, movRax, 3, 7, RVA_CHEAT_TARGET);
    __try {
        ok = ok && memcmp(ExeBase() + RVA_COMPONENT, comp, sizeof comp) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

// Stats: the reset at save load (FUN_1404886e0) looks a stat up and reports to it.
static bool VerifyStats() {
    static const BYTE movRbx[] = { 0x48, 0x8B, 0x1D };  // mov rbx, [g_Stats]
    return RipRefersTo(0x488789, movRbx, 3, 7, RVA_STATS) && CallsTo(0x48879a, RVA_STAT_FIND) &&
           CallsTo(0x4887aa, RVA_STAT_REPORT);
}

// Locks: the LockManager_SetLockStateByName script native finds the lock and sets its
// state; the count getter is a single mov.
static bool VerifyLocks() {
    static const BYTE movRcx[] = { 0x48, 0x8B, 0x0D }, movEax[] = { 0x8B, 0x05 };
    return RipRefersTo(0x0b8329, movRcx, 3, 7, RVA_LOCKS) && CallsTo(0x0b8330, RVA_LOCK_FIND) &&
           CallsTo(0x0b833f, RVA_LOCK_SET) && RipRefersTo(0x5bbe40, movEax, 2, 6, RVA_LOCK_COUNT);
}

// Level switching: the game's own "SwitchLevel" event, as level scripts post it.
static bool VerifyLevelSwitch() {
    static const BYTE post[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x48, 0x83, 0xEC, 0x20 };
    static const BYTE movRcx[] = { 0x48, 0x8B, 0x0D };  // mov rcx, [g_EventManager]
    static const BYTE movRax[] = { 0x48, 0x8B, 0x05 };  // mov rax, [g_World]          (WorldDb_Get)
    if (!RipRefersTo(0x54516f, movRcx, 3, 7, RVA_EVENT_MGR) || !RipRefersTo(0x5bb6a0, movRax, 3, 7, RVA_WORLD)) return false;
    __try {
        return memcmp(ExeBase() + RVA_EVENT_POST, post, sizeof post) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The game's variable at rva, as a T*.
template <class T> static T* At(DWORD rva) { return (T*)(ExeBase() + rva); }

// ------------------------------------------------------------------ state

// HEADER: a section title. INFO: text only. TOGGLE: on/off (A or Left/Right). CHOICE: a
// value Left/Right cycles through. ACTION: does something on A. The id says which
// (Activate).
enum RowKind { ROW_HEADER, ROW_INFO, ROW_TOGGLE, ROW_CHOICE, ROW_ACTION };
enum RowId { ID_NONE, ID_CHEAT, ID_GOD, ID_SPEED, ID_COINS, ID_UNLOCKALL, ID_LOCKPICK, ID_LOCKSTATE,
             ID_FXAA, ID_SMAA, ID_FOV, ID_FREECAM, ID_LEVEL, ID_LOAD, ID_DEBUGOPTS,
             ID_TPSLOT, ID_TP_OTHER, ID_TP_SAVE, ID_TP_GO, ID_TP_SPAWN, ID_TP_SPAWNGO, ID_HUNDRED, ID_PRESET, ID_CLOSE };
static bool g_statsOk, g_locksOk;                // the stats / lock code matched this build

// Lock states 0..6 by name (see RVA_LOCKS).
static const char* kLockStates[] = { "LOCKED", "PURCHASABLE_NEW", "PURCHASABLE", "PURCHASED", "DELIVERED", "AVAILABLE", "ACTIVE" };
// Locks worth poking at by hand: the cut Skate Park module (Western Town still has
// its locator, lock monitors and entry mission), Toy Box modules nothing references,
// and vehicles, portals and areas with no script references.
static const char* kLockPicks[] = {
    "CAR_SkatePark", "CAR_StuntTrack", "MOD_Dragon", "MOD_Robots", "MOD_Slime", "MOD_UFO", "MOD_UFOStory",
    "MOD_EnchantedGlen", "MOD_HauntedHouse", "MOD_SpacePort", "CAR_SpaceBuggy", "CAR_RallyRacer",
    "CAR_SuperJump", "CAR_ShackJump", "CAR_SkiBall", "CAR_Corkscrew", "TP_Portal01", "AREA_Canal",
    "AREA_Suburbs", "AREA_Farm", "AREA_Quarry", "AREA_Town", "CC_SportsCar_Flame",
};
static const int kLockPickCount = sizeof kLockPicks / sizeof kLockPicks[0];
// Index of a lock in kLockPicks (any case), or -1.
static int LockPickIndex(const char* name) {
    for (int i = 0; i < kLockPickCount; i++) if (!_stricmp(kLockPicks[i], name)) return i;
    return -1;
}

// One-press presets for content the game ships behind locks it never opens (re\UNLOCKS_CHECK.md),
// through the lock editor's own shared action, so they also work (for both players) in co-op.
// Use them in the Toy Box town. A lock watcher can award a Steam achievement on a lock change;
// none of these is a watched lock, but test_save=1 blocks achievements anyway.
//   Skate Park: the module, its entrance trigger, lock monitors and missions all ship. The first
//     press makes it AVAILABLE, the second ACTIVE (graph lock monitors fire on each change), then
//     walk into its entrance (TRIG_SkateParkEnt, near LOC_SkateParkBeacon1).
//   Two car bodies with no other way in: CAR_SuperJump (rallycar_black_body) and CC_SportsCar_Flame
//     (sportscar_haunted_body). AVAILABLE, then look in car customization.
struct LockPreset { const char* label; const char* lock; bool twoStep; };
static const LockPreset kLockPresets[] = {
    { "Skate Park (press twice, then walk in)", "CAR_SkatePark", true },
    { "Car body: black rally car", "CAR_SuperJump", false },
    { "Car body: haunted sports car", "CC_SportsCar_Flame", false },
};
static const int kLockPresetCount = sizeof kLockPresets / sizeof kLockPresets[0];
// The state the next press sets, or -1 when there's nothing left to do.
static int PresetTarget(int i, int state) {
    if (state < 0) return -1;
    if (state < 5) return 5;
    return kLockPresets[i].twoStep && state == 5 ? 6 : -1;
}
static int g_lockPick;                      // the lock the Lock row shows (kLockPicks index)
static int g_unlockedCount = -1;            // result of the last "unlock everything"

// Loadable worlds (the load path doesn't check the "debug" flag; wii_only rows
// load as "hidden": no HUD). After loading, the game spawns the player at the
// scene's Meridian_Player_N points, or at the origin if there are none. Worlds
// with no geometry at all (Holo_Lott, HoloVlad, holochrisw) or no script
// (modelviewer) aren't listed.
//   row:     world-database row posted with SwitchLevel
//   files:   load this archive instead of the row's own (complete worlds that have
//            no row of their own borrow a row with suitable settings)
//   spawnAt: scene locator to spawn at when the world has no spawn points
struct Level { const char* row; const char* label; const char* files; const char* spawnAt; };
static const Level kLevels[] = {
    { "WesternTown", "Toy Box: Western Town" },       { "WT_StuntPark", "Toy Box: Stunt Park" },
    { "WT_Trendy", "Toy Box: Trendy town (cut)" },
    { "WT_SkatePark", "Toy Box: Skate Park (unfinished)", NULL, "StartPosition_P2_WT_Mod_SkatePark|Transform" },
    { "EnchantedGlen_Interior", "Toy Box: Enchanted Glen" }, { "HauntedHouse_Tutorial", "Toy Box: Haunted House" },
    { "SpacePort_interior", "Toy Box: Space Port" },  { "GUI_toybarn", "Toy Barn" },
    { "ST_Train", "Story: Train Rescue" },            { "ST_AndysRoom", "Story: Andy's Room" },
    { "ST_DayCare", "Story: Daycare" },               { "ST_Bonnie_Woody", "Story: Bonnie's (Woody)" },
    { "ST_Bonnie_Woody_Rocket", "Story: Bonnie's (rocket)" }, { "ST_PrisonBreak", "Story: Prison Break" },
    { "ST_JunkYard", "Story: Junkyard" },             { "ST_HauntedBakery", "Story: Haunted Bakery" },
    { "ST_BuzzGame", "Story: Buzz asteroid game" },   { "ST_BuzzGame_INT", "Story: Buzz game (intro)" },
    { "ST_AndysBasement", "Story: Andy's Basement (Wii, no HUD)" }, { "ST_BuzzFlight", "Story: Buzz flight (Wii, no HUD)" },
    { "GUI_StoryMode_Boardgame", "Board game menu" },
    { "WT_Trendy", "Hidden: Trendy town, no AI", "wt_trendynoai" },
    { "WT_Trendy", "Hidden: Trendy town, no buildings", "wt_trendynobldg" },
    { "Holo_Template", "Hidden: Bonnie's (part 4)", "st_bonnie_woody4" },
    { "HoloTest", "Hidden: holopeter", "holopeter" },
    // Sets other levels load for cutscenes or as building exteriors. They have spawn
    // points (in their .ap) and collision; those without collision would drop you
    // through the floor and aren't listed (cin_buzztrench01/02, cin_buzzland02,
    // st_train_cine, st_woodybullseyechase_world).
    { "HoloRoss", "Set: Enchanted Glen exterior", "enchantedglen" },
    { "HoloRoss", "Set: Haunted House exterior", "hauntedhouse" },
    { "HoloRoss", "Set: Space Port exterior", "spaceport" },
    { "HoloTest", "Set: Andy's room (cutscene)", "cin_st_andysroom" },
    { "HoloTest", "Set: Andy's hallway (cutscene)", "cin_st_andysroom_hallway" },
    { "HoloTest", "Set: Buzz's planet (cutscene)", "cin_buzzland01" },
    { "HoloTest", "Set: Woody & Bullseye train", "st_woodybullseyetrain_world" },
    { "Holo_Template", "Set: Bonnie's house (cutscene)", "st_bonnie_woody_cin" },
    { "Holo_Template", "Set: Bonnie's house (cutscene 2)", "st_bonnie_woody_cin02" },
    { "HoloTest", "Set: Story mode hub", "gui_storymode", "LOC_Woody|Transform" },
    { "HoloTest", "Dev: tiny racer ring test", "holo_del_empty-tiniracer", "LOC_JempGenPlayer1|Transform" },
    // re\wii_levels\DEV_WORLDS.md. The Western Town AI test row borrows Western Town's files, so it plays on
    // (and saves to) the real Toy Box save, and its 25 creature presets stay in the catalog until restart.
    { "WesternTownAi", "Dev: Western Town AI test (uses your Toy Box save!)", "westerntown" },
    { "modelviewer", "Dev: model viewer room (empty)", NULL, "START_Buzz" },
    { "HoloPlayground", "Dev: HoloPlayground" },      { "holoempty", "Dev: holoempty (test room)" },
    { "TrainStub", "Dev: TrainStub (Wii, no HUD)" },  { "ShootingGallery", "Dev: Shooting gallery" },
};
static const int kLevelCount = sizeof kLevels / sizeof kLevels[0];
static int  g_level;                         // the level the Level row shows (kLevels index)
static bool g_levelOk, g_levelHooksOk;       // SwitchLevel verified; both level-loading detours in
static const Level* volatile g_switchTo;     // posted on the next tick (main thread)

// The level the menu loaded last; its file redirect and spawn point stay in force
// (reloads, checkpoints) until the game sets up a different main world.
static const Level* volatile g_menuLevel;
static char g_mainWorld[128];                // slot-0 world name as last set up (after redirect)

typedef void (*PFN_SetWorldPath)(void* world, const char* name, int slot);
static PFN_SetWorldPath g_setWorldPathOrig;

// Detour of World_SetWorldPath. For the main world (slot 0): a menu level that borrows
// another world's files gets those files' name instead of its row's, and once a different
// world sets up, the menu level is forgotten. Notes the name actually loaded either way.
static void SetWorldPathHook(void* world, const char* name, int slot) {
    const Level* lv = g_menuLevel;
    if (slot == 0 && name && *name) {
        if (lv && lv->files && _stricmp(name, lv->row) == 0) {
            if (Log) Log("debug menu: loading %s's files for row %s", lv->files, name);
            name = lv->files;
        } else if (lv && _stricmp(name, lv->files ? lv->files : lv->row) != 0) {
            g_menuLevel = NULL;                      // something else is loading now
        }
        strncpy_s(g_mainWorld, name, _TRUNCATE);
    }
    g_setWorldPathOrig(world, name, slot);
}

typedef void* (*PFN_StrMake)(void** out, const char* s, void* alloc);
typedef void* (*PFN_StrAlloc0)();
typedef void* (*PFN_GetScene)(void* mgr, unsigned index);
typedef BYTE* (*PFN_FindNode)(void* scene, void** str);

// World position of a named scene locator ("Name|Transform"), the way the game
// finds "AvatarNewLocation_N|Transform".
static bool LocatorPos(const char* path, float* xyz) {
    __try {
        // An engine string for the name, from the game's string allocator.
        BYTE* base = ExeBase();
        void* alloc = *(void**)(base + RVA_STR_ALLOC);
        if (!alloc) alloc = ((PFN_StrAlloc0)(base + RVA_STR_ALLOC0))();
        void* str = NULL;
        ((PFN_StrMake)(base + RVA_STR_MAKE))(&str, path, alloc);
        // Look the node up in the world's scene.
        void* scene = ((PFN_GetScene)(base + RVA_GET_SCENE))(*(void**)(base + RVA_SCENES), 0);
        BYTE* node = scene ? ((PFN_FindNode)(base + RVA_FIND_NODE))(scene, &str) : NULL;
        BYTE* hdr = (BYTE*)str - 0x18;               // release the string as the game does
        if (InterlockedDecrement((LONG*)(hdr + 0x10)) <= 0) {
            void* a = *(void**)hdr;
            (*(void (**)(void*, void*))(*(BYTE**)a + 0x10))(a, hdr);
        }
        if (!node) return false;
        void* owner = *(void**)(node + 0x30);       // bring the cached transform up to date
        if (*(uint16_t*)(node + 0x40) != *(uint16_t*)(*(BYTE**)(node + 0x38) + 0x40) && owner)
            (*(void (**)(void*))(*(BYTE**)owner + 0x78))(owner);
        memcpy(xyz, node + 0x48, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

typedef void* (*PFN_SpawnAvatar)(void* mgr, void* params, float* pos, float heading, BYTE flag);
static PFN_SpawnAvatar g_spawnAvatarOrig;

// Detour of PlayerMgr_SpawnAvatar. The no-spawn-point call (params NULL) for a world the
// menu loaded with a spawnAt locator spawns at that locator instead of the origin.
static void* SpawnAvatarHook(void* mgr, void* params, float* pos, float heading, BYTE flag) {
    const Level* lv = g_menuLevel;
    float at[3];
    // g_mainWorld is the name actually loaded: the borrowed files' for a row that swaps them.
    if (!params && lv && lv->spawnAt && _stricmp(g_mainWorld, lv->files ? lv->files : lv->row) == 0) {
        if (LocatorPos(lv->spawnAt, at)) {
            if (Log) Log("debug menu: no spawn points in %s; spawning at %s (%.1f %.1f %.1f)",
                         lv->row, lv->spawnAt, at[0], at[1], at[2]);
            pos = at;
        } else if (Log) {
            Log("debug menu: no spawn points in %s and %s wasn't found; spawning at the origin", lv->row, lv->spawnAt);
        }
    }
    return g_spawnAvatarOrig(mgr, params, pos, heading, flag);
}

// Installs the two level-loading detours above. Offline at install; when the proxy starts
// in netplay mode, later, from DebugMenu_SetNetplay(false) or DebugMenu_EnableSharedActions.
static void HookLevelLoading() {
    static const BYTE setPath[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x57,
                                    0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00 };
    static const BYTE spawn[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89,
                                  0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x70 };
    static const BYTE movRcx[] = { 0x48, 0x8B, 0x0D }, movRax[] = { 0x48, 0x8B, 0x05 };
    // FUN_1405ae570's own locator lookup confirms every address LocatorPos uses.
    bool lookupOk = RipRefersTo(0x5ae831, movRcx, 3, 7, RVA_SCENES) && RipRefersTo(0x5ae807, movRax, 3, 7, RVA_STR_ALLOC) &&
                    CallsTo(0x5ae813, RVA_STR_ALLOC0) && CallsTo(0x5ae829, RVA_STR_MAKE) &&
                    CallsTo(0x5ae838, RVA_GET_SCENE) && CallsTo(0x5ae84c, RVA_FIND_NODE);
    if (!lookupOk || !BytesAt(RVA_SET_WORLD_PATH, setPath, sizeof setPath) || !BytesAt(RVA_SPAWN_AVATAR, spawn, sizeof spawn)) {
        Log("debug menu: level-loading code doesn't match this build; no hidden levels or spawn fix");
        return;
    }
    g_setWorldPathOrig = (PFN_SetWorldPath)Detour(RVA_SET_WORLD_PATH, setPath, sizeof setPath, (void*)SetWorldPathHook);
    g_spawnAvatarOrig = (PFN_SpawnAvatar)Detour(RVA_SPAWN_AVATAR, spawn, sizeof spawn, (void*)SpawnAvatarHook);
    g_levelHooksOk = g_setWorldPathOrig && g_spawnAvatarOrig;
}

// Can this level load? One that swaps files or needs a spawn locator needs the level hooks.
static bool LevelAvailable(const Level& lv) { return g_levelHooksOk || (!lv.files && !lv.spawnAt); }

// Title screen hook (main thread).
typedef void (*PFN_FrontendUpdate)(BYTE* self);
static PFN_FrontendUpdate g_frontendOrig;
static volatile ULONGLONG g_frontendSeen;    // GetTickCount64 of the last title-screen update
static volatile bool g_wantDebugOptions;     // the Debug Options row asked; FrontendTick does it

static volatile bool g_frontendViaProxy;    // the proxy's own vf37 hook reports in instead

// Every title-screen frame: notes that the title is up (OnTitleScreen) and, if the Debug
// Options row asked, switches the title to the developers' Debug Options page (+0x290).
static void FrontendTick(BYTE* self) {
    g_frontendSeen = GetTickCount64();
    if (g_wantDebugOptions) {
        g_wantDebugOptions = false;
        __try {
            *(uint64_t*)(self + 0x290) = 0x10000000000ULL;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (Log) Log("debug menu: opening the developers' Debug Options page");
    }
}

// Our vf37: the menu's work, then the game's update.
static void FrontendUpdateHook(BYTE* self) {
    FrontendTick(self);
    g_frontendOrig(self);
}

// For a proxy that owns the ScreenFrontend vf37 slot itself (netplay mode): call this
// from that hook before the original, on the game thread, so the menu's title-screen
// detection and its Debug Options request keep working.
void DebugMenu_FrontendUpdate(void* screenFrontend) {
    if (!Log || !screenFrontend) return;
    g_frontendViaProxy = true;
    FrontendTick((BYTE*)screenFrontend);         // no request can be pending in netplay (SetNetplay clears it)
}

// The title screen updated within the last half second.
static bool OnTitleScreen() { return GetTickCount64() - g_frontendSeen < 500; }

// Puts FrontendUpdateHook in ScreenFrontend's vtable slot 37, only if the slot still holds
// the game's vf37 (so it backs off when another hook, such as the proxy's own, is there).
static void HookFrontend() {
    void** slot = (void**)(ExeBase() + RVA_FRONTEND_VT37);
    void* expect = ExeBase() + RVA_FRONTEND_VF37;
    __try {
        if (*slot != expect) return;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    g_frontendOrig = (PFN_FrontendUpdate)expect;
    void* hook = (void*)FrontendUpdateHook;
    DWORD old;
    if (VirtualProtect(slot, sizeof hook, PAGE_READWRITE, &old)) {
        *slot = hook;
        VirtualProtect(slot, sizeof hook, old, &old);
    }
}

// Put the original back (only if the slot still holds our hook).
static void UnhookFrontend() {
    void** slot = (void**)(ExeBase() + RVA_FRONTEND_VT37);
    void* hook = (void*)FrontendUpdateHook;
    DWORD old;
    if (*slot == hook && VirtualProtect(slot, sizeof hook, PAGE_READWRITE, &old)) {
        *slot = ExeBase() + RVA_FRONTEND_VF37;
        VirtualProtect(slot, sizeof hook, old, &old);
    }
}

// One menu line, as BuildRows makes it and DrawOverlay draws it.
struct Row {
    RowKind  kind;
    RowId    id;
    int      arg;          // cheat index
    bool     disabled;
    char     label[64];
    char     value[64];
    uint32_t valueColor;   // 0xAABBGGRR
};

// The whole menu at one moment: rows, selection, open or not. The main thread builds it
// (MenuTick); the render thread draws a copy (DrawOverlay).
static const int kMaxRows = 56;
struct Snapshot { Row rows[kMaxRows]; int count; int sel; bool open; };

static CRITICAL_SECTION g_lock;
static Snapshot g_snap;              // what the renderer draws (copied under g_lock)
// open: shown. netplay: an online session (in the proxy's netplay mode, also from install
// until the proxy first says there's no session).
// gameOk: the cheat/player addresses matched. god: god mode wanted; godApplied: set on the avatars.
static bool     g_open, g_netplay, g_gameOk, g_god, g_godApplied;
static bool     g_shared, g_canAct;          // online session: menu actions shared / this PC may act
static int      g_sel = -1;
static XINPUT_STATE g_pad0;          // last pad 1 state, before we neutralise it
static WORD     g_prevButtons;
static bool     g_justClosed;        // this frame's tick closed the menu: swallow the press that closed it too
static bool     g_prevF1;
static ULONGLONG g_repeatAt;                 // Up/Down held: when the selection moves again
static int      g_repeatDir;

// Colours are 0xAABBGGRR (the vertex colour's bytes are R, G, B, A).
static const uint32_t kWhite = 0xFFFFFFFF, kGrey = 0xFF9A9A9A, kGreen = 0xFF66E07A, kGold = 0xFF33C8FF,
                      kRed = 0xFF6A6AFF, kBlue = 0xFFFFC860;

// The Game speed row: RVA_TIME_MODE values and their names.
static const int kSpeedModes[] = { 9, 5, 4, 3, 2, 0 };
static const char* kSpeedNames[] = { "Normal", "2x", "1/2", "1/4", "1/8", "Frozen" };

// ------------------------------------------------------------------ game actions (main thread)

// Registered cheats (0 until a level has set them up), at most the table's 16.
static int CheatCount() {
    __try {
        int n = *At<int>(RVA_CHEAT_COUNT);
        return n < 0 ? 0 : n > 16 ? 16 : n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Cheat i's 0x68-byte table entry (layout at RVA_CHEAT_TABLE).
static BYTE* CheatEntry(int i) { return At<BYTE>(RVA_CHEAT_TABLE) + i * 0x68; }

// Copies cheat i's name; false if it can't be read.
static bool CheatName(int i, char* out, size_t n) {
    __try {
        const char* name = *(const char**)(CheatEntry(i) + 8);
        strncpy_s(out, n, name ? name : "?", _TRUNCATE);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Feed the cheat's own button sequence to the code buffer; the game toggles it next update.
static void ToggleCheat(int i) {
    __try {
        BYTE* e = CheatEntry(i);
        int len = *(int*)(e + 0x60);
        if (len <= 0 || len > 16) return;
        int* buf = At<int>(RVA_CHEAT_BUF);
        for (int k = 0; k < len; k++) buf[k] = ((int*)(e + 0x20))[k];
        *At<int>(RVA_CHEAT_BUFN) = len;
        *At<int>(RVA_CHEAT_LASTMS) = *At<int>(RVA_NOW_MS);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Cheat i's on flag (+0x14).
static bool CheatOn(int i) {
    __try {
        return CheatEntry(i)[0x14] != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The current game speed as a kSpeedModes index (0, Normal, if it's none of them).
static int SpeedIndex() {
    int mode = *At<int>(RVA_TIME_MODE);
    for (int i = 0; i < 6; i++) if (kSpeedModes[i] == mode) return i;
    return 0;
}

// Sets the speed mode and the dirty flag, so CheatCodes_Update posts TimeSpeed as the time cheats do.
static void SetSpeed(int idx) {
    *At<int>(RVA_TIME_MODE) = kSpeedModes[idx];
    *At<BYTE>(RVA_TIME_DIRTY) = 1;
}

typedef BYTE* (*PFN_Component)(BYTE* actor, const char* name);

// Player avatars' HealthTracker "invulnerable" flag (+0x3c), as the actor data's
// "invulnerable" key sets it. The game's own "God" cheat has no effect.
static void ApplyGod(bool on) {
    __try {
        BYTE* pm = *At<BYTE*>(RVA_PLAYER_MGR);
        if (!pm) return;
        int n = *(int*)(pm + 4);
        if (n > 4) n = 4;
        for (int slot = 0; slot < n; slot++) {
            BYTE* actor = *(BYTE**)(pm + 0x1a0 + slot * 0x58);
            if (!actor) continue;
            BYTE* ht = ((PFN_Component)(ExeBase() + RVA_COMPONENT))(actor, "HealthTracker");
            if (ht) ht[0x3c] = on ? 1 : 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

typedef int (*PFN_StatFind)(BYTE* stats, const char* name);
typedef void (*PFN_StatReport)(BYTE* stats, int index, int amount);

// Current value of a counter stat, or -1 if there's no such stat (yet).
static int StatValue(const char* name) {
    __try {
        BYTE* stats = *At<BYTE*>(RVA_STATS);
        if (!stats) return -1;
        int i = ((PFN_StatFind)(ExeBase() + RVA_STAT_FIND))(stats, name);
        if (i < 0) return -1;
        return *(int*)(*(BYTE**)(stats + 8) + (size_t)i * 0x60 + 0x4c);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Add to a stat the way pickups do (the game's own report call, so its totals,
// high-water mark and any cap stay consistent).
static void GiveStat(const char* name, int amount) {
    __try {
        BYTE* stats = *At<BYTE*>(RVA_STATS);
        if (!stats) return;
        int i = ((PFN_StatFind)(ExeBase() + RVA_STAT_FIND))(stats, name);
        if (i >= 0) ((PFN_StatReport)(ExeBase() + RVA_STAT_REPORT))(stats, i, amount);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

typedef BYTE* (*PFN_LockFind)(BYTE* mgr, const char* name);
typedef void (*PFN_LockSet)(BYTE* lock, int state);

// The lock named name (any case), or NULL (no lock manager yet, or no such lock).
static BYTE* FindLock(const char* name) {
    __try {
        BYTE* mgr = *At<BYTE*>(RVA_LOCKS);
        return mgr ? ((PFN_LockFind)(ExeBase() + RVA_LOCK_FIND))(mgr, name) : NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

// A lock's state (+0xa0), or -1 if there's no such lock.
static int LockState(const char* name) {
    __try {
        BYTE* lock = FindLock(name);
        return lock ? *(int*)(lock + 0xa0) : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Sets a lock's state through the game's setter (which posts "LockUnlocked" on leaving LOCKED).
static void SetLockState(BYTE* lock, int state) {
    __try {
        ((PFN_LockSet)(ExeBase() + RVA_LOCK_SET))(lock, state);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Every lock still LOCKED goes to the state the database unlocks it to (+0x9c),
// as earning it in play would: shop items become buyable, the rest available.
static int UnlockEverything() {
    int n = 0;
    __try {
        BYTE* mgr = *At<BYTE*>(RVA_LOCKS);
        int count = *At<int>(RVA_LOCK_COUNT);
        if (!mgr || count <= 0 || count > 4096) return 0;
        BYTE** locks = *(BYTE***)(mgr + 0x10);
        for (int i = 0; i < count; i++) {
            BYTE* lock = locks[i];
            if (!lock || *(int*)(lock + 0xa0) != 0) continue;
            int to = *(int*)(lock + 0x9c);
            SetLockState(lock, to > 0 && to <= 6 ? to : 5);
            n++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (Log) Log("debug menu: unlocked %d lock(s)", n);
    return n;
}

// "Make this test save 100%" (re\TEST_SAVE_100.md, the quiet recipe). Only offered in the
// proxy's test_save mode: the save lives in its own folder and the game's achievement
// queue push is blocked from boot. The game has no completion percentage or "unlock all",
// so 100% means what the achievement list checks: challenges passed, everything owned,
// the story finished, plus tutorials and one-shot area triggers marked done.
// Changing any of it while a level runs plays cinematics: the level graph's lock monitors
// poll their lock every frame and fire on any change, and the event-posting setters start
// mission intros and rewards. So the row only arms it (g_hundredArmed), and the proxy calls
// DebugMenu_SaveApplied inside the next level load's save apply (FUN_140492900, right after
// the apply call at 0x140492c08, before LoadedSavedGame is delivered). Every write is a raw
// field write there, and the lock monitors re-read their state on LoadedSavedGame without
// firing. Known gaps: graph gate/count states and player-placed buildings can't be faked
// generically (start from a copy of a well-played save); clothing sets aren't given their
// stagecoach flag.
static const DWORD RVA_GAMEVARS          = 0xb29a48;  // g_GameVars (pointer): named game variables
static const DWORD RVA_GAMEVARS_SETINT   = 0x4a3fb0;  // GameVars_SetInt(vars, name, value): no events
static const DWORD RVA_CHALLENGES        = 0xb2fdc8;  // ChallengeManager (pointer): nodes +0x10, count +0x18
static const DWORD RVA_TUTORIALS         = 0x99b868;  // TutorialManager (pointer): entries +0x48, count (u64) +0x50
static const DWORD RVA_PROXIMITY         = 0x99b8d8;  // CMeridianGNodeSerializer (pointer): nodes +8, count +0x10
static const DWORD RVA_SKIP_PRESENTATION = 0xb2f4b4;  // byte: 1 = challenge presentations resolve at once (dev flag)
typedef int (*PFN_GameVarsSetInt)(BYTE* vars, const char* name, int value);
static bool g_testSave;                      // the proxy's test_save mode is on (DebugMenu_SetTestSave)
static bool g_hundredOk;                     // the code it calls matches this build
static volatile bool g_hundredArmed;         // apply at the next level load's save apply
static int  g_skipPresentationFrames;        // frames left with challenge presentations skipped
static char g_hundredResult[48];

// The game-vars setter HundredStory calls matches this build (its prologue).
static bool VerifyHundred() {
    // GameVars_SetInt: mov [rsp+8],rbx; mov [rsp+10h],rbp; mov [rsp+18h],rsi
    static const BYTE setInt[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18 };
    return BytesAt(RVA_GAMEVARS_SETINT, setInt, sizeof setInt);
}

// Each list is (array of pointers, count); each element gets one raw write. Returns how many changed.
// Locks: every one the game can unlock goes to AVAILABLE (owned).
static int HundredLocks() {
    int n = 0;
    __try {
        BYTE* mgr = *At<BYTE*>(RVA_LOCKS);
        int count = *At<int>(RVA_LOCK_COUNT);
        if (!mgr || count <= 0 || count > 4096) return 0;
        BYTE** locks = *(BYTE***)(mgr + 0x10);
        for (int i = 0; i < count; i++) {
            BYTE* lock = locks[i];
            if (!lock) continue;
            int st = *(int*)(lock + 0xa0), earned = *(int*)(lock + 0x9c);
            // Only locks the game can unlock (a database unlock state); 5 AVAILABLE = owned. 6 ACTIVE stays.
            if (st < 5 && earned > 0 && earned <= 6) { *(int*)(lock + 0xa0) = 5; n++; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}
// Challenges: state (+0x94) PASSED and award level (+0x14c) at least 3. *total: how many there are.
static int HundredChallenges(int* total) {
    int n = 0;
    __try {
        BYTE* mgr = *At<BYTE*>(RVA_CHALLENGES);
        if (!mgr) return 0;
        BYTE** nodes = *(BYTE***)(mgr + 0x10);
        int count = *(int*)(mgr + 0x18);
        if (!nodes || count <= 0 || count > 4096) return 0;
        *total = count;
        for (int i = 0; i < count; i++) {
            BYTE* node = nodes[i];
            if (!node || *(int*)(node + 0x94) == 4) continue;
            *(int*)(node + 0x94) = 4;                      // PASSED
            if (*(int*)(node + 0x14c) < 3) *(int*)(node + 0x14c) = 3;   // award level: 3 taken as gold (a guess)
            n++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}
// Tutorials: each entry's done flag (+0x18).
static int HundredTutorials() {
    int n = 0;
    __try {
        BYTE* mgr = *At<BYTE*>(RVA_TUTORIALS);
        if (!mgr) return 0;
        BYTE** list = *(BYTE***)(mgr + 0x48);
        uint64_t count = *(uint64_t*)(mgr + 0x50);
        if (!list || count > 4096) return 0;
        for (uint64_t i = 0; i < count; i++) if (list[i] && !list[i][0x18]) { list[i][0x18] = 1; n++; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}
// One-shot area triggers (the proximity nodes' serializer): each node's ACTIVATED flag (+0x69).
static int HundredProximity() {
    int n = 0;
    __try {
        BYTE* ser = *At<BYTE*>(RVA_PROXIMITY);
        if (!ser) return 0;
        BYTE** nodes = *(BYTE***)(ser + 8);
        int count = *(int*)(ser + 0x10);
        if (!nodes || count <= 0 || count > 65536) return 0;
        for (int i = 0; i < count; i++) if (nodes[i] && !nodes[i][0x69]) { nodes[i][0x69] = 1; n++; }   // ACTIVATED
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}
// Story progress, through the game's own game-vars setter (it posts no events).
static int HundredStory() {
    // The board game reads "<Level>_C": 0 locked, 2 newly opened (plays the reveal), 1 open.
    // ST_Train counts as open once any other is; ST_GameDone_C = 1 places the crayon.
    static const char* const kStory[] = { "ST_AndysRoom_C", "ST_BuzzGame_C", "ST_BuzzFlight_C", "ST_DayCare_C",
                                          "ST_Bonnie_Woody_C", "ST_PrisonBreak_C", "ST_JunkYard_C",
                                          "ST_HauntedBakery_C", "ST_GameDone_C" };
    int n = 0;
    __try {
        if (BYTE* vars = *At<BYTE*>(RVA_GAMEVARS))
            for (const char* name : kStory) { ((PFN_GameVarsSetInt)(ExeBase() + RVA_GAMEVARS_SETINT))(vars, name, 1); n++; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}
// Coins up to at least 100000, written straight into the stat's value (+0x4c).
static void HundredCoins() {
    __try {
        BYTE* stats = *At<BYTE*>(RVA_STATS);
        if (!stats) return;
        int i = ((PFN_StatFind)(ExeBase() + RVA_STAT_FIND))(stats, "Stat_GOLD_COLLECTED");
        int* v = i >= 0 ? (int*)(*(BYTE**)(stats + 8) + (size_t)i * 0x60 + 0x4c) : NULL;
        if (v && *v < 100000) *v = 100000;             // raw: the report call can feed stat monitors
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Called by the proxy inside a level load's save apply (see above). Returns true, with a
// line for the screen, when it applied the armed 100%.
bool DebugMenu_SaveApplied(char* msg, size_t cap) {
    if (!g_hundredArmed || !g_testSave || !g_hundredOk) return false;
    g_hundredArmed = false;
    // Belt and braces: any challenge presentation that still starts in the next minute is
    // skipped and resolved at once (graph-driven reactions aren't covered).
    __try { *At<BYTE>(RVA_SKIP_PRESENTATION) = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_skipPresentationFrames = 60 * 60;
    int total = 0;
    int challenges = HundredChallenges(&total), locks = HundredLocks(), tutorials = HundredTutorials();
    int areas = HundredProximity(), story = HundredStory();
    HundredCoins();
    sprintf_s(g_hundredResult, "done: %d challenges", challenges);
    if (Log) Log("debug menu: test save 100%% in the save load: %d of %d challenges passed, %d lock(s) owned, %d tutorial(s) "
                 "and %d area trigger(s) marked done, %d story values, coins >= 100000; leave the level to save it",
                 challenges, total, locks, tutorials, areas, story);
    sprintf_s(msg, cap, "%d challenges, %d unlocks, %d tutorials, %d area triggers", challenges, locks, tutorials, areas);
    return true;
}
static void HundredTick() {                    // once a frame: the skip-presentation window runs out
    if (g_skipPresentationFrames > 0 && --g_skipPresentationFrames == 0) {
        __try { *At<BYTE>(RVA_SKIP_PRESENTATION) = 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (Log) Log("debug menu: test save 100%%: challenge presentations play normally again");
    }
}

// Point the Ammo/Stealth cheats at player 1 (the game never does).
static void ApplyCheatTarget() {
    __try {
        BYTE* pm = *At<BYTE*>(RVA_PLAYER_MGR);
        if (!pm || *(int*)(pm + 4) < 1) return;
        BYTE* actor = *(BYTE**)(pm + 0x1a0);
        if (actor) *At<uint64_t>(RVA_CHEAT_TARGET) = *(uint64_t*)(actor + 0x38);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The Ammo cheat's two checks (lobbed throws, and the weapon that counts ammo at
// +0x3d8) also require the weapon's owner key (+0x18) to equal the cheat target.
// Dropping that jnz makes Ammo apply to every such weapon, whoever holds it.
static bool PatchAmmoOwner() {
    static const DWORD sites[] = { 0x58e613, 0x595ed3 };
    static const BYTE cmpRdi[] = { 0x48, 0x39, 0x47, 0x18 }, cmpRcx[] = { 0x48, 0x39, 0x41, 0x18 };
    const BYTE* cmps[] = { cmpRdi, cmpRcx };
    __try {
        for (int i = 0; i < 2; i++) {
            BYTE* p = ExeBase() + sites[i];
            if (memcmp(p - 4, cmps[i], 4) != 0 || p[0] != 0x75) return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    for (DWORD rva : sites) {
        BYTE* p = ExeBase() + rva;
        DWORD old;
        if (!VirtualProtect(p, 2, PAGE_EXECUTE_READWRITE, &old)) return false;
        p[0] = 0x90;
        p[1] = 0x90;
        VirtualProtect(p, 2, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 2);
    }
    return true;
}

// Puts the two jnz back, where our nops still are (online: the game's own Ammo rule).
static void UnpatchAmmoOwner() {
    static const DWORD sites[] = { 0x58e613, 0x595ed3 };
    static const BYTE jnz[2][2] = { { 0x75, 0x41 }, { 0x75, 0x43 } };   // jnz +0x41 / +0x43
    for (int i = 0; i < 2; i++) {
        BYTE* p = ExeBase() + sites[i];
        DWORD old;
        if (p[0] != 0x90 || p[1] != 0x90 || !VirtualProtect(p, 2, PAGE_EXECUTE_READWRITE, &old)) continue;
        memcpy(p, jnz[i], 2);
        VirtualProtect(p, 2, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 2);
    }
}

typedef void* (*PFN_EventPost)(void* mgr, const char* event, const void* arg, void* extra);

// Post "SwitchLevel" with the world name, the way level scripts change level.
static void SwitchLevel(const char* world) {
    __try {
        void* mgr = *At<void*>(RVA_EVENT_MGR);
        if (!mgr || !*At<void*>(RVA_WORLD)) return;
        ((PFN_EventPost)(ExeBase() + RVA_EVENT_POST))(mgr, "SwitchLevel", world, NULL);
        Log("debug menu: SwitchLevel %s", world);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("debug menu: SwitchLevel %s faulted", world);
    }
}

// The avatar actor of player slot (0 = player 1), or NULL if there's no such player.
static BYTE* PlayerActor(int slot) {
    __try {
        BYTE* pm = *At<BYTE*>(RVA_PLAYER_MGR);
        if (!pm || slot < 0 || slot >= *(int*)(pm + 4)) return NULL;
        return *(BYTE**)(pm + 0x1a0 + slot * 0x58);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

// A player's position (actor+0x80).
static bool PlayerPos(int slot, float* xyz) {
    __try {
        BYTE* actor = PlayerActor(slot);
        if (!actor) return false;
        memcpy(xyz, actor + 0x80, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ------------------------------------------------------------------ teleport (main thread)

static bool  g_teleportOk;                   // the position-set code matches this build
static int   g_tpSlot;                       // which player the teleport rows move
static int   g_tpSpawn;                      // spawn point index for "Go to spawn point"
static float g_spot[2][3];                   // saved spots, per player
static bool  g_spotSet[2];

// Position of spawn point n: the names the game's own spawner looks up.
static bool SpawnPointPos(int n, float* xyz) {
    char name[64];
    sprintf_s(name, "AvatarNewLocation_%d|Transform", n);
    if (LocatorPos(name, xyz)) return true;
    sprintf_s(name, "Meridian_Player_%d|Transform", n);
    return LocatorPos(name, xyz);
}

// Teleports an actor the way Script_ActorSetPosition does (see RVA_XFORM_DIRTY above).
static bool MoveActor(BYTE* actor, const float* xyz) {
    if (!actor || !g_teleportOk) return false;
    __try {
        memcpy(actor + 0x80, xyz, 12);
        ((void (*)(BYTE*))(ExeBase() + RVA_XFORM_DIRTY))(actor + 0x50);
        BYTE* vt = *(BYTE**)actor;
        (*(void (**)(BYTE*, int))(vt + 0xa8))(actor, 0);
        (*(void (**)(BYTE*))(vt + 0xb8))(actor);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

enum TeleportMode { TP_TO_OTHER, TP_SAVE, TP_GO, TP_SPAWN };

// Teleport action: mode, slot, and for TP_SPAWN the spawn index. Runs on both PCs in a
// shared session, so every input comes from the simulation (positions, locators).
static void Teleport(int mode, int slot, int spawn) {
    float p[3];
    switch (mode) {
    case TP_TO_OTHER:
        if (PlayerPos(1 - slot, p)) { p[0] += 1.0f; MoveActor(PlayerActor(slot), p); }
        break;
    case TP_SAVE:
        if (slot >= 0 && slot < 2 && PlayerPos(slot, g_spot[slot])) g_spotSet[slot] = true;
        break;
    case TP_GO:
        if (slot >= 0 && slot < 2 && g_spotSet[slot]) MoveActor(PlayerActor(slot), g_spot[slot]);
        break;
    case TP_SPAWN:
        if (SpawnPointPos(spawn, p)) MoveActor(PlayerActor(slot), p);
        else if (Log) Log("debug menu: spawn point %d not found in this level", spawn);
        break;
    }
}

// ------------------------------------------------------------------ field of view (render only)

static const float kFovScales[] = { 1.0f, 1.15f, 1.3f, 1.5f, 0.85f };
static const char* kFovNames[]  = { "Game", "Wider (x1.15)", "Wide (x1.3)", "Very wide (x1.5)", "Narrow (x0.85)" };
static int g_fovIdx;                         // local: the projection is presentation only
// Applied in the view build (ViewBuildHook below): the camera node's fov (+0x238) is
// scaled only while the frame's draw view is built from it.

// ------------------------------------------------------------------ free camera (render only)
//
// Controls while it's on (the game sees a neutral pad): left stick moves, right stick
// looks, triggers go down/up, A is fast. LB + RB + Y opens the menu to turn it off.
// The camera frame (right, up, forward, position) starts from the game's camera and is
// steered with rotations about the world up axis and the camera's right axis, so it
// works whatever the engine's axis convention is; the convention is read from the
// first frame (which rows are which, and the handedness).

// The draw takes its view from the camera node itself: Renderer_SetupFramePasses builds
// each viewport's frame view in FUN_1402d8e00(view, node, a3), which refreshes the node
// (FUN_140295c00(node, 1)), copies its world (+0xd0..+0x10c) and builds the projection
// from +0x230 near, +0x234 far, +0x238 fov (FUN_1402abb10). The cull reads the render
// camera (node+0x260) instead, and the sim reads the cull's on-screen stamps, so only the
// view build may see the free camera (decomp session).
typedef void (*PFN_ViewBuild)(void* view, BYTE* node, float a3);
static PFN_ViewBuild g_viewBuildOrig;

static volatile bool g_freeCam;              // menu toggle
static bool  g_freeInit;                     // frame captured from the game's camera
static float g_fcRight[3], g_fcUp[3], g_fcFwd[3], g_fcPos[3];
static float g_fcWorldUp[3];
static float g_fcFwdSign = 1.0f;             // the node's row 2 is forward (+1) or backward (-1)
static bool  g_fcRightHanded;
static bool  g_fcFast;

// Small 3-vector helpers for the free camera.
static void V3Cross(const float* a, const float* b, float* out) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}
static float V3Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void V3Normalize(float* v) {
    float l = sqrtf(V3Dot(v, v));
    if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}
// Rotate v about the unit axis k by angle a (Rodrigues).
static void V3Rotate(float* v, const float* k, float a) {
    float c = cosf(a), s = sinf(a), kv[3];
    V3Cross(k, v, kv);
    float d = V3Dot(k, v) * (1.0f - c);
    for (int i = 0; i < 3; i++) v[i] = v[i] * c + kv[i] * s + k[i] * d;
}

// The game's camera at the top of player view 0's camera stack (position +0x190, facing +0x1a8).
static BYTE* GameCamera() {
    __try {
        BYTE* pm = *At<BYTE*>(RVA_PLAYER_MGR);
        if (!pm) return NULL;
        BYTE* list = *(BYTE**)(pm + 0xd8);
        if (!list || *(int*)list < 1) return NULL;
        BYTE* view = (*(BYTE***)(list + 8))[0];
        if (!view) return NULL;
        int n = *(int*)(view + 0x80);
        BYTE** stk = *(BYTE***)(view + 0x78);
        if (n < 1 || !stk || !stk[n - 1]) return NULL;
        return *(BYTE**)(stk[n - 1] + 0x50);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

// Starts the free camera from a camera world matrix (rows right, up, forward, position)
// and works out the engine's conventions from it: forward's sign, world up, handedness.
static void FreeCamInit(const float* world) {
    memcpy(g_fcRight, world + 0, 12);
    memcpy(g_fcUp, world + 4, 12);
    memcpy(g_fcFwd, world + 8, 12);
    memcpy(g_fcPos, world + 12, 12);
    V3Normalize(g_fcRight); V3Normalize(g_fcUp); V3Normalize(g_fcFwd);
    // Is row 2 the view direction or its opposite? Compare with the game camera's facing.
    g_fcFwdSign = 1.0f;
    if (BYTE* cam = GameCamera()) {
        __try {
            float facing[3];
            memcpy(facing, cam + 0x1a8, 12);
            if (V3Dot(facing, g_fcFwd) < 0) { g_fcFwdSign = -1.0f; for (int i = 0; i < 3; i++) g_fcFwd[i] = -g_fcFwd[i]; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    // World up: the axis the camera's up row leans on most.
    int ax = fabsf(g_fcUp[0]) > fabsf(g_fcUp[1]) ? (fabsf(g_fcUp[0]) > fabsf(g_fcUp[2]) ? 0 : 2)
                                                  : (fabsf(g_fcUp[1]) > fabsf(g_fcUp[2]) ? 1 : 2);
    g_fcWorldUp[0] = g_fcWorldUp[1] = g_fcWorldUp[2] = 0;
    g_fcWorldUp[ax] = g_fcUp[ax] < 0 ? -1.0f : 1.0f;
    float c[3];
    V3Cross(g_fcRight, g_fcUp, c);
    g_fcRightHanded = V3Dot(c, g_fcFwd) < 0;          // right x up = backward in a right-handed frame
    g_freeInit = true;
    if (Log) Log("debug menu: free camera from (%.1f %.1f %.1f), %s-handed, up axis %c",
                 g_fcPos[0], g_fcPos[1], g_fcPos[2], g_fcRightHanded ? "right" : "left", "xyz"[ax]);
}

// Once per frame tick, from the pad state (before it's neutralised for the game).
static void FreeCamTick() {
    if (!g_freeCam || !g_freeInit) return;
    const XINPUT_GAMEPAD& g = g_pad0.Gamepad;
    auto axis = [](SHORT v) { float f = v / 32767.0f; return fabsf(f) < 0.2f ? 0.0f : f; };
    const float dt = 1.0f / 60.0f;
    // Look: yaw about world up, pitch about the camera's right (stopped short of straight up/down).
    float yaw = axis(g.sThumbRX) * 2.2f * dt, pitch = axis(g.sThumbRY) * 1.6f * dt;
    if (g_fcRightHanded) yaw = -yaw; else pitch = -pitch;    // stick right turns right, stick up looks up
    if (yaw != 0) { V3Rotate(g_fcFwd, g_fcWorldUp, yaw); V3Rotate(g_fcRight, g_fcWorldUp, yaw); V3Rotate(g_fcUp, g_fcWorldUp, yaw); }
    if (pitch != 0) {
        float f[3]; memcpy(f, g_fcFwd, 12);
        V3Rotate(f, g_fcRight, pitch);
        if (fabsf(V3Dot(f, g_fcWorldUp)) < 0.98f) { memcpy(g_fcFwd, f, 12); V3Rotate(g_fcUp, g_fcRight, pitch); }
    }
    // Re-orthonormalise, keeping the frame's own handedness.
    float r[3], u[3];
    V3Cross(g_fcFwd, g_fcUp, r);
    if (V3Dot(r, g_fcRight) < 0) for (int i = 0; i < 3; i++) r[i] = -r[i];
    V3Normalize(r); memcpy(g_fcRight, r, 12);
    V3Cross(g_fcRight, g_fcFwd, u);
    if (V3Dot(u, g_fcUp) < 0) for (int i = 0; i < 3; i++) u[i] = -u[i];
    V3Normalize(u); memcpy(g_fcUp, u, 12);
    V3Normalize(g_fcFwd);
    // Move: left stick along right/forward, triggers along world up; A for 4x speed.
    g_fcFast = (g.wButtons & XINPUT_GAMEPAD_A) != 0;
    float speed = (g_fcFast ? 40.0f : 10.0f) * dt;
    float mx = axis(g.sThumbLX) * speed, mz = axis(g.sThumbLY) * speed;
    float my = ((g.bRightTrigger - g.bLeftTrigger) / 255.0f) * speed;
    for (int i = 0; i < 3; i++) g_fcPos[i] += g_fcRight[i] * mx + g_fcFwd[i] * mz + g_fcWorldUp[i] * my;
}

// Is node one of the frame builder's viewport cameras right now (main thread)?
static bool InViewportTable(BYTE* node) {
    __try {
        int n = *At<int>(RVA_VP_COUNT);
        if (n < 0 || n > 8) return false;
        for (int i = 0; i < n; i++)
            if (*At<BYTE*>(RVA_VP_NODES + i * 0x40) == node) return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

// The frame's draw view gets the free camera and the field of view; nothing else does.
// The node's world and fov are swapped in only for the original call and put back, so
// the cull, the node and the game's camera keep the game's values.
static volatile LONG g_viewBuilds, g_viewCalls;  // for the camera log: views built from our camera / all view builds
// Is this a camera node (Renderer::CameraNode, vtable 0x1407bfbf8)? The view build runs in
// a render job after the frame builder has moved on, so the viewport table (InViewportTable)
// no longer lists the node by then (run 93: never matched).
static bool IsCameraNode(BYTE* node) {
    __try { return node && *(BYTE**)node == ExeBase() + 0x7bfbf8; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool FreeFar();                       // free camera offline: far plane x kFreeFar (see CullFollows)
static const float kFreeFar = 4.0f;
// [graphics] camera_race_probe=1: the one-frame full-sun flash on camera cuts (re\SUN_FLASH.md).
// The render thread builds frame N's view from the LIVE camera node (refreshing it at the
// view build), while the main thread may already be moving that camera for frame N+1. If a
// view build catches the camera mid-update it gets a camera that never existed (say the new
// rotation with the old position): every pixel can then fall outside the shadow cascades.
// The main thread's own copy of each frame's camera is the render camera's world (written
// by the camera update, FUN_1402d86f0 -> FUN_1402e3fd0, at [[[node+0x260]+0x28]+0x18]+0x1b0);
// it's kept for the last 4 frames. Each view build of viewport 0's camera is checked against
// them: the same as the newest (normal), an older one (behind), a newer one taken later
// (ahead: it read frame N+1's camera), or none at all (torn), and how far its rotation rows
// are from orthonormal. Ahead and torn builds are logged with the time, to line up with a flash.
static bool g_camProbe;
struct CamSnap { float m[16]; uint32_t seq; };
static CamSnap  g_camSnaps[4];
static uint32_t g_camSeq;
static SRWLOCK  g_camProbeLock = SRWLOCK_INIT;
struct CamPending { float m[16]; uint32_t seq; float ortho; ULONGLONG t; bool used; };   // seq: newest snapshot when built
static CamPending g_camPending[8];
static LONG  g_cpBuilds, g_cpNewest, g_cpBehind, g_cpAhead, g_cpTorn, g_cpLines;
static float g_cpMaxOrtho;

// The same camera matrix, within float noise.
static bool CamSame(const float* a, const float* b) {
    for (int i = 0; i < 16; i++) {
        float tol = i >= 12 ? 0.01f : 1e-4f;             // position row in metres, rotation unitless
        if (fabsf(a[i] - b[i]) > tol) return false;
    }
    return true;
}
static float CamOrtho(const float* m) {                  // largest error of the 3x3 rows from orthonormal
    float e = 0;
    for (int r = 0; r < 3; r++) {
        const float* a = m + r * 4;
        e = fmaxf(e, fabsf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] - 1.0f));
        for (int s = r + 1; s < 3; s++) {
            const float* b = m + s * 4;
            e = fmaxf(e, fabsf(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]));
        }
    }
    return e;
}
// One probe log line (at most 300 per run).
static void CamProbeLog(const char* what, const float* m, float ortho, ULONGLONG t, const char* extra) {
    if (InterlockedIncrement(&g_cpLines) > 300) return;
    Log("debug menu: camera race probe: %s at %llu ms: view at (%.2f %.2f %.2f), forward (%.3f %.3f %.3f), orthonormal error %.5f%s",
        what, t, m[12], m[13], m[14], m[8], m[9], m[10], ortho, extra);
}
// Main thread, after viewport 0's camera update: keep the render camera's world.
static void CamProbeSnapshot(BYTE* node) {
    float m[16];
    __try {
        BYTE* rc = *(BYTE**)(node + 0x260);
        BYTE* target = rc ? *(BYTE**)(rc + 0x28) : NULL;
        BYTE* inner = target ? *(BYTE**)(target + 0x18) : NULL;
        if (!inner) return;
        memcpy(m, inner + 0x1b0, sizeof m);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    AcquireSRWLockExclusive(&g_camProbeLock);
    uint32_t seq = ++g_camSeq;
    CamSnap& s = g_camSnaps[seq % 4];
    memcpy(s.m, m, sizeof m);
    s.seq = seq;
    // Builds that matched nothing yet: did they read this newer camera (ahead), or nothing (torn)?
    for (CamPending& p : g_camPending) {
        if (!p.used || p.seq + 1 != seq) continue;
        p.used = false;
        if (CamSame(p.m, m)) { InterlockedIncrement(&g_cpAhead); CamProbeLog("view built AHEAD (the next frame's camera)", p.m, p.ortho, p.t, ""); }
        else { InterlockedIncrement(&g_cpTorn); CamProbeLog("view built from a camera that never existed (TORN)", p.m, p.ortho, p.t, ""); }
    }
    for (CamPending& p : g_camPending) if (p.used && seq - p.seq > 1) p.used = false;
    ReleaseSRWLockExclusive(&g_camProbeLock);
}
// Render thread, at viewport 0's view build: what the view was built from, after the refresh.
static void CamProbeBuild(const float* w) {
    float ortho = CamOrtho(w);
    InterlockedIncrement(&g_cpBuilds);
    AcquireSRWLockExclusive(&g_camProbeLock);
    static bool first = true;                            // the two copies should share a layout: show both once
    if (first && g_camSeq) {
        first = false;
        const float* s = g_camSnaps[g_camSeq % 4].m;
        Log("debug menu: camera race probe: first view build: node world pos (%.2f %.2f %.2f) fwd (%.3f %.3f %.3f); "
            "main thread's render camera pos (%.2f %.2f %.2f) fwd (%.3f %.3f %.3f)",
            w[12], w[13], w[14], w[8], w[9], w[10], s[12], s[13], s[14], s[8], s[9], s[10]);
    }
    if (ortho > g_cpMaxOrtho) g_cpMaxOrtho = ortho;
    uint32_t seq = g_camSeq;
    int age = -1;
    for (int k = 0; k < 4 && seq >= (uint32_t)k + 1; k++)
        if (g_camSnaps[(seq - k) % 4].seq == seq - k && CamSame(w, g_camSnaps[(seq - k) % 4].m)) { age = k; break; }
    if (age == 0) InterlockedIncrement(&g_cpNewest);
    else if (age > 0) InterlockedIncrement(&g_cpBehind);
    else {
        int slot = 0;
        for (int i = 0; i < (int)_countof(g_camPending); i++) if (!g_camPending[i].used) { slot = i; break; }
        CamPending& p = g_camPending[slot];
        memcpy(p.m, w, sizeof p.m);
        p.seq = seq; p.ortho = ortho; p.t = GetTickCount64(); p.used = true;
    }
    ReleaseSRWLockExclusive(&g_camProbeLock);
    if (ortho > 1e-3f) CamProbeLog("view built from a SKEWED camera", w, ortho, GetTickCount64(), "");
    if (g_cpBuilds % 600 == 0) {
        Log("debug menu: camera race probe: last 600 view builds: %ld from the newest camera, %ld older, %ld ahead, %ld torn; "
            "largest orthonormal error %.5f", InterlockedExchange(&g_cpNewest, 0), InterlockedExchange(&g_cpBehind, 0),
            InterlockedExchange(&g_cpAhead, 0), InterlockedExchange(&g_cpTorn, 0), g_cpMaxOrtho);
        g_cpMaxOrtho = 0;
    }
}
// Viewport 0's camera node, as the main thread's viewport table lists it at the camera update.
// (By the render thread's view build the table may have moved on: compare with this instead.)
static BYTE* volatile g_camProbeNode;
static bool IsViewport0(BYTE* node) {
    __try { return *At<int>(RVA_VP_COUNT) > 0 && *At<BYTE*>(RVA_VP_NODES) == node; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Detour of FUN_1402d8e00, the view build (render thread). See the comments above: the
// probe, then the free camera and field of view swapped in for the original call only.
static void ViewBuildHook(void* view, BYTE* node, float a3) {
    // Camera race probe (viewport 0, free camera off): what this build is about to see.
    if (g_camProbe && !g_freeCam && node && node == g_camProbeNode) {
        float w[16];
        bool ok = false;
        __try {
            ((void (*)(BYTE*, int))(ExeBase() + 0x295c00))(node, 1);   // the refresh the original does first
            memcpy(w, node + 0xd0, sizeof w);
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (ok) CamProbeBuild(w);
    }
    // Nothing to change: the game's view build as it is.
    if (!g_freeCam && !g_fovIdx) { g_viewBuildOrig(view, node, a3); return; }
    InterlockedIncrement(&g_viewCalls);
    if (!IsCameraNode(node)) {
        static ULONGLONG nextMiss;
        ULONGLONG now = GetTickCount64();
        if (Log && now >= nextMiss) {
            nextMiss = now + 2000;
            uint64_t vt = 0;
            __try { vt = node ? *(uint64_t*)node - (uint64_t)ExeBase() + 0x140000000ULL : 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            Log("debug menu: camera: a view was built from a node that isn't a camera node (vtable %llx); left alone",
                (unsigned long long)vt);
        }
        g_viewBuildOrig(view, node, a3);
        return;
    }
    // Save the node's world, fov and far plane; swap in the free camera, far x kFreeFar
    // (offline) and the scaled fov (capped at 170 degrees).
    float world[16], fov = 0, farZ = 0;
    bool swapped = false;
    __try {
        ((void (*)(BYTE*, int))(ExeBase() + 0x295c00))(node, 1);     // refresh now, so the original won't redo it
        memcpy(world, node + 0xd0, sizeof world);
        fov = *(float*)(node + 0x238);
        farZ = *(float*)(node + 0x234);
        if (FreeFar() && farZ > 0.0f) *(float*)(node + 0x234) = farZ * kFreeFar;
        if (g_freeCam) {
            if (!g_freeInit) FreeCamInit(world);
            float m[16] = {};
            memcpy(m + 0, g_fcRight, 12);
            memcpy(m + 4, g_fcUp, 12);
            for (int i = 0; i < 3; i++) m[8 + i] = g_fcFwd[i] * g_fcFwdSign;
            memcpy(m + 12, g_fcPos, 12);
            m[15] = 1.0f;
            memcpy(node + 0xd0, m, sizeof m);
        }
        float k = kFovScales[g_fovIdx];
        if (k != 1.0f && fov > 1.0f && fov < 170.0f) *(float*)(node + 0x238) = fov * k > 170.0f ? 170.0f : fov * k;
        swapped = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    // Build the view from them, then put the game's values back.
    g_viewBuildOrig(view, node, a3);
    if (!swapped) return;
    __try {
        memcpy(node + 0xd0, world, sizeof world);
        *(float*)(node + 0x238) = fov;
        *(float*)(node + 0x234) = farZ;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    InterlockedIncrement(&g_viewBuilds);
    // Once a second: where the free camera is, the game camera's fov and what the view got.
    static ULONGLONG next;
    ULONGLONG now = GetTickCount64();
    if (Log && now >= next) {
        next = now + 1000;
        float k = kFovScales[g_fovIdx];
        Log("debug menu: camera: free %s at (%.1f %.1f %.1f), game camera at (%.1f %.1f %.1f); fov %.1f -> %.1f; "
            "%ld of %ld views built with it", g_freeCam ? "on" : "off", g_fcPos[0], g_fcPos[1], g_fcPos[2], world[12], world[13],
            world[14], fov, k != 1.0f && fov > 1.0f && fov < 170.0f ? (fov * k > 170.0f ? 170.0f : fov * k) : fov,
            InterlockedExchange(&g_viewBuilds, 0), InterlockedExchange(&g_viewCalls, 0));
    }
}

// Culling. The cull reads the render camera (node+0x260 -> +0x28), not the draw's view, so
// with only the view swapped, things the free camera sees outside the game camera's view
// (or beyond its fov) aren't drawn. Offline, the render camera follows the free camera and
// its projection gets the same fov scale. Online (g_netplay) it doesn't: the cull writes the
// on-screen stamps the simulation reads, so moving it on one PC could desync the session;
// the free camera then shows what the game camera's cull allowed.
typedef void (*PFN_CamUpdate)(BYTE* node, float aspect);
typedef void (*PFN_CamSetWorld)(BYTE* renderCam, const float* m16);
typedef void (*PFN_CamNodeRefresh)(BYTE* node, uint64_t a2);
typedef uint32_t* (*PFN_ProjSetup)(uint32_t* out, float nearZ, float farZ, float fovDeg, float aspect);
static PFN_CamUpdate g_camUpdateOrig;
static PFN_CamNodeRefresh g_camRefreshOrig;
static PFN_ProjSetup g_projSetupOrig;
static bool CullFollows() { return !g_netplay; }
// Offline the free camera also draws everything it looks at: the proxy turns the small-object
// culls and fade-outs off while it's on (Gfx_FreeCamCulling), and its far plane is kFreeFar
// times the game's, in the draw's view and in the cull's projection alike.
void Gfx_FreeCamCulling(bool off);
static bool FreeFar() { return g_freeCam && CullFollows(); }

// The free camera into a viewport node's render camera (FUN_1402e3fd0 copies the 16 floats
// to [target+0x18]+0x1b0 and rebuilds its view). The node's own world is left alone.
static void ApplyCullCam(BYTE* node) {
    __try {
        if (!g_freeInit) FreeCamInit((const float*)(node + 0xd0));
        float m[16] = {};
        memcpy(m + 0, g_fcRight, 12);
        memcpy(m + 4, g_fcUp, 12);
        for (int i = 0; i < 3; i++) m[8 + i] = g_fcFwd[i] * g_fcFwdSign;
        memcpy(m + 12, g_fcPos, 12);
        m[15] = 1.0f;
        BYTE* rc = *(BYTE**)(node + 0x260);
        BYTE* target = rc ? *(BYTE**)(rc + 0x28) : NULL;
        if (target) ((PFN_CamSetWorld)(ExeBase() + RVA_CAM_SETWORLD))(target, m);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
// After the frame builder's per-viewport camera update (FUN_1402d86f0)...
static void CamUpdateHook(BYTE* node, float aspect) {
    g_camUpdateOrig(node, aspect);
    if (g_camProbe && !g_freeCam && IsViewport0(node)) { g_camProbeNode = node; CamProbeSnapshot(node); }
    if (g_freeCam && CullFollows() && InViewportTable(node)) ApplyCullCam(node);
}
// ... and after any later refresh of the node (Renderer::CameraNode::vf14), which rebuilds
// the render camera from the node's world and would put the game's camera back.
static void CamRefreshHook(BYTE* node, uint64_t a2) {
    g_camRefreshOrig(node, a2);
    if (g_freeCam && CullFollows() && g_freeInit && InViewportTable(node)) ApplyCullCam(node);
}
// The render camera's projection (FUN_1402e43c0 from CameraNode::vf14): the same fov scale as
// the draw, so a wider view isn't culled at the game's edges. Perspective game cameras only.
static uint32_t* ProjSetupHook(uint32_t* out, float nearZ, float farZ, float fovDeg, float aspect) {
    float k = kFovScales[g_fovIdx];
    if (k != 1.0f && CullFollows() && fovDeg > 1.0f && fovDeg < 170.0f) fovDeg = fovDeg * k > 170.0f ? 170.0f : fovDeg * k;
    if (FreeFar() && fovDeg > 1.0f && fovDeg < 170.0f) farZ *= kFreeFar;
    return g_projSetupOrig(out, nearZ, farZ, fovDeg, aspect);
}

// Installs the camera detours: the view build (free camera, field of view) and, for the
// offline cull, the camera update, the node refresh and the projection setup. Each is
// checked against its prologue; whatever doesn't match is left out and logged.
static void HookCamera() {
    // FUN_1402d8e00: mov rax,rsp; movss [rax+18h],xmm2; push rbp; push rbx; push rsi; push rdi; lea rbp,[rax-2b8h]
    static const BYTE view[] = { 0x48, 0x8B, 0xC4, 0xF3, 0x0F, 0x11, 0x50, 0x18, 0x55, 0x53, 0x56, 0x57,
                                 0x48, 0x8D, 0xA8, 0x48, 0xFD, 0xFF, 0xFF };
    // ... and its call to the node refresh FUN_140295c00 at +0x64, which the hook calls first.
    if (!CallsTo(0x2d8e64, 0x295c00)) { Log("debug menu: view build code doesn't match this build; no free camera or field of view"); return; }
    g_viewBuildOrig = (PFN_ViewBuild)Detour(0x2d8e00, view, sizeof view, (void*)ViewBuildHook);
    if (!g_viewBuildOrig) { Log("debug menu: view build code doesn't match this build; no free camera or field of view"); return; }
    // The cull side (see CullFollows). FUN_1402d86f0: push rbx (REX 40 53); sub rsp,0x1d0; movaps [rsp+0x1c0],xmm6
    static const BYTE upd[] = { 0x40, 0x53, 0x48, 0x81, 0xEC, 0xD0, 0x01, 0x00, 0x00, 0x0F, 0x29, 0xB4, 0x24, 0xC0, 0x01, 0x00, 0x00 };
    // FUN_1402e3fd0: mov eax,[rdx]; mov rcx,[rcx+0x18]; mov [rcx+0x1b0],eax
    static const BYTE setWorld[] = { 0x8B, 0x02, 0x48, 0x8B, 0x49, 0x18, 0x89, 0x81, 0xB0, 0x01, 0x00, 0x00 };
    // Renderer::CameraNode::vf14 (0x1402b7ea0): mov [rsp+8],rbx; push rdi; sub rsp,0x220; mov r8b,1
    static const BYTE refresh[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x81, 0xEC, 0x20, 0x02, 0x00, 0x00, 0x41, 0xB0, 0x01 };
    // FUN_1402e43c0: mov rax,rsp; mov [rax+8],rbx; push rdi; sub rsp,90h
    static const BYTE proj[] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x57, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00 };
    if (BytesAt(RVA_CAM_SETWORLD, setWorld, sizeof setWorld)) {
        g_camUpdateOrig = (PFN_CamUpdate)Detour(RVA_CAM_UPDATE, upd, sizeof upd, (void*)CamUpdateHook);
        if (g_camUpdateOrig) g_camRefreshOrig = (PFN_CamNodeRefresh)Detour(0x2b7ea0, refresh, sizeof refresh, (void*)CamRefreshHook);
    }
    g_projSetupOrig = (PFN_ProjSetup)Detour(RVA_PROJ_SETUP, proj, sizeof proj, (void*)ProjSetupHook);
    Log("debug menu: free camera culling follows offline: camera update %s, node refresh %s, projection %s",
        g_camUpdateOrig ? "yes" : "NO", g_camRefreshOrig ? "yes" : "NO", g_projSetupOrig ? "yes" : "NO");
}

// The game code MoveActor copies matches this build.
static bool VerifyTeleport() {
    // FUN_14056dab0: ...; mov rdi,[rcx+0xc0]
    static const BYTE dirty[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xB9, 0xC0, 0x00, 0x00, 0x00 };
    // Script_ActorSetPosition: push rbx (REX 40 53); sub rsp,0x30; lea rdx,[rsp+0x20]
    static const BYTE setPos[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8D, 0x54, 0x24, 0x20 };
    // Script_ActorSetPosition: arg fetch (+0x16), the three stores, the dirty call (+0x48), vf 0xa8 and 0xb8.
    return BytesAt(RVA_XFORM_DIRTY, dirty, sizeof dirty) && BytesAt(RVA_ACTOR_SETPOS, setPos, sizeof setPos) &&
           CallsTo(RVA_ACTOR_SETPOS + 0x16, 0x544740) && CallsTo(RVA_ACTOR_SETPOS + 0x48, RVA_XFORM_DIRTY);
}

// ------------------------------------------------------------------ rows

// Appends a blank row (white value); NULL when the menu is full.
static Row* AddRow(Snapshot& s, RowKind kind, RowId id, const char* label) {
    if (s.count >= kMaxRows) return NULL;
    Row* r = &s.rows[s.count++];
    memset(r, 0, sizeof *r);
    r->kind = kind;
    r->id = id;
    r->valueColor = kWhite;
    strncpy_s(r->label, label, _TRUNCATE);
    return r;
}

// ON in green or OFF in grey.
static void OnOff(Row* r, bool on) {
    strcpy_s(r->value, on ? "ON" : "OFF");
    r->valueColor = on ? kGreen : kGrey;
}

// Builds the whole menu from the game's current state, section by section; a section
// whose code didn't match this build is left out. lock: online and this PC may not act,
// so every row that changes the simulation is greyed out.
static void BuildRows(Snapshot& s) {
    s.count = 0;
    bool lock = g_netplay && !(g_shared && g_canAct);
    AddRow(s, ROW_HEADER, ID_NONE, "CHEATS");
    if (!g_gameOk) {
        Row* r = AddRow(s, ROW_INFO, ID_NONE, "Not available in this build");
        r->disabled = true;
    } else {
        int n = CheatCount();
        for (int i = 0; i < n; i++) {
            char name[64];
            if (!CheatName(i, name, sizeof name)) continue;
            // God and Wireframe do nothing in this build, Say Stress is a dialogue
            // test, and the time cheats are the Game speed row below. Unlock levels
            // is forced on at every level load on PC (the cheat reset FUN_14049be30
            // sets it whenever DAT_140ac3fd0 is null, and nothing assigns it) and
            // nothing reads it, so a toggle would only mislead.
            static const char* skip[] = { "God", "Wire", "Say", "Time", "Stop", "Unlock" };
            bool hidden = false;
            for (const char* p : skip) hidden |= _strnicmp(name, p, strlen(p)) == 0;
            if (hidden) continue;
            Row* r = AddRow(s, ROW_TOGGLE, ID_CHEAT, name);
            if (!r) break;
            r->arg = i;
            OnOff(r, CheatOn(i));
            r->disabled = lock;
        }
        if (!n) AddRow(s, ROW_INFO, ID_NONE, "Load a level to use cheats")->disabled = true;
        Row* g = AddRow(s, ROW_TOGGLE, ID_GOD, "God mode");
        OnOff(g, g_god);
        g->disabled = lock;
        Row* sp = AddRow(s, ROW_CHOICE, ID_SPEED, "Game speed");
        int si = SpeedIndex();
        sprintf_s(sp->value, "< %s >", kSpeedNames[si]);
        sp->valueColor = si == 0 ? kWhite : kGold;
        sp->disabled = lock;
        if (g_netplay && g_shared && g_canAct) AddRow(s, ROW_INFO, ID_NONE, "Changes apply to both players")->valueColor = kBlue;
        else if (g_netplay && g_shared) AddRow(s, ROW_INFO, ID_NONE, "Player 1 controls these")->valueColor = kBlue;
        else if (lock) AddRow(s, ROW_INFO, ID_NONE, "Locked in online co-op")->valueColor = kRed;
    }
    if (g_statsOk) {
        int coins = StatValue("Stat_GOLD_COLLECTED");
        if (coins >= 0) {
            AddRow(s, ROW_HEADER, ID_NONE, "PROGRESS");
            Row* r = AddRow(s, ROW_ACTION, ID_COINS, "Give 1000 coins");
            sprintf_s(r->value, "%d", coins);
            r->valueColor = kGold;
            r->disabled = lock;
        }
    }
    if (g_locksOk && *At<BYTE*>(RVA_LOCKS)) {
        if (!g_statsOk) AddRow(s, ROW_HEADER, ID_NONE, "PROGRESS");
        Row* u = AddRow(s, ROW_ACTION, ID_UNLOCKALL, "Unlock everything (shop, items)");
        if (g_unlockedCount >= 0) sprintf_s(u->value, "%d unlocked", g_unlockedCount);
        u->valueColor = kGreen;
        u->disabled = lock;
        if (g_testSave && g_hundredOk && !g_netplay) {
            Row* h = AddRow(s, ROW_ACTION, ID_HUNDRED, "Make this test save 100%");
            strcpy_s(h->value, g_hundredResult[0] ? g_hundredResult : "on the next level load");
            h->valueColor = kGold;
        }
        Row* p = AddRow(s, ROW_CHOICE, ID_LOCKPICK, "Lock");
        sprintf_s(p->value, "< %s >", kLockPicks[g_lockPick]);
        p->disabled = lock;
        int st = LockState(kLockPicks[g_lockPick]);
        Row* v = AddRow(s, ROW_CHOICE, ID_LOCKSTATE, "  State");
        if (st >= 0 && st <= 6) {
            sprintf_s(v->value, "< %s >", kLockStates[st]);
            v->valueColor = st ? kGreen : kGrey;
        } else {
            strcpy_s(v->value, "not in this save");
            v->valueColor = kGrey;
        }
        v->disabled = lock || st < 0;
        for (int i = 0; i < kLockPresetCount; i++) {
            int pst = LockState(kLockPresets[i].lock);
            if (pst < 0) continue;                       // not in this save's lock list
            Row* r = AddRow(s, ROW_ACTION, ID_PRESET, kLockPresets[i].label);
            r->arg = i;
            strcpy_s(r->value, pst <= 6 ? kLockStates[pst] : "?");
            r->valueColor = pst >= 5 ? kGreen : kGrey;
            r->disabled = lock || PresetTarget(i, pst) < 0;
        }
    }
    bool debugOpts = g_frontendOrig || g_frontendViaProxy;
    if (g_levelOk || debugOpts) {
        AddRow(s, ROW_HEADER, ID_NONE, "LEVELS");
        bool title = OnTitleScreen();
        if (g_levelOk) {
            Row* l = AddRow(s, ROW_CHOICE, ID_LEVEL, "Level");
            sprintf_s(l->value, "< %s >", kLevels[g_level].label);
            l->disabled = lock || title;
            Row* go = AddRow(s, ROW_ACTION, ID_LOAD, title ? "Load level (start a game first)" : "Load level");
            go->disabled = lock || title;
        }
        if (debugOpts) {
            Row* d = AddRow(s, ROW_ACTION, ID_DEBUGOPTS, title ? "Developer Debug Options" : "Developer Debug Options (title screen)");
            d->disabled = g_netplay || !title;
        }
        if (g_netplay && g_levelOk && !g_levelHooksOk) {
            Row* n = AddRow(s, ROW_INFO, ID_NONE, "Hidden levels need the level hooks");
            n->disabled = true;
        }
    }
    if (g_teleportOk && PlayerActor(0)) {
        AddRow(s, ROW_HEADER, ID_NONE, "TELEPORT");
        bool two = PlayerActor(1) != NULL;
        if (!two) g_tpSlot = 0;
        Row* who = AddRow(s, ROW_CHOICE, ID_TPSLOT, "Move");
        sprintf_s(who->value, "< Player %d >", g_tpSlot + 1);
        who->disabled = lock || !two;
        Row* o = AddRow(s, ROW_ACTION, ID_TP_OTHER, "Teleport to the other player");
        o->disabled = lock || !two;
        Row* sv = AddRow(s, ROW_ACTION, ID_TP_SAVE, "Save this spot");
        sv->disabled = lock;
        Row* go = AddRow(s, ROW_ACTION, ID_TP_GO, "Go to the saved spot");
        if (g_spotSet[g_tpSlot]) sprintf_s(go->value, "%.0f  %.0f  %.0f", g_spot[g_tpSlot][0], g_spot[g_tpSlot][1], g_spot[g_tpSlot][2]);
        else strcpy_s(go->value, "none saved");
        go->disabled = lock || !g_spotSet[g_tpSlot];
        if (g_levelHooksOk) {                        // LocatorPos's addresses were verified with the level hooks
            float at[3];
            bool have = SpawnPointPos(g_tpSpawn, at);
            Row* sp = AddRow(s, ROW_CHOICE, ID_TP_SPAWN, "Spawn point");
            sprintf_s(sp->value, "< %d >", g_tpSpawn);
            sp->disabled = lock;
            Row* sg = AddRow(s, ROW_ACTION, ID_TP_SPAWNGO, "Go to the spawn point");
            if (have) sprintf_s(sg->value, "%.0f  %.0f  %.0f", at[0], at[1], at[2]);
            else strcpy_s(sg->value, "not in this level");
            sg->disabled = lock || !have;
        }
        if (g_netplay && g_shared && g_canAct) AddRow(s, ROW_INFO, ID_NONE, "Moves happen on both PCs")->valueColor = kBlue;
    }
    AddRow(s, ROW_HEADER, ID_NONE, "GRAPHICS");
    int smaa = GfxExt_GetSmaa();
    if (Row* fx = AddRow(s, ROW_TOGGLE, ID_FXAA, "Anti-aliasing (FXAA)")) {
        OnOff(fx, GfxExt_GetFxaa());
        if (smaa > 0 && GfxExt_GetFxaa()) { strcpy_s(fx->value, "ON (SMAA used instead)"); fx->valueColor = kGrey; }
    }
    if (smaa >= 0) {                                 // render-only: this PC
        static const char* kSmaaNames[] = { "Off", "Low", "Medium", "High", "Ultra" };
        if (Row* sm = AddRow(s, ROW_CHOICE, ID_SMAA, "Anti-aliasing (SMAA)")) {
            sprintf_s(sm->value, "< %s >", kSmaaNames[smaa]);
            sm->valueColor = smaa ? kGold : kWhite;
        }
    } else if (Row* n = AddRow(s, ROW_INFO, ID_NONE, "SMAA: set smaa=1 in the ini and restart")) {
        n->disabled = true;
    }
    if (g_viewBuildOrig) {
        Row* f = AddRow(s, ROW_CHOICE, ID_FOV, "Field of view");
        sprintf_s(f->value, "< %s >", kFovNames[g_fovIdx]);
        f->valueColor = g_fovIdx ? kGold : kWhite;
    }
    if (g_viewBuildOrig) {
        Row* c = AddRow(s, ROW_TOGGLE, ID_FREECAM, "Free camera (closes the menu)");
        OnOff(c, g_freeCam);
        c->disabled = !PlayerActor(0);
        Row* h = AddRow(s, ROW_INFO, ID_NONE, g_freeCam ? "LS move, RS look, LT/RT down/up, A fast"
                                              : g_netplay ? "Online: view only; far things may not draw (keeps both PCs in step)"
                                                          : "Look around freely; the game keeps running");
        h->disabled = true;
        if (g_freeCam && g_freeInit) {
            Row* p = AddRow(s, ROW_INFO, ID_NONE, "Camera");
            sprintf_s(p->value, "%.1f  %.1f  %.1f", g_fcPos[0], g_fcPos[1], g_fcPos[2]);
        }
    }
    AddRow(s, ROW_HEADER, ID_NONE, "INFO");
    Row* f = AddRow(s, ROW_INFO, ID_NONE, "Frame");
    sprintf_s(f->value, "%d", *At<int>(RVA_GAME_FRAME));
    for (int slot = 0; slot < 2; slot++) {
        float p[3];
        if (!PlayerPos(slot, p)) continue;
        char label[32];
        sprintf_s(label, "Player %d", slot + 1);
        Row* r = AddRow(s, ROW_INFO, ID_NONE, label);
        sprintf_s(r->value, "%.1f  %.1f  %.1f", p[0], p[1], p[2]);
    }
    if (g_gameOk) {
        Row* r = AddRow(s, ROW_INFO, ID_NONE, "Cheat target (Stealth)");
        uint64_t t = *At<uint64_t>(RVA_CHEAT_TARGET);
        if (t) sprintf_s(r->value, "%llx", (unsigned long long)t);
        else strcpy_s(r->value, "none");
    }
    if (g_netplay) {
        Row* r = AddRow(s, ROW_INFO, ID_NONE, "Online co-op");
        strcpy_s(r->value, "active");
        r->valueColor = kBlue;
    }
    AddRow(s, ROW_ACTION, ID_CLOSE, "Close");
}

// The rows the selection can land on.
static bool Selectable(const Row& r) { return r.kind != ROW_HEADER && r.kind != ROW_INFO && !r.disabled; }

// Moves the selection to the next selectable row in dir (+1 down, -1 up), wrapping around.
static void MoveSel(Snapshot& s, int dir) {
    if (!s.count) return;
    int i = g_sel;
    for (int step = 0; step < s.count; step++) {
        i = (i + dir + s.count) % s.count;
        if (Selectable(s.rows[i])) { g_sel = i; return; }
    }
}

// ------------------------------------------------------------------ actions (shared in online co-op)
//
// Everything the menu does to the game is a MenuAction. Solo it's performed at once.
// In an online session with shared actions enabled, it goes to an outbox instead; the
// proxy sends it through lockstep and both PCs perform it on the same frame
// (DebugMenu_PollAction / DebugMenu_ApplyAction). Table indices (cheats, levels, lock
// picks) are the same on both PCs: same build, same level.

enum ActionId : uint8_t { ACT_CHEAT = 1, ACT_GOD, ACT_SPEED, ACT_GIVE_COINS, ACT_UNLOCK_ALL,
                          ACT_LOCK_STATE, ACT_LOAD_LEVEL, ACT_TELEPORT, ACT_HUNDRED };
#pragma pack(push, 1)
struct MenuAction { uint8_t id, pad[3]; int32_t a, b; };     // 12 bytes on the wire; a, b: see Perform
#pragma pack(pop)

// Actions waiting to be sent (a ring of 8); DebugMenu_PollAction hands them out.
static MenuAction g_outbox[8];
static int g_outHead, g_outCount;

// Does one action to the game. Alone it's called straight from Submit; in a session from
// DebugMenu_ApplyAction, on both PCs on the same frame. Arguments are range-checked, since
// they may come from the other PC.
static void Perform(const MenuAction& m) {
    switch (m.id) {
    case ACT_CHEAT: if (m.a >= 0 && m.a < CheatCount()) ToggleCheat(m.a); break;
    case ACT_GOD: g_god = m.a != 0; break;
    case ACT_SPEED: if (m.a >= 0 && m.a < 6) SetSpeed(m.a); break;
    case ACT_GIVE_COINS: GiveStat("Stat_GOLD_COLLECTED", m.a); break;
    case ACT_UNLOCK_ALL: g_unlockedCount = UnlockEverything(); break;
    case ACT_LOCK_STATE:
        if (m.a >= 0 && m.a < kLockPickCount && m.b >= 0 && m.b <= 6) {
            if (BYTE* lock = FindLock(kLockPicks[m.a])) {
                SetLockState(lock, m.b);
                if (Log) Log("debug menu: lock %s -> %s", kLockPicks[m.a], kLockStates[m.b]);
            }
        }
        break;
    case ACT_LOAD_LEVEL:
        if (m.a >= 0 && m.a < kLevelCount && LevelAvailable(kLevels[m.a])) {
            const Level* lv = &kLevels[m.a];
            g_menuLevel = (lv->files || lv->spawnAt) ? lv : NULL;
            SwitchLevel(lv->row);
        }
        break;
    case ACT_TELEPORT: Teleport(m.a, m.b & 0xff, m.b >> 8); break;    // b: slot | spawn << 8
    case ACT_HUNDRED:                                                 // never shared: test save only, alone
        if (!g_netplay && g_testSave && g_hundredOk) {
            g_hundredArmed = !g_hundredArmed;                         // a second press disarms
            strcpy_s(g_hundredResult, g_hundredArmed ? "armed: leave or load a level" : "");
            if (Log) Log("debug menu: test save 100%% %s", g_hundredArmed ? "armed for the next level load" : "disarmed");
        }
        break;
    default: break;
    }
}

// Performs an action now (alone), or queues it for both PCs (in a session, if this PC may
// act); otherwise it's dropped.
static void Submit(uint8_t id, int a = 0, int b = 0) {
    MenuAction m = { id, { 0, 0, 0 }, a, b };
    if (!g_netplay) {
        Perform(m);
    } else if (g_shared && g_canAct && g_outCount < 8) {
        g_outbox[(g_outHead + g_outCount++) % 8] = m;
        if (Log) Log("debug menu: shared action %d (%d, %d) queued for both players", id, a, b);
    }
}

// What a row does. dir: 0 for A, -1 / +1 for Left / Right. Anything that changes the
// simulation goes through Submit, except the solo-only paths (a level load alone, Debug
// Options); what only this PC sees (graphics, field of view, free camera, which lock,
// level, player or spawn point the rows show) changes here directly.
static void Activate(const Row& r, int dir) {
    switch (r.id) {
    case ID_CHEAT: Submit(ACT_CHEAT, r.arg); break;
    case ID_GOD:   Submit(ACT_GOD, !g_god); break;
    case ID_SPEED: Submit(ACT_SPEED, (SpeedIndex() + (dir ? dir : 1) + 6) % 6); break;
    case ID_COINS: Submit(ACT_GIVE_COINS, 1000); break;
    case ID_UNLOCKALL: Submit(ACT_UNLOCK_ALL); break;
    case ID_HUNDRED: if (!g_netplay) Submit(ACT_HUNDRED); break;
    case ID_LOCKPICK: g_lockPick = (g_lockPick + (dir ? dir : 1) + kLockPickCount) % kLockPickCount; break;
    case ID_PRESET:
        if (r.arg >= 0 && r.arg < kLockPresetCount) {
            int pick = LockPickIndex(kLockPresets[r.arg].lock);
            int to = PresetTarget(r.arg, LockState(kLockPresets[r.arg].lock));
            if (pick >= 0 && to >= 0) Submit(ACT_LOCK_STATE, pick, to);
        }
        break;
    case ID_LOCKSTATE: {
        int st = LockState(kLockPicks[g_lockPick]);
        if (st >= 0) Submit(ACT_LOCK_STATE, g_lockPick, (st + (dir ? dir : 1) + 7) % 7);
        break;
    }
    case ID_FXAA:  GfxExt_SetFxaa(!GfxExt_GetFxaa()); break;
    case ID_SMAA: {
        int lv = GfxExt_GetSmaa();
        if (lv >= 0) GfxExt_SetSmaa((lv + (dir ? dir : 1) + 5) % 5);
        break;
    }
    case ID_FOV: {
        const int n = sizeof kFovScales / sizeof kFovScales[0];
        g_fovIdx = (g_fovIdx + (dir ? dir : 1) + n) % n;
        break;
    }
    case ID_FREECAM:
        g_freeCam = !g_freeCam;
        g_freeInit = false;                      // start again from the game's camera
        if (g_freeCam) g_open = false;           // hand the pad to the camera
        if (Log) Log("debug menu: free camera %s", g_freeCam ? "on" : "off");
        break;
    case ID_TPSLOT:   g_tpSlot = 1 - g_tpSlot; break;
    case ID_TP_OTHER: Submit(ACT_TELEPORT, TP_TO_OTHER, g_tpSlot); break;
    case ID_TP_SAVE:  Submit(ACT_TELEPORT, TP_SAVE, g_tpSlot); break;
    case ID_TP_GO:    Submit(ACT_TELEPORT, TP_GO, g_tpSlot); break;
    case ID_TP_SPAWN: g_tpSpawn = (g_tpSpawn + (dir ? dir : 1) + 8) % 8; break;
    case ID_TP_SPAWNGO: Submit(ACT_TELEPORT, TP_SPAWN, g_tpSlot | (g_tpSpawn << 8)); break;
    case ID_LEVEL:
        for (int step = 0; step < kLevelCount; step++) {         // skip worlds this build can't load
            g_level = (g_level + (dir ? dir : 1) + kLevelCount) % kLevelCount;
            if (LevelAvailable(kLevels[g_level])) break;
        }
        break;
    case ID_LOAD:
        if (g_netplay) Submit(ACT_LOAD_LEVEL, g_level);
        else g_switchTo = &kLevels[g_level];                     // solo: post at the end of this tick
        g_open = false;
        break;
    case ID_DEBUGOPTS: g_wantDebugOptions = true; g_open = false; break;
    case ID_CLOSE: g_open = false; break;
    default: break;
    }
}

// ------------------------------------------------------------------ input (main thread, from the pad poll)

// The key is held and the game's window has focus (F1 in another app doesn't count).
static bool KeyDownInForeground(int vk) {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId() && (GetAsyncKeyState(vk) & 0x8000);
}

// The menu's frame (main thread, from DebugMenu_FilterPad): reads pad 1 and F1, opens or
// closes, moves the selection, runs the selected row, applies the per-frame effects and
// hands the result to the renderer.
static void MenuTick() {
    // The left stick counts as the d-pad. pressed: down this frame, not last frame.
    WORD b = g_pad0.Gamepad.wButtons;
    SHORT ly = g_pad0.Gamepad.sThumbLY;
    if (ly > 16000) b |= XINPUT_GAMEPAD_DPAD_UP;
    if (ly < -16000) b |= XINPUT_GAMEPAD_DPAD_DOWN;
    SHORT lx = g_pad0.Gamepad.sThumbLX;
    if (lx > 16000) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if (lx < -16000) b |= XINPUT_GAMEPAD_DPAD_LEFT;
    WORD pressed = b & ~g_prevButtons;
    g_prevButtons = b;
    bool f1 = KeyDownInForeground(VK_F1);
    bool f1Pressed = f1 && !g_prevF1;
    g_prevF1 = f1;

    // Hold LB + RB, press Y. Not Back: that's the game's menu button, and Back + Start /
    // Back + RB belong to the netplay session controls.
    const WORD kHold = XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER;
    bool combo = (b & kHold) == kHold && (pressed & XINPUT_GAMEPAD_Y);
    if (combo || f1Pressed) {
        g_open = !g_open;
        Log("debug menu %s", g_open ? "opened" : "closed");
    }

    Snapshot s;
    BuildRows(s);
    if (g_open) {
        if (g_sel < 0 || g_sel >= s.count || !Selectable(s.rows[g_sel])) { g_sel = -1; MoveSel(s, 1); }
        // Up/Down: a step on the press, then, while held, again after 350 ms and every 90 ms.
        ULONGLONG now = GetTickCount64();
        int dir = (b & XINPUT_GAMEPAD_DPAD_UP) ? -1 : (b & XINPUT_GAMEPAD_DPAD_DOWN) ? 1 : 0;
        if (pressed & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN)) {
            MoveSel(s, dir);
            g_repeatDir = dir;
            g_repeatAt = now + 350;
        } else if (dir && dir == g_repeatDir && now >= g_repeatAt) {
            MoveSel(s, dir);
            g_repeatAt = now + 90;
        } else if (!dir) {
            g_repeatDir = 0;
        }
        if (!combo && g_sel >= 0) {
            const Row& r = s.rows[g_sel];
            if (pressed & XINPUT_GAMEPAD_A) Activate(r, 0);
            else if (r.kind == ROW_CHOICE && (pressed & XINPUT_GAMEPAD_DPAD_LEFT)) Activate(r, -1);
            else if (r.kind == ROW_CHOICE && (pressed & XINPUT_GAMEPAD_DPAD_RIGHT)) Activate(r, 1);
            else if (r.kind == ROW_TOGGLE && (pressed & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT))) Activate(r, 0);
        }
        if (pressed & XINPUT_GAMEPAD_B) g_open = false;
        BuildRows(s);                                // show the result of this tick's action
    }

    // Per-tick effects. In a shared session both PCs run them on the same lockstep frame.
    bool effects = !g_netplay || g_shared;
    if (g_god && effects) { ApplyGod(true); g_godApplied = true; }
    else if (g_godApplied) { ApplyGod(false); g_godApplied = false; }
    if (g_gameOk && effects) ApplyCheatTarget();
    if (g_switchTo) {
        const Level* lv = g_switchTo;
        g_switchTo = NULL;
        if (!g_netplay) {
            if (lv->files || lv->spawnAt) g_menuLevel = lv;
            SwitchLevel(lv->row);
        }
    }

    // Publish for the render thread.
    s.sel = g_sel;
    s.open = g_open;
    EnterCriticalSection(&g_lock);
    g_snap = s;
    LeaveCriticalSection(&g_lock);
}

// Called by the proxy for every pad poll, before the state is recorded or sent.
// frameTick: this poll is the once-per-frame anchor. Returns true if it
// neutralised the pad (menu open).
bool DebugMenu_FilterPad(unsigned user, XINPUT_STATE* st, bool frameTick) {
    if (!Log) return false;
    if (user == 0 && st) g_pad0 = *st;
    if (frameTick) {
        bool wasOpen = g_open;
        MenuTick();
        g_justClosed = wasOpen && !g_open;   // B, or A on Load/Close: that press must not reach the game
        if (!g_open) FreeCamTick();          // the free camera flies on the pad the game doesn't see
        Gfx_FreeCamCulling(FreeFar());       // offline: everything draws while the free camera is on
        HundredTick();
    }
    if ((g_open || g_justClosed || g_freeCam) && user == 0 && st) {
        DWORD packet = st->dwPacketNumber;
        ZeroMemory(st, sizeof *st);
        st->dwPacketNumber = packet;
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ fonts

// A glyph: its rectangle in the atlas (pixels), then its offsets and advance (pixels).
struct Glyph { float x0, y0, x1, y1, left, top, adv; };
// A game font: character map (code point - 1 -> glyph number, 1-based, 0 = none), glyphs,
// line height and the atlas: CPU copy, GPU texture and the upload buffer that fills it.
struct Font {
    int16_t* cmap; int cmapN;
    Glyph*   glyphs; int glyphN;
    float    lineHeight;
    BYTE*    pixels; UINT w, h;          // BGRA atlas (white, coverage in alpha)
    ID3D12Resource* tex;
    ID3D12Resource* upload;
};
static Font g_font[2];                   // 0 = DIN Condensed (items), 1 = Gill Sans Bold (title)
static bool g_fontsOk;

// Reads one game font out of fonts.zip: <base>.fnt (an "fnt2" file: a header of offsets,
// metrics, the character map and 44-byte glyph records in 16.16 fixed point) and <base>.oct,
// which holds the atlas as an uncompressed 32-bit DDS (found by its "DDS |" header, 128
// bytes). False if anything is missing or out of range.
static bool LoadFont(const BYTE* zip, size_t zipLen, const char* base, Font& f) {
    char name[96];
    size_t fl = 0, ol = 0;
    sprintf_s(name, "%s.fnt", base);
    BYTE* fnt = tinyzip::Extract(zip, zipLen, name, &fl);
    sprintf_s(name, "%s.oct", base);
    BYTE* oct = tinyzip::Extract(zip, zipLen, name, &ol);
    bool ok = false;
    if (fnt && oct && fl > 0x40 && memcmp(fnt, "fnt2", 4) == 0) {
        const uint32_t* h = (const uint32_t*)fnt;
        uint32_t metOff = h[4], cmapOff = h[9], cmapN = h[10], gOff = h[11], gN = h[12];
        BYTE* dds = NULL;
        for (size_t i = 0; i + 128 <= ol; i++)
            if (memcmp(oct + i, "DDS |", 5) == 0) { dds = oct + i; break; }
        if (dds && metOff + 4 <= fl && cmapOff + cmapN * 2 <= fl && gOff + gN * 44 <= fl) {
            uint32_t hh = *(uint32_t*)(dds + 12), ww = *(uint32_t*)(dds + 16);
            if ((size_t)(dds - oct) + 128 + (size_t)ww * hh * 4 <= ol && ww && hh && ww <= 4096 && hh <= 4096) {
                f.w = ww; f.h = hh;
                f.pixels = (BYTE*)malloc((size_t)ww * hh * 4);
                memcpy(f.pixels, dds + 128, (size_t)ww * hh * 4);
                f.cmapN = (int)cmapN;
                f.cmap = (int16_t*)malloc(cmapN * 2);
                memcpy(f.cmap, fnt + cmapOff, cmapN * 2);
                f.glyphN = (int)gN;
                f.glyphs = (Glyph*)malloc(gN * sizeof(Glyph));
                for (uint32_t g = 0; g < gN; g++) {
                    const int32_t* r = (const int32_t*)(fnt + gOff + g * 44);
                    f.glyphs[g] = { (float)r[0], (float)r[1], (float)r[2], (float)r[3],
                                    r[4] / 65536.0f, r[5] / 65536.0f, r[8] / 65536.0f };
                }
                f.lineHeight = *(const int32_t*)(fnt + metOff) / 65536.0f;
                ok = true;
            }
        }
    }
    free(fnt);
    free(oct);
    return ok;
}

// Loads the menu's two fonts from assets\ui\fonts\fonts.zip next to the exe; g_fontsOk
// stays false (and the menu off) if either fails.
static void LoadFonts() {
    wchar_t exe[MAX_PATH], path[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) *slash = 0;
    swprintf_s(path, L"%s\\assets\\ui\\fonts\\fonts.zip", exe);
    HANDLE fh = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (fh == INVALID_HANDLE_VALUE) { Log("debug menu: can't open %ls", path); return; }
    DWORD size = GetFileSize(fh, NULL), got = 0;
    BYTE* zip = (BYTE*)malloc(size);
    if (zip && ReadFile(fh, zip, size, &got, NULL) && got == size) {
        g_fontsOk = LoadFont(zip, size, "tradegothic_english_ntsc", g_font[0]) &&
                    LoadFont(zip, size, "urwegyptienne_english_ntsc", g_font[1]);
    }
    CloseHandle(fh);
    free(zip);
    Log(g_fontsOk ? "debug menu: game fonts loaded (DIN Condensed %ux%u, Gill Sans Bold %ux%u)"
                  : "debug menu: couldn't read the game's fonts; menu disabled",
        g_font[0].w, g_font[0].h, g_font[1].w, g_font[1].h);
}

// The glyph for a character, or NULL if the font has none.
static const Glyph* FindGlyph(const Font& f, unsigned cp) {
    if (cp == 0 || cp > (unsigned)f.cmapN) return NULL;
    int v = f.cmap[cp - 1];
    if (v <= 0 || v > f.glyphN) return NULL;
    return &f.glyphs[v - 1];
}

// Width of s in pixels at scale, as Text lays it out (a missing glyph is a quarter line wide).
static float TextWidth(const Font& f, const char* s, float scale) {
    float pen = 0;
    for (; *s; s++) {
        const Glyph* g = FindGlyph(f, (unsigned char)*s);
        if (g) pen += g->left + (g->x1 - g->x0) + g->adv;
        else pen += f.lineHeight * 0.25f;
    }
    return pen * scale;
}

// ------------------------------------------------------------------ rendering (render thread)

// A screen vertex: position in pixels, atlas uv (negative: solid colour), colour. The
// vertex buffer holds kRing frames of kMaxVerts each, used in turn, so a frame doesn't
// overwrite vertices an earlier frame's draw may still be reading.
struct Vtx { float x, y, u, v; uint32_t c; };
static const UINT kRing = 4, kMaxVerts = 24000;

// The menu's D3D12 objects, made on the first draw (GpuInit, GpuPso).
struct Gpu {
    bool failed, ready;
    ID3D12RootSignature* rs;
    ID3D12PipelineState* pso;
    DXGI_FORMAT psoFmt;
    ID3D12DescriptorHeap* srv;
    ID3D12Resource* vb;
    Vtx* vbMap;
    UINT ring;
    bool texUploaded;
    ID3DBlob* vs;
    ID3DBlob* ps;
};
static Gpu g_gpu;

// The menu's shaders: pixels to clip space; a vertex with uv.x < 0 draws its colour, any
// other is the colour with the font atlas's coverage as alpha.
static const char kMenuHlsl[] = R"(
cbuffer C : register(b0) { float2 invScreen; };
Texture2D tex : register(t0);
SamplerState samp : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 col : COLOR0; };
V VS(float2 p : POSITION, float2 uv : TEXCOORD0, float4 col : COLOR0) {
    V o;
    o.pos = float4(p.x * invScreen.x * 2 - 1, 1 - p.y * invScreen.y * 2, 0, 1);
    o.uv = uv;
    o.col = col;
    return o;
}
float4 PS(V i) : SV_Target {
    if (i.uv.x < 0) return i.col;
    return float4(i.col.rgb, i.col.a * tex.SampleLevel(samp, i.uv, 0).a);
}
)";

// Logs why and turns drawing off for good.
static void GpuFail(const char* what, HRESULT hr) {
    Log("debug menu: drawing disabled: %s failed (0x%08lx)", what, (unsigned long)hr);
    g_gpu.failed = true;
}

// Creates what the menu draws with, on the first DrawOverlay (render thread): compiles the
// shaders (d3dcompiler_47.dll, loaded at run time), then the root signature, descriptor
// heap, vertex buffer and both font textures with their upload buffers.
static bool GpuInit(ID3D12Device* dev) {
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    pD3DCompile compile = dc ? (pD3DCompile)GetProcAddress(dc, "D3DCompile") : NULL;
    if (!compile) { GpuFail("d3dcompiler_47.dll", E_FAIL); return false; }
    ID3DBlob* err = NULL;
    HRESULT hr = compile(kMenuHlsl, sizeof kMenuHlsl - 1, "menu", NULL, NULL, "VS", "vs_5_1", 0, 0, &g_gpu.vs, &err);
    if (SUCCEEDED(hr)) {
        if (err) { err->Release(); err = NULL; }
        hr = compile(kMenuHlsl, sizeof kMenuHlsl - 1, "menu", NULL, NULL, "PS", "ps_5_1", 0, 0, &g_gpu.ps, &err);
    }
    if (FAILED(hr)) {
        Log("debug menu: shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        if (err) err->Release();
        g_gpu.failed = true;
        return false;
    }

    // Root signature: 2 constants (1 / screen size) for the vertex shader, one texture
    // table and a linear clamp sampler for the pixel shader.
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 2;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, params, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    typedef HRESULT (WINAPI* PFN_Ser)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    PFN_Ser ser = (PFN_Ser)GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature");
    ID3DBlob* blob = NULL;
    if (!ser || FAILED(hr = ser(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) { if (err) err->Release(); GpuFail("root signature", hr); return false; }
    hr = dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), (void**)&g_gpu.rs);
    blob->Release();
    if (FAILED(hr)) { GpuFail("CreateRootSignature", hr); return false; }

    // A shader-visible heap with one SRV per font.
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    if (FAILED(hr = dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&g_gpu.srv))) { GpuFail("SRV heap", hr); return false; }
    UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // The vertex ring, in an upload heap the CPU writes directly (mapped for good).
    D3D12_HEAP_PROPERTIES up = { D3D12_HEAP_TYPE_UPLOAD }, def = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = sizeof(Vtx) * kMaxVerts * kRing;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(hr = dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                                 __uuidof(ID3D12Resource), (void**)&g_gpu.vb))) { GpuFail("vertex buffer", hr); return false; }
    D3D12_RANGE none = { 0, 0 };
    g_gpu.vb->Map(0, &none, (void**)&g_gpu.vbMap);

    // Each font: a GPU texture, an upload buffer holding its pixels row by row in the GPU's
    // layout (DrawOverlay records the copy), and its SRV.
    for (int i = 0; i < 2; i++) {
        Font& f = g_font[i];
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = f.w; td.Height = f.h;
        td.DepthOrArraySize = td.MipLevels = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        if (FAILED(hr = dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                                     __uuidof(ID3D12Resource), (void**)&f.tex))) { GpuFail("font texture", hr); return false; }
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
        UINT rows; UINT64 rowSize, total;
        dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &rowSize, &total);
        D3D12_RESOURCE_DESC ud = bd;
        ud.Width = total;
        if (FAILED(hr = dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &ud, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                                     __uuidof(ID3D12Resource), (void**)&f.upload))) { GpuFail("font upload", hr); return false; }
        BYTE* m = NULL;
        f.upload->Map(0, &none, (void**)&m);
        for (UINT y = 0; y < f.h; y++) memcpy(m + fp.Offset + (size_t)y * fp.Footprint.RowPitch, f.pixels + (size_t)y * f.w * 4, (size_t)f.w * 4);
        f.upload->Unmap(0, NULL);
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_gpu.srv->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * inc;
        dev->CreateShaderResourceView(f.tex, &sv, h);
    }
    g_gpu.ready = true;
    return true;
}

// The pipeline state for the back buffer's format, remade if the format changes: alpha
// blended, no depth, no culling.
static bool GpuPso(ID3D12Device* dev, DXGI_FORMAT fmt) {
    if (g_gpu.pso && g_gpu.psoFmt == fmt) return true;
    if (g_gpu.pso) { g_gpu.pso->Release(); g_gpu.pso = NULL; }
    D3D12_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 } };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = g_gpu.rs;
    d.VS = { g_gpu.vs->GetBufferPointer(), g_gpu.vs->GetBufferSize() };
    d.PS = { g_gpu.ps->GetBufferPointer(), g_gpu.ps->GetBufferSize() };
    D3D12_RENDER_TARGET_BLEND_DESC& bl = d.BlendState.RenderTarget[0];
    bl.BlendEnable = TRUE;
    bl.SrcBlend = D3D12_BLEND_SRC_ALPHA; bl.DestBlend = D3D12_BLEND_INV_SRC_ALPHA; bl.BlendOp = D3D12_BLEND_OP_ADD;
    bl.SrcBlendAlpha = D3D12_BLEND_ONE; bl.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; bl.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    bl.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.InputLayout = { il, 3 };
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = fmt;
    d.SampleDesc.Count = 1;
    HRESULT hr = dev->CreateGraphicsPipelineState(&d, __uuidof(ID3D12PipelineState), (void**)&g_gpu.pso);
    if (FAILED(hr)) { GpuFail("pipeline state", hr); return false; }
    g_gpu.psoFmt = fmt;
    return true;
}

// Vertices being written into this frame's ring slot.
struct Batch { Vtx* v; UINT n, cap; };

// Two triangles; dropped once the batch is full.
static void Quad(Batch& b, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t c) {
    if (b.n + 6 > b.cap) return;
    Vtx q[4] = { { x0, y0, u0, v0, c }, { x1, y0, u1, v0, c }, { x0, y1, u0, v1, c }, { x1, y1, u1, v1, c } };
    Vtx* o = b.v + b.n;
    o[0] = q[0]; o[1] = q[1]; o[2] = q[2]; o[3] = q[2]; o[4] = q[1]; o[5] = q[3];
    b.n += 6;
}

// A solid rectangle (uv -1: the shader's colour-only path).
static void Rect(Batch& b, float x0, float y0, float x1, float y1, uint32_t c) { Quad(b, x0, y0, x1, y1, -1, -1, -1, -1, c); }

// Draws s with its pen starting at x, glyph tops placed relative to y.
static void Text(Batch& b, const Font& f, float x, float y, float scale, uint32_t c, const char* s) {
    float pen = x;
    for (; *s; s++) {
        const Glyph* g = FindGlyph(f, (unsigned char)*s);
        if (!g) { pen += f.lineHeight * 0.25f * scale; continue; }
        if (g->x1 > g->x0) {
            float gx = pen + g->left * scale, gy = y + g->top * scale;
            Quad(b, gx, gy, gx + (g->x1 - g->x0) * scale, gy + (g->y1 - g->y0) * scale,
                 g->x0 / f.w, g->y0 / f.h, g->x1 / f.w, g->y1 / f.h, c);
        }
        pen += (g->left + (g->x1 - g->x0) + g->adv) * scale;
    }
}

// c with its alpha multiplied by a.
static uint32_t Alpha(uint32_t c, float a) { return (c & 0x00FFFFFF) | ((uint32_t)(((c >> 24) & 0xFF) * a) << 24); }

// gfx_d3d12.cpp's Present pass calls this every frame on the render thread, with the back
// buffer to draw on. Draws the latest snapshot while the menu is open. The fonts' upload
// is recorded on the first call either way.
static void DrawOverlay(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                        DXGI_FORMAT fmt, UINT w, UINT h) {
    if (!g_fontsOk || g_gpu.failed) return;
    // A copy of the menu, taken under the lock MenuTick writes it under.
    Snapshot s;
    EnterCriticalSection(&g_lock);
    bool open = g_snap.open;
    if (open) s = g_snap;
    LeaveCriticalSection(&g_lock);
    if (!open && g_gpu.texUploaded) return;
    if (!g_gpu.ready && !GpuInit(dev)) return;
    if (!GpuPso(dev, fmt)) return;
    if (!g_gpu.texUploaded) {                        // first call: copy the font atlases to the GPU
        for (int i = 0; i < 2; i++) {
            Font& f = g_font[i];
            D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
            dst.pResource = f.tex;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = f.upload;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            D3D12_RESOURCE_DESC td = f.tex->GetDesc();
            UINT rows; UINT64 rowSize, total;
            dev->GetCopyableFootprints(&td, 0, 1, 0, &src.PlacedFootprint, &rows, &rowSize, &total);
            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = f.tex;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            cl->ResourceBarrier(1, &b);
        }
        g_gpu.texUploaded = true;
        if (!open) return;
    }

    // Layout, scaled from 1080p.
    float k = h / 1080.0f;
    float itemScale = 0.60f * k, titleScale = 0.72f * k, headScale = 0.48f * k;
    float rowH = 40.0f * k, pad = 22.0f * k;
    float x0 = 70.0f * k, y0 = 90.0f * k, width = 720.0f * k;
    float titleH = 78.0f * k;
    // Rows that fit on screen; a longer menu scrolls to keep the selection in view.
    int maxVis = (int)((h - y0 - titleH - pad * 1.1f - 40.0f * k) / rowH);
    int first = 0, last = s.count;
    if (maxVis > 0 && s.count > maxVis) {
        first = s.sel - maxVis / 2;
        if (first > s.count - maxVis) first = s.count - maxVis;
        if (first < 0) first = 0;
        last = first + maxVis;
    }
    float height = titleH + pad;
    for (int i = first; i < last; i++) height += s.rows[i].kind == ROW_HEADER ? rowH * 0.95f : rowH;
    height += pad * 0.5f;

    UINT slot = g_gpu.ring++ % kRing;
    Batch b = { g_gpu.vbMap + (size_t)slot * kMaxVerts, 0, kMaxVerts };
    // Panel: drop shadow, blue body, gold title bar, selection bar. (All drawn with texture 0, uv < 0 = solid.)
    Rect(b, x0 + 8 * k, y0 + 8 * k, x0 + width + 8 * k, y0 + height + 8 * k, 0x66000000);
    Rect(b, x0, y0, x0 + width, y0 + height, 0xE6703A14);
    Rect(b, x0, y0, x0 + width, y0 + titleH, 0xFF1FA6F2);
    Rect(b, x0, y0 + titleH, x0 + width, y0 + titleH + 4 * k, 0xFF0B5FA0);
    float y = y0 + titleH + pad * 0.6f;
    for (int i = first; i < last; i++) {
        const Row& r = s.rows[i];
        float rh = r.kind == ROW_HEADER ? rowH * 0.95f : rowH;
        if (i == s.sel) Rect(b, x0 + 10 * k, y - 2 * k, x0 + width - 10 * k, y + rh - 4 * k, 0x5533C8FF);
        y += rh;
    }
    // Rows, in the item font (texture 0)
    y = y0 + titleH + pad * 0.6f;
    const Font& item = g_font[0];
    if (first > 0) Text(b, item, x0 + width * 0.5f, y0 + titleH - 2 * k, headScale, kGold, "^ more ^");
    if (last < s.count) Text(b, item, x0 + width * 0.5f, y0 + height - pad * 0.9f, headScale, kGold, "v more v");
    for (int i = first; i < last; i++) {
        const Row& r = s.rows[i];
        float rh = r.kind == ROW_HEADER ? rowH * 0.95f : rowH;
        float textY = y + (rh - item.lineHeight * itemScale) * 0.5f - 2 * k;
        if (r.kind == ROW_HEADER) {
            Text(b, item, x0 + pad, y + (rh - item.lineHeight * headScale) * 0.5f, headScale, kGold, r.label);
        } else {
            uint32_t lc = r.disabled ? kGrey : (i == s.sel ? 0xFF7FF0FF : kWhite);
            Text(b, item, x0 + pad + 14 * k, textY, itemScale, lc, r.label);
            if (r.value[0]) {
                float vw = TextWidth(item, r.value, itemScale);
                Text(b, item, x0 + width - pad - vw, textY, itemScale, r.disabled ? Alpha(r.valueColor, 0.6f) : r.valueColor, r.value);
            }
        }
        y += rh;
    }
    const char* hint = "LB+RB+Y close";
    Text(b, item, x0 + width - pad - TextWidth(item, hint, headScale), y0 + (titleH - item.lineHeight * headScale) * 0.5f,
         headScale, 0xCCFFFFFF, hint);
    UINT itemEnd = b.n;
    // Title, in the title font (texture 1)
    const Font& title = g_font[1];
    Text(b, title, x0 + pad, y0 + (titleH - title.lineHeight * titleScale) * 0.5f, titleScale, kWhite, "DEBUG MENU");
    UINT end = b.n;

    // Draw: the item-font vertices with texture 0, then the title's with texture 1.
    D3D12_GPU_VIRTUAL_ADDRESS va = g_gpu.vb->GetGPUVirtualAddress() + (UINT64)slot * kMaxVerts * sizeof(Vtx);
    D3D12_VERTEX_BUFFER_VIEW vbv = { va, (UINT)(end * sizeof(Vtx)), sizeof(Vtx) };
    cl->SetPipelineState(g_gpu.pso);
    cl->SetGraphicsRootSignature(g_gpu.rs);
    cl->SetDescriptorHeaps(1, &g_gpu.srv);
    float inv[2] = { 1.0f / w, 1.0f / h };
    cl->SetGraphicsRoot32BitConstants(0, 2, inv, 0);
    D3D12_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
    D3D12_RECT sr = { 0, 0, (LONG)w, (LONG)h };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sr);
    cl->OMSetRenderTargets(1, &rtv, FALSE, NULL);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->IASetVertexBuffers(0, 1, &vbv);
    UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_GPU_DESCRIPTOR_HANDLE t0 = g_gpu.srv->GetGPUDescriptorHandleForHeapStart(), t1 = t0;
    t1.ptr += inc;
    cl->SetGraphicsRootDescriptorTable(1, t0);
    cl->DrawInstanced(itemEnd, 1, 0, 0);
    if (end > itemEnd) {
        cl->SetGraphicsRootDescriptorTable(1, t1);
        cl->DrawInstanced(end - itemEnd, 1, itemEnd, 0);
    }
}

// ------------------------------------------------------------------ entry

// From DllMain (xinput_proxy.cpp) at process attach. Checks each feature's game code
// against this build, loads the fonts, installs the hooks and registers DrawOverlay. In the
// proxy's netplay mode the hooks that change the game wait for DebugMenu_SetNetplay(false)
// or a session's shared actions.
void DebugMenu_Install(const wchar_t* iniPath, MenuLogFn log, bool netplay) {
    // No ini means "silent passthrough": no menu, no hooks. With an ini the menu is on unless enabled=0.
    if (GetFileAttributesW(iniPath) == INVALID_FILE_ATTRIBUTES) return;
    if (!GetPrivateProfileIntW(L"debug_menu", L"enabled", 1, iniPath)) return;
    Log = log;
    g_netplay = netplay;
    InitializeCriticalSection(&g_lock);
    g_gameOk = VerifyGame();
    if (!g_gameOk) Log("debug menu: cheat/player addresses don't match this build; only graphics and info rows");
    LoadFonts();
    if (!g_fontsOk) { Log = NULL; return; }
    g_statsOk = VerifyStats();
    if (!g_statsOk) Log("debug menu: stats code doesn't match this build; no coin/star rows");
    g_locksOk = VerifyLocks();
    if (!g_locksOk) Log("debug menu: lock manager code doesn't match this build; no unlock rows");
    g_levelOk = VerifyLevelSwitch();
    if (!g_levelOk) Log("debug menu: SwitchLevel code doesn't match this build; no level loader");
    g_teleportOk = VerifyTeleport();
    if (!g_teleportOk) Log("debug menu: ActorSetPosition code doesn't match this build; no teleport rows");
    g_hundredOk = g_locksOk && g_statsOk && VerifyHundred();
    if (!g_hundredOk) Log("debug menu: game-vars/challenge code doesn't match this build; no \"test save 100%%\" row");
    HookCamera();                                // free camera and field of view: the draw's view only (fine online)
    g_camProbe = GetPrivateProfileIntW(L"graphics", L"camera_race_probe", 0, iniPath) != 0;
    if (g_camProbe) Log("debug menu: camera race probe on (viewport 0's view builds against the main thread's camera)%s",
                        g_camUpdateOrig ? "" : "; NO camera update hook, so it can't run");
    if (g_levelOk && !netplay) HookLevelLoading();
    if (!netplay && g_gameOk) Log(PatchAmmoOwner() ? "debug menu: Ammo cheat covers every lobbed/thrown weapon"
                                                   : "debug menu: Ammo owner check doesn't match this build; left as is");
    if (!netplay) HookFrontend();
    if (!netplay && !g_frontendOrig) Log("debug menu: title screen code doesn't match this build; no Debug Options");
    GfxExt_InstallOverlay(DrawOverlay, log);
    Log("debug menu ready: hold LB + RB and press Y on pad 1 (or F1)%s", netplay ? "; cheats locked in online co-op" : "");
}

// The proxy's test_save mode is on: the save is in its own folder and achievements are
// blocked from boot, so the "make this test save 100%" row may be offered.
void DebugMenu_SetTestSave(bool on) { g_testSave = on; }

// For a game that switches between solo play and an online session (drop-in: a friend
// joining while the host is already playing): call with true before lockstep starts and
// false when it ends, or with false on the first poll when the proxy runs in netplay mode
// without a session. Call on the game thread. Turning netplay on
// puts everything the menu can change back to the game's defaults, so the host's
// solo cheats can't make the two PCs differ: cheats off, cheat target cleared,
// normal game speed, god mode off, the Ammo owner check restored and the title
// screen hook removed. The level-loading detours stay but do nothing without a
// menu-chosen level. Turning it off again re-applies the menu's patches.
void DebugMenu_SetNetplay(bool on) {
    if (!Log || on == g_netplay) return;
    g_netplay = on;
    g_shared = g_canAct = false;
    g_outHead = g_outCount = 0;
    if (on) {
        g_open = false;
        Gfx_FreeCamCulling(false);               // the culls feed the simulation: the game's own from now on
        g_switchTo = NULL;
        g_menuLevel = NULL;
        g_wantDebugOptions = false;
        if (g_gameOk) {
            __try {
                for (int i = 0, n = CheatCount(); i < n; i++) CheatEntry(i)[0x14] = 0;
                *At<uint64_t>(RVA_CHEAT_TARGET) = 0;
                if (*At<int>(RVA_TIME_MODE) != kSpeedModes[0]) SetSpeed(0);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            UnpatchAmmoOwner();
        }
        if (g_godApplied) ApplyGod(false);
        g_god = g_godApplied = false;
        g_spotSet[0] = g_spotSet[1] = false;     // spots saved alone could differ between the PCs
        UnhookFrontend();
        Log("debug menu: online session: cheats, speed and god mode reset; menu patches removed");
    } else {
        // A menu installed in netplay mode never installed these; do it the first time.
        if (g_levelOk && !g_setWorldPathOrig) HookLevelLoading();
        if (g_gameOk) PatchAmmoOwner();
        HookFrontend();                          // backs off if the proxy's own hook holds the slot
        Log("debug menu: no online session; menu unlocked");
    }
}

// ------------------------------------------------------------------ shared menu actions (online co-op)
//
// Lets the players of an online session use the menu together: one PC's menu
// actions (cheats, god mode, game speed, coins, unlocks, lock states, level loads, teleports)
// are carried through lockstep and performed by both PCs on the same frame.
// Protocol, all on the game thread:
//   1. Session start: DebugMenu_SetNetplay(true), then DebugMenu_EnableSharedActions(true,
//      canAct) on BOTH PCs (canAct: may this PC's menu act; e.g. host only). This puts
//      the menu's code patches in place identically on both PCs.
//   2. Every frame, after the pad poll that runs the menu: while DebugMenu_PollAction
//      returns bytes, send them with this frame's input.
//   3. When lockstep delivers frame N, before the game reads that frame's input, call
//      DebugMenu_ApplyAction with each action scheduled for N, in the same order on
//      both PCs (the sender too: its own actions are only queued, not performed).
// Actions are fixed-size (DebugMenu_ActionSize bytes) and refer to tables compiled
// into this DLL, so both PCs must run the same build (the netplay fingerprint: a hash of
// the settings sheet, the settings both PCs must share, build number included; PCs whose
// fingerprints differ don't connect).
//
// Step 1 above. Turning shared actions on also installs the level hooks (if they aren't
// yet) and the Ammo patch, the same on both PCs; turning them off takes the Ammo patch out.
void DebugMenu_EnableSharedActions(bool enable, bool canAct) {
    if (!Log || !g_netplay) return;
    g_shared = enable;
    g_canAct = enable && canAct;
    g_outHead = g_outCount = 0;
    if (enable) {
        if (g_levelOk && !g_setWorldPathOrig) HookLevelLoading();
        if (g_gameOk) PatchAmmoOwner();
    } else if (g_gameOk) {
        UnpatchAmmoOwner();
    }
    Log("debug menu: shared actions %s%s", enable ? "on" : "off", enable ? (canAct ? " (this PC can act)" : " (view only)") : "");
}

// The size of one action on the wire (12 bytes).
int DebugMenu_ActionSize() { return (int)sizeof(MenuAction); }

// Copies the next queued action into out (cap >= DebugMenu_ActionSize()); returns its size, or 0.
int DebugMenu_PollAction(void* out, int cap) {
    if (!Log || !g_outCount || !out || cap < (int)sizeof(MenuAction)) return 0;
    memcpy(out, &g_outbox[g_outHead], sizeof(MenuAction));
    g_outHead = (g_outHead + 1) % 8;
    g_outCount--;
    return (int)sizeof(MenuAction);
}

// Step 3 above: performs one action lockstep delivered for this frame. Ignored outside a
// shared session or if the size is wrong.
void DebugMenu_ApplyAction(const void* data, int len) {
    if (!Log || !g_shared || !data || len != (int)sizeof(MenuAction)) return;
    MenuAction m;
    memcpy(&m, data, sizeof m);
    if (Log) Log("debug menu: performing shared action %d (%d, %d)", m.id, m.a, m.b);
    Perform(m);
}
