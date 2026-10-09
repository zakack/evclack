/*
 * evclack - keyboard hitsound daemon
 *
 * The daemon passively reads one or more evdev keyboard devices - either an
 * explicit config list or, by default, every keyboard-shaped device that
 * advertises every configured key, with inotify-driven hotplug in both
 * cases - and plays a sample through PipeWire on every press of a
 * configured key.
 *
 * Nothing is grabbed, remapped, filtered or re-emitted: the keys reach the
 * rest of the system exactly as they always did, and evclack only listens.
 * Releases and autorepeat are silent; only the key-down edge sounds.
 *
 * Every trigger overlaps rather than cutting off the one before it. One
 * PipeWire node, an internal polyphonic mixer, and a pool of voices - and
 * each voice is placed at the frame the key event actually fell on, so the
 * spacing between two taps survives to the speaker.
 *
 * Copyright 2026 Zachary Kessler
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/eventfd.h>
#include <sys/mman.h>

#include <linux/input.h>

#include <libevdev/libevdev.h>

#include <samplerate.h>
#include <sndfile.h>
#include <yaml.h>

#include <spa/param/audio/format-utils.h>
#include <pipewire/pipewire.h>

/* ------------------------------------------------------------------------- */
/* Defaults                                                                  */
/* ------------------------------------------------------------------------- */

/* Installed data paths; CMake overrides these to match the install prefix. */
#ifndef DEF_WAV
#define DEF_WAV    "/usr/share/evclack/click.wav"
#endif
#ifndef DEF_CONFIG
#define DEF_CONFIG "/usr/share/evclack/config.yaml"
#endif

/* ------------------------------------------------------------------------- */
/* Logging                                                                   */
/* ------------------------------------------------------------------------- */

static void logf_(const char *level, const char *fmt, ...)
__attribute__((format(printf, 2, 3)));

/* Set by the offline tools while they drive the daemon's own code. They run
 * the same translation unit, so the daemon's progress chatter would come out
 * of them too. Warnings and errors are never suppressed; only the running
 * commentary. */
static int g_log_quiet;

static void logf_(const char *level, const char *fmt, ...) {
    va_list ap;
    if (g_log_quiet && strcmp(level, "info") == 0)
        return;
    fprintf(stderr, "[evclack] %s: ", level);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

#define LOG_INFO(...) logf_("info",  __VA_ARGS__)
#define LOG_WARN(...) logf_("warn",  __VA_ARGS__)
#define LOG_ERR(...)  logf_("error", __VA_ARGS__)

/* ------------------------------------------------------------------------- */
/* Signals                                                                   */
/* ------------------------------------------------------------------------- */

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_reload;

/* Written by the handler to wake epoll_wait. A bare flag checked at the top
 * of the loop is not enough: a SIGHUP landing between the check and the
 * epoll_wait it precedes is not delivered until the NEXT event, so a reload
 * on an idle daemon would appear not to have happened until the user pressed
 * a key. write() is async-signal-safe, and an eventfd behind an epoll tag is
 * already how the audio thread wakes this loop. */
static int g_sigfd = -1;

static void on_signal(int sig) {
    if (sig == SIGHUP)
        g_reload = 1;
    else
        g_running = 0;

    if (g_sigfd >= 0) {
        uint64_t one = 1;
        ssize_t  n = write(g_sigfd, &one, sizeof(one));
        (void)n;   /* EAGAIN here means the counter is already raised */
    }
}

/* ------------------------------------------------------------------------- */
/* Audio (PipeWire)                                                          */
/* ------------------------------------------------------------------------- */

static int audio_available;

/* The mixer's internal format. FIXED at compile time rather than derived
 * from whatever loaded: the WAV loader has to know its resampling target
 * before it runs, and deriving the rate from "the first sample that opened
 * successfully" would make the graph's rate depend on which load failed.
 * 48kHz is PipeWire's default graph rate, so the shipped click costs
 * nothing to play. */
#define MIX_RATE     48000
#define MIX_CHANNELS 2

/* Requested node quantum, in frames at MIX_RATE. Without a node.latency the
 * stream inherits the graph quantum, which is 1024 on a stock PipeWire - so
 * a click landed anywhere in a 21ms window. 256 frames is 5.3ms.
 *
 * Overridable from config rather than fixed, because a node.latency is a
 * request the graph honours by running EVERY node at the minimum of all of
 * them: a session-long daemon asking for 256 drags the whole graph down for
 * as long as it runs. That is the right default for a rhythm game on wired
 * output and the wrong one for a Bluetooth headset. */
#define AUDIO_LATENCY_DEFAULT 256

/* Distinct loaded samples - the ceiling on how many different sounds a
 * config can map. Bindings that name the same file at the same gain are
 * interned onto one entry (see bindings_plan), so this is a count of
 * SAMPLES, not of keys; the key table is KEY_CNT wide and costs nothing.
 *
 * Deliberately not equal to AUDIO_MAX_VOICES. How many samples are MAPPED
 * and how many sounds can be IN FLIGHT were the same number in the old
 * one-stream-per-sample design, and that is exactly what made overlapping
 * clicks impossible. Keeping the two literals different is a stronger
 * statement than a comment saying they differ. The ring entry is a uint8_t,
 * so anything under 256 works.
 *
 * 160 rather than the two dozen a hand-written config needs, because an
 * IMPORTED PACK is what sets the floor now: a Mechvibes soundpack slices one
 * recording into a separate sound per key, so ~100 distinct samples is the
 * ordinary case rather than the pathological one. The cost is the static
 * audio.sample[] table and two scratch arrays a reload puts on the stack -
 * 24 bytes an entry, so a few KB - not decode time, which is paid per
 * sample the config actually names. */
#define AUDIO_NSAMPLES 160

/* Concurrent sounds. Far more than a pair of fingers can ask for; the pool
 * is ~1KB static and exhaustion is a formality (see mix_voice_alloc). */
#define AUDIO_MAX_VOICES 32

/* Trigger ring depth. Power of two, indexed by free-running counters. */
#define TRIG_RING 64

/* A loaded, CONFORMED sample: interleaved float at MIX_RATE/MIX_CHANNELS.
 * All rate and channel conversion happened at load time, so nothing in the
 * RT callback has to know what the file on disk looked like. */
typedef struct {
    float  *samples;
    size_t  num_frames;
    float   gain;        /* linear multiplier, 1.0 = unity */
} audio_sample_t;

/* ONE DISTINCT SOUND, as the loader is asked for it: a file, the volume to
 * play it at, and optionally the slice of that file to take.
 *
 * `start_ms`/`end_ms` are a half-open window in milliseconds of the SOURCE
 * file; an all-zero pair means the whole file, which is what every
 * hand-written binding produces and what the zero-initialised struct already
 * means. The slice exists because a Mechvibes soundpack is a sprite sheet -
 * one 20-60 second recording plus a window per key - so the alternative
 * would be an importer that splits a pack into a hundred derived WAVs and a
 * config that no longer points at the pack it came from.
 *
 * Lives here rather than beside bindings_plan because this is the loader's
 * input, and audio_load_refs has to see it. Why the interning is on the
 * whole tuple, and not on the path alone, is bindings_plan's business - the
 * rationale is with the code that does it. */
typedef struct {
    const char *path;
    float       gain;
    double      start_ms;   /* 0 with end_ms 0 == the whole file */
    double      end_ms;     /* 0 == to the end */
} sample_ref_t;

/* A PLAYING INSTANCE of a sample. This is the thing a keypress allocates.
 * Owned exclusively by the RT thread - only audio_mix() ever reads or writes
 * one - which is why there is not an atomic in sight here. */
typedef struct {
    const float *samples;   /* borrowed from audio.sample[]; RT never frees */
    size_t       nframes;
    size_t       pos;
    size_t       start_off; /* frames into its FIRST buffer; 0 thereafter */
    float        gain;
    uint32_t     seq;       /* allocation order, for oldest-steal */
    bool         active;
} mix_voice_t;

static struct {
    struct pw_thread_loop *loop;
    struct pw_stream      *stream;    /* ONE node, whatever is mapped */
    audio_sample_t         sample[AUDIO_NSAMPLES];
    mix_voice_t            voice[AUDIO_MAX_VOICES];
    uint32_t               seq_next;  /* RT-thread private, like the pool */
    unsigned               latency;   /* requested quantum, frames */
    int                    wakefd;    /* eventfd: RT/thread-loop -> epoll */
    atomic_int             restart;   /* set by state_changed, read by epoll */
    atomic_int             closing;   /* teardown in progress: ignore states */
} audio;

/* The ONLY state shared between the epoll thread and the RT thread.
 * Single-producer/single-consumer: the producer is the epoll loop and only
 * the epoll loop - drain_device is the sole caller of audio_trigger, so the
 * single-producer rule holds by construction rather than by discipline. The
 * consumer is audio_mix(). head and tail free-run and wrap; the unsigned
 * difference is the fill level. */
static struct {
    struct {
        uint8_t  sample;           /* sample index */
        uint64_t t_ns;             /* CLOCK_MONOTONIC, when the key moved */
    }           e[TRIG_RING];
    atomic_uint head;              /* producer writes */
    atomic_uint tail;              /* consumer writes */
    atomic_uint overruns;          /* triggers dropped on a full ring */
} trig;

/* Pure decoder: a file on disk in, interleaved float out. Knows nothing about
 * the mix format - conforming to it is sample_conform's job, kept separate so
 * the loader stays testable against a buffer with no file behind it.
 *
 * libsndfile rather than a hand-rolled RIFF reader. The reader this replaced
 * accepted only fmt == 1, which rejected float32 WAVs and WAVE_FORMAT_EXTENSIBLE
 * - what ffmpeg and Audacity emit above 16-bit or 2 channels - and could not
 * read .ogg or .mp3 at all. osu! skins ship all three, and a sampler that makes
 * you transcode your own skin before it will play is the wrong trade for ninety
 * lines of chunk parsing. One dependency covers WAV/FLAC/OGG/Opus/MP3.
 *
 * sf_readf_float normalises every format to [-1, 1] for us, so the bit-depth
 * conversion ladder goes with the parser. */
static float *sample_decode(const char *path, size_t *frames_out,
                            int *ch_out, int *rate_out) {
    SF_INFO   si = { 0 };
    SNDFILE  *sf = sf_open(path, SFM_READ, &si);
    if (!sf) {
        LOG_WARN("Cannot open sample %s: %s", path, sf_strerror(NULL));
        return NULL;
    }
    if (si.frames <= 0 || si.channels <= 0 || si.samplerate <= 0) {
        LOG_WARN("Empty or unreadable sample: %s", path);
        sf_close(sf);
        return NULL;
    }

    size_t frames = (size_t)si.frames;
    float *out = calloc(frames * (size_t)si.channels, sizeof(float));
    if (!out) { sf_close(sf); LOG_ERR("oom"); return NULL; }

    sf_count_t got = sf_readf_float(sf, out, si.frames);
    sf_close(sf);
    if (got <= 0) {
        LOG_WARN("Decoded no frames from %s", path);
        free(out);
        return NULL;
    }
    /* A short read is not fatal - a truncated file still plays what it has -
     * but the buffer has to agree with what was actually decoded. */
    if (got < si.frames)
        LOG_WARN("%s: expected %lld frames, decoded %lld",
                 path, (long long)si.frames, (long long)got);
    frames = (size_t)got;

    LOG_INFO("Loaded %s: %zu frames, %d ch, %d Hz",
             path, frames, si.channels, si.samplerate);

    *frames_out = frames;
    *ch_out     = si.channels;
    *rate_out   = si.samplerate;
    return out;
}

/* Bring a decoded buffer to MIX_RATE/MIX_CHANNELS. Does NOT take ownership
 * of `in`; the caller frees it either way.
 *
 * Mixing is what forces this: with a stream per sample, PipeWire negotiated
 * a format per stream and converted for us. One shared output buffer means
 * every sample has to already agree on rate and channel count by the time it
 * reaches the callback. Doing it here is offline startup work and strictly
 * better for latency than converting on the graph every cycle. */
static int sample_conform(const float *in, size_t frames, int ch, int rate,
                          audio_sample_t *out) {
    if (!in || frames == 0 || ch <= 0 || rate <= 0) return -1;

    /* 1. Channels. Mono is duplicated rather than left to PipeWire, since
     *    the mix buffer is a single interleaved layout by then. */
    float *lay = calloc(frames * MIX_CHANNELS, sizeof(float));
    if (!lay) return -1;
    if (ch == 1) {
        for (size_t i = 0; i < frames; i++)
            lay[i * 2] = lay[i * 2 + 1] = in[i];
    } else {
        if (ch > MIX_CHANNELS)
            LOG_WARN("Sample has %d channels; using the first %d",
                     ch, MIX_CHANNELS);
        for (size_t i = 0; i < frames; i++) {
            lay[i * 2]     = in[i * (size_t)ch];
            lay[i * 2 + 1] = in[i * (size_t)ch + 1];
        }
    }

    /* 2. Rate. */
    if (rate == MIX_RATE) {
        out->samples    = lay;
        out->num_frames = frames;
        return 0;
    }

    double ratio = (double)MIX_RATE / (double)rate;
    if (!src_is_valid_ratio(ratio)) {
        LOG_WARN("Cannot resample %d Hz to %d Hz", rate, MIX_RATE);
        free(lay);
        return -1;
    }

    /* Headroom, not a tight bound: src_simple stops at output_frames and a
     * cap sitting exactly on the arithmetic answer clips the sinc tail. */
    size_t cap = (size_t)(frames * ratio) + 16;
    float *res = calloc(cap * MIX_CHANNELS, sizeof(float));
    if (!res) { free(lay); return -1; }

    SRC_DATA d = {
        .data_in       = lay,
        .input_frames  = (long)frames,
        .data_out      = res,
        .output_frames = (long)cap,
        .src_ratio     = ratio,
    };
    int err = src_simple(&d, SRC_SINC_BEST_QUALITY, MIX_CHANNELS);
    free(lay);
    if (err) {
        LOG_WARN("Resample failed: %s", src_strerror(err));
        free(res);
        return -1;
    }

    LOG_INFO("Resampled %d Hz -> %d Hz: %zu -> %ld frames",
             rate, MIX_RATE, frames, d.output_frames_gen);
    out->samples    = res;
    out->num_frames = (size_t)d.output_frames_gen;
    return 0;
}

/* One decoded file, held only long enough for every ref that names it.
 *
 * This is NOT an optimisation, and dropping it does not merely make startup
 * slower. A Mechvibes pack points a hundred refs at one 50-second .ogg, and
 * decoding plus SRC_SINC_BEST_QUALITY on 50 seconds of stereo audio costs
 * seconds; paid a hundred times it is minutes of startup and minutes of
 * epoll-thread stall on every config save. Caching the decode makes the cost
 * proportional to the number of FILES, and the conform proportional to the
 * total sliced audio, which is the shape it has to have.
 *
 * One slot is enough because audio_load_refs groups by path before asking. */
typedef struct {
    char   *path;      /* strdup; NULL when the slot is empty */
    float  *raw;
    size_t  frames;
    int     ch, rate;
} decode_cache_t;

static void decode_cache_clear(decode_cache_t *dc) {
    if (!dc) return;
    free(dc->path);
    free(dc->raw);
    memset(dc, 0, sizeof(*dc));
}

/* Borrowed on success - the cache keeps ownership and frees on the next
 * clear. NULL means the file would not decode. */
static const float *decode_cache_get(decode_cache_t *dc, const char *path,
                                     size_t *frames, int *ch, int *rate) {
    if (dc->path && strcmp(dc->path, path) == 0) {
        *frames = dc->frames; *ch = dc->ch; *rate = dc->rate;
        return dc->raw;
    }
    decode_cache_clear(dc);

    dc->raw = sample_decode(path, &dc->frames, &dc->ch, &dc->rate);
    if (!dc->raw) return NULL;
    dc->path = strdup(path);
    if (!dc->path) { decode_cache_clear(dc); return NULL; }

    *frames = dc->frames; *ch = dc->ch; *rate = dc->rate;
    return dc->raw;
}

/* Narrow a decoded buffer to `ref`'s millisecond window, in SOURCE-rate
 * frames. Returns the first frame and the count; 0 frames means the window
 * selects nothing, which is a load failure rather than a silent sample.
 *
 * SLICING HAPPENS BEFORE sample_conform, NEVER AFTER, and the reason is
 * arithmetic rather than taste: after conforming, the window would have to
 * be converted into resampled frames, and the sinc tail means that mapping
 * is not exact. Cutting first keeps the window in the units the pack author
 * measured it in, and hands the resampler a buffer that is already only as
 * long as the sound - which is also what keeps the conform cost
 * proportional to the audio actually used. */
static int slice_window(const sample_ref_t *ref, size_t frames, int rate,
                        size_t *first_out, size_t *count_out) {
    if (ref->start_ms <= 0.0 && ref->end_ms <= 0.0) {
        *first_out = 0; *count_out = frames;
        return 0;
    }

    double fpms = (double)rate / 1000.0;
    /* Round rather than truncate, for the same reason the trigger placement
     * math does: truncating biases every window one frame early. */
    double fd = ref->start_ms > 0.0 ? ref->start_ms * fpms + 0.5 : 0.0;
    double ld = ref->end_ms   > 0.0 ? ref->end_ms   * fpms + 0.5
                                    : (double)frames;

    if (fd < 0.0) fd = 0.0;
    if (ld > (double)frames) ld = (double)frames;

    size_t first = (size_t)fd;
    size_t last  = ld > 0.0 ? (size_t)ld : 0;

    if (first >= frames || last <= first) {
        LOG_WARN("%s: slice %.1f-%.1f ms selects nothing of a %.1f ms file",
                 ref->path, ref->start_ms, ref->end_ms,
                 (double)frames / fpms);
        return -1;
    }

    *first_out = first;
    *count_out = last - first;
    return 0;
}

/* Decode `ref`, cut it to its window, conform it to the mix format, and
 * write the result into `dst`.
 *
 * This lives here rather than in main() on purpose: it is the last piece of
 * the audio subsystem that used to sit outside the banners, so everything
 * between them is now the whole of it - load, start, trigger, mix, cleanup.
 * What stays the caller's business is which sample id means what and where
 * the paths, gains and slices came from, which is exactly the part that
 * differs between one program and the next.
 *
 * `dc` may be NULL, in which case the file is decoded and thrown away. */
static int audio_load_into(audio_sample_t *dst, const sample_ref_t *ref,
                           decode_cache_t *dc) {
    if (!dst || !ref || !ref->path) return -1;

    decode_cache_t own = { 0 };
    if (!dc) dc = &own;

    size_t frames; int ch, rate;
    const float *raw = decode_cache_get(dc, ref->path, &frames, &ch, &rate);
    if (!raw) { decode_cache_clear(&own); return -1; }

    size_t first, count;
    if (slice_window(ref, frames, rate, &first, &count) != 0) {
        decode_cache_clear(&own);
        return -1;
    }

    int rc = sample_conform(raw + first * (size_t)ch, count, ch, rate, dst);
    decode_cache_clear(&own);
    if (rc != 0) return -1;

    dst->gain = ref->gain;
    return 0;
}

/* Load a whole planned set into `dst[0..n)`, decoding each distinct FILE
 * exactly once however many refs slice it.
 *
 * The grouping is the outer loop rather than a sort, so the caller's ids are
 * untouched: refs keep the ids bindings_plan gave them, and only the ORDER
 * they are visited in changes. A ref that fails to load leaves its slot
 * zeroed, which audio_trigger already reads as nothing to play - so those
 * keys go silent and the rest still sound.
 *
 * Returns the number that failed. */
static int audio_load_refs(audio_sample_t *dst, const sample_ref_t *refs,
                           int n) {
    if (n > AUDIO_NSAMPLES) n = AUDIO_NSAMPLES;

    bool done[AUDIO_NSAMPLES];
    memset(done, 0, sizeof(done));

    decode_cache_t dc = { 0 };
    int failed = 0;

    for (int i = 0; i < n; i++) {
        if (done[i]) continue;
        /* i is the first ref naming this file; every later ref naming the
         * same file rides its decode before the cache moves on. */
        for (int j = i; j < n; j++) {
            if (done[j] || !refs[j].path ||
                strcmp(refs[j].path, refs[i].path) != 0)
                continue;
            done[j] = true;
            if (audio_load_into(&dst[j], &refs[j], &dc) != 0) {
                LOG_WARN("hitsound load failed (%s); the keys bound to it "
                         "stay silent", refs[j].path);
                failed++;
            }
        }
        decode_cache_clear(&dc);
    }
    decode_cache_clear(&dc);
    return failed;
}

/* Take a voice for `s`. RT side, called only from the ring drain.
 *
 * Allocating here rather than in audio_trigger is deliberate and is what
 * keeps the pool atomic-free: a producer that reached into the pool would
 * need a per-voice flag to claim a slot, which is exactly the per-stream
 * atomics this design removes. */
static void mix_voice_alloc(const audio_sample_t *s, size_t start_off) {
    if (!s->samples || s->num_frames == 0) return;

    mix_voice_t *v = NULL;
    for (int i = 0; i < AUDIO_MAX_VOICES; i++) {
        if (!audio.voice[i].active) { v = &audio.voice[i]; break; }
    }
    if (!v) {
        /* Steal the oldest. Signed difference so a wrapped seq still orders
         * correctly. Three lines, and in practice unreachable: 32 voices is
         * far more overlap than two fingers can ask for. */
        v = &audio.voice[0];
        for (int i = 1; i < AUDIO_MAX_VOICES; i++)
            if ((int32_t)(audio.voice[i].seq - v->seq) < 0)
                v = &audio.voice[i];
    }

    v->samples = s->samples;
    v->nframes = s->num_frames;
    v->gain    = s->gain;
    v->pos       = 0;
    v->start_off = start_off;
    v->seq       = audio.seq_next++;
    v->active    = true;
}

/* Drain the trigger ring into the voice pool, placing each voice at its own
 * frame within this buffer.
 *
 * WHY SUB-BUFFER PLACEMENT. Dropping every trigger at frame 0 is minimum
 * latency, but the events being drained arrived at any point across the
 * previous cycle, so identical taps come out up to a whole quantum apart. On
 * a rhythm-game hitsound that jitter is the thing you hear; a constant
 * latency is the thing hands adapt to. So a trigger is placed where it
 * actually fell within the cycle, which costs one fixed quantum and returns
 * an even burst.
 *
 * `cycle_ns` is the monotonic time of THIS cycle, and the events being
 * drained fall in [cycle_ns - nframes, cycle_ns). Mapping that window onto
 * [0, nframes) is the line below. Anything older - a backlog, a stalled loop -
 * clamps to 0 and plays at once, which is the right way to fall over. */
static void mix_drain_triggers(uint64_t cycle_ns, size_t nframes) {
    unsigned t = atomic_load_explicit(&trig.tail, memory_order_relaxed);
    unsigned h = atomic_load_explicit(&trig.head, memory_order_acquire);

    for (; t != h; t++) {
        unsigned s     = trig.e[t & (TRIG_RING - 1)].sample;
        uint64_t t_ns  = trig.e[t & (TRIG_RING - 1)].t_ns;
        if (s >= AUDIO_NSAMPLES) continue;

        size_t off = 0;
        if (cycle_ns && t_ns && nframes) {
            int64_t age = (int64_t)cycle_ns - (int64_t)t_ns;

            /* Bound before multiplying: age is attacker-free but not
             * bounded (a wrong clock, a device stamping garbage), and
             * age * MIX_RATE overflows int64 past about a week. Anything
             * over a second is stale by every measure that matters. */
            if (age > 1000000000) age = 1000000000;
            if (age < 0)          age = 0;

            /* ROUNDED, not truncated. A frame is 20833.3ns at 48kHz, so a
             * timestamp exactly N frames back truncates to N-1 and lands
             * every voice a frame late - small, but a systematic bias in
             * the one direction this whole path exists to remove. */
            int64_t back = (age * MIX_RATE + 500000000) / 1000000000;
            int64_t o    = (int64_t)nframes - back;
            if (o < 0)                    o = 0;
            if (o > (int64_t)nframes - 1) o = (int64_t)nframes - 1;
            off = (size_t)o;
        }
        mix_voice_alloc(&audio.sample[s], off);
    }

    atomic_store_explicit(&trig.tail, t, memory_order_release);
}

/* Render `nframes` of MIX_CHANNELS-interleaved float. The entire mixer, with
 * no PipeWire in it - tools/mixtest.c calls this directly.
 *
 * No malloc, no lock, no syscall: everything it touches is either the ring
 * (two atomics) or the RT thread's own pool. */
static void audio_mix(float *dst, size_t nframes, uint64_t cycle_ns) {
    size_t nsamp = nframes * MIX_CHANNELS;

    mix_drain_triggers(cycle_ns, nframes);
    memset(dst, 0, nsamp * sizeof *dst);

    for (int i = 0; i < AUDIO_MAX_VOICES; i++) {
        mix_voice_t *v = &audio.voice[i];
        if (!v->active) continue;

        /* start_off is the voice's placement within its FIRST buffer only:
         * it delays where the copy begins and shortens what fits, and is
         * cleared below so every later buffer starts at frame 0. */
        size_t off = v->start_off;
        if (off >= nframes) { v->start_off = off - nframes; continue; }
        v->start_off = 0;

        size_t room = nframes - off;
        size_t rem  = v->nframes - v->pos;
        size_t n    = room < rem ? room : rem;

        const float *src = v->samples + v->pos * MIX_CHANNELS;
        float *out = dst + off * MIX_CHANNELS;
        float g = v->gain;
        for (size_t k = 0; k < n * MIX_CHANNELS; k++)
            out[k] += src[k] * g;

        v->pos += n;
        if (v->pos >= v->nframes) v->active = false;
    }

    /* Sum into float, then clamp. With realistic overlap this never
     * engages; a proper limiter would be over-engineering for a click. */
    for (size_t k = 0; k < nsamp; k++) {
        if (dst[k] >  1.0f) dst[k] =  1.0f;
        else if (dst[k] < -1.0f) dst[k] = -1.0f;
    }
}

static void on_process(void *userdata) {
    (void)userdata;
    struct pw_buffer *b;

    if ((b = pw_stream_dequeue_buffer(audio.stream)) == NULL) {
        pw_log_warn("out of buffers: %m");
        return;
    }

    struct spa_buffer *buf = b->buffer;
    float *dst = buf->datas[0].data;
    if (!dst) { pw_stream_queue_buffer(audio.stream, b); return; }

    int stride = (int)(sizeof(float) * MIX_CHANNELS);
    int n_frames = buf->datas[0].maxsize / stride;
    if (b->requested)
        n_frames = SPA_MIN((int)b->requested, n_frames);

    /* The cycle's monotonic timestamp, so the drain can place each trigger
     * where it actually fell. RT safe - it reads a snapshot the graph
     * already captured rather than asking the clock. */
    struct pw_time pwt;
    uint64_t cycle_ns = 0;
    if (pw_stream_get_time_n(audio.stream, &pwt, sizeof(pwt)) == 0)
        cycle_ns = (uint64_t)pwt.now;

    audio_mix(dst, (size_t)n_frames, cycle_ns);

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = stride;
    buf->datas[0].chunk->size   = (uint32_t)n_frames * (uint32_t)stride;

    pw_stream_queue_buffer(audio.stream, b);
}

/* The stream died. This runs on PipeWire's thread loop, and it deliberately
 * does NOT rebuild anything: a stream cannot be destroyed from inside its own
 * callback. It raises a flag and pokes the epoll loop, which owns the rebuild.
 *
 * ERROR and UNCONNECTED are both handled because they are different failures.
 * A sink disappearing errors the node; the PipeWire DAEMON restarting takes
 * the core with it, which surfaces as UNCONNECTED - and no amount of
 * pw_stream_set_active revives a dead core, since pw_stream_new_simple owns
 * it. Both therefore route to the same destroy-and-rebuild. */
static void on_state_changed(void *userdata, enum pw_stream_state old_state,
                             enum pw_stream_state state, const char *error) {
    (void)userdata; (void)old_state;

    if (state != PW_STREAM_STATE_ERROR &&
        state != PW_STREAM_STATE_UNCONNECTED)
        return;

    /* Our own teardown. pw_stream_destroy emits a final state_changed
     * whenever the stream was not already unconnected, so both audio_cleanup
     * and the rebuild's own close land here - and neither is news. Measured:
     * a clean SIGTERM shutdown reports "paused -> unconnected". */
    if (atomic_load(&audio.closing))
        return;

    if (atomic_exchange(&audio.restart, 1))
        return;   /* a rebuild is already pending */

    /* LOG_WARN, not pw_log_warn: this runs on the thread loop, not the data
     * thread, so an fprintf here breaks no RT rule - and pw_log_warn is off
     * at the default log level, which made this path invisible exactly when
     * it needed watching. */
    LOG_WARN("Audio stream %s -> %s (%s); requesting rebuild",
             pw_stream_state_as_string(old_state),
             pw_stream_state_as_string(state), error ? error : "-");

    if (audio.wakefd >= 0) {
        uint64_t one = 1;
        ssize_t  n = write(audio.wakefd, &one, sizeof(one));
        (void)n;  /* an EAGAIN here means the counter is already raised */
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process       = on_process,
    .state_changed = on_state_changed,
};

/* Bring up the thread loop, the node and the format. Split from audio_start
 * because the reconnect path runs it again on a live daemon. */
static int audio_stream_open(void) {
    pw_init(NULL, NULL);

    audio.loop = pw_thread_loop_new("evclack-audio", NULL);
    if (!audio.loop) { pw_deinit(); return -1; }

    pw_thread_loop_lock(audio.loop);
    struct pw_loop *pl = pw_thread_loop_get_loop(audio.loop);

    uint8_t podbuf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(podbuf, sizeof(podbuf));
    const struct spa_pod *params[1];

    char lat[32];
    snprintf(lat, sizeof(lat), "%u/%d", audio.latency, MIX_RATE);

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,     "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE,     "Game",
        PW_KEY_NODE_LATENCY,   lat,
        NULL);

    audio.stream = pw_stream_new_simple(pl, "evclack", props,
                                        &stream_events, NULL);
    if (!audio.stream) {
        pw_thread_loop_unlock(audio.loop);
        pw_thread_loop_destroy(audio.loop);
        audio.loop = NULL;
        pw_deinit();
        return -1;
    }

    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(
            .format   = SPA_AUDIO_FORMAT_F32,
            .channels = MIX_CHANNELS,
            .rate     = MIX_RATE));

    pw_stream_connect(audio.stream,
                      PW_DIRECTION_OUTPUT,
                      PW_ID_ANY,
                      PW_STREAM_FLAG_AUTOCONNECT |
                      PW_STREAM_FLAG_MAP_BUFFERS  |
                      PW_STREAM_FLAG_RT_PROCESS,
                      params, 1);

    pw_thread_loop_unlock(audio.loop);

    if (pw_thread_loop_start(audio.loop) < 0) {
        pw_thread_loop_lock(audio.loop);
        pw_stream_destroy(audio.stream);
        audio.stream = NULL;
        pw_thread_loop_unlock(audio.loop);
        pw_thread_loop_destroy(audio.loop);
        audio.loop = NULL;
        pw_deinit();
        return -1;
    }

    return 0;
}

/* Commit to playing: audio is available if at least one sample mapped AND
 * the stream came up. A sample that failed to load leaves its key silent
 * rather than taking the others down with it.
 *
 * mlockall belongs here rather than in main() because the reason for it is
 * local: the RT callback must not take a page fault. */
static int audio_start(unsigned latency_frames) {
    audio.latency = latency_frames ? latency_frames : AUDIO_LATENCY_DEFAULT;
    audio.wakefd  = -1;   /* explicit: the struct is static, and 0 is stdin */

    int loaded = 0;
    for (int i = 0; i < AUDIO_NSAMPLES; i++)
        if (audio.sample[i].samples) loaded++;

    if (loaded > 0) {
        /* Before the stream, so a state_changed racing the first connect
         * still finds somewhere to put its flag. */
        audio.wakefd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (audio.wakefd < 0)
            LOG_WARN("eventfd: %s - audio will not recover a lost stream",
                     strerror(errno));

        if (audio_stream_open() == 0) {
            mlockall(MCL_CURRENT | MCL_FUTURE);
            audio_available = 1;
            return 0;
        }
        if (audio.wakefd >= 0) { close(audio.wakefd); audio.wakefd = -1; }
    }

    LOG_WARN("Audio disabled (sample load or PipeWire init failed)");
    for (int i = 0; i < AUDIO_NSAMPLES; i++) {
        free(audio.sample[i].samples);
        audio.sample[i].samples = NULL;
    }
    audio_available = 0;
    return -1;
}

/* sample: 0 == V1, 1 == V2. Pushes onto the trigger ring and returns; the
 * RT thread turns it into a voice. Every trigger gets its OWN voice, so a
 * click never cuts off the one before it - not the other key's, and not an
 * earlier press of the same key.
 *
 * PRODUCER SIDE, and there is exactly one producer: the epoll loop. */
static void audio_trigger(int sample, uint64_t t_ns) {
    if (!audio_available) return;
    if (sample < 0 || sample >= AUDIO_NSAMPLES) return;
    if (!audio.sample[sample].samples) return;  /* nothing mapped: silent */

    unsigned h = atomic_load_explicit(&trig.head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&trig.tail, memory_order_acquire);
    if (h - t >= TRIG_RING) {
        /* Drop it. Never block and never spin: this is called from the
         * input path, and a stalled epoll loop costs a keypress. */
        atomic_fetch_add_explicit(&trig.overruns, 1, memory_order_relaxed);
        return;
    }
    trig.e[h & (TRIG_RING - 1)].sample = (uint8_t)sample;
    trig.e[h & (TRIG_RING - 1)].t_ns   = t_ns;
    atomic_store_explicit(&trig.head, h + 1, memory_order_release);
}

/* Tear the node down without touching the samples. The rebuild path needs
 * exactly this much: the loaded, conformed samples outlive a PipeWire restart
 * and re-decoding them would be minutes of work for nothing. */
static void audio_stream_close(void) {
    atomic_store(&audio.closing, 1);
    if (audio.loop) {
        pw_thread_loop_stop(audio.loop);
        pw_thread_loop_lock(audio.loop);
        if (audio.stream) {
            pw_stream_destroy(audio.stream);
            audio.stream = NULL;
        }
        pw_thread_loop_unlock(audio.loop);
        pw_thread_loop_destroy(audio.loop);
        audio.loop = NULL;
    }
    pw_deinit();
    atomic_store(&audio.closing, 0);
}

/* The fd the epoll loop watches to learn the stream needs rebuilding, or -1
 * if audio is not running. */
static int audio_wake_fd(void) {
    return audio_available ? audio.wakefd : -1;
}

static void audio_cleanup(void);

/* Rebuild after the stream or the PipeWire daemon went away. Called from the
 * epoll loop - NEVER from a stream callback, and never from audio_mix.
 *
 * Voices in flight are lost with the old node. That is correct: they are
 * clicks, and the alternative is carrying a voice pool across a graph whose
 * rate we are about to renegotiate. */
/* The second half of every rebuild: bring the node back up around whatever
 * audio.sample[] and audio.latency now hold. Split out of audio_restart so
 * any other caller reuses it verbatim rather than copying its tail -
 * the "clear the flag after the teardown" rule below is the one thing in
 * this file that causes an INFINITE rebuild loop when got wrong, and it is
 * worth a great deal that it exists in exactly one place.
 *
 * Every caller must have run audio_stream_close() first. */
static void audio_stream_reopen(void) {
    /* Clear the flag AFTER the teardown, never before. pw_stream_destroy
     * emits a final state_changed on its way out whenever the stream was not
     * already UNCONNECTED - which is precisely the ERROR case - and that
     * callback re-arms the flag and pokes the eventfd. Clearing first leaves
     * that self-inflicted signal standing, so the next epoll wake tears down
     * the stream just built, forever. */
    atomic_store(&audio.restart, 0);
    uint64_t drain;
    while (audio.wakefd >= 0 && read(audio.wakefd, &drain, sizeof(drain)) > 0)
        ;

    memset(audio.voice, 0, sizeof(audio.voice));

    /* Drop the backlog. audio_trigger kept filling the ring while the stream
     * was dead - up to all 64 entries - and every one of them is now old
     * enough to clamp to frame 0. Draining them into the new stream would
     * fire dozens of seconds-stale clicks at once, which is a good deal worse
     * than losing them. Safe to touch tail here: the thread loop is stopped
     * and destroyed, so the consumer does not exist. */
    atomic_store(&trig.tail, atomic_load(&trig.head));

    if (audio_stream_open() != 0) {
        /* Terminal. Tear down through the normal path rather than just
         * clearing the flag: audio_cleanup gates on audio_available, so
         * dropping it here would make the shutdown teardown a no-op and
         * strand the samples and the eventfd. */
        LOG_WARN("Audio stream lost and could not be rebuilt; going silent");
        audio_cleanup();
        return;
    }
    LOG_INFO("Audio stream rebuilt");
}

static void audio_restart(void) {
    if (!audio_available) return;
    if (!atomic_load(&audio.restart)) return;

    audio_stream_close();
    audio_stream_reopen();
}

/* Change the requested quantum on the live node. node.latency is a node
 * property PipeWire re-reads on update and re-plans the graph around, so
 * this costs no rebuild - and must not cost one, for the reason
 * audio_swap_samples gives: a node that leaves the graph, even briefly, can
 * hand the session a quantum it cannot get back while a JACK client holds
 * the lock. If that same lock is held NOW, the new request is simply not
 * honoured until it is released, which is the lock doing its job.
 *
 * Silent no-op when the value is unchanged, so audio_swap_samples can call
 * it unconditionally. */
static void audio_relatency(unsigned latency) {
    if (!audio_available) return;
    latency = latency ? latency : AUDIO_LATENCY_DEFAULT;
    if (latency == audio.latency) return;
    audio.latency = latency;   /* also what the next rebuild requests */
    if (!audio.stream) return;

    char lat[32];
    snprintf(lat, sizeof(lat), "%u/%d", audio.latency, MIX_RATE);
    struct spa_dict_item it[] = { SPA_DICT_ITEM_INIT(PW_KEY_NODE_LATENCY, lat) };

    pw_thread_loop_lock(audio.loop);
    pw_stream_update_properties(audio.stream, &SPA_DICT_INIT_ARRAY(it));
    pw_thread_loop_unlock(audio.loop);
}

/* The part of a sample swap that must not overlap a process cycle. Runs
 * under the stream's DATA LOOP lock (pw_loop_locked), which that loop holds
 * for the whole of every dispatch and drops only while it sleeps in poll -
 * so on_process is not running and cannot start until this returns. Kept to
 * pointer moves and a memset: the RT thread is waiting on it.
 *
 * The ring is drained here for a different reason than a rebuild drains it.
 * Pending entries are sample ids from the OLD numbering, and after this
 * returns they would index the new set and play the wrong sound. Writing
 * tail is the consumer's job, and with the consumer excluded this is it. */
typedef struct {
    audio_sample_t *newset;
    int             n_new;
    float          *old[AUDIO_NSAMPLES];
} swap_args_t;

static int swap_locked(struct spa_loop *loop, bool async, uint32_t seq,
                       const void *data, size_t size, void *user_data) {
    (void)loop; (void)async; (void)seq; (void)data; (void)size;
    swap_args_t *a = user_data;

    for (int i = 0; i < AUDIO_NSAMPLES; i++) {
        a->old[i] = audio.sample[i].samples;
        if (i < a->n_new) {
            audio.sample[i]      = a->newset[i];
            a->newset[i].samples = NULL;
        } else {
            audio.sample[i] = (audio_sample_t){ 0 };
        }
    }
    memset(audio.voice, 0, sizeof(audio.voice));
    atomic_store(&trig.tail, atomic_load(&trig.head));
    return 0;
}

/* Replace the whole loaded sample set ON THE LIVE NODE. mix_voice_t.samples
 * borrows audio.sample[i] outright and on_process runs with
 * PW_STREAM_FLAG_RT_PROCESS - on PipeWire's DATA thread, which
 * pw_thread_loop_lock does not cover - so a buffer may only be freed once no
 * process cycle can be reading it. swap_locked gives that guarantee by
 * running under the data loop's own lock; after it returns no voice points
 * at an old buffer, and the frees below race nothing.
 *
 * THE NODE IS NOT TORN DOWN, and that is the point. This used to destroy
 * and rebuild the stream, and the moment the graph spends without our node
 * is enough to lose the session's quantum: PipeWire re-picks it without our
 * node.latency (clock.quantum, typically 1024), and a JACK client - osu!,
 * a DAW, anything through pipewire-jack, which sets node.lock-quantum by
 * default - then pins the graph there, so the rebuilt node's request is
 * refused until that client exits. Seen live with osu! as a JACK client:
 * a sample edit mid-game left the graph at 1024 until the game closed. A
 * node that never leaves cannot cause that.
 *
 * Voices in flight are cut, which is what the rebuild did too.
 *
 * Takes ownership of the buffers in `newset` and nulls what it took, so a
 * caller can free the remainder unconditionally. */
static int audio_swap_samples(audio_sample_t *newset, int n_new,
                              unsigned latency) {
    if (!audio_available) return -1;

    swap_args_t a = { .newset = newset,
                      .n_new  = n_new < AUDIO_NSAMPLES ? n_new
                                                       : AUDIO_NSAMPLES };

    /* The stream gets its data loop at connect, which audio_stream_open
     * always does, so this is not expected to be NULL. If it ever is, there
     * is no lock to take, and the teardown is the only other thing that
     * excludes on_process - so fall back to it, quantum hazard and all,
     * rather than free under a running callback. */
    struct pw_loop *dl = audio.stream ? pw_stream_get_data_loop(audio.stream)
                                      : NULL;
    if (dl) {
        pw_loop_locked(dl, swap_locked, 0, NULL, 0, &a);
    } else {
        audio_stream_close();
        swap_locked(NULL, false, 0, NULL, 0, &a);
        audio_stream_reopen();
    }
    for (int i = 0; i < AUDIO_NSAMPLES; i++)
        free(a.old[i]);

    audio_relatency(latency);
    return audio_available ? 0 : -1;
}

static void audio_cleanup(void) {
    if (!audio_available) return;
    audio_stream_close();
    if (audio.wakefd >= 0) { close(audio.wakefd); audio.wakefd = -1; }
    unsigned dropped = atomic_exchange(&trig.overruns, 0);
    if (dropped)
        LOG_WARN("%u hitsound trigger(s) dropped on a full ring", dropped);
    for (int i = 0; i < AUDIO_NSAMPLES; i++) {
        free(audio.sample[i].samples);
        audio.sample[i].samples = NULL;
    }
    audio_available = 0;
}
/* ------------------------------------------------------------------------- */
/* Config                                                                    */
/* ------------------------------------------------------------------------- */

/* One key -> one sample. `sample` and `gain` are NULL / negative when the
 * binding did not override them, and are filled in from audio.sample /
 * audio.gain once the whole file has been read.
 *
 * `start_ms`/`end_ms` select a window of the sample file; both zero means
 * the whole file, which is what an omitted `start:`/`end:` leaves behind and
 * what every hand-written binding wants. They exist for imported soundpacks,
 * where one recording holds every key's sound - see sample_ref_t. */
typedef struct {
    int    key;
    char  *sample;
    float  gain;
    double start_ms;
    double end_ms;
} binding_t;

typedef struct {
    char   **device_paths;
    size_t   n_devices;
    int      auto_discover; /* no 'devices' list: scan for matching keyboards */
    /* The bound keys, in config order. There is no fixed ceiling here - the
     * only real one is AUDIO_NSAMPLES distinct SAMPLES, since bindings that
     * share a file and a gain share a sample. */
    binding_t *bind;
    size_t     n_bind;
    int      audio_enabled;
    char    *wav_path;      /* default sample for bindings that omit one */
    float    gain;          /* default gain, likewise (1.0) */
    unsigned audio_latency; /* requested node quantum, frames (0 -> default) */
} evclack_config_t;

static void config_init(evclack_config_t *c) {
    memset(c, 0, sizeof(*c));
    c->audio_enabled = 1;
    c->gain          = 1.0f;
    c->audio_latency = AUDIO_LATENCY_DEFAULT;
}

static void config_free(evclack_config_t *c) {
    if (!c) return;
    for (size_t i = 0; i < c->n_devices; i++)
        free(c->device_paths[i]);
    free(c->device_paths);
    for (size_t i = 0; i < c->n_bind; i++)
        free(c->bind[i].sample);
    free(c->bind);
    free(c->wav_path);
    memset(c, 0, sizeof(*c));
}

/* Name a key code for a message: the symbolic name when libevdev knows one,
 * otherwise the bare number. Rotates through a few static buffers so more
 * than one can appear in a single call. */
static const char *key_name_or(int code) {
    static char buf[4][32];
    static int  turn;
    const char *nm = libevdev_event_code_get_name(EV_KEY, (unsigned)code);
    if (nm) return nm;
    turn = (turn + 1) % 4;
    snprintf(buf[turn], sizeof(buf[turn]), "%d", code);
    return buf[turn];
}

/* libyaml document-API helpers --------------------------------------------- */

static yaml_node_t* ynode(yaml_document_t *doc, int id) {
    return id ? yaml_document_get_node(doc, id) : NULL;
}

/* Look up a scalar key in a mapping node. Returns the value node or NULL. */
static yaml_node_t* map_get(yaml_document_t *doc, yaml_node_t *map, const char *key) {
    if (!map || map->type != YAML_MAPPING_NODE) return NULL;
    for (yaml_node_pair_t *p = map->data.mapping.pairs.start;
         p < map->data.mapping.pairs.top; p++) {
        yaml_node_t *k = ynode(doc, p->key);
        if (!k || k->type != YAML_SCALAR_NODE) continue;
        if (strcmp((const char *)k->data.scalar.value, key) == 0)
            return ynode(doc, p->value);
    }
    return NULL;
}

static int scalar_dup(yaml_node_t *n, char **out) {
    if (!n || n->type != YAML_SCALAR_NODE) return -1;
    char *s = strdup((const char *)n->data.scalar.value);
    if (!s) return -1;
    free(*out);
    *out = s;
    return 0;
}

static int parse_bool(yaml_node_t *n, int *out) {
    if (!n || n->type != YAML_SCALAR_NODE) return -1;
    const char *s = (const char *)n->data.scalar.value;
    if (!strcasecmp(s, "true")  || !strcasecmp(s, "yes") ||
        !strcasecmp(s, "on")    || !strcmp(s, "1"))    { *out = 1; return 0; }
    if (!strcasecmp(s, "false") || !strcasecmp(s, "no") ||
        !strcasecmp(s, "off")   || !strcmp(s, "0"))    { *out = 0; return 0; }
    return -1;
}

/* Parse a non-negative float (a linear gain). Rejects garbage and negatives. */
static int parse_gain(yaml_node_t *n, float *out) {
    if (!n || n->type != YAML_SCALAR_NODE) return -1;
    const char *s = (const char *)n->data.scalar.value;
    char *end = NULL;
    errno = 0;
    double d = strtod(s, &end);
    if (end == s || *end != '\0' || errno != 0 || d < 0.0)
        return -1;
    *out = (float)d;
    return 0;
}

/* Parse a non-negative time in milliseconds. Kept separate from parse_gain
 * because it stays a double: a soundpack's slice bounds carry a half
 * millisecond (45832.5), and narrowing them to float would move a window by
 * a frame or two AND break the exact compare that lets a reload recognise an
 * unchanged sample set. */
static int parse_ms(yaml_node_t *n, double *out) {
    if (!n || n->type != YAML_SCALAR_NODE) return -1;
    const char *s = (const char *)n->data.scalar.value;
    char *end = NULL;
    errno = 0;
    double d = strtod(s, &end);
    if (end == s || *end != '\0' || errno != 0 || d < 0.0 || !isfinite(d))
        return -1;
    *out = d;
    return 0;
}

/* Parse a frame count, bounded. The range is not decoration: PipeWire's own
 * quantum-floor/limit sit at 4 and 8192, and a node.latency outside what the
 * graph will run is silently ignored rather than clamped - which reads as
 * "my config did nothing". */
static int parse_frames(yaml_node_t *n, long *out) {
    if (!n || n->type != YAML_SCALAR_NODE) return -1;
    const char *s = (const char *)n->data.scalar.value;
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 0);
    if (end == s || *end != '\0' || errno != 0)
        return -1;
    *out = v;
    return 0;
}


/* Resolve a key code from a scalar - either a symbolic name ("KEY_Z") or
 * a decimal/hex integer. Returns 0 on success. */
static int parse_key_code(yaml_node_t *n, int *out) {
    if (!n || n->type != YAML_SCALAR_NODE) return -1;
    const char *s = (const char *)n->data.scalar.value;

    /* Try numeric first. */
    if (s[0] != '\0') {
        char *end = NULL;
        long v = strtol(s, &end, 0);
        if (end && *end == '\0' && end != s &&
            v >= 0 && v < KEY_MAX) {
            *out = (int)v;
            return 0;
        }
    }

    int code = libevdev_event_code_from_name(EV_KEY, s);
    if (code < 0) return -1;
    *out = code;
    return 0;
}

static int load_config(const char *path, evclack_config_t *c) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        LOG_ERR("Cannot open config %s: %s", path, strerror(errno));
        return -1;
    }

    yaml_parser_t parser;
    yaml_document_t doc;
    memset(&doc, 0, sizeof(doc));
    int rc = -1;
    int doc_loaded = 0;

    if (!yaml_parser_initialize(&parser)) {
        LOG_ERR("yaml_parser_initialize failed");
        fclose(fp);
        return -1;
    }
    yaml_parser_set_input_file(&parser, fp);

    if (!yaml_parser_load(&parser, &doc)) {
        LOG_ERR("YAML parse error in %s: %s (line %zu, col %zu)",
                path, parser.problem ? parser.problem : "unknown",
                (size_t)parser.problem_mark.line + 1,
                (size_t)parser.problem_mark.column + 1);
        goto out;
    }
    doc_loaded = 1;

    yaml_node_t *root = yaml_document_get_root_node(&doc);
    if (!root) {
        LOG_ERR("config %s is empty", path);
        goto out;
    }
    if (root->type != YAML_MAPPING_NODE) {
        LOG_ERR("config root must be a mapping");
        goto out;
    }

    /* devices: optional sequence of scalar paths. Omitted (or the scalar
     * "auto") means auto-discovery: open every keyboard-shaped device that
     * advertises every bound key (see auto_open_ok). ------------------ */
    yaml_node_t *devs = map_get(&doc, root, "devices");
    if (!devs ||
        (devs->type == YAML_SCALAR_NODE &&
         !strcasecmp((const char *)devs->data.scalar.value, "auto"))) {
        c->auto_discover = 1;
    } else if (devs->type != YAML_SEQUENCE_NODE) {
        LOG_ERR("'devices' must be a sequence of paths, or \"auto\"");
        goto out;
    } else {
        size_t n = (size_t)(devs->data.sequence.items.top -
                            devs->data.sequence.items.start);
        if (n == 0) {
            LOG_ERR("'devices' list is empty");
            goto out;
        }
        c->device_paths = calloc(n, sizeof(char *));
        if (!c->device_paths) { LOG_ERR("oom"); goto out; }
        for (yaml_node_item_t *it = devs->data.sequence.items.start;
             it < devs->data.sequence.items.top; it++) {
            yaml_node_t *item = ynode(&doc, *it);
            if (scalar_dup(item, &c->device_paths[c->n_devices]) != 0) {
                LOG_ERR("'devices' entry must be a scalar string");
                goto out;
            }
            c->n_devices++;
        }
    }

    /* keys: the bindings. A sequence, each entry either a bare key code
     * (symbolic "KEY_Z" or numeric) or a mapping with an optional per-key
     * `sample` and `gain`. Whatever a binding omits is filled in from
     * audio.sample / audio.gain once the whole document has been read, so
     * the two blocks can appear in either order. ---------------------- */
    yaml_node_t *keys = map_get(&doc, root, "keys");
    if (!keys) {
        LOG_ERR("'keys' is required: evclack only sounds keys you bind, so "
                "a config without it would open every keyboard on the "
                "system and then run in total silence");
        goto out;
    }
    if (keys->type != YAML_SEQUENCE_NODE) {
        LOG_ERR("'keys' must be a sequence of key codes or {key, sample, "
                "gain, start, end} mappings");
        goto out;
    }
    {
        size_t n = (size_t)(keys->data.sequence.items.top -
                            keys->data.sequence.items.start);
        if (n == 0) {
            LOG_ERR("'keys' list is empty; bind at least one key");
            goto out;
        }
        c->bind = calloc(n, sizeof(*c->bind));
        if (!c->bind) { LOG_ERR("oom"); goto out; }
        for (yaml_node_item_t *it = keys->data.sequence.items.start;
             it < keys->data.sequence.items.top; it++) {
            yaml_node_t *item = ynode(&doc, *it);
            binding_t *b = &c->bind[c->n_bind];
            b->gain = -1.0f;      /* sentinel: unset -> falls back to gain */

            if (item && item->type == YAML_SCALAR_NODE) {
                if (parse_key_code(item, &b->key) != 0) {
                    LOG_ERR("invalid key code in 'keys' (entry %zu)",
                            c->n_bind + 1);
                    goto out;
                }
            } else if (item && item->type == YAML_MAPPING_NODE) {
                yaml_node_t *kn = map_get(&doc, item, "key");
                if (!kn || parse_key_code(kn, &b->key) != 0) {
                    LOG_ERR("'keys' entry %zu needs a valid 'key'",
                            c->n_bind + 1);
                    goto out;
                }
                yaml_node_t *sn = map_get(&doc, item, "sample");
                if (sn && scalar_dup(sn, &b->sample) != 0) {
                    LOG_ERR("'keys' entry %zu: 'sample' must be a scalar "
                            "string", c->n_bind + 1);
                    goto out;
                }
                yaml_node_t *gn = map_get(&doc, item, "gain");
                if (gn && parse_gain(gn, &b->gain) != 0) {
                    LOG_ERR("'keys' entry %zu: 'gain' must be a non-negative "
                            "number", c->n_bind + 1);
                    goto out;
                }
                /* A window into the sample, in milliseconds. Validated
                 * against each other here; whether they fall inside the
                 * FILE is not knowable without opening it, so that check
                 * belongs to the loader (slice_window) and costs the one
                 * sample rather than the whole config. */
                yaml_node_t *sm = map_get(&doc, item, "start");
                yaml_node_t *em = map_get(&doc, item, "end");
                if (sm && parse_ms(sm, &b->start_ms) != 0) {
                    LOG_ERR("'keys' entry %zu: 'start' must be a "
                            "non-negative number of milliseconds",
                            c->n_bind + 1);
                    goto out;
                }
                if (em && parse_ms(em, &b->end_ms) != 0) {
                    LOG_ERR("'keys' entry %zu: 'end' must be a non-negative "
                            "number of milliseconds", c->n_bind + 1);
                    goto out;
                }
                if (b->end_ms > 0.0 && b->end_ms <= b->start_ms) {
                    LOG_ERR("'keys' entry %zu: 'end' (%.1f) must be after "
                            "'start' (%.1f)", c->n_bind + 1,
                            b->end_ms, b->start_ms);
                    goto out;
                }
            } else {
                LOG_ERR("'keys' entry %zu must be a key code or a mapping",
                        c->n_bind + 1);
                goto out;
            }

            /* One key, one sample. A repeat is a typo, and silently keeping
             * either the first or the last would be a config that does not
             * do what it reads like. */
            for (size_t j = 0; j < c->n_bind; j++) {
                if (c->bind[j].key == b->key) {
                    LOG_ERR("'keys' binds %s twice", key_name_or(b->key));
                    goto out;
                }
            }
            c->n_bind++;
        }
    }

    /* audio: optional mapping */
    yaml_node_t *aud = map_get(&doc, root, "audio");
    if (aud) {
        if (aud->type != YAML_MAPPING_NODE) {
            LOG_ERR("'audio' must be a mapping");
            goto out;
        }
        yaml_node_t *en = map_get(&doc, aud, "enabled");
        if (en && parse_bool(en, &c->audio_enabled) != 0) {
            LOG_ERR("'audio.enabled' must be a boolean");
            goto out;
        }
        /* 'wav' is accepted as an alias so a config written against the
         * daemon this grew out of still loads. */
        yaml_node_t *wav = map_get(&doc, aud, "sample");
        if (!wav) wav = map_get(&doc, aud, "wav");
        if (wav && scalar_dup(wav, &c->wav_path) != 0) {
            LOG_ERR("'audio.sample' must be a scalar string");
            goto out;
        }
        yaml_node_t *g = map_get(&doc, aud, "gain");
        if (g && parse_gain(g, &c->gain) != 0) {
            LOG_ERR("'audio.gain' must be a non-negative number");
            goto out;
        }

        yaml_node_t *lat = map_get(&doc, aud, "latency");
        if (lat) {
            long v = 0;
            if (parse_frames(lat, &v) != 0 || v < 16 || v > 8192) {
                LOG_ERR("'audio.latency' must be a frame count in 16..8192");
                goto out;
            }
            c->audio_latency = (unsigned)v;
        }
    }
    if (!c->wav_path) {
        c->wav_path = strdup(DEF_WAV);
        if (!c->wav_path) { LOG_ERR("oom"); goto out; }
    }

    /* Fill in whatever each binding left unset. Doing it here rather than at
     * parse time is what lets `keys` precede `audio` in the file. */
    for (size_t i = 0; i < c->n_bind; i++) {
        if (c->bind[i].gain < 0.0f)
            c->bind[i].gain = c->gain;
        if (!c->bind[i].sample) {
            c->bind[i].sample = strdup(c->wav_path);
            if (!c->bind[i].sample) { LOG_ERR("oom"); goto out; }
        }
    }

    rc = 0;

out:
    if (doc_loaded) yaml_document_delete(&doc);
    yaml_parser_delete(&parser);
    fclose(fp);
    if (rc != 0) config_free(c);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Key bindings                                                              */
/* ------------------------------------------------------------------------- */

/* sample_ref_t itself is up in the audio region, next to the audio_sample_t
 * it becomes; what belongs here is why bindings_plan interns on the WHOLE of
 * it. Two bindings that agree on file, gain AND slice share a slot, so ten
 * keys pointing at one hitsound decode and resample it once. Interning on
 * the path alone would keep audio_sample_t from owning its buffer outright -
 * split that ownership and audio_cleanup's frees, and every test that pokes
 * at audio.sample[], would have to learn about refcounts to buy a case
 * nobody hits. It would also be wrong now rather than merely wasteful: every
 * key of a Mechvibes pack names the SAME path and differs only in the slice.
 */

/* code -> sample id, or -1. KEY_CNT entries of int16_t is 1.5KB, which buys
 * an O(1) lookup in the drain path and removes the question of how many keys
 * may be bound: the only ceiling left is AUDIO_NSAMPLES distinct sounds.
 *
 * Filled ONLY by bindings_plan. A static int16_t array is .bss zeros, and 0
 * is a valid sample id - a table that reached the drain path unfilled would
 * make every key on the board clack with sample 0, silently, which is the
 * exact inverse of what the config says. */
static int16_t g_key_sample[KEY_CNT];

/* The sample set g_key_sample's ids refer to, indexed by LOADED SAMPLE ID -
 * the same index as audio.sample[]. Kept so a reload can ask "is this the
 * same set of sounds?" without re-decoding anything.
 *
 * The paths are BORROWED from the live config's bind[].sample, so the config
 * these were planned from must outlive them: a reload compares before it
 * frees, and re-points these into the new config as part of the commit. */
static sample_ref_t g_refs[AUDIO_NSAMPLES];
static int          g_n_refs;

/* Resolve the bindings into the key table and the distinct samples they
 * need. No I/O: loading is the caller's job, which is what lets this be
 * tested without a file on disk. Returns 0, or -1 with a message. */
static int bindings_plan(const evclack_config_t *cfg, int16_t *key_sample,
                         sample_ref_t *refs, int *n_refs) {
    for (int i = 0; i < KEY_CNT; i++)
        key_sample[i] = -1;
    *n_refs = 0;

    for (size_t i = 0; i < cfg->n_bind; i++) {
        const binding_t *b = &cfg->bind[i];
        if (b->key < 0 || b->key >= KEY_CNT) {
            LOG_ERR("key code %d is out of range", b->key);
            return -1;
        }
        if (!b->sample) {
            LOG_ERR("%s has no sample", key_name_or(b->key));
            return -1;
        }

        int id = -1;
        for (int j = 0; j < *n_refs; j++)
            if (strcmp(refs[j].path, b->sample) == 0 &&
                refs[j].gain     == b->gain     &&
                refs[j].start_ms == b->start_ms &&
                refs[j].end_ms   == b->end_ms) { id = j; break; }
        if (id < 0) {
            if (*n_refs >= AUDIO_NSAMPLES) {
                LOG_ERR("more than %d distinct samples; bindings that share "
                        "a file, a gain AND a slice share a sample, so give "
                        "some of them the same gain or use fewer sounds",
                        AUDIO_NSAMPLES);
                return -1;
            }
            id = (*n_refs)++;
            refs[id].path     = b->sample;
            refs[id].gain     = b->gain;
            refs[id].start_ms = b->start_ms;
            refs[id].end_ms   = b->end_ms;
        }
        key_sample[b->key] = (int16_t)id;
    }
    return 0;
}

/* Do two planned sets name the same distinct sounds, ignoring the order the
 * config happened to list them in? On success `map` is filled so that
 * map[new_id] == old_id, which is what lets a reload re-point the key table
 * at the buffers ALREADY LOADED instead of decoding them again.
 *
 * Order-independence is the whole point: moving one binding up the file must
 * not cost a stream rebuild. bindings_plan interns, so ids within a set are
 * dense and distinct and a match therefore makes `map` a permutation - which
 * the commit relies on. `used` enforces that rather than assuming it. */
static int refs_same_set(const sample_ref_t *old, int n_old,
                         const sample_ref_t *nw, int n_new, int *map) {
    if (n_old != n_new) return 0;
    if (n_old > AUDIO_NSAMPLES) return 0;

    int used[AUDIO_NSAMPLES] = { 0 };
    for (int i = 0; i < n_new; i++) {
        int found = -1;
        for (int j = 0; j < n_old; j++) {
            if (used[j]) continue;
            if (old[j].path && nw[i].path &&
                strcmp(old[j].path, nw[i].path) == 0 &&
                old[j].gain     == nw[i].gain     &&
                old[j].start_ms == nw[i].start_ms &&
                old[j].end_ms   == nw[i].end_ms) { found = j; break; }
        }
        if (found < 0) return 0;
        used[found] = 1;
        map[i] = found;
    }
    return 1;
}

/* What a reload has to do to the audio subsystem, cheapest first. */
enum {
    RELOAD_NONE = 0,   /* key table only - no seam, nothing rebuilt */
    RELOAD_LATENCY,    /* re-request the quantum, keep every loaded sample */
    RELOAD_SAMPLES,    /* re-decode and swap the sample set */
    RELOAD_AUDIO_OFF,  /* audio.enabled went false */
    RELOAD_AUDIO_ON,   /* audio.enabled went true, or startup had failed */
};

/* A validated, not-yet-committed configuration. */
typedef struct {
    evclack_config_t cfg;
    int16_t          table[KEY_CNT];
    sample_ref_t     refs[AUDIO_NSAMPLES];
    int              n_refs;
    int              map[AUDIO_NSAMPLES];   /* map[new_id] = loaded id */
    unsigned         action;
} reload_plan_t;

/* Parse and plan a config WITHOUT touching a single live byte: no device is
 * opened or closed, no sample is decoded, g_key_sample and g_refs are not
 * written. Everything that can be rejected is rejected here, so the caller's
 * commit cannot fail halfway and leave a daemon running half of one config
 * and half of another.
 *
 * That is the same reason bindings_plan does no I/O, and it is what makes
 * "a bad config changes nothing" a testable claim rather than a hope -
 * maptest calls this directly. Returns 0 with `out` owning a config the
 * caller must either commit or config_free. */
static int config_validate(const char *path, reload_plan_t *out) {
    memset(out, 0, sizeof(*out));
    config_init(&out->cfg);

    if (load_config(path, &out->cfg) != 0)
        return -1;
    if (bindings_plan(&out->cfg, out->table, out->refs, &out->n_refs) != 0) {
        config_free(&out->cfg);
        return -1;
    }

    /* Identity unless the fast path below earns a real permutation. */
    for (int i = 0; i < AUDIO_NSAMPLES; i++) out->map[i] = i;

    if (!out->cfg.audio_enabled) {
        out->action = audio_available ? RELOAD_AUDIO_OFF : RELOAD_NONE;
        return 0;
    }
    /* Audio wanted but not running - either disabled before, or PipeWire was
     * down when the daemon started. Reload is the retry. */
    if (!audio_available) {
        out->action = RELOAD_AUDIO_ON;
        return 0;
    }

    /* A committed ref whose file never decoded counts as CHANGED, so putting
     * the missing sample in place and saving picks it up. Without this, an
     * otherwise-identical config takes the fast path and the key stays
     * silent forever. */
    int all_loaded = 1;
    for (int i = 0; i < g_n_refs; i++)
        if (!audio.sample[i].samples) { all_loaded = 0; break; }

    if (!all_loaded ||
        !refs_same_set(g_refs, g_n_refs, out->refs, out->n_refs, out->map)) {
        /* refs_same_set may have written part of map before giving up. */
        for (int i = 0; i < AUDIO_NSAMPLES; i++) out->map[i] = i;
        out->action = RELOAD_SAMPLES;
        return 0;
    }

    out->action = (out->cfg.audio_latency != audio.latency) ? RELOAD_LATENCY
                                                            : RELOAD_NONE;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Input devices                                                             */
/* ------------------------------------------------------------------------- */

/* epoll user-data tag. The inotify fd is still identified by a NULL ptr;
 * everything else leads with one of these so run_loop can tell an evdev
 * device from the audio wake eventfd. `kind` must stay the FIRST member of
 * input_dev_t: the dispatch reads the leading int off whatever data.ptr
 * points at. */
enum { EP_INPUT = 1, EP_AUDIO = 2, EP_CONFIG = 3, EP_SIGNAL = 4 };

/* The audio wake eventfd has no struct of its own; the epoll set is keyed
 * on a leading discriminator, so it gets a standing one. So do the config
 * watch and the signal eventfd. */
static int g_ep_audio  = EP_AUDIO;
static int g_ep_config = EP_CONFIG;
static int g_ep_signal = EP_SIGNAL;

typedef struct {
    int               kind;    /* EP_INPUT */
    struct libevdev  *dev;
    int               fd;
    char             *path;
    dev_t             rdev; /* st_rdev, dedupes nodes reached via symlinks */
} input_dev_t;

/* Open devices as stable heap pointers: epoll user data points at the
 * entries, so the list may grow and shrink but entries never move. */
typedef struct {
    input_dev_t **v;
    size_t        n, cap;
} dev_list_t;

static int dev_list_add(dev_list_t *l, input_dev_t *in) {
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 8;
        input_dev_t **v = realloc(l->v, cap * sizeof(*v));
        if (!v) return -1;
        l->v   = v;
        l->cap = cap;
    }
    l->v[l->n++] = in;
    return 0;
}

static void dev_list_remove(dev_list_t *l, input_dev_t *in) {
    for (size_t i = 0; i < l->n; i++) {
        if (l->v[i] == in) {
            l->v[i] = l->v[--l->n];
            return;
        }
    }
}

static int dev_list_has_rdev(const dev_list_t *l, dev_t rdev) {
    for (size_t i = 0; i < l->n; i++)
        if (l->v[i]->rdev == rdev) return 1;
    return 0;
}

/* Auto-discovery filter: keyboard-shaped devices carrying every bound key.
 *
 * Devices with pointer/absolute axes are rejected - a gaming mouse whose HID
 * descriptor also claims keyboard codes is not what anyone means by "my
 * keyboard", and opening it would sound its side buttons.
 *
 * VIRTUAL devices are deliberately NOT rejected, unlike in the remapper this
 * grew out of. Remappers (keyd, doubletap, ...) grab their source board
 * exclusively, so the real keystrokes exist ONLY on the uinput node they
 * emit; filtering it out would leave evclack silent on exactly the setups
 * most likely to want it. There is no double-fire to avoid either - a
 * grabbed node yields nothing to a passive reader - and evclack emits no
 * events of its own, so it cannot hear itself. */
/* The first bound key this device cannot report, or -1 if it carries them
 * all. Split out of auto_open_ok so a reload can SAY which binding
 * disqualified a board it is dropping. */
static int auto_missing_key(struct libevdev *dev, const evclack_config_t *cfg) {
    for (size_t i = 0; i < cfg->n_bind; i++)
        if (!libevdev_has_event_code(dev, EV_KEY, (unsigned)cfg->bind[i].key))
            return cfg->bind[i].key;
    return -1;
}

static int auto_open_ok(struct libevdev *dev, const evclack_config_t *cfg) {
    /* Every bound key, not just some. A board carrying a subset would be
     * opened for keys it cannot report. */
    if (auto_missing_key(dev, cfg) >= 0)
        return 0;
    if (libevdev_has_event_type(dev, EV_REL) ||
        libevdev_has_event_type(dev, EV_ABS))
        return 0;
    return 1;
}

/* Open and vet one event node. Returns NULL (silently, for the expected
 * cases) when the device isn't one we should be listening to. */
static input_dev_t *input_try_open(const char *path, const evclack_config_t *cfg,
                                   int auto_mode, int quiet) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        /* EACCES right after a hotplug is expected - udev hasn't applied
         * the input-group permissions yet; the IN_ATTRIB watch retriggers
         * reconcile_devices once it does. */
        if (!quiet)
            LOG_ERR("open(%s): %s", path, strerror(errno));
        else if (errno != EACCES && errno != ENOENT)
            LOG_WARN("open(%s): %s", path, strerror(errno));
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return NULL;
    }

    struct libevdev *dev = NULL;
    int rc = libevdev_new_from_fd(fd, &dev);
    if (rc < 0) {
        if (!quiet)
            LOG_ERR("libevdev_new_from_fd(%s): %s", path, strerror(-rc));
        close(fd);
        return NULL;
    }

    if (auto_mode && !auto_open_ok(dev, cfg)) {
        /* Say WHY, when the thing rejected was obviously a keyboard.
         *
         * Discovery rejecting a board is normally uninteresting - most
         * event nodes are power buttons and video buses - so this path is
         * silent by design. But the one interesting case is a real keyboard
         * turned away for missing ONE bound key, and it is the failure mode
         * a soundpack import walks straight into: a pack binds a hundred
         * keys whether or not the board has them, and the symptom is a
         * daemon that starts, says "Running." and never makes a sound.
         * devices_refilter logs this when a RELOAD closes a board that was
         * working; without the same line here, a fresh start on a too-wide
         * config explains nothing at all.
         *
         * Gated on `quiet`, which is 0 only for the startup pass, so a
         * flurry of hotplug rescans cannot turn it into noise. */
        if (!quiet && !libevdev_has_event_type(dev, EV_REL) &&
            !libevdev_has_event_type(dev, EV_ABS) &&
            libevdev_has_event_code(dev, EV_KEY, KEY_A) &&
            libevdev_has_event_code(dev, EV_KEY, KEY_SPACE)) {
            int miss = auto_missing_key(dev, cfg);
            if (miss >= 0)
                LOG_WARN("Not listening to %s (\"%s\"): it cannot report %s, "
                         "and auto-discovery needs every bound key. Unbind "
                         "that key to use this keyboard.",
                         path, libevdev_get_name(dev), key_name_or(miss));
        }
        libevdev_free(dev);
        close(fd);
        return NULL;
    }

    /* Put event timestamps on the same clock the audio graph runs on.
     * evdev stamps CLOCK_REALTIME by default and PipeWire is monotonic, so
     * without this the sub-buffer placement in mix_drain_triggers would be
     * comparing two unrelated epochs.
     *
     * Through libevdev rather than a raw EVIOCSCLOCKID ioctl: libevdev keeps
     * its own clock_id to stamp the events it synthesises during SYN_DROPPED
     * resync, and an ioctl issued behind its back switches the kernel while
     * leaving those on REALTIME. Not fatal if it fails - the timestamps just
     * fall out of range and every trigger clamps to frame 0, which is where
     * they all landed before any of this. */
    if (libevdev_set_clock_id(dev, CLOCK_MONOTONIC) != 0 && !quiet)
        LOG_WARN("%s: cannot use CLOCK_MONOTONIC; hitsound placement will "
                 "fall back to the start of the buffer", path);

    /* Nothing is grabbed. The device stays exactly as available to the rest
     * of the system as it was, which is the whole design: evclack listens,
     * and a key it sounds still reaches the focused window. It also means
     * there is no held-key hazard to defer around, and no way to strand a
     * key by dying at the wrong moment. */
    input_dev_t *in = calloc(1, sizeof(*in));
    if (!in) {
        LOG_ERR("oom");
        libevdev_free(dev);
        close(fd);
        return NULL;
    }
    in->kind = EP_INPUT;
    in->fd   = fd;
    in->dev  = dev;
    in->path = strdup(path);
    in->rdev = st.st_rdev;
    LOG_INFO("Listening to %s (\"%s\")",
             path, libevdev_get_name(dev) ? libevdev_get_name(dev) : "?");
    return in;
}

static void input_close(input_dev_t *in) {
    if (!in) return;
    if (in->dev) {
        libevdev_free(in->dev);
        in->dev = NULL;
    }
    if (in->fd >= 0) {
        close(in->fd);
        in->fd = -1;
    }
    free(in->path);
    in->path = NULL;
}

static int is_event_node(const struct dirent *d) {
    return strncmp(d->d_name, "event", 5) == 0;
}

static void try_open(dev_list_t *devs, const char *path,
                     const evclack_config_t *cfg, int auto_mode, int loud,
                     int epfd) {
    struct stat st;
    if (stat(path, &st) != 0) {
        if (loud && !auto_mode)
            LOG_WARN("stat(%s): %s", path, strerror(errno));
        return;
    }
    if (!S_ISCHR(st.st_mode))
        return;
    if (dev_list_has_rdev(devs, st.st_rdev))
        return; /* already open (possibly via another path/symlink) */

    input_dev_t *in = input_try_open(path, cfg, auto_mode, !loud);
    if (!in)
        return;

    struct epoll_event ev = { .events = EPOLLIN, .data = { .ptr = in } };
    if (dev_list_add(devs, in) != 0 ||
        epoll_ctl(epfd, EPOLL_CTL_ADD, in->fd, &ev) < 0) {
        LOG_ERR("failed to register %s: %s", path, strerror(errno));
        dev_list_remove(devs, in);
        input_close(in);
        free(in);
    }
}

/* (Re)open whatever should be open but currently isn't: every configured
 * path in explicit mode, every matching event node under input_dir in auto
 * mode. Runs at startup (loud) and again on every inotify event under
 * input_dir (quiet - the same non-matching nodes get revisited each time). */
static void reconcile_devices(dev_list_t *devs, const evclack_config_t *cfg,
                              const char *input_dir, int epfd, int loud) {
    if (!cfg->auto_discover) {
        for (size_t i = 0; i < cfg->n_devices; i++)
            try_open(devs, cfg->device_paths[i], cfg, 0, loud, epfd);
        return;
    }

    struct dirent **ents = NULL;
    int n = scandir(input_dir, &ents, is_event_node, alphasort);
    if (n < 0) {
        LOG_WARN("scandir(%s): %s", input_dir, strerror(errno));
        return;
    }
    for (int i = 0; i < n; i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", input_dir, ents[i]->d_name);
        try_open(devs, path, cfg, 1, loud, epfd);
        free(ents[i]);
    }
    free(ents);
}

/* Would this already-open device be opened again under `cfg`? Reads the live
 * libevdev handle, so it costs no I/O. */
static int device_keep(const input_dev_t *in, const evclack_config_t *cfg) {
    if (cfg->auto_discover)
        return auto_open_ok(in->dev, cfg);
    for (size_t i = 0; i < cfg->n_devices; i++)
        if (strcmp(in->path, cfg->device_paths[i]) == 0)
            return 1;
    return 0;
}

/* The half reconcile_devices does not do: CLOSE whatever should no longer be
 * open. Only a config reload needs it, and it is not optional there - in auto
 * mode the discovery filter is derived from the bindings, so adding one
 * exotic key narrows which boards qualify and an already-open board would
 * otherwise keep sounding keys the new config never mentions. Running this
 * before the reopen pass also makes an explicit/auto mode switch fall out for
 * free. */
static void devices_refilter(dev_list_t *devs, const evclack_config_t *cfg,
                             int epfd) {
    for (size_t i = 0; i < devs->n; ) {
        input_dev_t *in = devs->v[i];
        if (device_keep(in, cfg)) { i++; continue; }

        /* Say WHY. "Adding a binding silently narrowed auto-discovery" is a
         * documented consequence of the every-bound-key rule, and with no
         * line naming the key it reads as the daemon breaking. */
        if (cfg->auto_discover) {
            int miss = auto_missing_key(in->dev, cfg);
            if (miss >= 0)
                LOG_INFO("Dropping %s: it cannot report %s, and "
                         "auto-discovery needs every bound key",
                         in->path, key_name_or(miss));
            else
                LOG_INFO("Dropping %s: no longer matches auto-discovery",
                         in->path);
        } else {
            LOG_INFO("Dropping %s: no longer listed in 'devices'", in->path);
        }

        epoll_ctl(epfd, EPOLL_CTL_DEL, in->fd, NULL);
        dev_list_remove(devs, in);   /* swaps the tail into slot i */
        input_close(in);
        free(in);
    }
}

/* ------------------------------------------------------------------------- */
/* loop                                                                      */
/* ------------------------------------------------------------------------- */

/* ---- config watch -------------------------------------------------------
 *
 * The config's PARENT DIRECTORY is what gets watched, not the file. Editors
 * and dotfile managers overwhelmingly save by writing a temp file and
 * renaming it over the target, which replaces the inode - a watch on the file
 * itself would follow the old one and never fire again.
 *
 * IN_CLOSE_WRITE | IN_MOVED_TO, and deliberately NOT IN_MODIFY: both of these
 * fire only once the file is COMPLETE (the writer closed its fd, or the
 * rename landed), which is what removes the need for a debounce timer. A
 * half-written file would just be rejected by config_validate anyway, but
 * a spurious parse error per keystroke-in-vim is not a good look.
 *
 * Up to two (dir, name) pairs are watched: the literal path, and its
 * realpath() when that differs, so a ~/.config symlinked into a dotfile repo
 * reloads whether the editor writes through the link or into the repo. */
typedef struct {
    char dir[PATH_MAX];
    char base[NAME_MAX + 1];
} cfg_watch_t;

static cfg_watch_t g_cfg_watch[2];
static int         g_n_cfg_watch;

static void cfg_watch_add(const char *path) {
    if (g_n_cfg_watch >= (int)(sizeof(g_cfg_watch) / sizeof(g_cfg_watch[0])))
        return;

    cfg_watch_t w;
    const char *slash = strrchr(path, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - path);
        if (dlen == 0) dlen = 1;                    /* "/foo" -> "/" */
        if (dlen >= sizeof(w.dir)) return;
        memcpy(w.dir, path, dlen);
        w.dir[dlen] = '\0';
        snprintf(w.base, sizeof(w.base), "%s", slash + 1);
    } else {
        snprintf(w.dir,  sizeof(w.dir),  ".");
        snprintf(w.base, sizeof(w.base), "%s", path);
    }
    if (w.base[0] == '\0') return;

    for (int i = 0; i < g_n_cfg_watch; i++)
        if (strcmp(g_cfg_watch[i].dir,  w.dir)  == 0 &&
            strcmp(g_cfg_watch[i].base, w.base) == 0)
            return;
    g_cfg_watch[g_n_cfg_watch++] = w;
}

/* Returns the inotify fd, or -1 with a warning. Not fatal: SIGHUP still
 * reloads, and the daemon still runs. */
static int config_watch_open(const char *config_path) {
    char real[PATH_MAX];

    cfg_watch_add(config_path);
    if (realpath(config_path, real))
        cfg_watch_add(real);

    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) {
        LOG_WARN("inotify_init1: %s - config auto-reload disabled "
                 "(SIGHUP still works)", strerror(errno));
        return -1;
    }

    int ok = 0;
    for (int i = 0; i < g_n_cfg_watch; i++) {
        if (inotify_add_watch(fd, g_cfg_watch[i].dir,
                              IN_CLOSE_WRITE | IN_MOVED_TO) >= 0)
            ok = 1;
        else
            LOG_WARN("inotify_add_watch(%s): %s", g_cfg_watch[i].dir,
                     strerror(errno));
    }
    if (!ok) {
        LOG_WARN("Config auto-reload disabled (SIGHUP still works)");
        close(fd);
        return -1;
    }
    return fd;
}

/* Drain the watch and report whether anything named the config. The
 * directory carries every other file in it too, so the name filter is what
 * keeps an unrelated save from rebuilding the audio graph. */
static int config_watch_hit(int fd) {
    union {
        char buf[4096];
        struct inotify_event align;
    } u;
    int hit = 0;
    ssize_t n;

    while ((n = read(fd, u.buf, sizeof(u.buf))) > 0) {
        for (char *q = u.buf; q < u.buf + n; ) {
            const struct inotify_event *e = (const struct inotify_event *)q;
            if (e->len) {
                for (int i = 0; i < g_n_cfg_watch; i++)
                    if (strcmp(e->name, g_cfg_watch[i].base) == 0) {
                        hit = 1;
                        break;
                    }
            }
            q += sizeof(*e) + e->len;
        }
    }
    return hit;
}

/* Install a validated plan: the key table, the committed ref set, and the
 * config itself. Cannot fail, touches no device and no sample buffer, and
 * does no I/O - which is what lets maptest run a whole reload SEQUENCE with
 * no daemon around it. Takes ownership of p->cfg. */
static void reload_commit(reload_plan_t *p, evclack_config_t *cfg) {
    /* The plan's ids are in config order; map takes them to where the buffers
     * actually live. Identity on every path that reloaded the samples. */
    for (int k = 0; k < KEY_CNT; k++)
        if (p->table[k] >= 0)
            p->table[k] = (int16_t)p->map[p->table[k]];
    memcpy(g_key_sample, p->table, sizeof(g_key_sample));

    /* g_refs is indexed by LOADED SAMPLE ID, so it is written THROUGH map,
     * not memcpy'd in plan order. Getting this wrong is invisible on this
     * reload and wrong on the NEXT one: that compare would build its map
     * against a mis-indexed set and keys would start playing the wrong
     * sample, with no error anywhere. maptest case M runs two reloads in a
     * row for exactly this reason. */
    memset(g_refs, 0, sizeof(g_refs));
    for (int i = 0; i < p->n_refs; i++)
        g_refs[p->map[i]] = p->refs[i];
    g_n_refs = p->n_refs;

    /* g_refs borrows p->cfg's strings, so p->cfg must BECOME the live config
     * rather than be freed. The pointer the loop holds stays put. */
    config_free(cfg);
    *cfg = p->cfg;
    memset(&p->cfg, 0, sizeof(p->cfg));
}

/* Apply a new config to the running daemon, or keep the old one entirely.
 *
 * Ordering is the whole design. Everything that can be rejected is rejected
 * by config_validate before a live byte moves; everything that can fail
 * slowly (decoding) happens into scratch buffers while the old ones are still
 * playing; and only then does the commit run, which cannot fail. */
static void config_reload(dev_list_t *devs, evclack_config_t *cfg,
                          const char *config_path, const char *input_dir,
                          int epfd) {
    reload_plan_t p;

    LOG_INFO("Reloading %s", config_path);
    if (config_validate(config_path, &p) != 0) {
        LOG_WARN("Reload rejected; keeping the running configuration");
        return;
    }

    /* Decode BEFORE anything is torn down. A sample that fails to open then
     * costs its own keys and nothing else - not the stream, not the reload.
     * This is also the slow part: SRC_SINC_BEST_QUALITY runs here, on the
     * epoll thread, at SCHED_FIFO, so keypresses landing during it are lost.
     * Bounded, user-triggered, and the price of not decoding in RT.
     *
     * An imported soundpack makes that window much larger than a
     * hand-written config's - a hundred slices instead of one or two - which
     * is what audio_load_refs's decode cache exists to bound: the file is
     * decoded once and only the sliced audio is resampled. */
    audio_sample_t newset[AUDIO_NSAMPLES];
    memset(newset, 0, sizeof(newset));
    if (p.action == RELOAD_SAMPLES || p.action == RELOAD_AUDIO_ON)
        audio_load_refs(newset, p.refs, p.n_refs);

    switch (p.action) {
    case RELOAD_AUDIO_OFF:
        /* Explicit DEL before the fd is closed. Closing removes it from the
         * set on its own, but a later eventfd reusing the number would then
         * look registered when it is not. */
        if (audio_wake_fd() >= 0)
            epoll_ctl(epfd, EPOLL_CTL_DEL, audio_wake_fd(), NULL);
        audio_cleanup();
        LOG_INFO("Audio disabled by config");
        break;

    case RELOAD_AUDIO_ON:
        /* audio_available is 0, so audio.sample[] is empty - either it never
         * started or audio_cleanup emptied it. Nothing to free here. */
        for (int i = 0; i < p.n_refs && i < AUDIO_NSAMPLES; i++) {
            audio.sample[i]   = newset[i];
            newset[i].samples = NULL;
        }
        if (audio_start(p.cfg.audio_latency) == 0) {
            int afd = audio_wake_fd();
            struct epoll_event ev = { .events = EPOLLIN,
                                      .data = { .ptr = &g_ep_audio } };
            if (afd >= 0 && epoll_ctl(epfd, EPOLL_CTL_ADD, afd, &ev) < 0)
                LOG_WARN("epoll_ctl ADD audio: %s - a lost stream will stay "
                         "lost", strerror(errno));
            LOG_INFO("Audio started");
        }
        break;

    case RELOAD_SAMPLES:
        audio_swap_samples(newset, p.n_refs, p.cfg.audio_latency);
        break;

    case RELOAD_LATENCY:
        audio_relatency(p.cfg.audio_latency);
        break;

    default:
        break;   /* RELOAD_NONE: the key table alone */
    }

    /* Whatever nobody took ownership of - a failed load, or a whole set the
     * audio path rejected. Every consumer nulls what it kept. */
    for (int i = 0; i < AUDIO_NSAMPLES; i++)
        free(newset[i].samples);

    reload_commit(&p, cfg);

    devices_refilter(devs, cfg, epfd);
    reconcile_devices(devs, cfg, input_dir, epfd, 1);

    LOG_INFO("Reloaded: %zu key(s) on %d sample(s), audio=%s, %zu device(s) "
             "open", cfg->n_bind, p.n_refs,
             cfg->audio_enabled ? (audio_available ? "on" : "failed") : "off",
             devs->n);
}

/* drain everything libevdev has buffered, looping over SYNC drops
 * as needed. return 0 on normal EAGAIN, -1 on fatal error.
 *
 * This is the entire input path, and the ONLY caller of audio_trigger - the
 * ring's single-producer rule therefore holds by construction, not by
 * discipline. Anything that wants to trigger a sound from somewhere else is
 * a design change, not a call site. */
static int drain_device(input_dev_t *in) {
    unsigned int flag = LIBEVDEV_READ_FLAG_NORMAL;
    for (;;) {
        struct input_event ie;
        int rc = libevdev_next_event(in->dev, flag, &ie);
        if (rc == -EAGAIN)
            break;
        if (rc == LIBEVDEV_READ_STATUS_SYNC) {
            flag = LIBEVDEV_READ_FLAG_SYNC;
            continue;
        }
        if (rc != LIBEVDEV_READ_STATUS_SUCCESS) {
            LOG_WARN("libevdev_next_event(%s): %s",
                     in->path, strerror(-rc));
            return -1;
        }
        flag = LIBEVDEV_READ_FLAG_NORMAL;

        /* Presses only. A release is not a second strike, and autorepeat
         * (value == 2) is the kernel talking, not a finger - one held key
         * would otherwise machine-gun the sample. */
        if (ie.type != EV_KEY || ie.value != 1)
            continue;
        int s = ie.code < KEY_CNT ? g_key_sample[ie.code] : -1;
        if (s < 0)
            continue;
        /* Stamped with the key's own event time rather than now: that is
         * the whole point of the placement math, and the two differ by
         * however long this drain has run. */
        audio_trigger(s, (uint64_t)ie.time.tv_sec * 1000000000ull +
                         (uint64_t)ie.time.tv_usec * 1000ull);
    }
    return 0;
}

static int run_loop(dev_list_t *devs, evclack_config_t *cfg,
                    const char *input_dir, const char *config_path) {
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        LOG_ERR("epoll_create1: %s", strerror(errno));
        return -1;
    }

    /* Hotplug: any create/attrib change under input_dir (or its by-id /
     * by-path symlink dirs) triggers a reconcile pass. IN_ATTRIB matters:
     * nodes are typically root-only at IN_CREATE time and only become
     * readable once udev applies the input-group permissions. */
    int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd < 0) {
        LOG_WARN("inotify_init1: %s - hotplug disabled", strerror(errno));
    } else {
        static const char *subs[] = { "", "/by-id", "/by-path" };
        for (size_t i = 0; i < sizeof(subs) / sizeof(subs[0]); i++) {
            char p[PATH_MAX];
            snprintf(p, sizeof(p), "%s%s", input_dir, subs[i]);
            inotify_add_watch(ifd, p, IN_CREATE | IN_ATTRIB | IN_MOVED_TO);
        }
        struct epoll_event ev = { .events = EPOLLIN, .data = { .ptr = NULL } };
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, ifd, &ev) < 0) {
            LOG_WARN("epoll_ctl ADD inotify: %s - hotplug disabled",
                     strerror(errno));
            close(ifd);
            ifd = -1;
        }
    }

    /* Audio asking to be rebuilt. Losing this watch is not fatal - it costs
     * recovery from a PipeWire restart, not the daemon. */
    int afd = audio_wake_fd();
    if (afd >= 0) {
        struct epoll_event ev = { .events = EPOLLIN,
                                  .data = { .ptr = &g_ep_audio } };
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, afd, &ev) < 0)
            LOG_WARN("epoll_ctl ADD audio: %s - a lost stream will stay lost",
                     strerror(errno));
    }

    /* Config auto-reload. Losing this costs the watch, not the daemon -
     * SIGHUP still reloads. */
    int cfd = config_watch_open(config_path);
    if (cfd >= 0) {
        struct epoll_event ev = { .events = EPOLLIN,
                                  .data = { .ptr = &g_ep_config } };
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev) < 0) {
            LOG_WARN("epoll_ctl ADD config watch: %s - auto-reload disabled",
                     strerror(errno));
            close(cfd);
            cfd = -1;
        }
    }

    /* SIGHUP, via the handler's eventfd. */
    if (g_sigfd >= 0) {
        struct epoll_event ev = { .events = EPOLLIN,
                                  .data = { .ptr = &g_ep_signal } };
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, g_sigfd, &ev) < 0)
            LOG_WARN("epoll_ctl ADD signal: %s - SIGHUP will not reload until "
                     "the next key event", strerror(errno));
    }

    reconcile_devices(devs, cfg, input_dir, epfd, 1);
    if (devs->n == 0) {
        if (ifd < 0) {
            LOG_ERR("Failed to open any input device - aborting");
            close(epfd);
            return -1;
        }
        LOG_WARN("No %s present; waiting for hotplug",
                 cfg->auto_discover ? "matching keyboard"
                                    : "configured device");
    }

    struct epoll_event events[16];

    while (g_running) {
        int nfd = epoll_wait(epfd, events, 16, -1);
        if (nfd < 0) {
            if (errno == EINTR) continue;
            LOG_ERR("epoll_wait: %s", strerror(errno));
            break;
        }
        int rescan = 0, reload = 0;
        for (int i = 0; i < nfd; i++) {
            if (events[i].data.ptr == NULL) { /* inotify fd */
                char buf[4096];
                while (read(ifd, buf, sizeof(buf)) > 0)
                    ;
                rescan = 1;
                continue;
            }

            if (*(int *)events[i].data.ptr == EP_AUDIO) {
                audio_restart();
                continue;
            }

            if (*(int *)events[i].data.ptr == EP_CONFIG) {
                if (config_watch_hit(cfd))
                    reload = 1;
                continue;
            }

            if (*(int *)events[i].data.ptr == EP_SIGNAL) {
                uint64_t drain;
                while (read(g_sigfd, &drain, sizeof(drain)) > 0)
                    ;
                continue;   /* g_reload / g_running are read below */
            }


            input_dev_t *in = events[i].data.ptr;
            int dead = 0;

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                LOG_WARN("device %s gone (EPOLLERR/HUP), removing",
                         in->path);
                dead = 1;
            } else if (drain_device(in) < 0) {
                LOG_WARN("dropping %s", in->path);
                dead = 1;
            }

            if (dead) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, in->fd, NULL);
                dev_list_remove(devs, in);
                input_close(in);
                free(in);
            }
        }
        if (g_reload) {
            g_reload = 0;
            reload   = 1;
        }
        if (reload) {
            /* Reloading reconciles devices itself, against the NEW config. */
            config_reload(devs, cfg, config_path, input_dir, epfd);
        } else if (rescan) {
            reconcile_devices(devs, cfg, input_dir, epfd, 0);
        }
        if (devs->n == 0 && ifd < 0) {
            LOG_ERR("No devices left and hotplug unavailable - exiting");
            break;
        }
    }

    if (ifd >= 0) close(ifd);
    if (cfd >= 0) close(cfd);
    close(epfd);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* main                                                                      */
/* ------------------------------------------------------------------------- */

static void print_usage(FILE *s, const char *prog) {
    fprintf(s,
        "evclack - keyboard hitsound daemon\n"
        "\n"
        "Watches evdev keyboards and plays a sample on every press of a configured\n"
        "key. Nothing is grabbed and nothing is re-emitted: the keys reach the rest\n"
        "of the system exactly as they always did. Releases and autorepeat are\n"
        "silent, and every sound overlaps rather than cutting off the one before it.\n"
        "\n"
        "usage: %s [-h] [-c CONFIG] [-i DIR]\n"
        "\n"
        "options:\n"
        "    -h          show this help and exit\n"
        "    -c CONFIG   path to YAML config\n"
        "    -i DIR      directory to scan/watch for event devices\n"
        "                (default /dev/input; mainly for testing)\n"
        "\n"
        "Without -c, the config is looked up at\n"
        "$XDG_CONFIG_HOME/evclack/config.yaml (~/.config if unset),\n"
        "falling back to %s.\n",
        prog, DEF_CONFIG);
}

/* Resolve the config path when -c wasn't given: prefer the per-user XDG
 * config, fall back to the installed default. */
static const char *default_config_path(void) {
    static char path[4096];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    int n;

    if (xdg && *xdg) {
        n = snprintf(path, sizeof(path), "%s/evclack/config.yaml", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home)
            return DEF_CONFIG;
        n = snprintf(path, sizeof(path), "%s/.config/evclack/config.yaml",
                     home);
    }
    if (n < 0 || (size_t)n >= sizeof(path))
        return DEF_CONFIG;
    if (access(path, R_OK) == 0)
        return path;
    return DEF_CONFIG;
}

int
main(int argc, char **argv) {
    const char *config_path = NULL;
    const char *input_dir   = "/dev/input";

    for (int opt; (opt = getopt(argc, argv, "hc:i:")) != -1; ) {
        switch (opt) {
            case 'h':
            print_usage(stdout, argv[0]);
            return EXIT_SUCCESS;
            case 'c':
            config_path = optarg;
            break;
            case 'i':
            input_dir = optarg;
            break;
            default:
            print_usage(stderr, argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (!config_path)
        config_path = default_config_path();
    LOG_INFO("Using config %s", config_path);

    evclack_config_t cfg;
    config_init(&cfg);
    if (load_config(config_path, &cfg) != 0)
        return EXIT_FAILURE;

    /* Resolve the bindings before anything is opened: this is where a
     * duplicate key or an over-full sample table is caught, and there is no
     * point bringing up a graph for a config that cannot run. */
    if (bindings_plan(&cfg, g_key_sample, g_refs, &g_n_refs) != 0) {
        config_free(&cfg);
        return EXIT_FAILURE;
    }

    char devdesc[32];
    if (cfg.auto_discover)
        snprintf(devdesc, sizeof(devdesc), "auto-discover");
    else
        snprintf(devdesc, sizeof(devdesc), "%zu device(s)", cfg.n_devices);
    LOG_INFO("Config: %s, %zu key(s) on %d sample(s), audio=%s",
             devdesc, cfg.n_bind, g_n_refs,
             cfg.audio_enabled ? "enabled" : "disabled");
    for (size_t i = 0; i < cfg.n_bind; i++) {
        const binding_t *b = &cfg.bind[i];
        if (b->start_ms > 0.0 || b->end_ms > 0.0)
            LOG_INFO("  %-14s -> %s [%.1f-%.1f ms] @ %.2f",
                     key_name_or(b->key), b->sample, b->start_ms, b->end_ms,
                     (double)b->gain);
        else
            LOG_INFO("  %-14s -> %s @ %.2f", key_name_or(b->key), b->sample,
                     (double)b->gain);
    }

	/* use rt scheduler if we can */
    struct sched_param sp = { .sched_priority = 90 };
    if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0) {
        LOG_WARN("Failed to set SCHED_FIFO: %s. Falling back to standard scheduler.", strerror(errno));
    } else {
        LOG_INFO("Successfully acquired SCHED_FIFO real-time priority.");
    }

    /* Best-effort audio. All main() still owns is which sample id each
     * distinct sound got; decoding, conforming and bringing up the node are
     * the subsystem's own business. A sample that fails to load leaves its
     * id unmapped, and audio_trigger treats that as nothing to play - so
     * those keys go silent and the rest still sound. */
    if (cfg.audio_enabled) {
        audio_load_refs(audio.sample, g_refs, g_n_refs);
        audio_start(cfg.audio_latency);
    } else {
        /* Worth saying out loud. In the daemon this grew out of, disabling
         * audio still left a working SOCD cleaner; here it leaves a process
         * that opens every matching keyboard and does nothing at all. */
        LOG_WARN("'audio.enabled' is false - evclack will watch keyboards "
                 "and play nothing");
    }

    /* Handlers for graceful shutdown, and for SIGHUP -> reload. The eventfd
     * comes first so the handler always has somewhere to poke: a SIGHUP that
     * only set a flag would not be acted on until the next key event, since
     * epoll_wait is where this process spends its life. No SA_RESTART, so a
     * signal landing before the fd is watched still breaks the wait. */
    g_sigfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_sigfd < 0)
        LOG_WARN("eventfd: %s - SIGHUP reload may be delayed until the next "
                 "key event", strerror(errno));

    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* run_loop opens devices itself (initial reconcile + hotplug). */
    dev_list_t devs = { 0 };
    LOG_INFO("Running.");
    int rc = run_loop(&devs, &cfg, input_dir, config_path);

    LOG_INFO("Shutting down");

    /* Devices first, then audio: drain_device is the ring's only producer,
     * so nothing can be pushing into it while the consumer is stopped. */
    for (size_t i = 0; i < devs.n; i++) {
        input_close(devs.v[i]);
        free(devs.v[i]);
    }
    free(devs.v);
    audio_cleanup();
    if (g_sigfd >= 0) close(g_sigfd);
    config_free(&cfg);
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
