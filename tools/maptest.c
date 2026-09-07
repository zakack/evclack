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
        char yaml[4096];
        size_t o = (size_t)snprintf(yaml, sizeof yaml,
                                    "audio: {sample: /s/base.wav}\nkeys:\n");
        /* One distinct gain per key, so nothing interns. */
        for (int i = 0; i <= AUDIO_NSAMPLES && o < sizeof yaml; i++)
            o += (size_t)snprintf(yaml + o, sizeof yaml - o,
                                  "  - {key: %d, gain: %.4f}\n",
                                  KEY_A + i, 0.01 * (i + 1));
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

    if (failures) {
        printf("\n%d assertion(s) FAILED\n", failures);
        return 1;
    }
    puts("\nall assertions passed");
    return 0;
}
