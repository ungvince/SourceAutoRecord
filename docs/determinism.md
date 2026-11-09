# Deterministic TAS playback (`sar_tas_determinism`)

## What it is

Without this cvar, TAS playback is not reproducible: the same script can move props, turrets and cubes differently from
one playback to the next and from one game launch to the next. There are two causes. The game keeps hidden state from
what it did before (earlier playbacks, the length of loads, old heap contents). And some of its inputs follow the wall
clock (frame times, the sound device, the moments at which Steam answers).

With `sar_tas_determinism 1`:

1. **Every TAS starts like the first playback after launching the game.** SAR sets the hidden state to the values it
   has when a freshly launched game loads its first map or save, with SAR's fast loads.
2. **The wall-clock inputs follow the frame count instead**: they get what a machine would give them whose frames
   take exactly 1/`host_framerate` seconds.

The intended result is that a script plays the same way every time: the same position for the player and for every
physics entity on every tick, on every playback and after every launch, in real time, uncapped or with `sar_tas_skipto`.
This was tested with 11 scripts on Linux, 16 playbacks each: 4 game launches (2 in real time, 2 uncapped) with 4
playbacks per launch, one of them with `sar_tas_skipto`.

## Usage

- `sar_tas_determinism 0/1` turns it on. It only works in Portal 2 (the `portal2` game directory, not mods) on Linux
  for now. Elsewhere, setting it to 1 prints that it is not supported, and it stays 0.
- The values SAR sets were recorded on the Portal 2 build of November 2026 (Steam build 25240193). After a game update,
  SAR may no longer find a part of the game it needs. Setting the cvar to 1 then prints
  `sar_tas_determinism is not available: ... not found`, and it stays 0.
- The cvar counts when a TAS is started (`sar_tas_play`, `sar_tas_replay`, the TAS protocol). Turning it on later does
  nothing for a TAS that is already active. Turning it off ends it for that TAS.
- Refused while the cvar is 1: scripts with an `rngmanip` line and `sar_rng_load` (they replay the random seeds of
  another session), and `start next` and coop scripts (not supported yet).
- SAR's fast loads are required. If they are off when a TAS starts, SAR runs `sar_fast_load_preset full` and says so
  in the console. This changes your loading settings (eight cvars: the `sar_loads_*` cvars and the loading screen
  times). They are not restored when the TAS ends.
- `sar_tas_skipto` renders every frame while the cvar is 1, so skipping is slower than without it. Skipping the
  rendering changes what the client does, which can change the outcome.
- Three optional header lines act only while the cvar is 1: `prerun_restarts`, `prerun_wait` and `expect_load_ticks`
  (see [below](#header-lines)). A script that has them does not parse in SAR versions that do not know them.
- Things outside SAR also change how a script plays, like its inputs do: the `config.cfg` the game starts with (for
  example, closed captions add random draws to every map load), the render resolution, and whether and from which
  tick a demo is recorded. Publish them with a script if others should get the same result.

**Do not leave the cvar at 1 for normal play.** Some of its changes are active whenever it is 1, also at the menu and
between TASes:

- The sound mixer mixes one frame of audio per frame instead of following the sound device. When frames do not take
  exactly 1/`host_framerate` of real time (uncapped playback, `sar_tas_skipto`, loads), the audio can stutter or echo.
  The game state does not depend on it.
- While `host_framerate` is not 0 (the TAS player sets it while a TAS is active), the engine's `realtime`, `net_time`
  and the sound timers advance by 1/`host_framerate` per frame.
- A sound whose data is not loaded yet blocks the game until it is (10 s at most).
- The uninitialised memory of physics shadow controllers and AI motors is set to fixed values (see the tables below).

`net_time` and the DSP timers never go back. After an uncapped TAS they stay ahead of the wall clock by what they
gained, until SAR is unloaded, also with the cvar back at 0.

## The start sequence

For `start map`, `start cm` and `start save` scripts, SAR does the following between the start command and the first
tick of the script. `start now` scripts play from the current state: they get the lockstep clocks, and nothing that
the start sequence sets. A `start now` TAS is not made reproducible. Played after a map TAS in the same game process, it
keeps that TAS's CheckStuck clock offset and usercmd seed numbering. For example, a stuck player was unstuck at tick 91
in a freshly launched game and at tick 95 after a map TAS (without the cvar the clock differs between these two cases
too).

1. **Wait for Steam's profile data.** SAR waits until Steam's first profile event has arrived and no profile or
   workshop event has arrived for 1.5 s, like a player who waits at the menu after launching the game. Frames keep
   running. If this takes more than 10 s (for example because SAR was loaded after the profile), the TAS starts
   anyway and later TASes do not wait. `sar_tas_stop` cancels the wait. Setting the cvar to 0 ends it and starts the
   TAS at once, without the rest of the start sequence.
2. **Warm-up load.** A full `map X` (or `load X`), also when the map is already loaded. If this is the first load
   since SAR was loaded, a second warm-up load follows (a `restart_level`, or `load X`), so that the next step is
   never the second load of the game process.
3. **Reset load.** A `restart_level` (or `load X`). During this load and at the start of its session, SAR sets the
   values listed in the tables below. On sp_a5_credits the load is `disconnect` + `map X` instead: there a
   `restart_level` load is 2 ticks longer than the `map` load of a freshly launched game.
4. **Prerun restarts**, if the script has `prerun_restarts N`: N times `restart_level` (or `load X`), each after
   `prerun_wait M` ticks. SAR resets nothing for them.
5. The script starts.

So a TAS start loads the map twice (three times if it is the first load since SAR was loaded), plus the prerun
restarts. With fast loads this takes a few seconds.

No demo is recorded during steps 2 to 4, because a freshly launched game does not record its first load either. A
recording that is running or armed is stopped (as `stop` does) when the first of these loads begins. `sar_record_at`
only fires in the session the script plays in: use it to record the TAS.

### Header lines

| Header line | What it does |
|---|---|
| `prerun_restarts N` | Restarts the level (or loads the save again) N times after the reset load. The restarts are real ones, as if a player restarted N times after launching the game. This is the way to get a different outcome from the same inputs, when the first playback after launching is not the one you want. |
| `prerun_wait M` | Waits M ticks in the session before each prerun restart. |
| `expect_load_ticks N` | The server tick at which the session should start after the reset load. SAR prints a warning if it starts at another tick. Use the tick at which the session starts when a freshly launched game loads the map with fast loads; some maps have two (sp_a2_turret_blocker: 96 or 98). |

## For maintainers: every change

The fixed values below were recorded on first playbacks after launching the unmodified game with SAR's fast loads, on
Linux. "Reset load", "start sequence" and "reset session" are explained above; the reset session is the session the
reset load starts.

### Start state (`Determinism/StartState.cpp`)

| What | Without the cvar | With the cvar | Reason |
|---|---|---|---|
| Physics random seed (`ivp_rand`) | One seed for the whole process, kept across loads. | 1 at the level init of the reset load. | A new process starts at 1. |
| Uniform random stream | Seeded once at startup from `Plat_FloatTime()`, then advanced by everything that runs before the map's level init. | At the level init of the reset load: seeded from the float bits of 3.520046 s, then 95291 draws (map) or 94779 (save). | One recorded launch. Launches differ (startup at 3.01 to 4.17 s, 95291 to 95297 or 94779 to 94788 draws), so one of them is used. |
| First-connection draws | Only the first load of a process creates the loopback client's challenge (2 draws) and runs the client's `cl_predict 0` while `cl_predict` has its default value (2 draws in the command buffer). | `cl_predict` is set back to its default 1.0 at the level init of the reset load, and the 2 challenge draws are made at its first signon state change or tick. | The reset load then draws like a first load, in the same order. |
| Usercmd `random_seed` | `MD5_PseudoRandom(command_number)`. Command numbers keep counting across restarts. | While a TAS plays, computed from the command number of a first playback: the first one is the load's length in host ticks - 3. For a map that length is the session start tick - 57; for a save it is counted during the reset load. Movement code keeps the real numbers. | Every recorded first playback has these relations. |
| CheckStuck clock | SAR runs CheckStuck on host time, which counts from the start of the process. | While a TAS plays, host time counts as if the reset session had started at host tick (load length in host ticks + 120). The stuck timers are 0 at the start of the reset session. A `start now` TAS keeps the offset of the TAS before it. | Every recorded first playback has these values. |
| AI motor `m_flNextTurnGesture` | Never initialised. What the heap held before decides whether a motor draws a random number on each think. | On a motor's first think in a session, an uninitialised value becomes 0x0a0d3420 for the reset load and its session, and 0x6afa85f0 otherwise. Times the motor set itself are kept. | First playbacks read 0x0a0d3420 (a time in the past). Later playbacks read other values, which all act like 0x6afa85f0 (a time that is never reached). |
| Local network backdoor | A load started from a running session keeps it, and the server then skips the instance baselines. | Switched off at the level init of the reset load, with the engine's own function. The engine sets it up again on the next frame. | A freshly launched game loads from the menu, where there is none. |
| Interpolation phase (`host_remainder`) | The fraction of a tick left over by the wall-clock frames at the menu. | At each session start of the start sequence: whole ticks kept, fraction 0.664074. | A value recorded on a first playback (recorded range: 0.009 to 0.94). |
| Shadow controllers | The 4th (w) component of two SIMD vectors and a bone follower's seconds-to-arrival are never initialised. | The w components are set to 0 before every server physics step, a bone follower's time the first time it is seen in a level. | First playbacks read 0 there, like new memory. |

### Clocks (`Determinism/Clocks.cpp`)

| What | Without the cvar | With the cvar | Reason |
|---|---|---|---|
| Engine `realtime` | Advances by the measured frame time. | Advances by 1/`host_framerate`. | A machine whose frames take exactly that long. |
| `net_time` | `Plat_FloatTime()`. It decides when snapshots and usercmds are sent. | Advances by 1/`host_framerate` on each frame that advances `realtime`. It never goes back when that ends. | The same machine. |
| Sound mixing | Mixes what the audio device's clock asks for. DSP modulators draw random numbers per mixed block. | The engine's movie-mode mixing, without output: one frame of audio per frame. | The engine's own way of mixing while it records a movie. |
| Sound clock | The sample position modulo 4, the mix-ahead filter and how far ahead the mixer has painted all depend on earlier wall-clock frames. | At each session start of the start sequence: sample position rounded down to a multiple of 4 with no fraction, the filter at the value it has at a constant frame time, and the clock moved forward to what is already painted. | A sound device that started up to 3 samples later, or ran that much longer. |
| DSP preset timers | Compare against `Plat_MSTime()`. | The DSP code's calls read a clock that advances by 1000/`host_framerate` ms per frame. It is restarted at each session start of the start sequence and never goes back. | The same machine as for `realtime`. |
| Sound operator `math_random` | Reseeds from `Plat_FloatTime() * 100`. | That call reads a clock that is set to 10.992413 s at each session start of the start sequence, then advances by 1/`host_framerate` per frame. | The engine time at the first tick of a first playback in real time (recorded range: 9.55 to 11.90 s). |
| `sar_tas_skipto` | Replaces rendering by a few frame calls. The client then does not run the same code, and some of that code draws random numbers. | Renders every frame as normal playback does, uncapped. SAR's HUD stays hidden while skipping. | The same code as normal playback, only faster. |

### Sound (`Determinism/Sound.cpp`)

| What | Without the cvar | With the cvar | Reason |
|---|---|---|---|
| DSP, wave cache, mixers | Kept from the previous playback (for example a mix layer that ducks other sounds). | `dsp_reload; snd_async_flush; snd_soundmixer_flush` at the level init of the reset load. | A freshly launched game has new DSP processors, an empty cache and newly loaded mixers. |
| Sound entry matches | Timed blocks ("do not start this sound again yet") of the previous session can mute sounds in the next. | Freed at the level init of the warm-up loads and of the reset load. | The first load of a freshly launched game has none. |
| Sound listener | Stays at the last view of the previous session until the first rendered frame. | Zero at the same level inits. | A new process has it at zero. |
| Impact sound timer | Kept across sessions, and it advances during loads. | 0 when the session before the reset load ends, and again at the start of the reset session. | First playbacks have 0 at the session start. |
| Wave slots | Remember which waves of a multi-wave sound were played before. | From the reset load on, each sound has all its waves available the first time it is used. Prerun restarts keep what was played. | First playbacks have every wave available at a sound's first use. |
| Async sound loads | A sound is ready on a frame that depends on disk and thread timing. | The load request, made on the main thread, returns once the data has been read. | A machine with the file cached and an idle job thread. |

### Steam timing (`Determinism/Steam.cpp`)

| What | Without the cvar | With the cvar | Reason |
|---|---|---|---|
| Profile data | When Steam's "profile data loaded" arrives, the game runs its config again (a random draw per `exec`). | A map, cm or save TAS waits for it before it starts (start sequence, step 1). | A player waiting at the menu. |
| Profile and workshop events | Can arrive in the middle of a TAS. | In a map, cm or save TAS: during the warm-up loads each event is let through once. Repeats, and every event from the moment the reset load is requested, are held until the TAS ends (or the cvar is set to 0, or SAR unloads) and delivered then, all together. Steam sends about 2 to 3 profile events per minute of TAS. | Steam answering after the run, as on a slow connection. |
| Game-server callbacks | Run every 0.1 s of wall time. The auth ticket answer zeroes the player's bounds for a moment, at a tick that depends on timing. | Not run from the moment the reset load is requested until the TAS ends. Steam keeps them queued. | The same: delivered unchanged, later. |

## Not changed

- Game rules, physics, movement, collision and inputs.
- Movement code sees the real command numbers. Only `random_seed` follows the numbering of a first playback.
- Random streams are only set to values they have after a fresh launch. No random number goes to other code than
  the code that gets it in the unmodified game. A TAS changes its random numbers through its inputs and timing.
- Prerun restarts, and a demo recording in the session the script plays in (`sar_record_at`, `sar_autorecord`), work
  as they do without the cvar.
- With `sar_tas_determinism 0`, SAR's hooks pass the game's own values through. They only keep counting (sessions,
  command numbers).
