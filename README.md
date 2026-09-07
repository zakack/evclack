# evclack

A keyboard hitsound daemon for Linux — press a key, hear a sound.

`evclack` watches your keyboards at the evdev level and plays a sample on
every press of a key you configure. It does **not** grab anything, does not
remap anything, and creates no virtual device: your keys reach the rest of
the system exactly as they always did, and evclack only listens. That is the
whole design, and it is what makes it safe to leave running alongside a
game, a remapper, or anything else.

Every sound overlaps rather than cutting off the one before it — including
two presses of the same key — and each one is placed at the frame the key
event actually fell on, so the spacing between two taps survives to the
speaker. Point it at an osu! skin's hitsounds and they work as they ship.

## Features

- **Passive** — read-only on `/dev/input/event*`. No `EVIOCGRAB`, no uinput,
  no udev rule, no write access to anything under `/dev`. Works identically
  under Xorg and Wayland, and cannot swallow, delay or alter a keystroke.
- **Per-key samples and volumes** — bind as many keys as you like, each with
  its own sound and gain, or share one default across all of them. Bindings
  that name the same file at the same volume are loaded once.
- **Real polyphony** — one PipeWire output node with an internal mixer and a
  pool of 32 voices. Nothing restarts, nothing truncates. The shipped click
  is 125ms and a 300 BPM stream is one tap every 50ms, so anything that
  restarted playback would clip every note of a burst to 40%.
- **Sub-buffer timing** — the node requests a 256-frame quantum instead of
  inheriting the graph's 1024, and each trigger carries the key event's
  `CLOCK_MONOTONIC` timestamp, so a voice starts at the frame it belongs on
  rather than at the top of the next buffer.
- **Any format libsndfile reads** — WAV (any bit depth, float and
  `WAVE_FORMAT_EXTENSIBLE` included), FLAC, OGG/Vorbis, Opus and MP3, at any
  sample rate and channel count. Decoded and resampled once, at startup.
- **Auto-discovery + hotplug** — by default every keyboard-shaped device
  carrying every bound key is opened; unplugged keyboards are dropped and
  reopened on replug (inotify-driven). You can also pin an explicit device
  list.
- **Works under a remapper** — virtual keyboards are watched too, so if
  keyd or a SOCD cleaner has grabbed your board, evclack still hears you.
- **Low latency** — single-threaded epoll loop, best-effort `SCHED_FIFO`
  realtime scheduling, and `mlockall` once audio is up.

The whole thing is a single-file C11 daemon (`evclack.c`).

## Requirements

- Linux with evdev (any remotely modern kernel)
- [libevdev](https://www.freedesktop.org/wiki/Software/libevdev/)
- [libyaml](https://pyyaml.org/wiki/LibYAML)
- [libsndfile](https://libsndfile.github.io/libsndfile/) and
  [libsamplerate](https://libsndfile.github.io/libsamplerate/)
- [PipeWire](https://pipewire.org/) (`libpipewire-0.3`)
- CMake ≥ 3.10, a C11 compiler, and `pkg-config` to build

## Installation

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

This installs the `evclack` binary, the default config and click sample
(`/usr/share/evclack/`), and a systemd **user** unit.

## Setup

`evclack` runs as an **unprivileged user service** — not as root. (It must
not: PipeWire is a per-user session daemon, so a root system service would
have no audio.)

1. Add yourself to the `input` group, which covers read access to
   `/dev/input/event*`, then log out and back in:

   ```sh
   sudo usermod -aG input $USER
   ```

   That is the only privilege evclack wants. Nothing is grabbed and nothing
   is emitted, so there is no `/dev/uinput` access and no udev rule.

2. Copy the example config and edit it to taste:

   ```sh
   mkdir -p ~/.config/evclack
   cp /usr/share/evclack/config.yaml ~/.config/evclack/
   ```

3. Enable the service:

   ```sh
   systemctl --user enable --now evclack
   ```

Optional: for `SCHED_FIFO` realtime scheduling, grant your user realtime
privileges (on Arch, install `realtime-privileges` and join the `realtime`
group). The daemon warns and falls back gracefully without it.

## Configuration

Config is read from `~/.config/evclack/config.yaml` (`$XDG_CONFIG_HOME`
respected), falling back to the installed default. See the extensively
commented [`config.yaml`](config.yaml) for the full schema.

**Edits apply immediately.** The daemon watches the config file and reloads
itself when you save; `systemctl --user reload evclack` (SIGHUP) does the
same on demand. Keys, samples, gains, devices and latency can all change on
a running daemon, and a config that does not parse or is rejected is logged
and ignored rather than taken — the last good one keeps running. Rebinding a
key is seamless; changing a sample or the latency rebuilds the output node,
which costs the clicks in flight. Creating `~/.config/evclack/config.yaml`
for the *first* time still needs a restart, since until it exists the daemon
is watching the fallback's directory.

The short version:

```yaml
# Omit `devices` (or set it to "auto") to watch every keyboard that has
# every bound key. To pin specific keyboards, use stable by-id paths:
# devices:
#   - /dev/input/by-id/usb-Your_Keyboard-event-kbd

keys:
  - KEY_Z                                       # uses the audio defaults
  - KEY_X
  - {key: KEY_C, sample: /path/to/clack.ogg}    # its own sound
  - {key: KEY_V, gain: 0.6}                     # its own volume

audio:
  enabled: true
  sample: /usr/share/evclack/click.wav   # default for bindings omitting one
  gain: 1.0                              # linear, 1.0 = unity
  latency: 256                           # output quantum, frames @48kHz
```

A key that is not listed is silent. Releases and autorepeat are silent too:
holding a key clacks once, not at the repeat rate.

### Key bindings

Codes can be symbolic (`KEY_Z`) or numeric (`44`); see
`/usr/include/linux/input-event-codes.h` for the full list. An entry is
either a bare code or a `{key, sample, gain}` mapping, and anything it
leaves out is filled in from the `audio` block — so the common case is just
a list of names. The two blocks may appear in either order in the file.

There is no limit on how many keys you may bind. The one real ceiling is
**24 distinct (file, gain) pairs**: bindings that agree on both share a
loaded sample, so pointing thirty keys at one hitsound costs one.

### Latency

`audio.latency` is the output quantum requested from PipeWire, in frames at
48kHz. Without it the node inherits the graph quantum — 1024 on a stock
PipeWire, so a click landed anywhere in a 21ms window and a burst came out
unevenly spaced.

Lower is not free, and the cost is not local: **PipeWire runs the whole
graph at the minimum latency any node asks for**, so evclack holds your
entire session at whatever you set for as long as it runs. 256 (5.3ms) is
right for wired output; a Bluetooth headset or a loaded machine may want 512
or 1024, and will crackle if you insist otherwise. Range 16..8192.

## Running manually

```
usage: evclack [-h] [-c CONFIG] [-i DIR]

options:
    -h          show this help and exit
    -c CONFIG   path to YAML config
    -i DIR      directory to scan/watch for event devices
                (default /dev/input; mainly for testing)
```

Handy for trying config changes without touching the service (and it picks
up edits to the file while it runs, same as the service does):

```sh
./build/evclack -c config.yaml
```

## How it works

1. **Plan** — the bindings are resolved into a `KEY_CNT`-wide lookup table
   and a list of distinct samples, before anything is opened. A duplicate
   key or an over-full sample table is rejected here rather than after a
   PipeWire graph has been built.
2. **Load** — every distinct sample is decoded (libsndfile) and conformed to
   48kHz stereo (libsamplerate) once, at startup. Mixing is what forces
   that: one shared output buffer means everything has to agree on a format
   before the realtime callback ever sees it.
3. **Watch** — keyboards are opened read-only, never grabbed, and joined to
   a single epoll loop alongside an inotify watch for hotplug.
4. **Trigger** — a key-down on a bound key pushes one entry onto a
   lock-free ring: a sample id and the event's own `CLOCK_MONOTONIC`
   timestamp. That is all the input thread does.
5. **Mix** — PipeWire's realtime thread drains the ring, gives each trigger
   a voice from a pool of 32 placed at the frame it fell on, sums the active
   voices into the output buffer, retires the ones that ran out, and clamps.
   No allocation, no locks, no syscalls.

## Running alongside a remapper

evclack watches virtual keyboards on purpose. A remapper such as `keyd` — or
`doubletap`, the SOCD cleaner this was carved out of — grabs your physical
board exclusively, so the real keystrokes exist only on the uinput device it
emits; ignoring those would leave evclack silent on exactly the setups most
likely to want it. There is nothing to double up, because a grabbed node
yields no events to a passive reader.

One thing to watch: if the remapper also plays its own click, you will hear
both. Disable audio in one of them.

## Development

`tools/mixtest.c` and `tools/maptest.c` `#include evclack.c` outright, so
they exercise the daemon's real code rather than a copy that could drift.
Both build as part of `all`, deliberately — the guarantee is worthless if
the binaries can go stale.

- `build/mixtest` — assertions over the voice pool, the SPSC trigger ring,
  the summed output buffer, sample conforming, and sub-buffer placement.
- `build/maptest` — assertions over the config parser and the key → sample
  layer: interning, per-key overrides and their fallbacks, and the
  rejections.

## License

See [LICENSE.md](LICENSE.md).
