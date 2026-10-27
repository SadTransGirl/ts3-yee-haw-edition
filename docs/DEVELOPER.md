# Developer guide: the determinism harness

Answers one question before anyone spends weeks on an online-co-op mod:
**does this game produce the same result twice when fed the same controller
input?** If two replays of one recording drift apart, a lockstep netcode mod
cannot work without first fixing whatever drifts, and this harness shows where
it starts.

## What it is

`xinput1_4.dll` in the game folder is a proxy. Windows loads it instead of the
system XInput because the game folder comes first in the DLL search order. It
forwards every call to the real `System32\xinput1_4.dll` and additionally:

- **record**: writes every `XInputGetState` result to `runs\<name>\input.bin`
- **replay**: answers `XInputGetState` from that file instead of the controller
- optionally replaces the game's clock with a virtual one that advances exactly
  `1/fps` per polled frame (`fixed_timestep=1`)
- captures the game window every N frames to `shot_NNNNNN.bmp` plus a pixel
  hash in `frames.csv`

- watches how much the game reads from disk each frame and treats heavy
  reading as a **loading screen**: replay pauses the input stream during loads
  so a load that takes longer or shorter than when recording no longer shifts
  every later button press (see below)

`compare_runs.py` diffs two runs and reports the first frame where they differ.

## Fixes from the decompiled game

The reverse engineering in `re\` (see `re\SYNC_VIABILITY.md`) found two
things the harness now uses. Both need `fixed_timestep=1`, and a recording and
its replays must use the same settings; the log warns if they don't.

- **Engine clock** (`engine_clock=1`). All game time comes from one function,
  `Clock_GetTimeMs`. The harness replaces it with its own version through a
  14-byte jump at its start, installed before any game code runs:
  - When the frame timer (`GameTimer_Tick`) reads it, a new frame starts
    exactly 1/fps after the previous one (16, 17, 17 ms at 60 fps). Time
    follows game frames, not controller polls, so fades and transitions that
    don't poll still get frame time.
  - Any other read on the main thread adds 1 µs, or 1 ms once a frame has
    made over 5,000 reads. A loop waiting on game time, such as the 350 ms
    music fade before a level change, finishes after the same number of
    reads every run instead of hanging.
  - Reads from other threads see the time without moving it.
  - Time starts at the same value every run. The engine's unused override
    flag and value are kept in step, because one function reads them directly.

  The real clock is untouched, so the game's own frame limiter keeps normal
  speed. Before patching, the harness checks the exact instructions of the
  clock function and of the frame timer's call. On any other build it patches
  nothing, logs why, and falls back to virtualising `QueryPerformanceCounter`.
  `io.csv` has a `ticks` column: game frames per controller poll.
- **Per-site `rand`** (`rand_isolate=1`). Gameplay, audio and others share the
  C runtime's `rand`. If audio draws a different number of values in two
  runs, every later gameplay value shifts. The harness gives each of the 12
  call sites its own stream, and logs how many values each site drew. A site
  whose count differs between a recording and its replay is a lead.

## Hidden and Wii-only levels

The Complete Edition ships the full HD data of some levels it never lists
properly:
- three Wii-only story levels: Andy's Basement, TrainStub and Buzz Flight;
- Trendy Town, the Skate Park and the Shooting Gallery.

Their preload lists are missing (no sounds, empty dialogue boxes) or their
database entries are thin or hidden. With `[harness] restore_hidden_levels=1`
(the default), the harness fixes both in memory as the game reads two text files
at boot. No game file changes:

- `FileList.txt` gets the missing preload sections.
- `WorldDatabase.txt` loses the `debug` / `wii_only` / `wii_hidden` flags on
  those six levels. Trendy Town also gets the Toy Box keys it lacks, such as
  `storyMode = false`, so the avatar behaves as in the Toy Box.
- The Shooting Gallery (the old PlayStation Move mode) gets usable menu buttons.
  On PC, X both started and exited its title menu, so you just left the level.
  Now **A** starts (and fires), **B** exits, and **Y** is Calibrate; avoid Y, it
  may wait for a Move controller. On the keyboard, Start no longer also exits;
  leave through the pause menu.
- The sound-bank name list (`data_dx\sound\bank_name_replacement.txt`) gets two
  lines it lacks, as the game reads it. Without them, the Shooting Gallery had
  no sound effects, and four story levels never loaded the bank they ask for as
  `jessie.fev`: the list only spells it `jesse.fev`.

The log shows `levels: FileList.txt +5 sections ...` and `levels:
WorldDatabase.txt: 6 of 6 sections edited ...`. Both co-op players must use the
same setting; the session refuses to start otherwise. Details and data are in
`re\wii_levels\`.

## Engine RNG tracing

`mt_trace=1` (the default in record and replay) traces the engine's own
random generator, a Mersenne Twister, separate from `rand`. Every draw goes
through one routine, `MT_Next` at 0x14025ded0. The harness replaces it with
a bit-exact copy that also records who asked. When the immediate caller is a
helper (a tiny wrapper such as the float helper at 0x14012be40, or anything
inside the random-number module at 0x14025c000 to 0x14025f000), it walks up
the stack past every helper and records the first real game function as
well. It writes:

- `mt_callers.csv`: draws per caller, split by global or private generator
  and by thread.
- `io.csv` column `mt`: main-thread draws per frame.
- `mt_draws.bin`: every main-thread draw in order (frame, caller, wrapper's
  caller, generator, whether a load was in progress).

To find where two runs part ways:

```
python harness\tools\diff_mt.py run11_replay run11_replay_2
```

It skips draws made during loads, compares each stretch between loads, and
prints the first draw that differs with the callers on both sides, plus the
callers whose totals differ. Addresses match Ghidra. Tracing doesn't change
what the game does. If the game stutters with it on, set `mt_trace=0`.

## Spawn gate

Characters and objects are streamed in during play by a background loader.
Its work each frame is limited by real time, so a spawn can finish loading
a frame earlier or later from one run to the next, and the game places it on
that frame. That's what split run12 and run13: `pigbrown.zip` arrived a frame
apart and everything after diverged. See re\SYNC_VIABILITY.md, "Traced: async
loading and spawns".

With `spawn_gate=1` (the default in record and replay), the harness redirects
the game's once-per-frame spawn update (`AsyncActorCreator_Update`, called at
0x14021ed80) through a gate:

- It notes the frame each spawn is first queued.
- A spawn that finishes loading early is held back until `spawn_delay` frames
  (default 15, a quarter of a second) after it was queued.
- If it isn't loaded by then, the game is made to finish it right away
  through its own "run the loader until this is done" function. That costs a
  brief hitch, because that function finishes everything queued.
- The game still places at most one spawn per frame, in queue order, so
  spawns that come due together land on consecutive frames.

`spawns.csv` lists every spawn as `queued`, `forced`, `placed` or `cancelled`,
numbered in queue order. Its summary line in `harness.log` says how many
spawns were forced. If many are, raise `spawn_delay`. A recording and its
replays must use the same gate settings; the log warns if they don't. The
gate is off in passthrough, so normal play isn't affected.

## File activity log

With `io_log=1` (the default), `files.csv` lists every file the game opens
and every read it makes during play, skipping loads, which read thousands of
times. Each row has the poll and logical frame, the thread (with the game's
own thread name when it set one), the source (game, C runtime, Bink or FMOD),
the size and the file. To compare two runs:

```
python harness\tools\diff_files.py run13_replay run13_replay_2
```

It shows the first file event that lands on a different frame in each
stretch between loads, then every file first touched during play on a
different frame. This exists because run12's replays parted ways on the same
frame a 128 KB streamed read arrived a frame early, which points at
background streaming as the remaining source of non-determinism.

## Save games

The game autosaves coins and items to
`%APPDATA%\Disney\Toy Story 3 Complete Edition\savedata`. Without care, each
replay starts with the progress the previous run made, and the game diverges.
run6 showed this: the coin counter read 310, 390 and 470 in the recording and
its two replays.

- **Record** copies the save folder into `runs\<name>\save_start`.
- **Replay** backs up whatever save is on disk to `runs\<replay>\save_backup`,
  then restores `save_start`.
- It happens inside the game process at startup, after Steam Cloud has synced
  and before the game reads its save, so Steam sees an ordinary save change.
- Recordings made before this (run1 to run6) have no snapshot and can't be
  replayed reliably.
- To get a particular save back, copy the files from any `save_start` or
  `save_backup` folder into the save folder while the game is closed.

## Loading screens

Without this, replay feeds inputs back by poll count. A load that is quicker
the second time (files already cached) makes the replay reach the next menu
early, and every recorded press lands on the wrong item. That is what ended
run2's replay on the Exit dialog.

How it works:

- The proxy hooks `ReadFile`/`CreateFile` in the game and in the DLLs it
  reads through, and counts bytes read per frame.
- Loading starts when the last `load_window` frames read `load_enter_kb` or
  more, and ends after `load_quiet_frames` frames in a row of near-zero reads.
- **Recording** keeps every poll, flags the ones made during a load, and
  writes a load marker when each load ends.
- **Replay** never consumes recorded input while loading. When it reaches a
  recorded load marker it waits until its own matching load has finished. The
  3rd load of the replay always lines up with the 3rd load of the recording,
  whatever their lengths.
- While loading or waiting, and after the recording ends, the replay keeps
  reporting each pad's last state, including its packet number, just as a
  controller left alone does. An earlier version sent zeros, so the game saw
  the sticks jump back to their resting values (-129/128 on a DSX pad) and a
  new packet when play resumed. That made replays differ from the recording
  (run11).
- Frame numbers in screenshots and `frames.csv` are now **logical frames**,
  which skip loading time, so captures from runs with different load lengths
  still line up.

Limits worth knowing:

- Presses made **during** a loading screen are not replayed. Don't mash
  buttons through loads while recording.
- If the game streams lots of data during normal play, a busy moment can be
  mistaken for a load. If a loading screen reads very little (assets already
  in memory), it can be missed. `tune_loads.py` shows both cases; see below.
- The thresholds are a first guess. They were verified against a simulated
  loading screen, not yet against this game.

### Checking the detector on a run

```
python harness\tools\tune_loads.py run3
```

It lists every load the run detected, with time and size, and re-runs the
detector offline. You want one load per loading screen you saw, and none
during play. To try other thresholds without playing again:

```
python harness\tools\tune_loads.py run3 --enter-kb 4096 --quiet-frames 45
```

When the offline result looks right, copy the values into the ini and record
again. Changing thresholds after recording does not move the markers already
in a recording.

Verified against a stand-in program (record, replay twice, byte-identical
replayed states, constant virtual clock, no files written without an ini). It
has **not yet been run against the game itself**: that is the test.

## Files

| Path | Purpose |
|---|---|
| `src\` | the proxy's source, one module per feature; each file's top comment says what it does. `src\xinput_proxy.cpp` holds DllMain and the exports, `src\proxy_shared.h` what the modules share, `src\xinput1_4.def` the export ordinals (they match the real DLL because the game imports by ordinal) |
| `build.cmd` | builds with MSVC 2022 and installs the DLL + ini into the game folder (`build.cmd uninstall` removes the DLL) |
| `xinput_harness.ini` | the reference settings file, with every key explained; `..\xinput_harness.ini` (next to the game's exe) is the one the game reads |
| `launcher\` | the settings launcher (WPF, .NET 8) |
| `local_coop_test.cmd` | the one-PC co-op test (see below) |
| `tools\build_check.cmd`, `tools\code_equiv.py` | a compile-only check build, and the check that a refactor only moved code and added comments |
| `tools\compare_runs.py` | `python tools\compare_runs.py runA runB` or `python tools\compare_runs.py inspect run` |
| `tools\tune_loads.py` | `python tools\tune_loads.py run [--enter-kb N ...]`: loads a run detected, and an offline re-run with other thresholds |
| `tools\check_pads.py` | live view of what the system XInput sees; run before the game to confirm the pad works |
| `runs\<name>\` | `input.bin`, `harness.log`, `frames.csv`, `io.csv`, screenshots |

## Setup before the first run

1. Steam: right-click the game, Properties, Controller, set to **Disable Steam
   Input**. With it enabled Steam hooks XInput inside the process and gets in
   the way of the proxy.
2. In-game video settings: **windowed or borderless**, fixed resolution,
   vsync on. Exclusive fullscreen can defeat the screenshot capture.
3. Use one Xbox-style (XInput) controller. A DualSense or DualShock is read
   through DirectInput and is invisible to the proxy, so run it through an
   XInput emulator such as DSX in Xbox 360 mode. `check_pads.py` confirms it.
   Keyboard and mouse are not recorded, so don't touch them during a run.
4. Turn off the Steam overlay for this game (Properties, General). Its
   first-launch popups take real time in one run and not the next.
5. Confirm the proxy loads: after launching once with `mode=record`, the folder
   `runs\run1\` must contain `harness.log`. If it doesn't, the game never
   called XInput through our DLL.

## Test procedure

Remember to switch the ini to `mode=replay` before launching to replay. If you
forget, nothing is lost: record mode never overwrites, so the accidental
recording goes to `<name>_2` and the log says so.

### Stage 1: real clock

1. `xinput_harness.ini`: `mode=record`, `input=run1`, `fixed_timestep=0`.
2. Launch from Steam. Play 2 to 3 minutes: navigate menus, enter Toy Box, do
   physics-heavy things (throw objects, drive, knock things over). Keep your
   hands off the controller while a loading screen is up. Quit normally so the
   log summary is written.
3. Before replaying, run `python harness\tools\tune_loads.py run1` and check that
   it found one load per loading screen you saw. If not, tune it first (see
   "Checking the detector on a run").
4. `mode=replay` (leave `input=run1`). Launch, then **don't touch anything**.
   The game replays your inputs. Wait until the recording runs out (controller
   goes neutral), then quit with the mouse or Alt+F4.
5. Launch again in replay mode. Output goes to `run1_replay_2` automatically.
6. Compare:

```
python harness\tools\compare_runs.py run1 run1_replay
python harness\tools\compare_runs.py run1_replay run1_replay_2
```

Expect this to diverge early, often in the first menu, because real frame
times differ between runs. That's fine: it establishes the baseline and proves
the pipeline works on this game.

### Stage 2: fixed clock (the real verdict)

Set `fixed_timestep=1` and record a **new** run (`input=run2`); a recording and
its replays must use the same clock setting. Replay twice, compare again.

How the fixed clock behaves: every clock read on the game's main thread
advances time by 1 µs, `Sleep` on that thread advances it by the requested
amount, and each input frame is guaranteed at least 1/fps. Frame waits and
splash-screen timers therefore always finish, and finish identically every
run. The proxy separately paces frames to real time so the game plays at
normal speed. If the log ends with a `stall assist` warning, a background
thread had to be allowed to move time forward and that run may not
reproduce exactly.

- **Replays stay in sync for several minutes**: the simulation is
  deterministic given identical input and time. Lockstep is feasible. Next step
  would be networking the input exchange and handling loading-screen sync.
- **Replays diverge at the same point every time**: something reads a time
  source the harness doesn't hook (`rdtsc`, a static CRT `time()`, a thread
  with its own clock). Try `freeze_wallclock=1`. If it still diverges, that is
  the first real reverse-engineering target.
- **Replays diverge at random points**: true non-determinism (threaded
  physics, uninitialised memory, GPU readback). This is the bad outcome; a
  lockstep mod would need the engine changed, which isn't realistic.

## Reading the results

- `harness.log` lines to look for:
  - `LOAD #n begin` / `end`: each detected load. In a replay, `reached recorded load`
    and `matched after holding N polls` show the replay waiting for its own load.
  - `summary: X recorded loads matched, Y wait timeouts, Z unmatched loads`: any
    timeout or unmatched load means the loads didn't pair up and the replay is
    out of sync from that point. Run `tune_loads.py` on both runs.
  - `CALL PATTERN MISMATCH`: the game polled a different pad or call type than
    it did when recording. The game itself has already diverged by this frame.
  - `frame numbering drifted`: same thing, caught by frame numbers.
  - `WARNING: XInput polled from more than one thread`: frame counting is
    unreliable; replays may still work but frame numbers won't line up.
  - `no clock imports found`: `fixed_timestep` had nothing to hook; the game
    gets time another way.
- `compare_runs.py` output: a single frame with a small difference that
  returns to ~0% is DWM presenting an adjacent frame. A difference that
  persists or grows is a desync. The first flagged frame gets a `diff_*.png`
  showing where on screen it happened.
- Rendering can be non-deterministic (particles, time-based shaders) even when
  the simulation is deterministic. Judge by whether characters and objects are
  in the same place, not by whether every pixel matches.

## Online co-op sessions

- **Starting.** Both games must show the "Press Start" title screen. Both players
  hold Back + Start. The ready combo is ignored on any other screen.
- **Start values.** The host picks the start values: game time rounded up to the
  next hour and entity keys rounded up to the next 0x100000, both above both
  PCs' values. They travel in READY and INPUT packets, and the client adopts them.
  A rejoined session's clock therefore never runs backwards.
- **Ending.** When a session ends mid-play (left, timed out or desynced), player
  2 is removed through the game's own `PlayerMgr_SetPlayerAvatar(mgr, 1, 0)`. That
  call collapses the view to one player without a "Reconnect controller" screen,
  and each PC carries on alone.
- **Rejoining.** Go back to "Press Start" and hold Back + Start for a third of a
  second. The session restarts from scratch: inputs, fingerprints, save transfer
  and start values. While Back is held at "Press Start", Start isn't passed to the
  game, so the combo doesn't leave the title.
- **Own screen.** Each PC draws only its own player full screen. That player's HUD
  uses the one-player layout: `FUN_1405f4af0` passes count 1 and mode 0 for this
  PC's player's elements. The other player's elements keep their split-screen
  place. With `audio_wait` on, each PC also hears only from its own player's
  camera, through one FMOD listener.

## Drop-in while the host plays

When a friend connects while the host is in a level, `drop_in=1` (the default)
brings them in through one reload. Neither PC's own save file is shared.
1. **Save and return to the title.** The host saves now (`FUN_14049c930`) and
   waits for the storage to go idle. It captures the in-memory save buffer
   (`inner+0x258`, size `+0x260`) and the world name (`saveObj+0x160`), then posts
   `QuitGame`.
2. **Send the save.** Once the host is at the title, it sends `newsettings.bin`
   plus the buffer and world name ("@buffer", "@world") over the save channel. The
   client keeps them in memory. While that client game runs, every write to its
   own "Saved Game" goes to "Saved Game (co-op session)" instead.
3. **Start and launch.** Both start lockstep without a combo. On lockstep frame
   30, both PCs make the same calls:
   - install the buffer;
   - bind player 1 to controller 1;
   - launch the world the way the character-select screen does: world database
     `SelectEntry`, then `SetupLaunch`.
   `dropin_launch=switchlevel` posts `SwitchLevel` instead.
4. **Reset at the level load.** When the level publishes (`FUN_1405d81a0`, via a
   trampoline), the engine RNG, rand streams, spawn slots and entity keys reset
   on both PCs. Fingerprints aren't compared from the launch until 120 frames
   after the publish, because the two title screens may differ.
5. **The friend joins.** They press Start to drop in as player 2.

If any step fails, the banner says so and falls back to the title-screen start.
None of the game calls have been tried in the real game yet. They're
byte-checked, and the stand-in test `test_dropin.py` covers the transfer, the
protected save, the shared launch frame and sync.

## Testing online co-op on one PC

`harness\local_coop_test.cmd` starts two copies of the game that play together
over this PC's loopback. They run the same lockstep, own-screen and status
banner code as two PCs over Steam, just over UDP instead. Steam must be running.

- The window titled "PLAYER 1 (host)" is controller 1. "PLAYER 2 (client)" is
  controller 2.
- When both banners say "Connected", hold Back + Start on both controllers.
- With one controller, both windows share it. It starts on player 1, and
  Back + RB hands it to player 2 and back, while the other player stands
  still. To add player 2 in Toy Box, hand the controller over, then press Start.
- Player 2 plays from a copy of your save under
  `%APPDATA%\TS3 local co-op test\player 2`, refreshed from the real save at each
  launch. The real save is only read.
- Player 2's screen settings stay in that folder, so its window can be a
  different size.
- Logs go to `harness\runs\local_host` and `harness\runs\local_client`.

Optional `[netplay]` keys:

| Key | Default | Meaning |
|---|---|---|
| `local_test_host_pad` | 0 | which controller is player 1 |
| `local_test_client_pad` | 1 | which controller is player 2 |
| `local_test_controllers` | `auto` | `auto` shares one controller when player 2's pad isn't connected. `1` always shares one, `2` always uses two. |
| `local_test_client` | `pad` | `bot` drives player 2 with random presses (never Start or Back) |
| `local_test_latency_ms` | 0 | added delay each way, to feel online play |

The launcher passes `+tsnet_local host` or `+tsnet_local client`. That switch
forces netplay whatever `mode` says. It also gives the game's single-instance
mutex a per-copy name and skips Steam's relaunch check, so a second copy can
start.

## Repairing a desync in place

Once a second, at the fingerprint frame, both games hash every piece of state
the repair can copy: the game timer, the random generators, the `rand()`
streams, every moving physics body and every live actor's transform. When player
2's hashes differ from the host's, player 2 asks for just those pieces. Both
games then copy them at the same agreed frame, about half a second later, and
play carries on with no reload. The design is in `re\SYNC_VIABILITY.md`
("Design: repair a desync in place").

- The log says `repair: frame F: N items differ ... copying them at frame T`,
  then `repaired N items`. Each line names the first item that differed, which
  points at the next determinism bug worth fixing at its source.
- Some differences can't be copied: an actor spawned on one PC only, or frame
  counters that differ. Those still end the session, as before.
- While the repair runs, the fingerprint is allowed to differ for up to 3
  checks in a row before the session ends.

**Per-frame random-number check.** An extra random draw on one PC shifts every
later draw there, so enemies and spawns start choosing differently. Once a
second is too slow to stop that spreading. So every frame, both games also hash
just their random-number state and send it with their inputs. A slip is seen
within a few frames:

- Both logs say `rng check: frame F: the random numbers differ`. The next line
  lists which game functions drew from the engine's random generator in that
  frame on that PC. Compare the two players' lists: the extra entry is the
  function that broke sync.
- Player 2 then asks for the host's random-number state straight away
  (`repair: frame F: the random numbers differ ... (per-frame check)`). It's
  copied about 14 frames later (`2 × input_delay + 6`), not 30.
- After 3 tries for the same slip it leaves the rest to the once-a-second check.

| `[netplay]` key | Default | Meaning |
|---|---|---|
| `repair` | 1 | `0` turns the repair off: any difference ends the session |
| `repair_lead` | 30 | frames between finding a difference and copying it |
| `rng_check` | 1 | `0` turns the per-frame random-number check off |
| `test_desync_keys` | 0 | `1` turns on the test hotkeys below |

**Test hotkeys** (`test_desync_keys=1`, during a session, in the focused game
window). They knock **this PC only** slightly out of step, to watch the repair
work:

- **Ctrl+F9**: one extra draw from the engine's random generator.
- **Ctrl+F10**: moves one moving physics object by 1 mm.

The log notes each press as `test: Ctrl+F9 at frame ...`. With `repair=1`, the
next lines should show the difference found and repaired; for Ctrl+F9 that's
within a few frames, by the per-frame check. With `repair=0`, the session ends
with a desync at the next fingerprint.

## Removing it

```
harness\build.cmd uninstall
```

or just delete `xinput1_4.dll` from the game folder. Steam's "verify integrity"
leaves extra files alone, so it won't remove the proxy for you.

## Debug menu

Hold **LB + RB** and press **Y** (or F1) to open and close it. Back is the
game's own menu button, and Back + Start / Back + RB belong to co-op, so the
chord avoids it. Solo, everything is unlocked. In an online session the cheats
are locked; the remaining rows (level select, progress, graphics, teleport)
act on both PCs at once through the session, and only player 1 controls them.
The free camera and field-of-view rows are render-only and work for either
player during a session; a player flying the camera stands still.
