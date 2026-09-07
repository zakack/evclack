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

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
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
 * so anything under 256 works. */
#define AUDIO_NSAMPLES 24

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

/* Decode `path`, conform it to the mix format, and map it as sample `idx`.
 *
 * This lives here rather than in main() on purpose: it is the last piece of
 * the audio subsystem that used to sit outside the banners, so everything
 * between them is now the whole of it - load, start, trigger, mix, cleanup.
 * What stays the caller's business is which sample id means what and where
 * the paths and gains came from, which is exactly the part that differs
 * between one program and the next. */
static int audio_load(int idx, const char *path, float gain) {
    if (idx < 0 || idx >= AUDIO_NSAMPLES || !path) return -1;

    size_t frames; int ch, rate;
    float *raw = sample_decode(path, &frames, &ch, &rate);
    if (!raw) return -1;

    int rc = sample_conform(raw, frames, ch, rate, &audio.sample[idx]);
    free(raw);
    if (rc != 0) return -1;

    audio.sample[idx].gain = gain;
    return 0;
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
static void audio_restart(void) {
    if (!audio_available) return;
    if (!atomic_load(&audio.restart)) return;

    audio_stream_close();

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

static void audio_cleanup(void) {
    if (!audio_available) return;
    audio_stream_close();
    if (audio.wakefd >= 0) { close(audio.wakefd); audio.wakefd = -1; }
    unsigned dropped = atomic_load(&trig.overruns);
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
 * audio.gain once the whole file has been read. */
typedef struct {
    int    key;
    char  *sample;
    float  gain;
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
                "gain} mappings");
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

/* One distinct sound: a file and the volume to play it at. Two bindings that
 * agree on both share a slot, so ten keys pointing at one hitsound decode and
 * resample it once. Interning on the PAIR rather than on the path alone is
 * what keeps audio_sample_t owning its buffer outright - split that ownership
 * and audio_cleanup's frees, and every test that pokes at audio.sample[],
 * would have to learn about refcounts to buy a case nobody hits. */
typedef struct {
    const char *path;
    float       gain;
} sample_ref_t;

/* code -> sample id, or -1. KEY_CNT entries of int16_t is 1.5KB, which buys
 * an O(1) lookup in the drain path and removes the question of how many keys
 * may be bound: the only ceiling left is AUDIO_NSAMPLES distinct sounds.
 *
 * Filled ONLY by bindings_plan. A static int16_t array is .bss zeros, and 0
 * is a valid sample id - a table that reached the drain path unfilled would
 * make every key on the board clack with sample 0, silently, which is the
 * exact inverse of what the config says. */
static int16_t g_key_sample[KEY_CNT];

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
                refs[j].gain == b->gain) { id = j; break; }
        if (id < 0) {
            if (*n_refs >= AUDIO_NSAMPLES) {
                LOG_ERR("more than %d distinct samples; bindings that share "
                        "a file AND a gain share a sample, so give some of "
                        "them the same gain or use fewer files",
                        AUDIO_NSAMPLES);
                return -1;
            }
            id = (*n_refs)++;
            refs[id].path = b->sample;
            refs[id].gain = b->gain;
        }
        key_sample[b->key] = (int16_t)id;
    }
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
enum { EP_INPUT = 1, EP_AUDIO = 2 };

/* The audio wake eventfd has no struct of its own; the epoll set is keyed
 * on a leading discriminator, so it gets a standing one. */
static int g_ep_audio = EP_AUDIO;

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
static int auto_open_ok(struct libevdev *dev, const evclack_config_t *cfg) {
    /* Every bound key, not just some. A board carrying a subset would be
     * opened for keys it cannot report. */
    for (size_t i = 0; i < cfg->n_bind; i++)
        if (!libevdev_has_event_code(dev, EV_KEY, (unsigned)cfg->bind[i].key))
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

/* ------------------------------------------------------------------------- */
/* loop                                                                      */
/* ------------------------------------------------------------------------- */

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

static int run_loop(dev_list_t *devs, const evclack_config_t *cfg,
                    const char *input_dir) {
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
        int rescan = 0;
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
        if (rescan) {
            reconcile_devices(devs, cfg, input_dir, epfd, 0);
        }
        if (devs->n == 0 && ifd < 0) {
            LOG_ERR("No devices left and hotplug unavailable - exiting");
            break;
        }
    }

    if (ifd >= 0) close(ifd);
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
    sample_ref_t refs[AUDIO_NSAMPLES];
    int n_refs = 0;
    if (bindings_plan(&cfg, g_key_sample, refs, &n_refs) != 0) {
        config_free(&cfg);
        return EXIT_FAILURE;
    }

    char devdesc[32];
    if (cfg.auto_discover)
        snprintf(devdesc, sizeof(devdesc), "auto-discover");
    else
        snprintf(devdesc, sizeof(devdesc), "%zu device(s)", cfg.n_devices);
    LOG_INFO("Config: %s, %zu key(s) on %d sample(s), audio=%s",
             devdesc, cfg.n_bind, n_refs,
             cfg.audio_enabled ? "enabled" : "disabled");
    for (size_t i = 0; i < cfg.n_bind; i++)
        LOG_INFO("  %-14s -> %s @ %.2f", key_name_or(cfg.bind[i].key),
                 cfg.bind[i].sample, (double)cfg.bind[i].gain);

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
        for (int i = 0; i < n_refs; i++)
            if (audio_load(i, refs[i].path, refs[i].gain) != 0)
                LOG_WARN("hitsound load failed (%s); the keys bound to it "
                         "stay silent", refs[i].path);

        audio_start(cfg.audio_latency);
    }

    /* handlers for graceful shutdown. */
    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* run_loop opens devices itself (initial reconcile + hotplug). */
    dev_list_t devs = { 0 };
    LOG_INFO("Running.");
    int rc = run_loop(&devs, &cfg, input_dir);

    LOG_INFO("Shutting down");

    /* Devices first, then audio: drain_device is the ring's only producer,
     * so nothing can be pushing into it while the consumer is stopped. */
    for (size_t i = 0; i < devs.n; i++) {
        input_close(devs.v[i]);
        free(devs.v[i]);
    }
    free(devs.v);
    audio_cleanup();
    config_free(&cfg);
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
