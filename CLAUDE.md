# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`evclack` is a single-file C11 daemon that plays a sound when you press a
key. It passively reads evdev keyboard devices — either an explicit config
list or, by default, every keyboard-shaped device advertising every bound
key, with inotify-driven hotplug in both cases — and triggers a configured
sample on every press of a bound key, mixing everything through one PipeWire
output node.

Nothing is grabbed, remapped, filtered or re-emitted. There is no uinput
device. The keys reach the rest of the system exactly as they always did and
evclack only listens, which is what makes it safe to run alongside a
remapper, a game, or anything else.

It is a strip of `doubletap`, a SOCD cleaner whose audio subsystem was
deliberately built to be liftable out as a block. The mixer came across
near-verbatim; the grab, the virtual device, the SOCD state machine, the
slot layer and the analog/hidraw front end did not. The history explains a
lot of the mixer's shape — most of its invariants were MEASURED against a
300 BPM alternating stream, which is why they read as absolutes.

The entire implementation lives in `evclack.c` (~1700 lines) — there is no
multi-module structure.

## Build

```sh
cmake -S . -B build
cmake --build build
```

Produces `build/evclack`. Requires pkg-config-visible dev packages for
`libpipewire-0.3`, `libevdev`, `yaml-0.1` (libyaml), `samplerate`
(libsamplerate — the mixer conforms every sample to one rate at load time;
PipeWire's own resampler is not linkable from here), and `sndfile`
(libsndfile — the sample decoder, which is what lets an osu! skin's `.ogg`
and `.mp3` hitsounds be used as they ship).

## Running

The daemon runs as an unprivileged user in the `input` group, which covers
read access to `/dev/input/event*`. That is the ONLY privilege it wants:
there is no uinput device, so no udev rule and no write access to anything
under `/dev`. It must NOT run as a root system service — PipeWire is a
per-user session daemon, so audio only works from inside a user session.
The systemd unit (`evclack.service.in`, configured by CMake) is a *user*
unit: `systemctl --user enable --now evclack`.

```sh
./build/evclack -c config.yaml
```

Without `-c`, the daemon looks for `$XDG_CONFIG_HOME/evclack/config.yaml`
(`~/.config` if unset), then falls back to the installed default
(`/usr/share/evclack/config.yaml`; CMake bakes the real prefix in via
`DEF_CONFIG`/`DEF_WAV` compile definitions). See `config.yaml` for the
schema: `devices` (optional list of `/dev/input/by-id/*` paths; omitted or
`auto` means auto-discovery), `keys` (REQUIRED — the bindings, each a bare
`KEY_*` name or numeric code, or a mapping with a per-key `sample` and/or
`gain`), and `audio` (`enabled`, the default `sample` and `gain`, and
`latency`).

CONFIG EDITS APPLY LIVE. The daemon watches the config file's PARENT
DIRECTORY (`IN_CLOSE_WRITE | IN_MOVED_TO`, basename-filtered) and reloads on
save; SIGHUP does the same on demand, and the unit carries an `ExecReload`
so `systemctl --user reload evclack` works. Keys, samples, gains, devices
and latency all change on a running daemon. A restart is needed only to
create `~/.config/evclack/config.yaml` for the FIRST time, since until it
exists the watch is on the fallback's directory. The `-i DIR` flag overrides the
scanned/watched device directory (default `/dev/input`) — mainly for
testing against a directory of symlinks to synthetic uinput nodes.

`tools/mixtest.c` is a unit test that `#include`s `evclack.c` outright, so it
exercises the daemon's real mixer rather than a copy that could drift; keep
it that way. It needs no PipeWire and no stubs: `audio_mix` is the whole
mixer as a pure function over a float buffer, which is exactly why it was
factored out of `on_process`. Triggers go in through the real
`audio_trigger`, since the ring is the thread boundary and that is where the
bug worth catching lives. Cases C (overlap sums), E (retirement exactly on a
buffer boundary), G (oldest-steal) and H/I (ring full, ring wrap) are the
ones that pin the design; L/M cover `sample_conform`; N/O cover sub-buffer
placement, including its degradations. A–M pass a ZERO timestamp through the
`trigger()` wrapper, which the drain reads as "no placement information" and
puts at frame 0 — they predate placement and are the regression gate on the
mixer proper, so they must keep testing the mixer and not the clock. It
cannot see a memory-ordering bug — it is single-threaded by construction — so
a `-fsanitize=thread` run of the daemon is still the check for that.

`tools/maptest.c` is the same trick over the key → sample layer, which is
the only part of this program that is not inherited: it writes a temp YAML,
runs the real `load_config` and `bindings_plan`, and asserts on the result.
`bindings_plan` does no I/O by design, which is what lets every case name
sample files that do not exist. Case F is the load-bearing one — it POISONS
the table with zeros first, because `.bss` zeros read as "every key plays
sample 0" and a spot check on the bound key alone would pass. Cases G–I are
the rejections (duplicate key, over-`AUDIO_NSAMPLES`, missing/empty `keys`).
K–M cover the reload layer: K pins `refs_same_set` as order-independent and
permutation-producing, L pins that a REJECTED reload leaves `g_key_sample`
and `g_refs` byte-identical, and M runs three reorder-only reloads in a row
to catch a `reload_commit` that writes `g_refs` in plan order instead of
through `map` — a bug that is invisible after only one. M stands up
`audio_available` and a couple of non-NULL sample pointers by hand, since the
fast path only engages when audio is up; those buffers are never
dereferenced.

Both build as part of `all`, deliberately: they `#include evclack.c`, and
that guarantee is worthless if the binaries can go stale.

There is no committed end-to-end test. `doubletap`'s asserted on what came
out of the daemon's own virtual device, and there is no longer one to
observe; anything equivalent would have to capture PipeWire, and would need
`/dev/uinput` write access that evclack deliberately does not require. The
end-to-end checks that were run against this strip live in the commit
message, not in the tree: a scratch harness that `#include`d `evclack.c`,
created a synthetic uinput keyboard, and drove the real `drain_device`
against it to confirm press-only triggering, silent autorepeat, silent
unbound keys, a 5-tap burst yielding 5 triggers, `CLOCK_MONOTONIC`
timestamps, and that the node was left ungrabbed. That shape is worth
rebuilding if this area changes.

## Architecture

Everything is in `evclack.c`, organized into clearly delimited sections
(search for the `/* --- */` banner comments):

1. **Audio** (`audio_load`, `audio_start`, `audio_trigger`, `audio_mix`,
   `audio_cleanup` — with `sample_decode`, `sample_conform`,
   `audio_stream_open`/`_close` and `audio_restart` behind them) — see the
   mixer invariants below. This is the reason the program exists and the
   part that came across unchanged.

2. **Config** (`load_config` and friends) — parses YAML via libyaml's
   document API (not the streaming API) into an `evclack_config_t`. Key
   codes accept either symbolic names (resolved via
   `libevdev_event_code_from_name`) or raw integers. `keys` is a sequence
   whose entries are either a bare code or a `{key, sample, gain}` mapping;
   whatever a binding omits is filled in from `audio.sample`/`audio.gain`
   AFTER the whole document is read, which is what lets the two blocks
   appear in either order.

3. **Key bindings** (`bindings_plan`) — resolves the bindings into
   `g_key_sample[KEY_CNT]` (code → sample id, `-1` unbound) and the list of
   distinct samples to load. Interns on the (path, gain) PAIR. Does no I/O:
   loading is `main()`'s job, which is what makes it testable without files.

4. **Input devices** (`input_try_open`/`input_close`/`reconcile_devices`) —
   devices are opened read-only and wrapped in `libevdev` handles. Nothing
   is grabbed. `reconcile_devices` is the single (re)open path, run at
   startup and again on every inotify event under the input dir: in explicit
   mode it retries any configured path not currently open; in auto-discovery
   mode it scans `event*` nodes and opens those passing `auto_open_ok`.
   Open devices are deduped by `st_rdev` (a node reached via two symlinks is
   opened once) and tracked in a `dev_list_t` of stable heap pointers (epoll
   user data points at the entries).

5. **Event loop** (`run_loop`/`drain_device`) — a single-threaded
   `epoll`-based loop multiplexes all open devices, an inotify fd watching
   the input dir (and its `by-id`/`by-path` subdirs) with
   `IN_CREATE | IN_ATTRIB | IN_MOVED_TO`, the audio wake eventfd, a SECOND
   inotify fd watching the config file, and a signal eventfd.
   `IN_ATTRIB` matters because a hotplugged node is typically root-only
   until udev applies the input-group permissions, so the first open gets
   `EACCES` and the chmod retriggers the reconcile pass. Devices are drained
   with `libevdev_next_event` (handling `LIBEVDEV_READ_STATUS_SYNC`
   dropped-event resync) until `EAGAIN`. A device is dropped from the poll
   set on error/HUP without killing the daemon, and the loop keeps running
   with zero devices, waiting for hotplug (it only aborts at startup if
   nothing opened AND inotify is unavailable).

6. **Config reload** (`config_validate` → `reload_commit` → `config_reload`)
   — the config watch or a SIGHUP sets one flag, consumed once per epoll
   wake. `config_validate` re-parses and re-plans into a `reload_plan_t`
   WITHOUT touching a live byte, and decides how much has to move
   (`RELOAD_NONE` / `LATENCY` / `SAMPLES` / `AUDIO_OFF` / `AUDIO_ON`).
   `config_reload` then decodes into scratch, moves the audio subsystem,
   calls `reload_commit`, and finally re-filters and re-opens devices via
   `devices_refilter` + `reconcile_devices`. See the reload invariants
   below.

`main()` wires these together: parse args → load config → `bindings_plan`
(so a bad config is rejected before a graph is built) → attempt `SCHED_FIFO`
realtime priority (best-effort, warns and falls back on failure) →
best-effort audio init (`audio_load` per distinct sample, then `audio_start`,
which `mlockall`s to avoid page faults in the RT callback) → install
SIGINT/SIGTERM handlers → run the event loop (which opens devices via the
initial reconcile pass and handles hotplug thereafter) → tear down in
reverse order.

## Key invariants to preserve in the input path

- NOTHING IS EVER GRABBED. `EVIOCGRAB`/`libevdev_grab` has no place here,
  and re-adding it would change what the program IS: a passive listener that
  can run alongside anything becomes a thing that steals your keyboard. It
  is also what makes several problems vanish rather than be solved — there
  is no held-key hazard to defer around at open time, no way to strand a key
  by dying at the wrong moment, and nothing to release on shutdown.
  `doubletap` needed `any_key_down`, a deferred-grab path, an `in->grabbed`
  gate in `drain_device` and a `release_stuck` for exactly those; all four
  are gone and none should come back.

- `libevdev_set_clock_id(dev, CLOCK_MONOTONIC)` IN `input_try_open` IS
  LOAD-BEARING, and losing it is SILENT. See the timestamp bullet in the
  mixer section: without it evdev stamps `CLOCK_REALTIME`, the placement
  math compares epochs decades apart, and every voice quietly clamps to
  frame 0. There is no error and nothing to see except bursts that come out
  unevenly spaced. It must go through libevdev, not a raw `EVIOCSCLOCKID`
  ioctl.

- VIRTUAL DEVICES ARE DELIBERATELY NOT FILTERED OUT. `doubletap`'s
  `auto_grab_ok` rejected `BUS_VIRTUAL` to stay loop-free among remappers;
  `auto_open_ok` does not, and re-adding the reject would break the case
  most likely to matter. Remappers (keyd, doubletap, an on-screen keyboard)
  grab their source board exclusively, so the real keystrokes exist ONLY on
  the uinput node they emit — filter those and evclack goes silent on
  exactly those setups. The two arguments for the reject also both fail
  here: a grabbed node yields no events to a passive reader, so there is
  nothing to double-fire; and evclack emits no events at all, so it cannot
  hear itself.

- The `EV_REL`/`EV_ABS` reject in `auto_open_ok` DOES stay. A gaming mouse
  whose HID descriptor also claims keyboard codes is not what anyone means
  by "my keyboard", and opening it would sound its side buttons.

- AUTO-DISCOVERY REQUIRES *EVERY* BOUND KEY, not merely one. A board
  carrying a subset would be opened for keys it cannot report. The
  consequence is that adding an exotic binding can silently narrow which
  keyboards are picked up — which is the right trade, but worth saying out
  loud when a binding is added.

- `load_config` REJECTS A MISSING OR EMPTY `keys` LIST, and this is not
  pedantry. With zero bindings `auto_open_ok`'s "every bound key" test
  passes VACUOUSLY, so every keyboard on the system is opened and the daemon
  then runs forever in complete silence. `maptest` case I pins it.

- `bindings_plan` FILLS `g_key_sample` WITH `-1` BEFORE WRITING, and is the
  only thing that fills it. A static `int16_t` array is `.bss` zeros and
  **0 is a valid sample id**, so a table that reached `drain_device`
  unfilled would make every key on the board clack with sample 0 — the exact
  inverse of what the config says, with no error. `maptest` case F poisons
  the table with zeros specifically to catch a `memset(0)` regression.

- PRESSES ONLY (`ie.value == 1`). Releases are not a second strike, and
  autorepeat (`value == 2`) is the kernel talking rather than a finger — one
  held key would machine-gun the sample at the repeat rate. If a release
  sound is ever wanted, it is a schema change (a per-binding `up:` sample),
  not a relaxed condition here.

- INTERNING IS ON THE (PATH, GAIN) PAIR, not on the path alone. Interning on
  the path would force `audio_sample_t` to split buffer ownership from gain,
  which ripples into `audio_cleanup`'s frees and into every test that pokes
  at `audio.sample[]` — refcounts, to buy a case (one file at two volumes)
  that costs exactly one extra decode today.

## Key invariants to preserve in the config-reload path

- FREEING A SAMPLE BUFFER REQUIRES THE STREAM TEARDOWN, and there is no lock
  that substitutes. `mix_voice_t.samples` BORROWS `audio.sample[i].samples`
  outright, and `on_process` runs with `PW_STREAM_FLAG_RT_PROCESS` — on
  PipeWire's DATA thread, which `pw_thread_loop_lock` does NOT cover. Freeing
  a buffer a click is still playing out of is a use-after-free in the RT
  callback. `audio_swap_samples` therefore runs `audio_stream_close()` FIRST:
  after the node is destroyed no callback exists, which is the same guarantee
  `audio_restart` already leans on when it memsets the voice pool. Verified
  under `-fsanitize=address` and `-fsanitize=thread`: 56 reloads that each
  free and re-decode the set, against a synthetic board at 124 presses/sec
  into a 259ms sample (~32 voices live throughout), reported nothing.

- `audio_stream_reopen` IS THE ONE PLACE THE RESTART FLAG IS CLEARED. It was
  split out of `audio_restart` so the reload path reuses it rather than
  copying its tail — the flag-after-teardown rule it carries is the one
  documented cause of an infinite rebuild loop, and it must not exist twice.
  Every caller runs `audio_stream_close()` immediately before it.

- VALIDATE INTO SCRATCH, COMMIT ONLY ON SUCCESS. `config_validate` parses and
  plans into a `reload_plan_t` and writes NOTHING live; `bindings_plan`
  returns `-1` partway through on a duplicate key, so pointing it at
  `g_key_sample` directly would leave a half-written table behind on a
  rejected config. Decoding likewise happens into a scratch
  `audio_sample_t[]` BEFORE the teardown, so a missing sample file costs no
  stream. `maptest` case L pins it, and that case only means anything because
  `config_validate` is the real function the reload path calls.

- `g_refs` IS INDEXED BY LOADED SAMPLE ID, NOT BY PLAN ORDER, so
  `reload_commit` writes it THROUGH `map` (`g_refs[map[i]] = p->refs[i]`) and
  never with a straight copy. On the fast path the key table is remapped onto
  the buffers already loaded, so a plan-order commit leaves `g_refs[id]`
  describing a buffer sitting at some other index. Nothing breaks on THAT
  reload; the NEXT one builds its compare against the mis-indexed set and
  keys start playing the wrong sample, silently. `maptest` case M runs three
  reorder-only reloads in a row precisely because one is not enough to see it.

- `g_refs[].path` BORROWS THE LIVE CONFIG'S STRINGS. The compare must run
  before the old config is freed, and `reload_commit` hands `p->cfg` over to
  BECOME the live config rather than freeing it. Do not `config_free` a plan
  that has been committed.

- THE FAST PATH NEEDS FOUR CONDITIONS, not one: the ref set unchanged AND the
  latency unchanged AND `audio_available == cfg.audio_enabled` AND every
  committed ref actually loaded. The last two are not paranoia — without them,
  saving an unchanged config to retry a PipeWire that was down, or to pick up
  a sample file that has since appeared, takes the no-op path and does
  nothing at all.

- `reconcile_devices` ONLY OPENS; `devices_refilter` IS WHAT CLOSES. A reload
  must run both, refilter first. In auto mode the discovery predicate is
  DERIVED FROM THE BINDINGS, so adding one exotic key narrows which boards
  qualify, and an already-open board would otherwise keep sounding keys the
  new config never mentions. The drop is logged with the offending key name
  (`auto_missing_key`), because this is the "adding a binding silently
  narrows discovery" consequence biting live, and with no reason given it
  reads as a bug.

- THE CONFIG WATCH IS ON THE PARENT DIRECTORY, never on the file. Editors and
  dotfile managers save by writing a temp file and renaming over the target,
  which replaces the inode — a watch on the file follows the old one and never
  fires again. `IN_CLOSE_WRITE | IN_MOVED_TO`, and deliberately NOT
  `IN_MODIFY`: both fire only once the file is COMPLETE, which is what removes
  the need for a debounce timer.

- SIGHUP GOES THROUGH AN EVENTFD, not a bare flag. A `volatile sig_atomic_t`
  checked at the top of the loop races with the `epoll_wait` it precedes: a
  signal landing in that window is not acted on until the next key event, so
  a reload on an idle daemon appears not to have happened. `write()` is
  async-signal-safe, and the eventfd-behind-an-epoll-tag pattern is already
  how the audio thread wakes this loop.

- RELOADING DECODES ON THE EPOLL THREAD, AT `SCHED_FIFO` 90.
  `SRC_SINC_BEST_QUALITY` is tens of milliseconds per sample, so a reload that
  changes the sample set drops the keypresses landing during it and can xrun
  the graph. Bounded, and user-triggered; the alternative is decoding in RT,
  which is not an alternative. Worth knowing before anything else is added to
  the reload path.

## Key invariants to preserve when editing the audio mixer

- THE TRIGGER RING IS THE ONLY STATE SHARED BETWEEN THREADS. Not "the main
  shared state" — the only one. Every other field the mixer touches is
  private to one side, which is why there is not an atomic anywhere in
  `mix_voice_t`. The previous design had four atomics PER STREAM
  (`playing`/`pending`/`reset`/`frame_pos`) and would have grown four more
  per sample added; this one does not grow at all.
- THERE IS EXACTLY ONE PRODUCER, and it is the epoll loop. `drain_device` is
  now the ONLY caller of `audio_trigger`, so this holds by construction
  rather than by discipline — which is worth keeping that way. The ring's
  `head`/`tail` protocol is SPSC and is wrong the moment a second thread
  pushes. If audio ever needs triggering from somewhere else, that is a
  design change, not a call site.
- ALLOCATION AND STEALING HAPPEN ON THE CONSUMER SIDE, inside
  `mix_drain_triggers`. It is tempting to have `audio_trigger` pick the
  voice, since it already knows the sample — but a producer reaching into
  the pool would need a per-voice atomic flag to claim a slot, which is
  precisely the per-stream atomics this design removed. The producer's whole
  job is to write one byte and bump `head`.
- `audio_mix` DOES NOT ALLOCATE, LOCK, OR MAKE A SYSCALL. It runs on
  PipeWire's RT thread. Anything that would (loading a sample, resizing the
  pool, logging) belongs at startup.
- SAMPLES ARE CONFORMED AT LOAD, NEVER IN RT. `sample_conform` is the reason
  mixing is possible at all: one shared output buffer means every sample
  must already be at `MIX_RATE`/`MIX_CHANNELS` before the callback sees it.
  `MIX_RATE` is a compile-time constant and must stay one — deriving it from
  the loaded samples would make the graph's rate depend on which WAV
  happened to open successfully, and the loader has to know its target
  before it resamples.
- `sample_decode` STAYS A PURE DECODER. It returns an interleaved float
  buffer and reports the file's own rate and channel count; it does not know
  `MIX_RATE` exists. Folding the conversion back into it would make the
  conform path untestable without a file on disk, which is what `mixtest`
  cases L and M rely on. It is a libsndfile wrapper and should stay one — the
  hand-rolled RIFF reader it replaced accepted only `fmt == 1`, which
  rejected float32 and `WAVE_FORMAT_EXTENSIBLE` (what ffmpeg and Audacity
  emit above 16-bit or 2 channels) and could not open an `.ogg` or `.mp3` at
  all. Verified bit-identical to that reader on the shipped `click.wav`.
- THE SUBSYSTEM IS ONE REGION WITH A SMALL ENTRY SET: `audio_load`,
  `audio_start`, `audio_trigger`, `audio_mix`, `audio_cleanup` (plus
  `audio_wake_fd`/`audio_restart` for the epoll loop, and
  `audio_load_into`/`audio_swap_samples`/`audio_relatency` for reload). Sample loading used to
  sit inline in `main()`, outside the banners. It does not any more, and what
  is left in `main()` is only what a DIFFERENT program would write
  differently: which sample id each distinct sound got, and where its path
  and gain came from. Keep new audio work inside the region — it was lifted
  out of `doubletap` as a block precisely because it stayed one, and the
  next program to want it should be able to do the same.
- A FULL RING DROPS AND COUNTS. `audio_trigger` never blocks and never
  spins: it is called from the input path, and a stalled epoll loop costs a
  keypress, which matters infinitely more than a click. 64 entries against a
  ~10ms graph quantum is not reachable by human hands; the counter exists to
  prove that rather than to be acted on.
- VOICE STEALING TAKES THE OLDEST, by signed `seq` difference so a wrapped
  counter still orders correctly. Three lines, and unreachable in practice
  with 32 voices — but a pool that silently dropped the NEWEST trigger would
  swallow the note you just played, which is the one that matters.
- THE RESTART FLAG IS CLEARED AFTER THE TEARDOWN, NEVER BEFORE, and this is
  not defensive: `pw_stream_destroy` EMITS a final `state_changed` whenever
  the stream was not already unconnected. Measured — a clean SIGTERM reports
  `paused -> unconnected`, and a rebuild's own close does the same. Clearing
  first leaves that self-inflicted signal standing, so the next epoll wake
  tears down the stream just built, forever. `audio.closing` suppresses the
  log line for teardowns we asked for; the post-close clear plus the eventfd
  drain is what actually breaks the loop.
- THE RING IS DISCARDED ON REBUILD (`trig.tail = trig.head`). `audio_trigger`
  keeps pushing while the stream is dead, so the ring can be full of entries
  seconds old by the time a new one comes up — every one of which clamps to
  frame 0 and fires at once. Losing voices across a rebuild is intended; a
  machine-gun burst of stale ones is worse than losing them.
- THE REBUILD NEVER RUNS IN A STREAM CALLBACK. `on_state_changed` sets a
  flag and writes to an eventfd; the epoll loop calls `audio_restart`. A
  stream cannot be destroyed from inside its own callback, and the failure
  that forces this is not the obvious one: a sink disappearing errors the
  node, but the PipeWire DAEMON restarting takes the core with it and
  surfaces as `UNCONNECTED`, which no amount of `pw_stream_set_active`
  revives. Both route to the same destroy-and-rebuild, and the loaded samples
  survive it — only `audio_stream_close` runs, not `audio_cleanup`.
- AUTOMATIC STARTUP-FAILURE RETRY IS DELIBERATELY ABSENT. There is no stream
  to raise a callback when `audio_start` fails, so recovering it ON A TIMER
  needs a timerfd in the epoll loop — a second mechanism for a case the user
  can already fix. `audio_start` warns and the daemon runs silently.
  A CONFIG RELOAD IS THE EXCEPTION, and is the intended recovery: if PipeWire
  was down at startup, `config_validate` sees `audio_available == 0` against
  an `enabled` config and returns `RELOAD_AUDIO_ON`, which runs the full
  `audio_start` path and registers the NEW wake eventfd with epoll (`run_loop`
  only does that once). User-triggered, not a timer, so the reason the
  timerfd was rejected still holds.
- `audio.latency` IS CONFIG, NOT A CONSTANT, and the reason is that its cost
  is not local: PipeWire runs the whole graph at the minimum latency any node
  requests, so this daemon holds the entire session at whatever is set for as
  long as it runs. Verified at 256: the node AND the output sink both drop to
  256 frames. Hardcoding it small would be picking a fight with every
  Bluetooth headset on the user's behalf.
- `AUDIO_NSAMPLES` (24) AND `AUDIO_MAX_VOICES` (32) ARE DIFFERENT NUMBERS
  and must stay that way — different LITERALS, not merely different in
  principle. They were the same number in the old per-stream design and that
  is exactly what made overlapping clicks impossible. `AUDIO_NSAMPLES` is a
  ceiling on distinct SOUNDS, not on bound keys: `bindings_plan` interns on
  the (path, gain) pair, so ten keys sharing a hitsound cost one entry. More
  keys means growing nothing; more distinct sounds means growing the first
  alone.
- THE OUTPUT IS SUMMED IN FLOAT AND CLAMPED. No limiter, no per-voice
  ducking: with realistic overlap the clamp never engages, and a click is
  not worth a compressor. If a test's expected value comes back exactly
  1.0, suspect the fixture's amplitudes before suspecting the mixer.
- A TRIGGER IS PLACED AT THE FRAME IT FELL ON, not at the start of the next
  buffer. The ring entry carries the event's `CLOCK_MONOTONIC` timestamp and
  the voice carries a `start_off`; `mix_drain_triggers` maps the window
  `[cycle_ns - nframes, cycle_ns)` onto `[0, nframes)`. This was a non-goal
  right up until the node stopped inheriting a 1024-frame quantum: dropping
  everything at frame 0 quantises every burst to the graph quantum, so
  identical taps came out up to 21ms apart. It buys that back for ONE FIXED
  QUANTUM of added latency, which is the trade on purpose — a constant
  latency is something hands adapt to and jitter is something you hear.
- THE PLACEMENT MATH ROUNDS, and degrades toward frame 0. A frame is 20833ns
  at 48kHz, so truncating puts a timestamp exactly N frames back at N-1 and
  biases every voice one frame late — a systematic error in the one direction
  this path exists to remove. A stale trigger (a backlog, a stalled loop)
  clamps to 0 and plays at once; a timestamp from the future clamps to the
  last frame; a missing `cycle_ns` or `t_ns` means frame 0. It never drops a
  voice for being badly timed. `mixtest` case O pins all four.
- THE TIMESTAMP IS ONLY MEANINGFUL BECAUSE THE INPUT PATH IS ON
  `CLOCK_MONOTONIC`. evdev stamps `CLOCK_REALTIME` by default, so
  `input_try_open` calls `libevdev_set_clock_id` — through libevdev, NOT a
  raw `EVIOCSCLOCKID` ioctl, because libevdev keeps its own `clock_id` to
  stamp the events it synthesises during `SYN_DROPPED` resync and an ioctl
  behind its back leaves those on REALTIME. Getting this wrong is SILENT:
  the two epochs differ by decades, every age clamps, and every trigger
  quietly reverts to frame 0. There is no error and nothing to see except a
  burst that comes out unevenly spaced, so the only cheap check is to read a
  ring entry's `t_ns` back and compare it against `CLOCK_MONOTONIC`.
