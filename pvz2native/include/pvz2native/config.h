#ifndef PVZ2NATIVE_CONFIG_H
#define PVZ2NATIVE_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime configuration, read once from config.ini next to the executable.
 *
 * Every debug switch defaults OFF, so a plain build (no config.ini at all) is
 * quiet and fast. The .ini is the ONLY source of truth -- the old PVZ2_*
 * environment variables are no longer consulted. See config.cpp for the parser
 * and pvz2_config_load() for how defaults and path resolution are filled in.
 *
 * C-callable on purpose: main.c (the SDL host loop) is C, and the dependency /
 * runtime shims that read these flags are C++. Both include this one header. */
typedef struct pvz2_config {
    /* [log] -- what the harness prints. */
    int verbose;        /* blow-by-blow boot log: per .init_array entry, per lifecycle call */
    int trace;          /* per-import-call trace (~57MB/run, very loud) */
    int pc_sample;      /* [pc-sample]/[HOT] instruction sampling + hot-frame slicing */
    int input;          /* per-event touch/key logging + a frame heartbeat */

    /* [runtime] -- how the emulator behaves. */
    int no_page_table;        /* route guest memory through the slow callback path */
    unsigned heap_quarantine; /* hold the N most-recently-freed heap blocks out of reuse */

    /* [gl] -- rendering diagnostics. */
    int gl_debug_clear;     /* force a loud clear colour instead of the engine's black */
    int gl_no_viewport_fix; /* disable the zero-size viewport substitution */
    int gl_flat_fragment;   /* replace every fragment shader body with a constant colour */
    int gl_strict;          /* query GL state on every suspicious call */

    /* [game] -- what the engine is told about the "device".
     *
     * The Java locale, e.g. "en_US" or "es_AR". Info_SysGetUserLocale returns it
     * verbatim, and Info_SysGetCountryCodeString derives the country from the
     * part after '_'. It steers currency, store region and localized text; a
     * value the .obb has no translation for just falls back to English, so any
     * <lang>_<REGION> string is safe. Empty -> "en_US". */
    char user_locale[32];

    /* [paths] -- always concrete after load: a value left unset in the .ini is
     * filled with <exe_dir>/lib/<default filename>. */
    char so_path[512];
    char obb_path[512];
} pvz2_config_t;

/* Loads config.ini from `ini_path` (may be NULL or missing -> all defaults).
 * Any path left unset in the file is resolved against `base_dir` (the
 * executable's directory, normally with a trailing separator) as
 * base_dir + "lib/" + default name. Call once, early, before
 * pvz2_session_start. Safe to call again to reload. */
void pvz2_config_load(const char *ini_path, const char *base_dir);

/* The loaded config. Never NULL: before pvz2_config_load runs it returns an
 * all-defaults (all-off, empty paths) instance, so shim code can read it
 * unconditionally without an init check. */
const pvz2_config_t *pvz2_config(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif
