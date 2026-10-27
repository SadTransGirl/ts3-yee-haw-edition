# Online co-op: design

Lockstep netplay built into the `xinput1_4.dll` proxy, on top of the
determinism work in `README.md`. Both PCs run the full game; only controller
input crosses the network. Decided with the user on 2026-10-23:

- **Connection:** Steam peer-to-peer (`ISteamNetworkingMessages`, in the game's
  own `steam_api64.dll`). No port forwarding; Steam relays through routers.
- **Testing:** the user and a friend, each on their own PC, over the internet.
- **First goal:** Toy Box co-op, each player seeing their own character full
  screen.

## How it works

### Roles and controllers

- The **host** is player 1, the **client** player 2. Both run the same
  settings and start from the host's save.
- On both PCs the game sees two controllers: pad 0 is the host's input and
  pad 1 is the client's input. Each PC reads its own physical controller from
  the real pad 0 and sends it to the other.
- Pad 1 always exists in the game as device slot 2. The proxy reports it as
  connected (`XInputGetCapabilities(1)`) while a peer is connected, so the
  game's own drop-in works: the client presses Start, picks a character, and
  becomes player 2 (`re\COOP_NOTES.md`, "Join flow").

### Lockstep

- The lockstep frame is the input poll (`XInputGetState(0)` is called once
  per game frame on the main thread), counted from session start.
- Each PC sends its input for frame `N + delay` (default delay: 4 frames, about
  67 ms) and, before answering the game's poll for frame `N`, waits until the
  peer's input for frame `N` has arrived. The game then gets the same pad 0
  and pad 1 on both PCs, on the same frame.
- Inputs are sent redundantly (the last 8 frames in every packet) over the
  unreliable channel, so a lost packet costs nothing and a late one only stalls.
- If nothing arrives for 10 s, the session ends: pad 1 is reported
  disconnected and the game carries on single player.

### What makes both PCs compute the same frames

Already in the harness, all required in netplay:

- **Engine clock** (`engine_clock`): every game frame advances exactly 1/60 s.
- **Per-site `rand`** (`rand_isolate`).
- **Spawn gate** (`spawn_gate`): streamed spawns placed a fixed number of
  frames after they're queued.

New for netplay:

- **Synchronous loader.** The loader's per-frame time slice is wall-clock
  (`Engine_FrameTick` -> `Loader_YieldBudget(...)`), so loading screens take a
  different number of frames on different PCs, and the board's physics bodies
  can be added in a different order (run14 replay 1). In netplay the loader is
  given an unlimited budget each frame, so it finishes all queued work before
  the frame ends: same work, same frames, same order on both PCs. Streaming
  during play may cause small hitches.
- **Clock reset at session start.** Both PCs set game time to the same value
  when lockstep begins, since time before that depends on how long each
  intro took.

### Session start

1. Both players launch the game with the netplay ini (role and the other
   player's Steam ID). The host's save is sent to the client and written to
   its save folder before the game reads it.
2. Each player gets to the title screen ("Press Start") on their own. Until
   lockstep starts, the game gets neutral input except from the local pad.
3. Both hold **Back + Start** on the title screen. When both PCs have seen it,
   lockstep starts on the next frame, with the clock reset. From here on the
   two games run frame for frame.
4. The host picks Load Game and goes to Toy Box; the client's game follows
   identically. In Toy Box the client presses Start to join.

### Desync detection

Every 60 frames each PC sends a fingerprint: engine RNG state, the per-site
`rand` states, the frame counter, spawn sequence, and player positions once
their offsets are confirmed. On a mismatch both log the frame and stop
lockstep with a clear message, instead of drifting silently.

### Own full screen each

`SetSplitScreenLayout` builds one viewport per player and installs them with
`Viewport_SetAll(rects, count)` (`0x1401f7bd0`). On each PC the proxy makes the
local player's viewport full screen and the other zero-sized. This changes
only what's drawn, not the simulation. Still to confirm (decompilation
session): HUD layout, FMOD listener per viewport, cost of a zero-size view,
and that viewport *i* is player *i*.

## Testing without two PCs

The transport is behind an interface with two implementations:

- **Steam** for real sessions.
- **Local UDP** (`127.0.0.1`) for tests: two copies of a stand-in program
  on one PC lockstep with each other, with artificial latency and packet loss.

## Phases

1. **Done.** Lockstep core: input exchange, delay, pad 0/pad 1 mapping, pad 1
   connected, timeouts, session start handshake, desync fingerprints, goodbye
   on quit (hooked `ExitProcess` and `SteamAPI_Shutdown`, carrying the last
   input frame sent so the other side plays through it).
2. **Built, tested with stand-ins only.** Steam transport and save transfer.
   The game reads `Saved Game` only after the title screen, so the host's save
   is sent during the handshake. The client backs up its own save, skips
   files that are already identical, and if its settings file differed, takes
   the host's and asks the player to restart. Not yet tried between two real
   PCs, since the Steam transport needs two accounts.

   Stand-in test (two processes, local UDP, 40 ms lag, 10% loss, one side 3x
   slower): identical frames and all fingerprints matched; a planted desync
   was caught at the next fingerprint; clean quit and crash both handled;
   save transfer exact with a backup; mismatched settings refused.
3. Synchronous loader and clock reset. Tested with record/replay first.
4. Own full screen per player.
5. First online session with a friend.

## Open questions (for the decompilation session)

- Synchronous loader: is patching `Loader_YieldBudget`'s argument in
  `Engine_FrameTick` to `INT64_MAX` safe during loading screens and play?
- When does the game read `Saved Game`: at startup or at Load Game?
- Where is drop-in enabled in Toy Box (`PlayerManager+0x90`), and does it need
  anything else?
- Player avatar transform offsets, for the desync fingerprint.
- The viewport questions above.
