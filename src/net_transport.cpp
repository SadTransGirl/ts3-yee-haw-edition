// net_transport.cpp -- the netplay connection: the netplay settings globals, the UDP and Steam
// transports, the Steam join callback, opening a session (data fingerprint, settings sheet,
// transport) and the end-of-run summary.
//
// How it fits: xinput_proxy.cpp reads the [netplay] ini keys into g_net at start-up (and
// "+tsnet_host <id>" from the command line). Nothing network-related happens in DllMain:
// NetOpen runs once, on the game's main thread, from the first per-frame poll (NetFrameBody
// in net_lockstep.cpp). It builds the settings sheet, opens a transport into g_netT and
// starts the helper threads. From then on net_session.cpp sends and receives packets through
// g_netT (NetSend, NetPump) and net_lockstep.cpp runs the lockstep frames. NetSummary runs
// from CloseRun (xinput_proxy.cpp) when the process exits.
//
// A transport (NetTransport, proxy_shared.h) only moves datagrams to and from the one other
// player; it knows nothing about packet types (that's net_session.cpp). UdpTransport talks
// to a fixed address and port (the one-PC tests, or a direct connection by IP address) and
// can fake latency and packet loss. SteamTransport goes through Steam's relays to a Steam ID:
// what real players use.
//
// Terms: a "fingerprint" is a hash both PCs compare. The settings fingerprint (g_netSettings)
// rides in every packet header, so two PCs set up differently never start a session; the
// state fingerprint (net_lockstep.cpp) checks that two running games still match. The
// "settings sheet" is the text the settings fingerprint hashes (see net_session.cpp).
// "Drop-in" is a friend joining while the host is already in a level: the host saves and
// both PCs load that save together.

#include "proxy_shared.h"

// ------------------------------------------------------------------ netplay
//
// Lockstep online co-op (design: harness\docs\NETPLAY_DESIGN.md). Both PCs run
// the full game; only controller input crosses the network. On both PCs the
// game's pad 0 is the host's controller and pad 1 the client's. Each PC reads
// its own physical controller and sends it to the other.
//
// The lockstep frame is the input poll (the anchor XInputGetState call, once
// per game frame). At frame f each PC sends its input for f + delay (with the
// previous 7 frames again, so a lost packet costs nothing), then waits for
// the other PC's input for frame f before answering the game. Frames before
// `delay` are neutral on both sides.
//
// Session: HELLO (settings fingerprint must match) -> both players hold
// Back+Start (or start=immediate for tests) -> READY -> lockstep from the
// next frame, with game time, frame counters and rand streams reset to the
// same values on both PCs. Every hash_interval frames both send a fingerprint
// of the random-number state; a mismatch ends the session as a desync.

NetCfg g_net;                   // the [netplay] settings (fields explained in proxy_shared.h)
bool g_netLaunchedByJoin;       // started by Steam "Join Game" (+tsnet_host <id> on the command line)
wchar_t g_dropLaunchHow[16] = L"auto";   // [netplay] dropin_launch: auto, setuplaunch or switchlevel
int  g_localTest = -1;          // one-PC co-op test (+tsnet_local host|client): 0 host, 1 client, -1 off
// One-PC test with ONE controller: both copies read the same pad, and
// Back + RB hands it between player 1 and player 2 (the other stands still).
int  g_localControllers;        // [netplay] local_test_controllers: 0 auto, 1 one shared, 2 two
// g_sharedPad: the one controller is shared (decided in NetOpen). g_sharedActive: it drives
// this copy's player right now. g_sharedComboWas: Back + RB was down last frame (a press
// counts once).
bool g_sharedPad, g_sharedActive, g_sharedComboWas;
int  g_localHostPad = 0, g_localClientPad = 1;   // one-PC test: the real XInput pads of player 1 and player 2

// "+tsnet_host <steam id>" in a Steam connect string or command line.
static uint64_t ParseJoinHostA(const char* s) {
    const char* k = s ? strstr(s, "+tsnet_host") : NULL;
    return k ? _strtoui64(k + 11, NULL, 10) : 0;
}

// Why a transport's Open() failed, in words for the player's banner (NetOpen shows it).
// Each failure path sets it; NULL means no specific reason was found.
static const wchar_t* g_netOpenWhy;

// --- transport -------------------------------------------------------------

// Plain UDP: receives on local_port, sends to peer_host:peer_port. The one-PC tests run two
// copies of the game against 127.0.0.1. sim_latency_ms and sim_loss_pct fake a bad network:
// Send drops packets at random and holds the rest in q until they're due (Pump sends them).
struct UdpTransport : NetTransport {
    SOCKET s = INVALID_SOCKET;
    sockaddr_in peer = {};
    struct Delayed { ULONGLONG due; int len; char data[512]; };
    Delayed q[512]; int qn = 0;                     // packets held back by sim_latency_ms, oldest first
    uint32_t lcg = 0x1234567;                       // the fake packet loss's dice (fixed seed)
    // A non-blocking socket bound to local_port. False if any step fails: NetOpen then
    // stays single player.
    bool Open() override {
        g_netOpenWhy = L"couldn't open the local network port (is another copy of the game using it?)";
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w)) return false;
        s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) return false;
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        // A repair digest in a big level is ~50 datagrams sent at once; Windows' default
        // UDP buffers drop most of such a burst (Toy Box one-PC test, build 42).
        int big = 1 << 20;
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&big, sizeof big);
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&big, sizeof big);
        sockaddr_in me = {};
        me.sin_family = AF_INET;
        me.sin_port = htons((u_short)g_net.localPort);
        me.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, (sockaddr*)&me, sizeof me)) return false;
        peer.sin_family = AF_INET;
        peer.sin_port = htons((u_short)g_net.peerPort);
        return InetPtonW(AF_INET, g_net.peerHost, &peer.sin_addr) == 1;
    }
    void RawSend(const void* d, int n) { sendto(s, (const char*)d, n, 0, (sockaddr*)&peer, sizeof peer); }
    // The normal path: fake loss, then the fake latency queue (when it has room and the
    // packet fits), else straight out.
    void Send(const void* d, int n) override {
        if (g_net.simLossPct > 0) {
            lcg = lcg * 1664525u + 1013904223u;
            if ((int)((lcg >> 8) % 100) < g_net.simLossPct) return;
        }
        if (g_net.simLatencyMs > 0 && qn < 512 && n <= 512) {
            q[qn].due = GetTickCount64() + g_net.simLatencyMs;
            q[qn].len = n;
            memcpy(q[qn].data, d, n);
            qn++;
            return;
        }
        RawSend(d, n);
    }
    // Skips the latency queue (the fake loss still applies).
    void SendNow(const void* d, int n) override {
        if (g_net.simLossPct > 0) {
            lcg = lcg * 1664525u + 1013904223u;
            if ((int)((lcg >> 8) % 100) < g_net.simLossPct) return;
        }
        RawSend(d, n);
    }
    void Flush() override { for (int i = 0; i < qn; i++) RawSend(q[i].data, q[i].len); qn = 0; }   // the whole queue, due or not
    // Sends the queued packets whose delay is over (NetPump calls this).
    void Pump() override {
        ULONGLONG now = GetTickCount64();
        int k = 0;
        for (int i = 0; i < qn; i++) {
            if (q[i].due <= now) RawSend(q[i].data, q[i].len);
            else q[k++] = q[i];
        }
        qn = k;
    }
    // One waiting datagram. One from any address but the peer's is thrown away, and that
    // call returns 0 (nothing) like an empty socket.
    int Recv(void* buf, int cap) override {
        sockaddr_in from = {};
        int fl = sizeof from;
        int n = recvfrom(s, (char*)buf, cap, 0, (sockaddr*)&from, &fl);
        if (n <= 0) return 0;
        if (from.sin_addr.s_addr != peer.sin_addr.s_addr || from.sin_port != peer.sin_port) return 0;
        return n;
    }
    const char* Name() override { return "udp"; }
};

// --- Steam transport --------------------------------------------------------
//
// ISteamNetworkingMessages (v002) from the game's own steam_api64.dll, via its
// flat C exports. Messages go to the other player's Steam ID; Steam relays
// handle NAT, so nobody forwards ports. Sending to the peer implicitly accepts
// their session, and both sides send HELLO, so no Steam callbacks are needed.
// The game pumps SteamAPI_RunCallbacks itself.

struct SteamNetIdentity { int32_t type; int32_t size; uint8_t data[128]; };     // SteamNetworkingIdentity
static_assert(sizeof(SteamNetIdentity) == 136, "SteamNetworkingIdentity size");
struct SteamNetMsg {                                                             // SteamNetworkingMessage_t
    void* pData; int32_t cbSize; uint32_t conn; SteamNetIdentity peer;
    int64_t connUserData, usecTimeReceived, messageNumber;
    void* pfnFreeData; void* pfnRelease; int32_t channel, flags; int64_t userData; uint16_t lane;
};

// Steam's own networking messages (connection attempts, route finding),
// copied into the log. Called on Steam's threads; Log's stream is locked.
static volatile LONG g_steamDbgLines;
static void SteamDebugOut(int type, const char* msg) {
    LONG n = InterlockedIncrement(&g_steamDbgLines);
    if (n > 400) return;
    char line[600];
    strncpy_s(line, msg ? msg : "", _TRUNCATE);
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
    Log("steam[%d]: %s%s", type, line, n == 400 ? " (no more Steam messages logged)" : "");
}

// Steam's ESteamNetworkingAvailability values in words, for the relay status line.
static const char* RelayAvailName(int a) {
    switch (a) {
    case -102: return "cannot try"; case -101: return "failed"; case -100: return "failed before";
    case -10: return "retrying"; case 1: return "never tried"; case 2: return "waiting";
    case 3: return "attempting"; case 100: return "OK";
    default: return "unknown";
    }
}

// Steam's callback base, declared exactly like steam_api's CCallbackBase so the
// vtable matches (MSVC orders the Run overloads the same way for both).
class SteamCallbackBase {
public:
    virtual void Run(void* param) = 0;
    virtual void Run(void* param, bool ioFailure, uint64_t call) = 0;
    virtual int GetCallbackSizeBytes() = 0;
    uint8_t m_nCallbackFlags = 0;
    int m_iCallback = 0;
};
// GameRichPresenceJoinRequested_t (337): the player clicked "Join Game" or
// accepted an invite while this game was already running.
// SteamTransport::Open registers it; Steam calls it from SteamAPI_RunCallbacks, which the
// game pumps itself. It only stores the host's Steam ID in g_netJoinReq; NetFrameBody
// (net_lockstep.cpp) acts on it on the next frame.
struct JoinRequestedCb : SteamCallbackBase {
    void Run(void* p) override { Handle(p); }
    void Run(void* p, bool, uint64_t) override { Handle(p); }
    int GetCallbackSizeBytes() override { return 8 + 256; }
    // The callback data: the friend's Steam ID (8 bytes), then the 256-byte connect string.
    static void Handle(void* p) {
        if (!p) return;
        char connect[257];
        memcpy(connect, (const char*)p + 8, 256);
        connect[256] = 0;
        uint64_t host = ParseJoinHostA(connect);
        Log("netplay: Steam join request from %llu: \"%s\"", *(uint64_t*)p, connect);
        if (host) InterlockedExchange64(&g_netJoinReq, (LONG64)host);
    }
};
static JoinRequestedCb g_joinCb;

// The transport real players use. Every Steam call is looked up by name in the game's
// steam_api64.dll at Open (no Steam SDK headers or import library): the networking-messages
// interface for the packets, networking utils for the relays and Steam's own log, SteamUser
// for this player's ID, and SteamFriends for names, rich presence ("Join Game") and the
// invite dialog. A host with no peer_steamid takes whichever friend messages it first (Pump).
struct SteamTransport : NetTransport {
    // The flat C API's signatures: the first argument is the interface object.
    typedef void* (*PFN_Iface)();
    typedef int (*PFN_Send)(void*, const SteamNetIdentity*, const void*, uint32_t, int, int);
    typedef int (*PFN_Recv)(void*, int, SteamNetMsg**, int);
    typedef void (*PFN_SetId)(SteamNetIdentity*, uint64_t);
    typedef uint64_t (*PFN_GetId)(SteamNetIdentity*);
    typedef void (*PFN_Release)(SteamNetMsg*);
    typedef void (*PFN_InitRelay)(void*);
    typedef uint64_t (*PFN_UserId)(void*);
    typedef const char* (*PFN_MyName)(void*);
    typedef const char* (*PFN_FriendName)(void*, uint64_t);
    typedef int (*PFN_SessInfo)(void*, const SteamNetIdentity*, void*, void*);
    typedef bool (*PFN_Accept)(void*, const SteamNetIdentity*);
    typedef int (*PFN_RelayStatus)(void*, void*);
    typedef bool (*PFN_SetDebugOut)(void*, int, void (*)(int, const char*));
    typedef int (*PFN_FriendCount)(void*, int);
    typedef uint64_t (*PFN_FriendByIndex)(void*, int, int);
    typedef bool (*PFN_SetRP)(void*, const char*, const char*);
    typedef void (*PFN_RegisterCb)(void*, int);
    typedef void (*PFN_InviteDialog)(void*, const char*);
    PFN_InviteDialog inviteDialog = NULL;
    char connectStr[64] = "";                         // "+tsnet_host <this player's Steam ID>"
    void* msgs = NULL;
    PFN_Send send = NULL; PFN_Recv recv = NULL; PFN_GetId getId = NULL; PFN_Release release = NULL;
    PFN_SessInfo sessInfo = NULL;
    PFN_Accept accept = NULL;
    void* utilsObj = NULL;
    PFN_RelayStatus relayStatus = NULL;
    int lastSendRes = -999, sendResLogs = 0;
    ULONGLONG lastAccept = 0;
    bool acceptLogged = false;
    SteamNetIdentity peer = {};
    PFN_SetId setIdFn = NULL;
    void* friends = NULL;
    PFN_FriendName friendName = NULL;
    PFN_FriendCount friendCount = NULL;
    PFN_FriendByIndex friendByIndex = NULL;
    PFN_SetRP setRP = NULL;
    ULONGLONG lastScan = 0;
    // Steam's k_nSteamNetworkingSend_* flags, and k_EFriendFlagImmediate (regular friends)
    // for the friend list.
    enum { kUnreliableNoNagle = 1, kReliable = 8, kAutoRestart = 32, kFriendFlagImmediate = 4 };

    // The other player's Steam name, into g_netPeerNameW for the on-screen messages.
    void NoteName(uint64_t id) {
        const char* n = friendName && friends && id ? friendName(friends, id) : NULL;
        if (n && n[0]) MultiByteToWideChar(CP_UTF8, 0, n, -1, g_netPeerNameW, _countof(g_netPeerNameW));
    }
    // Talk to this Steam ID from now on: a Join Game request (NetFrameBody), or the first
    // friend to message a waiting host (Pump).
    void SetPeer(uint64_t id) override {
        g_net.peerSteamId = id;
        if (setIdFn) setIdFn(&peer, id);
        NoteName(id);
        acceptLogged = false;
    }
    // A session started, or this player is joining one: friends no longer see "Join Game".
    void SessionStarted() override {
        if (setRP && friends) {
            setRP(friends, "connect", "");                       // two players: no third joining a session in progress
            setRP(friends, "status", "Toy Story 3 online co-op");
        }
    }
    // Friends see "Join Game" on the host (and Steam's "Invite to Game" sends this string);
    // joining starts their game with it, or reaches a running one as a join request.
    void Advertise() override {
        if (g_net.role != 0 || !setRP || !friends || !connectStr[0]) return;
        setRP(friends, "connect", connectStr);
        setRP(friends, "status", "Toy Story 3 online co-op: waiting for a player");
        Log("netplay: Steam friends can now Join Game (connect \"%s\")", connectStr);
    }
    // Steam's overlay "invite a friend" dialog for this game, carrying the connect string.
    bool OpenInvite() override {
        if (!inviteDialog || !friends || !connectStr[0]) return false;
        inviteDialog(friends, connectStr);
        return true;
    }

    // Finds the Steam calls and sets up the peer, names, Join Game and the join callback.
    // False (reason logged, and put in g_netOpenWhy for the banner) when Steam or its
    // networking isn't there, or when peer_steamid is this player's own ID.
    // A client with no host ID is fine: it waits for a Steam join request (an accepted invite,
    // or Join Game on a friend's name while the game runs), which NetFrameBody picks up from
    // g_netJoinReq. A release's settings file has no ID in it, so this is the usual way to join.
    bool Open() override {
        HMODULE st = GetModuleHandleW(L"steam_api64.dll");
        if (!st) {
            Log("netplay: steam_api64.dll isn't loaded");
            g_netOpenWhy = L"the game isn't running through Steam (start it from Steam)";
            return false;
        }
        // The networking-messages calls: the first six are required.
        auto get = [&](const char* n) { return (void*)GetProcAddress(st, n); };
        PFN_Iface iface = (PFN_Iface)get("SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
        send = (PFN_Send)get("SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
        recv = (PFN_Recv)get("SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
        PFN_SetId setId = (PFN_SetId)get("SteamAPI_SteamNetworkingIdentity_SetSteamID64");
        getId = (PFN_GetId)get("SteamAPI_SteamNetworkingIdentity_GetSteamID64");
        release = (PFN_Release)get("SteamAPI_SteamNetworkingMessage_t_Release");
        sessInfo = (PFN_SessInfo)get("SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo");   // optional: diagnostics
        accept = (PFN_Accept)get("SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");             // optional
        if (!iface || !send || !recv || !setId || !getId || !release) {
            Log("netplay: this steam_api64.dll lacks the networking calls");
            g_netOpenWhy = L"this copy of the game's Steam library has no online networking";
            return false;
        }
        msgs = iface();
        if (!msgs) {
            Log("netplay: Steam networking isn't available (is Steam running and signed in?)");
            g_netOpenWhy = L"Steam networking isn't available (is Steam running and signed in?)";
            return false;
        }
        setIdFn = setId;
        if (!g_net.peerSteamId && g_net.role == 1)
            Log("netplay: joining, no host ID yet: waiting for a Steam invite to be accepted or for Join Game");
        if (g_net.peerSteamId) setId(&peer, g_net.peerSteamId);

        // Networking utils (optional): Steam's log, the relay status, and starting the relay
        // connection now rather than at the first message.
        if (PFN_Iface utils = (PFN_Iface)get("SteamAPI_SteamNetworkingUtils_SteamAPI_v004"))
            utilsObj = utils();
        if (utilsObj) {
            // Steam's own messages first, so the relay start-up is in the log too. 5 = "Msg" detail.
            if (PFN_SetDebugOut dbg = (PFN_SetDebugOut)get("SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction"))
                dbg(utilsObj, 5, SteamDebugOut);
            relayStatus = (PFN_RelayStatus)get("SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus");
            if (PFN_InitRelay init = (PFN_InitRelay)get("SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess"))
                init(utilsObj);
        }
        // This player's Steam ID, and the friends interface: names, the friend list (Pump),
        // rich presence and the invite dialog.
        uint64_t me = 0;
        if (PFN_Iface user = (PFN_Iface)get("SteamAPI_SteamUser_v023"))
            if (PFN_UserId id = (PFN_UserId)get("SteamAPI_ISteamUser_GetSteamID"))
                if (void* u = user()) me = id(u);
        g_netMyId = me;
        const char* myName = "?"; const char* theirName = "?";
        if (PFN_Iface fr = (PFN_Iface)get("SteamAPI_SteamFriends_v018"))
            if (void* f = fr()) {
                friends = f;
                friendName = (PFN_FriendName)get("SteamAPI_ISteamFriends_GetFriendPersonaName");
                friendCount = (PFN_FriendCount)get("SteamAPI_ISteamFriends_GetFriendCount");
                friendByIndex = (PFN_FriendByIndex)get("SteamAPI_ISteamFriends_GetFriendByIndex");
                setRP = (PFN_SetRP)get("SteamAPI_ISteamFriends_SetRichPresence");
                inviteDialog = (PFN_InviteDialog)get("SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialogConnectString");
                if (PFN_MyName n = (PFN_MyName)get("SteamAPI_ISteamFriends_GetPersonaName")) myName = n(f);
                if (friendName && g_net.peerSteamId) theirName = friendName(f, g_net.peerSteamId);
            }
        if (g_net.peerSteamId) NoteName(g_net.peerSteamId);
        Log("netplay: Steam: this player %s (%llu), other player %s (%llu)%s", myName, me,
            g_net.peerSteamId ? theirName : "(anyone who joins)", g_net.peerSteamId,
            g_netLaunchedByJoin ? "; started by Steam Join Game" : "");
        if (me && me == g_net.peerSteamId) {
            Log("netplay: peer_steamid is this player's own Steam ID");
            g_netOpenWhy = L"the partner's Steam ID in your settings is your own (enter your friend's, or leave it at 0)";
            return false;
        }
        // Join requests while the game is running (Join Game / invites).
        if (PFN_RegisterCb reg = (PFN_RegisterCb)get("SteamAPI_RegisterCallback")) {
            g_joinCb.m_iCallback = 337;
            reg(&g_joinCb, 337);
        }
        // The connect string friends join with; a host advertises it at once.
        if (me) snprintf(connectStr, sizeof connectStr, "+tsnet_host %llu", me);
        Advertise();
        if (g_net.role == 0) Log("netplay: Steam invite dialog %s (hold Back + Y while waiting)", inviteDialog ? "available" : "NOT available");
        return true;
    }
    // Steam's EResult for a send: 1 is OK. Logged when it changes (at most 12 times).
    void NoteSend(int r) {
        if (r == lastSendRes || sendResLogs >= 12) return;
        lastSendRes = r;
        sendResLogs++;
        const char* what = r == 1 ? "OK" : r == 2 ? "failed" : r == 3 ? "no connection" : r == 8 ? "invalid parameter"
                         : r == 25 ? "limit exceeded" : r == 5 ? "invalid password" : r == 16 ? "timeout" : "see Steam EResult";
        Log("netplay: Steam send result %d (%s)", r, what);
    }
    // The other player's first message arrives as a session request. Sending
    // to them accepts it implicitly; accepting it explicitly as well costs nothing.
    void Pump() override {
        if (!accept) return;
        ULONGLONG now = GetTickCount64();
        // No partner yet: accept the first friend who reaches us. For a host that's whoever
        // joins; for a joining player with no host ID, a host who has this player's ID set.
        if (!g_net.peerSteamId) {
            // Once a second, try to accept a session from each friend. Accepting only
            // succeeds for someone whose session request is waiting, so the first friend
            // whose message has arrived becomes the peer.
            if (now - lastScan < 1000 || !friends || !friendCount || !friendByIndex || !setIdFn) return;
            lastScan = now;
            int n = friendCount(friends, kFriendFlagImmediate);
            for (int i = 0; i < n && i < 2000; i++) {
                uint64_t id = friendByIndex(friends, i, kFriendFlagImmediate);
                SteamNetIdentity who = {};
                setIdFn(&who, id);
                if (id && accept(msgs, &who)) {
                    SetPeer(id);
                    char name[128] = "?";
                    WideCharToMultiByte(CP_UTF8, 0, g_netPeerNameW, -1, name, sizeof name, NULL, NULL);
                    Log(g_net.role == 0 ? "netplay: %s (%llu) joined; they are player 2"
                                        : "netplay: %s (%llu) reached us; they are the host, player 1", name, id);
                    break;
                }
            }
            return;
        }
        if (now - lastAccept < 1000) return;
        lastAccept = now;
        if (accept(msgs, &peer) && !acceptLogged) { acceptLogged = true; Log("netplay: accepted the other player's Steam session request"); }
    }
    // Unreliable (no resends by Steam): an input packet repeats the last kNetRedundancy
    // frames anyway, so one lost costs nothing.
    void Send(const void* d, int n) override {
        if (!g_net.peerSteamId) return;                               // hosting: nobody to talk to yet
        NoteSend(send(msgs, &peer, d, (uint32_t)n, kUnreliableNoNagle | kAutoRestart, 0));
    }
    void SendNow(const void* d, int n) override { Send(d, n); }   // no queue of ours to skip
    // Steam's reliable mode, for what must arrive: the settings sheet, repair messages,
    // the drop-in save and its acks.
    void SendReliable(const void* d, int n) override {
        if (!g_net.peerSteamId) return;
        NoteSend(send(msgs, &peer, d, (uint32_t)n, kReliable | kAutoRestart, 0));
    }
    // The next message on channel 0 from the other player (at most cap bytes copied);
    // messages from anyone else are released and skipped. 0 = nothing waiting.
    int Recv(void* buf, int cap) override {
        for (;;) {
            SteamNetMsg* m = NULL;
            if (recv(msgs, 0, &m, 1) != 1 || !m) return 0;
            int n = 0;
            if (getId(&m->peer) == g_net.peerSteamId) {               // only the configured player
                n = m->cbSize < cap ? m->cbSize : cap;
                memcpy(buf, m->pData, n);
            }
            release(m);
            if (n > 0) return n;
        }
    }
    // Steam's view of the link to the other player, and its reason when it
    // failed. SteamNetConnectionInfo_t (pack 8): eState at +176, szEndDebug
    // at +184; the reason is only used when +176 matches the return value.
    int LinkState(char* why, size_t cap) override {
        if (why && cap) why[0] = 0;
        if (!sessInfo || !g_net.peerSteamId) return -1;
        alignas(8) BYTE info[1024] = {};
        int st = sessInfo(msgs, &peer, info, NULL);
        if (why && cap && *(int*)(info + 176) == st && info[184]) {
            char tmp[129];
            memcpy(tmp, info + 184, 128);
            tmp[128] = 0;
            strncpy_s(why, cap, tmp, _TRUNCATE);
        }
        return st;
    }
    // SteamRelayNetworkStatus_t: overall, ping in progress, network config,
    // any relay (4 ints), then a 256-byte debug message.
    void RelayStatus(char* out, size_t cap) override {
        if (!out || !cap) return;
        out[0] = 0;
        if (!relayStatus || !utilsObj) return;
        alignas(8) BYTE st[512] = {};
        int a = relayStatus(utilsObj, st);
        int* v = (int*)st;
        char dbg[257];
        memcpy(dbg, st + 16, 256);
        dbg[256] = 0;
        snprintf(out, cap, "relays %s (config %s, any relay %s%s)%s%s", RelayAvailName(a), RelayAvailName(v[2]),
                 RelayAvailName(v[3]), v[1] ? ", measuring pings" : "", dbg[0] ? ": " : "", dbg);
    }
    const char* Name() override { return "steam"; }
};

NetTransport* g_netT;           // the open transport (NULL when netplay is off or failed to open)

// Game data both PCs must share: every file under assets\ and DATA_DX\, by
// relative path and size (no reads, so it's quick). A different game version
// or a modified archive is refused at connect instead of desyncing later.
static void CollectDataFiles(const std::wstring& root, const std::wstring& rel, std::vector<std::pair<std::wstring, uint64_t>>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + rel + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        std::wstring r = rel + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) CollectDataFiles(root, r, out);
        else {
            for (auto& ch : r) ch = (wchar_t)towlower(ch);
            out.push_back({ r, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow });
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
// FNV-1a over the sorted (lower-case path, size) list, so the order the folders are walked
// in doesn't matter. *files gets the file count.
static uint32_t GameDataFingerprint(size_t* files) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    std::vector<std::pair<std::wstring, uint64_t>> list;
    CollectDataFiles(dir, L"\\assets", list);
    CollectDataFiles(dir, L"\\data_dx", list);
    std::sort(list.begin(), list.end());
    uint32_t h = 0x811C9DC5;
    for (auto& e : list) {
        for (wchar_t ch : e.first) { h = (h ^ (uint32_t)ch) * 16777619u; }
        for (int k = 0; k < 8; k++) h = (h ^ (uint32_t)((e.second >> (k * 8)) & 0xff)) * 16777619u;
    }
    *files = list.size();
    return h;
}

// Starts netplay: the settings sheet and its hash, the transport, the helper threads, and
// the first message on screen. Called once, on the game's main thread, from the first
// frame poll (NetFrameBody). If the transport won't open, the game plays on single player.
void NetOpen() {
    // The settings sheet (see "settings sheet" in net_session.cpp): everything both PCs must share,
    // one named line each, so a mismatch can say what differs. The fingerprint in every
    // packet is its hash.
    g_netSheet.clear();
    auto line = [](const char* key, const char* fmt, ...) {
        char v[64];
        va_list ap;
        va_start(ap, fmt);
        _vsnprintf_s(v, _countof(v), _TRUNCATE, fmt, ap);
        va_end(ap);
        g_netSheet += key; g_netSheet += '='; g_netSheet += v; g_netSheet += '\n';
    };
    auto num = [&](const char* key, double d) {        // shortest text that reads back as the same double
        char v[32];
        for (int prec = 1; prec <= 17; prec++) {
            sprintf_s(v, "%.*g", prec, d);
            if (strtod(v, NULL) == d) break;
        }
        line(key, "%s", v);
    };
    // Versions: this mod's build and protocol, the game exe's link time stamp, the game data.
    BYTE* base = (BYTE*)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    line("build", "%u", kYeeHawBuild);                 // 95: + this sheet
    line("proto", "%u", kNetVersion);
    line("exe", "%08x", nt->FileHeader.TimeDateStamp); // same game build
    {
        size_t files = 0;
        ULONGLONG t0 = GetTickCount64();
        uint32_t data = GameDataFingerprint(&files);
        line("files", "%08x/%zu", data, files);
        Log("netplay: game data both PCs must share: %zu files under assets and DATA_DX, fingerprint %08x (%llu ms)",
            files, data, GetTickCount64() - t0);
    }
    // Netplay and level options both PCs must agree on.
    line("input_delay", "%d", g_net.delay);
    line("own_screen", "%d", g_net.ownScreen);         // own screen also sets the split-screen scale
    line("drop_in", "%d", g_net.dropIn);
    line("hidden_levels", "%d", g_levelsWanted ? 1 : 0);
    line("script_fixes", "%d", g_scriptFixesWanted && g_scriptFixesOk ? 1 : 0);   // the 2010 script fixes change the simulation
    // Graphics mod: these values change which actors and world chunks are
    // simulated at what detail, so both PCs must share them. That includes the
    // shadow-caster culls: like the main view's, each cascade's cull fires the
    // "became visible" event that animation reads (decomp session), so everything
    // that changes which casters a cascade returns counts. Everything else in
    // [graphics] (fps limit, shadow resolution and filtering, reflections, glow,
    // AA, AO...) only changes the picture and may differ. Values are the ones in
    // effect: with [graphics] enabled=0 they're the game's own (1, or -1 = not set).
    {
        bool on = g_gfx.enabled != 0;
        line("~graphics", "%d", on ? 1 : 0);           // not hashed: explains why the values below read "game"
        num("lod_scale", on ? g_gfx.lodScale : 1.0);
        num("world_lod_scale", on ? g_gfx.worldLodScale : 1.0);
        num("object_fade_scale", on ? g_gfx.objectFadeScale : 1.0);
        num("cull_pop", on && g_gfx.hasCullPop ? g_gfx.cullPop : -1.0);
        num("cull_fade", on && g_gfx.hasCullFade ? g_gfx.cullFade : -1.0);
        static const char* const scKey[3] = { "shadow_cull_near", "shadow_cull_mid", "shadow_cull_far" };
        for (int i = 0; i < 3; i++)                    // shadow caster culls: almost surely render-only, kept to be safe
            num(scKey[i], on && g_gfx.hasShadowCull[i] ? g_gfx.shadowCull[i] : -1.0);
        line("shadow_all_zones", "%d", on ? g_gfx.shadowAllZones : 0);
        line("shadow_no_contrib", "%d", on ? g_gfx.shadowNoContrib : 0);
        num("shadow_near_extend", on ? g_gfx.shadowNearExtend : 0.0);
        // shadow_far / _cascades / _split are applied by gfx_d3d12.cpp whatever [graphics] enabled says.
        num("shadow_far", g_gfx.shadowFar);
        num("shadow_cascades", g_gfx.shadowCascades);
        num("shadow_split", g_gfx.shadowSplit);
    }
    // The determinism layer's settings (sim_determinism.cpp, net_determinism.cpp) and the
    // fingerprint and repair settings.
    num("fps", g_cfg.fps);
    line("rand_isolate", "%d", g_cfg.randIsolate);
    line("spawn_gate", "%d", g_spawnGateOn ? 1 : 0);
    line("spawn_delay", "%d", g_spawnGateOn ? g_cfg.spawnDelay : 0);
    line("sync_loader", "%d", g_syncLoaderOn ? 1 : 0);
    line("audio_wait", "%d", g_audioWaitOn ? 1 : 0);
    line("hash_interval", "%d", g_net.hashInterval);
    line("repair", "%d", g_net.repair);                // both PCs must agree on repair
    line("repair_lead", "%d", g_net.repairLead);
    line("repair_followers", "%d", g_net.repairFollowers != 0 ? 1 : 0);
    // The determinism patches that applied: each is code-checked first, so one can
    // fail on one PC only (another game build, another mod). One bit each.
    const bool fixes[] = { g_engineClockReady, g_attractOff, g_mtReseedOk, g_musicSyncOk, g_zeroAllocOk, g_aspectOk,
                           g_visSyncOk, g_screenZOk, g_boneFlushOk, g_regionFixOk, g_coinFlipOk, g_lineGateOk, g_fadeOnMain,
                           g_levelsFileListOk, g_levelsWorldDbOk, g_galleryButtonsOk, g_bankListOk, g_bankRenameOk,
                           g_movieFixOn, g_rbZeroOn, g_feHooked, g_fma3Off, g_noDesktopInput };
    uint32_t fixBits = 0;
    for (size_t i = 0; i < _countof(fixes); i++) if (fixes[i]) fixBits |= 1u << i;
    line("fixes", "%06x", fixBits);
    g_netSettings = 0x811C9DC5;                        // FNV-1a of every line but the '~' ones
    for (const auto& kv : SheetParse(g_netSheet)) {
        if (kv.first[0] == '~') continue;
        for (char ch : kv.first + "=" + kv.second + "\n") g_netSettings = (g_netSettings ^ (uint8_t)ch) * 16777619u;
    }
    {                                                  // the whole sheet on one log line
        std::string flat = g_netSheet;
        std::replace(flat.begin(), flat.end(), '\n', ' ');
        Log("netplay: settings both PCs must share (%zu bytes): %s", g_netSheet.size(), flat.c_str());
    }
    // Open the connection ([netplay] transport: udp or steam).
    if (g_net.transport == 0) g_netT = new UdpTransport();
    else g_netT = new SteamTransport();
    if (!g_netT || !g_netT->Open()) {
        Log("netplay: couldn't open the %s connection; staying single player", g_net.transport ? "Steam" : "UDP");
        Status(15000, kOvRed, L"Online co-op couldn't start: %s.\nPlaying alone.",
               g_netOpenWhy ? g_netOpenWhy : L"the connection couldn't be opened (the log has details)");
        DebugMenu_SetNetplay(false);
        delete g_netT;                                     // half-open: its calls may be unset
        g_netT = NULL;
        g_netPhase = NP_OFF;
        return;
    }
    // Connecting: NetFrameBody says HELLO every 250 ms until the other PC answers.
    g_netPhase = NP_CONNECTING;
    g_netLastHeard = g_netPhaseSince = GetTickCount64();
    DebugMenu_SetNetplay(false);                       // alone until someone connects
    // One-PC test with real controllers: one shared controller, or one each?
    if (g_localTest >= 0 && !g_net.testInputSeed) {
        bool two = g_localControllers == 2;
        if (g_localControllers == 0) {                       // auto: is there a second controller?
            XINPUT_STATE t;
            two = XInputGetState((DWORD)g_localClientPad, &t) == ERROR_SUCCESS;
        }
        if (!two) {
            g_sharedPad = true;
            if (g_localTest == 1) g_net.localPad = g_localHostPad;    // both copies read player 1's controller
            g_sharedActive = g_localTest == 0;
            Log("local co-op test: one controller (pad %d) shared; Back + RB hands it between player 1 and player 2", g_net.localPad);
        } else {
            Log("local co-op test: two controllers (player 1 pad %d, player 2 pad %d)", g_localHostPad, g_localClientPad);
        }
    }
    // A goodbye however the game exits (net_session.cpp), the keepalive thread (a BUSY
    // packet whenever nothing has been sent for half a second, as during a long load) and
    // the profiler that samples the main thread during lockstep frames over 0.4 s.
    InstallNetExitHooks();
    if (HANDLE kt = CreateThread(NULL, 0, NetKeepaliveThread, NULL, 0, NULL)) CloseHandle(kt);
    if (HANDLE pt = CreateThread(NULL, 0, LoadProfilerThread, NULL, 0, NULL)) CloseHandle(pt);
    Log("netplay: %s over %s, settings fingerprint %08x; connecting", g_net.role ? "client / player 2" : "host / player 1",
        g_netT->Name(), g_netSettings);
    // What the player sees while nobody is connected yet. Without a partner ID, a host waits for
    // any friend and a joining player waits for an invite: both just play on meanwhile.
    if (g_net.role == 0 && !g_net.peerSteamId)
        Status(12000, kOvBlue, L"Online co-op: hosting. Play on while you wait.\nYour friend joins from Steam: Friends > your name > Join Game.");
    else if (g_net.role == 1 && !g_net.peerSteamId)
        Status(15000, kOvBlue, L"Online co-op: ready to join a friend. Play on while you wait.\n"
                               L"Accept their Steam invite, or in Steam: Friends > their name > Join Game.");
    else
        Status(0, kOvBlue, L"Online co-op: connecting to %s...", g_netPeerNameW);
}

// At process exit (CloseRun, xinput_proxy.cpp): the session's totals into the log and, if a
// session is still running, a last word to the other PC so its game ends the session cleanly
// ("the other player left") instead of waiting for a timeout.
void NetSummary() {
    if (g_netPhase == NP_OFF) return;
    // At process exit the keepalive thread is already gone, maybe mid-send: never wait for the lock here.
    bool locked = TryEnterCriticalSection(NetLockObj()) != FALSE;
    struct Unlock { bool on; ~Unlock() { if (on) LeaveCriticalSection(NetLockObj()); } } unlock = { locked };
    if (!locked) Log("netplay: network busy at exit; no goodbye sent");
    Log("summary: netplay %s; %u lockstep frames, %llu frames waited for the other PC (%llu ms total, longest %llu ms), "
        "%llu fingerprints matched, %llu packets out, %llu in", g_netPhase == NP_ENDED ? g_netEndReason : "still running at exit",
        g_netFrame, g_netStalls, g_netStallMs, g_netMaxStallMs, g_netHashesChecked, g_netPacketsOut, g_netPacketsIn);
    if (g_rngSlips || g_rngAsks || g_rngCatchUps)
        Log("summary: rng check: %llu random-number slips caught, %llu fixed locally (catch-up), %llu early repairs asked for",
            g_rngSlips, g_rngCatchUps, g_rngAsks);
    if (g_repairOn || g_repRounds)
        Log("summary: repair: %llu rounds, %llu items repaired, %llu missed (asked too late), %llu unrepairable",
            g_repRounds, g_repItemsFixed, g_repMissedCount, g_repUnrepairable);
    if (g_boneFlushOk) Log("summary: bone flush: %llu dirty nodes pushed into physics before a step", g_boneFlushNodes);
    // Anything queued, our last inputs again (the other PC may still need them to finish
    // its frames), then BYE three times, since it travels unreliably.
    if (locked && g_netPhase == NP_RUNNING && g_netT) {
        g_netT->Flush();
        if (g_netLocalSentUpTo != 0xFFFFFFFF) { NetSendInputs(g_netLocalSentUpTo); g_netT->Flush(); }
        NetByePkt bye = { NetHeader(NET_BYE), g_netLocalSentUpTo };
        for (int i = 0; i < 3; i++) g_netT->SendNow(&bye, sizeof bye);
    }
}
