/* evclack-import - turn a Mechvibes soundpack or an osu! skin into a
 * config.yaml evclack can run.
 *
 * evclack's decoder has always accepted what these packs ship: libsndfile
 * reads the .ogg and .mp3 an osu! skin and a Mechvibes pack are made of,
 * with no transcoding. What was missing was the PACKAGING layer - a pack is
 * a hundred sounds and a manifest, and turning that into a `keys:` list by
 * hand is not something anyone will do twice.
 *
 * This is a separate binary and deliberately NOT a `soundpack:` key inside
 * the daemon. The daemon stays a passive listener with one dependency set
 * and no JSON, no .ini and no archive layout in it; what comes out here is
 * an ordinary config the user can read, diff and hand-edit, and dropping it
 * on ~/.config/evclack/config.yaml is picked up live by the running daemon.
 *
 * It writes text and nothing else. libsndfile is opened read-only, for two
 * questions text cannot answer: whether an osu! sound file is one of the
 * deliberately-silent placeholders skins use to MUTE a sound, and whether a
 * pack's last slice runs off the end of its own recording.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <libevdev/libevdev.h>
#include <linux/input-event-codes.h>
#include <sndfile.h>
#include <yaml.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static const char *g_prog = "evclack-import";

static void warnf(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s: warning: ", g_prog);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
}

static void dief(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s: ", g_prog);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) dief("out of memory");
    return p;
}

static char *xstrdup(const char *s) {
    char *p = strdup(s);
    if (!p) dief("out of memory");
    return p;
}

/* ------------------------------------------------------------------------- */
/* Key names                                                                 */
/* ------------------------------------------------------------------------- */

/* Both formats name keys in W3C KeyboardEvent.code, either directly
 * (Mechvibes-DX) or after one hop through an iohook number (classic
 * Mechvibes). This is the single place that decides what a key name means,
 * and everything else goes through it.
 *
 * SCOPE is the interesting column, and it exists because of a property of
 * the daemon rather than of the packs. evclack's auto-discovery opens a
 * keyboard only if it advertises EVERY bound key, so a config is not a
 * neutral list of wishes: binding one key no real board reports disqualifies
 * the board entirely, and the only symptom is silence plus an
 * `auto_missing_key` line in the log. A pack defines around a hundred keys
 * whether or not the user owns them, so importing everything a pack offers
 * is the single most likely way to end up with a silent daemon.
 *
 * MAIN is therefore the default and holds the USB HID boot-protocol range -
 * what a keyboard driver claims whether or not the plastic has the key on
 * it. FULL adds the rest, and is worth having because a board that does
 * report them sounds better for it.
 *
 * The evdev names are not paired with numbers here on purpose:
 * libevdev_event_code_from_name resolves them at startup, so the table
 * cannot drift from what evclack itself will accept, and a typo is a startup
 * error rather than a key that silently never binds. */
enum { SCOPE_MAIN = 0, SCOPE_FULL = 1 };

typedef struct {
    const char *w3c;
    const char *evdev;
    int         scope;
} keymap_t;

static const keymap_t g_keys[] = {
    /* Letters and digits. */
    { "KeyA", "KEY_A", SCOPE_MAIN }, { "KeyB", "KEY_B", SCOPE_MAIN },
    { "KeyC", "KEY_C", SCOPE_MAIN }, { "KeyD", "KEY_D", SCOPE_MAIN },
    { "KeyE", "KEY_E", SCOPE_MAIN }, { "KeyF", "KEY_F", SCOPE_MAIN },
    { "KeyG", "KEY_G", SCOPE_MAIN }, { "KeyH", "KEY_H", SCOPE_MAIN },
    { "KeyI", "KEY_I", SCOPE_MAIN }, { "KeyJ", "KEY_J", SCOPE_MAIN },
    { "KeyK", "KEY_K", SCOPE_MAIN }, { "KeyL", "KEY_L", SCOPE_MAIN },
    { "KeyM", "KEY_M", SCOPE_MAIN }, { "KeyN", "KEY_N", SCOPE_MAIN },
    { "KeyO", "KEY_O", SCOPE_MAIN }, { "KeyP", "KEY_P", SCOPE_MAIN },
    { "KeyQ", "KEY_Q", SCOPE_MAIN }, { "KeyR", "KEY_R", SCOPE_MAIN },
    { "KeyS", "KEY_S", SCOPE_MAIN }, { "KeyT", "KEY_T", SCOPE_MAIN },
    { "KeyU", "KEY_U", SCOPE_MAIN }, { "KeyV", "KEY_V", SCOPE_MAIN },
    { "KeyW", "KEY_W", SCOPE_MAIN }, { "KeyX", "KEY_X", SCOPE_MAIN },
    { "KeyY", "KEY_Y", SCOPE_MAIN }, { "KeyZ", "KEY_Z", SCOPE_MAIN },
    { "Digit0", "KEY_0", SCOPE_MAIN }, { "Digit1", "KEY_1", SCOPE_MAIN },
    { "Digit2", "KEY_2", SCOPE_MAIN }, { "Digit3", "KEY_3", SCOPE_MAIN },
    { "Digit4", "KEY_4", SCOPE_MAIN }, { "Digit5", "KEY_5", SCOPE_MAIN },
    { "Digit6", "KEY_6", SCOPE_MAIN }, { "Digit7", "KEY_7", SCOPE_MAIN },
    { "Digit8", "KEY_8", SCOPE_MAIN }, { "Digit9", "KEY_9", SCOPE_MAIN },

    /* Punctuation. */
    { "Minus", "KEY_MINUS", SCOPE_MAIN },
    { "Equal", "KEY_EQUAL", SCOPE_MAIN },
    { "BracketLeft", "KEY_LEFTBRACE", SCOPE_MAIN },
    { "BracketRight", "KEY_RIGHTBRACE", SCOPE_MAIN },
    { "Backslash", "KEY_BACKSLASH", SCOPE_MAIN },
    { "Semicolon", "KEY_SEMICOLON", SCOPE_MAIN },
    { "Quote", "KEY_APOSTROPHE", SCOPE_MAIN },
    { "Backquote", "KEY_GRAVE", SCOPE_MAIN },
    { "Comma", "KEY_COMMA", SCOPE_MAIN },
    { "Period", "KEY_DOT", SCOPE_MAIN },
    { "Slash", "KEY_SLASH", SCOPE_MAIN },

    /* Editing and whitespace. */
    { "Space", "KEY_SPACE", SCOPE_MAIN },
    { "Enter", "KEY_ENTER", SCOPE_MAIN },
    { "Tab", "KEY_TAB", SCOPE_MAIN },
    { "Backspace", "KEY_BACKSPACE", SCOPE_MAIN },
    { "Escape", "KEY_ESC", SCOPE_MAIN },
    { "CapsLock", "KEY_CAPSLOCK", SCOPE_MAIN },

    /* Modifiers. */
    { "ShiftLeft", "KEY_LEFTSHIFT", SCOPE_MAIN },
    { "ShiftRight", "KEY_RIGHTSHIFT", SCOPE_MAIN },
    { "ControlLeft", "KEY_LEFTCTRL", SCOPE_MAIN },
    { "ControlRight", "KEY_RIGHTCTRL", SCOPE_MAIN },
    { "AltLeft", "KEY_LEFTALT", SCOPE_MAIN },
    { "AltRight", "KEY_RIGHTALT", SCOPE_MAIN },
    { "MetaLeft", "KEY_LEFTMETA", SCOPE_MAIN },
    { "MetaRight", "KEY_RIGHTMETA", SCOPE_MAIN },

    /* Function row. */
    { "F1", "KEY_F1", SCOPE_MAIN },   { "F2", "KEY_F2", SCOPE_MAIN },
    { "F3", "KEY_F3", SCOPE_MAIN },   { "F4", "KEY_F4", SCOPE_MAIN },
    { "F5", "KEY_F5", SCOPE_MAIN },   { "F6", "KEY_F6", SCOPE_MAIN },
    { "F7", "KEY_F7", SCOPE_MAIN },   { "F8", "KEY_F8", SCOPE_MAIN },
    { "F9", "KEY_F9", SCOPE_MAIN },   { "F10", "KEY_F10", SCOPE_MAIN },
    { "F11", "KEY_F11", SCOPE_MAIN }, { "F12", "KEY_F12", SCOPE_MAIN },

    /* Navigation cluster. */
    { "PrintScreen", "KEY_SYSRQ", SCOPE_MAIN },
    { "ScrollLock", "KEY_SCROLLLOCK", SCOPE_MAIN },
    { "Pause", "KEY_PAUSE", SCOPE_MAIN },
    { "Insert", "KEY_INSERT", SCOPE_MAIN },
    { "Delete", "KEY_DELETE", SCOPE_MAIN },
    { "Home", "KEY_HOME", SCOPE_MAIN },
    { "End", "KEY_END", SCOPE_MAIN },
    { "PageUp", "KEY_PAGEUP", SCOPE_MAIN },
    { "PageDown", "KEY_PAGEDOWN", SCOPE_MAIN },
    { "ArrowUp", "KEY_UP", SCOPE_MAIN },
    { "ArrowDown", "KEY_DOWN", SCOPE_MAIN },
    { "ArrowLeft", "KEY_LEFT", SCOPE_MAIN },
    { "ArrowRight", "KEY_RIGHT", SCOPE_MAIN },

    /* Numpad. */
    { "NumLock", "KEY_NUMLOCK", SCOPE_MAIN },
    { "Numpad0", "KEY_KP0", SCOPE_MAIN }, { "Numpad1", "KEY_KP1", SCOPE_MAIN },
    { "Numpad2", "KEY_KP2", SCOPE_MAIN }, { "Numpad3", "KEY_KP3", SCOPE_MAIN },
    { "Numpad4", "KEY_KP4", SCOPE_MAIN }, { "Numpad5", "KEY_KP5", SCOPE_MAIN },
    { "Numpad6", "KEY_KP6", SCOPE_MAIN }, { "Numpad7", "KEY_KP7", SCOPE_MAIN },
    { "Numpad8", "KEY_KP8", SCOPE_MAIN }, { "Numpad9", "KEY_KP9", SCOPE_MAIN },
    { "NumpadDivide", "KEY_KPSLASH", SCOPE_MAIN },
    { "NumpadMultiply", "KEY_KPASTERISK", SCOPE_MAIN },
    { "NumpadSubtract", "KEY_KPMINUS", SCOPE_MAIN },
    { "NumpadAdd", "KEY_KPPLUS", SCOPE_MAIN },
    { "NumpadEnter", "KEY_KPENTER", SCOPE_MAIN },
    { "NumpadDecimal", "KEY_KPDOT", SCOPE_MAIN },

    /* --- Everything below is SCOPE_FULL: real keys, but ones a given
     * board may well not report, and any single one of them is enough to
     * disqualify that board from auto-discovery. --- */

    /* Layout-specific. ContextMenu is missing from most TKL and 60% boards;
     * the Intl* keys are ISO and JIS respectively. */
    { "ContextMenu", "KEY_COMPOSE", SCOPE_FULL },
    { "IntlBackslash", "KEY_102ND", SCOPE_FULL },
    { "IntlRo", "KEY_RO", SCOPE_FULL },
    { "IntlYen", "KEY_YEN", SCOPE_FULL },
    { "NumpadEqual", "KEY_KPEQUAL", SCOPE_FULL },
    { "NumpadComma", "KEY_KPCOMMA", SCOPE_FULL },

    /* F13-F24 exist in the packs because iohook has codes for them. */
    { "F13", "KEY_F13", SCOPE_FULL }, { "F14", "KEY_F14", SCOPE_FULL },
    { "F15", "KEY_F15", SCOPE_FULL }, { "F16", "KEY_F16", SCOPE_FULL },
    { "F17", "KEY_F17", SCOPE_FULL }, { "F18", "KEY_F18", SCOPE_FULL },
    { "F19", "KEY_F19", SCOPE_FULL }, { "F20", "KEY_F20", SCOPE_FULL },
    { "F21", "KEY_F21", SCOPE_FULL }, { "F22", "KEY_F22", SCOPE_FULL },
    { "F23", "KEY_F23", SCOPE_FULL }, { "F24", "KEY_F24", SCOPE_FULL },

    /* Japanese input keys. */
    { "Convert", "KEY_HENKAN", SCOPE_FULL },
    { "NonConvert", "KEY_MUHENKAN", SCOPE_FULL },
    { "KanaMode", "KEY_KATAKANAHIRAGANA", SCOPE_FULL },
    { "Lang1", "KEY_HANGEUL", SCOPE_FULL },
    { "Lang2", "KEY_HANJA", SCOPE_FULL },

    /* Media and system keys. On most boards these live on an Fn layer and
     * are reported by a SEPARATE consumer-control device, which is exactly
     * the case where binding them costs the real keyboard. */
    { "AudioVolumeMute", "KEY_MUTE", SCOPE_FULL },
    { "AudioVolumeDown", "KEY_VOLUMEDOWN", SCOPE_FULL },
    { "AudioVolumeUp", "KEY_VOLUMEUP", SCOPE_FULL },
    { "MediaTrackNext", "KEY_NEXTSONG", SCOPE_FULL },
    { "MediaTrackPrevious", "KEY_PREVIOUSSONG", SCOPE_FULL },
    { "MediaStop", "KEY_STOPCD", SCOPE_FULL },
    { "MediaPlayPause", "KEY_PLAYPAUSE", SCOPE_FULL },
    { "MediaSelect", "KEY_MEDIA", SCOPE_FULL },
    { "LaunchMail", "KEY_MAIL", SCOPE_FULL },
    { "LaunchApp1", "KEY_COMPUTER", SCOPE_FULL },
    { "LaunchApp2", "KEY_CALC", SCOPE_FULL },
    { "BrowserHome", "KEY_HOMEPAGE", SCOPE_FULL },
    { "BrowserSearch", "KEY_SEARCH", SCOPE_FULL },
    { "BrowserFavorites", "KEY_BOOKMARKS", SCOPE_FULL },
    { "BrowserRefresh", "KEY_REFRESH", SCOPE_FULL },
    { "BrowserStop", "KEY_STOP", SCOPE_FULL },
    { "BrowserForward", "KEY_FORWARD", SCOPE_FULL },
    { "BrowserBack", "KEY_BACK", SCOPE_FULL },
    { "Power", "KEY_POWER", SCOPE_FULL },
    { "Sleep", "KEY_SLEEP", SCOPE_FULL },
    { "WakeUp", "KEY_WAKEUP", SCOPE_FULL },

    /* The Sun-derived editing keys iohook still carries codes for. */
    { "Help", "KEY_HELP", SCOPE_FULL },
    { "Undo", "KEY_UNDO", SCOPE_FULL },
    { "Cut", "KEY_CUT", SCOPE_FULL },
    { "Copy", "KEY_COPY", SCOPE_FULL },
    { "Paste", "KEY_PASTE", SCOPE_FULL },
    { "Find", "KEY_FIND", SCOPE_FULL },
    { "Again", "KEY_AGAIN", SCOPE_FULL },
    { "Props", "KEY_PROPS", SCOPE_FULL },
    { "Front", "KEY_FRONT", SCOPE_FULL },
    { "Open", "KEY_OPEN", SCOPE_FULL },
};

/* Classic Mechvibes keys its `defines` by an iohook virtual code, which is
 * an AT set-1 scancode: 1-88 are numerically what Linux evdev uses, and
 * extended keys carry a prefix.
 *
 * BOTH prefixes are here because both are in the wild. Modern iohook uses
 * 0xE000|scancode (57399 for PrintScreen); older Mechvibes packs, and the
 * editor that produced most of them, wrote 0xE00|scancode (3675 for the left
 * Meta key). A table that covers one and not the other loses a pack's whole
 * navigation cluster silently, which is the failure this is written out
 * longhand to avoid.
 *
 * Derived from the scancodes rather than copied from mechvibes-dx's own
 * converter, whose table inserts 3597, 3612, 3613, 3675 and 3676 twice into
 * a HashMap - so its ControlRight, NumpadEnter and NumpadDivide entries are
 * overwritten by a later "alternative range" block and never take effect. */
typedef struct { unsigned code; const char *w3c; } iohook_t;

static const iohook_t g_iohook[] = {
    /* Base set-1 range: identical to Linux evdev codes. */
    { 1, "Escape" }, { 2, "Digit1" }, { 3, "Digit2" }, { 4, "Digit3" },
    { 5, "Digit4" }, { 6, "Digit5" }, { 7, "Digit6" }, { 8, "Digit7" },
    { 9, "Digit8" }, { 10, "Digit9" }, { 11, "Digit0" }, { 12, "Minus" },
    { 13, "Equal" }, { 14, "Backspace" }, { 15, "Tab" }, { 16, "KeyQ" },
    { 17, "KeyW" }, { 18, "KeyE" }, { 19, "KeyR" }, { 20, "KeyT" },
    { 21, "KeyY" }, { 22, "KeyU" }, { 23, "KeyI" }, { 24, "KeyO" },
    { 25, "KeyP" }, { 26, "BracketLeft" }, { 27, "BracketRight" },
    { 28, "Enter" }, { 29, "ControlLeft" }, { 30, "KeyA" }, { 31, "KeyS" },
    { 32, "KeyD" }, { 33, "KeyF" }, { 34, "KeyG" }, { 35, "KeyH" },
    { 36, "KeyJ" }, { 37, "KeyK" }, { 38, "KeyL" }, { 39, "Semicolon" },
    { 40, "Quote" }, { 41, "Backquote" }, { 42, "ShiftLeft" },
    { 43, "Backslash" }, { 44, "KeyZ" }, { 45, "KeyX" }, { 46, "KeyC" },
    { 47, "KeyV" }, { 48, "KeyB" }, { 49, "KeyN" }, { 50, "KeyM" },
    { 51, "Comma" }, { 52, "Period" }, { 53, "Slash" }, { 54, "ShiftRight" },
    { 55, "NumpadMultiply" }, { 56, "AltLeft" }, { 57, "Space" },
    { 58, "CapsLock" }, { 59, "F1" }, { 60, "F2" }, { 61, "F3" },
    { 62, "F4" }, { 63, "F5" }, { 64, "F6" }, { 65, "F7" }, { 66, "F8" },
    { 67, "F9" }, { 68, "F10" }, { 69, "NumLock" }, { 70, "ScrollLock" },
    { 71, "Numpad7" }, { 72, "Numpad8" }, { 73, "Numpad9" },
    { 74, "NumpadSubtract" }, { 75, "Numpad4" }, { 76, "Numpad5" },
    { 77, "Numpad6" }, { 78, "NumpadAdd" }, { 79, "Numpad1" },
    { 80, "Numpad2" }, { 81, "Numpad3" }, { 82, "Numpad0" },
    { 83, "NumpadDecimal" }, { 86, "IntlBackslash" }, { 87, "F11" },
    { 88, "F12" }, { 91, "F13" }, { 92, "F14" }, { 93, "F15" },
    { 99, "F16" }, { 100, "F17" }, { 101, "F18" }, { 102, "F19" },
    { 103, "F20" }, { 104, "F21" }, { 105, "F22" }, { 106, "F23" },
    { 107, "F24" }, { 112, "KanaMode" }, { 115, "IntlRo" },
    { 121, "Convert" }, { 123, "NonConvert" }, { 125, "IntlYen" },
    { 126, "NumpadComma" },

    /* Extended keys, 0xE00 | scancode - what the Mechvibes editor wrote. */
    { 0xE00 | 0x1C, "NumpadEnter" }, { 0xE00 | 0x1D, "ControlRight" },
    { 0xE00 | 0x35, "NumpadDivide" }, { 0xE00 | 0x37, "PrintScreen" },
    { 0xE00 | 0x38, "AltRight" }, { 0xE00 | 0x45, "Pause" },
    { 0xE00 | 0x47, "Home" }, { 0xE00 | 0x48, "ArrowUp" },
    { 0xE00 | 0x49, "PageUp" }, { 0xE00 | 0x4B, "ArrowLeft" },
    { 0xE00 | 0x4D, "ArrowRight" }, { 0xE00 | 0x4F, "End" },
    { 0xE00 | 0x50, "ArrowDown" }, { 0xE00 | 0x51, "PageDown" },
    { 0xE00 | 0x52, "Insert" }, { 0xE00 | 0x53, "Delete" },
    { 0xE00 | 0x5B, "MetaLeft" }, { 0xE00 | 0x5C, "MetaRight" },
    { 0xE00 | 0x5D, "ContextMenu" },

    /* Extended keys, 0xE000 | scancode - what libuiohook reports. */
    { 0xE000 | 0x1C, "NumpadEnter" }, { 0xE000 | 0x1D, "ControlRight" },
    { 0xE000 | 0x35, "NumpadDivide" }, { 0xE000 | 0x37, "PrintScreen" },
    { 0xE000 | 0x38, "AltRight" }, { 0xE000 | 0x45, "Pause" },
    { 0xE000 | 0x47, "Home" },
    { 0xE000 | 0x48, "ArrowUp" }, { 0xE000 | 0x49, "PageUp" },
    { 0xE000 | 0x4B, "ArrowLeft" }, { 0xE000 | 0x4D, "ArrowRight" },
    { 0xE000 | 0x4F, "End" }, { 0xE000 | 0x50, "ArrowDown" },
    { 0xE000 | 0x51, "PageDown" }, { 0xE000 | 0x52, "Insert" },
    { 0xE000 | 0x53, "Delete" }, { 0xE000 | 0x5B, "MetaLeft" },
    { 0xE000 | 0x5C, "MetaRight" }, { 0xE000 | 0x5D, "ContextMenu" },
    { 0xE000 | 0x10, "MediaTrackPrevious" },
    { 0xE000 | 0x19, "MediaTrackNext" }, { 0xE000 | 0x20, "AudioVolumeMute" },
    { 0xE000 | 0x22, "MediaPlayPause" }, { 0xE000 | 0x24, "MediaStop" },
    { 0xE000 | 0x2E, "AudioVolumeDown" }, { 0xE000 | 0x30, "AudioVolumeUp" },
};

static const keymap_t *key_by_w3c(const char *w3c) {
    for (size_t i = 0; i < ARRAY_LEN(g_keys); i++)
        if (strcmp(g_keys[i].w3c, w3c) == 0) return &g_keys[i];
    return NULL;
}

static const char *w3c_by_iohook(unsigned code) {
    for (size_t i = 0; i < ARRAY_LEN(g_iohook); i++)
        if (g_iohook[i].code == code) return g_iohook[i].w3c;
    return NULL;
}

/* The evdev code behind a table entry. Resolved through libevdev so the
 * names this tool emits are exactly the names evclack will accept. */
static int key_code(const keymap_t *k) {
    return libevdev_event_code_from_name(EV_KEY, k->evdev);
}

/* ------------------------------------------------------------------------- */
/* Output bindings                                                           */
/* ------------------------------------------------------------------------- */

typedef struct {
    const keymap_t *key;
    char           *path;    /* absolute */
    double          start_ms, end_ms;
} outbind_t;

typedef struct {
    outbind_t *v;
    size_t     n, cap;
} binds_t;

/* Last write wins: a pack that names a key twice (a `-up` variant, or both
 * iohook prefixes for the same key) should not produce a config evclack
 * rejects for binding it twice. */
static void binds_set(binds_t *b, const keymap_t *k, char *path,
                      double start_ms, double end_ms) {
    for (size_t i = 0; i < b->n; i++) {
        if (b->v[i].key == k) {
            free(b->v[i].path);
            b->v[i].path = path;
            b->v[i].start_ms = start_ms;
            b->v[i].end_ms = end_ms;
            return;
        }
    }
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->v = realloc(b->v, b->cap * sizeof(*b->v));
        if (!b->v) dief("out of memory");
    }
    b->v[b->n++] = (outbind_t){ k, path, start_ms, end_ms };
}

static void binds_free(binds_t *b) {
    for (size_t i = 0; i < b->n; i++) free(b->v[i].path);
    free(b->v);
    memset(b, 0, sizeof(*b));
}

/* ------------------------------------------------------------------------- */
/* Files                                                                     */
/* ------------------------------------------------------------------------- */

/* Every name in the pack directory, read once. Both formats need
 * case-insensitive lookup: osu! skins are authored on Windows and ship
 * `Key-Press-1.WAV` as readily as `key-press-1.wav`, and a Mechvibes
 * `audio_file` is just as likely to disagree with the file's real case.
 * On Linux that is the difference between a working import and a silent
 * one. */
typedef struct {
    char  *dir;      /* absolute */
    char **name;
    size_t n, cap;
} dirlist_t;

static void dirlist_load(dirlist_t *d, const char *dir) {
    memset(d, 0, sizeof(*d));
    char *abs = realpath(dir, NULL);
    if (!abs) dief("%s: %s", dir, strerror(errno));
    d->dir = abs;

    DIR *dp = opendir(abs);
    if (!dp) dief("%s: %s", abs, strerror(errno));
    struct dirent *e;
    while ((e = readdir(dp))) {
        if (e->d_name[0] == '.') continue;
        if (d->n == d->cap) {
            d->cap = d->cap ? d->cap * 2 : 64;
            d->name = realloc(d->name, d->cap * sizeof(*d->name));
            if (!d->name) dief("out of memory");
        }
        d->name[d->n++] = xstrdup(e->d_name);
    }
    closedir(dp);
}

static void dirlist_free(dirlist_t *d) {
    for (size_t i = 0; i < d->n; i++) free(d->name[i]);
    free(d->name);
    free(d->dir);
    memset(d, 0, sizeof(*d));
}

static bool dirlist_has(const dirlist_t *d, const char *name) {
    for (size_t i = 0; i < d->n; i++)
        if (strcasecmp(d->name[i], name) == 0) return true;
    return false;
}

/* Resolve `name` to an absolute path, matching case-insensitively. Returns
 * NULL if nothing in the directory matches. */
static char *dirlist_path(const dirlist_t *d, const char *name) {
    for (size_t i = 0; i < d->n; i++) {
        if (strcasecmp(d->name[i], name) != 0) continue;
        size_t n = strlen(d->dir) + 1 + strlen(d->name[i]) + 1;
        char *p = xmalloc(n);
        snprintf(p, n, "%s/%s", d->dir, d->name[i]);
        return p;
    }
    return NULL;
}

/* osu! names a sound without an extension and accepts three. Its own
 * priority puts .wav first, and a skin that ships both means the .wav. */
static char *dirlist_sound(const dirlist_t *d, const char *stem) {
    static const char *ext[] = { ".wav", ".mp3", ".ogg" };
    for (size_t i = 0; i < ARRAY_LEN(ext); i++) {
        char name[256];
        snprintf(name, sizeof name, "%s%s", stem, ext[i]);
        char *p = dirlist_path(d, name);
        if (p) return p;
    }
    return NULL;
}

/* Duration in milliseconds, or -1 if the file will not open. 0 means the
 * file opened but holds no audio - which osu! skins use DELIBERATELY, as a
 * silent placeholder that mutes a sound the default skin would otherwise
 * provide. Binding one is a key that mysteriously does nothing, so they are
 * skipped rather than imported. */
static double sound_duration_ms(const char *path) {
    SF_INFO si = { 0 };
    SNDFILE *sf = sf_open(path, SFM_READ, &si);
    if (!sf) return -1.0;
    double ms = (si.samplerate > 0 && si.frames > 0)
                    ? (double)si.frames * 1000.0 / (double)si.samplerate
                    : 0.0;
    sf_close(sf);
    return ms;
}

/* ------------------------------------------------------------------------- */
/* JSON                                                                      */
/* ------------------------------------------------------------------------- */

/* JSON is read with libyaml rather than a JSON library, because JSON is a
 * subset of what libyaml's document API already accepts and libyaml is
 * already a dependency of this project. Verified against the config.json of
 * the packs mechvibes-dx ships, including the \/ and \uXXXX escapes YAML's
 * double-quoted scalars share with JSON: the parse is identical to a real
 * JSON parser's, key for key. Adding a JSON dependency to read four fields
 * would be the more expensive of the two mistakes available here. */
static yaml_node_t *ynode(yaml_document_t *doc, int id) {
    return id ? yaml_document_get_node(doc, id) : NULL;
}

static yaml_node_t *jget(yaml_document_t *doc, yaml_node_t *map,
                         const char *key) {
    if (!map || map->type != YAML_MAPPING_NODE) return NULL;
    for (yaml_node_pair_t *p = map->data.mapping.pairs.start;
         p < map->data.mapping.pairs.top; p++) {
        yaml_node_t *k = ynode(doc, p->key);
        if (k && k->type == YAML_SCALAR_NODE &&
            strcmp((const char *)k->data.scalar.value, key) == 0)
            return ynode(doc, p->value);
    }
    return NULL;
}

static const char *jstr(yaml_node_t *n) {
    return (n && n->type == YAML_SCALAR_NODE)
               ? (const char *)n->data.scalar.value : NULL;
}

static bool jnum(yaml_node_t *n, double *out) {
    const char *s = jstr(n);
    if (!s) return false;
    char *end = NULL;
    errno = 0;
    double d = strtod(s, &end);
    if (end == s || *end != '\0' || errno != 0 || !isfinite(d)) return false;
    *out = d;
    return true;
}

/* The i'th element of a sequence, or NULL. */
static yaml_node_t *jat(yaml_document_t *doc, yaml_node_t *seq, size_t i) {
    if (!seq || seq->type != YAML_SEQUENCE_NODE) return NULL;
    size_t n = (size_t)(seq->data.sequence.items.top -
                        seq->data.sequence.items.start);
    if (i >= n) return NULL;
    return ynode(doc, seq->data.sequence.items.start[i]);
}

static size_t jlen(yaml_node_t *seq) {
    if (!seq || seq->type != YAML_SEQUENCE_NODE) return 0;
    return (size_t)(seq->data.sequence.items.top -
                    seq->data.sequence.items.start);
}

/* ------------------------------------------------------------------------- */
/* YAML output                                                               */
/* ------------------------------------------------------------------------- */

/* Print `v` at the shortest precision that reads back as exactly `v`.
 *
 * This is not cosmetic. evclack interns a sample on the whole of
 * (path, gain, start, end) and a reload asks whether the new set is the same
 * one; both compare the slice bounds exactly. A bound printed as 45832.5000
 * when the pack said 45832.5 still round-trips, but one printed at a
 * precision that loses a digit turns two identical slices into two samples,
 * or - worse - makes an unchanged config look changed on every save. */
static void fmt_num(char *buf, size_t n, double v) {
    /* Plain decimal, never an exponent: "4.575e+04" round-trips perfectly
     * well and is still the wrong thing to put in a file whose whole point
     * is that a human can read a key's line and retune it. */
    for (int p = 0; p <= 9; p++) {
        snprintf(buf, n, "%.*f", p, v);
        if (strtod(buf, NULL) == v) return;
    }
    snprintf(buf, n, "%.17g", v);
}

/* Pack directory names routinely contain spaces and apostrophes ("Super
 * Paper Mario Talk"), so paths are always double-quoted. */
static void emit_quoted(FILE *f, const char *s) {
    fputc('"', f);
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', f);
        fputc(*p, f);
    }
    fputc('"', f);
}

static void emit_config(FILE *f, const binds_t *b, const char *source,
                        const char *pack_name, float gain, int scope,
                        size_t skipped) {
    fprintf(f, "# evclack configuration, generated by evclack-import.\n"
               "#\n"
               "#   pack:   %s\n"
               "#   source: %s\n"
               "#   keys:   %zu bound",
            pack_name && *pack_name ? pack_name : "(unnamed)", source, b->n);
    if (skipped)
        fprintf(f, ", %zu skipped", skipped);
    fprintf(f, " (scope: %s)\n", scope == SCOPE_FULL ? "full" : "main");
    fprintf(f,
        "#\n"
        "# This file REPLACED whatever was here before, so any 'devices:'\n"
        "# list or 'audio.latency' you had set by hand is not preserved -\n"
        "# add it back below and it will survive the next reload.\n"
        "#\n"
        "# Editing this file reloads the running daemon; nothing needs\n"
        "# restarting. Every binding names its own file, so you can retune\n"
        "# or unbind a single key by editing its line.\n"
        "#\n"
        "# If evclack goes SILENT after this import, look in its log for a\n"
        "# 'Not listening to ...' warning: auto-discovery only opens a\n"
        "# keyboard that reports EVERY bound key, so one key your board does\n"
        "# not have is enough to disqualify it. The warning names the key -\n"
        "# delete that key's line here and save.\n"
        "\n");

    /* Left commented so auto-discovery stays the default, and so the line
     * is there to uncomment rather than to remember. */
    fprintf(f, "# devices:\n"
               "#   - /dev/input/by-id/your-keyboard-event-kbd\n"
               "\n");

    fprintf(f, "keys:\n");
    for (size_t i = 0; i < b->n; i++) {
        const outbind_t *o = &b->v[i];
        /* The comma goes inside the padded field, so the sample column
         * stays straight down a hundred lines of varying key names. */
        char kf[32];
        snprintf(kf, sizeof kf, "%s,", o->key->evdev);
        fprintf(f, "  - {key: %-18s sample: ", kf);
        emit_quoted(f, o->path);
        if (o->start_ms > 0.0 || o->end_ms > 0.0) {
            char sb[32], eb[32];
            fmt_num(sb, sizeof sb, o->start_ms);
            fmt_num(eb, sizeof eb, o->end_ms);
            fprintf(f, ", start: %s, end: %s", sb, eb);
        }
        fprintf(f, "}\n");
    }

    char gb[32];
    fmt_num(gb, sizeof gb, (double)gain);
    fprintf(f, "\naudio:\n"
               "  enabled: true\n"
               "  gain: %s\n"
               "  latency: 256\n", gb);
}

/* ------------------------------------------------------------------------- */
/* Mechvibes                                                                 */
/* ------------------------------------------------------------------------- */

/* A `{0-4}` range in a v2 filename means the pack ships several takes of the
 * sound and the app picks one per keystroke. evclack has no per-key variant
 * list, so the choice is made here and made DETERMINISTICALLY, spread across
 * the keyboard by key code: a typed word still varies audibly, and it varies
 * the same way every time. Picking at random per key would be no better and
 * would make the generated config unreproducible.
 *
 * Returns a newly allocated name with the range replaced. */
static char *expand_range(const char *name, int code) {
    const char *open = strchr(name, '{');
    if (!open) return xstrdup(name);
    const char *dash = strchr(open, '-');
    const char *close = dash ? strchr(dash, '}') : NULL;
    if (!dash || !close) return xstrdup(name);

    int lo = atoi(open + 1), hi = atoi(dash + 1);
    if (hi < lo) return xstrdup(name);
    int pick = lo + (code % (hi - lo + 1));

    size_t n = strlen(name) + 16;
    char *out = xmalloc(n);
    snprintf(out, n, "%.*s%d%s", (int)(open - name), name, pick, close + 1);
    return out;
}

/* Classic Mechvibes encodes a key RELEASE as the press code with a leading
 * zero ("01" for the release of "1"). evclack is presses-only, so those are
 * dropped - but "01" parses as 1, so parsing the key naively would make a
 * release silently overwrite its own press. */
static bool iohook_is_press(const char *key) {
    return !(strlen(key) > 1 && key[0] == '0');
}

static void import_mechvibes(const dirlist_t *d, binds_t *out, int scope,
                             float *gain_out, char **name_out,
                             size_t *skipped) {
    char *cfg_path = dirlist_path(d, "config.json");
    if (!cfg_path) dief("%s: no config.json", d->dir);

    FILE *f = fopen(cfg_path, "rb");
    if (!f) dief("%s: %s", cfg_path, strerror(errno));

    yaml_parser_t parser;
    yaml_document_t doc;
    if (!yaml_parser_initialize(&parser)) dief("yaml_parser_initialize");
    yaml_parser_set_input_file(&parser, f);
    if (!yaml_parser_load(&parser, &doc))
        dief("%s: line %zu: %s", cfg_path,
             (size_t)parser.problem_mark.line + 1,
             parser.problem ? parser.problem : "parse error");
    fclose(f);

    yaml_node_t *root = yaml_document_get_root_node(&doc);
    if (!root || root->type != YAML_MAPPING_NODE)
        dief("%s: not a JSON object", cfg_path);

    const char *nm = jstr(jget(&doc, root, "name"));
    if (nm) *name_out = xstrdup(nm);

    yaml_node_t *opts = jget(&doc, root, "options");
    double vol;
    if (opts && jnum(jget(&doc, opts, "recommended_volume"), &vol) &&
        vol > 0.0)
        *gain_out = (float)vol;

    /* Which generation? DX writes `definitions` and `definition_method`;
     * classic Mechvibes writes `defines` and `key_define_type`. */
    yaml_node_t *defs = jget(&doc, root, "definitions");
    if (!defs) defs = jget(&doc, root, "defs");
    bool dx = defs != NULL;
    if (!defs) defs = jget(&doc, root, "defines");
    if (!defs || defs->type != YAML_MAPPING_NODE)
        dief("%s: no 'definitions' or 'defines' object", cfg_path);

    const char *method = jstr(jget(&doc, root, "definition_method"));
    if (!method) method = jstr(jget(&doc, root, "key_define_type"));
    bool single = method && strcmp(method, "single") == 0;

    const char *audio = jstr(jget(&doc, root, "audio_file"));
    if (!audio) audio = jstr(jget(&doc, root, "sound"));

    char *sheet = NULL;
    if (single) {
        if (!audio) dief("%s: 'single' pack with no audio file named",
                         cfg_path);
        sheet = dirlist_path(d, audio);
        if (!sheet) dief("%s: names %s, which is not in the pack",
                         cfg_path, audio);
    }
    double sheet_ms = sheet ? sound_duration_ms(sheet) : -1.0;
    if (sheet && sheet_ms <= 0.0)
        dief("%s: will not decode", sheet);

    size_t mouse_skipped = 0, unknown = 0, overrun = 0;

    /* Mechvibes V1 had no mouse packs of its own, so the ones the community
     * built were authored in the KEYBOARD editor: the left button recorded
     * against iohook code 1, the right against 2, the middle against 3.
     * Read through the keyboard table those are Escape, Digit1 and Digit2 -
     * so a mouse pack imports cleanly and then binds three keys to the wrong
     * sounds, which is worse than refusing it. A pack whose every definition
     * falls in 1..3 is that pack; a real keyboard pack that happened to
     * define only Escape, 1 and 2 does not exist. */
    if (!dx) {
        bool all_low = true;
        size_t n_defs = 0;
        for (yaml_node_pair_t *p = defs->data.mapping.pairs.start;
             p < defs->data.mapping.pairs.top; p++) {
            const char *raw = jstr(ynode(&doc, p->key));
            if (!raw || !iohook_is_press(raw)) continue;
            n_defs++;
            unsigned long code = strtoul(raw, NULL, 10);
            if (code < 1 || code > 3) { all_low = false; break; }
        }
        if (all_low && n_defs > 0)
            dief("%s looks like a Mechvibes mouse pack (it defines only "
                 "iohook 1-3, which are the mouse buttons the V1 editor "
                 "recorded them against). evclack never opens a pointing "
                 "device, so there is nothing here it can play.", cfg_path);
    }

    for (yaml_node_pair_t *p = defs->data.mapping.pairs.start;
         p < defs->data.mapping.pairs.top; p++) {
        yaml_node_t *kn = ynode(&doc, p->key);
        yaml_node_t *vn = ynode(&doc, p->value);
        const char *raw = jstr(kn);
        if (!raw || !vn) continue;

        /* Releases are not a second strike in evclack: `14-up` in DX, and a
         * zero-padded code in classic packs. */
        if (strstr(raw, "-up")) continue;

        const char *w3c;
        if (dx) {
            w3c = raw;
        } else {
            if (!iohook_is_press(raw)) continue;
            char *end = NULL;
            unsigned long code = strtoul(raw, &end, 10);
            if (end == raw || *end != '\0') continue;
            w3c = w3c_by_iohook((unsigned)code);
            if (!w3c) { unknown++; continue; }
        }

        if (strncmp(w3c, "Mouse", 5) == 0 || strncmp(w3c, "Button", 6) == 0 ||
            strncmp(w3c, "Wheel", 5) == 0) { mouse_skipped++; continue; }

        const keymap_t *k = key_by_w3c(w3c);
        if (!k) { unknown++; continue; }
        if (k->scope > scope) { (*skipped)++; continue; }
        int code = key_code(k);
        if (code < 0) { unknown++; continue; }

        if (single) {
            double start = 0.0, end = 0.0;
            if (dx) {
                /* timing is [[start_ms, end_ms], ...]. Length 2 is
                 * [keydown, keyup] and length 1 is keydown-only - NOT random
                 * variants, whatever the DX docs say; its engine indexes the
                 * array by press-or-release. evclack plays presses, so the
                 * first pair is always the one. */
                yaml_node_t *t = jget(&doc, vn, "timing");
                yaml_node_t *pair = jat(&doc, t, 0);
                if (jlen(pair) < 2) continue;
                if (!jnum(jat(&doc, pair, 0), &start) ||
                    !jnum(jat(&doc, pair, 1), &end)) continue;
            } else {
                /* Classic v1 is [start_ms, LENGTH_ms] - a length, not an
                 * end. The DX packs on disk look like halves of these
                 * because an early converter split each region down the
                 * middle into a fake keydown/keyup pair; the region itself
                 * is the whole sound and must not be halved again here. */
                double len;
                if (jlen(vn) < 2) continue;
                if (!jnum(jat(&doc, vn, 0), &start) ||
                    !jnum(jat(&doc, vn, 1), &len)) continue;
                end = start + len;
            }
            if (end <= start) continue;
            if (start >= sheet_ms) { overrun++; continue; }
            binds_set(out, k, xstrdup(sheet), start, end);
        } else {
            /* Multi: the value is a filename, either bare or wrapped in an
             * object with an `audio_file`. */
            const char *file = dx ? jstr(jget(&doc, vn, "audio_file"))
                                  : jstr(vn);
            if (!file || !*file) continue;
            char *expanded = expand_range(file, code);
            char *path = dirlist_path(d, expanded);
            free(expanded);
            if (!path) { unknown++; continue; }
            binds_set(out, k, path, 0.0, 0.0);
        }
    }

    /* In MULTI mode a pack's `sound` is the sound for everything it did not
     * name, so keys the pack left out still clack. In SINGLE mode there is
     * no such fallback to give them: the file is a sprite sheet, and playing
     * all fifty seconds of it would be worse than silence. */
    if (!single && audio) {
        /* Resolved per key rather than probed once, because `sound` is
         * itself allowed to carry a {0-4} range - the fallback for a v2 pack
         * is usually "generic{0-4}.mp3", which names no file until the range
         * is expanded. */
        for (size_t i = 0; i < ARRAY_LEN(g_keys); i++) {
            const keymap_t *k = &g_keys[i];
            if (k->scope > scope) continue;
            int code = key_code(k);
            if (code < 0) continue;
            bool have = false;
            for (size_t j = 0; j < out->n; j++)
                if (out->v[j].key == k) { have = true; break; }
            if (have) continue;
            char *expanded = expand_range(audio, code);
            char *path = dirlist_path(d, expanded);
            free(expanded);
            if (path) binds_set(out, k, path, 0.0, 0.0);
        }
    }

    if (mouse_skipped)
        warnf("skipped %zu mouse binding(s): evclack never opens a pointing "
              "device", mouse_skipped);
    if (unknown)
        warnf("skipped %zu definition(s) with no Linux key to bind them to",
              unknown);
    if (overrun)
        warnf("skipped %zu slice(s) starting past the end of %s",
              overrun, audio);

    free(sheet);
    free(cfg_path);
    yaml_document_delete(&doc);
    yaml_parser_delete(&parser);
}

/* ------------------------------------------------------------------------- */
/* osu!                                                                      */
/* ------------------------------------------------------------------------- */

enum { OSU_AUTO = 0, OSU_TYPING, OSU_HITSOUNDS, OSU_MENU };

/* skin.ini, read for exactly two fields. Parsed leniently on purpose: skins
 * are hand-edited on Windows, so a BOM, CRLF endings, stray spaces around
 * the '=' and a missing file are all ordinary rather than errors. */
static void read_skin_ini(const dirlist_t *d, char **name, char **sampleset) {
    char *path = dirlist_path(d, "skin.ini");
    if (!path) return;
    FILE *f = fopen(path, "rb");
    free(path);
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        /* A UTF-8 BOM on the first line would otherwise hide the key. */
        if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
            (unsigned char)s[2] == 0xBF) s += 3;
        char *nl = strpbrk(s, "\r\n");
        if (nl) *nl = '\0';

        char *eq = strchr(s, ':');
        if (!eq) continue;
        *eq = '\0';
        char *k = s, *v = eq + 1;
        while (*k == ' ' || *k == '\t') k++;
        while (*v == ' ' || *v == '\t') v++;
        for (char *e = k + strlen(k); e > k && isspace((unsigned char)e[-1]); )
            *--e = '\0';
        for (char *e = v + strlen(v); e > v && isspace((unsigned char)e[-1]); )
            *--e = '\0';

        if (!*name && strcasecmp(k, "Name") == 0 && *v) *name = xstrdup(v);
        if (!*sampleset && strcasecmp(k, "SampleSet") == 0 && *v)
            *sampleset = xstrdup(v);
    }
    fclose(f);
}

/* A skin's sound, skipping the silent placeholders. Returns NULL if the file
 * is missing, will not decode, or holds no audio. */
static char *osu_sound(const dirlist_t *d, const char *stem) {
    char *p = dirlist_sound(d, stem);
    if (!p) return NULL;
    double ms = sound_duration_ms(p);
    if (ms <= 0.0) {
        free(p);
        return NULL;
    }
    return p;
}

/* Which keys get which of osu!'s typing sounds.
 *
 * This is the whole answer to "how do we decide which sounds and which
 * keys", and the answer is that osu! already decided. A skin does not only
 * carry hitsounds; it carries a purpose-built set for typing into osu!'s own
 * chat and search boxes, with a sound for an ordinary keypress, one for
 * sending, one for deleting and one for moving the cursor. Reusing that is
 * not an interpretation of the skin, it is the skin used for what it is
 * for. */
static const char *osu_typing_stem(const char *evdev, int code, int nvariants,
                                   char *buf, size_t bufn) {
    if (strcmp(evdev, "KEY_ENTER") == 0 || strcmp(evdev, "KEY_KPENTER") == 0)
        return "key-confirm";
    if (strcmp(evdev, "KEY_BACKSPACE") == 0 ||
        strcmp(evdev, "KEY_DELETE") == 0)
        return "key-delete";
    if (strcmp(evdev, "KEY_UP") == 0 || strcmp(evdev, "KEY_DOWN") == 0 ||
        strcmp(evdev, "KEY_LEFT") == 0 || strcmp(evdev, "KEY_RIGHT") == 0 ||
        strcmp(evdev, "KEY_TAB") == 0 || strcmp(evdev, "KEY_HOME") == 0 ||
        strcmp(evdev, "KEY_END") == 0 || strcmp(evdev, "KEY_PAGEUP") == 0 ||
        strcmp(evdev, "KEY_PAGEDOWN") == 0)
        return "key-movement";

    /* Spread the takes across the keyboard by key code rather than picking
     * one. osu! chooses at random per keystroke; evclack has no per-key
     * variant list, and a fixed spread gets most of the effect - a typed
     * word moves across several takes - with no runtime machinery and a
     * config that is the same every time it is generated. */
    snprintf(buf, bufn, "key-press-%d", 1 + (code % nvariants));
    return buf;
}

static void import_osu(const dirlist_t *d, binds_t *out, int scope,
                       int profile, char **name_out, size_t *skipped) {
    char *sampleset = NULL;
    read_skin_ini(d, name_out, &sampleset);

    /* How many key-press takes this skin actually ships. They are numbered
     * from 1 and a skin may stop early. */
    int nvariants = 0;
    for (int i = 1; i <= 4; i++) {
        char stem[32];
        snprintf(stem, sizeof stem, "key-press-%d", i);
        char *p = osu_sound(d, stem);
        if (!p) break;
        free(p);
        nvariants = i;
    }

    if (profile == OSU_AUTO) {
        profile = nvariants > 0 ? OSU_TYPING : OSU_HITSOUNDS;
        if (profile == OSU_HITSOUNDS)
            warnf("this skin ships no key-press sounds; falling back to its "
                  "hitsounds (-p menu is the other option)");
    }
    if (profile == OSU_TYPING && nvariants == 0)
        dief("%s: no key-press sounds; try -p hitsounds or -p menu", d->dir);

    /* The sample set the skin itself nominates, which is the one osu! plays
     * unless a beatmap overrides it. */
    char set[32] = "normal";
    if (sampleset) {
        snprintf(set, sizeof set, "%s", sampleset);
        for (char *p = set; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (strcmp(set, "normal") && strcmp(set, "soft") && strcmp(set, "drum"))
            snprintf(set, sizeof set, "normal");
    }
    free(sampleset);

    size_t missing = 0;

    for (size_t i = 0; i < ARRAY_LEN(g_keys); i++) {
        const keymap_t *k = &g_keys[i];
        if (k->scope > scope) { (*skipped)++; continue; }
        int code = key_code(k);
        if (code < 0) continue;

        char buf[64];
        const char *stem = NULL;
        switch (profile) {
        case OSU_TYPING:
            stem = osu_typing_stem(k->evdev, code, nvariants, buf, sizeof buf);
            break;
        case OSU_MENU:
            stem = "menuhit";
            break;
        default: {
            /* Hitsounds. hitnormal is the sound of a note being hit and is
             * the one that reads as a keystroke; the accents go on the two
             * keys that are not ordinary typing. */
            const char *suffix = "hitnormal";
            if (strcmp(k->evdev, "KEY_ENTER") == 0 ||
                strcmp(k->evdev, "KEY_KPENTER") == 0) suffix = "hitfinish";
            else if (strcmp(k->evdev, "KEY_BACKSPACE") == 0 ||
                     strcmp(k->evdev, "KEY_DELETE") == 0) suffix = "hitclap";
            snprintf(buf, sizeof buf, "%s-%s", set, suffix);
            stem = buf;
            break;
        }
        }

        char *path = osu_sound(d, stem);

        /* osu! fills any file a skin omits from its DEFAULT skin, which is
         * not something evclack has, so every hole has to be filled from
         * inside the skin instead. Two of them are common enough to be worth
         * handling rather than reporting:
         *
         * a skin that ships key-press-1 but not the confirm/delete/movement
         * sounds; and a skin whose skin.ini nominates one sample set while
         * the files on disk are another - which happens constantly, because
         * shipping one set and leaning on osu! for the rest is normal. So
         * the nominated set is tried first and the other two after it. */
        if (!path && profile == OSU_TYPING)
            path = osu_sound(d, "key-press-1");
        if (!path) {
            static const char *sets[] = { "normal", "soft", "drum" };
            char alt[64];
            snprintf(alt, sizeof alt, "%s-hitnormal", set);
            path = osu_sound(d, alt);
            for (size_t j = 0; !path && j < ARRAY_LEN(sets); j++) {
                snprintf(alt, sizeof alt, "%s-hitnormal", sets[j]);
                path = osu_sound(d, alt);
            }
        }
        if (!path) { missing++; continue; }

        binds_set(out, k, path, 0.0, 0.0);
    }

    if (missing)
        warnf("%zu key(s) left unbound: the skin ships no usable sound for "
              "them", missing);
}

/* ------------------------------------------------------------------------- */
/* main                                                                      */
/* ------------------------------------------------------------------------- */

static void usage(FILE *f) {
    fprintf(f,
        "usage: %s [-p PROFILE] [-k SCOPE] [-g GAIN] [-o FILE] [-f] DIR\n"
        "\n"
        "Turn a Mechvibes soundpack or an osu! skin into an evclack config.\n"
        "DIR is the unpacked pack or skin directory.\n"
        "\n"
        "  -p PROFILE  osu! skins only: auto (default), typing, hitsounds, menu\n"
        "  -k SCOPE    which keys to bind: main (default) or full\n"
        "  -g GAIN     override the gain the pack asks for\n"
        "  -o FILE     write here instead of stdout\n"
        "  -f          overwrite an existing -o file\n"
        "  -h          this message\n"
        "\n"
        "  %s ~/packs/cherrymx-blue -o ~/.config/evclack/config.yaml\n"
        "\n"
        "The running daemon reloads on save; nothing needs restarting.\n",
        g_prog, g_prog);
}

/* .osk and the .zip a soundpack downloads as are both archives, and both
 * have to be unpacked before anything can point a config at the files
 * inside them. Rather than link an archive library to do one thing the
 * system already does, say so. */
static bool looks_like_archive(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char magic[2] = { 0 };
    size_t n = fread(magic, 1, 2, f);
    fclose(f);
    return n == 2 && magic[0] == 'P' && magic[1] == 'K';
}

int main(int argc, char **argv) {
    int   profile = OSU_AUTO, scope = SCOPE_MAIN, force = 0;
    float gain = 0.0f;   /* 0 = let the pack decide */
    const char *outpath = NULL;

    int c;
    while ((c = getopt(argc, argv, "hfp:k:g:o:")) != -1) {
        switch (c) {
        case 'p':
            if (!strcmp(optarg, "auto"))           profile = OSU_AUTO;
            else if (!strcmp(optarg, "typing"))    profile = OSU_TYPING;
            else if (!strcmp(optarg, "hitsounds")) profile = OSU_HITSOUNDS;
            else if (!strcmp(optarg, "menu"))      profile = OSU_MENU;
            else dief("unknown profile '%s'", optarg);
            break;
        case 'k':
            if (!strcmp(optarg, "main"))      scope = SCOPE_MAIN;
            else if (!strcmp(optarg, "full")) scope = SCOPE_FULL;
            else dief("unknown key scope '%s'", optarg);
            break;
        case 'g': {
            char *end = NULL;
            double d = strtod(optarg, &end);
            if (end == optarg || *end || d < 0.0) dief("bad gain '%s'", optarg);
            gain = (float)d;
            break;
        }
        case 'o': outpath = optarg; break;
        case 'f': force = 1; break;
        case 'h': usage(stdout); return 0;
        default:  usage(stderr); return 2;
        }
    }
    if (optind + 1 != argc) { usage(stderr); return 2; }
    const char *src = argv[optind];

    if (looks_like_archive(src))
        dief("%s is an archive, not a directory. Unpack it first:\n"
             "    unzip -q %s -d ~/.local/share/evclack/packs/NAME\n"
             "then point this at that directory.", src, src);

    dirlist_t dir;
    dirlist_load(&dir, src);

    binds_t binds = { 0 };
    char  *name = NULL;
    size_t skipped = 0;
    float  pack_gain = 1.0f;

    /* A config.json is what makes it a Mechvibes pack; anything else is
     * treated as a skin, since a skin has no manifest that must be there. */
    if (dirlist_has(&dir, "config.json")) {
        import_mechvibes(&dir, &binds, scope, &pack_gain, &name, &skipped);
        if (profile != OSU_AUTO)
            warnf("-p applies to osu! skins; ignored for a Mechvibes pack");
    } else {
        import_osu(&dir, &binds, scope, profile, &name, &skipped);
        /* osu! hitsounds are mastered loud for a game that plays one at a
         * time. A whole keyboard of them at unity is not what anyone wants
         * on the first try. */
        pack_gain = 0.6f;
    }

    if (binds.n == 0)
        dief("%s: nothing to bind - is this a soundpack or a skin?", dir.dir);
    if (gain > 0.0f) pack_gain = gain;

    FILE *out = stdout;
    if (outpath) {
        if (!force && access(outpath, F_OK) == 0)
            dief("%s already exists; pass -f to replace it", outpath);
        out = fopen(outpath, "w");
        if (!out) dief("%s: %s", outpath, strerror(errno));
    }

    emit_config(out, &binds, dir.dir, name ? name : "", pack_gain, scope,
                skipped);

    if (out != stdout) {
        if (fclose(out) != 0) dief("%s: %s", outpath, strerror(errno));
        fprintf(stderr, "%s: wrote %zu binding(s) to %s\n",
                g_prog, binds.n, outpath);
    }

    /* Nothing needs this back - the process is about to exit - but a tool
     * that reads user-supplied packs is worth being able to run under
     * -fsanitize=address without wading through its own exit-time noise. */
    binds_free(&binds);
    dirlist_free(&dir);
    free(name);
    return 0;
}
