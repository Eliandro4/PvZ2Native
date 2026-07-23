/* config.ini reader and self-seeder -- see include/pvz2native/config.h.
 *
 * A tiny, dependency-free INI parser: `[section]` headers, `key = value`
 * lines, `;`/`#` comments. Unknown sections and keys are ignored on purpose so
 * a config written for a newer build does not break an older one. Booleans
 * accept 1/0, true/false, yes/no, on/off. The whole file is optional; with no
 * file every flag stays off and the two resource paths fall back to
 * <exe_dir>/lib/<name>.
 *
 * The default file is embedded here (kDefaultIni) rather than shipped as a
 * loose template: on first run, when config.ini is missing, it is written next
 * to the executable so the switches are discoverable. Everything the feature
 * needs -- parsing, defaults, and seeding -- lives in this one translation
 * unit.
 */

#include <pvz2native/config.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

/* The single instance. Zero-initialised => every flag off and both paths
 * empty, which is the correct "no config loaded yet" state for anything that
 * reads pvz2_config() before pvz2_config_load() runs. */
pvz2_config_t g_config{};

/* Default resource file names, resolved under <exe_dir>/lib. */
constexpr char kDefaultSoName[]  = "libPVZ2.so";
constexpr char kDefaultObbName[] = "main.7.com.ea.game.pvz2_na.obb";

/* Written verbatim to config.ini the first time the game runs without one. It
 * is all-off, so seeding it changes nothing about the run -- it just documents
 * every switch in place. Keep in sync with apply() below. */
constexpr char kDefaultIni[] =
    "; PvZ2Native configuration.\n"
    ";\n"
    "; This file is OPTIONAL and was created with every switch off. With no\n"
    "; config.ini present the behaviour is identical. Set a value to change it;\n"
    "; booleans accept 1/0, true/false, yes/no, on/off. This is the single\n"
    "; source of truth -- the old PVZ2_* environment variables are not read.\n"
    "\n"
    "[paths]\n"
    "; Where to load the game data from. Leave commented to use the defaults:\n"
    ";   <exe folder>/lib/libPVZ2.so\n"
    ";   <exe folder>/lib/main.7.com.ea.game.pvz2_na.obb\n"
    "; so  = lib/libPVZ2.so\n"
    "; obb = lib/main.7.com.ea.game.pvz2_na.obb\n"
    "\n"
    "[log]\n"
    "; Blow-by-blow boot log: one banner per .init_array entry and lifecycle call.\n"
    "verbose = 0\n"
    "; Per-import-call trace. Extremely loud (~57 MB of stdout per run).\n"
    "trace = 0\n"
    "; [pc-sample]/[HOT] instruction sampling and hot-frame slicing. This is the\n"
    "; source of the \"pc=0x.... lr=0x....\" spam.\n"
    "pc_sample = 0\n"
    "\n"
    "[runtime]\n"
    "; Route guest memory through the slow callback path (needed by watchpoints).\n"
    "no_page_table = 0\n"
    "; Hold the N most-recently-freed heap blocks out of the reuse pool (0 = off).\n"
    "heap_quarantine = 0\n"
    "\n"
    "[gl]\n"
    "; Force a loud clear colour instead of the engine's opaque black.\n"
    "debug_clear = 0\n"
    "; Disable the zero-size viewport substitution.\n"
    "no_viewport_fix = 0\n"
    "; Replace every fragment shader body with a constant colour (bisection tool).\n"
    "flat_fragment = 0\n"
    "; Query GL state on every suspicious call and report mismatches.\n"
    "strict = 0\n"
    "\n"
    "[game]\n"
    "; Locale the engine is told the device uses, as <lang>_<REGION>. Steers\n"
    "; currency, store region and which localized text loads; an unavailable\n"
    "; language falls back to English. The country code is taken from the part\n"
    "; after '_'. Leave commented for en_US.\n"
    "; user_locale = en_US\n";

std::string trim(const std::string &s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

int parse_bool(const std::string &v) {
    const std::string t = lower(trim(v));
    if (t == "1" || t == "true" || t == "yes" || t == "on")  return 1;
    return 0; /* anything else, including empty/garbage, reads as off */
}

void set_path(char (&dst)[512], const std::string &v) {
    std::snprintf(dst, sizeof(dst), "%s", v.c_str());
}

/* base_dir with a guaranteed trailing separator (SDL_GetBasePath supplies one,
 * but be defensive), or "" when there is no base. */
std::string base_with_sep(const char *base_dir) {
    std::string base = (base_dir != nullptr) ? base_dir : "";
    if (!base.empty() && base.back() != '/' && base.back() != '\\') base.push_back('/');
    return base;
}

std::string default_lib_path(const char *base_dir, const char *name) {
    return base_with_sep(base_dir) + "lib/" + name;
}

/* A drive-qualified ("C:/..."), UNC, or root-relative path is used verbatim;
 * anything else is treated as relative to the executable's folder, so a
 * config.ini can say `so = lib/libPVZ2.so` and still work from any CWD. */
bool is_absolute(const char *p) {
    if (p == nullptr || p[0] == '\0') return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    return std::isalpha((unsigned char)p[0]) && p[1] == ':';
}

void resolve_relative(char (&dst)[512], const char *base_dir) {
    if (dst[0] == '\0' || is_absolute(dst)) return;
    set_path(dst, base_with_sep(base_dir) + dst);
}

void apply(const std::string &section, const std::string &key, const std::string &val) {
    if (section == "log") {
        if (key == "verbose")            g_config.verbose = parse_bool(val);
        else if (key == "trace")         g_config.trace = parse_bool(val);
        else if (key == "pc_sample")     g_config.pc_sample = parse_bool(val);
        else if (key == "input")         g_config.input = parse_bool(val);
    } else if (section == "runtime") {
        if (key == "no_page_table")      g_config.no_page_table = parse_bool(val);
        else if (key == "heap_quarantine")
            g_config.heap_quarantine = (unsigned)std::strtoul(val.c_str(), nullptr, 0);
    } else if (section == "gl") {
        if (key == "debug_clear")        g_config.gl_debug_clear = parse_bool(val);
        else if (key == "no_viewport_fix") g_config.gl_no_viewport_fix = parse_bool(val);
        else if (key == "flat_fragment") g_config.gl_flat_fragment = parse_bool(val);
        else if (key == "strict")        g_config.gl_strict = parse_bool(val);
    } else if (section == "paths") {
        if (key == "so" && !trim(val).empty())  set_path(g_config.so_path, trim(val));
        else if (key == "obb" && !trim(val).empty()) set_path(g_config.obb_path, trim(val));
    } else if (section == "game") {
        if (key == "user_locale" && !trim(val).empty())
            std::snprintf(g_config.user_locale, sizeof(g_config.user_locale), "%s", trim(val).c_str());
    }
    /* Unknown (section, key) pairs are silently ignored. */
}

/* Parses an already-open INI stream into g_config. */
void parse(FILE *f) {
    std::string section;
    char line[1024];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        std::string s = trim(line);
        if (s.empty() || s[0] == ';' || s[0] == '#') continue;
        if (s.front() == '[' && s.back() == ']') {
            section = lower(trim(s.substr(1, s.size() - 2)));
            continue;
        }
        const std::size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        apply(section, lower(trim(s.substr(0, eq))), s.substr(eq + 1));
    }
}

/* Best-effort write of the embedded default. Failure (read-only folder, etc.)
 * is fine: the in-memory defaults are the same all-off state. */
void seed_default(const char *ini_path) {
    if (FILE *f = std::fopen(ini_path, "wb")) {
        std::fwrite(kDefaultIni, 1, std::strlen(kDefaultIni), f);
        std::fclose(f);
    }
}

}  // namespace

extern "C" void pvz2_config_load(const char *ini_path, const char *base_dir) {
    g_config = pvz2_config_t{}; /* reset to all-off before (re)reading */

    if (ini_path != nullptr) {
        FILE *f = std::fopen(ini_path, "rb");
        if (f == nullptr) {          /* first run: write the documented default */
            seed_default(ini_path);
            f = std::fopen(ini_path, "rb");
        }
        if (f != nullptr) {
            parse(f);
            std::fclose(f);
        }
    }

    /* Any path the file did not set falls back to <exe_dir>/lib/<name>; a
     * relative override is resolved against the exe folder. Either way the rest
     * of the codebase can read a concrete, absolute path unconditionally. */
    if (g_config.so_path[0] == '\0')
        set_path(g_config.so_path, default_lib_path(base_dir, kDefaultSoName));
    else
        resolve_relative(g_config.so_path, base_dir);
    if (g_config.obb_path[0] == '\0')
        set_path(g_config.obb_path, default_lib_path(base_dir, kDefaultObbName));
    else
        resolve_relative(g_config.obb_path, base_dir);

    if (g_config.user_locale[0] == '\0')
        std::snprintf(g_config.user_locale, sizeof(g_config.user_locale), "en_US");
}

extern "C" const pvz2_config_t *pvz2_config(void) { return &g_config; }
