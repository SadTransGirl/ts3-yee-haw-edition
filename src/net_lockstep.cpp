// net_lockstep.cpp -- the lockstep core: the state fingerprint and its check against the other
// PC's (NetCompareHash, NetRemoteHash), in-place desync repair, session
// start, the per-frame driver (NetFrame/NetFrameBody), the drop-in steps the frame driver runs and
// the per-frame state diagnostics.
//
// Lockstep: both PCs run the whole game and exchange only controller input. Frame f uses the
// same two pads on both PCs, so as long as the simulation is deterministic (sim_determinism.cpp,
// net_determinism.cpp) the two games stay identical. NetFrame is the entry point: it runs once
// per game frame on the game's main thread, from the "anchor" XInputGetState poll (NetGetState
// in net_session.cpp). Before a session it drives the connection (HELLO, the drop-in save and
// quit, READY); in a session it sends this PC's input for frame f + delay, waits for the other
// PC's input for f, and runs the per-frame steps at the "lockstep point": the same place in the
// same frame on both PCs, so anything done there is done identically on both.
//
// Fingerprint: every hash_interval frames each PC hashes the state that must match
// (NetFingerprint) and sends it; different hashes mean a desync. Repair: rather than ending the
// session, the client copies the host's values for the pieces that differ (the repair section).
// Drop-in: a friend connecting while the host is in a level. The host saves, both quit to the
// front end, and on lockstep frame kDropLaunchFrame both load that save (DropLaunch). A level
// "publishes" when it has finished loading; until then the two PCs' screens differ, so the
// fingerprint check is held off (g_netHashHoldUntil). The load gate (g_loadGate, see
// net_session.cpp) holds each PC's "level loaded" step until both PCs have loaded and streamed.
// Packets come in through NetPump (net_session.cpp) under the net lock, on any thread; the
// handlers here (NetRemoteHash, RepHandle) mostly just store what arrived.
//
// Game addresses: a 0x14... number is an address in the game's exe as the decomp session's
// tools show it (the exe's preferred base is 0x140000000). Code here uses RVAs, offsets from
// the exe's base (ExeBase() + rva), because Windows may load the exe elsewhere. Before relying
// on one, the code checks the game's bytes there (BytesAt, RipRefersTo), so another game build
// is detected instead of patched blindly.

#include "proxy_shared.h"

static void NetLogStateParts(uint32_t f);
static void NetLogPhysFrame(uint32_t f);          // per-frame body hashes after a level's physics comes up

static BYTE* GameTimer();
// Only the rand() sites that feed the simulation (decomp session's list): town-person
// interaction, AI/animation picks, vehicle physics, pathfinding. Ambient sound (listener
// and real-time driven) and the one-time noise table may differ without any desync.
// A site is known by its call's return address, turned into the 0x14... form the list uses
// (RVA + 0x140000000); a + 6 is accepted too.
bool NetSimRandSite(int i) {
    uint64_t ret = 0x140000000ULL + Rva(g_randSites[i].ret);
    static const uint64_t sim[] = { 0x1400c4794, 0x140189d02, 0x140712a16, 0x140523000 };
    for (uint64_t a : sim) if (ret == a || ret == a + 6) return true;
    return g_net.testInputSeed != 0;                   // tests: stand-ins have their own sites
}
// The state fingerprint: a 64-bit FNV-1a hash of what must be identical on both PCs and is
// known to be read correctly: the engine frame count, the frame's game time, the simulation's
// rand() streams, the spawn sequence, the game timer (in a session) and the engine's
// Mersenne Twister (MT, the game's main random generator). Sent every hash_interval frames.
static uint64_t NetFingerprint() {
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    };
    mix(&g_engineFrames, sizeof g_engineFrames);
    LONG64 vus = g_clockLockstep ? g_frameStartUs : g_vus;   // in a session: deterministic frame start
    mix(&vus, sizeof vus);
    for (int i = 0; i < g_randSiteCount; i++)
        if (NetSimRandSite(i)) mix((const void*)&g_randSites[i].state, sizeof(LONG));
    mix(&g_spawnSeq, sizeof g_spawnSeq);
    if (g_clockLockstep) if (BYTE* t = GameTimer()) mix(t, 0x34);   // aligned at session start (NetResetGameTimer)
    if (g_mtGlobalPtr && *g_mtGlobalPtr) {
        const uint8_t* inst = (const uint8_t*)*g_mtGlobalPtr;
        __try {
            mix(inst, 624 * 4);                    // the state words
            mix(inst + 0x9c8, 4);                  // words left (the cursor itself is an address)
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return h;
}

// In-place repair (see the repair section below): while it runs, a fingerprint
// that disagrees is the repair layer's job first; the session only ends when
// several in a row disagree (the repair couldn't keep up) or the repair layer
// itself finds something it can't copy.
bool g_repairOn;
static int  g_fpBadRun;                                 // fingerprint mismatches in a row
// Compares frame f's two fingerprints once both are in (a ring slot's tag holds its frame + 1,
// 0 = empty), then empties the slot so the frame is compared once.
static void NetCompareHash(uint32_t f) {
    if (f <= g_netHashHoldUntil) return;                // drop-in: the two title screens differ until the level publishes
    int i = f % kNetRing;
    if (g_netHashLocalTag[i] != f + 1 || g_netHashRemoteTag[i] != f + 1) return;
    g_netHashesChecked++;
    if (g_netHashLocal[i] != g_netHashRemote[i]) {
        if (g_repairOn && ++g_fpBadRun < 4) {
            Log("netplay: fingerprints differ at frame %u (%d in a row); the repair layer is copying the host's state",
                f, g_fpBadRun);
        } else {
            Log("netplay: DESYNC at frame %u: local fingerprint %016llx, peer %016llx", f, g_netHashLocal[i], g_netHashRemote[i]);
            NetEnd("the two games stopped matching (desync)");
        }
    } else {
        g_fpBadRun = 0;
    }
    g_netHashLocalTag[i] = g_netHashRemoteTag[i] = 0;
}

// The other PC's fingerprint for frame f, from a HASH packet or the one an INPUT packet
// carries (NetHandle, net_session.cpp, under the net lock).
void NetRemoteHash(uint32_t f, uint64_t h) {
    int i = f % kNetRing;
    if (g_netHashRemoteTag[i] == f + 1 || f + kNetRing <= g_netFrame) return;   // already have it, or too old
    g_netHashRemote[i] = h;
    g_netHashRemoteTag[i] = f + 1;
    NetCompareHash(f);
}

// --- in-place desync repair (Tier 1) -----------------------------------------
//
// Spec from the decomp session (re\SYNC_VIABILITY.md "Design: repair a desync
// in place, without reloading"). Every desync traced so far began as a few low
// bits in one Bullet body or one extra MT draw, and only later spread into AI,
// spawns and missions. So at every fingerprint frame both PCs hash each piece
// of copyable state; when the client's differs from the host's, the client
// asks for those pieces and copies the host's values at an agreed later frame
// T (repair_lead frames ahead), at the same lockstep point on both PCs, and
// play carries on with no reload.
//   host  -> client  RDIGEST F: unrepairable hash + (kind, id, hash) per item
//   client -> host   RREQ T: the items that differ
//   host  (frame T)  captures them, sends RVAL T, drops the bodies' contact pairs
//   client (frame T) waits for RVAL T, writes them, drops the same pairs
// The client re-asks (RNAK) every 200 ms while it waits; a host past T answers
// RMISSED and the next round tries again.
// Copyable: game timer, the global MT and this DLL's three private MTs, the
// rand() streams, every non-static Bullet body (transform, interpolation
// transform and velocities, linear and angular velocity, inverse world inertia,
// activation state, sleep timer, broadphase AABB) and every live actor's
// transform block (+0x80..+0xdf), physicsFlags (+0x3e8) and scene position.
// Not copyable (ends the session as before): different body or actor sets
// (a spawn on one PC only), engine frame count, frame start, spawn sequence,
// entity-key counter. Inside the stand-in tests only the rand() streams exist.
// Item kinds. RK_PROBE, or'd into a requested kind, asks for values to compare only (never written).
enum { RK_TIMER = 1, RK_MT = 2, RK_MT_UI = 3, RK_MT_DLG = 4, RK_MT_COS = 5, RK_RAND = 6, RK_BODY = 7, RK_ACTOR = 8, RK_PROBE = 0x80 };
static const int kRepChunk = 480;                       // payload bytes per packet (a chunk stays under 512 bytes)
#pragma pack(push, 1)
// One packet of a repair message (digest, request or values): chunk seq of total, all
// sharing the message's frame. RNAK and RMISSED use only the header and frame.
struct NetRepChunk { NetHdr h; uint32_t frame; uint16_t seq, total, len; uint8_t data[kRepChunk]; };
// cls 0: core state (RNG, timer, rand streams, dynamic bodies): copied whenever it differs.
// cls 1: followers (kinematic bodies, actor transforms): with own screens the render path
// moves some of these differently on each PC without the game diverging (one-PC test
// 2026-10-25: 31 differ at frame 60 with matching fingerprints, and copying them made the
// RNG diverge). Copied only when the fingerprint differs too (a real desync).
struct RepItem { uint8_t kind, cls; uint32_t id; uint32_t hash; };
// A digest's head: hashes of the parts that can't be copied (they must already match), counts
// and raw values for the log, then `items` RepItems follow.
struct RepDigestHead {
    uint32_t frame;
    uint32_t fixedCounters, fixedKeys, fixedSlots, fixedActors;   // parts that can't be copied
    uint32_t bodies, actors, spawnSeq, items;
    uint64_t engineFrames, frameStartUs, fingerprint;
};
#pragma pack(pop)

struct RepAsm {                                         // one chunked message being reassembled
    uint32_t frame = 0; uint16_t total = 0, got = 0; uint32_t size = 0; bool done = false;
    std::vector<uint8_t> data; std::vector<uint8_t> have;
    void Reset(uint32_t f, uint16_t t) { frame = f; total = t; got = 0; size = 0; done = false; data.assign((size_t)t * kRepChunk, 0); have.assign(t, 0); }
    // Takes one chunk (a chunk of another message starts over; duplicates are ignored).
    // True once every chunk is in.
    bool Add(const NetRepChunk* c) {
        if (c->total == 0 || c->total > 256 || c->len > kRepChunk) return false;
        if (frame != c->frame || total != c->total) Reset(c->frame, c->total);
        if (c->seq >= total || have[c->seq] || done) return done;
        memcpy(&data[(size_t)c->seq * kRepChunk], c->data, c->len);
        have[c->seq] = 1;
        if (c->seq == total - 1) size = (uint32_t)(c->seq * kRepChunk + c->len);
        if (++got == total) { data.resize(size); done = true; }
        return done;
    }
};
static RepAsm g_repDigestIn, g_repReqIn, g_repValIn;
static std::vector<uint8_t> g_repMyDigest;              // client: own digest for g_repMyDigestFrame
static uint32_t g_repMyDigestFrame = 0xFFFFFFFF, g_repHandledDigest = 0xFFFFFFFF;
static uint32_t g_repPendingT = 0;                      // client: waiting to copy at this frame (0 = none)
static std::vector<uint8_t> g_repPendingItems;          // client: the request sent for g_repPendingT
static uint32_t g_repHostT = 0;                         // host: capture at this frame (0 = none)
static std::vector<uint8_t> g_repHostItems;             // host: the request for g_repHostT
static uint32_t g_repSentT = 0;                         // host: last values sent, kept for re-asks
static std::vector<uint8_t> g_repSentVals;
static volatile uint32_t g_repMissedT = 0;              // client: the host said it was already past this T
uint64_t g_repRounds, g_repItemsFixed, g_repMissedCount, g_repUnrepairable;   // totals for NetSummary
static int g_repGame = -1;                              // RepGameOk's answer: -1 not checked yet

// Sends a repair message in kRepChunk pieces (always at least one, so an empty RNAK or
// RMISSED still goes out), each tagged with the message's frame.
static void RepSend(int type, uint32_t frame, const uint8_t* d, size_t n, bool reliable) {
    if (!g_netT) return;
    uint16_t total = (uint16_t)((n + kRepChunk - 1) / kRepChunk);
    if (total == 0) total = 1;
    for (uint16_t s = 0; s < total; s++) {
        NetRepChunk c = {};
        c.h = NetHeader((uint8_t)type);
        c.frame = frame; c.seq = s; c.total = total;
        size_t off = (size_t)s * kRepChunk;
        c.len = (uint16_t)(n > off ? min((size_t)kRepChunk, n - off) : 0);
        if (c.len) memcpy(c.data, d + off, c.len);
        int len = (int)offsetof(NetRepChunk, data) + c.len;
        if (reliable) g_netT->SendReliable(&c, len); else g_netT->Send(&c, len);
        g_netPacketsOut++;
    }
    g_netLastSend = GetTickCount64();
}

// 32-bit FNV-1a, chainable through h. The per-frame RNG check (net_determinism.cpp) uses it too.
uint32_t RepFnv(const void* p, size_t n, uint32_t h = 2166136261u) {
    const uint8_t* q = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) { h ^= q[i]; h *= 16777619u; }
    return h;
}

// The real game's code where the repair writes and calls (the stand-in tests
// have none of it: there only the rand() streams take part).
// Checked once: Transform_Sync's first bytes (0x56dab0), the live-actor lookup's reference
// to the actor registry, the game timer, and the three Bullet vtable slots RepBodyEpilogue
// calls through.
static bool RepGameOk() {
    if (g_repGame < 0) {
        static const BYTE sync[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xB9, 0xC0, 0x00, 0x00, 0x00 };
        bool ok = BytesAt(0x56dab0, sync, sizeof sync) && RipRefersTo(0x558403, 3, 7, 0xb2bb90) && GameTimer() != NULL;
        __try {
            void** pc = (void**)(ExeBase() + 0x81f200);   // btHashedOverlappingPairCache vtable
            void** bp = (void**)(ExeBase() + 0x81f298);   // btDbvtBroadphase vtable
            ok = ok && (BYTE*)pc[9] == ExeBase() + 0x6d4ef0 && (BYTE*)bp[3] == ExeBase() + 0x6ef2f0 && (BYTE*)bp[9] == ExeBase() + 0x6e0b10;
        } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        g_repGame = ok ? 1 : 0;
        Log("repair: in-place desync repair %s", ok ? "covers the game timer, random generators, rand() streams, physics bodies and actors"
                                                    : "covers this DLL's rand() streams only (game code not found: stand-in test?)");
    }
    return g_repGame == 1;
}
// The Bullet physics world (the physics manager at exe+0xb275c8, its world at +0x1168) and
// the world's collision-object array (count +0x0c, array +0x18). NULL with no level loaded or
// an implausible count.
static BYTE* RepWorld(int* n, BYTE*** arr) {
    *n = 0; *arr = NULL;
    __try {
        if (BYTE* phys = *(BYTE**)(ExeBase() + 0xb275c8))
            if (BYTE* world = *(BYTE**)(phys + 0x1168)) {
                int c = *(int*)(world + 0x0c);
                if (c < 0 || c > 20000) return NULL;
                *n = c; *arr = *(BYTE***)(world + 0x18);
                return world;
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return NULL;
}
// Live actors, sorted by key (the registry: 256 buckets, node {object +0, key +8, next +0x18}).
struct RepActor { uint64_t key; BYTE* actor; };
// Collects them in registry order (callers sort with RepActorLess); an actor counts as live
// when the game's own lookup by key (exe+0x558400) finds it.
static int RepCollectActors(RepActor* out, int cap) {
    int n = 0;
    __try {
        BYTE* b = ExeBase();
        typedef BYTE* (*PFN_Live)(uint64_t);
        PFN_Live live = (PFN_Live)(b + 0x558400);
        BYTE** buckets = (BYTE**)(b + 0xb2bb90);
        for (int k = 0; k < 256; k++)
            for (BYTE* node = buckets[k]; node && n < cap; node = *(BYTE**)(node + 0x18)) {
                BYTE* a = *(BYTE**)node;
                uint64_t key = *(uint64_t*)(node + 8);
                if (a && live(key) == a) out[n++] = { key, a };
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}
static bool RepActorLess(const RepActor& a, const RepActor& b) { return a.key < b.key; }

// Capture an item's bytes (what is hashed, sent and written). Returns length, 0 = absent.
// An MT instance: its 624 state words (0x9c0 bytes), its cursor as an index (the instance
// holds an address, which differs per process) and the words-left count at +0x9c8.
static int RepCaptureMt(uint8_t* inst, uint8_t* out) {
    if (!inst) return 0;
    __try {
        memcpy(out, inst, 0x9c0);
        uint32_t idx = (uint32_t)((*(uint8_t**)(inst + 0x9c0) - inst) / 4);
        memcpy(out + 0x9c0, &idx, 4);
        memcpy(out + 0x9c4, inst + 0x9c8, 4);
        return 0x9c8;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
// A Bullet collision object (btCollisionObject, or a btRigidBody with velocities): its pool
// slot (from the game's body record at +0xe8, to check it's the same body on both PCs), what
// it is, then its moving state. Static objects (collision flag 1 at +0xc8) aren't items.
static int RepCaptureBody(BYTE* o, uint8_t* out) {
    __try {
        if (!o || (*(uint32_t*)(o + 0xc8) & 1)) return 0;    // static: never moves
        int len = 0;
        BYTE* rec = *(BYTE**)(o + 0xe8);
        uint16_t slot = rec ? *(uint16_t*)rec : 0xFFFF;
        BYTE* proxy = *(BYTE**)(o + 0xb0);
        uint8_t fl = (*(int*)(o + 0xf0) == 2 ? 1 : 0) | (proxy ? 2 : 0);   // 1: rigid body (internalType 2), 2: in the broadphase
        memcpy(out + len, &slot, 2); len += 2;
        out[len++] = fl;
        memcpy(out + len, o + 0x10, 0xa0); len += 0xa0;      // transform, interpolation transform, interpolation velocities
        memcpy(out + len, o + 0xd4, 8); len += 8;            // activation state, sleep timer
        if (fl & 1) { memcpy(out + len, o + 0x110, 0x50); len += 0x50; }   // inverse world inertia, linear, angular velocity
        if (fl & 2) { memcpy(out + len, proxy + 0x20, 0x20); len += 0x20; }   // broadphase AABB
        return len;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
// The transform component's 0x60-byte state record (decomp session): +0x80 position,
// +0x90/+0xa0/+0xb0 orientation and scale rows, +0xc0 linear and +0xd0 angular velocity,
// each a vec4 whose w lane (+0xc etc.) some paths never set. Only the x, y, z lanes
// are captured (6 x 12 bytes), plus physicsFlags (+0x3e8, simulation-owned). Not the
// scene transform's cached position *(+0x200)+0x48: the render traversal refreshes it
// lazily, so it differs per PC with own screens while the game matches.
static const int kActorLanes = 6, kActorCap = kActorLanes * 12 + 4;
static int RepCaptureActor(BYTE* a, uint8_t* out) {
    __try {
        int len = 0;
        for (int r = 0; r < kActorLanes; r++) { memcpy(out + len, a + 0x80 + r * 0x10, 12); len += 12; }
        memcpy(out + len, a + 0x3e8, 4); len += 4;
        return len;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
// Every rand() call site's own stream (sim_determinism.cpp's rand isolation), then the
// fallback stream, 4 bytes each.
static int RepCaptureRand(uint8_t* out) {
    int len = 0;
    for (int i = 0; i < g_randSiteCount; i++) { LONG s = g_randSites[i].state; memcpy(out + len, &s, 4); len += 4; }
    LONG fb = g_randFallbackState;
    memcpy(out + len, &fb, 4); len += 4;
    return len;
}
// The MT instance for an item kind: the engine's global one, or this DLL's UI, dialog and
// cosmetic ones (sim_determinism.cpp).
static uint8_t* RepMtInst(int kind) {
    switch (kind) {
    case RK_MT: return g_mtGlobalPtr ? (uint8_t*)*g_mtGlobalPtr : NULL;
    case RK_MT_UI: return g_uiMt;
    case RK_MT_DLG: return g_dlgMt;
    case RK_MT_COS: return g_cosMt;
    }
    return NULL;
}
// One item's bytes into out (cap >= 0xa00). Body ids are indices into the
// world's object array; actor ids are the low 32 bits of the entity key.
static int RepCapture(uint8_t kind, uint32_t id, uint8_t* out, BYTE** arr, int nb, const RepActor* acts, int na) {
    switch (kind) {
    case RK_TIMER: {
        BYTE* t = GameTimer();
        if (!t) return 0;
        __try { memcpy(out, t, 0x68); return 0x68; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
    case RK_MT: case RK_MT_UI: case RK_MT_DLG: case RK_MT_COS: return RepCaptureMt(RepMtInst(kind), out);
    case RK_RAND: return RepCaptureRand(out);
    case RK_BODY: return arr && (int)id < nb ? RepCaptureBody(arr[id], out) : 0;
    case RK_ACTOR:
        for (int i = 0; i < na; i++) if ((uint32_t)acts[i].key == id) return RepCaptureActor(acts[i].actor, out);
        return 0;
    }
    return 0;
}

// Both PCs, frame T: drop the body's contact pairs (cached impulses warm-start
// the next solve); the client also moves its broadphase AABB to the host's.
// The world's dispatcher is at +0x30 and its broadphase at +0x78. Calls go through the
// broadphase's vtable (slot 3 takes proxy, AABB min, AABB max, dispatcher; slot 9 returns
// the pair cache) and the pair cache's (slot 9 takes proxy, dispatcher): the slots
// RepGameOk checked.
static void RepBodyEpilogue(BYTE* o, const float* aabb) {
    __try {
        int n; BYTE** arr;
        BYTE* world = RepWorld(&n, &arr);
        BYTE* proxy = *(BYTE**)(o + 0xb0);
        if (!world || !proxy) return;
        BYTE* disp = *(BYTE**)(world + 0x30);
        BYTE* bp = *(BYTE**)(world + 0x78);
        void** bpvt = *(void***)bp;
        if (aabb) ((void (*)(BYTE*, BYTE*, const float*, const float*, BYTE*))bpvt[3])(bp, proxy, aabb, aabb + 4, disp);
        BYTE* pc = ((BYTE* (*)(BYTE*))bpvt[9])(bp);
        if (pc) ((void (*)(BYTE*, BYTE*, BYTE*))(*(void***)pc)[9])(pc, proxy, disp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// Writes a captured MT back (cursor rebuilt from the index) under g_mtLock, so no draw on
// another thread sees half a state. Refuses a wrong length or an out-of-range index.
static bool RepApplyMt(uint8_t* inst, const uint8_t* v, int len) {
    if (!inst || len != 0x9c8) return false;
    uint32_t idx; int32_t left;
    memcpy(&idx, v + 0x9c0, 4); memcpy(&left, v + 0x9c4, 4);
    if (idx > 624 || left < 0 || left > 624) return false;
    AcquireSRWLockExclusive(&g_mtLock);
    memcpy(inst, v, 0x9c0);
    *(uint8_t**)(inst + 0x9c0) = inst + idx * 4;
    *(int32_t*)(inst + 0x9c8) = left;
    ReleaseSRWLockExclusive(&g_mtLock);
    return true;
}
// Writes a captured body back, only onto the same body (same pool slot) of the same shape
// (rigid or not, in the broadphase or not); then the AABB and contact-pair epilogue.
static bool RepApplyBody(BYTE* o, const uint8_t* v, int len) {
    __try {
        if (!o || len < 3 + 0xa0 + 8) return false;
        BYTE* rec = *(BYTE**)(o + 0xe8);
        uint16_t slot; memcpy(&slot, v, 2);
        if ((rec ? *(uint16_t*)rec : 0xFFFF) != slot) return false;   // not the same body: leave it
        uint8_t fl = v[2];
        BYTE* proxy = *(BYTE**)(o + 0xb0);
        uint8_t mine = (*(int*)(o + 0xf0) == 2 ? 1 : 0) | (proxy ? 2 : 0);
        if (fl != mine || len != 3 + 0xa0 + 8 + ((fl & 1) ? 0x50 : 0) + ((fl & 2) ? 0x20 : 0)) return false;
        int p = 3;
        memcpy(o + 0x10, v + p, 0xa0); p += 0xa0;
        memcpy(o + 0xd4, v + p, 8); p += 8;
        if (fl & 1) { memcpy(o + 0x110, v + p, 0x50); p += 0x50; }
        float aabb[8] = {};
        if (fl & 2) { memcpy(aabb, v + p, 0x20); RepBodyEpilogue(o, aabb); }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Writes the six x, y, z lanes and physicsFlags back, then lets the scene node follow.
static bool RepApplyActor(BYTE* a, const uint8_t* v, int len) {
    __try {
        if (!a || len != kActorCap) return false;
        for (int r = 0; r < kActorLanes; r++) memcpy(a + 0x80 + r * 0x10, v + r * 12, 12);
        memcpy(a + 0x3e8, v + kActorLanes * 12, 4);
        ((void (*)(void*))(ExeBase() + 0x56dab0))(a + 0x50);   // Transform_Sync: the scene node follows
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Host, frame T: the same Transform_Sync the client makes (it bumps a scene
// version counter, so both PCs must make it) and the same AABB call.
static void RepHostActorSync(BYTE* a) {
    __try { if (a) ((void (*)(void*))(ExeBase() + 0x56dab0))(a + 0x50); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void RepHostBodySync(BYTE* o) {
    float aabb[8] = {};
    bool have = false;
    __try {
        BYTE* proxy = o ? *(BYTE**)(o + 0xb0) : NULL;
        if (proxy) { memcpy(aabb, proxy + 0x20, 0x20); have = true; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (have) RepBodyEpilogue(o, aabb);
}

// The two characters the players are controlling (PlayerManager +0x1a0 + slot*0x58; a
// switched-out avatar isn't there). Every actor is a "follower" (noted, never copied:
// copying animation-driven pieces made things worse), except these: the simulation reads
// the players' positions everywhere (contacts, AI perception, "stay near home" region
// tests, facing tests), so a player 1 mm out (Jessie in the Toy Box briefing, netplay_47)
// fed random rolls on one PC only. They're copied like any other differing item.
static bool RepIsPlayerAvatar(BYTE* a) {
    __try {
        BYTE* pm = *(BYTE**)(ExeBase() + 0xb2ef50);
        for (int slot = 0; pm && slot < 2; slot++)
            if (a && *(BYTE**)(pm + 0x1a0 + slot * 0x58) == a) return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// A kinematic body (collision flag 2: moved by the game, not simulated) is a follower (cls 1).
static bool RepBodyKinematic(BYTE* o) {
    __try { return o && (*(uint32_t*)(o + 0xc8) & 2) != 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// This PC's digest at frame f: header, then (kind, id, hash) per item.
// Built on both PCs at every fingerprint frame (RepStep); the host sends its own, the client
// compares the two (RepCompare).
static void RepBuildDigest(uint32_t f, std::vector<uint8_t>& out) {
    static uint8_t buf[0xa00];
    std::vector<RepItem> items;
    // What can't be copied goes into the head as hashes: counters, entity keys, which bodies
    // and which actors exist. Everything copyable becomes an item.
    uint32_t fCounters = 2166136261u, fKeys = 2166136261u, fSlots = 2166136261u, fActors = 2166136261u;
    fCounters = RepFnv(&g_engineFrames, 8, fCounters);
    fCounters = RepFnv(&g_frameStartUs, 8, fCounters);
    fCounters = RepFnv(&g_spawnSeq, 4, fCounters);
    auto add = [&](uint8_t kind, uint8_t cls, uint32_t id, int len) { if (len > 0) items.push_back({ kind, cls, id, RepFnv(buf, len) }); };
    add(RK_RAND, 0, 0, RepCaptureRand(buf));
    int nb = 0, na = 0;
    BYTE** arr = NULL;
    static RepActor acts[8192];
    if (RepGameOk()) {                                  // the real game (not a stand-in test)
        // The entity-key counter (verified as in NetResetForStart), the timer, the four MTs,
        // every body by index, every live actor by key (sorted, so both PCs list them alike).
        if (RipRefersTo(0x23ccd, 3, 7, 0x9575c8)) { uint64_t keys = *(volatile uint64_t*)(ExeBase() + 0x9575c8); fKeys = RepFnv(&keys, 8, fKeys); }
        add(RK_TIMER, 0, 0, RepCapture(RK_TIMER, 0, buf, NULL, 0, NULL, 0));
        for (int k = RK_MT; k <= RK_MT_COS; k++) add((uint8_t)k, 0, 0, RepCaptureMt(RepMtInst(k), buf));
        RepWorld(&nb, &arr);
        for (int i = 0; arr && i < nb; i++) {
            int len = RepCaptureBody(arr[i], buf);
            uint16_t slot = len ? *(uint16_t*)buf : 0xFFFF;
            fSlots = RepFnv(&slot, 2, fSlots);               // which bodies exist, in which order
            add(RK_BODY, RepBodyKinematic(arr[i]) ? 1 : 0, (uint32_t)i, len);
        }
        na = RepCollectActors(acts, _countof(acts));
        std::sort(acts, acts + na, RepActorLess);
        for (int i = 0; i < na; i++) {
            fActors = RepFnv(&acts[i].key, 8, fActors);      // which actors are live
            add(RK_ACTOR, RepIsPlayerAvatar(acts[i].actor) ? 0 : 1, (uint32_t)acts[i].key, RepCaptureActor(acts[i].actor, buf));
        }
    }
    RepDigestHead h = {};
    h.frame = f; h.fixedCounters = fCounters; h.fixedKeys = fKeys; h.fixedSlots = fSlots; h.fixedActors = fActors;
    h.bodies = (uint32_t)nb; h.actors = (uint32_t)na; h.spawnSeq = g_spawnSeq; h.items = (uint32_t)items.size();
    h.engineFrames = g_engineFrames; h.frameStartUs = (uint64_t)g_frameStartUs; h.fingerprint = NetFingerprint();
    out.resize(sizeof h + items.size() * sizeof(RepItem));
    memcpy(out.data(), &h, sizeof h);
    if (!items.empty()) memcpy(out.data() + sizeof h, items.data(), items.size() * sizeof(RepItem));
}

// Item kinds as the log names them.
static const char* RepKindName(int k) {
    switch (k) {
    case RK_TIMER: return "game timer"; case RK_MT: return "engine RNG"; case RK_MT_UI: return "UI RNG";
    case RK_MT_DLG: return "dialog RNG"; case RK_MT_COS: return "cosmetic RNG"; case RK_RAND: return "rand() streams";
    case RK_BODY: return "body"; case RK_ACTOR: return "actor";
    }
    return "?";
}

// "instance/definition" names of an actor (string pointers at +0x148 and +0x150), for the log.
static void RepActorName(BYTE* a, char* nm, size_t cap) {
    __try {
        const char* s = *(const char**)(a + 0x148);
        const char* c = *(const char**)(a + 0x150);
        _snprintf_s(nm, cap, _TRUNCATE, "%s/%s", s ? s : "?", c ? c : "?");
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// Client: ask the host for the items in req (12-byte head, then kind + id per item),
// to be copied at T = now + lead. Returns T.
// The head is T, the frame the differences were found at, and the item count. Sent reliably,
// and kept for re-sending (RepStep).
uint32_t g_rngQuietUntil;                          // per-frame RNG check: a round copies at this frame
static uint32_t RepRequest(std::vector<uint8_t>& req, uint32_t from, uint32_t n, uint32_t lead) {
    uint32_t T = g_netFrame + lead;
    memcpy(req.data(), &T, 4); memcpy(req.data() + 4, &from, 4); memcpy(req.data() + 8, &n, 4);
    g_repPendingT = T;
    g_repPendingItems = req;
    g_repMissedT = 0;
    g_repValIn.Reset(0, 0);
    RepSend(NET_RREQ, T, req.data(), req.size(), true);
    g_rngQuietUntil = T;                                      // differences before T are this round's to fix
    return T;
}

// Client: compare the host's digest with this PC's of the same frame.
static void RepCompare(const std::vector<uint8_t>& host, const std::vector<uint8_t>& mine) {
    if (host.size() < sizeof(RepDigestHead) || mine.size() < sizeof(RepDigestHead)) return;
    RepDigestHead hh, mh;
    memcpy(&hh, host.data(), sizeof hh); memcpy(&mh, mine.data(), sizeof mh);
    if (hh.frame != mh.frame || host.size() != sizeof hh + (size_t)hh.items * sizeof(RepItem) ||
        mine.size() != sizeof mh + (size_t)mh.items * sizeof(RepItem)) return;
    // A part that can't be copied differs: the session ends.
    if (hh.fixedCounters != mh.fixedCounters || hh.fixedKeys != mh.fixedKeys || hh.fixedSlots != mh.fixedSlots ||
        hh.fixedActors != mh.fixedActors) {
        g_repUnrepairable++;
        Log("repair: frame %u: can't be repaired in place; differing: %s%s%s%s (host %u bodies, %u actors, spawn seq %u, "
            "engine frames %llu, frame start %llu; this PC %u, %u, %u, %llu, %llu)", hh.frame,
            hh.fixedCounters != mh.fixedCounters ? "frame counters/spawn seq " : "", hh.fixedKeys != mh.fixedKeys ? "entity-key counter " : "",
            hh.fixedSlots != mh.fixedSlots ? "the set of bodies " : "", hh.fixedActors != mh.fixedActors ? "the set of live actors" : "",
            hh.bodies, hh.actors, hh.spawnSeq, (unsigned long long)hh.engineFrames, (unsigned long long)hh.frameStartUs,
            mh.bodies, mh.actors, mh.spawnSeq, (unsigned long long)mh.engineFrames, (unsigned long long)mh.frameStartUs);
        NetEnd("the two games stopped matching in a way the repair can't copy (a spawn on one PC only, or frame counts differ)");
        return;
    }
    const RepItem* hi = (const RepItem*)(host.data() + sizeof hh);
    const RepItem* mi = (const RepItem*)(mine.data() + sizeof mh);
    std::unordered_map<uint64_t, uint32_t> myHash;           // (kind, id) -> this PC's hash
    myHash.reserve(mh.items * 2);
    for (uint32_t j = 0; j < mh.items; j++) myHash[(uint64_t)mi[j].kind << 32 | mi[j].id] = mi[j].hash;
    // Followers (kinematic bodies, actor transforms) are only noted: one-PC Toy Box test
    // 2026-10-25, build 40: copying 385 of them on a real desync knocked the games apart
    // again (a body set mismatch 90 frames later), while the RNG alone would have fixed it.
    // repair_followers=1 copies them on a real desync anyway (experiments).
    bool realDesync = hh.fingerprint != mh.fingerprint;
    bool copyFollowers = realDesync && g_net.repairFollowers;
    // Names for the log (this PC's own bodies and actors).
    static RepActor acts[8192];
    int nb = 0, na = 0; BYTE** arr = NULL;
    if (RepGameOk()) { RepWorld(&nb, &arr); na = RepCollectActors(acts, _countof(acts)); }
    static int detailRounds, changeLogs;
    static uint32_t lastNoted;
    std::vector<uint8_t> req;
    uint32_t n = 0, noted = 0, counts[9] = {};
    char first[160] = "";
    static char list[1600];
    list[0] = 0;
    std::vector<uint32_t> probeIds;                          // noted actors to compare word by word (once a session)
    req.resize(12);
    // Every host item this PC lacks or hashes differently: core items (and, with
    // repair_followers=1 on a real desync, followers) go on the request; the rest are only
    // noted. The first ones are named in the log.
    for (uint32_t i = 0; i < hh.items; i++) {
        auto it = myHash.find((uint64_t)hi[i].kind << 32 | hi[i].id);
        if (it != myHash.end() && it->second == hi[i].hash) continue;
        bool copy = hi[i].cls == 0 || copyFollowers;
        if (strlen(list) < sizeof list - 80) {
            char nm[48] = "";
            if (hi[i].kind == RK_BODY && arr && (int)hi[i].id < nb) BodyActorName(arr[hi[i].id], nm, sizeof nm);
            if (hi[i].kind == RK_ACTOR)
                for (int a = 0; a < na; a++) if ((uint32_t)acts[a].key == hi[i].id) { RepActorName(acts[a].actor, nm, sizeof nm); break; }
            size_t len = strlen(list);
            _snprintf_s(list + len, sizeof list - len, _TRUNCATE, "%s%s %u%s%s%s%s", len ? ", " : "", RepKindName(hi[i].kind), hi[i].id,
                        nm[0] ? " " : "", nm, hi[i].cls ? " (follower)" : "", copy ? "" : " [noted]");
        }
        if (!copy) {
            noted++;
            if (hi[i].kind == RK_ACTOR && probeIds.size() < 8) probeIds.push_back(hi[i].id);
            continue;
        }
        if (n >= 400) break;                                  // one round copies at most 400 items
        uint8_t e[5]; e[0] = hi[i].kind; memcpy(e + 1, &hi[i].id, 4);
        req.insert(req.end(), e, e + 5);
        n++;
        if (hi[i].kind < 9) counts[hi[i].kind]++;
        if (!first[0]) _snprintf_s(first, sizeof first, _TRUNCATE, "%s %u", RepKindName(hi[i].kind), hi[i].id);
    }
    // The named list: the first 6 rounds, every repair, and whenever the noted count changes.
    bool changed = noted != lastNoted && changeLogs < 40;
    if ((n || noted) && (detailRounds < 6 || n || changed)) {
        if (detailRounds < 6) detailRounds++; else if (changed && !n) changeLogs++;
        Log("repair: frame %u: fingerprints %s; %u differ (%u noted): %s", hh.frame, realDesync ? "DIFFER" : "match", n + noted, noted, list);
    }
    lastNoted = noted;
    // Whenever a noted actor starts differing that hasn't been probed this session (at most 40
    // rounds): ask the host for its values too, marked as probes (kind | RK_PROBE). Nothing is
    // written; the client logs which words differ. (Once a session was used up by the board
    // hub's pieces at frame 60, so a later split, Hamm and Jessie in the Toy Box, had none.)
    static int probedSession = -1, probeRounds;
    static std::vector<uint32_t> probedIds;
    if (probedSession != g_netSessions) { probedSession = g_netSessions; probeRounds = 0; probedIds.clear(); }
    uint32_t probes = 0;
    std::vector<uint32_t> fresh;
    for (uint32_t id : probeIds)
        if (std::find(probedIds.begin(), probedIds.end(), id) == probedIds.end()) fresh.push_back(id);
    if (!fresh.empty() && probeRounds < 40 && !g_repPendingT && n + fresh.size() <= 400) {
        probeRounds++;
        for (uint32_t id : fresh) {
            uint8_t e[5]; e[0] = RK_ACTOR | RK_PROBE; memcpy(e + 1, &id, 4);
            req.insert(req.end(), e, e + 5);
            probedIds.push_back(id);
            probes++;
        }
        n += probes;
    }
    if (!n) {
        if (noted) Log("repair: frame %u: %u follower items differ%s; noted, not copied", hh.frame, noted,
                       realDesync ? " (the game differs too, but followers aren't copied: repair_followers=0)" : " but the game itself matches");
        return;
    }
    if (probes) Log("repair: frame %u: asking the host for %u noted actors' values to compare (nothing is written)", hh.frame, probes);
    if (g_repPendingT) return;                                // a repair is already on its way
    // Ask for the copy (and any probes), repair_lead frames ahead.
    uint32_t T = RepRequest(req, hh.frame, n, (uint32_t)g_net.repairLead);
    Log("repair: frame %u: %u items differ from the host's (first: %s; %u bodies, %u actors, %u RNG/timer%s%s); copying them at frame %u",
        hh.frame, n - probes, first[0] ? first : "none", counts[RK_BODY], counts[RK_ACTOR], n - probes - counts[RK_BODY] - counts[RK_ACTOR],
        noted ? "; followers left alone" : "", probes ? "; plus noted actors to compare" : "", T);
}

static void RepLogActorState(const char* who, uint32_t f, uint32_t id, BYTE* a);   // (probe diagnostics, below)
// Host, frame T: the values of the requested items, then the same epilogue.
// The values message (RVAL): T and the item count (8 bytes), then per item its kind, id and
// length (7 bytes) and the captured bytes. Kept in g_repSentVals for re-asks.
static void RepHostCapture(uint32_t f) {
    if (g_repHostItems.size() < 12) { g_repHostT = 0; return; }
    uint32_t n; memcpy(&n, g_repHostItems.data() + 8, 4);
    if (g_repHostItems.size() < 12 + (size_t)n * 5) { g_repHostT = 0; return; }
    static uint8_t buf[0xa00];
    static RepActor acts[8192];
    int nb = 0, na = 0; BYTE** arr = NULL;
    if (RepGameOk()) { RepWorld(&nb, &arr); na = RepCollectActors(acts, _countof(acts)); }
    std::vector<uint8_t> vals(8);
    uint32_t sent = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t kind = g_repHostItems[12 + i * 5];
        uint32_t id; memcpy(&id, &g_repHostItems[12 + i * 5 + 1], 4);
        int len = RepCapture(kind & ~RK_PROBE, id, buf, arr, nb, acts, na);
        uint8_t e[7]; e[0] = kind; memcpy(e + 1, &id, 4); uint16_t l = (uint16_t)len; memcpy(e + 5, &l, 2);
        vals.insert(vals.end(), e, e + 7);
        if (len) vals.insert(vals.end(), buf, buf + len);
        sent++;
        if (kind & RK_PROBE) {                                // probe: the client only compares, so no epilogue here either
            if ((kind & ~RK_PROBE) == RK_ACTOR)
                for (int a = 0; a < na; a++) if ((uint32_t)acts[a].key == id) { RepLogActorState("host", f, id, acts[a].actor); break; }
            continue;
        }
        // The same calls the client makes after writing: AABB update + contact-pair drop, Transform_Sync.
        if (kind == RK_BODY && len && arr && (int)id < nb && (buf[2] & 2)) RepHostBodySync(arr[id]);
        if (kind == RK_ACTOR && len)
            for (int a = 0; a < na; a++) if ((uint32_t)acts[a].key == id) { RepHostActorSync(acts[a].actor); break; }
    }
    memcpy(vals.data(), &f, 4); memcpy(vals.data() + 4, &sent, 4);
    g_repSentT = f;
    g_repSentVals = vals;
    RepSend(NET_RVAL, f, vals.data(), vals.size(), true);
    g_repRounds++;
    Log("repair: frame %u: sent the other player %u items (%zu bytes)", f, sent, vals.size());
    g_repHostT = 0;
}

// Before writing a body (first 10 per session): which parts differ, with the origin values,
// so a body that always differs at the first digest (slot 16 on the level-select map) can be
// traced to its source instead of being repaired every time.
static void RepLogBodyDiff(uint32_t f, uint32_t id, BYTE* o, const uint8_t* host, int len) {
    static int logs, sess = -1;
    if (sess != g_netSessions) { sess = g_netSessions; logs = 0; }
    if (logs >= 10) return;
    uint8_t mine[0x200] = {};
    int ml = RepCaptureBody(o, mine);
    if (ml != len || ml < 3 + 0xa0 + 8) { logs++; Log("repair: body %u: not comparable (lengths %d/%d)", id, len, ml); return; }
    logs++;
    struct Part { const char* name; int off, size; } parts[] = {
        { "basis", 3, 0x30 }, { "origin", 3 + 0x30, 0x10 }, { "interp transform", 3 + 0x40, 0x40 },
        { "interp velocities", 3 + 0x80, 0x20 }, { "activation", 3 + 0xa0, 4 }, { "sleep timer", 3 + 0xa4, 4 } };
    char out[600] = "", nm[48] = "";
    for (const Part& p : parts)
        if (memcmp(host + p.off, mine + p.off, p.size)) {
            size_t l = strlen(out);
            _snprintf_s(out + l, sizeof out - l, _TRUNCATE, "%s%s", l ? ", " : "", p.name);
        }
    int q = 3 + 0xa0 + 8;
    if ((host[2] & 1) && len >= q + 0x50) {
        if (memcmp(host + q, mine + q, 0x30)) { size_t l = strlen(out); _snprintf_s(out + l, sizeof out - l, _TRUNCATE, "%sinverse inertia", l ? ", " : ""); }
        if (memcmp(host + q + 0x30, mine + q + 0x30, 0x10)) { size_t l = strlen(out); _snprintf_s(out + l, sizeof out - l, _TRUNCATE, "%slinear velocity", l ? ", " : ""); }
        if (memcmp(host + q + 0x40, mine + q + 0x40, 0x10)) { size_t l = strlen(out); _snprintf_s(out + l, sizeof out - l, _TRUNCATE, "%sangular velocity", l ? ", " : ""); }
        q += 0x50;
    }
    if ((host[2] & 2) && len >= q + 0x20 && memcmp(host + q, mine + q, 0x20)) { size_t l = strlen(out); _snprintf_s(out + l, sizeof out - l, _TRUNCATE, "%sbroadphase AABB", l ? ", " : ""); }
    float ho[3], mo[3];
    memcpy(ho, host + 3 + 0x30, 12); memcpy(mo, mine + 3 + 0x30, 12);
    uint32_t hf = 0, mf = 0; memcpy(&hf, host + 3 + 0xa0, 4); memcpy(&mf, mine + 3 + 0xa0, 4);
    BodyActorName(o, nm, sizeof nm);
    uint16_t slot; memcpy(&slot, host, 2);
    Log("repair: frame %u: body %u (slot %u, %s) differs in: %s; origin host (%.4f %.4f %.4f) / this PC (%.4f %.4f %.4f), activation %u/%u",
        f, id, slot, nm, out[0] ? out : "nothing now", ho[0], ho[1], ho[2], mo[0], mo[1], mo[2], hf, mf);
}
// Probe, both PCs at frame T: the state that decides whether the actor pulls its position from
// its body (decomp session): activated +0x188/+0x189, activationState +0x420, the Performance
// Manager node +0x168, and the motion state's graphics origin (body+0x208 -> +0x40) of the
// actor's own body, if any. Comparing the two PCs' lines shows which gate differs.
// RepActorBody finds that body: the one whose record is owned (+0x14 low bits = 1) by a
// component carrying the actor's key (component +0x58, actor +0x38). NULL if none.
static BYTE* RepActorBody(BYTE* actor) {
    int n; BYTE** arr;
    if (!RepWorld(&n, &arr) || !arr) return NULL;
    __try {
        uint64_t key = *(uint64_t*)(actor + 0x38);
        for (int i = 0; i < n; i++) {
            BYTE* o = arr[i];
            BYTE* rec = o ? *(BYTE**)(o + 0xe8) : NULL;
            if (!rec || (*(BYTE*)(rec + 0x14) & 3) != 1) continue;
            BYTE* h = *(BYTE**)(rec + 8);
            BYTE* comp = h ? *(BYTE**)(h + 0x10) : NULL;
            if (comp && *(uint64_t*)(comp + 0x58) == key) return o;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return NULL;
}
static void RepLogActorState(const char* who, uint32_t f, uint32_t id, BYTE* a) {
    char nm[96] = "";
    RepActorName(a, nm, sizeof nm);
    BYTE* o = RepActorBody(a);
    uint32_t b188 = 0, b189 = 0, st = 0, ms[3] = {}, bo[3] = {}, bflags = 0, bact = 0;
    void* pm = NULL;
    __try {
        b188 = *(BYTE*)(a + 0x188); b189 = *(BYTE*)(a + 0x189); st = *(uint32_t*)(a + 0x420); pm = *(void**)(a + 0x168);
        if (o) {
            memcpy(bo, o + 0x40, 12); bflags = *(uint32_t*)(o + 0xc8); bact = *(uint32_t*)(o + 0xd4);
            if (BYTE* m = *(BYTE**)(o + 0x208)) memcpy(ms, m + 0x40, 12);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    Log("repair: probe state (%s) frame %u actor %u %s: activated %u/%u activationState %u pmNode %s; body %s flags %u act %u "
        "origin %08x%08x%08x motionState origin %08x%08x%08x", who, f, id, nm, b188, b189, st, pm ? "yes" : "no",
        o ? "yes" : "none", bflags, bact, bo[0], bo[1], bo[2], ms[0], ms[1], ms[2]);
}
// Probe: name the words of a noted actor's record that differ from the host's.
static void RepProbeActor(uint32_t f, uint32_t id, BYTE* a, const uint8_t* host, int len) {
    static const char* lane[] = { "pos", "rot0", "rot1", "rot2", "vel", "angvel" };
    uint8_t mine[kActorCap] = {};
    char nm[96] = "", out[700] = "";
    RepActorName(a, nm, sizeof nm);
    if (len != kActorCap || RepCaptureActor(a, mine) != kActorCap) { Log("repair: probe actor %u %s: not comparable", id, nm); return; }
    int words = 0;
    for (int w = 0; w < kActorCap / 4; w++) {
        uint32_t hv, mv;
        memcpy(&hv, host + w * 4, 4); memcpy(&mv, mine + w * 4, 4);
        if (hv == mv) continue;
        words++;
        size_t l = strlen(out);
        if (w < kActorLanes * 3) {
            float hf, mf; memcpy(&hf, &hv, 4); memcpy(&mf, &mv, 4);
            _snprintf_s(out + l, sizeof out - l, _TRUNCATE, "%s%s.%c %g/%g", l ? ", " : "", lane[w / 3], "xyz"[w % 3], hf, mf);
        } else {
            _snprintf_s(out + l, sizeof out - l, _TRUNCATE, "%sphysicsFlags %08x/%08x", l ? ", " : "", hv, mv);
        }
    }
    Log("repair: probe frame %u actor %u %s: %d words differ (host/this PC): %s", f, id, nm, words, words ? out : "none now");
    RepLogActorState("this PC", f, id, a);
}
// Client, frame T: write the host's values.
static void RepClientApply(uint32_t f, const std::vector<uint8_t>& vals) {
    if (vals.size() < 8) return;
    uint32_t n; memcpy(&n, vals.data() + 4, 4);
    static RepActor acts[8192];
    int nb = 0, na = 0; BYTE** arr = NULL;
    if (RepGameOk()) { RepWorld(&nb, &arr); na = RepCollectActors(acts, _countof(acts)); }
    size_t p = 8;
    uint32_t ok = 0, skipped = 0;
    for (uint32_t i = 0; i < n && p + 7 <= vals.size(); i++) {
        uint8_t kind = vals[p];
        uint32_t id; memcpy(&id, &vals[p + 1], 4);
        uint16_t len; memcpy(&len, &vals[p + 5], 2);
        p += 7;
        if (p + len > vals.size()) break;
        const uint8_t* v = &vals[p];
        p += len;
        bool done = false;
        if (kind == (RK_ACTOR | RK_PROBE)) {                  // compare only: which words of the record differ
            for (int a = 0; a < na; a++) if ((uint32_t)acts[a].key == id) { RepProbeActor(f, id, acts[a].actor, v, len); break; }
            continue;
        }
        switch (kind) {
        case RK_TIMER:
            if (BYTE* t = GameTimer()) if (len == 0x68) { memcpy(t, v, 0x68); done = true; }
            break;
        case RK_MT: case RK_MT_UI: case RK_MT_DLG: case RK_MT_COS: done = RepApplyMt(RepMtInst(kind), v, len); break;
        case RK_RAND:
            if (len == (g_randSiteCount + 1) * 4) {
                for (int s = 0; s < g_randSiteCount; s++) { LONG x; memcpy(&x, v + s * 4, 4); InterlockedExchange(&g_randSites[s].state, x); }
                LONG fb; memcpy(&fb, v + g_randSiteCount * 4, 4); InterlockedExchange(&g_randFallbackState, fb);
                done = true;
            }
            break;
        case RK_BODY:
            if (arr && (int)id < nb) RepLogBodyDiff(f, id, arr[id], v, len);
            done = arr && (int)id < nb && RepApplyBody(arr[id], v, len);
            break;
        case RK_ACTOR:
            for (int a = 0; a < na; a++) if ((uint32_t)acts[a].key == id) { done = RepApplyActor(acts[a].actor, v, len); break; }
            break;
        }
        if (done) ok++; else skipped++;
    }
    g_repRounds++;
    g_repItemsFixed += ok;
    Log("repair: frame %u: repaired %u items%s", f, ok, skipped ? " (some couldn't be written; the next round retries)" : "");
    if (skipped) Log("repair: frame %u: %u items skipped (absent on this PC or a different body in that place)", f, skipped);
}

// A new session (NetStart): the repair and the per-frame RNG check start empty; the repair is
// on when [netplay] repair says so.
static void RepReset() {
    RngReset();
    g_repairOn = g_net.repair != 0;
    if (g_net.testDesyncKeys) Log("test: desync hotkeys active this session: Ctrl+F9 extra RNG draw, Ctrl+F10 move a physics body 1 mm (focused window only)");
    g_fpBadRun = 0;
    g_repDigestIn.Reset(0, 0); g_repReqIn.Reset(0, 0); g_repValIn.Reset(0, 0);
    g_repMyDigest.clear(); g_repMyDigestFrame = 0xFFFFFFFF; g_repHandledDigest = 0xFFFFFFFF;
    g_repPendingT = 0; g_repHostT = 0; g_repSentT = 0; g_repMissedT = 0;
    g_repPendingItems.clear(); g_repHostItems.clear(); g_repSentVals.clear();
}

// Packets (under the net lock, any thread): only store.
// RNAK (the client is still waiting for T): the host resends its values, or says RMISSED when
// it's past T without having had the request. RMISSED: the client stops waiting for T.
// RDIGEST, RREQ and RVAL chunks are reassembled; a complete request in time becomes the
// host's job for frame T (RepStep), a late one is answered RMISSED.
void RepHandle(const uint8_t* p, int n, int type) {
    if (!g_repairOn || g_netPhase != NP_RUNNING) return;
    if (type == NET_RNAK || type == NET_RMISSED) {
        if (n < (int)offsetof(NetRepChunk, seq)) return;
        uint32_t T = ((const NetRepChunk*)p)->frame;
        if (type == NET_RMISSED) { if (g_net.role == 1 && T == g_repPendingT) g_repMissedT = T; return; }
        if (g_net.role != 0) return;
        if (g_repSentT == T && !g_repSentVals.empty()) RepSend(NET_RVAL, T, g_repSentVals.data(), g_repSentVals.size(), true);
        else if (g_repHostT != T && T <= g_netFrame) RepSend(NET_RMISSED, T, NULL, 0, true);   // past T without the request
        return;                                               // (not at T yet: the client re-sends its request)
    }
    if (n < (int)offsetof(NetRepChunk, data)) return;
    const NetRepChunk* c = (const NetRepChunk*)p;
    if (n < (int)offsetof(NetRepChunk, data) + c->len) return;
    if (type == NET_RDIGEST && g_net.role == 1) {
        g_repDigestIn.Add(c);
    } else if (type == NET_RREQ && g_net.role == 0) {
        if (!g_repReqIn.Add(c) || g_repReqIn.frame == g_repHostT || g_repReqIn.frame == g_repSentT) return;
        uint32_t T = g_repReqIn.frame;
        static uint32_t lastMissed;
        if (T <= g_netFrame) {                                // too late: this PC is already past T
            if (lastMissed == T) return;                      // (said so already; copies of the request keep coming)
            lastMissed = T;
            g_repMissedCount++;
            RepSend(NET_RMISSED, T, NULL, 0, true);
            Log("repair: the other player asked for frame %u, already past (now %u); they'll ask again", T, g_netFrame);
        } else {
            g_repHostT = T;
            g_repHostItems = g_repReqIn.data;
            uint32_t cnt = 0, from = 0;
            if (g_repHostItems.size() >= 12) { memcpy(&from, g_repHostItems.data() + 4, 4); memcpy(&cnt, g_repHostItems.data() + 8, 4); }
            Log("repair: the other player found %u items differing at frame %u; copying them at frame %u", cnt, from, T);
        }
    } else if (type == NET_RVAL && g_net.role == 1) {
        if (c->frame == g_repPendingT) g_repValIn.Add(c);
    }
}

// Owner-less dynamic bodies that fell out of the world (decomp session: body slot 16 on the
// level-select map is in free fall below the floor, released ~4 frames apart during the
// load, so the two PCs' copies sit metres apart at the first digest). In a session, every
// such body below y = -100 m is stopped for good on both PCs at the lockstep point:
// velocities zeroed, simulation disabled (activation state 5). The repair then copies
// the one remaining position difference once, and the body costs nothing from then on.
static void FreezeFallenOrphans(uint32_t f) {
    if (!RepGameOk()) return;
    int n; BYTE** arr;
    if (!RepWorld(&n, &arr) || !arr) return;
    static int logs, sess = -1;
    if (sess != g_netSessions) { sess = g_netSessions; logs = 0; }
    __try {
        for (int i = 0; i < n; i++) {
            BYTE* o = arr[i];
            if (!o || *(int*)(o + 0xf0) != 2 || (*(uint32_t*)(o + 0xc8) & 3)) continue;   // dynamic rigid bodies only
            if (*(int*)(o + 0xd4) == 5) continue;                                     // already stopped
            float y = *(float*)(o + 0x44);
            if (!(y < -100.0f)) continue;
            BYTE* rec = *(BYTE**)(o + 0xe8);
            if (rec && (*(BYTE*)(rec + 0x14) & 3) == 1) {                             // has an owner: leave it to the game...
                // ...unless the "owner" is only its own Physics::RigidBody wrapper with no
                // actor key: the board hub's and TrainStub's never-placed boxes (slot 16)
                // count as owned that way and fell forever (-16.7 km by the end of the
                // train level). This far below the level nothing can bind them any more.
                BYTE* h = *(BYTE**)(rec + 8);
                BYTE* comp = h ? *(BYTE**)(h + 0x10) : NULL;
                bool bareWrapper = comp && *(uint64_t*)comp == (uint64_t)(ExeBase() + 0x7edca8) && !*(uint64_t*)(comp + 0x58);
                if (!bareWrapper) continue;
            }
            memset(o + 0x90, 0, 0x20);                                                // interpolation velocities
            memset(o + 0x140, 0, 0x20);                                               // linear, angular velocity
            *(int*)(o + 0xd4) = 5;                                                    // DISABLE_SIMULATION
            *(float*)(o + 0xd8) = 0.0f;
            if (logs++ < 10)
                Log("determinism: frame %u: owner-less body #%d (slot %u, def %u) fell out of the world (y %.1f): stopped on this PC",
                    f, i, rec ? *(uint16_t*)rec : 0xFFFF, rec ? *(uint16_t*)(rec + 2) : 0xFFFF, y);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Once per lockstep frame, before the fingerprint, at the same point on both PCs.
static void RepStep(uint32_t f) {
    if (!g_repairOn || g_netPhase != NP_RUNNING || f <= g_netHashHoldUntil) return;
    if (g_net.role == 0 && g_repHostT == f) RepHostCapture(f);
    // Client: a request can be lost (UDP, or Steam unreliable in a pinch): send it again every
    // 6 frames until T; the host ignores copies of a request it already has.
    if (g_net.role == 1 && g_repPendingT && f < g_repPendingT && (g_repPendingT - f) % 6 == 0 && !g_repPendingItems.empty())
        RepSend(NET_RREQ, g_repPendingT, g_repPendingItems.data(), g_repPendingItems.size(), true);
    // Client at T: wait here (the frame doesn't go on) until the host's values arrive,
    // re-asking every 200 ms, then write them. Past T without them: give up this round.
    if (g_net.role == 1 && g_repPendingT && f >= g_repPendingT) {
        if (f == g_repPendingT) {
            ULONGLONG t0 = GetTickCount64(), lastAsk = 0;
            while (!g_repValIn.done && g_repMissedT != f && g_netPhase == NP_RUNNING) {
                ULONGLONG t = GetTickCount64();
                if (t - lastAsk >= 200) {
                    lastAsk = t;
                    RepSend(NET_RNAK, f, NULL, 0, false);
                    if (!g_repPendingItems.empty()) RepSend(NET_RREQ, f, g_repPendingItems.data(), g_repPendingItems.size(), true);
                }
                if (t - t0 > 600) Status(800, kOvBlue, L"Fixing a small difference with %s...", g_netPeerNameW);
                if (t - g_netLastHeard > (ULONGLONG)g_net.timeoutMs) { NetEnd("the other player stopped responding"); return; }
                NetPump();
                Sleep(1);
            }
            if (g_netPhase != NP_RUNNING) return;
            if (g_repValIn.done && g_repValIn.frame == f) RepClientApply(f, g_repValIn.data);
            else { g_repMissedCount++; Log("repair: frame %u: the host was already past it; trying again next round", f); }
        }
        g_repPendingT = 0;
        g_repValIn.Reset(0, 0);
    }
    // Fingerprint frames: both PCs build a digest. The host sends its own; the client keeps
    // its own until the host's for the same frame is complete.
    if (g_net.hashInterval > 0 && f > 0 && f % g_net.hashInterval == 0) {
        std::vector<uint8_t> d;
        RepBuildDigest(f, d);
        if (g_net.role == 0) RepSend(NET_RDIGEST, f, d.data(), d.size(), false);
        else {
            // The previous round's host digest never completed: say so (lost chunks), at most 20 times.
            static int incompleteLogs;
            if (g_repMyDigestFrame != 0xFFFFFFFF && g_repHandledDigest != g_repMyDigestFrame && incompleteLogs < 20) {
                incompleteLogs++;
                Log("repair: frame %u: the host's digest didn't arrive complete (%u of %u chunks for frame %u); this round is skipped",
                    g_repMyDigestFrame, g_repDigestIn.frame == g_repMyDigestFrame ? g_repDigestIn.got : 0,
                    g_repDigestIn.frame == g_repMyDigestFrame ? g_repDigestIn.total : 0, g_repMyDigestFrame);
            }
            g_repMyDigest.swap(d); g_repMyDigestFrame = f;
        }
    }
    // Client: compare once, as soon as the host's digest for the kept frame is complete.
    if (g_net.role == 1 && g_repDigestIn.done && g_repDigestIn.frame == g_repMyDigestFrame && g_repHandledDigest != g_repMyDigestFrame) {
        g_repHandledDigest = g_repMyDigestFrame;
        RepCompare(g_repDigestIn.data, g_repMyDigest);
    }
}

// Client: ask the host for its random-number state, copied a few frames from now.
void RngAskRepair(uint32_t g) {
    if (!g_repairOn || g_net.role != 1 || g_repPendingT || g <= g_rngQuietUntil) return;
    if (g_rngEpisodeAsks >= kRngMaxAsks) return;
    std::vector<uint8_t> req(12);
    uint32_t n = 0;
    auto add = [&](uint8_t kind) { uint8_t e[5] = { kind, 0, 0, 0, 0 }; req.insert(req.end(), e, e + 5); n++; };
    add(RK_RAND);
    if (RepGameOk() && RepMtInst(RK_MT)) add(RK_MT);
    uint32_t lead = (std::min)((uint32_t)g_net.repairLead, (uint32_t)(2 * g_net.delay + 6));
    uint32_t T = RepRequest(req, g, n, lead);
    g_rngEpisodeAsks++;
    g_rngAsks++;
    Log("repair: frame %u: the random numbers differ from the host's (per-frame check); copying them at frame %u", g, T);
}

// Test hotkeys ([netplay] test_desync_keys=1): knock THIS PC slightly out of
// step, on purpose, to watch the repair (or, with repair=0, the desync check)
// do its job. Only in a session, only while this game's window has focus (the
// one-PC test runs two windows), applied at the lockstep point like a repair.
//   Ctrl+F9   one extra draw from the engine's random generator
//   Ctrl+F10  move one awake dynamic physics body by 1 mm
static bool TestKeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
static bool OurWindowFocused() {
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}
// Ctrl+F10's body: the first awake dynamic rigid body (activation state 1), else any dynamic
// one, moved 1 mm along x. Returns its index in the world's array, -1 if there's none.
static int TestNudgeBody(uint32_t f, uint16_t* slotOut) {
    int n; BYTE** arr;
    if (!RepGameOk() || !RepWorld(&n, &arr) || !arr) return -1;
    int pick = -1;
    __try {
        for (int pass = 0; pass < 2 && pick < 0; pass++)       // first an awake dynamic body, then any dynamic one
            for (int i = 0; i < n; i++) {
                BYTE* o = arr[i];
                if (!o || *(int*)(o + 0xf0) != 2 || (*(uint32_t*)(o + 0xc8) & 3)) continue;   // rigid, neither static nor kinematic
                if (pass == 0 && *(int*)(o + 0xd4) != 1) continue;
                pick = i;
                break;
            }
        if (pick < 0) return -1;
        BYTE* o = arr[pick];
        *(float*)(o + 0x40) += 0.001f;                           // origin x
        *(float*)(o + 0x80) += 0.001f;                           // interpolation origin x
        BYTE* rec = *(BYTE**)(o + 0xe8);
        *slotOut = rec ? *(uint16_t*)rec : 0xFFFF;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return pick;
}
// Each frame at the lockstep point; a key acts once per press.
static void TestDesyncKeys(uint32_t f) {
    if (!g_net.testDesyncKeys || g_netPhase != NP_RUNNING) return;
    static bool wasF9, wasF10;
    bool ctrl = TestKeyDown(VK_CONTROL), focused = OurWindowFocused();
    bool f9 = ctrl && focused && TestKeyDown(VK_F9), f10 = ctrl && focused && TestKeyDown(VK_F10);
    if (f9 && !wasF9) {
        if (g_mtGlobalPtr && *g_mtGlobalPtr && g_mtRefill) {
            uint32_t v = H_MTNext((uint8_t*)*g_mtGlobalPtr);
            Log("test: Ctrl+F9 at frame %u: one extra engine RNG draw on this PC only (%08x)", f, v);
            Status(4000, kOvAmber, L"Test: this PC drew one extra random number.\nWatch for the repair (or a desync if repair is off).");
        } else {
            Log("test: Ctrl+F9 at frame %u: the engine RNG isn't available here", f);
        }
    }
    if (f10 && !wasF10) {
        uint16_t slot = 0xFFFF;
        int i = TestNudgeBody(f, &slot);
        if (i >= 0) {
            Log("test: Ctrl+F10 at frame %u: body #%d (slot %u) moved 1 mm on this PC only", f, i, slot);
            Status(4000, kOvAmber, L"Test: this PC moved one physics object by 1 mm.\nWatch for the repair (or a desync if repair is off).");
        } else {
            Log("test: Ctrl+F10 at frame %u: no dynamic physics body to move", f);
        }
    }
    wasF9 = f9; wasF10 = f10;
}

// The local player's controller this frame (or synthesized test input).
// tick: this is the frame's real read, so presses count (the debug menu's, and the shared
// controller's Back + RB). The test input is a hash of the seed and the frame number, the same
// on every run.
NetPad NetReadLocal(uint32_t frame, bool tick = false) {
    NetPad p = {};
    if (g_net.testInputSeed) {
        uint32_t x = (g_net.testInputSeed * 2654435761u) ^ (frame * 40503u);
        x ^= x >> 13; x *= 0x5bd1e995; x ^= x >> 15;
        p.pad.wButtons = (WORD)(x & (g_localTest >= 0 ? 0xF30F : 0xF33F));   // local test bot: no Start/Back
        p.pad.sThumbLX = (SHORT)((int)(x >> 3 & 0x3FFF) - 0x2000);
        p.pad.sThumbLY = (SHORT)((int)(x >> 9 & 0x3FFF) - 0x2000);
        p.packet = frame;
        return p;
    }
    XINPUT_STATE s = {};
    if (XInputGetState((DWORD)g_net.localPad, &s) != ERROR_SUCCESS) ZeroMemory(&s, sizeof s);
    DebugMenu_FilterPad(0, &s, tick);                    // before it's packed and sent: both PCs see the same
    // One-PC test sharing one controller: Back + RB hands it to the other window's player.
    if (g_sharedPad && g_netPhase == NP_RUNNING) {
        const WORD kSwap = XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_RIGHT_SHOULDER;
        bool combo = (s.Gamepad.wButtons & kSwap) == kSwap;
        if (tick) {
            if (combo && !g_sharedComboWas) {
                g_sharedActive = !g_sharedActive;           // both copies see the same press, so they swap together
                Log("local co-op test: controller %s player %d", g_sharedActive ? "now drives" : "left", g_net.role + 1);
                if (g_sharedActive) Status(3000, kOvGreen, L"Controller: you're now PLAYER %d.", g_net.role + 1);
                else Status(3000, kOvAmber, L"Controller moved to the other window. Back + RB to come back.");
            }
            g_sharedComboWas = combo;
        }
        if (!g_sharedActive || combo) {                      // this player stands still
            DWORD packet = s.dwPacketNumber;
            ZeroMemory(&s, sizeof s);
            s.dwPacketNumber = packet;
        }
    }
    p.pad = s.Gamepad;
    p.packet = s.dwPacketNumber;
    return p;
}

// g_GameTimer (0x140954820), advanced by GameTimer_Tick (FUN_1404779d0). Its
// running state carries each PC's history since boot: +0x00 frame count,
// +0x04/+0x10 raw/game dt ms, +0x08/+0x0C last/now clock ms, +0x14/+0x18
// previous/total game ms, +0x1C dt s, +0x20/+0x24 previous/total t s, +0x28
// the smoothed dt physics steps with (dt*[+0x34] + s*[+0x38]), +0x2C/+0x30
// previous/running sum of it. +0x34 on is fixed configuration. Physics and
// ~100 "t + x <= t" timers read these, so both PCs start them from the same
// values, built from the session start time with the game's own constants.
static BYTE* GameTimer() {
    return RipRefersTo(0x48001f, 3, 7, 0x954820) ? ExeBase() + 0x954820 : NULL;   // lea rcx,[g_GameTimer] after a movie
}
// Writes the timer's running state as if the game had run at 60 frames a second, 16 ms a
// tick, up to the session start time (g_frameStartUs), using the game's own ms-to-seconds
// factors. Same start time on both PCs, same values.
static void NetResetGameTimer() {
    BYTE* t = GameTimer();
    if (!t || !g_engineClockReady) { Log("netplay: game timer NOT aligned (code doesn't match this build)"); return; }
    BYTE* b = ExeBase();
    float msToS = *(float*)(b + 0x776dbc);               // the tick's dt_ms -> seconds factor
    double totalToS = *(double*)(b + 0x7c3d08);          // its total_ms -> t factor
    uint32_t nowMs = kStartMs + (uint32_t)(g_frameStartUs / 1000);
    uint32_t total = (uint32_t)(g_frameStartUs / 1000), dt = 16;
    float dtS = (float)dt * msToS;
    float tNow = (float)((double)total * totalToS), tPrev = (float)((double)(total - dt) * totalToS);
    *(uint32_t*)(t + 0x00) = (uint32_t)(g_frameStartUs / 1000000) * 60;
    *(uint32_t*)(t + 0x04) = dt;
    *(uint32_t*)(t + 0x08) = nowMs - dt;
    *(uint32_t*)(t + 0x0c) = nowMs;
    *(uint32_t*)(t + 0x10) = dt;
    *(uint32_t*)(t + 0x14) = total - dt;
    *(uint32_t*)(t + 0x18) = total;
    *(float*)(t + 0x1c) = dtS;
    *(float*)(t + 0x20) = tPrev;
    *(float*)(t + 0x24) = tNow;
    *(float*)(t + 0x28) = dtS;
    *(float*)(t + 0x2c) = tPrev;
    *(float*)(t + 0x30) = tNow;
    Log("netplay: game timer aligned (frame %u, total %u ms, t %.3f s, smoothed dt %.6f s)", *(uint32_t*)t, total, tNow, dtS);
}

// Everything that must start out identical on both PCs.
// Called from NetStart, before lockstep frame 0: game time, the engine frame count, the game
// timer, the entity-key counter, the random generators, the rand() streams and the spawn queue.
static void NetResetForStart() {
    if (g_engineClockReady) {
        // Same game time on both PCs. One hour is past anything a title
        // screen reaches, so time only ever moves forward here.
        LONG64 kSessionStartUs = g_netStartKnown ? (LONG64)g_netStartVus : 3600LL * 1000000;
        if (g_vus > kSessionStartUs)
            Log("netplay: WARNING: game time is already past the session start time; frame times may jump");
        InterlockedExchange64(&g_vus, kSessionStartUs);
        g_frameStartUs = g_vus;
        g_readsThisFrame = 0;
        PublishMs();
    }
    g_engineFrames = 0;
    NetResetGameTimer();
    // Entity keys (DAT_1409575c8, bumped by every entity constructor, never
    // reset) order the actor registry. Two PCs can reach the title screen with
    // slightly different counts, so both round up to the same boundary.
    if (RipRefersTo(0x23ccd, 3, 7, 0x9575c8)) {          // mov rdx,[counter] in the entity constructor
        volatile uint64_t* keys = (volatile uint64_t*)(ExeBase() + 0x9575c8);
        uint64_t before = *keys, after = g_netStartKnown ? g_netStartKeys : (before + 0x100000) & ~0xFFFFFULL;
        *keys = after;
        Log("netplay: entity keys aligned (%llu -> %llu)", before, after);
    } else {
        Log("netplay: entity key counter doesn't match this build; not aligned");
    }
    if (g_mtReseedOk) {
        void* mt = *g_mtGlobalPtr;
        if (mt) {
            AcquireSRWLockExclusive(&g_mtLock);
            g_mtSeed(mt, kMtEngineSeed);
            g_mtRefill(mt);
            DialogMt(true);                              // dialog takes: same sequence on both PCs from here
            CosmeticMt(true);                            // blink timing: likewise
            ReleaseSRWLockExclusive(&g_mtLock);
            Log("netplay: engine RNG reseeded (0x%x)", kMtEngineSeed);
        } else {
            Log("netplay: engine RNG doesn't exist yet; not reseeded");
        }
    }
    for (int i = 0; i < g_randSiteCount; i++) { g_randSites[i].state = 1; g_randSites[i].mainCalls = g_randSites[i].otherCalls = 0; }
    g_randFallbackState = 1;
    for (auto& s : g_spawnSlots) s.used = false;
    g_spawnSeq = 0;
}

static bool InFrontEnd();                           // defined with the drop-in code
// Both players are ready (NetFrameBody, WAITSTART): lockstep begins at frame 0. Resets what
// must start equal, then switches on what only runs in a session: the shared debug-menu
// actions, the in-frame loader and the lockstep clock.
static void NetStart() {
    g_netPhase = NP_RUNNING;
    g_netFrame = 0;
    for (int f = 0; f < g_net.delay; f++) {           // frames before the delay are neutral on both sides
        g_netLocal[f] = NetPad{};  g_netLocalTag[f] = f + 1;
        g_netRemote[f] = NetPad{}; g_netRemoteTag[f] = f + 1;
    }
    NetResetForStart();
    RepReset();
    g_netLastHeard = GetTickCount64();
    g_netSessions++;
    Log("netplay: lockstep started as %s (input delay %d frames, session %d, start time %.0f s, keys from %llu)",
        g_net.role ? "client / player 2" : "host / player 1", g_net.delay, g_netSessions,
        g_netStartVus / 1e6, g_netStartKeys);
    Status(6000, kOvGreen, L"Online co-op started with %s!", g_netPeerNameW);
    // Without drop-in, two copies that each loaded a level on their own can't be
    // brought together by starting here: every such start ends at the first
    // fingerprint. Say so instead of leaving a silent "desync".
    if (!g_dropIn && g_feHooked && !InFrontEnd()) {
        Log("netplay: WARNING: session started inside a level with drop_in=0; the two worlds were loaded separately and will not match");
        Status(12000, kOvRed, L"Started inside a level: the two games can't line up here.\n"
                              L"Both go back to the title screen and start from \"Press Start\".");
    }
    if (g_netT) g_netT->SessionStarted();
    DebugMenu_EnableSharedActions(true, g_net.role == 0);   // player 1's menu acts for both
    if (SyncLoaderOk()) SetSyncLoader(true);               // loads in the session finish on the same frame on both PCs
    g_clockLockstep = true;                                // frame times from the tick count alone
    // Autosave (FUN_14049c930) only runs, and posts "SavingGame" to the
    // checkpoint and inventory systems, when storage+0xa4 is set. Make it the same
    // on both PCs; with no storage device (+0xb4 0) the write itself just fails.
    __try {
        if (BYTE* st = *(BYTE**)(ExeBase() + 0xac6270)) {
            BYTE ready = *(BYTE*)(st + 0xa4);
            Log("netplay: storage ready flag %u, device %d%s", ready, *(int*)(st + 0xb4), ready ? "" : "; set to 1 so autosaves run as on the host");
            if (!ready) *(BYTE*)(st + 0xa4) = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    NetViewportsRefresh();
    NetRumble(500, 45000);                            // both players feel the start
}

// The INPUT packet: this PC's pads for up to kNetRedundancy frames ending at upTo (so a lost
// packet costs nothing), plus the newest fingerprint, the start values (the client adopts the
// host's) and the newest per-frame RNG tags. Sent every frame, and again while waiting.
void NetSendInputs(uint32_t upTo) {
    NetInputPkt pk = {};
    pk.h = NetHeader(NET_INPUT);
    pk.hashFrame = g_netLastHashFrame;
    pk.hash = g_netLastHash;
    pk.startVus = g_netStartVus;
    pk.startKeys = g_netStartKeys;
    if (g_rngLocalNewest) {                                // the newest RNG tags (consecutive frames only)
        uint32_t last = g_rngLocalNewest - 1;
        int cnt = 0;
        while (cnt < kNetRngTags && cnt <= (int)last && g_rngLocalTag[(last - cnt) % kNetRing] == last - cnt + 1) cnt++;
        pk.rngLast = last;
        pk.rngCount = (uint8_t)cnt;
        for (int k = 0; k < cnt; k++) pk.rng[k] = g_rngLocal[(last - (cnt - 1) + k) % kNetRing];
    }
    uint32_t first = upTo + 1 >= (uint32_t)kNetRedundancy ? upTo + 1 - kNetRedundancy : 0;
    pk.lastFrame = upTo;
    pk.count = (uint8_t)(upTo - first + 1);
    for (uint32_t f = first; f <= upTo; f++) pk.pads[f - first] = g_netLocal[f % kNetRing];
    NetSend(&pk, (int)(offsetof(NetInputPkt, pads) + pk.count * sizeof(NetPad)));
    g_netLocalSentUpTo = upTo;
}
// The game's own "in the front-end loop" flag (Script_InFrontEndLoop: g_Display+0x1a8 bit 2), the same test
// the character-select launch makes at 0x14012e829. True at the title, the main menu and the level select.
static bool InFrontEnd() {
    static int ok = -1;
    if (ok < 0) ok = RipRefersTo(0x12e829, 3, 7, 0xac66a0) ? 1 : 0;
    if (!ok) return false;
    __try {
        BYTE* disp = *(BYTE**)(ExeBase() + 0xac66a0);
        return disp && (*(BYTE*)(disp + 0x1a8) & 4) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// True when the front-end test's code matches this build, so InFrontEnd's answer means something.
static bool InFrontEndCodeOk() { InFrontEnd(); return RipRefersTo(0x12e829, 3, 7, 0xac66a0); }

// Drop-in, on both PCs at lockstep frame kDropLaunchFrame: install the host's save (the client
// got it through the save transfer, net_dropin.cpp), set both players' characters and load
// the saved world.
// The fingerprint check is held off until the level publishes.
static void DropLaunch() {
    BYTE* b = ExeBase();
    if (g_net.testInputSeed) {                           // tests: no game to launch; act as if it published at once
        g_netHashHoldUntil = g_netFrame + 120;
        Log("drop-in: frame %u: test launch of \"%s\" (%u bytes in memory)", g_netFrame, g_dropWorld, g_dropSize);
        return;
    }
    DropPrepareSave();
    bool ok = DropInstall();
    DropAlignBindings();
    if (PmAsyncClear()) Log("drop-in: Performance Manager async loading was on; switched off before the load");
    LotSelectorReset("the drop-in launch");
    void* db;
    int idx = DropWorldIndex(&db);
    // Same characters on both PCs (player 2 is "" unless the host already had one:
    // the friend then joins in-level with Start and picks theirs on ScreenJoinIn).
    if (idx >= 0 && DropWriteAvatars(db, idx))
        Log("drop-in: characters set: player 1 \"%s\", player 2 \"%s\"", g_dropAvatar[0], g_dropAvatar[1]);
    else
        Log("drop-in: characters NOT set (world \"%s\" %s): the PCs may spawn different characters",
            g_dropWorld, idx < 0 ? "not found" : "found, but the setter code doesn't match");
    // How to load it ([netplay] dropin_launch): the SwitchLevel event, or the front end's own
    // launch path. auto picks SwitchLevel when the game is known to be outside the front end.
    bool useSwitch = !_wcsicmp(g_dropLaunchHow, L"switchlevel") ||
                     (!_wcsicmp(g_dropLaunchHow, L"auto") && InFrontEndCodeOk() && !InFrontEnd());
    Log("drop-in: this PC is %s: launching with %s", InFrontEnd() ? "in the front end" : "in a level (level select?)",
        useSwitch ? "SwitchLevel" : "SetupLaunch");
    if (useSwitch) {
        ok = ok && PostGameEvent("SwitchLevel", g_dropWorld);
    } else {
        // The front-end launch, checked against this build first (the "SetupLaunch" string,
        // the calls at 0x14012e86b and 0x14012e8c0): select the world in the world database
        // (exe+0x5f09b0), post "SetupLaunch", two calls on the screen manager (vtable +0x20)
        // and 0xb into exe+0x94d564.
        static const char kSetupLaunch[] = "SetupLaunch";
        bool code = BytesAt(0x792950, kSetupLaunch, sizeof kSetupLaunch) &&
                    RipRefersTo(0x12e86b, 1, 5, 0x5f09b0) && RipRefersTo(0x12e8c0, 3, 7, 0xb2ef10);
        if (!code) idx = -1;
        if (idx >= 0) {
            ((void (*)(void*, int))(b + 0x5f09b0))(db, idx);
            ok = ok && PostGameEvent((const char*)(b + 0x792950), NULL);
            if (void* sm = *(void**)(b + 0xb2ef10)) {
                void** vt = *(void***)sm;
                ((void (*)(void*, int, int))vt[0x20 / 8])(sm, 0, 0);
                ((void (*)(void*, int, int))vt[0x20 / 8])(sm, 0, 1);
            }
            *(int*)(b + 0x94d564) = 0xb;
        } else {
            ok = false;
            Log("drop-in: world \"%s\" not found in the world database (code %s)", g_dropWorld, code ? "ok" : "doesn't match");
        }
    }
    g_netHashHoldUntil = 0xFFFFFFFF;                     // until the level publishes
    Log("drop-in: frame %u: loading \"%s\" (%ls): %s", g_netFrame, g_dropWorld, g_dropLaunchHow, ok ? "requested" : "FAILED");
}

// After QuitGame: the game is "back" once it's been in the front end for 120
// frames, or once a load has finished since the quit and 120 frames have passed
// (QuitGame from a world lands on level select, which may not be the front-end
// loop). Counted in polled frames: the game doesn't poll while it unloads, so a
// long unload doesn't count against the time limit.
struct QuitWatch { uint32_t frames, feFrames, loadsAtQuit, afterLoad; int lastFe; };
static void QuitWatchStart(QuitWatch& w) { w = {}; w.loadsAtQuit = g_loadsCompleted; w.lastFe = -1; }
static int QuitWatchStep(QuitWatch& w, const char* who) {   // 1 back, -1 give up, 0 waiting
    w.frames++;
    int fe = InFrontEnd() ? 1 : 0;
    if (fe != w.lastFe) {
        Log("drop-in: %s: front-end flag %d after %u frames (%u loads finished since the quit)", who, fe, w.frames,
            g_loadsCompleted - w.loadsAtQuit);
        w.lastFe = fe;
    }
    w.feFrames = fe ? w.feFrames + 1 : 0;
    w.afterLoad = g_loadsCompleted > w.loadsAtQuit && !g_loading ? w.afterLoad + 1 : 0;
    if (w.feFrames >= 120 || w.afterLoad >= 120) return 1;
    return w.frames > 3600 ? -1 : 0;
}
static QuitWatch g_hostQuit, g_clientQuit;

// The drop-in failed: say why, and fall back to the normal start (both players at Press
// Start, holding Back + Start).
static void NetDropAbort(const char* why) {
    Log("drop-in: %s; falling back to the title screen start", why);
    Status(15000, kOvAmber, L"Couldn't bring %s in automatically (%S).\nGo to the Press Start screen and hold Back + Start.",
           g_netPeerNameW, why);
    g_dropIn = false;
    g_dropStep = DROP_NONE;
}
// The host's side of a drop-in, once per frame while waiting to start: save (DROP_SAVE), wait
// for the storage to go idle and copy the save out of memory (DROP_WAITSAVE), quit the level
// (DROP_QUIT), wait to be out of it (DROP_WAITTITLE). DROP_SEND then hands over to the save
// transfer (net_dropin.cpp). Any step that fails or takes too long aborts the drop-in.
static void NetDropHostStep(ULONGLONG now) {
    switch (g_dropStep) {
    case DROP_SAVE:
        if (DropSaveNow()) {
            g_dropStep = DROP_WAITSAVE;
            g_dropAt = now;
            g_dropSawBusy = false;
            Log("drop-in: saving");
        } else if (now - g_dropAt > 10000) {
            NetDropAbort("the game couldn't save");
        }
        break;
    case DROP_WAITSAVE: {
        int st = StorageState();                       // the game's storage state (net_dropin.cpp); 8 = failed
        bool idle = st == 0 || st == 3 || st == 8 || st == 9;
        if (!idle) g_dropSawBusy = true;
        if (st == 8 && g_dropSawBusy) { NetDropAbort("saving failed"); break; }
        if (idle && now - g_dropAt >= 1000 && (g_dropSawBusy || now - g_dropAt >= 2500)) {
            if (!DropCapture()) { NetDropAbort("no save data in memory"); break; }
            Log("drop-in: saved (%u bytes, world \"%s\"); returning to the title", g_dropSize, g_dropWorld);
            Status(0, kOvGreen, L"Saved. Bringing %s in...", g_netPeerNameW);
            g_dropStep = DROP_QUIT;
        } else if (now - g_dropAt > 15000) {
            NetDropAbort("saving took too long");
        }
        break;
    }
    case DROP_QUIT:
        DropQuitToTitle();
        QuitWatchStart(g_hostQuit);
        g_dropStep = DROP_WAITTITLE;
        g_dropAt = now;
        break;
    case DROP_WAITTITLE: {
        int r = QuitWatchStep(g_hostQuit, "this player");
        if (r > 0) {
            Log("drop-in: out of the level (%s); sending the save to the other player", InFrontEnd() ? "front end" : "level select");
            g_dropStep = DROP_SEND;
        } else if (r < 0) {
            NetDropAbort("the game didn't leave the level");
        }
        break;
    }
    default: break;
    }
}

// Called once per frame, at the anchor poll.
// The whole frame step runs under the net lock, so other threads that use the network (the
// keepalive thread) can't send in the middle of it.
static void NetFrameBody();
void NetFrame() {
    NetLockGuard lock;
    NetFrameBody();
    if (!g_net.testInputSeed) NetHoldPadPoll(g_netPhase == NP_RUNNING);   // (stand-in tests have no game)
}
// The per-frame state machine. CONNECTING: say HELLO until the other PC answers. WAITSTART:
// the drop-in save and quit, then both players ready and the start values agreed. ENDED:
// offer a rejoin. RUNNING: the lockstep step at the end of this function.
static void NetFrameBody() {
    if (g_netWanted && !g_netTried) { g_netTried = true; NetOpen(); }   // not in DllMain: sockets/Steam aren't safe there
    NetPump();
    ULONGLONG now = GetTickCount64();
    NetRumbleTick(now);
    // A Steam join request (Join Game, or an invite accepted while running), left by the
    // Steam callback (net_transport.cpp).
    if (uint64_t host = (uint64_t)InterlockedExchange64(&g_netJoinReq, 0)) {
        if (host == g_netMyId || !g_netT) {
        } else if (g_netPhase == NP_CONNECTING && !g_netPeerHello) {
            g_net.role = 1;                                // this game becomes player 2
            g_netT->SetPeer(host);
            g_netT->SessionStarted();                      // a joining player isn't joinable
            g_netPhaseSince = now;
            g_netNextStatus = 0;
            Log("netplay: joining %llu's game (Steam join request); this player is now the client / player 2", host);
            Status(0, kOvBlue, L"Joining %s...", g_netPeerNameW);
        } else if (g_netPhase == NP_ENDED || g_netPhase == NP_RUNNING) {
            Status(12000, kOvAmber, L"To join another game, restart Toy Story 3 first, then use Join Game again.");
        }
    }
    if (g_netSettingsNotice) {                         // this PC's settings were changed to the host's (net_session.cpp)
        g_netSettingsNotice = false;
        ShowNotice(L"Your game settings were updated to match the host's.\n\n"
                   L"Close the game and start it again, then connect again. The host should restart too.");
    }
    switch (g_netPhase) {
    case NP_CONNECTING: {
        NetCheckLocalPad();
        if (now - g_netLastSend > 250) { NetHdr h = NetHeader(NET_HELLO); NetSend(&h, sizeof h); }
        // The host, still waiting: hold Back + Y for half a second to open Steam's invite dialog.
        if (g_net.role == 0 && !g_netPeerHello) {
            static int inviteHeld;
            XINPUT_STATE s = {};
            bool combo = XInputGetState((DWORD)g_net.localPad, &s) == ERROR_SUCCESS &&
                         (s.Gamepad.wButtons & (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_Y)) == (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_Y);
            inviteHeld = combo ? inviteHeld + 1 : 0;
            if (inviteHeld == 30) {
                bool ok = g_netT->OpenInvite();
                Log("netplay: Steam invite dialog %s", ok ? "opened" : "not available");
                Status(8000, ok ? kOvBlue : kOvAmber, ok ? L"Pick a friend in the Steam window to invite them.\n"
                                                          L"They'll join this game when they accept."
                                                        : L"Steam's invite window isn't available.\n"
                                                          L"Invite from your Steam friends list instead.");
            }
        }
        // The other PC answered: on to WAITSTART, and the host decides whether this is a
        // drop-in (and where it starts from).
        if (g_netPeerHello) {
            char why[160];
            int st = g_netT->LinkState(why, sizeof why);
            g_netPhase = NP_WAITSTART; g_netPhaseSince = now; g_netNextStatus = now + 15000;
            DebugMenu_SetNetplay(true);                    // cheats off and locked before anything is shared
            if (g_net.role == 0 && g_net.testInputSeed && g_net.testDrop) {   // tests: skip the game's save and quit
                g_dropIn = true;
                g_dropStep = DROP_SEND;
                g_dropSize = 1000;
                g_dropBuf = (uint8_t*)malloc(g_dropSize);
                for (uint32_t i = 0; i < g_dropSize; i++) g_dropBuf[i] = (uint8_t)(i * 7);
                strcpy_s(g_dropWorld, "test_world");
                g_dropSlot = 0;
            } else if (g_net.role == 0 && g_net.dropIn && InFrontEndCodeOk() && DropCodeOk()) {
                // "Not in the front end" is also true while the game is still booting (logos,
                // the first loading screen): saving then wrote the empty boot state over the
                // real save (2026-10-24, 355-byte "Saved Game"). Only save and reload when a
                // level is really up: its physics world is built and a save is in memory.
                if (!InFrontEnd() && !DropLevelUp()) {
                    Log("drop-in: the other player connected while this player is neither in a level nor in the front end "
                        "(still starting up?); nothing is saved, both start from the Press Start screen");
                } else if (!InFrontEnd()) {
                    g_dropIn = true;                       // the host is in a level: bring the friend in by reloading it
                    g_dropStep = DROP_SAVE;
                    g_dropAt = now;
                    Log("drop-in: the other player connected while this player is in a level: saving and reloading it together");
                } else if (DropCapture()) {
                    g_dropIn = true;                       // at the title or level select: the save loaded at boot
                    g_dropStep = DROP_SEND;
                    Log("drop-in: the other player connected while this player is in the front end: loading \"%s\" "
                        "from the save in memory (%u bytes)", g_dropWorld, g_dropSize);
                } else {
                    Log("drop-in: no save in memory yet; both start from the Press Start screen instead");
                }
            }
            // Log and tell the players what happens next.
            Log("netplay: Steam link: %s", NetLinkName(st));
            Log("netplay: waiting for both players to hold Back+Start");
            if (g_dropIn && g_dropStep == DROP_SEND)
                Status(0, kOvGreen, L"%s joined!\nLoading your world together...", g_netPeerNameW);
            else if (g_dropIn)
                Status(0, kOvGreen, L"%s joined!\nSaving your game and bringing them in...", g_netPeerNameW);
            else if (g_net.role == 1)                       // the client learns whether it's a drop-in when the save arrives
                Status(0, kOvGreen, L"Connected to %s!\nWaiting for their game...", g_netPeerNameW);
            else if (g_feHooked && !OnPressStart())
                Status(0, kOvGreen, L"%s joined!\nSave your game, go to the Press Start screen, then hold Back + Start.", g_netPeerNameW);
            else
                Status(0, kOvGreen, L"Connected to %s!\nBoth hold Back + Start to begin.", g_netPeerNameW);
            NetRumble(250, 30000);
            break;
        }
        // No answer yet: the link state in the log every 10 s.
        if (now >= g_netNextStatus) {
            if (g_netNextStatus) {
                char why[160];
                int st = g_netT->LinkState(why, sizeof why);
                Log("netplay: still no reply from the other player after %llu s (%llu packets sent; Steam link: %s%s%s)",
                    (now - g_netPhaseSince) / 1000, g_netPacketsOut, NetLinkName(st), why[0] ? ": " : "", why);
                char relay[400];
                g_netT->RelayStatus(relay, sizeof relay);
                if (relay[0]) Log("netplay: Steam %s", relay);
            }
            g_netNextStatus = now + 10000;
        }
        break;
    }
    case NP_WAITSTART: {
        NetCheckLocalPad();
        if (now >= g_netNextStatus) {
            Log("netplay: still waiting to start: this player %s, the other player %s (hold Back+Start on the Press Start screen)",
                g_netLocalReady ? "ready" : "not ready", g_netPeerReady ? "ready" : "not ready");
            g_netNextStatus = now + 15000;
        }
        if (g_dropIn && g_net.role == 0 && g_dropStep != DROP_SEND) {
            NetDropHostStep(now);
            if (!g_netLocalReady && now - g_netLastSend > 250) { NetHdr h = NetHeader(NET_HELLO); NetSend(&h, sizeof h); }
            break;
        }
        // The drop-in save transfer (net_dropin.cpp) must be done before this PC is ready.
        bool saveOk = SaveSyncReady();
        if (g_netPhase != NP_WAITSTART) break;                 // the save transfer can end the session
        if (g_dropIn && g_net.role == 1 && saveOk && !g_netLocalReady && InFrontEndCodeOk() && DropCodeOk()) {
            // The world loads from the front end on both PCs: if this player is in a level, quit to it first.
            // (Only where that can be told and done; otherwise carry on as before.) Neither in a level nor
            // in the front end means still starting up (intro, EULA, first load): QuitGame there exits the
            // whole game (run 84), so wait for the front end instead.
            if (!InFrontEnd() && !g_clientQuitPosted && !DropLevelUp()) {
                static ULONGLONG lastNote;
                if (now - lastNote > 5000) {
                    lastNote = now;
                    Log("drop-in: this player isn't in a level or the front end yet (still starting up?); waiting for the front end");
                    Status(0, kOvGreen, L"Joining %s's world...\nWaiting for your game to finish starting.", g_netPeerNameW);
                }
                saveOk = false;
            } else if (!InFrontEnd()) {
                g_clientFeSince = 0;
                if (!g_clientQuitPosted && InFrontEndCodeOk() && DropCodeOk()) {
                    g_clientQuitPosted = true;
                    QuitWatchStart(g_clientQuit);
                    DropQuitToTitle();
                    Log("drop-in: this player was in a level: returning to the front end first");
                    Status(0, kOvGreen, L"Joining %s's world...", g_netPeerNameW);
                }
                if (g_clientQuitPosted && QuitWatchStep(g_clientQuit, "this player") > 0) {
                    g_clientQuitPosted = false;              // landed somewhere that isn't the front-end loop: go on
                    Log("drop-in: this player is out of the level; ready");
                } else {
                    saveOk = false;
                }
            } else if (g_clientQuitPosted) {
                if (QuitWatchStep(g_clientQuit, "this player") <= 0) saveOk = false;   // let the front end settle
            }
        }
        // Ready: Back + Start held on the Press Start screen, or at once in a drop-in or with
        // start=immediate. Ready PCs send READY (with their game time and entity keys).
        bool combo = !g_netLocalReady && saveOk && (g_net.startImmediate || g_dropIn || NetComboHeld(NetReadLocal(0)));
        if (combo && !g_net.startImmediate && !g_dropIn && !OnPressStart()) {
            combo = false;                                     // both games must start from "Press Start"
            if (now >= g_netNextHint) {
                g_netNextHint = now + 4000;
                Status(4000, kOvAmber, L"Go to the Press Start screen first, then hold Back + Start.");
            }
        }
        if (combo) {
            g_netLocalReady = true;
            Log("netplay: this player is ready");
            if (g_dropIn)                                      // nobody presses anything in a drop-in
                Status(0, kOvGreen, g_net.role == 0 ? L"%s is joining...\nLoading your world together."
                                                    : L"Joining %s's game...\nLoading their world.", g_netPeerNameW);
            else
                Status(0, kOvGreen, L"Ready! Waiting for %s to hold Back + Start.", g_netPeerNameW);
        }
        if (g_netLocalReady && now - g_netLastSend > 100) NetSendReady();
        if (!g_netLocalReady && now - g_netLastSend > 250) { NetHdr h = NetHeader(NET_HELLO); NetSend(&h, sizeof h); }
        if (g_netLocalReady && g_netPeerReady && g_net.role == 0 && g_netPeerValsKnown) {
            // Host: start time and entity keys above both PCs' current values,
            // rounded to coarse blocks (an hour of game time, 0x100000 keys).
            const uint64_t kHour = 3600ULL * 1000000, kKeys = 0x100000;
            uint64_t v = (uint64_t)g_vus > g_netPeerVus ? (uint64_t)g_vus : g_netPeerVus;
            uint64_t k = LocalKeys() > g_netPeerKeys ? LocalKeys() : g_netPeerKeys;
            g_netStartVus = (v / kHour + 1) * kHour;
            g_netStartKeys = (k / kKeys + 1) * kKeys;
            g_netStartKnown = true;
            for (int i = 0; i < 3; i++) NetSendReady();              // make sure they know
            NetStart();
        } else if (g_netLocalReady && g_netPeerReady && g_net.role == 1 && g_netStartKnown) {
            NetStart();
        }
        break;
    }
    case NP_ENDED: {                                   // over: hold Back + Start (20 frames) to reconnect;
                                                       // a host whose session never started reopens by itself
        static int held;
        if (g_net.testInputSeed && g_net.testRejoinMs > 0 && g_netT && now - g_netEndedAt >= (ULONGLONG)g_net.testRejoinMs) {
            NetRejoin();                                   // tests only: stand-ins have no title screen
            break;
        }
        if (!g_netT) break;
        if (g_net.role == 0 && !g_netEndedRunning && !g_netSettingsNotice && now - g_netEndedAt >= 5000 && !g_net.testInputSeed) {
            Log("netplay: that session never started; waiting for a friend to join again");
            NetRejoin();                                   // nothing was shared yet: just reopen
            break;
        }
        XINPUT_STATE s = {};
        bool combo = XInputGetState((DWORD)g_net.localPad, &s) == ERROR_SUCCESS &&
                     (s.Gamepad.wButtons & (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START)) == (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START);
        held = combo ? held + 1 : 0;                     // anywhere: the drop-in path reloads the host's world
        if (held >= 20) { held = 0; NetRejoin(); }
        break;
    }
    default: break;
    }
    if (g_netPhase != NP_RUNNING) return;

    // Lockstep step for frame f.
    uint32_t f = g_netFrame;
    uint32_t mine = f + g_net.delay;
    // This PC's input for frame f + delay, carrying at most one debug-menu action and the
    // load gate flag, into the ring and out to the other PC.
    g_netLocal[mine % kNetRing] = NetReadLocal(mine, true);
    {
        uint8_t a[64];
        int n = DebugMenu_PollAction(a, sizeof a);
        if (n <= 0 && g_net.testInputSeed && g_net.role == 0 && g_net.testActionFrame && mine == (uint32_t)g_net.testActionFrame) {
            n = 12;                                        // tests: a dummy action from player 1
            memset(a, 0x5A, 12);
        }
        if (n > 0 && n <= (int)sizeof g_netLocal[0].act) {
            g_netLocal[mine % kNetRing].actLen = (uint8_t)n;
            memcpy(g_netLocal[mine % kNetRing].act, a, n);
            Log("netplay: debug menu action (%d bytes) scheduled for frame %u", n, mine);
        } else if (n > 0) {
            Log("netplay: debug menu action too big (%d bytes); dropped", n);
        }
        if (n <= 0 && g_net.role == 0 && !g_net.testInputSeed) {   // quiet: the host's aim invert flags (NetApplyAimInvert)
            int m = NetAimInvertAction(a, mine);
            if (m > 0) {
                g_netLocal[mine % kNetRing].actLen = (uint8_t)m;
                memcpy(g_netLocal[mine % kNetRing].act, a, m);
            }
        }
    }
    g_netLocal[mine % kNetRing].flags = g_loadGate && g_loadGateReady ? 1 : 0;
    g_netLocalTag[mine % kNetRing] = mine + 1;
    NetSendInputs(mine);

    // Wait for the other PC's input for frame f, pumping the network. The session ends if
    // they said goodbye and frame f's input won't come, or nothing came for timeout_ms.
    auto peerGone = [&]() {
        if (!g_netPeerBye || g_netRemoteTag[f % kNetRing] == f + 1) return false;
        return g_netPeerLastFrame == 0xFFFFFFFF || f > g_netPeerLastFrame ||
               GetTickCount64() - g_netPeerByeAt > 1000;          // their last inputs got lost
    };
    if (peerGone()) { NetEnd("the other player left"); return; }
    ULONGLONG waitStart = GetTickCount64();
    bool stalled = false;
    while (g_netRemoteTag[f % kNetRing] != f + 1) {
        stalled = true;
        NetPump();
        if (g_netPhase != NP_RUNNING) return;
        if (peerGone()) { NetEnd("the other player left"); return; }
        ULONGLONG t = GetTickCount64();
        if (t - g_netLastSend > 15) NetSendInputs(mine);              // keep them fed while we wait
        static ULONGLONG lastWaitStatus;
        if (t - waitStart > 400 && t - lastWaitStatus > 300) {
            lastWaitStatus = t;
            if (t - g_peerBusyAt < 1000)
                Status(1200, kOvBlue, L"%s is loading... (%llu s)", g_netPeerNameW, (t - waitStart) / 1000);
            else if (t - g_peerMovieAt < 1000)
                Status(1200, kOvBlue, L"%s is watching a movie... press A to skip it for both of you", g_netPeerNameW);
            else
                Status(1200, kOvAmber, L"Waiting for %s... (%llu s)", g_netPeerNameW, (t - waitStart) / 1000);
        }
        if (t - g_netLastHeard > (ULONGLONG)g_net.timeoutMs) { NetEnd("the other player stopped responding"); return; }
        Sleep(1);
    }
    if (stalled) {
        ULONGLONG ms = GetTickCount64() - waitStart;
        g_netStalls++;
        g_netStallMs += ms;
        if (ms > g_netMaxStallMs) g_netMaxStallMs = ms;
    }
    // Both pads for frame f are here: apply the debug-menu actions they carry (player 1's
    // aim-invert flags travel the same way), then the load gate.
    {
        const NetPad& p0 = g_net.role == 0 ? g_netLocal[f % kNetRing] : g_netRemote[f % kNetRing];
        const NetPad& p1 = g_net.role == 0 ? g_netRemote[f % kNetRing] : g_netLocal[f % kNetRing];
        if (p0.actLen && !NetApplyAimInvert(p0, f)) {
            DebugMenu_ApplyAction(p0.act, p0.actLen);
            Log("netplay: frame %u: player 1's debug menu action applied", f);
        }
        if (p1.actLen) { DebugMenu_ApplyAction(p1.act, p1.actLen); Log("netplay: frame %u: player 2's debug menu action applied", f); }
        // Load gate: while it is on, each PC holds the game's load-complete counter
        // (kLoadDoneCounter) below 4 once its own load step is done, until its streaming has
        // been idle kStreamIdleFrames frames (or kStreamWaitMax have passed); then its input
        // carries flag bit 0. When both pads of a frame carry it, both PCs let the counter
        // reach 4 on that same frame.
        if (g_loadGate) {
            __try {
                BYTE* b = ExeBase();
                volatile int* done = (volatile int*)(b + kLoadDoneCounter);
                // Streaming activity this frame (see g_worldUpdateOrig). The game's own file
                // reads only (exe, crt): FMOD streams music and Bink movies all the time.
                LONG64 reads = g_ioReads[0] + g_ioReads[1];
                int loaderTasks = 0;
                if (BYTE* ld = *(BYTE**)(b + 0x9c2e40)) loaderTasks = *(int*)(ld + 0x48);
                bool pumpBusy = *(volatile char*)(b + 0x957280 + 0x84) != 0;
                bool hogBusy = *(void* volatile*)(b + 0x9c29f8) != NULL;
                bool readsMoved = reads != g_streamLastReads;
                bool busy = readsMoved || loaderTasks > 0 || pumpBusy || hogBusy;
                g_streamLastReads = reads;
                g_streamIdleFrames = busy ? 0 : g_streamIdleFrames + 1;
                if ((p0.flags & 1) && (p1.flags & 1)) {
                    *done = 3;                                 // the next vf24 reaches 4: LevelLoadComplete on both PCs
                    g_loadGate = false;
                    Log("load gate: released at frame %u (both PCs loaded and streamed; world frozen %u frames; "
                        "%u early unpauses held back)", f, g_worldFrozen, g_unpauseBlocked);
                } else if (*done >= 4) {
                    g_loadGate = false;                        // got past us (several game ticks in one frame?)
                    Log("load gate: MISSED at frame %u: the level finished loading on its own on this PC", f);
                } else {
                    if (*done >= 2) {
                        if (!g_loadStepDone) {
                            g_loadStepDone = true;
                            g_loadStepAt = f;
                            Log("load gate: this PC's load step is done at frame %u; waiting for its streaming to finish", f);
                        }
                        *done = 1;                             // hold: room for two vf24 ticks before our next frame
                    }
                    if (g_loadStepDone && !g_loadGateReady) {
                        bool idle = g_streamIdleFrames >= kStreamIdleFrames;
                        if (idle || f - g_loadStepAt >= kStreamWaitMax) {
                            g_loadGateReady = true;
                            if (idle)
                                Log("load gate: this PC's streaming finished at frame %u (%u frames after its load step); "
                                    "waiting for the other PC", f, f - g_loadStepAt);
                            else
                                Log("load gate: this PC's streaming still busy at frame %u (%u frames after its load step: "
                                    "reads %s, loader tasks %d, pump %s, file thread %s); ready anyway", f, f - g_loadStepAt,
                                    readsMoved ? "moving" : "still", loaderTasks, pumpBusy ? "busy" : "idle",
                                    hogBusy ? "busy" : "idle");
                        }
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { g_loadGate = false; }
        }
    }
    if (g_dropIn && !g_dropLaunched && f == kDropLaunchFrame) {   // drop-in: both PCs load the saved world on this frame
        g_dropLaunched = true;
        DropLaunch();
        Status(12000, kOvGreen, g_net.role ? L"Loading the host's world...\nPress Start to join when it's loaded."
                                           : L"Loading your world with %s...\nThey press Start to join.", g_netPeerNameW);
    }
    // Floating-point mode. The MXCSR rounding / flush-to-zero / denormals-are-
    // zero bits decide the low bits of every physics result; a video decoder
    // (Bink) or any library that sets them and is torn down at a different
    // point on each PC leaves the two main threads in different modes. Seen
    // as body #0 drifting from the first sample after a skipped cutscene. In a
    // session the mode seen at the first frame is restored every frame.
    // Only the mode bits matter (masks, rounding, FTZ, DAZ); the low six are
    // sticky "an exception happened" flags that change all the time harmlessly.
    {
        unsigned csr = _mm_getcsr();
        if (!g_fpCsrRef) g_fpCsrRef = csr;
        if ((csr & ~0x3fu) != (g_fpCsrRef & ~0x3fu)) {
            g_fpCsrFixes++;
            if (g_fpCsrFixes <= 20) Log("fp: MXCSR mode was %08x at frame %u, restored to %08x", csr, f, g_fpCsrRef);
            _mm_setcsr((g_fpCsrRef & ~0x3fu) | (csr & 0x3fu));
        }
    }
    // The game replaced Bullet's free hook twice during start-up; make sure an
    // arena block can never reach the CRT between two arena allocations.
    KeepBtFreeHooked();
    // Performance Manager loads (an area's actors and props) run ASYNC during
    // play: the manager's byte +0x41 is set by the stand-alone game loop, and a
    // queued load completes on whatever frame the disk finishes, so entering a
    // new area woke actors, started behaviours (MinMax draws) and created bodies
    // a frame apart on two PCs. The level load already clears it (FUN_1405d81a0);
    // in a session it stays 0, so every PM load runs synchronously at its
    // deterministic request frame. Cost: a hitch on area entry.
    __try {
        if (BYTE* pmgr = *(BYTE**)(ExeBase() + 0xb2ef20)) {
            static uint64_t pmForced;
            if (*(volatile BYTE*)(pmgr + 0x41)) {
                *(volatile BYTE*)(pmgr + 0x41) = 0;
                if (pmForced++ < 5) Log("determinism: Performance Manager async loading switched off at frame %u (area loads finish in-frame)", f);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // Per-frame determinism steps (net_dropin.cpp, net_determinism.cpp) and the physics log.
    NetFadeStep(f);
    NetMusicSyncHold(true);
    g_zeroAllocs = g_zeroAllocOk;
    NetVisSync();
    NetLogPhysFrame(f);
    // Loading gates scripts wait on (decomp session's list), logged on change:
    // fiber loader pending, data pump busy, PerformanceManager pending tasks /
    // mini-load state / flags, the mini-load flag, and global MT draws so far.
    {
        static char last[200];
        char now[200] = "";
        __try {
            BYTE* b = ExeBase();
            BYTE* ld = *(BYTE**)(b + 0x9c2e40);
            BYTE* pm = *(BYTE**)(b + 0xb2ef20);
            _snprintf_s(now, sizeof now, _TRUNCATE, "loader %d pump %d perf %d/%d/%d%d mini %d",
                        ld ? *(int*)(ld + 0x48) : -1, (int)*(char*)(b + 0x957280 + 0x84),
                        pm ? *(int*)(pm + 0xb4) : -1, pm ? *(int*)(pm + 0xbc) : -1,
                        pm ? (int)*(BYTE*)(pm + 0x68) : -1, pm ? (int)*(BYTE*)(pm + 0x69) : -1, (int)*(BYTE*)(b + 0xb2c5a4));
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (strcmp(now, last)) {
            strcpy_s(last, now);
            Log("gates %u: %s (engine rng draws %llu)", f, now, g_mtGlobalDraws);
        }
    }
    // (Player 2's health bar is no longer raised level with player 1's: the
    // players preferred the game's own height. HudPlaceHealthBars(true) did it.)
    // The pads the game reads this frame (NetGetState): pad 0 the host's, pad 1 the client's.
    const NetPad& local = g_netLocal[f % kNetRing];
    const NetPad& remote = g_netRemote[f % kNetRing];
    g_netCur[0] = g_net.role == 0 ? local : remote;
    g_netCur[1] = g_net.role == 1 ? local : remote;

    TestDesyncKeys(f);                                   // test hotkeys (test_desync_keys=1): knock this PC out of step
    FreezeFallenOrphans(f);                              // owner-less bodies falling forever below the world
    RepStep(f);                                          // in-place repair: copy at T, digest at fingerprint frames
    RngStep(f);                                          // per-frame RNG check (after any repair write)
    if (g_netPhase != NP_RUNNING) return;
    if (!g_net.testInputSeed) {
        NetLogBindings(f); NetParkOrphanBodies(f); KeyLogFlush(f); NetLogAimSettings(f);
        HudHidePrompts(NetOwnScreenActive());
        // Drop-in: the "press Start" hint once the level is up (published at least a second ago,
        // no load in progress), not under the loading screen where Start does nothing yet.
        if (g_dropHintAt && f >= g_dropHintAt && !g_loading) {
            g_dropHintAt = 0;
            Status(12000, kOvGreen, g_net.role ? L"You're in! Press Start to join and pick your character."
                                               : L"%s is here. They press Start to join in.", g_netPeerNameW);
        }
    }

    // Fingerprint frames: hash, log the state's parts, send, and compare if theirs is in.
    if (g_net.hashInterval > 0 && f > 0 && f % g_net.hashInterval == 0) {
        int i = f % kNetRing;
        g_netHashLocal[i] = NetFingerprint();
        g_netHashLocalTag[i] = f + 1;
        NetLogStateParts(f);
        g_netLastHashFrame = f;
        g_netLastHash = g_netHashLocal[i];
        NetHashPkt hp = { NetHeader(NET_HASH), f, g_netHashLocal[i] };
        NetSend(&hp, sizeof hp);
        NetCompareHash(f);
    }
    g_netFrame++;
}

// Continues a 64-bit FNV-1a hash h over n bytes, 8 at a time, skipping any 8 bytes that look
// like a pointer, which would make two PCs' hashes differ for no reason.
static uint64_t HashSkippingPointers(uint64_t h, const BYTE* p, size_t n) {
    for (size_t i = 0; i + 8 <= n; i += 8) {
        uint64_t q = *(const uint64_t*)(p + i);
        uint32_t hi = (uint32_t)(q >> 32);
        if (hi >= 1 && hi <= 0x7fff) continue;           // a user-mode pointer: differs per process
        for (int k = 0; k < 8; k++) { h ^= (q >> (k * 8)) & 0xff; h *= 1099511628211ULL; }
    }
    return h;
}
// Diagnostic: per frame (the first 600 frames of NetLogPhysFrame's level window), every
// townsperson actor (name contains "Townperson"; at most 16), sorted by actor key:
// position (+0x80), transform node (*(+0x200)+0x48), activation (+0x188,
// +0x189, +0x420), its PerformanceNode (via the CCDB_ModelDefinition component
// +0x7e8: level +0x10, state +0x14, countdown +0xbc, wanted +0xc0, hold +0xc2,
// delay +0xcc), the behaviour-tree node name (script variable
// "BehaviorNodeName", read raw: no refresh), and a hash of every animation
// channel's state block (pointer-looking qwords skipped) plus "motion ended"
// bits. Offsets from the decomp session; reads only, no game state written.
static void NetLogTownFrame(uint32_t f) {
    BYTE* b = ExeBase();
    struct Town { uint64_t key; BYTE* actor; };
    Town towns[16];
    int nt = 0;
    // Found through the physics bodies (rec +0xe8 -> handle +8 -> component
    // +0x10 -> actor key +0x58 -> FUN_140558400), the path that resolves the
    // townspeople's names in the body dump.
    __try {
        BYTE* phys = *(BYTE**)(b + 0xb275c8);
        BYTE* world = phys ? *(BYTE**)(phys + 0x1168) : NULL;
        int n = world ? *(int*)(world + 0x0c) : 0;
        BYTE** arr = world ? *(BYTE***)(world + 0x18) : NULL;
        for (int i = 0; arr && i < n && i < 200000 && nt < 16; i++) {
            BYTE* o = arr[i];
            BYTE* rec = o ? *(BYTE**)(o + 0xe8) : NULL;
            if (!rec || (*(BYTE*)(rec + 0x14) & 3) != 1) continue;
            BYTE* h = *(BYTE**)(rec + 8);
            BYTE* comp = h ? *(BYTE**)(h + 0x10) : NULL;
            if (!comp) continue;
            uint64_t key = *(uint64_t*)(comp + 0x58);
            BYTE* actor = ((BYTE* (*)(uint64_t))(b + 0x558400))(key);
            if (!actor) continue;
            const char* n1 = *(const char**)(actor + 0x148);
            const char* n2 = *(const char**)(actor + 0x150);
            if (!((n1 && strstr(n1, "Townperson")) || (n2 && strstr(n2, "Townperson")))) continue;
            bool dup = false;
            for (int k = 0; k < nt; k++) if (towns[k].key == key) dup = true;
            if (!dup) { towns[nt].key = key; towns[nt].actor = actor; nt++; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    for (int i = 1; i < nt; i++)                         // sort by key: both logs list them in the same order
        for (int j = i; j > 0 && towns[j - 1].key > towns[j].key; j--) { Town t = towns[j]; towns[j] = towns[j - 1]; towns[j - 1] = t; }
    typedef BYTE* (*PFN_FindCompByName)(BYTE*, const char*, int);
    typedef BYTE* (*PFN_FindVar)(BYTE*, const char*);
    for (int i = 0; i < nt; i++) {
        BYTE* a = towns[i].actor;
        uint32_t pos[3] = {}, xf[3] = {}, pmLevel = 0xffffffff, pmState = 0xffffffff, pmCd = 0, pmDelay = 0;
        int act0 = -1, act1 = -1, actState = -1, pmWanted = -1, pmHold = -1;
        char bt[40] = "?";
        uint64_t anim = 1469598103934665603ULL;
        uint32_t ended = 0;
        int channels = 0;
        __try {
            memcpy(pos, a + 0x80, 12);
            if (BYTE* tn = *(BYTE**)(a + 0x200)) memcpy(xf, tn + 0x48, 12);
            act0 = *(BYTE*)(a + 0x188); act1 = *(BYTE*)(a + 0x189); actState = *(int*)(a + 0x420);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        __try {
            BYTE* c = ((PFN_FindCompByName)(b + 0x558450))(a, "CCDB_ModelDefinition", 0);
            BYTE* pn = c ? *(BYTE**)(c + 0x7e8) : NULL;
            if (pn) {
                pmLevel = *(uint32_t*)(pn + 0x10); pmState = *(uint32_t*)(pn + 0x14);
                pmCd = *(uint32_t*)(pn + 0xbc); pmWanted = *(BYTE*)(pn + 0xc0); pmHold = *(BYTE*)(pn + 0xc2);
                pmDelay = *(uint32_t*)(pn + 0xcc);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        __try {
            BYTE* var = ((PFN_FindVar)(b + 0x55fd40))(a, (const char*)(b + 0x7f9fd0));   // "BehaviorNodeName"
            if (var && *(BYTE*)(var + 0x2a) == 7)
                if (const char* s = *(const char**)(var + 8)) strncpy_s(bt, s, _TRUNCATE);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        __try {
            BYTE* ad = *(BYTE**)(a + 0x360);
            BYTE* ai = ad ? *(BYTE**)(ad + 0x18) : NULL;
            if (ai) {
                BYTE** bkts = *(BYTE***)(ai + 0x140);
                uint32_t cnt = *(uint32_t*)(ai + 0x150);
                for (uint32_t k = 0; bkts && k < cnt && k < 64; k++)
                    for (BYTE* nd = bkts[k]; nd && channels < 32; nd = *(BYTE**)nd) {
                        BYTE* np = *(BYTE**)(nd + 0x10);
                        BYTE* C = np ? *(BYTE**)(np + 0x10) : NULL;
                        if (!C) continue;
                        uint32_t chKey = *(uint32_t*)(nd + 8);
                        anim ^= chKey; anim *= 1099511628211ULL;
                        anim = HashSkippingPointers(anim, C, 0x80);
                        if (!*(BYTE**)(C + 0x60))
                            if (BYTE* c70 = *(BYTE**)(C + 0x70))
                                if (BYTE* S = *(BYTE**)(c70 + 0x10)) {
                                    anim = HashSkippingPointers(anim, S, 0xa0);
                                    if (*(BYTE*)(S + 0x96) & 2) ended |= 1u << (chKey & 31);
                                }
                        channels++;
                    }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Log("town %u: key %llu pos %08x%08x%08x xf %08x%08x%08x act %d/%d/%d pm %d/%d cd %08x want %d hold %d delay %08x bt %s anim %d/%08x/%08x",
            f, (unsigned long long)towns[i].key, pos[0], pos[1], pos[2], xf[0], xf[1], xf[2], act0, act1, actState,
            (int)pmLevel, (int)pmState, pmCd, pmWanted, pmHold, pmDelay, bt, channels, (uint32_t)anim, ended);
    }
}

// Diagnostic: every lockstep frame from when a level's physics comes up (more
// than 100 bodies) until it goes away, the body list in 16 chunk hashes
// (origin, activation, velocity) plus body #0's exact bits and the step's
// inputs (smoothed dt, t). The once-a-second samples showed a box drifting in
// its low bits within 60 frames of a cutscene; this pins the frame.
static void NetLogPhysFrame(uint32_t f) {
    static int sess = -1;
    static uint32_t startF;
    static bool on;
    if (sess != g_netSessions) { sess = g_netSessions; on = false; startF = 0; }
    BYTE* b = ExeBase();
    int n = -1;
    BYTE** arr = NULL;
    __try {
        if (BYTE* phys = *(BYTE**)(b + 0xb275c8))
            if (BYTE* world = *(BYTE**)(phys + 0x1168)) { n = *(int*)(world + 0x0c); arr = *(BYTE***)(world + 0x18); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (on && n >= 0 && n <= 100) { on = false; startF = 0; }   // back on a menu or the map: arm for the next level
    if (!on) {
        if (n > 100 && arr && !startF) {
            on = true;
            startF = f;
            // The game's gravity mode (world+0x120): 1 = every body is pulled
            // toward the world origin, and within 1 m of it a body's gravity
            // keeps its last value (decomp session). The drifting box sits there.
            __try {
                BYTE* world = *(BYTE**)(*(BYTE**)(b + 0xb275c8) + 0x1168);
                Log("phys: level physics up at frame %u with %d bodies; gravity mode %d", f, n, *(int*)(world + 0x120));
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            LogActivationLists(f);
        }
        else return;
    }
    if (n > 0 && arr) LogSilentWakes(f, arr, n);
    // The body/camera line runs for the whole level (one short line a frame);
    // the townsperson detail only for its first 600 frames.
    char line[256] = "";
    __try {
        for (int c = 0; c < 16 && n > 0 && arr; c++) {
            uint64_t h = 1469598103934665603ULL;
            for (int i = c * n / 16; i < (c + 1) * n / 16; i++) {
                const BYTE* o = arr[i];
                if (!o) continue;
                const BYTE* parts[4] = { o + 0x40, o + 0xd4, o + 0x140, o + 0x150 };
                const size_t lens[4] = { 12, 8, 12, 12 };
                for (int k = 0; k < 4; k++) for (size_t j = 0; j < lens[k]; j++) { h ^= parts[k][j]; h *= 1099511628211ULL; }
            }
            size_t len = strlen(line);
            _snprintf_s(line + len, sizeof line - len, _TRUNCATE, " %04x", (unsigned)(h & 0xffff));
        }
        uint32_t p[3] = {}, v[3] = {}, g[3] = {}, dt = 0, t = 0;
        if (n > 0 && arr && arr[0]) { memcpy(p, arr[0] + 0x40, 12); memcpy(v, arr[0] + 0x140, 12); memcpy(g, arr[0] + 0x170, 12); }
        if (BYTE* gt = GameTimer()) { dt = *(uint32_t*)(gt + 0x28); t = *(uint32_t*)(gt + 0x24); }
        // Both views' active camera positions, exact bits: the Performance
        // Manager wakes, sleeps and throttles actors by distance from EACH
        // view's camera (PerformanceNodeTest_Actor / Weight_Distance), so a
        // camera a hair apart moves townspeople a frame apart.
        uint32_t cam[2][3] = {};
        if (BYTE* pm = *(BYTE**)(b + 0xb2ef50))
            if (BYTE* list = *(BYTE**)(pm + 0xd8)) {
                int count = *(int*)list;
                BYTE** views = *(BYTE***)(list + 8);
                for (int i = 0; i < count && i < 2 && views; i++) {
                    BYTE* view = *(BYTE*)(pm + 0xd0) ? views[0] : views[i];
                    BYTE** stk = view ? *(BYTE***)(view + 0x78) : NULL;
                    int sn = view ? *(int*)(view + 0x80) : 0;
                    BYTE* c = stk && sn > 0 && stk[sn - 1] ? *(BYTE**)(stk[sn - 1] + 0x50) : NULL;
                    if (c) memcpy(cam[i], c + 0x190, 12);
                }
            }
        Log("phys %u: n %d:%s | b0 %08x%08x%08x v %08x%08x%08x | dt %08x t %08x | g %08x%08x%08x | cam %08x%08x%08x %08x%08x%08x",
            f, n, line, p[0], p[1], p[2], v[0], v[1], v[2], dt, t, g[0], g[1], g[2],
            cam[0][0], cam[0][1], cam[0][2], cam[1][0], cam[1][1], cam[1][2]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // Diagnostic: what body 0 touches. In the train level it's an orphan box (no scene
    // node bound, so it drops to the world origin) and the first to split when the two
    // PCs run different resolutions. Every collision object whose broadphase AABB
    // (proxy +0x20 min, +0x30 max) overlaps body 0's: slot, def, collision flags
    // (1 static, 2 kinematic), shape vtable, exact position bits.
    {
        char near0[700] = "";
        int found = 0;
        __try {
            const BYTE* o0 = n > 1 && arr ? arr[0] : NULL;
            const BYTE* px0 = o0 ? *(BYTE**)(o0 + 0xb0) : NULL;
            if (px0) {
                const float* mn0 = (const float*)(px0 + 0x20);
                const float* mx0 = (const float*)(px0 + 0x30);
                for (int i = 1; i < n && found < 12; i++) {
                    const BYTE* o = arr[i];
                    const BYTE* px = o ? *(BYTE**)(o + 0xb0) : NULL;
                    if (!px) continue;
                    const float* mn = (const float*)(px + 0x20);
                    const float* mx = (const float*)(px + 0x30);
                    bool hit = true;
                    for (int k = 0; k < 3; k++) if (mn[k] > mx0[k] + 0.05f || mx[k] < mn0[k] - 0.05f) hit = false;
                    if (!hit) continue;
                    found++;
                    const BYTE* rec = *(BYTE**)(o + 0xe8);
                    const BYTE* shape = *(BYTE**)(o + 0xb8);
                    uint32_t q[3];
                    memcpy(q, o + 0x40, 12);
                    size_t len = strlen(near0);
                    _snprintf_s(near0 + len, sizeof near0 - len, _TRUNCATE, " [#%d slot %d def %d fl %u shape %llx at %08x%08x%08x]", i,
                                rec ? *(uint16_t*)rec : -1, rec ? *(uint16_t*)(rec + 2) : -1, *(uint32_t*)(o + 0xc8),
                                shape ? (unsigned long long)(*(uint64_t*)shape - (uint64_t)b + 0x140000000ULL) : 0ULL, q[0], q[1], q[2]);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (found) Log("near0 %u:%s", f, near0);
        // The same bodies' state, field group by field group (decomp session's list), as
        // 32-bit hashes, so two logs show which field of which body differs first:
        // B basis (+0x10), I interpolation transform (+0x50), J interpolation velocities
        // (+0x90), V linear/angular velocity (+0x140), then raw activation state (+0xd4),
        // deactivation time (+0xd8), collision flags (+0xc8), proxy group/mask (+8/+0xa).
        static int fieldLines;
        if (found && fieldLines < 400) {
            char line[1400] = "", rawLine[600] = "";
            bool raw[2] = { false, false };
            __try {
                auto fnv = [](const BYTE* p, int len) {
                    uint32_t h = 2166136261u;
                    for (int k = 0; k < len; k++) h = (h ^ p[k]) * 16777619u;
                    return h;
                };
                const BYTE* o0 = arr[0];
                const BYTE* px0 = *(BYTE**)(o0 + 0xb0);
                const float* mn0 = (const float*)(px0 + 0x20);
                const float* mx0 = (const float*)(px0 + 0x30);
                int shown = 0;
                for (int i = 0; i < n && shown < 13; i++) {
                    const BYTE* o = arr[i];
                    const BYTE* px = o ? *(BYTE**)(o + 0xb0) : NULL;
                    if (!px) continue;
                    if (i) {
                        const float* mn = (const float*)(px + 0x20);
                        const float* mx = (const float*)(px + 0x30);
                        bool hit = true;
                        for (int k = 0; k < 3; k++) if (mn[k] > mx0[k] + 0.05f || mx[k] < mn0[k] - 0.05f) hit = false;
                        if (!hit) continue;
                    }
                    shown++;
                    size_t len = strlen(line);
                    _snprintf_s(line + len, sizeof line - len, _TRUNCATE, " [#%d B%08x I%08x J%08x V%08x a%u d%08x f%x g%04x/%04x]", i,
                                fnv(o + 0x10, 0x30), fnv(o + 0x50, 0x40), fnv(o + 0x90, 0x20), fnv(o + 0x140, 0x20),
                                *(uint32_t*)(o + 0xd4), *(uint32_t*)(o + 0xd8), *(uint32_t*)(o + 0xc8),
                                (unsigned)*(uint16_t*)(px + 8), (unsigned)*(uint16_t*)(px + 0xa));
                    // Raw bits for body 0 and the first kinematic box: every btVector3 is 4 floats,
                    // and the hashes above include the 4th (w) lane. Origin +0x40, interpolation
                    // origin +0x80, linear velocity +0x140, angular +0x150, all 16 bytes each.
                    if (i == 0 || ((*(uint32_t*)(o + 0xc8) & 2) && !(raw[0] && raw[1]))) {
                        int r = i == 0 ? 0 : 1;
                        if (!raw[r]) {
                            raw[r] = true;
                            const uint32_t* w = (const uint32_t*)o;
                            size_t rl = strlen(rawLine);
                            _snprintf_s(rawLine + rl, sizeof rawLine - rl, _TRUNCATE,
                                        " [#%d O %08x %08x %08x %08x  IO %08x %08x %08x %08x  V %08x %08x %08x %08x  W %08x %08x %08x %08x]", i,
                                        w[0x40 / 4], w[0x44 / 4], w[0x48 / 4], w[0x4c / 4], w[0x80 / 4], w[0x84 / 4], w[0x88 / 4], w[0x8c / 4],
                                        w[0x140 / 4], w[0x144 / 4], w[0x148 / 4], w[0x14c / 4],
                                        w[0x150 / 4], w[0x154 / 4], w[0x158 / 4], w[0x15c / 4]);
                        }
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            fieldLines++;
            Log("fields0 %u:%s", f, line);
            if (rawLine[0]) Log("raw0 %u:%s", f, rawLine);
        }
    }
    if (f - startF <= 600) NetLogTownFrame(f);
    if (g_updFrame + 1 == f || g_updFrame == f)          // the previous frame's actor update order
        Log("upd %u: n %u hash %08x", g_updFrame, g_updCount, (uint32_t)g_updHash);
    if (f - startF == 60 || f - startF == 600) LogActivationLists(f);
}

// Diagnostic: one short hash per physics body at every fingerprint (once a
// second), keyed by the body's pool slot, so a quiet drift (Toy Box train level
// 2026-10-25: chunk 6 differed from 14435, the session ended at 19260) names
// its body within 60 frames even after the full dumps' 25-per-session cap.
static void LogBodyHashes(uint32_t f) {
    BYTE* b = ExeBase();
    int n = -1;
    BYTE** arr = NULL;
    __try {
        if (BYTE* phys = *(BYTE**)(b + 0xb275c8))
            if (BYTE* world = *(BYTE**)(phys + 0x1168)) { n = *(int*)(world + 0x0c); arr = *(BYTE***)(world + 0x18); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (n <= 100 || n > 4000 || !arr) return;
    static char line[4000 * 14 + 64];
    int len = _snprintf_s(line, sizeof line, _TRUNCATE, "bodyhash %u: n %d:", f, n);
    __try {
        for (int i = 0; i < n && len > 0 && len < (int)sizeof line - 16; i++) {
            BYTE* o = arr[i];
            if (!o) continue;
            uint32_t h = 2166136261u;
            const BYTE* parts[5] = { o + 0x40, o + 0xd4, o + 0x140, o + 0x150, o + 0x10 };
            const int lens[5] = { 12, 8, 12, 12, 48 };      // origin, activation + sleep timer, velocities, rotation
            for (int k = 0; k < 5; k++) for (int j = 0; j < lens[k]; j++) { h ^= parts[k][j]; h *= 16777619u; }
            int slot = -1;
            if (BYTE* rec = *(BYTE**)(o + 0xe8)) slot = *(uint16_t*)rec;
            len += _snprintf_s(line + len, sizeof line - len, _TRUNCATE, " %d:%08x", slot, h);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (g_log) { fputs("[xinput-harness] ", g_log); fputs(line, g_log); fputs("\n", g_log); fflush(g_log); }   // longer than Log's 1 KB
}

// Diagnostic only (not part of the fingerprint): parts of the game state, so
// two logs show which part diverged first. Unverified layouts stay out of the
// fingerprint so a bad guess can't end a session.
// Called at every fingerprint frame (NetFrameBody).
static void NetLogStateParts(uint32_t f) {
    BYTE* b = ExeBase();
    LogBodyHashes(f);
    auto fnv = [](uint64_t h, const void* p, size_t n) {
        const uint8_t* q = (const uint8_t*)p;
        for (size_t i = 0; i < n; i++) { h ^= q[i]; h *= 1099511628211ULL; }
        return h;
    };
    // Hashes of the main parts (engine RNG, rand() streams, the players' avatars, the bodies),
    // logged together on the "state" line at the end.
    const uint64_t seed = 1469598103934665603ULL;
    uint64_t hMt = seed, hRand = seed, hAv = seed, hBody = seed;
    int avatars = -1, bodies = -1;
    __try {
        if (g_mtGlobalPtr && *g_mtGlobalPtr) hMt = fnv(hMt, *g_mtGlobalPtr, 624 * 4);
        for (int i = 0; i < g_randSiteCount; i++) hRand = fnv(hRand, (const void*)&g_randSites[i].state, sizeof(LONG));
        if (BYTE* pm = *(BYTE**)(b + 0xb2ef50)) {
            avatars = *(int*)(pm + 4);
            for (int slot = 0; slot < 2; slot++)
                if (BYTE* a = *(BYTE**)(pm + 0x1a0 + slot * 0x58)) { hAv = fnv(hAv, a + 0x80, 12); hAv = fnv(hAv, a + 0xc0, 12); }
        }
        if (BYTE* phys = *(BYTE**)(b + 0xb275c8))
            if (BYTE* world = *(BYTE**)(phys + 0x1168)) {
                int n = *(int*)(world + 0x0c);
                BYTE** arr = *(BYTE***)(world + 0x18);
                if (n >= 0 && n < 200000 && arr) {
                    bodies = n;
                    hBody = fnv(hBody, &n, 4);
                    // btCollisionObject: origin at +0x40 (basis rows +0x10/+0x20/+0x30), activation state +0xd4 and
                    // deactivation time +0xd8 (a sleeping body keeps a stale AABB, so pairs differ), btRigidBody linear velocity +0x140
                    for (int i = 0; i < n; i++) if (arr[i]) {
                        hBody = fnv(hBody, arr[i] + 0x40, 12); hBody = fnv(hBody, arr[i] + 0xd4, 8); hBody = fnv(hBody, arr[i] + 0x140, 12);
                        hBody = fnv(hBody, arr[i] + 0x150, 12);   // angular velocity (also decides sleeping)
                    }
                }
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    uint64_t keys = 0;
    __try { keys = *(volatile uint64_t*)(b + 0x9575c8); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // Every live actor's physics flags (Actor+0x3e8): the collision callback
    // writes bit 0x400 from one frame's hit and reads bits 0x10/0x200 the next,
    // so they carry history the body hash can't see. Registry buckets at
    // 0x140b2bb90: node +0x00 actor, +0x08 key, +0x18 next (decomp session).
    uint64_t hAct = seed;
    int actors = -1;
    __try {
        BYTE** buckets = (BYTE**)(b + 0xb2bb90);
        actors = 0;
        for (int bk = 0; bk < 256; bk++)
            for (BYTE* node = buckets[bk]; node && actors < 100000; node = *(BYTE**)(node + 0x18)) {
                BYTE* actor = *(BYTE**)node;
                if (!actor) continue;
                hAct = fnv(hAct, node + 0x08, 8);
                hAct = fnv(hAct, actor + 0x3e8, 4);
                actors++;
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) { actors = -2; }
    // Per call site rand() activity (address as in Ghidra: calls/state), and player 1's position:
    // when the fingerprints differ, the two logs show which site or what moved first.
    {
        char line[1024] = "";
        for (int i = 0; i < g_randSiteCount; i++) {
            LONG calls = g_randSites[i].mainCalls + g_randSites[i].otherCalls;
            if (!calls) continue;
            size_t len = strlen(line);
            _snprintf_s(line + len, sizeof line - len, _TRUNCATE, " %llx:%ld/%08lx", 0x140000000ULL + Rva(g_randSites[i].ret),
                        calls, (unsigned long)g_randSites[i].state);
        }
        float pos[3] = {};
        __try {
            if (BYTE* pm = *(BYTE**)(b + 0xb2ef50))
                if (BYTE* a = *(BYTE**)(pm + 0x1a0)) memcpy(pos, a + 0x80, sizeof pos);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Log("detail %u: p1 at %.3f %.3f %.3f; rand sites (calls/state):%s", f, pos[0], pos[1], pos[2], line);
    }
    // Physics bodies (same list as the "bodies" hash): 16 chunk hashes by index
    // every sample, and for the first three samples in a level (more than 100
    // bodies) every body's index and exact position bits, so two logs show
    // which bodies split first.
    {
        static int dumps, dumpSession, levelSamples;
        if (dumpSession != g_netSessions) { dumpSession = g_netSessions; dumps = 0; levelSamples = 0; }
        char line[400] = "";
        int n = -1;
        BYTE** arr = NULL;
        __try {
            if (BYTE* phys = *(BYTE**)(b + 0xb275c8))
                if (BYTE* world = *(BYTE**)(phys + 0x1168)) { n = *(int*)(world + 0x0c); arr = *(BYTE***)(world + 0x18); }
            if (n > 0 && n < 200000 && arr) {
                for (int c = 0; c < 16; c++) {
                    uint64_t hc = seed;
                    for (int i = c * n / 16; i < (c + 1) * n / 16; i++) if (arr[i]) {
                        hc = fnv(hc, arr[i] + 0x40, 12); hc = fnv(hc, arr[i] + 0xd4, 8); hc = fnv(hc, arr[i] + 0x140, 12);
                        hc = fnv(hc, arr[i] + 0x150, 12);
                    }
                    size_t len = strlen(line);
                    _snprintf_s(line + len, sizeof line - len, _TRUNCATE, " %04x", (unsigned)(hc & 0xffff));
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        // The two persistent body pools (mgr+0x78 + i*0x438: u16 word count at
        // +8, occupancy words at +0x30 + k*0x10). The world adds bodies in slot
        // order, lowest free slot first, so occupancy decides the build order.
        char pools[80] = "";
        __try {
            if (BYTE* mgr = *(BYTE**)(b + 0xb275c8))
                for (int p = 0; p < 2; p++) {
                    BYTE* pool = mgr + 0x78 + p * 0x438;
                    int words = *(uint16_t*)(pool + 8);
                    uint64_t hp = seed;
                    int live = 0;
                    for (int k = 0; k < words && k < 64; k++) {
                        uint64_t w = *(uint64_t*)(pool + 0x30 + k * 0x10);
                        hp = fnv(hp, &w, 8);
                        live += (int)__popcnt64(w);
                    }
                    size_t len = strlen(pools);
                    _snprintf_s(pools + len, sizeof pools - len, _TRUNCATE, " pool%d %d/%08x", p, live, (uint32_t)hp);
                }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (line[0]) Log("bodychunks %u: n %d:%s;%s", f, n, line, pools);
        // Full per-body dumps: the first three samples of each level, then every
        // tenth (10 s), up to 25 per session, so a split later in a level is named.
        if (n <= 100) levelSamples = 0; else levelSamples++;
        if (n > 100 && arr && dumps < 25 && (levelSamples <= 3 || levelSamples % 10 == 0)) {
            dumps++;
            // Per body (decomp session's table, re\SYNC_VIABILITY.md "Bullet body <->
            // game record <-> actor"): position bits and value, linear velocity,
            // collision flags (+0xc8: 1 static, 2 kinematic, 0 dynamic), activation
            // state (+0xd4), the pool record (+0xe8: slot id, def id) and, through
            // the record's component (+8 -> +0x10, actor key at +0x58) and the actor
            // registry (FUN_140558400), the actor's instance and definition names.
            for (int i = 0; i < n; i++) {
                uint32_t u[3] = {}, flags = 0, act = 0, uid = 0, rflags = 0, deact = 0, ang[3] = {};
                float p[3] = {}, vel[3] = {};
                int slot = -1, def = -1;
                uint64_t shapeVt = 0;
                char nm[48] = "?", dn[48] = "?";
                __try {
                    BYTE* o = arr[i];
                    if (o) {
                        memcpy(u, o + 0x40, 12); memcpy(p, o + 0x40, 12); memcpy(vel, o + 0x140, 12);
                        flags = *(uint32_t*)(o + 0xc8); act = *(uint32_t*)(o + 0xd4); deact = *(uint32_t*)(o + 0xd8);   // sleep timer bits
                        memcpy(ang, o + 0x150, 12);              // angular velocity bits
                        if (BYTE* proxy = *(BYTE**)(o + 0xb0)) uid = *(uint32_t*)(proxy + 0x18);   // broadphase unique id
                        if (BYTE* shape = *(BYTE**)(o + 0xb8)) shapeVt = *(uint64_t*)shape - (uint64_t)b + 0x140000000ULL;
                        if (BYTE* rec = *(BYTE**)(o + 0xe8)) {
                            slot = *(uint16_t*)rec; def = *(uint16_t*)(rec + 2);
                            rflags = *(BYTE*)(rec + 0x10) | (uint32_t)(*(BYTE*)(rec + 0x14) & 3) << 8;
                            if ((*(BYTE*)(rec + 0x14) & 3) == 1)
                                if (BYTE* h = *(BYTE**)(rec + 8))
                                    if (BYTE* comp = *(BYTE**)(h + 0x10))
                                        if (BYTE* actor = ((BYTE* (*)(uint64_t))(b + 0x558400))(*(uint64_t*)(comp + 0x58))) {
                                            if (const char* s = *(const char**)(actor + 0x148)) strncpy_s(nm, s, _TRUNCATE);
                                            if (const char* s = *(const char**)(actor + 0x150)) strncpy_s(dn, s, _TRUNCATE);
                                        }
                        }
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
                Log("body %u #%d: slot %d def %d flags %u act %u %08x%08x%08x (%.3f %.3f %.3f) v (%.2f %.2f %.2f) %s / %s"
                    " | uid %u shape %llx rec %03x deact %08x w %08x%08x%08x",
                    f, i, slot, def, flags, act, u[0], u[1], u[2], p[0], p[1], p[2], vel[0], vel[1], vel[2], nm, dn,
                    uid, (unsigned long long)shapeVt, rflags, deact, ang[0], ang[1], ang[2]);
            }
        }
    }
    // Each player view's active camera position (PlayerManager+0xd8 view list,
    // view+0x78/+0x80 camera stack, top entry +0x50 camera, position at +0x190;
    // +0xd0 set = every view uses view 0). Streaming follows these, so they must
    // match on both PCs even with own screens. Exact bits, then readable values.
    {
        char line[256] = "";
        __try {
            if (BYTE* pm = *(BYTE**)(b + 0xb2ef50))
                if (BYTE* list = *(BYTE**)(pm + 0xd8)) {
                    int count = *(int*)list;
                    BYTE** views = *(BYTE***)(list + 8);
                    for (int i = 0; i < count && i < 4 && views; i++) {
                        BYTE* view = *(BYTE*)(pm + 0xd0) ? views[0] : views[i];
                        BYTE** stk = view ? *(BYTE***)(view + 0x78) : NULL;
                        int n = view ? *(int*)(view + 0x80) : 0;
                        BYTE* cam = stk && n > 0 && stk[n - 1] ? *(BYTE**)(stk[n - 1] + 0x50) : NULL;
                        size_t len = strlen(line);
                        int on = view ? (int)*(BYTE*)(view + 0x10) : -1;   // view enabled (FUN_140400990); activation uses enabled views only
                        if (!cam) { _snprintf_s(line + len, sizeof line - len, _TRUNCATE, " v%d(on %d) none", i, on); continue; }
                        const uint32_t* u = (const uint32_t*)(cam + 0x190);
                        const float* p = (const float*)(cam + 0x190);
                        _snprintf_s(line + len, sizeof line - len, _TRUNCATE, " v%d(on %d) %08x%08x%08x (%.2f %.2f %.2f)",
                                    i, on, u[0], u[1], u[2], p[0], p[1], p[2]);
                    }
                }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (line[0]) Log("cams %u:%s", f, line);
    }
    // Open screens (g_ScreenManager's per-player lists: shared at +0x1c8, players at +0x1d0/+0x1d8,
    // next at screen+0x98, name from vtable[1]); logged when they change. Shows what pauses a game.
    {
        static char last[512];
        char now[512] = "";
        BYTE* mgr = NULL;
        if (RipRefersTo(0x3416a, 3, 7, 0xb2ef10)) __try { mgr = *(BYTE**)(b + 0xb2ef10); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (mgr) {
            static const int offs[3] = { 0x1c8, 0x1d0, 0x1d8 };
            static const char* tags[3] = { "shared", "p1", "p2" };
            for (int l = 0; l < 3; l++) {
                size_t len = strlen(now);
                _snprintf_s(now + len, sizeof now - len, _TRUNCATE, "%s%s:", l ? " | " : "", tags[l]);
                __try {
                    BYTE* sc = *(BYTE**)(mgr + offs[l]);
                    for (int k = 0; sc && k < 16; k++, sc = *(BYTE**)(sc + 0x98)) {
                        const char* nm = ((const char* (*)(void*))(*(void***)sc)[1])(sc);
                        // The screen's own name from the manager's records (ScreenRecord: screen +0x30,
                        // name string object +0x38, element +0x70, next +0x80); most menus' class is
                        // just "Screen". The string's text is read as a pointer at +0x40, else inline.
                        char rn[40] = "";
                        for (BYTE* r = *(BYTE**)(mgr + 0x30); r && !rn[0]; r = *(BYTE**)(r + 0x80)) {
                            if (*(BYTE**)(r + 0x30) != sc && *(BYTE**)(r + 0x70) != sc) continue;
                            const char* s = *(const char**)(r + 0x40);
                            bool ptr = false;
                            __try { ptr = s && isalpha((unsigned char)s[0]) && isprint((unsigned char)s[1]); } __except (EXCEPTION_EXECUTE_HANDLER) {}
                            if (!ptr) s = (const char*)(r + 0x40);
                            int j = 0;
                            for (; j < (int)sizeof rn - 1 && isprint((unsigned char)s[j]); j++) rn[j] = s[j];
                            rn[j] = 0;
                        }
                        len = strlen(now);
                        _snprintf_s(now + len, sizeof now - len, _TRUNCATE, " %s%s%s%s", rn[0] ? rn : "", rn[0] ? "(" : "",
                                    nm ? nm : "?", rn[0] ? ")" : "");
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            if (strcmp(now, last)) {
                strcpy_s(last, now);
                Log("screens %u: %s", f, now);
            }
        }
    }
    Log("clock %u: ticks %llu frame start %lld us, in-frame drift %lld us, spawn seq %u, most reads in a frame %u, "
        "data pump drains %llu (longest %llu ms), loader drains %llu (longest %llu ms, most %u slices), UI rng draws %llu, "
        "dialog rng draws %llu, fp csr %08x (fixed %llu times), limiter skips %llu", f, g_engineFrames, g_frameStartUs,
        g_vus - g_frameStartUs, (unsigned)g_spawnSeq, g_maxReadsPerFrame, g_pumpDrainFrames, g_pumpDrainMaxMs, g_loaderDrainFrames,
        g_loaderDrainMaxMs, g_loaderDrainMaxIters, g_uiMtDraws, g_dlgMtDraws, _mm_getcsr(), g_fpCsrFixes, g_frameLimitSkips);
    Log("state %u: mt %08x rand %08x avatars %d/%08x bodies %d/%08x keys %llu voice %llu aflags %d/%08x", f, (uint32_t)hMt,
        (uint32_t)hRand, avatars, (uint32_t)hAv, bodies, (uint32_t)hBody, keys, g_voiceWaits, actors, (uint32_t)hAct);
    // The RNG trace is buffered (1 MB); a game closed from its window never
    // flushes the tail, and two runs ended with exactly 65536 records each.
    AcquireSRWLockExclusive(&g_mtLock);
    if (g_mtDrawFile) fflush(g_mtDrawFile);
    ReleaseSRWLockExclusive(&g_mtLock);
    if (BYTE* t = GameTimer())
        Log("timer %u: frame %u dt %u ms total %u ms t %08x smoothed %08x sum %08x", f, *(uint32_t*)t, *(uint32_t*)(t + 0x10),
            *(uint32_t*)(t + 0x18), *(uint32_t*)(t + 0x24), *(uint32_t*)(t + 0x28), *(uint32_t*)(t + 0x30));
}
