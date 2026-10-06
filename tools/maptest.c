/* maptest - assertions over evclack's key -> sample layer.
 *
 * Includes evclack.c and drives load_config() and bindings_plan() directly,
 * so it tests the daemon's own parser and interner rather than a copy. That
 * is the point of the #include, and the reason this builds as part of `all`:
 * a test that can go stale against the thing it tests is worth nothing.
 *
 * bindings_plan does no I/O by design, which is what lets every case here
 * name sample files that do not exist. Only the YAML has to be real, and it
 * is written to a temp file per case.
 */
#define main evclack_main
#include "evclack.c"
#undef main

static int failures;

static void expect_int(const char *what, long long got, long long want) {
    if (got == want) {
        printf("  ok   %-52s %lld\n", what, got);
    } else {
        printf("  FAIL %-52s got %lld, want %lld\n", what, got, want);
        failures++;
    }
}

static void expect_str(const char *what, const char *got, const char *want) {
    if (got && want && strcmp(got, want) == 0) {
        printf("  ok   %-52s %s\n", what, got);
    } else {
        printf("  FAIL %-52s got %s, want %s\n", what,
               got ? got : "(null)", want ? want : "(null)");
        failures++;
    }
}

static void expect_f(const char *what, float got, float want) {
    if (fabsf(got - want) < 1e-6f) {
        printf("  ok   %-52s %.4f\n", what, (double)got);
    } else {
        printf("  FAIL %-52s got %.4f, want %.4f\n", what,
               (double)got, (double)want);
        failures++;
    }
}

/* Write `yaml` to a temp file and load it. Returns 0 on success like
 * load_config does; the caller owns cfg either way (load_config frees it
 * itself on failure, leaving a zeroed struct that config_free tolerates). */
static int load_yaml(const char *yaml, evclack_config_t *cfg) {
    char path[] = "/tmp/evclack-maptest-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); exit(2); }
    if (write(fd, yaml, strlen(yaml)) != (ssize_t)strlen(yaml)) {
        perror("write"); exit(2);
    }
    close(fd);
    config_init(cfg);
    int rc = load_config(path, cfg);
    unlink(path);
    return rc;
}

/* Write `yaml` to a temp file and leave it there; the caller unlinks. The
 * reload cases need a path that outlives the write, since config_validate
 * opens it itself. */
static void write_yaml(const char *yaml, char *path, size_t n) {
    snprintf(path, n, "/tmp/evclack-maptest-rl-XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); exit(2); }
    if (write(fd, yaml, strlen(yaml)) != (ssize_t)strlen(yaml)) {
        perror("write"); exit(2);
    }
    close(fd);
}

/* Validate + commit one config, the way config_reload does minus the device
 * and audio work. Returns config_validate's verdict. */
static int reload(const char *yaml, evclack_config_t *live, unsigned *action) {
    char path[64];
    reload_plan_t p;
    write_yaml(yaml, path, sizeof path);
    int rc = config_validate(path, &p);
    unlink(path);
    if (rc != 0) return rc;
    if (action) *action = p.action;
    reload_commit(&p, live);
    return 0;
}

/* Count table entries that are not -1, so "everything else is unbound" is a
 * property rather than a spot check. */
static int bound_keys(const int16_t *t) {
    int n = 0;
    for (int i = 0; i < KEY_CNT; i++)
        if (t[i] != -1) n++;
    return n;
}

int main(void) {
    int16_t      table[KEY_CNT];
    sample_ref_t refs[AUDIO_NSAMPLES];
    int          n_refs;
    evclack_config_t cfg;

    /* Line-buffer stdout so the LOG_ERR lines the rejection cases
     * deliberately produce interleave with the assertions they belong
     * to, instead of arriving in a block at exit. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    g_log_quiet = 1;

    puts("A. key codes: symbolic and numeric both resolve");
    {
        char yaml[256];
        snprintf(yaml, sizeof yaml,
                 "audio: {sample: /s/base.wav}\n"
                 "keys:\n"
                 "  - KEY_Z\n"
                 "  - %d\n", KEY_X);
        expect_int("load_config", load_yaml(yaml, &cfg), 0);
        expect_int("bindings", (long long)cfg.n_bind, 2);
        expect_int("keys[0] is KEY_Z", cfg.bind[0].key, KEY_Z);
        expect_int("keys[1] resolved from the integer", cfg.bind[1].key, KEY_X);
        expect_int("bindings_plan",
                   bindings_plan(&cfg, table, refs, &n_refs), 0);
        expect_int("KEY_Z is mapped", table[KEY_Z] >= 0, 1);
        expect_int("KEY_X is mapped", table[KEY_X] >= 0, 1);
        config_free(&cfg);
    }

    puts("B. same path AND same gain intern onto one sample");
    {
        expect_int("load_config", load_yaml(
            "audio: {sample: /s/base.wav, gain: 0.5}\n"
            "keys:\n"
            "  - KEY_Z\n"
            "  - KEY_X\n"
            "  - {key: KEY_C, sample: /s/base.wav, gain: 0.5}\n",
            &cfg), 0);
        expect_int("bindings_plan",
                   bindings_plan(&cfg, table, refs, &n_refs), 0);
        expect_int("three keys, one sample", n_refs, 1);
        expect_int("all three share id 0",
                   (table[KEY_Z] == 0) + (table[KEY_X] == 0) +
                   (table[KEY_C] == 0), 3);
        expect_f("interned gain", refs[0].gain, 0.5f);
        config_free(&cfg);
    }

    puts("C. same path at two gains is two samples");
    {
        expect_int("load_config", load_yaml(
            "audio: {sample: /s/base.wav, gain: 1.0}\n"
            "keys:\n"
            "  - KEY_Z\n"
            "  - {key: KEY_X, gain: 0.25}\n",
            &cfg), 0);
        expect_int("bindings_plan",
                   bindings_plan(&cfg, table, refs, &n_refs), 0);
        expect_int("two samples", n_refs, 2);
        expect_int("distinct ids", table[KEY_Z] != table[KEY_X], 1);
        expect_f("KEY_Z gain", refs[table[KEY_Z]].gain, 1.0f);
        expect_f("KEY_X gain", refs[table[KEY_X]].gain, 0.25f);
        expect_str("same file both times",
                   refs[table[KEY_X]].path, refs[table[KEY_Z]].path);
        config_free(&cfg);
    }

    puts("D. a binding inherits audio.sample / audio.gain");
    {
        expect_int("load_config", load_yaml(
            "audio: {sample: /s/base.wav, gain: 0.75}\n"
            "keys:\n"
            "  - KEY_Z\n"
            "  - {key: KEY_X, sample: /s/other.ogg}\n"
            "  - {key: KEY_C, gain: 0.1}\n",
            &cfg), 0);
        expect_str("bare scalar takes the default sample",
                   cfg.bind[0].sample, "/s/base.wav");
        expect_f("bare scalar takes the default gain", cfg.bind[0].gain, 0.75f);
        expect_str("override wins for its key",
                   cfg.bind[1].sample, "/s/other.ogg");
        expect_f("...but gain still falls back", cfg.bind[1].gain, 0.75f);
        expect_str("gain-only override keeps the default sample",
                   cfg.bind[2].sample, "/s/base.wav");
        expect_f("gain-only override", cfg.bind[2].gain, 0.1f);
        config_free(&cfg);
    }

    puts("E. `keys` may precede `audio` in the file");
    {
        expect_int("load_config", load_yaml(
            "keys: [KEY_Z]\n"
            "audio: {sample: /s/late.wav, gain: 0.3}\n",
            &cfg), 0);
        expect_str("sample resolved anyway", cfg.bind[0].sample, "/s/late.wav");
        expect_f("gain resolved anyway", cfg.bind[0].gain, 0.3f);
        config_free(&cfg);
    }

    puts("F. unbound keys are -1, not 0");
    {
        expect_int("load_config", load_yaml(
            "audio: {sample: /s/base.wav}\nkeys: [KEY_Z]\n", &cfg), 0);
        /* Poison first: .bss zeros would read as "everything plays sample
         * 0", and a table filled by memset(0) would pass a spot check on
         * KEY_Z alone. */
        memset(table, 0, sizeof table);
        expect_int("bindings_plan",
                   bindings_plan(&cfg, table, refs, &n_refs), 0);
        expect_int("exactly one key bound", bound_keys(table), 1);
        expect_int("KEY_Z is sample 0", table[KEY_Z], 0);
        expect_int("KEY_A is unbound", table[KEY_A], -1);
        expect_int("KEY_ESC is unbound", table[KEY_ESC], -1);
        expect_int("code 0 is unbound", table[0], -1);
        config_free(&cfg);
    }

    puts("G. a duplicate key is rejected");
    {
        expect_int("load_config rejects it", load_yaml(
            "audio: {sample: /s/base.wav}\n"
            "keys:\n"
            "  - KEY_Z\n"
            "  - {key: KEY_Z, gain: 0.5}\n",
            &cfg), -1);
        config_free(&cfg);
    }

    puts("H. more than AUDIO_NSAMPLES distinct samples is rejected");
    {
        /* Sized off AUDIO_NSAMPLES, not a round number: this generates a
         * line per distinct sample, so a raised ceiling silently truncated
         * the config and turned this case into a no-op that still passed. */
        char yaml[64 * (AUDIO_NSAMPLES + 4)];
        size_t o = (size_t)snprintf(yaml, sizeof yaml,
                                    "audio: {sample: /s/base.wav}\nkeys:\n");
        /* One distinct gain per key, so nothing interns. */
        for (int i = 0; i <= AUDIO_NSAMPLES && o < sizeof yaml; i++)
            o += (size_t)snprintf(yaml + o, sizeof yaml - o,
                                  "  - {key: %d, gain: %.4f}\n",
                                  KEY_A + i, 0.01 * (i + 1));
        expect_int("the fixture fits", o < sizeof yaml, 1);
        expect_int("load_config", load_yaml(yaml, &cfg), 0);
        expect_int("bindings", (long long)cfg.n_bind, AUDIO_NSAMPLES + 1);
        expect_int("bindings_plan rejects it",
                   bindings_plan(&cfg, table, refs, &n_refs), -1);
        config_free(&cfg);
    }

    puts("I. a missing or empty `keys` list is rejected");
    {
        expect_int("no keys at all", load_yaml(
            "audio: {sample: /s/base.wav}\n", &cfg), -1);
        config_free(&cfg);
        expect_int("empty list", load_yaml(
            "audio: {sample: /s/base.wav}\nkeys: []\n", &cfg), -1);
        config_free(&cfg);
        expect_int("not a sequence", load_yaml(
            "audio: {sample: /s/base.wav}\nkeys: {k1: KEY_Z}\n", &cfg), -1);
        config_free(&cfg);
    }

    puts("J. with no `audio` block at all, bindings fall back to DEF_WAV");
    {
        expect_int("load_config", load_yaml("keys: [KEY_Z]\n", &cfg), 0);
        expect_str("default sample", cfg.bind[0].sample, DEF_WAV);
        expect_f("default gain", cfg.bind[0].gain, 1.0f);
        config_free(&cfg);
    }

    puts("K. refs_same_set is order-independent and yields a permutation");
    {
        sample_ref_t a[3] = { {"/s/a.wav", 1.0f, 0, 0},
                              {"/s/b.wav", 1.0f, 0, 0},
                              {"/s/c.wav", 0.5f, 0, 0} };
        sample_ref_t b[3] = { {"/s/c.wav", 0.5f, 0, 0},
                              {"/s/a.wav", 1.0f, 0, 0},
                              {"/s/b.wav", 1.0f, 0, 0} };
        int map[AUDIO_NSAMPLES];

        expect_int("reordered set matches", refs_same_set(a, 3, b, 3, map), 1);
        expect_int("map[0] -> c", map[0], 2);
        expect_int("map[1] -> a", map[1], 0);
        expect_int("map[2] -> b", map[2], 1);

        /* Same file, different gain, is a DIFFERENT sound - the interning
         * rule is on the pair, so the compare has to be too. */
        sample_ref_t c[3] = { {"/s/a.wav", 1.0f, 0, 0},
                              {"/s/b.wav", 1.0f, 0, 0},
                              {"/s/c.wav", 0.9f, 0, 0} };
        expect_int("a changed gain does not match",
                   refs_same_set(a, 3, c, 3, map), 0);

        sample_ref_t d[2] = { {"/s/a.wav", 1.0f, 0, 0},
                              {"/s/b.wav", 1.0f, 0, 0} };
        expect_int("a smaller set does not match",
                   refs_same_set(a, 3, d, 2, map), 0);
    }

    puts("L. a rejected reload changes nothing that is live");
    {
        evclack_config_t live;
        config_init(&live);
        expect_int("a good config commits",
                   reload("audio: {sample: /s/a.wav}\n"
                          "keys: [KEY_Z, KEY_X]\n", &live, NULL), 0);

        int16_t      snap_table[KEY_CNT];
        sample_ref_t snap_refs[AUDIO_NSAMPLES];
        int          snap_n = g_n_refs;
        memcpy(snap_table, g_key_sample, sizeof snap_table);
        memcpy(snap_refs,  g_refs,       sizeof snap_refs);
        expect_int("committed 2 keys", bound_keys(g_key_sample), 2);

        /* Not a config at all. */
        expect_int("broken YAML is rejected",
                   reload("keys: [KEY_Z\n  - oops\n", &live, NULL), -1);
        expect_int("  table untouched",
                   memcmp(snap_table, g_key_sample, sizeof snap_table), 0);
        expect_int("  refs untouched",
                   memcmp(snap_refs, g_refs, sizeof snap_refs), 0);

        /* Parses, but bindings_plan rejects it - the case that would
         * otherwise leave a half-written table behind. */
        expect_int("duplicate key is rejected",
                   reload("audio: {sample: /s/a.wav}\n"
                          "keys: [KEY_Z, KEY_Z]\n", &live, NULL), -1);
        expect_int("  table untouched",
                   memcmp(snap_table, g_key_sample, sizeof snap_table), 0);
        expect_int("  refs untouched",
                   memcmp(snap_refs, g_refs, sizeof snap_refs), 0);
        expect_int("  ref count untouched", g_n_refs, snap_n);

        config_free(&live);
    }

    puts("M. reordering keys re-indexes without moving a loaded sample");
    {
        /* The fast path only engages when audio is up and every committed
         * sample loaded, so stand that state up by hand: no PipeWire here,
         * and the buffers are never dereferenced - config_validate only asks
         * whether they are NULL. */
        evclack_config_t live;
        config_init(&live);
        memset(g_refs, 0, sizeof g_refs);
        g_n_refs = 0;
        audio_available = 0;

        unsigned action = 0;
        expect_int("initial load",
                   reload("audio: {latency: 256}\n"
                          "keys:\n"
                          "  - {key: KEY_Z, sample: /s/a.wav}\n"
                          "  - {key: KEY_X, sample: /s/b.wav}\n",
                          &live, &action), 0);
        expect_int("  audio was down, so it is a full start",
                   (long long)action, RELOAD_AUDIO_ON);

        float dummy[2] = { 0.0f, 0.0f };
        audio.sample[0].samples = dummy;
        audio.sample[1].samples = dummy;
        audio.latency   = 256;
        audio_available = 1;

        /* Two reloads that only reorder the file. The second is the one that
         * matters: a commit that wrote g_refs in plan order instead of
         * through map looks fine after the first and silently swaps the two
         * samples on the next compare. */
        const char *swapped =
            "audio: {latency: 256}\n"
            "keys:\n"
            "  - {key: KEY_X, sample: /s/b.wav}\n"
            "  - {key: KEY_Z, sample: /s/a.wav}\n";
        const char *original =
            "audio: {latency: 256}\n"
            "keys:\n"
            "  - {key: KEY_Z, sample: /s/a.wav}\n"
            "  - {key: KEY_X, sample: /s/b.wav}\n";

        for (int pass = 0; pass < 3; pass++) {
            const char *yaml = (pass % 2 == 0) ? swapped : original;
            expect_int("reorder-only reload", reload(yaml, &live, &action), 0);
            expect_int("  nothing to rebuild", (long long)action, RELOAD_NONE);
            expect_str("  KEY_Z still plays a.wav",
                       g_refs[g_key_sample[KEY_Z]].path, "/s/a.wav");
            expect_str("  KEY_X still plays b.wav",
                       g_refs[g_key_sample[KEY_X]].path, "/s/b.wav");
        }

        /* A changed gain is a different sound, so the samples must move. */
        expect_int("changing a gain forces a re-decode",
                   reload("audio: {latency: 256}\n"
                          "keys:\n"
                          "  - {key: KEY_Z, sample: /s/a.wav, gain: 0.5}\n"
                          "  - {key: KEY_X, sample: /s/b.wav}\n",
                          &live, &action), 0);
        expect_int("  action", (long long)action, RELOAD_SAMPLES);

        audio.sample[0].samples = NULL;
        audio.sample[1].samples = NULL;
        audio_available = 0;
        config_free(&live);
    }

    /* --- The sprite-sheet layer: one file, one sound per key. -------- */

    puts("N. a slice is part of a sample's identity");
    {
        /* Exactly the shape an imported Mechvibes pack has: every binding
         * names the SAME file and differs only in the window. Interning on
         * the path alone would collapse all four onto one sound, and every
         * key would play the same slice - the failure this case exists for.
         */
        expect_int("load_config",
                   load_yaml("audio: {sample: /s/pack.ogg}\n"
                             "keys:\n"
                             "  - {key: KEY_A, start: 12946, end: 13137}\n"
                             "  - {key: KEY_B, start: 13470, end: 13660}\n"
                             "  - {key: KEY_C, start: 12946, end: 13137}\n"
                             "  - KEY_D\n", &cfg), 0);
        expect_int("bindings_plan",
                   bindings_plan(&cfg, table, refs, &n_refs), 0);

        /* Three distinct sounds, not four and not one: A and C name the same
         * window and share a slot, D takes the whole file. */
        expect_int("distinct samples", n_refs, 3);
        expect_int("KEY_A and KEY_C share a slice",
                   table[KEY_A], table[KEY_C]);
        expect_int("KEY_B is its own slice", table[KEY_B] != table[KEY_A], 1);
        expect_int("the unsliced binding is its own sample",
                   table[KEY_D] != table[KEY_A], 1);

        expect_f("start survives the plan", refs[table[KEY_A]].start_ms,
                 12946.0);
        expect_f("end survives the plan", refs[table[KEY_A]].end_ms, 13137.0);
        expect_f("an omitted start is 0", refs[table[KEY_D]].start_ms, 0.0);
        expect_f("an omitted end is 0", refs[table[KEY_D]].end_ms, 0.0);
        config_free(&cfg);
    }

    puts("O. a fractional slice survives the parse exactly");
    {
        /* Mechvibes-DX writes half-millisecond bounds (45832.5). Narrowing
         * them to float would move the window AND break the exact compare
         * refs_same_set does, so this pins the double all the way through. */
        expect_int("load_config",
                   load_yaml("audio: {sample: /s/pack.ogg}\n"
                             "keys:\n"
                             "  - {key: KEY_A, start: 45832.5, end: 45914.25}\n",
                             &cfg), 0);
        expect_int("bindings_plan",
                   bindings_plan(&cfg, table, refs, &n_refs), 0);
        expect_f("start", refs[0].start_ms, 45832.5);
        expect_f("end", refs[0].end_ms, 45914.25);
        config_free(&cfg);
    }

    puts("P. nonsense slices are rejected by load_config");
    {
        expect_int("end before start",
                   load_yaml("audio: {sample: /s/a.wav}\n"
                             "keys:\n  - {key: KEY_Z, start: 500, end: 100}\n",
                             &cfg), -1);
        config_free(&cfg);
        expect_int("end equal to start",
                   load_yaml("audio: {sample: /s/a.wav}\n"
                             "keys:\n  - {key: KEY_Z, start: 500, end: 500}\n",
                             &cfg), -1);
        config_free(&cfg);
        expect_int("a negative start",
                   load_yaml("audio: {sample: /s/a.wav}\n"
                             "keys:\n  - {key: KEY_Z, start: -1}\n",
                             &cfg), -1);
        config_free(&cfg);
        expect_int("a non-numeric start",
                   load_yaml("audio: {sample: /s/a.wav}\n"
                             "keys:\n  - {key: KEY_Z, start: soon}\n",
                             &cfg), -1);
        config_free(&cfg);

        /* A start with no end is legal: play from there to the end. */
        expect_int("start alone is fine",
                   load_yaml("audio: {sample: /s/a.wav}\n"
                             "keys:\n  - {key: KEY_Z, start: 500}\n",
                             &cfg), 0);
        expect_f("  start", cfg.bind[0].start_ms, 500.0);
        expect_f("  end stays open", cfg.bind[0].end_ms, 0.0);
        config_free(&cfg);
    }

    puts("Q. re-slicing forces a re-decode; reordering slices does not");
    {
        evclack_config_t live;
        unsigned action = 0;
        static float dummy[8];

        config_init(&live);
        expect_int("initial load",
                   reload("audio: {sample: /s/pack.ogg}\n"
                          "keys:\n"
                          "  - {key: KEY_Z, start: 100, end: 200}\n"
                          "  - {key: KEY_X, start: 300, end: 400}\n",
                          &live, &action), 0);

        /* Stand the fast path up by hand, as case M does: it only engages
         * when audio is up and every committed ref actually loaded. These
         * buffers are never dereferenced. */
        audio_available = 1;
        audio.sample[0].samples = dummy;
        audio.sample[1].samples = dummy;

        expect_int("reordering the same two slices",
                   reload("audio: {sample: /s/pack.ogg}\n"
                          "keys:\n"
                          "  - {key: KEY_X, start: 300, end: 400}\n"
                          "  - {key: KEY_Z, start: 100, end: 200}\n",
                          &live, &action), 0);
        expect_int("  nothing to rebuild", (long long)action, RELOAD_NONE);

        /* One window moved by a single millisecond. Same file, same gain,
         * same count - so only a slice-aware compare can see it, and missing
         * it leaves that key playing the OLD region of the pack forever. */
        expect_int("moving one slice",
                   reload("audio: {sample: /s/pack.ogg}\n"
                          "keys:\n"
                          "  - {key: KEY_Z, start: 100, end: 200}\n"
                          "  - {key: KEY_X, start: 300, end: 401}\n",
                          &live, &action), 0);
        expect_int("  forces a re-decode", (long long)action, RELOAD_SAMPLES);

        audio.sample[0].samples = NULL;
        audio.sample[1].samples = NULL;
        audio_available = 0;
        config_free(&live);
    }

    if (failures) {
        printf("\n%d assertion(s) FAILED\n", failures);
        return 1;
    }
    puts("\nall assertions passed");
    return 0;
}
