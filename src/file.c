/**
 * file.c
 *
 * Data file I/O: .FAN archive loading, offset-based reading,
 * actor/part/triangle/event deserialization, save/load game.
 */

#include "file.h"
#include "anim.h"
#include "asm_f.h"
#include "display.h"
#include "edit.h"
#include "ellipse.h"
#include "game.h"
#include "icon.h"
#include "init.h"
#include "map.h"
#include "menu.h"
#include "prof.h"
#include "move.h"
#include "music.h"
#include "req.h"
#include "topo.h"
#include "platform.h"
#include "win.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <ctype.h>
#include <sys/stat.h>
#include "render.h"
#include "compat.h"
/* compat.h supplies the shim on Win32 and DOS; elsewhere it is a real header. */
#if !defined(_WIN32) && !defined(__WATCOMC__)
#include <dirent.h>
#endif

FILE *file_pointer = NULL;
FILE *file2_pointer = NULL;
bool load_by_offset = false;
int32_t file_offsets[MAX_OFFSETS];
int32_t number_of_offsets = 0;

int32_t action_offset[ACTION_TAB_SIZE];
int32_t scene_offset[SCENE_TAB_SIZE];
int32_t actor_offset[THING_TAB_SIZE];
int32_t sound_offset[SOUND_TAB_SIZE];
int32_t repertoire_offset[REPERTOIRE_TAB_SIZE];

int16_t file_version = 0;
int game_version = GAME_VERSION_E2;
int16_t last_scene_dir = 0;
int16_t num_rep_names = 0;

/* Name translation tables (file→global index mapping during load) */
int16_t new_part_name[PART_TAB_SIZE];
int16_t new_thing_name[THING_TAB_SIZE];
int16_t new_action_name[ACTION_TAB_SIZE];
int16_t new_scene_name[SCENE_TAB_SIZE];
int16_t new_code_name[CODE_TAB_SIZE];
int16_t new_rep_name[REPERTOIRE_TAB_SIZE];
int16_t new_sound_name[SOUND_TAB_SIZE];
int16_t new_map_area_name[MAP_AREA_TAB_SIZE];
int16_t new_texture_name[TEXTURE_TAB_SIZE];
int16_t new_point_name[POINT_TAB_SIZE];
int16_t new_triangle_name[TRIANGLE_TAB_SIZE];

/* Read-only search roots for the swappable presentation assets.
 *
 * Root 0 is always the launch directory. E1's Win95 data sits in a nested W/
 * folder inside the DOS bundle; when present it becomes a second root, and
 * enhanced_graphics decides which of the two is consulted first.
 *
 * The database unit (CODE/ECSTATIC.FAN, OFFSETS, OFF2, FILES/) is deliberately
 * excluded — those files are index-paired, so mixing a DOS database with the
 * Win95 offsets shifts scene and sound indices under a running game. Only the
 * directories in swappable_dirs[] are looked up across roots. */
static const char *swappable_dirs[] = {
    "HIRES", "VIEWS", "VISIB", "GRAPHICS", "LOWGRAPH", "MUSIC",
    /* Root-level title art. The two resolutions live in different roots:
     * DOS ships only TITLE_S.RAW (320x200), Win95 only TSCREEN.RAW
     * (640x480), so load_background_title needs to see across both. */
    "TITLE_S.RAW", "TSCREEN.RAW"
};

/* The modern re-release ships the same files in a new tree: the database split
 * into loose .fan files under Code/ (no OFFSETS, no FILES/), the views under
 * Views/, root-level art under Graphics/, title and ending art per language
 * under Text/. The engine keeps asking for the original paths and
 * remaster_paths() lists where each one lives now. */
int remaster_layout = 0;

static const struct { const char *from; const char *to; } remaster_dirs[] = {
    { "HIRES", "Views/Win95" },      { "VIEWS", "Views/DOS" },
    { "VISIB", "Views/Visibility" }, { "SCENES", "Code/Scenes" },
    { "ACTORS", "Code/Actors" },     { "REP", "Code/Repertoires" },
    { "ACTIONS", "Code/Actions" },   { "TEXTURES", "Code/Textures" },
    { "GRAPHICS", "Graphics/Icons" },
};

/* Indexed by `language`, in the order of menu.c's language_names. */
static const char *remaster_languages[] = {
    "English", "Deutsch", "Fran\xc3\xa7" "ais", "", "Italiano",
    "Espa\xc3\xb1" "ol", "Polski"
};

/* Sized to match resolve_ci()'s 512-byte resolved path, which is the
 * largest thing ever snprintf'd in here. */
static char alt_data_root[512] = "";
int32_t enhanced_graphics = 0;

/* Which of the two roots holds the 640x480 set.
 *   1 — launched from the DOS bundle root, alt_data_root is its nested W/
 *   0 — launched from inside W/, alt_data_root is the DOS bundle above it
 * Only the search order depends on this; everything else is symmetric. */
static int alt_root_is_enhanced = 1;

/* Prefix applied to every otherwise-relative asset path. Empty on desktop,
 * where the game runs with the data directory as its working directory.
 * openfpgaOS has no per-process cwd — the platform layer mounts the data
 * ISO and points the root at the mount instead. */
static char data_root[256] = "";

void file_set_data_root(const char *root) {
    if (!root) root = "";
    snprintf(data_root, sizeof(data_root), "%s", root);
}

/* Path's first component names a directory that may come from either root. */
static int is_swappable_asset(const char *path) {
    if (!alt_data_root[0]) return 0;

    size_t len = 0;
    while (path[len] && path[len] != '/' && path[len] != '\\') len++;

    for (size_t i = 0; i < sizeof(swappable_dirs) / sizeof(swappable_dirs[0]); i++) {
        if (strlen(swappable_dirs[i]) == len &&
            strncasecmp(path, swappable_dirs[i], len) == 0)
            return 1;
    }
    return 0;
}

/* Cached directory listings for the case-resolving walk below. On openfpgaOS
 * every readdir pass reads the ISO through the SD bridge, and E2 ships 1119
 * files in VIEWS/; one tune probes five extensions across two roots. Asset
 * roots are read-only (saves use plain fopen), so a listing stays valid.
 * Enough slots for every asset directory of both E1 roots (about 14): with
 * fewer, VIEWS/ was evicted and listed again at camera cuts, 100+ ms each on
 * a Vita. Slots are only filled as directories are visited. */
#define DIRCACHE_SLOTS 32

typedef struct {
    char dir[512];
    char *names;    /* packed, NUL-separated */
    size_t used;
    size_t cap;
} dircache_t;

static dircache_t dir_cache[DIRCACHE_SLOTS];
static int dir_cache_next;

static void dircache_drop(dircache_t *e) {
    free(e->names);
    e->names = NULL;
    e->used = e->cap = 0;
    e->dir[0] = '\0';
}

/* Listing for `dir`, read once and reused. NULL if the directory can't be
 * opened; callers then treat it as "no match". */
static dircache_t *dircache_get(const char *dir) {
    for (int i = 0; i < DIRCACHE_SLOTS; i++)
        if (dir_cache[i].names && strcmp(dir_cache[i].dir, dir) == 0)
            return &dir_cache[i];

    DIR *d = opendir(dir);
    if (!d) return NULL;
    PROF_COUNT_EVENT(PROF_C_DIRLIST);

    dircache_t *e = &dir_cache[dir_cache_next];
    dir_cache_next = (dir_cache_next + 1) % DIRCACHE_SLOTS;
    dircache_drop(e);
    snprintf(e->dir, sizeof(e->dir), "%s", dir);

    struct dirent *ent;
    while ((ent = readdir(d))) {
        size_t n = strlen(ent->d_name) + 1;
        if (e->used + n > e->cap) {
            size_t cap = e->cap ? e->cap * 2 : 4096;
            while (cap < e->used + n) cap *= 2;
            char *grown = (char *)realloc(e->names, cap);
            if (!grown) break;
            e->names = grown;
            e->cap = cap;
        }
        memcpy(e->names + e->used, ent->d_name, n);
        e->used += n;
    }
    closedir(d);
    return e->names ? e : NULL;
}

/* Case-insensitive lookup of `name` in a cached listing. Returns the entry's
 * real spelling, or NULL. */
static const char *dircache_match(const char *dir, const char *name) {
    dircache_t *e = dircache_get(dir);
    if (!e) return NULL;
    for (size_t off = 0; off < e->used; off += strlen(e->names + off) + 1)
        if (strcasecmp(e->names + off, name) == 0)
            return e->names + off;
    return NULL;
}

/* The view prefetch thread opens files too; this guards the listings. NULL
 * until file_enable_threads, so a single-threaded run never locks. */
static platform_sem_t *path_lock;

void file_enable_threads(void) {
    if (!path_lock)
        path_lock = platform_sem_create(1);
}

void file_flush_path_cache(void) {
    if (path_lock) platform_sem_wait(path_lock);
    for (int i = 0; i < DIRCACHE_SLOTS; i++)
        dircache_drop(&dir_cache[i]);
    dir_cache_next = 0;
    if (path_lock) platform_sem_post(path_lock);
}

static int resolve_ci_unlocked(const char *base, const char *path, char *out, size_t outsz);

/* Walk each path component under base, resolving case at every level.
 * Returns 1 and fills out on success. */
static int resolve_ci(const char *base, const char *path, char *out, size_t outsz) {
    if (path_lock) platform_sem_wait(path_lock);
    int found = resolve_ci_unlocked(base, path, out, outsz);
    if (path_lock) platform_sem_post(path_lock);
    return found;
}

static int resolve_ci_unlocked(const char *base, const char *path, char *out, size_t outsz) {
    char buf[512];
    if (!base || !base[0]) base = data_root;
    if (strlen(path) >= sizeof(buf)) return 0;
    strcpy(buf, path);

    for (char *p = buf; *p; p++)
        if (*p == '\\') *p = '/';

    char resolved[512];
    if (base && base[0])
        snprintf(resolved, sizeof(resolved), "%s", base);
    else
        resolved[0] = '\0';

    char *rest = buf;
    while (*rest) {
        char *slash = strchr(rest, '/');
        char component[256];
        if (slash) {
            size_t len = slash - rest;
            if (len >= sizeof(component)) return 0;
            memcpy(component, rest, len);
            component[len] = '\0';
            rest = slash + 1;
        } else {
            if (strlen(rest) >= sizeof(component)) return 0;
            strcpy(component, rest);
            rest += strlen(rest);
        }

        char search_dir[512];
        if (resolved[0])
            snprintf(search_dir, sizeof(search_dir), "%s", resolved);
        else
            strcpy(search_dir, ".");

        const char *match = dircache_match(search_dir, component);
        if (!match) return 0;

        if (resolved[0])
            snprintf(resolved + strlen(resolved),
                     sizeof(resolved) - strlen(resolved), "/%s", match);
        else
            snprintf(resolved, sizeof(resolved), "%s", match);
    }

    snprintf(out, outsz, "%s", resolved);
    return 1;
}

static void join_root(const char *base, const char *rel, char *out, size_t outsz) {
    if (base && base[0])
        snprintf(out, outsz, "%s/%s", base, rel);
    else
        snprintf(out, outsz, "%s", rel);
}

static FILE *fopen_ci_root(const char *base, const char *path, const char *mode) {
    /* base and the normalised path (norm/upper below) are each up to 511
     * chars, so "%s/%s" can reach 1024 with the joining slash and the
     * terminator — matches join_root()'s worst case exactly. */
    char direct[1024];
    if (!base || !base[0]) base = data_root;

    /* The engine mixes separators and cases ("CODE\\ECSTATIC.FAN",
     * "graphics/iconpage.raw"). Normalising the separator and retrying in
     * upper case (ISO 9660 stores names upper-cased) avoids the directory
     * walk, which is expensive over an SD-backed ISO. */
    char norm[512];
    snprintf(norm, sizeof(norm), "%s", path);
    for (char *p = norm; *p; p++)
        if (*p == '\\') *p = '/';

    join_root(base, norm, direct, sizeof(direct));
    FILE *f = fopen(direct, mode);
    if (f) return f;

    /* Read-only probe: opening for write must never invent a second spelling,
     * or it would create the file under a name nothing else looks for. */
    if (mode && mode[0] == 'r') {
        char upper[512];
        int changed = 0;
        size_t i = 0;
        for (; norm[i] && i < sizeof(upper) - 1; i++) {
            upper[i] = (char)toupper((unsigned char)norm[i]);
            if (upper[i] != norm[i]) changed = 1;
        }
        upper[i] = '\0';
        if (changed) {
            join_root(base, upper, direct, sizeof(direct));
            f = fopen(direct, mode);
            if (f) return f;
        }
    }

    char resolved[512];
    if (!resolve_ci(base, norm, resolved, sizeof(resolved))) return NULL;
    return fopen(resolved, mode);
}

static const char *remaster_dir(const char *dir) {
    for (size_t i = 0; i < sizeof(remaster_dirs) / sizeof(remaster_dirs[0]); i++)
        if (strcasecmp(dir, remaster_dirs[i].from) == 0)
            return remaster_dirs[i].to;
    return NULL;
}

static const char *remaster_language_dir(void) {
    if (language >= 0 &&
        language < (int)(sizeof(remaster_languages) / sizeof(remaster_languages[0])) &&
        remaster_languages[language][0])
        return remaster_languages[language];
    return "English";
}

#define REMASTER_CANDIDATES 4

/* Where an original asset path lives in the re-release tree, most specific
 * first. Title and ending art falls back to English, which every language
 * folder's art is derived from. */
static int remaster_paths(const char *path, char out[REMASTER_CANDIDATES][512]) {
    char norm[256];
    snprintf(norm, sizeof(norm), "%s", path);
    for (char *p = norm; *p; p++)
        if (*p == '\\') *p = '/';

    char *slash = strchr(norm, '/');
    if (!slash) {
        snprintf(out[0], 512, "Graphics/%s", norm);
        return 1;
    }
    *slash = '\0';
    const char *dir = norm, *rest = slash + 1;
    const char *lang = remaster_language_dir();
    int n = 0;

    if (strcasecmp(dir, "LOWGRAPH") == 0) {
        char stem[256];
        snprintf(stem, sizeof(stem), "%s", rest);
        char *dot = strrchr(stem, '.');
        if (dot) *dot = '\0';
        snprintf(out[n++], 512, "Text/%s/Titles/%s.low.raw", lang, stem);
        if (strcmp(lang, "English") != 0)
            snprintf(out[n++], 512, "Text/English/Titles/%s.low.raw", stem);
        return n;
    }

    const char *to = remaster_dir(dir);
    if (to)
        snprintf(out[n++], 512, "%s/%s", to, rest);
    if (strcasecmp(dir, "GRAPHICS") == 0) {
        snprintf(out[n++], 512, "Text/%s/Titles/%s", lang, rest);
        if (strcmp(lang, "English") != 0)
            snprintf(out[n++], 512, "Text/English/Titles/%s", rest);
    }
    return n;
}

/* Case-insensitive fopen: tries exact path first, then scans directory
 * for a case-insensitive filename match. Handles nested subdirectories.
 * Swappable asset directories are searched across both data roots. */
static FILE *fopen_ci_any(const char *path, const char *mode) {
    if (remaster_layout && mode && mode[0] == 'r') {
        char cand[REMASTER_CANDIDATES][512];
        int n = remaster_paths(path, cand);
        for (int i = 0; i < n; i++) {
            FILE *f = fopen_ci_root("", cand[i], mode);
            if (f) return f;
        }
    }

    if (is_swappable_asset(path)) {
        /* Consult the root that holds the requested resolution first. */
        int prefer_alt = (enhanced_graphics != 0) == (alt_root_is_enhanced != 0);
        const char *first = prefer_alt ? alt_data_root : "";
        const char *second = prefer_alt ? "" : alt_data_root;

        FILE *f = fopen_ci_root(first, path, mode);
        if (f) return f;
        return fopen_ci_root(second, path, mode);
    }

    return fopen_ci_root("", path, mode);
}

/* newlib's default stdio buffer is 1 KB, so a background or an archive merge
 * became hundreds of sceIoRead calls — each one a Memory Stick round trip on
 * a PSP. 32 KB turns a scene change into a couple of dozen. */
FILE *fopen_ci(const char *path, const char *mode) {
    FILE *f = fopen_ci_any(path, mode);
    if (f && mode[0] == 'r')
        setvbuf(f, NULL, _IOFBF, 32 * 1024);
    return f;
}

/* A directory is a bundle root if it carries its own database. */
static int has_database(const char *base) {
    char resolved[512];
    if (!resolve_ci(base, "CODE/ECSTATIC.FAN", resolved, sizeof(resolved)))
        return 0;
    FILE *probe = fopen(resolved, "rb");
    if (!probe) return 0;
    fclose(probe);
    return 1;
}

/* Same probe, for the platform layer's data-directory search. A candidate
 * counts as the game folder if the database is there — the E1 W/ pairing is
 * worked out afterwards, by init_data_roots, once the cwd is settled. */
int file_dir_has_database(const char *dir) {
    return has_database(dir ? dir : "");
}

/* Pair up the two halves of an E1 install. Called once at init, before any
 * asset load. Works from either side:
 *
 *   run from the DOS bundle root  -> alt root is the nested W/
 *   run from inside W/            -> alt root is the DOS bundle above it
 *
 * Either way both resolutions are reachable and the graphics toggle has
 * somewhere to switch to. */
void init_data_roots(void) {
    alt_data_root[0] = '\0';
    alt_root_is_enhanced = 1;
    enhanced_graphics = 0;

    /* Both resolutions sit side by side under Views/. */
    if (remaster_layout) return;

    char resolved[512];
    if (resolve_ci("", "W/CODE/ECSTATIC.FAN", resolved, sizeof(resolved))) {
        FILE *probe = fopen(resolved, "rb");
        if (probe) {
            fclose(probe);
            /* Store the case-resolved W directory, not the literal "W". The
             * path starts with data_root when one is set (an absolute mount
             * such as ux0:data/ecstatica), so the W component is the first
             * one after it, not the first slash in the string. */
            size_t root_len = strlen(data_root);
            char *rel = resolved;
            if (root_len && strncmp(resolved, data_root, root_len) == 0 &&
                resolved[root_len] == '/')
                rel = resolved + root_len + 1;
            char *slash = strchr(rel, '/');
            if (slash) {
                *slash = '\0';
                snprintf(alt_data_root, sizeof(alt_data_root), "%s", resolved);
                alt_root_is_enhanced = 1;
                DBG_LOG(1, "[FILE] data roots: launch dir holds the 320x200 set, "
                           "'%s' holds 640x480\n", alt_data_root);
                return;
            }
        }
    }

    /* Inverse: we are the W/ folder and the DOS bundle is our parent. Only
     * accept it if the parent carries a database of its own — that is what
     * distinguishes a bundle root from an arbitrary containing directory. */
    char parent[256];
    if (data_root[0])
        snprintf(parent, sizeof(parent), "%s/..", data_root);
    else
        snprintf(parent, sizeof(parent), "..");

    if (!has_database(parent)) return;

    /* Guard against pairing a directory with itself: the launch directory has
     * to be a bundle half in its own right. */
    if (!has_database("")) return;

    snprintf(alt_data_root, sizeof(alt_data_root), "%s", parent);
    alt_root_is_enhanced = 0;
    DBG_LOG(1, "[FILE] data roots: launch dir holds the 640x480 set, "
               "DOS bundle at '%s' holds 320x200\n", alt_data_root);
}

int file_asset_roots(const char *roots[2]) {
    roots[0] = "";
    if (!alt_data_root[0]) return 1;
    roots[1] = alt_data_root;
    return 2;
}

int file_list_dir(const char *root, const char *dir, char *path, size_t pathsz,
                  void (*fn)(void *ctx, const char *name), void *ctx) {
    const char *base = (root && root[0]) ? root : data_root;
    if (remaster_layout && dir && remaster_dir(dir))
        dir = remaster_dir(dir);
    if (dir && dir[0]) {
        if (!resolve_ci(root, dir, path, pathsz)) return 0;
    } else {
        snprintf(path, pathsz, "%s", base[0] ? base : ".");
    }

    if (path_lock) platform_sem_wait(path_lock);
    dircache_t *e = dircache_get(path);
    /* Copied out: fn may open files, and the listing can be evicted meanwhile. */
    char *names = NULL;
    size_t used = 0;
    if (e && (names = (char *)malloc(e->used)) != NULL) {
        memcpy(names, e->names, e->used);
        used = e->used;
    }
    if (path_lock) platform_sem_post(path_lock);
    if (!e) return 0;

    for (size_t off = 0; off < used; off += strlen(names + off) + 1)
        if (names[off] != '.') fn(ctx, names + off);
    free(names);
    return 1;
}

/* Per-file actions are stored by slot number, not by name: each action
 * directory carries a `dir` index (le16 version, le16 count, count 26-byte
 * names) and slot i is NNNNNN.fan. E1 keeps one directory; E2 splits them by
 * animator (ACTIONS/Dave, ACTIONS/Ken, ...), and a name can be listed in more
 * than one, or listed with no file behind it. */
#define ACTION_DIRS_MAX 16

typedef struct {
    char name[26];
    int16_t slot;
    uint8_t dir;
} action_entry_t;

static char action_dir_names[ACTION_DIRS_MAX][64];
static int action_dir_count;
static action_entry_t *action_entries;
static int action_entry_count;
static int action_index_built;

static void read_action_dir_index(const char *sub) {
    if (action_dir_count == ACTION_DIRS_MAX) return;

    char path[128];
    if (sub[0])
        snprintf(path, sizeof(path), "ACTIONS/%s/dir", sub);
    else
        snprintf(path, sizeof(path), "ACTIONS/dir");
    FILE *f = fopen_ci(path, "rb");
    /* The DOS release names it "DIR." */
    if (!f && !sub[0]) f = fopen_ci("ACTIONS/DIR.", "rb");
    if (!f) return;

    getwLoHi(f);
    int count = (uint16_t)getwLoHi(f);
    int dir = action_dir_count++;
    snprintf(action_dir_names[dir], sizeof(action_dir_names[dir]), "%s", sub);

    action_entry_t *grown = (action_entry_t *)realloc(
        action_entries, (size_t)(action_entry_count + count) * sizeof(action_entry_t));
    if (!grown) { fclose(f); return; }
    action_entries = grown;

    for (int i = 0; i < count; i++) {
        char name[26];
        if (fread(name, 1, sizeof(name), f) != sizeof(name)) break;
        if (!name[0]) continue;
        action_entry_t *e = &action_entries[action_entry_count++];
        memcpy(e->name, name, sizeof(name));
        e->name[sizeof(e->name) - 1] = '\0';
        e->slot = (int16_t)i;
        e->dir = (uint8_t)dir;
    }
    fclose(f);
}

typedef struct {
    char names[ACTION_DIRS_MAX][64];
    int count;
} action_subdirs_t;

static void collect_action_subdir(void *ctx, const char *name) {
    action_subdirs_t *s = (action_subdirs_t *)ctx;
    if (strchr(name, '.') || strcasecmp(name, "dir") == 0) return;
    if (s->count == ACTION_DIRS_MAX) return;
    snprintf(s->names[s->count++], sizeof(s->names[0]), "%s", name);
}

static int subdir_cmp(const void *a, const void *b) {
    return strcasecmp((const char *)a, (const char *)b);
}

static void build_action_index(void) {
    action_index_built = 1;
    read_action_dir_index("");

    action_subdirs_t subs;
    subs.count = 0;
    char path[512];
    if (!file_list_dir("", "ACTIONS", path, sizeof(path), collect_action_subdir, &subs))
        return;
    /* Listing order is the filesystem's; sort so a name listed by two
     * animators always resolves to the same one. */
    qsort(subs.names, subs.count, sizeof(subs.names[0]), subdir_cmp);
    for (int i = 0; i < subs.count; i++)
        read_action_dir_index(subs.names[i]);
}

int file_find_action_file(const char *name, char *path, size_t pathsz) {
    if (!name || !name[0]) return 0;
    if (!action_index_built) build_action_index();

    for (int i = 0; i < action_entry_count; i++) {
        const action_entry_t *e = &action_entries[i];
        if (strcasecmp(e->name, name) != 0) continue;
        if (action_dir_names[e->dir][0])
            snprintf(path, pathsz, "ACTIONS/%s/%06d.fan", action_dir_names[e->dir], e->slot);
        else
            snprintf(path, pathsz, "ACTIONS/%06d.fan", e->slot);
        FILE *f = fopen_ci(path, "rb");
        if (f) {
            fclose(f);
            return 1;
        }
    }
    return 0;
}

/* Views/ErPalLut: a 48-byte header (le32 fields), `count` entries of a
 * 768-byte palette plus the offset and size of a zstd colour LUT the
 * re-release's own renderer uses, then two maps of le32 (palette << 16 |
 * camera): one for the view palettes, one for the intro's .PA2 set. Only the
 * palettes and maps are read; they match the original data byte for byte. */
typedef struct {
    int loaded;
    uint32_t count, entries;
    uint32_t *views, *intro;
    uint32_t num_views, num_intro;
} pal_lut_t;

static pal_lut_t pal_lut;

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t *read_pal_map(FILE *f, uint32_t offset, uint32_t n) {
    uint32_t *map = (uint32_t *)malloc((size_t)n * 4);
    if (!map) return NULL;
    uint8_t raw[4];
    fseek(f, (long)offset, SEEK_SET);
    for (uint32_t i = 0; i < n; i++) {
        if (fread(raw, 1, 4, f) != 4) { free(map); return NULL; }
        map[i] = le32(raw);
    }
    return map;
}

static void load_pal_lut(void) {
    pal_lut.loaded = 1;
    FILE *f = fopen_ci("Views/ErPalLut", "rb");
    if (!f) return;

    uint8_t h[48];
    if (fread(h, 1, sizeof(h), f) != sizeof(h) || memcmp(h, "ErPalLut", 8) != 0 ||
        le32(h + 8) != 1 || le32(h + 16) != 768) {
        DBG_LOG(1, "[FILE] Views/ErPalLut: unknown header\n");
        fclose(f);
        return;
    }
    pal_lut.count = le32(h + 12);
    pal_lut.entries = le32(h + 24);
    pal_lut.num_views = le32(h + 36);
    pal_lut.num_intro = le32(h + 44);
    pal_lut.views = read_pal_map(f, le32(h + 32), pal_lut.num_views);
    pal_lut.intro = read_pal_map(f, le32(h + 40), pal_lut.num_intro);
    if (!pal_lut.views) pal_lut.num_views = 0;
    if (!pal_lut.intro) pal_lut.num_intro = 0;
    fclose(f);
}

int file_read_remaster_palette(int camera, int intro, uint8_t out[768]) {
    if (!pal_lut.loaded) load_pal_lut();

    const uint32_t *map = intro ? pal_lut.intro : pal_lut.views;
    uint32_t n = intro ? pal_lut.num_intro : pal_lut.num_views;
    for (uint32_t i = 0; i < n; i++) {
        if ((int)(map[i] & 0xFFFF) != camera) continue;
        uint32_t pal = map[i] >> 16;
        if (pal >= pal_lut.count) return 0;

        FILE *f = fopen_ci("Views/ErPalLut", "rb");
        if (!f) return 0;
        fseek(f, (long)(pal_lut.entries + pal * 0x308), SEEK_SET);
        int ok = fread(out, 1, 768, f) == 768;
        fclose(f);
        return ok;
    }
    return 0;
}

int file_wide_views_available(void) {
    static int known = -1;
    if (known < 0) {
        char resolved[512];
        known = remaster_layout &&
                resolve_ci("", "Views/Fallback", resolved, sizeof(resolved));
    }
    return known;
}

#ifdef ECS_WIDESCREEN
/* platforms/third_party/zstddeclib.c */
size_t ZSTD_decompress(void *dst, size_t dst_capacity, const void *src, size_t src_size);
unsigned ZSTD_isError(size_t code);

/* Views/Fallback/NNNN ("ErBgView"): a 0x48-byte header, then zstd frames in
 * (image, depth) pairs; only the first pair is the camera's view. The depth is
 * stored as byte planes, low byte first: two planes (0xFFFF is sky) or, for a
 * few deep views, three making a signed 24-bit value (-1 is sky). */
int file_read_wide_view(int camera, uint8_t img[640 * 320], uint16_t depth[640 * 320]) {
    char path[64];
    snprintf(path, sizeof(path), "Views/Fallback/%04d", camera);
    FILE *f = fopen_ci(path, "rb");
    if (!f) return 0;

    uint8_t h[0x48];
    int ok = fread(h, 1, sizeof(h), f) == sizeof(h) && memcmp(h, "ErBgView", 8) == 0 &&
             le32(h + 8) == 1 && le32(h + 12) == 0x48;
    int w = h[16] | h[17] << 8, ht = h[18] | h[19] << 8;
    uint32_t pixels = (uint32_t)w * (uint32_t)ht;
    uint32_t planes_n = pixels ? le32(h + 28) / pixels : 0;
    ok = ok && w > 0 && w <= 640 && ht == 320 && le32(h + 24) == pixels &&
         (planes_n == 2 || planes_n == 3) && le32(h + 28) == pixels * planes_n;

    uint32_t img_size = le32(h + 36), depth_size = le32(h + 40);
    uint8_t *packed = NULL;
    uint8_t *planes = NULL;
    if (ok) {
        packed = (uint8_t *)malloc((size_t)img_size + depth_size);
        planes = (uint8_t *)malloc((size_t)pixels * planes_n);
        ok = packed && planes &&
             fread(packed, 1, (size_t)img_size + depth_size, f) == (size_t)img_size + depth_size;
    }
    fclose(f);

    if (ok) {
        size_t n = ZSTD_decompress(img, pixels, packed, img_size);
        ok = !ZSTD_isError(n) && n == pixels;
    }
    if (ok) {
        size_t n = ZSTD_decompress(planes, (size_t)pixels * planes_n, packed + img_size, depth_size);
        ok = !ZSTD_isError(n) && n == (size_t)pixels * planes_n;
    }
    if (ok) {
        for (uint32_t i = 0; i < pixels; i++) {
            int32_t d = planes[i] | planes[pixels + i] << 8;
            if (planes_n == 3) {
                d |= (int32_t)planes[2 * pixels + i] << 16;
                if (d & 0x800000) d -= 0x1000000;
                depth[i] = d < 0 ? 0xFFFF : (d > 0xFFFE ? 0xFFFE : (uint16_t)d);
            } else {
                depth[i] = (uint16_t)d;
            }
        }
    } else {
        DBG_LOG(1, "[FILE] %s: unreadable wide view\n", path);
    }

    free(packed);
    free(planes);
    return ok ? w : 0;
}
#else
int file_read_wide_view(int camera, uint8_t img[640 * 320], uint16_t depth[640 * 320]) {
    (void)camera; (void)img; (void)depth;
    return 0;
}
#endif

/* True when a HIRES background set resolves through either root. */
int hires_data_available(void) {
    char resolved[512];
    if (remaster_layout)
        return resolve_ci("", remaster_dir("HIRES"), resolved, sizeof(resolved));

    int was_enhanced = enhanced_graphics;
    enhanced_graphics = 1;

    int found = resolve_ci(alt_data_root[0] ? alt_data_root : "", "HIRES",
                           resolved, sizeof(resolved));
    if (!found)
        found = resolve_ci("", "HIRES", resolved, sizeof(resolved));

    enhanced_graphics = was_enhanced;
    return found;
}

void merge_sought_file(FILE *f, int quiet);
void file_read_thing(FILE *f);
void file_read_part(FILE *f, actor_t *actor);
void file_read_triangle(FILE *f, actor_t *actor);
void file_read_point(FILE *f, actor_t *actor);
void file_read_action(FILE *f);
void file_read_key(FILE *f, action_t *action);
void file_read_event(FILE *f, key_state_t *key);
void file_read_scene(FILE *f);
void file_read_code(FILE *f);
void file_read_sound(FILE *f);
void file_read_repertoire(FILE *f);
void file_read_map_area(FILE *f);
void file_read_texture(FILE *f);

/* file_find_name_index_sub_function  E1: ? | E2P: 0x441E38 */
static int16_t find_name_index_sub(const char *name_to_find, name_text_t *names, int max_count) {
    if (!name_to_find || !names) return -1;
    for (int i = 0; i < max_count; i++) {
        if (names[i].field_0[0] == '\0')
            break;
        if (strcmp(name_to_find, names[i].field_0) == 0)
            return (int16_t)i;
    }
    return -1;
}

/* file_find_thing_name_index  E1: 0x4375FC | E2: 0x44187C */
int16_t find_thing_name_index(const char *name) {
    return find_name_index_sub(name, thing_names, THING_TAB_SIZE);
}

/* file_find_action_name_index  E1: 0x4377C8 | E2: 0x441A48 */
int16_t find_action_name_index(const char *name) {
    return find_name_index_sub(name, action_names, ACTION_TAB_SIZE);
}

/* file_find_scene_name_index  E1: 0x437BA0 | E2: 0x441E24 */
int16_t find_scene_name_index(const char *name) {
    return find_name_index_sub(name, scene_names, SCENE_TAB_SIZE);
}

/* file_find_code_name_index  E1: 0x437BFC | E2: 0x441E80 */
int16_t find_code_name_index(const char *name) {
    return find_name_index_sub(name, code_names, CODE_TAB_SIZE);
}

/* file_find_code_name_442128 — return name string for a code name index */
char *file_find_code_name(int16_t index) {
    static char empty[] = "";
    if (index < 0 || index >= CODE_TAB_SIZE) return empty;
    return code_names[index].field_0;
}

/* file_find_scene_name_441E38 — return name string for a scene name index */
char *file_find_scene_name(int16_t index) {
    static char empty[] = "";
    if (index < 0 || index >= SCENE_TAB_SIZE) return empty;
    return scene_names[index].field_0;
}

/* file_find_thing_name  E2: 0x441810 — return name string for a thing name index */
char *file_find_thing_name(int16_t index) {
    static char empty[] = "";
    if (index < 0 || index >= THING_TAB_SIZE) return empty;
    return thing_names[index].field_0;
}

/* file_find_action_name  E2: 0x441AA4 */
char *file_find_action_name(int16_t index) {
    static char empty[] = "";
    if (index < 0 || index >= ACTION_TAB_SIZE) return empty;
    return action_names[index].field_0;
}

/* file_find_part_name_str  E2: 0x4415F8 */
char *file_find_part_name_str(int16_t index) {
    static char empty[] = "";
    if (index < 0 || index >= PART_TAB_SIZE) return empty;
    return part_names[index].field_0;
}

/* file_find_point_name_str  E2: 0x4426A8 */
char *file_find_point_name_str(int16_t index) {
    static char empty[] = "";
    if (index < 0 || index >= POINT_TAB_SIZE) return empty;
    return point_names[index].field_0;
}

/* file_find_triangle_name_str  E2: 0x442770 */
char *file_find_triangle_name_str(int16_t index) {
    static char empty[] = "";
    if (index < 0 || index >= TRIANGLE_TAB_SIZE) return empty;
    return triangle_names[index].field_0;
}

/* file_find_repertoire_name_str  E2: 0x4423A0 */
char *file_find_repertoire_name_str(int16_t index) {
    static char empty[] = "";
    if (index < 0 || index >= REPERTOIRE_TAB_SIZE) return empty;
    return repertoire_names[index].field_0;
}

/* file_find_code_name_ptr — search code_list for a code with matching name */
code_t *find_code_name(const char *name) {
    if (!name) return NULL;
    int16_t idx = find_code_name_index(name);
    if (idx < 0) return NULL;
    return code_tab[idx];
}

/* file_find_rep_name_index  E1: 0x437710 | E2: 0x441990 */
int16_t find_rep_name_index(const char *name) {
    return find_name_index_sub(name, repertoire_names, REPERTOIRE_TAB_SIZE);
}

/* file_find_sound_name_index  E1: 0x43776C | E2: 0x4419EC */
int16_t find_sound_name_index(const char *name) {
    return find_name_index_sub(name, sound_names, SOUND_TAB_SIZE);
}

/* file_find_map_area_name_index  E1: 0x437658 | E2: 0x4418D8 */
int16_t find_map_area_name_index(const char *name) {
    return find_name_index_sub(name, map_area_names, MAP_AREA_TAB_SIZE);
}

/* file_find_part_name_index  E2: 0x44159C */
int16_t find_part_name_index(const char *name) {
    return find_name_index_sub(name, part_names, PART_TAB_SIZE);
}

/* file_find_texture_name_index  E2: 0x441934 */
int16_t find_texture_name_index(const char *name) {
    return find_name_index_sub(name, texture_names, TEXTURE_TAB_SIZE);
}

/* file_find_point_name_index  E2: 0x44264C */
int16_t find_point_name_index(const char *name) {
    return find_name_index_sub(name, point_names, POINT_TAB_SIZE);
}

/* file_find_triangle_name_index  E2: 0x442714 */
int16_t find_triangle_name_index(const char *name) {
    return find_name_index_sub(name, triangle_names, TRIANGLE_TAB_SIZE);
}

/* file_find_named_thing  E2: 0x441664 */
actor_t *find_named_thing(const char *name) {
    int16_t idx = find_thing_name_index(name);
    if (idx < 0) return NULL;
    return thing_tab[idx];
}

/* file_recursively_calc_rel_offs  E2: 0x442800 */
void recursively_calc_rel_offs(part_t *part) {
    int16_t *squash = &part->VECTOR_Squash.X;
    int16_t *relcentre = &part->VECTOR_RelCentre.X;
    int16_t *rel_off = &part->rel_offset.X;
    for (int i = 0; i < 3; i++) {
        if (squash[i] != 0) {
            rel_off[i] = (int16_t)(((int32_t)relcentre[i] << 14) / squash[i]);
        } else {
            rel_off[i] = 0;
        }
    }

    part_t *child = part->actor_parts_list;
    while (child) {
        int16_t *child_offset = &child->Offset.X;
        int16_t *child_osr = &child->offset_squash_ratio.X;
        for (int i = 0; i < 3; i++) {
            if (squash[i] != 0) {
                int32_t val = ((int32_t)child_offset[i] << 14) / squash[i];
                int32_t absval = val < 0 ? -val : val;
                if (absval >= 0x8000)
                    child_osr[i] = 0;
                else
                    child_osr[i] = (int16_t)val;
            } else {
                child_osr[i] = 0;
            }
        }
        recursively_calc_rel_offs(child);
        child = child->next;
    }
}

/* file_calculate_all_relative_offsets  E2: 0x4427DC */
void calculate_all_relative_offsets(void) {
    actor_t *actor = thing_list;
    while (actor) {
        if (actor->actor_parts_list) {
            recursively_calc_rel_offs(actor->actor_parts_list);
        }
        actor = actor->next_thing1;
    }
}

/* file_write_event  E2: 0x4412F0 — write event fields via putw_be */
void file_write_event(event_t *event, FILE *f) {
    putw_be(event->event_type, f);
    putw_be(event->event_index, f);
    putw_be(event->param1, f);
    putw_be(event->param2, f);
    putw_be(event->param3, f);
}

/* Emit one event from loose fields. The save writer built each event with a
 * compound literal passed straight to file_write_event; C99 allows that and
 * Open Watcom does not. Same effect, and the call sites read no worse. */
static void put_event(FILE *f, int16_t index, uint16_t type,
                      int16_t p1, int16_t p2, int16_t p3) {
    event_t e;
    e.event_index = index;
    e.event_type  = type;
    e.param1      = p1;
    e.param2      = p2;
    e.param3      = p3;
    e.next        = NULL;
    file_write_event(&e, f);
}

/* Normalize Windows path separators to Unix for cross-platform support */
static void normalize_path(const char *src, char *dst, int max_len) {
    int i;
    for (i = 0; i < max_len - 1 && src[i]; i++) {
        dst[i] = (src[i] == '\\') ? '/' : src[i];
    }
    dst[i] = '\0';
}

/* file_open_read_file  E1: 0x43AF04 | E2: 0x445260 */
void open_read_file(const char *filename) {
    char path[260];
    normalize_path(filename, path, sizeof(path));
    if (file_pointer) fclose(file_pointer);
    file_pointer = fopen_ci(path, "rb");
    if (!file_pointer) {
        /* path is at most 259 chars; "../" adds 3 more. */
        char parent_path[264];
        snprintf(parent_path, sizeof(parent_path), "../%s", path);
        file_pointer = fopen_ci(parent_path, "rb");
    }
    if (!file_pointer) {
        quit2("Cannot open data file:", filename);
    }
}

/* file_open_read_file2  E1: 0x43AF54 | E2: 0x4452B0 */
void open_read_file2(const char *filename) {
    char path[260];
    normalize_path(filename, path, sizeof(path));
    if (file2_pointer) fclose(file2_pointer);
    file2_pointer = fopen_ci(path, "rb");
    /* File2 is optional — don't quit if missing */
}

/* Header version of the main database: 27 and 30 for the DOS E1 releases,
 * 48 for Win95 E1, 55 for E2. */
static int16_t database_version(void) {
    FILE *f = fopen_ci_root("", "CODE/ECSTATIC.FAN", "rb");
    if (!f) return -1;
    int16_t ver = -1;
    if (getl(f) == 0x46414E54) /* 'FANT' */
        ver = getw_be(f);
    fclose(f);
    return ver;
}

static int is_remaster_tree(void) {
    char resolved[512];
    if (!has_database("")) return 0;
    return resolve_ci("", "Views/Win95", resolved, sizeof(resolved)) ||
           resolve_ci("", "Views/DOS", resolved, sizeof(resolved));
}

void detect_game_version(void) {
    DBG_LOG(1, "[FILE] detect_game_version: entering, debug_verbose=%d\n", debug_verbose);
    remaster_layout = is_remaster_tree();
    if (remaster_layout) {
        game_version = database_version() > 48 ? GAME_VERSION_E2 : GAME_VERSION_E1;
        DBG_LOG(1, "[FILE] re-release data layout, E%d\n", game_version);
        return;
    }

    FILE *f = fopen_ci("OFFSETS", "rb");
    if (!f) f = fopen_ci("FILES/OFFSETS.DAT", "rb");
    if (!f) f = fopen_ci("../OFFSETS", "rb");
    if (!f) {
        FILE *actors_dir = fopen_ci("ACTORS/A.FAN", "rb");
        if (actors_dir) {
            fclose(actors_dir);
            game_version = GAME_VERSION_E1;
            return;
        }
        game_version = GAME_VERSION_E2;
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);

    game_version = (size <= 14100) ? GAME_VERSION_E1 : GAME_VERSION_E2;
}

/* file_read_offsets_file  E1: 0x43D81C | E2: 0x447CDC */
void read_offsets_file(void) {
    FILE *f = fopen_ci("OFFSETS", "rb");
    if (!f) f = fopen_ci("FILES/OFFSETS.DAT", "rb");
    if (!f) f = fopen_ci("../OFFSETS", "rb");
    if (!f) {
        do_info_req("Can't open offsets file");
        return;
    }

    /* Bulk-read entire offsets file, then decode big-endian int32s in memory. */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc(file_size);
    if (!buf) { fclose(f); return; }
    fread_ignore(buf, 1, file_size, f);
    fclose(f);

    const uint8_t *p = buf;
    const uint8_t *end = buf + file_size;

    #define READ_OFFSET_BE32() ( \
        (p + 4 <= end) ? \
        (p += 4, (int32_t)((p[-4] << 24) | (p[-3] << 16) | (p[-2] << 8) | p[-1])) : \
        (p = end, (int32_t)0) )

    int scene_count = (game_version == GAME_VERSION_E1) ? E1_SCENE_TAB_SIZE : SCENE_TAB_SIZE;
    int thing_count = (game_version == GAME_VERSION_E1) ? E1_THING_TAB_SIZE : THING_TAB_SIZE;
    int action_count = (game_version == GAME_VERSION_E1) ? E1_ACTION_TAB_SIZE : ACTION_TAB_SIZE;
    int rep_count = (game_version == GAME_VERSION_E1) ? E1_REPERTOIRE_TAB_SIZE : REPERTOIRE_TAB_SIZE;
    int snd_count = (game_version == GAME_VERSION_E1) ? E1_SOUND_TAB_SIZE : SOUND_TAB_SIZE;
    int drv_count = (game_version == GAME_VERSION_E1) ? E1_SOUND_DRIVER_COUNT : 10;
    int tune_count = (game_version == GAME_VERSION_E1) ? E1_TUNE_COUNT : 96;

    for (int i = 0; i < scene_count; ++i)
        scene_offset[i] = READ_OFFSET_BE32();
    for (int i = 0; i < thing_count; ++i)
        actor_offset[i] = READ_OFFSET_BE32();
    for (int i = 0; i < action_count; ++i)
        action_offset[i] = READ_OFFSET_BE32();
    for (int i = 0; i < rep_count; ++i)
        repertoire_offset[i] = READ_OFFSET_BE32();
    for (int i = 0; i < snd_count; ++i)
        sound_offset[i] = READ_OFFSET_BE32();
    for (int i = 0; i < drv_count; ++i)
        for (int j = 0; j < tune_count; ++j)
            tune_offset[i][j] = READ_OFFSET_BE32();
    if (game_version == GAME_VERSION_E2) {
        for (int i = 0; i < PALETTES_MAX; ++i)
            palette_offset[i] = READ_OFFSET_BE32();
        for (int i = 0; i < PALETTES_MAX; ++i)
            visib_offset[i] = READ_OFFSET_BE32();
    } else {
        for (int i = 0; i < PALETTES_MAX; ++i)
            palette_offset[i] = -1;
        for (int i = 0; i < PALETTES_MAX; ++i)
            visib_offset[i] = -1;
    }

    #undef READ_OFFSET_BE32
    free(buf);
}

/* file_getl_425D18 — read 32-bit big-endian */
int32_t getl(FILE *f) {
    unsigned char buf[4];
    if (fread(buf, 1, 4, f) != 4) return 0;
    return ((int32_t)buf[0] << 24) |
           ((int32_t)buf[1] << 16) |
           ((int32_t)buf[2] << 8)  |
           ((int32_t)buf[3]);
}

/* file_getlLoHi — read 32-bit little-endian */
int32_t getlLoHi(FILE *f) {
    unsigned char buf[4];
    if (fread(buf, 1, 4, f) != 4) return 0;
    return ((int32_t)buf[0]) |
           ((int32_t)buf[1] << 8) |
           ((int32_t)buf[2] << 16) |
           ((int32_t)buf[3] << 24);
}

/* file_putl_425D78 — write 32-bit little-endian */
void putl(int32_t val, FILE *f) {
    fputc((val) & 0xFF, f);
    fputc((val >> 8) & 0xFF, f);
    fputc((val >> 16) & 0xFF, f);
    fputc((val >> 24) & 0xFF, f);
}

/* file_merge_a_file_no_message  E1: 0x43AF9C | E2: 0x4452F8 */
void merge_a_file_no_message(const char *filename, int quiet) {
    FILE *f = fopen_ci(filename, "rb");
    if (!f) {
        DBG_LOG(1, "[FILE] merge_a_file_no_message: FAILED to open '%s'\n", filename);
        return;
    }

    merge_sought_file(f, quiet);
    fclose(f);
}

/* file_merge_a_file  E1: 0x43AF80 | E2: 0x4452DC */
void merge_a_file(const char *filename, int quiet) {
    /* The original printed this for its development per-file loads, which
     * shipped games never did. The re-release loads everything that way, and
     * the text was left on screen after almost every load. */
    if (!remaster_layout)
        beep_message("Loading...");
    merge_a_file_no_message(filename, quiet);
}

/* file_read_thing  E1: ? | E2P: 0x425F28 */
void file_read_thing(FILE *f) {
    if (!f) return;

    int16_t name_index = getw_be(f);
    if (name_index < 0 || name_index >= THING_TAB_SIZE) return;

    actor_t *thing = (actor_t *)calloc(1, sizeof(actor_t));
    if (!thing) return;

    thing->name_index = name_index;

    char name[26];
    int name_len = fgetc(f);
    if (name_len > 25) name_len = 25;
    fread_ignore(name, 1, name_len, f);
    name[name_len] = '\0';

    if (thing_names) {
        /* name is already NUL-terminated within its 26 bytes above, and
         * field_0 is the same size, so this copies it whole, terminator
         * included — strncpy's short-source-only termination doesn't apply. */
        memcpy(thing_names[name_index].field_0, name, sizeof(name));
    }

    thing->flags = getw_be(f);
    thing->position_vector.X = getw_be(f);
    thing->position_vector.Y = getw_be(f);
    thing->position_vector.Z = getw_be(f);
    thing->rotate_vector.X = getw_be(f);
    thing->rotate_vector.Y = getw_be(f);
    thing->rotate_vector.Z = getw_be(f);

    int16_t num_parts = getw_be(f);
    for (int i = 0; i < num_parts; i++) {
        file_read_part(f, thing);
    }

    int16_t num_tris = getw_be(f);
    for (int i = 0; i < num_tris; i++) {
        file_read_triangle(f, thing);
    }

    int16_t num_points = getw_be(f);
    for (int i = 0; i < num_points; i++) {
        file_read_point(f, thing);
    }

    thing_tab[name_index] = thing;
    thing->next_thing1 = thing_list;
    thing_list = thing;
}

/* file_read_part  E1: ? | E2P: 0x425FA8 */
void file_read_part(FILE *f, actor_t *actor) {
    if (!f || !actor) return;

    part_t *part = (part_t *)calloc(1, sizeof(part_t));
    if (!part) return;

    part->name_index = getw_be(f);

    part->Offset.X = getw_be(f);
    part->Offset.Y = getw_be(f);
    part->Offset.Z = getw_be(f);

    part->Rotate.X = getw_be(f);
    part->Rotate.Y = getw_be(f);
    part->Rotate.Z = getw_be(f);

    /* Squash (semi-axes) */
    part->VECTOR_Squash.X = getw_be(f);
    part->VECTOR_Squash.Y = getw_be(f);
    part->VECTOR_Squash.Z = getw_be(f);

    part->color = fgetc(f);
    part->type = fgetc(f);
    part->flags = getw_be(f);
    part->color_shade = 0x4000;

    part->def_offset = part->Offset;
    part->def_rotate = part->Rotate;
    part->def_Squash = part->VECTOR_Squash;
    calculate_squash(part);
    part->default_color = part->color;
    part->def_type = part->type;
    part->default_flags = part->flags;

    static int fpr_log = 0;
    if (fpr_log < 100) {
        DBG_LOG(2, "[FPART] actor=%d part=%d type=%d squash=(%d,%d,%d) offset=(%d,%d,%d)\n",
            actor->name_index, part->name_index, part->type,
            part->VECTOR_Squash.X, part->VECTOR_Squash.Y, part->VECTOR_Squash.Z,
            part->Offset.X, part->Offset.Y, part->Offset.Z);
        fpr_log++;
    }

    part->parent_link_index = getw_be(f);
    part->point_to_point = NULL;

    part->parent_actor = actor;
    part->next = actor->actor_parts_list;
    part->next_in_display_list = actor->actor_parts_list;
    actor->actor_parts_list = part;
}

/* file_read_triangle  E1: ? | E2P: 0x426028 */
void file_read_triangle(FILE *f, actor_t *actor) {
    if (!f || !actor) return;

    tri_t *tri = (tri_t *)calloc(1, sizeof(tri_t));
    if (!tri) return;

    tri->texture_name_index = -1;
    tri->point1 = (point_t *)(intptr_t)getw_be(f);
    tri->point2 = (point_t *)(intptr_t)getw_be(f);
    tri->point3 = (point_t *)(intptr_t)getw_be(f);
    tri->tri_shade_name = getw_be(f);
    tri->tri_color_3 = fgetc(f);
    tri->tri_use_flag = fgetc(f);
    tri->triangle_flags = getw_be(f);

    tri->parent_actor = actor;

    tri->next = actor->polygone_tri_list;
    actor->polygone_tri_list = tri;
}

/* file_read_point  E1: ? | E2P: 0x4260A8 */
void file_read_point(FILE *f, actor_t *actor) {
    if (!f || !actor) return;

    point_t *point = (point_t *)calloc(1, sizeof(point_t));
    if (!point) return;

    point->point_index = getw_be(f);

    point->offset_point.X = getw_be(f);
    point->offset_point.Y = getw_be(f);
    point->offset_point.Z = getw_be(f);

    point->parent_part_index = getw_be(f);  /* parent part index (resolved later) */
    point->point_use_flag = getw_be(f);

    point->next = point_list;
    point_list = point;
}

/* file_read_action  E1: ? | E2P: 0x426128 */
void file_read_action(FILE *f) {
    if (!f) return;

    int16_t name_index = getw_be(f);
    if (name_index < 0 || name_index >= ACTION_TAB_SIZE) return;

    action_t *action = (action_t *)calloc(1, sizeof(action_t));
    if (!action) return;

    action->action_index = name_index;
    action->thing_name_index = getw_be(f);
    action->act_duration = getw_be(f);
    action->action_flags = getw_be(f);
    action->next_action_index = -1;

    int16_t num_keys = getw_be(f);
    for (int i = 0; i < num_keys; i++) {
        file_read_key(f, action);
    }

    action_tab[name_index] = action;
    action->next = action_list;
    action_list = action;
}

/* file_read_key  E1: ? | E2P: 0x4261A8 */
void file_read_key(FILE *f, action_t *action) {
    if (!f || !action) return;

    key_state_t *key = (key_state_t *)calloc(1, sizeof(key_state_t));
    if (!key) return;

    key->KEY_position = getw_be(f);
    key->field_E = fgetc(f);

    int16_t num_events = getw_be(f);
    for (int i = 0; i < num_events; i++) {
        file_read_event(f, key);
    }

    key->next = action->key_list;
    action->key_list = key;
}

/* file_read_event  E1: 0x4370FC | E2: 0x44137C */
void file_read_event(FILE *f, key_state_t *key) {
    if (!f || !key) return;

    event_t *event = (event_t *)calloc(1, sizeof(event_t));
    if (!event) return;

    event->event_type = fgetc(f);
    event->event_index = fgetc(f);
    event->param1 = getw_be(f);
    event->param2 = getw_be(f);
    event->param3 = getw_be(f);

    add_event_to_key(event, key);
}

/* file_read_scene  E1: ? | E2P: 0x4262A8 */
void file_read_scene(FILE *f) {
    if (!f) return;

    int16_t name_index = getw_be(f);
    if (name_index < 0 || name_index >= SCENE_TAB_SIZE) return;

    scene_t *scene = (scene_t *)calloc(1, sizeof(scene_t));
    if (!scene) return;

    scene->scene_index = name_index;
    scene->scene_use_flag = getw_be(f);
    scene->scene_music_index = getw_be(f);

    /* Read scene action references (up to 18 slots) */
    int16_t num_actions = getw_be(f);
    for (int i = 0; i < num_actions; i++) {
        int16_t action_idx = getw_be(f);
        if (i < 18)
            scene->action_indices[i] = action_idx;
    }

    scene_tab[name_index] = scene;
    scene->next_scene = scene_list;
    scene_list = scene;
}

/* file_read_code  E1: 0x43A844 | E2: 0x444B90 */
void file_read_code(FILE *f) {
    if (!f) return;

    int16_t name_index = getw_be(f);
    if (name_index < 0 || name_index >= CODE_TAB_SIZE) return;

    code_t *code = (code_t *)calloc(1, sizeof(code_t));
    if (!code) return;

    code->index_code = name_index;

    int16_t token_count = getw_be(f);
    if (token_count > 0 && token_count < 10000) {
        code->token_store_index = top_of_tokens;
        for (int i = 0; i < token_count; i++) {
            token_store[top_of_tokens + i] = getw_be(f);
        }
        top_of_tokens += token_count;
    }
    code_tab[name_index] = code;
    code->next_code = code_list;
    code_list = code;
}

/* The record's rate field is not the playback rate: it reads 21000 for every
 * E1 sound and for most of E2, while the same utterance is stored at 11000 in
 * the E1 DOS database and 22050 in the Win95 one. The real rate is a 16-bit LE
 * field at offset 16 of the 32-byte MyNewDirectSoundBuffer header that precedes
 * the PCM (11000 / 11025 / 22050 across both games). */
static int16_t sound_rate_from_header(const char *hdr, int16_t fallback) {
    uint32_t hdr_size = (uint8_t)hdr[0] | ((uint8_t)hdr[1] << 8) |
                        ((uint8_t)hdr[2] << 16) | ((uint8_t)hdr[3] << 24);
    int rate = (uint8_t)hdr[16] | ((uint8_t)hdr[17] << 8);
    if (hdr_size != 32 || rate < 4000 || rate > 32767) return fallback;
    return (int16_t)rate;
}

/* file_read_sound_4263A8 — all fields little-endian: name_index, use_flag|1,
 * field_10, sound_length, volume; then sound_length - 32 bytes of 8-bit PCM. */
void file_read_sound(FILE *f) {
    if (!f) return;

    int16_t name_index = getwLoHi(f);
    if (name_index < 0 || name_index >= SOUND_TAB_SIZE) return;

    sound_t *sound = (sound_t *)calloc(1, sizeof(sound_t));
    if (!sound) return;

    sound->sound_name_index = name_index;
    sound->use_flag = getwLoHi(f) | 1;
    sound->sample_rate = getwLoHi(f);
    int32_t stored_length = getlLoHi(f);
    sound->volume = getwLoHi(f);
    if (sound->volume <= 0) sound->volume = 100;

    int32_t pcm_length = stored_length - 32;
    sound->sound_length = pcm_length;

    /* MyNewDirectSoundBuffer: 32 bytes header first, then sound_length
     * bytes of unsigned 8-bit PCM. */
    if (stored_length >= 32) {
        fread_ignore(sound->header, 1, 32, f);
        sound->sample_rate = sound_rate_from_header(sound->header,
                                                    sound->sample_rate);
    }
    if (pcm_length > 0) {
        sound->audio_ptr = (char *)calloc(pcm_length, 1);
        if (sound->audio_ptr) {
            fread_ignore(sound->audio_ptr, 1, pcm_length, f);
            for (int32_t b = 0; b < pcm_length; b++)
                sound->audio_ptr[b] = (char)((uint8_t)sound->audio_ptr[b] + 0x80);
        } else {
            fseek(f, pcm_length, SEEK_CUR);
        }
    }

    sound_tab[name_index] = sound;
    sound->next = sound_list;
    sound_list = sound;
}

/* The re-release ships each sound block's samples as a WAV: speech under
 * Audio/Voice/<language>, effects under Audio/Sounds. The 22 kHz files hold
 * the Win95 samples byte for byte (the blocks store them signed), so they land
 * in sound_tab exactly as file_read_sound would put them. */
static int load_wav_sound(int16_t name_index, const char *path) {
    FILE *f = fopen_ci(path, "rb");
    if (!f) return 0;

    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        fclose(f);
        return 0;
    }

    int channels = 0, bits = 0;
    long rate = 0;
    for (;;) {
        uint8_t ck[8];
        if (fread(ck, 1, 8, f) != 8) break;
        uint32_t len = (uint32_t)ck[4] | (uint32_t)ck[5] << 8 |
                       (uint32_t)ck[6] << 16 | (uint32_t)ck[7] << 24;
        if (!memcmp(ck, "fmt ", 4) && len >= 16) {
            uint8_t fmt[16];
            if (fread(fmt, 1, 16, f) != 16) break;
            channels = fmt[2] | fmt[3] << 8;
            rate = (long)fmt[4] | (long)fmt[5] << 8 | (long)fmt[6] << 16;
            bits = fmt[14] | fmt[15] << 8;
            if (fmt[0] != 1 || fmt[1] != 0) channels = 0; /* not PCM */
            fseek(f, (long)(len - 16 + (len & 1)), SEEK_CUR);
        } else if (!memcmp(ck, "data", 4)) {
            if (channels != 1 || (bits != 8 && bits != 16) || rate < 4000 || rate > 32767)
                break;
            int32_t frames = (int32_t)(len / (uint32_t)(bits / 8));
            sound_t *sound = (sound_t *)calloc(1, sizeof(sound_t));
            char *pcm = (char *)malloc(frames > 0 ? (size_t)frames : 1);
            if (!sound || !pcm) { free(sound); free(pcm); break; }
            for (int32_t i = 0; i < frames; i++) {
                /* Unsigned 8-bit, as file_read_sound leaves the blocks. */
                if (bits == 8) {
                    pcm[i] = (char)fgetc(f);
                } else {
                    fgetc(f);
                    pcm[i] = (char)(fgetc(f) + 0x80);
                }
            }
            sound->sound_name_index = name_index;
            sound->use_flag = 1;
            sound->sample_rate = (int16_t)rate;
            sound->sound_length = frames;
            sound->volume = 100;
            sound->audio_ptr = pcm;
            sound_tab[name_index] = sound;
            sound->next = sound_list;
            sound_list = sound;
            fclose(f);
            return 1;
        } else {
            fseek(f, (long)(len + (len & 1)), SEEK_CUR);
        }
    }
    DBG_LOG(1, "[FILE] %s: not 8/16-bit mono PCM\n", path);
    fclose(f);
    return 0;
}

int file_load_remaster_sound(int16_t sound_index) {
    if (sound_index < 0 || sound_index >= SOUND_TAB_SIZE) return 0;
    if (!sound_names || !sound_names[sound_index].field_0[0]) return 0;

    const char *name = sound_names[sound_index].field_0;
    const char *lang = remaster_language_dir();
    char path[256];
    snprintf(path, sizeof(path), "Audio/Voice/%s/%s.wav", lang, name);
    if (load_wav_sound(sound_index, path)) return 1;
    if (strcmp(lang, "English") != 0) {
        snprintf(path, sizeof(path), "Audio/Voice/English/%s.wav", name);
        if (load_wav_sound(sound_index, path)) return 1;
    }
    snprintf(path, sizeof(path), "Audio/Sounds/%s.wav", name);
    return load_wav_sound(sound_index, path);
}

/* file_read_repertoire  E1: ? | E2P: 0x426428 */
void file_read_repertoire(FILE *f) {
    if (!f) return;

    int16_t name_index = getw_be(f);
    if (name_index < 0 || name_index >= REPERTOIRE_TAB_SIZE) return;

    rephead_t *rep = (rephead_t *)calloc(1, sizeof(rephead_t));
    if (!rep) return;

    memset(rep->action_slots, 0xFF, sizeof(rep->action_slots));

    rep->rep_index = name_index;
    rep->thing_index = getw_be(f);
    rep->rep_flags = getw_be(f);

    int16_t num_actions = getw_be(f);
    for (int i = 0; i < num_actions; i++) {
        int16_t action_index = getw_be(f);
        if (i < 208)
            rep->action_slots[i] = action_index;
    }

    if (game_version == GAME_VERSION_E1) {
        for (int rt = 33; rt <= 35; rt++) {
            int variation = rt - 33;
            int16_t ai = rep->action_slots[rt];
            for (int d = 0; d < 9; d++)
                rep->action_slots[50 + variation + d * 3] = ai;
        }
    }

    repertoire_tab[name_index] = rep;
    rep->next_rep = repertoire_list;
    repertoire_list = rep;
}

/* file_read_map_area  E1: ? | E2P: 0x4264A8 */
void file_read_map_area(FILE *f) {
    if (!f) return;

    /* The record must be consumed whole even when it can't be stored, or the
     * stream desyncs for everything read after it. */
    map_area_t *area = (map_area_t *)calloc(1, sizeof(map_area_t));

    int16_t raw_index = getwLoHi(f);
    int16_t name_index = (raw_index >= 0 && raw_index < MAP_AREA_TAB_SIZE)
                         ? new_map_area_name[raw_index] : -1;
    if (area) area->map_area_index = name_index;

    int16_t num_elements = getwLoHi(f);
    for (int i = 0; i < num_elements && !feof(f); i++) {
        int16_t val = getwLoHi(f);
        if (area && i < 10) area->map_area_element_num[i] = val;
    }

    if (!area) return;
    if (name_index >= 0 && name_index < MAP_AREA_TAB_SIZE)
        map_area_tab[name_index] = area;
    area->next = map_area_list;
    map_area_list = area;
}

/* file_read_texture  E1: ? | E2P: 0x426528 */
void file_read_texture(FILE *f) {
    if (!f) return;

    int16_t name_index = getw_be(f);
    if (name_index < 0 || name_index >= TEXTURE_TAB_SIZE) return;

    texture_t *tex = (texture_t *)calloc(1, sizeof(texture_t));
    if (!tex) return;

    tex->textur_index = name_index;
    tex->x_size = getw_be(f);
    tex->y_size = getw_be(f);

    int32_t size = tex->x_size * tex->y_size;
    if (size > 0 && size < 0x100000) {
        tex->texture_data = (char *)calloc(size, 1);
        if (tex->texture_data) {
            fread_ignore(tex->texture_data, 1, size, f);
        } else {
            fseek(f, size, SEEK_CUR);
        }
    }

    texture_tab[name_index] = tex;
    tex->next = texture_list;
    texture_list = tex;
}

/* file_load_by_offset  E1: ? | E2P: 0x4265A8 */
void load_by_offset_index(int index) {
    if (!load_by_offset) return;
    if (!file_pointer) return;
    if (index < 0 || index >= number_of_offsets) return;

    fseek(file_pointer, file_offsets[index], SEEK_SET);
    merge_sought_file(file_pointer, 1);
}

/* file_load_a_thing  E1: ? | E2P: 0x426618 */
void load_a_thing(int thing_index) {
    if (!load_by_offset) {
        char path[256];
        snprintf(path, sizeof(path), "things/%d.fan", thing_index);
        merge_a_file_no_message(path, 1);
    } else if (thing_index >= 0 && thing_index < THING_TAB_SIZE &&
               actor_offset[thing_index] >= 0) {
        fseek(file_pointer, actor_offset[thing_index], SEEK_SET);
        merge_sought_file(file_pointer, 1);
    }
}

/* file_load_a_sound  E1: ? | E2P: 0x426688 */
void load_a_sound(int sound_index) {
    if (sound_index < 0 || sound_index >= SOUND_TAB_SIZE) return;
    if (load_by_offset && file_pointer) {
        if (sound_offset[sound_index] >= 0) {
            fseek(file_pointer, sound_offset[sound_index], SEEK_SET);
            merge_sought_file(file_pointer, 1);
        }
    } else if (remaster_layout) {
        file_load_remaster_sound((int16_t)sound_index);
    } else if (sound_names && sound_names[sound_index].field_0[0]) {
        char path[128];
        snprintf(path, sizeof(path), "sounds/%s.fan", sound_names[sound_index].field_0);
        merge_a_file(path, 1);
    }
}

/* file_load_a_scene  E1: ? | E2P: 0x4266F8 */
void load_a_scene(int scene_index) {
    if (scene_index < 0 || scene_index >= SCENE_TAB_SIZE) return;
    if (load_by_offset && file_pointer) {
        if (scene_offset[scene_index] >= 0) {
            fseek(file_pointer, scene_offset[scene_index], SEEK_SET);
            merge_sought_file(file_pointer, 1);
        }
    } else if (scene_names && scene_names[scene_index].field_0[0]) {
        search_scene_dirs_and_load(scene_names[scene_index].field_0);
    }
}

/* file_load_a_repertoire  E1: ? | E2P: 0x426768 */
void load_a_repertoire(int rep_index) {
    if (rep_index < 0 || rep_index >= REPERTOIRE_TAB_SIZE) return;
    if (load_by_offset && file_pointer) {
        if (repertoire_offset[rep_index] >= 0) {
            fseek(file_pointer, repertoire_offset[rep_index], SEEK_SET);
            merge_sought_file(file_pointer, 1);
        }
    } else if (repertoire_names && repertoire_names[rep_index].field_0[0]) {
        search_rep_dirs_and_load(repertoire_names[rep_index].field_0);
    }
}

/* Port-only settings.
 * Preferences the original never had (it stores its own settings inside the
 * save file, so anything kept there would reset on load). Plain text so it can
 * be edited or deleted by hand; a missing or malformed file just leaves the
 * defaults in place. */
#define PORT_SETTINGS_FILE "ecstatica.cfg"

static void apply_port_setting_overrides(void) {
    const char *wide = getenv("ECSTATICA_WIDESCREEN");
    if (wide && *wide) widescreen = atoi(wide) != 0;
    const char *map3d = getenv("ECSTATICA_MAP3D");
    if (map3d && *map3d) render_map3d = atoi(map3d) != 0;
}

void load_port_settings(void) {
    FILE *f = fopen(PORT_SETTINGS_FILE, "r");
    if (!f) {
        apply_port_setting_overrides();
        return;
    }

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int value;
        if (sscanf(line, "subtitle_scale = %d", &value) == 1) {
            if (value < 1) value = 1;
            if (value > 2) value = 2;
            subtitle_scale = (int16_t)value;
        } else if (sscanf(line, "subtitle_hold = %d", &value) == 1) {
            if (value < 0) value = 0;
            if (value > 2) value = 2;
            subtitle_hold = (int16_t)value;
        } else if (sscanf(line, "renderer = %d", &value) == 1) {
            render_hardware_pref = (value != 0);
        } else if (sscanf(line, "supersample = %d", &value) == 1) {
            /* 0 = auto (match the drawable); otherwise 1..4. */
            if (value < 0) value = 0;
            if (value > 4) value = 4;
            render_supersample = (int16_t)value;
        } else if (sscanf(line, "enhanced_light = %d", &value) == 1) {
            render_enhanced_light = (value != 0);
        } else if (sscanf(line, "map3d = %d", &value) == 1) {
            render_map3d = (value != 0);
        } else if (sscanf(line, "scale_mode = %d", &value) == 1) {
            if (value < SCALE_PILLARBOX) value = SCALE_PILLARBOX;
            if (value > SCALE_STRETCH) value = SCALE_STRETCH;
            display_scale_mode = (int16_t)value;
        } else if (sscanf(line, "widescreen = %d", &value) == 1) {
            widescreen = (value != 0);
        }
    }
    fclose(f);
    apply_port_setting_overrides();
}

void save_port_settings(void) {
    FILE *f = fopen(PORT_SETTINGS_FILE, "w");
    if (!f) return;
    fprintf(f, "subtitle_scale = %d\n", (int)subtitle_scale);
    fprintf(f, "subtitle_hold = %d\n", (int)subtitle_hold);
    fprintf(f, "renderer = %d\n", (int)render_hardware_pref);
    fprintf(f, "supersample = %d\n", (int)render_supersample);
    fprintf(f, "enhanced_light = %d\n", (int)render_enhanced_light);
    fprintf(f, "map3d = %d\n", (int)render_map3d);
    fprintf(f, "scale_mode = %d\n", (int)display_scale_mode);
    fprintf(f, "widescreen = %d\n", widescreen);
    fclose(f);
}

/* Save format (saved/XXXX.ecs): save name (26 bytes), version (2), thumbnail
 * (4800), actor events (terminated by NO_EVENT), repertoire list (-1), scene
 * list (-1), per-thing state (-1), per-actor arrays, map areas (-1), globals,
 * cameras (150), ambients, settings, sentinel 0x1234, then scene flags (v5+)
 * hero gender (v6+) and the swap-out tables (v8+). */

/* Bump when appending to the save stream. Older files stay loadable as long
 * as new sections are appended after the sentinel and read version-gated. */
#define SAVE_VERSION 9
#define SAVE_VERSION_MIN 4   /* oldest layout load_game can still read */
#define SAVE_NAME_LEN 26
#define THUMB_W 80
#define THUMB_H 60

static uint8_t pre_menu_thumbnail[THUMB_W * THUMB_H];

void capture_save_thumbnail(void) {
    for (int ty = 0; ty < THUMB_H; ty++) {
        int sy = mode_svga ? ty * 8 : ty * screen_height / THUMB_H;
        for (int tx = 0; tx < THUMB_W; tx++) {
            int sx = mode_svga ? (screen_width - 640) / 2 + tx * 8
                               : tx * screen_width / THUMB_W;
            if (sy < screen_height && sx < screen_width)
                pre_menu_thumbnail[ty * THUMB_W + tx] = bitmap[1 - db][sy * screen_width + sx];
            else
                pre_menu_thumbnail[ty * THUMB_W + tx] = 0;
        }
    }
}

/* Slot layout is the platform's business: a desktop writes files into a
 * directory, openfpgaOS hands out pre-declared nonvolatile slots. */
static void make_save_filename(char *buf, int bufsz, int slot) {
    platform_save_path(buf, bufsz, slot, game_version);
}

/* file_save_game_parts  E2: 0x443D3C
 * Takes a parent (actor or part) and writes events for all its children.
 * Actor_t and part_t share the first 80 bytes of layout, so casting is safe. */
void save_game_parts(void *parent_v, FILE *f) {
    if (!parent_v || !f) return;
    part_t *parent = (part_t *)parent_v;
    part_t *child = parent->actor_parts_list;
    if (!child) return;

    for (; child; child = child->next) {
        int16_t cidx = child->name_index;

        if (parent->type == 7)
            put_event(f, 0, ADD_PART_TO_THING, cidx, child->flags, (int16_t)child->type);
        else
            put_event(f, parent->name_index, ADD_PART, cidx, child->flags, (int16_t)child->type);

        put_event(f, cidx, OFFSET, child->Offset.X, child->Offset.Y, child->Offset.Z);
        put_event(f, cidx, POSITION, child->AbsPosition.X, child->AbsPosition.Y, child->AbsPosition.Z);
        put_event(f, cidx, ROTATE, child->Rotate.X, child->Rotate.Y, child->Rotate.Z);
        put_event(f, cidx, VECTOR1, child->VECTOR_Squash.X, child->VECTOR_Squash.Y, child->VECTOR_Squash.Z);
        put_event(f, cidx, VECTOR2, child->VECTOR_RelCentre.X, child->VECTOR_RelCentre.Y, child->VECTOR_RelCentre.Z);
        put_event(f, cidx, COLOUR, child->color, child->color_shade, child->max_squash);
        put_event(f, cidx, FLAGS, child->flags, -1, 0);
        put_event(f, cidx, SHADE, child->color_shade, 0, 0);
        put_event(f, cidx, DEF_OFFSET, child->def_offset.X, child->def_offset.Y, child->def_offset.Z);
        put_event(f, cidx, DEF_POSITION, child->def_position.X, child->def_position.Y, child->def_position.Z);
        put_event(f, cidx, DEF_ROTATE, child->def_rotate.X, child->def_rotate.Y, child->def_rotate.Z);
        put_event(f, cidx, DEF_VECTOR1, child->def_Squash.X, child->def_Squash.Y, child->def_Squash.Z);
        put_event(f, cidx, DEF_VECTOR2, child->def_RelCentre.X, child->def_RelCentre.Y, child->def_RelCentre.Z);
        put_event(f, cidx, DEF_COLOUR, child->default_color, child->work_color, (int16_t)child->default_flags);
        put_event(f, cidx, DEF_FLAGS, (int16_t)child->default_flags, -1, 0);

        for (point_t *pt = child->points_list; pt; pt = pt->next) {
            put_event(f, cidx, ADD_POINT, pt->point_index, pt->point_use_flag, pt->offset_point.X);
            put_event(f, pt->point_index, OFFSET_POINT, pt->offset_point.X, pt->offset_point.Y, pt->offset_point.Z);
        }

        save_game_parts(child, f);
    }
}

/* file_save_game_thing  E2: 0x443770 */
void save_game_thing(actor_t *actor, FILE *f) {
    if (!actor || !f) return;

    put_event(f, 0, ADD_THING, actor->name_index, (int16_t)actor->flags, actor->type);
    put_event(f, 0, START_POSITION, actor->start_position.X, actor->start_position.Y, actor->start_position.Z);
    put_event(f, 0, THING_FLAGS, (int16_t)actor->flags, actor->state_flags, -1);
    put_event(f, 0, HELD_OFFSET, actor->held_offset.X, actor->held_offset.Y, actor->held_offset.Z);
    put_event(f, 0, HELD_ROTATE, actor->held_rotate.X, actor->held_rotate.Y, actor->held_rotate.Z);
    put_event(f, 0, HELD_OFF_LEFT, actor->held_off_left.X, actor->held_off_left.Y, actor->held_off_left.Z);
    put_event(f, 0, HELD_ROT_LEFT, actor->held_rot_left.X, actor->held_rot_left.Y, actor->held_rot_left.Z);
    put_event(f, 0, ACTOR_REP, actor->actor_rep_index, 1, actor->default_repert);
    put_event(f, 0, THING_CODE, (int16_t)(actor->code_at_hp_change + 1), (int16_t)(actor->actor_hit_code + 1), (int16_t)(actor->actor_init_code + 1));
    put_event(f, 0, THING_CODE_2, (int16_t)(actor->picked_up_code + 1), (int16_t)(actor->dead_code_index + 1), 0);

    save_game_parts(actor, f);

    for (part_t *pt = (part_t *)actor->actor_parts_list; pt;
         pt = pt->next_in_display_list) {
        if (pt->point_to_point) {
            put_event(f, pt->name_index, POINT_TO_POINT, pt->point_to_point->point_index, 0, 0);
        }
    }

    for (tri_t *tri = actor->polygone_tri_list; tri; tri = tri->next) {
        put_event(f, tri->tri_index, ADD_TRIANGLE, tri->point1->point_index, tri->point2->point_index, tri->point3->point_index);

        if (tri->quad_point4)
            put_event(f, tri->tri_index, MAKE_QUAD, tri->quad_point4->point_index, 0, 0);

        put_event(f, tri->tri_index, COLOUR_TRIANGLE, tri->tri_color_3, tri->tri_color_4, 0);
        put_event(f, tri->tri_index, TRIANGLE_FLAGS, (int16_t)tri->tri_use_flag, -1, 0);
        put_event(f, tri->tri_index, TRI_SHADE_NAME, tri->tri_shade_name, 0, 0);

        if (tri->texture_name_index >= 0) {
            put_event(f, tri->tri_index, TRI_TEX_NAME, tri->texture_name_index, 0, 0);
            put_event(f, tri->tri_index, TRI_TEXTURE1, tri->u1, tri->v1, tri->u2);
            put_event(f, tri->tri_index, TRI_TEXTURE2, tri->v2, tri->u3, tri->v3);
            put_event(f, tri->tri_index, TRI_TEXTURE3, tri->u4, tri->v4, 0);
        }
    }
}

/* game_save_game  E1: 0x448968 | E2: 0x4537B8 */
void save_game(int slot) {
    if (slot < 0 || slot >= platform_save_slot_count()) return;

    platform_save_prepare();

    char filename[32];
    make_save_filename(filename, sizeof(filename), slot);
    FILE *f = fopen(filename, "wb");
    if (!f) {
        do_info_req("Can't create save file");
        return;
    }

    /* 1. Save name (26 bytes) */
    char save_name[SAVE_NAME_LEN];
    memset(save_name, 0, SAVE_NAME_LEN);
    snprintf(save_name, SAVE_NAME_LEN - 1, "Slot %d", slot + 1);
    fwrite(save_name, 1, SAVE_NAME_LEN, f);

    /* 2. Version */
    putwLoHi(SAVE_VERSION, f);

    /* 3. Thumbnail (pre-captured before menu) */
    fwrite(pre_menu_thumbnail, 1, THUMB_W * THUMB_H, f);

    /* 4. Actor hierarchy as events */
    for (actor_t *actor = root_thing; actor; actor = actor->next_in_display_list)
        save_game_thing(actor, f);
    put_event(f, 0, NO_EVENT, 0, 0, 0);

    /* 5. Repertoire list (only reps with use_flag & 2) */
    for (rephead_t *rep = repertoire_list; rep; rep = rep->next_rep) {
        if (rep->rep_use_flag & 2)
            putwLoHi(rep->rep_index, f);
    }
    putwLoHi(-1, f);

    /* 6. Scene list */
    for (scene_t *sc = root_scene; sc; sc = sc->next_scene)
        putwLoHi(sc->scene_index, f);
    putwLoHi(-1, f);

    /* 7. Per-thing runtime state */
    for (actor_t *actor = root_thing; actor; actor = actor->next_in_display_list) {
        int16_t idx = actor->name_index;
        putwLoHi(idx, f);
        putwLoHi(actor->actor_behavior, f);
        save_vector((int16_t *)&actor->position_vector, f);
        save_vector((int16_t *)&actor->actor_center, f);
        save_vector((int16_t *)&actor->rotate_vector, f);
        save_vector((int16_t *)&actor->start_position, f);
        putwLoHi(actor->move_type, f);
        putwLoHi(actor->wander_direction, f);
        putwLoHi(actor->action_delay, f);
        putwLoHi(actor->range_threshold, f);
        putwLoHi(actor->actor_hitpoints, f);
        putwLoHi(actor->full_actor_hp, f);
        putwLoHi(actor->action_state, f);
        putwLoHi((int16_t)actor->flags, f);
        putwLoHi(actor->state_flags, f);
        putwLoHi(actor->extra_action_index, f);
        save_matrix(&actor->matrix33_2, f);
        putwLoHi(actor->actor_strength_factor, f);
        putwLoHi(actor->actor_magic_factor, f);
        putwLoHi(actor->actor_magic, f);
        putwLoHi(actor->actor_hit_factor, f);
        putwLoHi(actor->actor_rep_index, f);
        putwLoHi(actor->default_repert, f);
        putwLoHi((int16_t)(actor->code_at_hp_change + 1), f);
        putwLoHi((int16_t)(actor->actor_hit_code + 1), f);
        putwLoHi((int16_t)(actor->actor_init_code + 1), f);
        putwLoHi((int16_t)(actor->picked_up_code + 1), f);
        putwLoHi((int16_t)(actor->dead_code_index + 1), f);
        save_vector((int16_t *)&actor->held_offset, f);
        save_vector((int16_t *)&actor->held_rotate, f);
        save_vector((int16_t *)&actor->held_off_left, f);
        save_vector((int16_t *)&actor->held_rot_left, f);
        int16_t fa_idx = actor->force_action_to_execute ?
            actor->force_action_to_execute->action_index : -1;
        putwLoHi(fa_idx, f);
        int16_t qa_idx = actor->queued_action ?
            actor->queued_action->action_index : -1;
        putwLoHi(qa_idx, f);
        /* Timed actions */
        int16_t ta_count = 0;
        for (taction_t *ta = actor->tactions_list; ta; ta = ta->next) ta_count++;
        putwLoHi(ta_count, f);
        for (taction_t *ta = actor->tactions_list; ta; ta = ta->next) {
            putwLoHi(ta->taction_index, f);
            putl(ta->taction_time - game_time, f);
        }
        /* Held-by link: holder actor, then (v7+) the holding part */
        part_t *hp = actor->part_heap_link;
        int16_t hba = (hp && hp->parent_actor) ? hp->parent_actor->name_index : -1;
        putwLoHi(hba, f);
        putwLoHi(hba >= 0 ? hp->name_index : -1, f);
    }
    putwLoHi(-1, f);

    /* 8. Sync actor runtime state to per-actor arrays */
    for (actor_t *actor = root_thing; actor; actor = actor->next_in_display_list) {
        int idx = actor->name_index;
        if (idx >= 0 && idx < THING_TAB_SIZE) {
            actor_position[idx] = actor->position_vector;
            actor_orientation[idx] = actor->rotate_vector;
            actor_hit_points[idx] = actor->actor_hitpoints;
            actor_magic[idx] = actor->actor_magic;
        }
    }

    /* 9. Per-actor arrays (THING_TAB_SIZE entries) */
    for (int i = 0; i < THING_TAB_SIZE; i++) {
        putwLoHi(thing_name_flags[i], f);
        putwLoHi(actor_flags[i], f);
        putwLoHi(actor_rep_name[i], f);
        save_vector((int16_t *)&actor_position[i], f);
        save_vector((int16_t *)&actor_orientation[i], f);
        putwLoHi(actor_hit_points[i], f);
        putwLoHi(actor_magic[i], f);
    }

    /* 10. Map areas */
    for (map_area_t *area = map_area_list; area; area = area->next) {
        putwLoHi((int16_t)area->map_area_index, f);
        for (int j = 0; j < 10; j++)
            putwLoHi((int16_t)area->map_area_element_num[j], f);
    }
    putwLoHi(-1, f);

    /* 11. Globals */
    putwLoHi(current_tune, f);
    putwLoHi(armour_factor, f);
    putwLoHi(kill_count, f);
    putwLoHi(treasure_count, f);
    putwLoHi((int16_t)poison_time, f);
    putwLoHi(difficulty, f);
    putl(game_timer, f);
    putl(game_timer_start, f);

    /* 12. Cameras viewed (150 entries) */
    for (int i = 0; i < 150; i++)
        fputc((uint8_t)cameras_viewed[i], f);

    /* 13. Ambient sounds */
    putwLoHi(num_ambients, f);
    for (int i = 0; i < num_ambients; i++) {
        putwLoHi((int16_t)ambiant_name[i], f);
        putwLoHi(ambiant_freq[i], f);
        putwLoHi(ambiant_rand[i], f);
        putwLoHi(ambiant_vol[i], f);
    }

    /* 14. Settings */
    putwLoHi(music_on ? 1 : 0, f);
    putwLoHi(sound_fx_on ? 1 : 0, f);
    putwLoHi(subtitles_on ? 1 : 0, f);
    putwLoHi((int16_t)chosen_svga, f);

    /* 15. Sentinel */
    putwLoHi(0x1234, f);

    /* 16. Scene flags (v5+). Appended after the sentinel so that saves written
     * by this build stay readable by any loader that stops at the sentinel. */
    for (int i = 0; i < SCENE_TAB_SIZE; i++)
        putwLoHi(scene_name_flags[i], f);

    /* 17. Hero gender (v6+), saved by the original too (game_save_game_448180).
     * CT_FEMALE scripts and the E1 death scenes read it. */
    putwLoHi(female ? 1 : 0, f);

    /* 18. Swap-out tables (v8+): pending scene replays and who holds an actor
     * that is swapped out. Sparse, as the 256 KB Pocket slots are near full. */
    for (int i = 0; i < THING_TAB_SIZE; i++) {
        if (!actor_last_act[i] && actor_held_by_actor[i] < 0) continue;
        putwLoHi((int16_t)i, f);
        putwLoHi(actor_last_act[i], f);
        putwLoHi(actor_held_by_actor[i], f);
        putwLoHi(actor_held_by_part[i], f);
    }
    putwLoHi(-1, f);

    fclose(f);
}

/* game_load_game  E1: 0x448B0C | E2: 0x45433C */
void load_game(int slot) {
    if (slot < 0 || slot >= platform_save_slot_count()) return;

    char filename[32];
    make_save_filename(filename, sizeof(filename), slot);
    FILE *f = fopen(filename, "rb");
    if (!f) return;

    /* 1. Skip save name */
    fseek(f, SAVE_NAME_LEN, SEEK_CUR);

    /* 2. Check version */
    int16_t version = getwLoHi(f);
    if (version < SAVE_VERSION_MIN || version > SAVE_VERSION) {
        fclose(f);
        do_info_req("Game was saved in old version");
        return;
    }

    /* 3. Reset game state */
    new_game();

    /* new_game() leaves scene_name_flags alone, so without this a load would
     * inherit the running session's started/finished bits. Same mask as
     * initialise_game(). v5+ saves overwrite these from file in step 16. */
    for (int i = 0; i < SCENE_TAB_SIZE; i++)
        scene_name_flags[i] &= (int16_t)0xFFF1;

    /* Nor the swap-out tables. A leftover actor_last_act from the session
     * before the load (a CT_INIT_SCENE prime at its start) made the first
     * visibility pass swap a held weapon back in at its pickup spot. */
    for (int i = 0; i < THING_TAB_SIZE; i++) {
        actor_last_act[i] = 0;
        actor_held_by_actor[i] = -1;
        actor_held_by_part[i] = -1;
    }

    free_all_heaps();
    /* Both point into the pools just freed. A stale root_scene kept the scene
     * running at load time in the active list; when the save had no active
     * scenes nothing replaced it, the slot was reused, and evicting that
     * reused scene quit with "Can't remove Scene in active list". */
    root_scene = NULL;
    stuck_thing_list = NULL;
    active_camera = NULL;
    clear_subtitles = 1;
    fade_to_black = false;
    fade_to_white = false;

    /* 4. Skip thumbnail */
    fseek(f, THUMB_W * THUMB_H, SEEK_CUR);

    /* 5. Load actor hierarchy from event stream */
    selected_thing = NULL;
    {
        int16_t event_type;
        do {
            event_t *event = read_event(f);
            event_type = event->event_type;
            modify_part(event, selected_thing, 0, 0);
            if (event_type == THING_FLAGS && selected_thing) {
                selected_thing->flags &= 0xFB77u;
                selected_thing->extra_action_index = event->param3;
            }
            free_event(event);
        } while (event_type);
    }

    /* 5b. Post-event init: copy actuals to defaults for triangles and points */
    for (actor_t *a = thing_list; a; a = a->next_thing1) {
        for (tri_t *tri = a->polygone_tri_list; tri; tri = tri->next) {
            tri->tri_color_1 = tri->tri_color_3;
            tri->tri_color_2 = tri->tri_color_4;
            tri->triangle_flags = (int16_t)tri->tri_use_flag;
        }
        for (part_t *p = a->actor_parts_list; p; p = p->next_in_display_list) {
            for (point_t *pt = p->points_list; pt; pt = pt->next)
                copy_vector(&pt->def_offset_point, &pt->offset_point);
        }
    }

    /* 6. Load repertoires */
    for (;;) {
        int16_t rep_idx = getwLoHi(f);
        if (rep_idx < 0) break;
        load_a_repertoire(rep_idx);
    }

    /* 7. Load scenes + add referenced actors to display list */
    {
        scene_t *prev = NULL;
        for (;;) {
            int16_t sc_idx = getwLoHi(f);
            if (sc_idx < 0) break;
            load_a_scene(sc_idx);
            scene_t *sc = scene_tab[sc_idx];
            if (sc) {
                if (prev)
                    prev->next_scene = sc;
                else
                    root_scene = sc;
                sc->next_scene = NULL;
                prev = sc;
                check_actors_in_scene_loaded(sc);
            }
        }
    }

    /* 8. Per-thing runtime state */
    actor_t *held_thing[ACTOR_POOL_SIZE];
    int16_t held_by_actor[ACTOR_POOL_SIZE], held_by_part[ACTOR_POOL_SIZE];
    int n_held = 0;
    for (;;) {
        int16_t idx = getwLoHi(f);
        if (idx < 0) break;

        actor_t *actor = (idx >= 0 && idx < THING_TAB_SIZE) ? thing_tab[idx] : NULL;
        if (!actor) {
            /* Skip: behavior(2) + 4 vectors(24) + 7 words(14) + flags(6) +
               matrix(18) + 4 words(8) + rep(4) + 5 codes(10) +
               4 held vectors(24) + 2 action refs(4) = 112 fixed bytes */
            fseek(f, 2 + 6+6+6+6 + 2+2+2+2+2+2+2 + 2+2+2 + 18 + 2+2+2+2 + 2+2 + 2+2+2+2+2 + 6+6+6+6 + 2+2, SEEK_CUR);
            int16_t ta_count = getwLoHi(f);
            fseek(f, ta_count * 6, SEEK_CUR);
            fseek(f, version >= 7 ? 4 : 2, SEEK_CUR);
            continue;
        }

        actor->actor_behavior = getwLoHi(f);
        load_vector((int16_t *)&actor->position_vector, f);
        load_vector((int16_t *)&actor->actor_center, f);
        load_vector((int16_t *)&actor->rotate_vector, f);
        load_vector((int16_t *)&actor->start_position, f);
        actor->move_type = getwLoHi(f);
        actor->wander_direction = getwLoHi(f);
        actor->action_delay = getwLoHi(f);
        actor->range_threshold = getwLoHi(f);
        actor->actor_hitpoints = getwLoHi(f);
        actor->full_actor_hp = getwLoHi(f);
        actor->action_state = getwLoHi(f);
        actor->flags = (uint16_t)getwLoHi(f);
        actor->state_flags = getwLoHi(f);
        actor->extra_action_index = getwLoHi(f);
        load_matrix(&actor->matrix33_2, f);
        actor->actor_strength_factor = getwLoHi(f);
        actor->actor_magic_factor = getwLoHi(f);
        actor->actor_magic = getwLoHi(f);
        actor->actor_hit_factor = getwLoHi(f);
        actor->actor_rep_index = getwLoHi(f);
        actor->default_repert = getwLoHi(f);
        actor->code_at_hp_change = getwLoHi(f) - 1;
        actor->actor_hit_code = getwLoHi(f) - 1;
        actor->actor_init_code = getwLoHi(f) - 1;
        actor->picked_up_code = getwLoHi(f) - 1;
        actor->dead_code_index = getwLoHi(f) - 1;
        load_vector((int16_t *)&actor->held_offset, f);
        load_vector((int16_t *)&actor->held_rotate, f);
        load_vector((int16_t *)&actor->held_off_left, f);
        load_vector((int16_t *)&actor->held_rot_left, f);

        int16_t fa_idx = getwLoHi(f);
        actor->force_action_to_execute =
            (fa_idx >= 0 && fa_idx < ACTION_TAB_SIZE) ? action_tab[fa_idx] : NULL;
        int16_t qa_idx = getwLoHi(f);
        actor->queued_action =
            (qa_idx >= 0 && qa_idx < ACTION_TAB_SIZE) ? action_tab[qa_idx] : NULL;

        int16_t ta_count = getwLoHi(f);
        actor->tactions_list = NULL;
        for (int t = 0; t < ta_count; t++) {
            int16_t ta_idx = getwLoHi(f);
            int32_t ta_off = getlLoHi(f);
            taction_t *ta = find_free_t_action();
            if (ta) {
                ta->taction_index = ta_idx;
                ta->taction_time = game_time + ta_off;
                ta->next = actor->tactions_list;
                actor->tactions_list = ta;
            }
        }

        int16_t hba = getwLoHi(f);
        int16_t hbp = version >= 7 ? getwLoHi(f) : -1;
        if (hba >= 0 && n_held < ACTOR_POOL_SIZE) {
            held_thing[n_held] = actor;
            held_by_actor[n_held] = hba;
            held_by_part[n_held] = hbp;
            n_held++;
        }
    }

    /* Held objects are relinked once every holder exists: without the link
     * they keep their saved world position and stop following the hand. */
    for (int i = 0; i < n_held; i++) {
        actor_t *holder = held_by_actor[i] < THING_TAB_SIZE ? thing_tab[held_by_actor[i]] : NULL;
        if (!holder || !holder->_PartTab) continue;
        part_t *hand = NULL;
        if (held_by_part[i] >= 0) {
            if (held_by_part[i] < PART_TAB_SIZE)
                hand = holder->_PartTab->field_0[held_by_part[i]];
        } else if (holder->actor_behavior == BH_JOYSTICK) {
            /* Pre-v7 saves name the holder but not the part; the hero only
             * holds in its hands, so take the first free one, right first. */
            int e1 = game_version == GAME_VERSION_E1;
            int16_t hands[2];
            hands[0] = e1 ? E1_RIGHT_HAND_PART : E2_RIGHT_HAND_PART;
            hands[1] = e1 ? E1_LEFT_HAND_PART : E2_LEFT_HAND_PART;
            for (int h = 0; h < 2 && !hand; h++) {
                part_t *p = holder->_PartTab->field_0[hands[h]];
                if (p && !p->actor_2_held) hand = p;
            }
        }
        if (!hand) continue;
        hand->actor_2_held = held_thing[i];
        held_thing[i]->part_heap_link = hand;
    }

    /* 9. Per-actor arrays */
    for (int i = 0; i < THING_TAB_SIZE; i++) {
        thing_name_flags[i] = getwLoHi(f);
        actor_flags[i] = getwLoHi(f);
        actor_rep_name[i] = getwLoHi(f);
        load_vector((int16_t *)&actor_position[i], f);
        load_vector((int16_t *)&actor_orientation[i], f);
        actor_hit_points[i] = getwLoHi(f);
        actor_magic[i] = getwLoHi(f);
    }

    /* 10. Map areas */
    for (;;) {
        int16_t area_idx = getwLoHi(f);
        if (area_idx < 0) break;
        map_area_t *area = (area_idx < MAP_AREA_TAB_SIZE) ? map_area_tab[area_idx] : NULL;
        if (area) {
            for (int j = 0; j < 10; j++)
                area->map_area_element_num[j] = (uint16_t)getwLoHi(f);
        } else {
            fseek(f, 20, SEEK_CUR);
        }
    }

    /* 11. Game globals */
    current_tune = getwLoHi(f);
    armour_factor = getwLoHi(f);
    kill_count = getwLoHi(f);
    treasure_count = getwLoHi(f);
    poison_time = getwLoHi(f);
    difficulty = getwLoHi(f);
    game_timer = getlLoHi(f);
    game_timer_start = getlLoHi(f);

    /* 12. Cameras viewed */
    for (int i = 0; i < 150; i++)
        cameras_viewed[i] = (int16_t)fgetc(f);

    /* 13. Ambient sounds */
    num_ambients = getwLoHi(f);
    for (int i = 0; i < num_ambients && i < 20; i++) {
        ambiant_name[i] = getwLoHi(f);
        ambiant_freq[i] = getwLoHi(f);
        ambiant_rand[i] = getwLoHi(f);
        ambiant_vol[i] = getwLoHi(f);
    }

    /* 14. Settings */
    music_on = getwLoHi(f) != 0;
    sound_fx_on = getwLoHi(f) != 0;
    subtitles_on = getwLoHi(f) != 0;
    chosen_svga = getwLoHi(f);

    /* 15. Verify sentinel */
    int16_t sentinel = getwLoHi(f);
    if (sentinel != 0x1234)
        DBG_LOG(1, "[LOAD] Warning: bad sentinel 0x%04X\n", (unsigned)sentinel);

    /* 16. Scene flags (v5+). Pre-v5 saves end at the sentinel; the flags then
     * keep whatever the running session had, which is the existing behaviour
     * (new_game() clears the *_tab arrays but not scene_name_flags). */
    if (version >= 5) {
        for (int i = 0; i < SCENE_TAB_SIZE && !feof(f); i++)
            scene_name_flags[i] = getwLoHi(f);
    }

    /* 17. Hero gender (v6+). Older saves did not store it, so the running
     * session's value leaked in; recover it from the hero below instead. */
    int have_female = 0;
    if (version >= 6) {
        female = getwLoHi(f) != 0;
        have_female = 1;
    }

    /* 18. Swap-out tables (v8+) */
    if (version >= 8) {
        for (;;) {
            int16_t i = getwLoHi(f);
            if (i < 0 || i >= THING_TAB_SIZE || feof(f)) break;
            actor_last_act[i] = getwLoHi(f);
            actor_held_by_actor[i] = getwLoHi(f);
            actor_held_by_part[i] = getwLoHi(f);
        }
    }

    fclose(f);

    if (version < 9)
        replay_skipped_startups();

    /* Post-load: apply positions from per-actor arrays to loaded actors */
    for (actor_t *actor = root_thing; actor; actor = actor->next_in_display_list) {
        int idx = actor->name_index;
        if (idx >= 0 && idx < THING_TAB_SIZE) {
            copy_vector(&actor->position_vector, &actor_position[idx]);
            copy_vector(&actor->rotate_vector, &actor_orientation[idx]);
        }
    }

    selected_thing = NULL;
    for (actor_t *a = root_thing; a; a = a->next_in_display_list) {
        if (a->actor_behavior == BH_JOYSTICK) {
            selected_thing = a;
            break;
        }
    }
    if (!selected_thing)
        selected_thing = thing_tab[0];
    /* E1 heroes are thing 0 (male) and thing 1 (female): start_game_medium
     * loads thing_names[female ? 1 : 0]. */
    if (!have_female && game_version == GAME_VERSION_E1 && selected_thing
        && (selected_thing->name_index == 0 || selected_thing->name_index == 1))
        female = selected_thing->name_index == 1;
    no_wanderers = false;
    intro_flag = false;
    remove_all_graphics();
    update_game_icons();
    draw_magic_bar();
    if (current_tune >= 0 && music_on)
        play_tune(current_tune);
}

/* file_load_saved_thumbnail — read thumbnail from save file into buffer */
int load_saved_thumbnail(int slot, uint8_t *thumb_buf) {
    if (slot < 0 || slot >= platform_save_slot_count() || !thumb_buf) return 0;
    char filename[32];
    make_save_filename(filename, sizeof(filename), slot);
    FILE *f = fopen(filename, "rb");
    if (!f) return 0;
    fseek(f, SAVE_NAME_LEN + 2, SEEK_SET);
    int read = (int)fread(thumb_buf, 1, THUMB_W * THUMB_H, f);
    fclose(f);
    return read == THUMB_W * THUMB_H;
}

/* file_get_save_name — read save name from a save file */
int get_save_name(int slot, char *buf, int buflen) {
    if (slot < 0 || slot >= platform_save_slot_count()) return 0;
    char filename[32];
    make_save_filename(filename, sizeof(filename), slot);
    FILE *f = fopen(filename, "rb");
    if (!f) return 0;
    char name[SAVE_NAME_LEN];
    if (fread(name, 1, SAVE_NAME_LEN, f) != SAVE_NAME_LEN) {
        fclose(f);
        return 0;
    }
    fclose(f);
    name[SAVE_NAME_LEN - 1] = '\0';
    strncpy(buf, name, buflen - 1);
    buf[buflen - 1] = '\0';
    return 1;
}

/* file_calc_rel_offset  E1: 0x438658 | E2: 0x4428D4 */
void calc_rel_offset(part_t *part) {
    part_t *parent = (part_t *)part->holding_actor;
    if (parent && parent->type != 7) {
        for (int i = 0; i < 3; ++i) {
            if (parent->VECTOR_Squash.data[i]) {
                int offset_sq = (part->Offset.data[i] << 14) / parent->VECTOR_Squash.data[i];
                if (abs(offset_sq) >= 0x8000)
                    part->offset_squash_ratio.data[i] = 0;
                else
                    part->offset_squash_ratio.data[i] = (int16_t)offset_sq;
            } else {
                part->offset_squash_ratio.data[i] = 0;
            }
        }
    }
}

/* file_calc_rel_centre  E1: 0x4386E4 | E2: 0x442960 */
void calc_rel_centre(part_t *part) {
    for (int i = 0; i < 3; ++i) {
        if (part->VECTOR_Squash.data[i])
            part->rel_offset.data[i] = (int16_t)((part->VECTOR_RelCentre.data[i] << 14) / part->VECTOR_Squash.data[i]);
        else
            part->rel_offset.data[i] = 0;
    }
}

static int16_t add_name_find_index_ex(const char *name, name_text_t *names, int max_count, bool *is_new) {
    if (!name || !names) {
        DBG_LOG(1, "[NAME] add_name_find_index: NULL name or names array\n");
        if (is_new) *is_new = false;
        return -1;
    }

    for (int i = 0; i < max_count; i++) {
        if (names[i].field_0[0] == '\0')
            break;  /* end of used entries */
        if (strcmp(name, names[i].field_0) == 0) {
            if (is_new) *is_new = false;
            return (int16_t)i;
        }
    }

    for (int i = 0; i < max_count; i++) {
        if (names[i].field_0[0] == '\0') {
            strncpy(names[i].field_0, name, 25);
            names[i].field_0[25] = '\0';
            if (is_new) *is_new = true;
            return (int16_t)i;
        }
    }

    DBG_LOG(1, "[NAME] Too many names! name='%s' max_count=%d\n", name, max_count);
    quit("Too many different names!!");
    if (is_new) *is_new = false;
    return -1;
}

static int16_t add_name_find_index(const char *name, name_text_t *names, int max_count) {
    return add_name_find_index_ex(name, names, max_count, NULL);
}

/* file_add_part_name  E1: 0x43C200 | E2: 0x44655C */
int16_t add_part_name(const char *name) {
    int16_t idx = add_name_find_index(name, part_names, PART_TAB_SIZE);
    if (idx < 0) { DBG_LOG(1, "[NAME] add_part_name failed for '%s'\n", name); return -1; }
    for (actor_t *actor = root_thing; actor; actor = actor->next_in_display_list) {
        if (actor->_PartTab)
            actor->_PartTab->field_0[idx] = NULL;
    }
    return idx;
}

/* file_add_thing_name  E1: 0x43C2E8 | E2: 0x446644 */
int16_t add_thing_name(const char *name) {
    bool is_new = false;
    int16_t idx = add_name_find_index_ex(name, thing_names, THING_TAB_SIZE, &is_new);
    if (is_new) thing_tab[idx] = NULL;
    return idx;
}

/* file_add_action_name  E1: 0x43C3C0 | E2: 0x44671C */
int16_t add_action_name(const char *name) {
    bool is_new = false;
    int16_t idx = add_name_find_index_ex(name, action_names, ACTION_TAB_SIZE, &is_new);
    if (is_new) action_tab[idx] = NULL;
    return idx;
}

/* file_add_scene_name  E1: 0x43C498 | E2: 0x4467F4 */
int16_t add_scene_name(const char *name) {
    bool is_new = false;
    int16_t idx = add_name_find_index_ex(name, scene_names, SCENE_TAB_SIZE, &is_new);
    if (is_new) scene_tab[idx] = NULL;
    return idx;
}

/* file_add_code_name  E1: 0x43C570 | E2: 0x4468CC */
int16_t add_code_name(const char *name) {
    bool is_new = false;
    int16_t idx = add_name_find_index_ex(name, code_names, CODE_TAB_SIZE, &is_new);
    if (is_new) code_tab[idx] = NULL;
    return idx;
}

/* file_add_repertoire_name  E1: 0x43C648 | E2: 0x4469A4 */
int16_t add_repertoire_name(const char *name) {
    bool is_new = false;
    int16_t idx = add_name_find_index_ex(name, repertoire_names, REPERTOIRE_TAB_SIZE, &is_new);
    if (is_new) repertoire_tab[idx] = NULL;
    return idx;
}

/* file_add_sound_name  E1: 0x43C720 | E2: 0x446A7C */
int16_t add_sound_name(const char *name) {
    bool is_new = false;
    int16_t idx = add_name_find_index_ex(name, sound_names, SOUND_TAB_SIZE, &is_new);
    if (is_new) {
        sound_tab[idx] = NULL;
    }
    return idx;
}

/* file_add_map_area_name  E1: 0x43C7F8 | E2: 0x446B54 */
int16_t add_map_area_name(const char *name) {
    bool is_new = false;
    int16_t idx = add_name_find_index_ex(name, map_area_names, MAP_AREA_TAB_SIZE, &is_new);
    if (is_new) map_area_tab[idx] = NULL;
    return idx;
}

/* file_add_texture_name  E1: 0x43C8D0 | E2: 0x446C2C */
int16_t add_texture_name(const char *name) {
    bool is_new = false;
    int16_t idx = add_name_find_index_ex(name, texture_names, TEXTURE_TAB_SIZE, &is_new);
    if (is_new) texture_tab[idx] = NULL;
    return idx;
}

/* file_add_point_name  E1: 0x43C9A4 | E2: 0x446D04 */
int16_t add_point_name(const char *name) {
    int16_t idx = add_name_find_index(name, point_names, POINT_TAB_SIZE);
    for (actor_t *actor = root_thing; actor; actor = actor->next_in_display_list) {
        if (actor->_PointTab)
            actor->_PointTab->field_0[idx] = NULL;
    }
    return idx;
}

/* file_add_triangle_name  E1: 0x43CA8C | E2: 0x446DEC */
int16_t add_triangle_name(const char *name) {
    int16_t idx = add_name_find_index(name, triangle_names, TRIANGLE_TAB_SIZE);
    for (actor_t *actor = root_thing; actor; actor = actor->next_in_display_list) {
        if (actor->_TriangleTab)
            actor->_TriangleTab->field_0[idx] = NULL;
    }
    return idx;
}

/* file_set_new_names_to_old  E1: 0x439048 | E2: 0x4432C4 */
void set_new_names_to_old(void) {
    for (int i = 0; i < PART_TAB_SIZE; ++i)     new_part_name[i] = i;
    for (int i = 0; i < TRIANGLE_TAB_SIZE; ++i) new_triangle_name[i] = i;
    for (int i = 0; i < POINT_TAB_SIZE; ++i)    new_point_name[i] = i;
    for (int i = 0; i < THING_TAB_SIZE; ++i)    new_thing_name[i] = i;
    for (int i = 0; i < ACTION_TAB_SIZE; ++i)   new_action_name[i] = i;
    for (int i = 0; i < SCENE_TAB_SIZE; ++i)    new_scene_name[i] = i;
    for (int i = 0; i < CODE_TAB_SIZE; ++i)     new_code_name[i] = i;
    for (int i = 0; i < REPERTOIRE_TAB_SIZE; ++i) new_rep_name[i] = i;
    for (int i = 0; i < SOUND_TAB_SIZE; ++i)    new_sound_name[i] = i;
    for (int i = 0; i < MAP_AREA_TAB_SIZE; ++i) new_map_area_name[i] = i;
    for (int i = 0; i < TEXTURE_TAB_SIZE; ++i)  new_texture_name[i] = i;
}

/* file_read_event_4413FC — allocate event from heap and read from stream */
event_t *read_event(FILE *f) {
    event_t *event = find_free_event();
    event->event_type = getw_be(f);
    event->event_index = getw_be(f);
    event->param1 = getw_be(f);
    event->param2 = getw_be(f);
    event->param3 = getw_be(f);
    return event;
}

/* file_start_things  E1: 0x43716C | E2: 0x4413EC */
void start_things(void) {
    for (actor_t *actor = root_thing; actor; actor = actor->next_in_display_list) {
        DBG_LOG(2, "[ST] start_things: actor=%p name=%d start_pos=(%d,%d,%d)\n",
            (void*)actor, actor->name_index,
            actor->start_position.X, actor->start_position.Y, actor->start_position.Z);
        copy_vector(&actor->position_vector, &actor->start_position);
        actor->actor_behavior = BH_SLEEP;
        actor->range_threshold = 0x7FFF;
        actor->actor_hitpoints = 0;
        actor->full_actor_hp = 0;
    }
}

/* file_merge_event_names  E1: 0x43CCB0 | E2: 0x447010 */
void merge_event_names(event_t *evt) {
    switch (evt->event_type) {
    case ROTATE: case OFFSET: case COLOUR: case VECTOR1:
    case VECTOR2: case VECTOR3: case TYPE: case DISP_PNT:
    case FLAGS: case ANCHOR_PART: case LOOSEN_JOINT:
    case UNLOOSEN_JOINT: case POSITION: case TWO_PART_LIMB:
    case FIX_PART: case UNFIX_PART: case UNMAKE_LIMB:
    case ABSOLUTE_POS: case ABSOLUTE_ROT: case DEF_ROTATE:
    case DEF_OFFSET: case DEF_VECTOR1: case DEF_VECTOR2:
    case DEF_COLOUR: case DEF_FLAGS: case DEF_POSITION:
    case CUT_PART: case SHADE:
        evt->event_index = new_part_name[evt->event_index];
        break;
    case INTERACT:
        evt->event_index = new_part_name[evt->event_index];
        if (evt->param1 == 3) {
            evt->param2 = new_thing_name[evt->param2];
        } else {
            if (file_version >= 15 &&
                (!evt->param1 || evt->param1 == 1 || evt->param1 == 4)
                && evt->param2)
                evt->param2 = new_code_name[evt->param2 - 1] + 1;
            if (file_version >= 16 && evt->param1 == 5 && evt->param2 >= 0)
                evt->param2 = new_sound_name[evt->param2];
            if (file_version >= 43) {
                if (evt->param1 == 8 || evt->param1 == 9) {
                    if (evt->param2 >= 0)
                        evt->param2 = new_thing_name[evt->param2];
                    if (evt->param3 >= 0)
                        evt->param3 = new_action_name[evt->param3];
                }
            }
        }
        break;
    case ADD_PART:
        evt->event_index = new_part_name[evt->event_index];
        evt->param1 = new_part_name[evt->param1];
        break;
    case ADD_PART_TO_THING:
        evt->param1 = new_part_name[evt->param1];
        break;
    case END_ACTION:
        if (evt->param1 >= 0)
            evt->param1 = new_action_name[evt->param1];
        break;
    case ADD_THING:
        evt->param1 = new_thing_name[evt->param1];
        break;
    case PSEUDO_ACTION:
        evt->event_index = new_action_name[evt->event_index];
        if (evt->param3 >= 0)
            evt->param3 = new_action_name[evt->param3];
        return;
    case PSEUDO_ACTION_2:
        if (evt->param1 >= 0)
            evt->param1 = new_action_name[evt->param1];
        break;
    case SPAWN_ACTION:
        evt->param1 = new_action_name[evt->param1];
        break;
    case MOVE_ACT: case RAND_ACT:
        if (evt->event_index >= 0)
            evt->event_index = new_thing_name[evt->event_index];
        evt->param2 = new_action_name[evt->param2];
        break;
    case RAND_INFO:
        if (evt->event_index >= 0)
            evt->event_index = new_thing_name[evt->event_index];
        break;
    case PSEUDO_SCENE:
        evt->event_index = new_scene_name[evt->event_index];
        if (evt->param2 >= 0)
            evt->param2 = new_scene_name[evt->param2];
        if (evt->param3 >= 0)
            evt->param3 = new_code_name[evt->param3];
        break;
    case PSEUDO_SCENE_2:
        if (evt->param1 >= 0)
            evt->param1 = new_code_name[evt->param1];
        break;
    case PSEUDO_SCRIPT:
        evt->param3 = new_thing_name[evt->param3];
        break;
    case NEXT_SCENE:
        if (evt->param1 >= 0)
            evt->param1 = new_scene_name[evt->param1];
        break;
    case ADD_POINT: case POINT_TO_POINT:
        evt->event_index = new_part_name[evt->event_index];
        evt->param1 = new_point_name[evt->param1];
        break;
    case OFFSET_POINT:
        evt->event_index = new_point_name[evt->event_index];
        break;
    case ADD_TRIANGLE:
        evt->event_index = new_triangle_name[evt->event_index];
        evt->param1 = new_point_name[evt->param1];
        evt->param2 = new_point_name[evt->param2];
        evt->param3 = new_point_name[evt->param3];
        break;
    case MAKE_QUAD:
        evt->event_index = new_triangle_name[evt->event_index];
        evt->param1 = new_point_name[evt->param1];
        break;
    case TRI_TEX_NAME:
        evt->event_index = new_triangle_name[evt->event_index];
        if (evt->param1 >= 0)
            evt->param1 = new_texture_name[evt->param1];
        break;
    case COLOUR_TRIANGLE: case TRIANGLE_FLAGS:
    case TRI_TEXTURE1: case TRI_TEXTURE2: case TRI_TEXTURE3:
        evt->event_index = new_triangle_name[evt->event_index];
        break;
    case TRI_SHADE_NAME:
        evt->event_index = new_triangle_name[evt->event_index];
        if (evt->param1 >= 0)
            evt->param1 = new_triangle_name[evt->param1];
        break;
    case PART_TEXTURE:
        evt->event_index = new_part_name[evt->event_index];
        if (evt->param1 >= 0)
            evt->param1 = new_texture_name[evt->param1];
        break;
    case PSEUDO_REP:
        evt->event_index = new_rep_name[evt->event_index];
        if (evt->param1)
            evt->param1 = new_rep_name[evt->param1];
        if (evt->param2)
            evt->param2 = new_rep_name[evt->param2];
        break;
    case REP_ENTRY:
        evt->event_index = new_rep_name[evt->event_index];
        if (evt->param2 >= 0)
            evt->param2 = new_action_name[evt->param2];
        break;
    case ACTOR_REP:
        if (evt->param1 >= 0)
            evt->param1 = new_rep_name[evt->param1];
        break;
    case THING_CODE: case THING_CODE_2:
        if (evt->param1)
            evt->param1 = new_code_name[evt->param1 - 1] + 1;
        if (evt->param2)
            evt->param2 = new_code_name[evt->param2 - 1] + 1;
        if (evt->param3)
            evt->param3 = new_code_name[evt->param3 - 1] + 1;
        break;
    case NO_EVENT: case PSEUDO_KEY: case ROTATE_THING:
    case MOVE_THING: case START_POSITION: case THING_FLAGS:
    case SCRIPT_MOVE: case SCRIPT_TURN: case REORIENT_THING:
    case ADD_ELLIPSE_EVT: case ADD_ELLIPSE_TO_KEY_EVT:
    case HELD_OFFSET: case HELD_ROTATE: case BACKGROUND:
    case HELD_OFF_LEFT: case HELD_ROT_LEFT:
        break;
    default:
#ifdef DEBUG
        DBG_LOG(1, "unknown event type = %d in merge_event_names\n", evt->event_type);
#endif
        break;
    }
}

/* file_read_actors  E1: 0x43A008 | E2: 0x4442B0 */
void read_actors(FILE *f, int quiet) {
    int16_t event_type;
    actor_t *old_thing = selected_thing; (void)old_thing;
    selected_thing = NULL;
    int actor_count = 0;
    int event_count = 0;
    do {
        event_t *event = read_event(f);
        event_count++;
        if (event_count <= 3)
            DBG_LOG(2, "[FILE] read_actors: event#%d type=%d idx=%d p1=%d p2=%d p3=%d\n",
                    event_count, event->event_type, event->event_index,
                    event->param1, event->param2, event->param3);
        if (event->event_type == ACTOR_REP && file_version < 18 && event->param1 >= num_rep_names)
            event->param1 = -1;
        merge_event_names(event);
        event_type = event->event_type;

        if (event_type == ADD_THING) {
            actor_count++;
            if (actor_count <= 5 || (actor_count % 100) == 0)
                DBG_LOG(2, "[FILE] read_actors: ADD_THING #%d (name_idx=%d) event#%d\n",
                        actor_count, event->param1, event_count);
        }

        if (event_type == ADD_THING || !event_type) {
            if (selected_thing) {
                copy_actual_to_defaults(selected_thing);
                /* Before version 22 there was no left-hand hold; it starts
                 * as the right hand's. */
                if (game_version == GAME_VERSION_E2 && file_version <= 21) {
                    copy_vector(&selected_thing->held_off_left, &selected_thing->held_offset);
                    copy_vector(&selected_thing->held_rot_left, &selected_thing->held_rotate);
                }
            }
        }

        /* Part flags from before bits 0x40 (fixed) and 0x800 meant anything. */
        if (event_type == FLAGS && game_version == GAME_VERSION_E2) {
            if (file_version <= 1)
                event->param2 = -1;
            if (file_version <= 30 && ((event->param1 & event->param2) & 0x40))
                event->param2 &= ~0x40;
            if (file_version <= 38)
                event->param2 &= ~0x800;
        }
        if (event_type == THING_CODE && game_version == GAME_VERSION_E2 && file_version <= 43) {
            event->param1 = 0;
            event->param2 = 0;
            event->param3 = 0;
        }

check_add_thing:
        if (event_type == ADD_THING && thing_tab[event->param1] != NULL) {
            if (quiet) {
                free_event(event);
                while (1) {
                    event = read_event(f);
                    merge_event_names(event);
                    event_type = event->event_type;
                    if (event_type == ADD_THING || !event_type)
                        break;
                    free_event(event);
                }
                goto check_add_thing;
            }
            do_delete_thing(thing_tab[event->param1]);
        }

        if (event_type == THING_FLAGS && file_version < 49)
            event->param3 = -1;

        modify_part(event, selected_thing, 0, 0);

        if (event_type == THING_FLAGS && selected_thing) {
            selected_thing->flags &= 0xFB77u;
            if (file_version < 11)
                selected_thing->flags &= 0xFFFDu;
            if (file_version >= 30)
                selected_thing->extra_action_index = event->param3;
            /* E2's database was re-saved at version 55, but the re-release
             * ships the loose files in every version back to 33. Each later
             * version gave meaning to bits an older file may have set, so
             * those are dropped (Ecstatica II.exe, sub_140113ED0). */
            if (game_version == GAME_VERSION_E2) {
                int v = file_version;
                uint16_t fl = selected_thing->flags;
                if (v <= 16) fl &= ~0x0020;
                if (v <= 23) fl &= ~0x4000;
                if (v <= 24) fl &= ~0x1000;
                if (v <= 32) selected_thing->state_flags = 0;
                if (v <= 46) fl &= ~0x0004;
                if (v <= 51) fl &= ~0x0010;
                if (v <= 53) fl &= ~0x0100;
                if (v <= 54) fl &= ~0x0200;
                selected_thing->flags = fl;
            }
        }
        free_event(event);
    } while (event_type);
}

/* file_read_actions  E1: 0x43A31C | E2: 0x4445E8 */
void read_actions(FILE *f) {
    key_state_t *inserted_key = NULL;
    action_t *added_action = NULL;
    int16_t event_type;
    int action_event_count = 0;
    do {
        event_t *event = read_event(f);
        action_event_count++;
        if (action_event_count <= 3)
            DBG_LOG(2, "[FILE] read_actions: event#%d type=%d idx=%d p1=%d p2=%d p3=%d\n",
                    action_event_count, event->event_type, event->event_index,
                    event->param1, event->param2, event->param3);
        merge_event_names(event);
        event_type = event->event_type;

        while (event_type == PSEUDO_ACTION) {
            if (!action_tab[event->event_index])
                break;
            do_delete_action(action_tab[event->event_index]);
            break;
        }

        switch (event_type) {
        case PSEUDO_ACTION:
            added_action = add_action();
            added_action->action_index = event->event_index;
            added_action->act_duration = event->param1;
            added_action->action_flags = (event->param2 | 0x200) & 0xEFFF;
            added_action->thing_name_index = event->param3;
            action_tab[added_action->action_index] = added_action;
            free_event(event);
            break;
        case PSEUDO_ACTION_2:
            added_action->next_action_index = event->param1;
            free_event(event);
            break;
        case PSEUDO_KEY:
            inserted_key = insert_key(added_action, event->param1);
            free_event(event);
            break;
        default:
            if (event_type) {
                if (event_type == FLAGS && file_version < 31 &&
                    ((event->param1 & event->param2) & 0x40)) {
                    do_info_req("Found FIXED event in action");
                    event->param2 &= ~0x40;
                }
                add_event_to_key(event, inserted_key);
            } else {
                free_event(event);
            }
            break;
        }
    } while (event_type);
}

/* file_read_repertoires  E1: 0x43A588 | E2: 0x444864 */
void read_repertoires(FILE *f) {
    event_t *event;
    int16_t event_type;
    rephead_t *repertoire = NULL;
    do {
        event = read_event(f);
        merge_event_names(event);
        event_type = event->event_type;

        if (event_type == PSEUDO_REP) {
            repertoire = add_repertoire();
            repertoire->rep_index = event->event_index;
            repertoire_tab[event->event_index] = repertoire;
            repertoire->thing_index = event->param1 - 1;
            repertoire->rep_flags = event->param2 - 1;
            free_event(event);
            continue;
        }
        if (event_type == REP_ENTRY) {
            if (repertoire) {
                int16_t rep_idx = event->event_index;
                if (rep_idx >= 0 && rep_idx < REPERTOIRE_TAB_SIZE) {
                    rephead_t *target_rep = repertoire_tab[rep_idx];
                    if (target_rep) {
                        int16_t slot = event->param1;
                        if (slot >= 0 && slot < 200)
                            target_rep->action_slots[slot] = event->param2;
                    }
                }
            }
            free_event(event);
        } else if (event_type == ACTOR_REP) {
            free_event(event);
        } else {
            free_event(event);
        }
    } while (event_type);
}

/* file_read_code_444C10 — tokens carry a 4-bit type prefix (0x1000 part,
 * 0x2000 thing, ...) and 0xE000 marks extended array tokens. */
static int16_t merge_token_names(int16_t token) {
    uint16_t type_nibble = (uint16_t)token & 0xF000u;
    int16_t val = token & 0x0FFF;
    switch (type_nibble) {
    case 0x1000: return (int16_t)(new_part_name[val] | 0x1000);
    case 0x2000: return (int16_t)(new_thing_name[val] | 0x2000);
    case 0x3000: return (int16_t)(new_action_name[val] | 0x3000);
    case 0x4000: return (int16_t)(new_scene_name[val] | 0x4000);
    case 0x5000: return (int16_t)(new_point_name[val] | 0x5000);
    case 0x6000: return (int16_t)(new_triangle_name[val] | 0x6000);
    case 0x7000: return (int16_t)(new_code_name[val] | 0x7000);
    case 0x8000: return (int16_t)(new_rep_name[val] | 0x8000);
    case 0x9000: return (int16_t)(new_map_area_name[val] | 0x9000);
    case 0xA000: return (int16_t)(new_sound_name[val] | 0xA000);
    default: return token;
    }
}

void read_code(FILE *f) {
    int16_t code_size = getw_be(f);
    DBG_LOG(2, "[FILE] read_code: %d code blocks\n", code_size);

    for (int i = 0; i < code_size; i++) {
        int16_t file_code_idx = getw_be(f);
        int16_t code_name_idx = -1;
        if (file_code_idx >= 0 && file_code_idx < CODE_TAB_SIZE)
            code_name_idx = new_code_name[file_code_idx];

        code_t *code = add_code();
        if (!code) {
            DBG_LOG(1, "[FILE] read_code: failed to allocate code_t #%d\n", i);
            /* Still need to skip the data */
            int16_t token = getw_be(f);
            while (token != 0) {
                if ((token & 0xF000) == 0xE000) {
                    int arr_size = ((token & 0x0FFF) + 1) / 2;
                    for (int j = 0; j < arr_size; j++) getw_be(f);
                }
                token = getw_be(f);
            }
            int16_t lc = getw_be(f);
            for (int j = 0; j < lc; j++) {
                int ch; while ((ch = fgetc(f)) != 0 && ch != EOF) {}
            }
            continue;
        }

        code->index_code = code_name_idx;

        if (code_name_idx >= 0 && code_name_idx < CODE_TAB_SIZE) {
            if (code_tab[code_name_idx] && code_tab[code_name_idx] != code)
                delete_code(code_tab[code_name_idx]);
            code_tab[code_name_idx] = code;
        }

        int16_t token = getw_be(f);
        if (token != 0) {
            code->token_store_index = top_of_tokens;
            while (token != 0) {
                int16_t merged = merge_token_names(token);
                if (top_of_tokens < 20000)
                    token_store[top_of_tokens++] = merged;

                /* Handle extended (array) tokens: 0xE000 prefix */
                if ((token & 0xF000) == 0xE000) {
                    int arr_size = ((token & 0x0FFF) + 1) / 2;
                    for (int j = 0; j < arr_size; j++) {
                        int16_t arr_val = getw_be(f);
                        if (top_of_tokens < 20000)
                            token_store[top_of_tokens++] = arr_val;
                    }
                }
                token = getw_be(f);
            }
            if (top_of_tokens < 20000)
                token_store[top_of_tokens++] = 0; /* null terminator */
        }
        /* E1 data v48 ships code 527 ('endintro') with 0 tokens;
           inject CT_END_OF_INTRO so the intro can actually finish. */
        if (code_name_idx == 527 && token == 0 &&
            game_version == GAME_VERSION_E1 && top_of_tokens + 2 < 20000) {
            code->token_store_index = top_of_tokens;
            token_store[top_of_tokens++] = CT_END_OF_INTRO;
            token_store[top_of_tokens++] = 0;
        }

        int16_t line_count = getw_be(f);
        line_of_code_t *prev_loc = NULL;
        for (int j = 0; j < line_count; j++) {
            line_of_code_t *loc;
            if (j == 0)
                loc = add_first_line_of_code(code);
            else
                loc = add_line_of_code(prev_loc);

            if (loc) {
                /* Read null-terminated string into loc->field_0 (max 53 chars) */
                int k = 0, ch;
                while ((ch = fgetc(f)) != 0 && ch != EOF) {
                    if (k < 53) loc->field_0[k++] = (char)ch;
                }
                if (k < 53) loc->field_0[k] = '\0';
                prev_loc = loc;
            } else {
                int ch;
                while ((ch = fgetc(f)) != 0 && ch != EOF) {}
            }
        }
    }

    /* E1 data v48: code 135 ('endshop2') has no code block at all.
       Scene 41 fires code2=135 on completion — inject CT_WANDERERS_ON
       so NPCs can spawn after the intro scenes finish. */
    if (game_version == GAME_VERSION_E1 &&
        135 < CODE_TAB_SIZE && !code_tab[135] && top_of_tokens + 2 < 20000) {
        code_t *code = add_code();
        if (code) {
            code->index_code = 135;
            code->token_store_index = top_of_tokens;
            token_store[top_of_tokens++] = CT_WANDERERS_ON;
            token_store[top_of_tokens++] = 0;
            code_tab[135] = code;
        }
    }
}

/* file_read_sounds_445000
 * Reads sounds from the FANT sub-archive sound section.
 * Format: sentinel byte, then per-sound: name(2LE) flags(2LE) rate(2LE)
 * raw_size(4LE) [volume(2LE)] 32-byte WAV header + PCM data.
 * Loads PCM into sound_tab so sounds are playable immediately. */
void read_sounds(FILE *f) {
    int sentinel = fgetc(f);
    while (sentinel && sentinel != EOF) {
        int16_t name_index = getwLoHi(f);
        int16_t use_flag   = getwLoHi(f) | 1;
        int16_t rate       = getwLoHi(f);
        int32_t raw_size   = getlLoHi(f);
        int16_t volume     = getwLoHi(f);
        if (volume <= 0) volume = 100;

        int32_t pcm_length = raw_size - 32;

        if (name_index >= 0 && name_index < SOUND_TAB_SIZE && !sound_tab[name_index]) {
            sound_t *sound = (sound_t *)calloc(1, sizeof(sound_t));
            if (sound) {
                sound->sound_name_index = name_index;
                sound->use_flag = use_flag;
                sound->sample_rate = rate;
                sound->sound_length = pcm_length;
                sound->volume = volume > 0 ? volume : 100;
                if (raw_size >= 32) {
                    fread_ignore(sound->header, 1, 32, f);
                    sound->sample_rate = sound_rate_from_header(sound->header,
                                                               sound->sample_rate);
                }
                if (pcm_length > 0) {
                    sound->audio_ptr = (char *)calloc(pcm_length, 1);
                    if (sound->audio_ptr) {
                        fread_ignore(sound->audio_ptr, 1, pcm_length, f);
                        for (int32_t b = 0; b < pcm_length; b++)
                            sound->audio_ptr[b] = (char)((uint8_t)sound->audio_ptr[b] + 0x80);
                    } else {
                        fseek(f, pcm_length, SEEK_CUR);
                    }
                }
                sound_tab[name_index] = sound;
                sound->next = sound_list;
                sound_list = sound;
            } else {
                if (raw_size >= 32) fseek(f, 32, SEEK_CUR);
                if (pcm_length > 0) fseek(f, pcm_length, SEEK_CUR);
            }
        } else {
            if (raw_size >= 32) fseek(f, 32, SEEK_CUR);
            if (pcm_length > 0) fseek(f, pcm_length, SEEK_CUR);
        }

        sentinel = fgetc(f);
    }
}

/* file_read_textures_4451A8 — sentinel byte, then per texture: index, flags,
 * width, height (LE) and pixel data. */
void read_textures(FILE *f) {
    int sentinel = fgetc(f);
    int tex_count = 0;
    while (sentinel && sentinel != EOF) {
        tex_count++;
        (void)getwLoHi(f);   /* 2 bytes LE: texture name index */
        (void)getwLoHi(f);   /* 2 bytes LE: flags/type, OR'd with 1 */
        int16_t width = getwLoHi(f);
        int16_t height = getwLoHi(f);

        int pixel_size = width * height;
        if (pixel_size > 0)
            fseek(f, pixel_size, SEEK_CUR);

        sentinel = fgetc(f);
    }
    DBG_LOG(2, "[FILE] read_textures: %d textures\n", tex_count);
}

/* file_merge_new_map  E1: 0x43D258 | E2: 0x4475B8 */
void merge_new_map(FILE *f) {
    for (int i = 0; i < 128; ++i)
        for (int j = 0; j < 128; ++j)
            new_map[i][j] = getwLoHi(f);

    top_of_map_elements = getl(f);

    if (file_version >= 34 && file_version < 40) {
        getw_be(f);
        getw_be(f);
    }

    for (int i = 0; i < top_of_map_elements; ++i) {
        map_area_element_t *elem = &map_elements[i];
        int height = fgetc(f);
        elem->def_height = height;
        elem->height = height;
        elem->block_config = fgetc(f);

        if (game_version == GAME_VERSION_E1) {
            uint8_t cam = fgetc(f);
            elem->camera_index = cam;
            elem->camera_override = cam;
        } else {
            elem->camera_index = getwLoHi(f);
            elem->camera_override = getwLoHi(f);
            getwLoHi(f);
            getwLoHi(f);
        }

        elem->height2 = fgetc(f);
        elem->material = fgetc(f);
        if (file_version >= 21)
            fgetc(f); /* unused byte */

        int16_t code_flags = getwLoHi(f);
        int16_t code_index;
        if ((code_flags & 0x3FFF) == 0x3FFF)
            code_index = 0;
        else
            code_index = new_code_name[code_flags & 0x3FFF] + 1;
        code_index &= 0x3FFF;
        elem->code_index_p1 = code_index | (code_flags & 0xC000);

        if (file_version >= 10)
            getwLoHi(f);

        if (game_version == GAME_VERSION_E2) {
            getwLoHi(f);
            getwLoHi(f);

            for (int j = 0; j < 7; ++j)
                getwLoHi(f);

            elem->wanderer_spawn = fgetc(f);
        } else {
            elem->wanderer_spawn = 0;
        }
    }
    num_cameras = getwLoHi(f);
    int cam_has_top_clip = (game_version == GAME_VERSION_E2 && file_version >= 36);
    for (int i = 0; i < num_cameras; ++i) {
        if (i >= 1200) {
            getwLoHi(f); getwLoHi(f); getwLoHi(f);
            getwLoHi(f); getwLoHi(f); getwLoHi(f);
            getwLoHi(f);
            if (cam_has_top_clip)
                getwLoHi(f);
        } else {
            camera[i].view_pos.X = getwLoHi(f);
            camera[i].view_pos.Y = getwLoHi(f);
            camera[i].view_pos.Z = getwLoHi(f);
            camera[i].view_rot.X = getwLoHi(f);
            camera[i].view_rot.Y = getwLoHi(f);
            camera[i].view_rot.Z = getwLoHi(f);
            camera[i].zoom_factor = getwLoHi(f);
            int16_t top_clip;
            if (cam_has_top_clip)
                top_clip = getwLoHi(f);
            else
                top_clip = -127 << height_shift;
            camera[i].top_clip = top_clip;
            camera[i].time = 0;
        }
    }
    if (num_cameras > 0 && num_cameras <= 1200) {
        DBG_LOG(2, "[FILE]   camera[0]: pos=(%d,%d,%d) rot=(%d,%d,%d) zoom=%d\n",
                camera[0].view_pos.X, camera[0].view_pos.Y, camera[0].view_pos.Z,
                camera[0].view_rot.X, camera[0].view_rot.Y, camera[0].view_rot.Z,
                camera[0].zoom_factor);
    }

    DBG_LOG(2, "[MAP] file_version=%d game_version=%d elems=%d cameras=%d\n",
            file_version, game_version, (int)top_of_map_elements, num_cameras);

    if (file_version >= 20) {
        int area_count = 0;
        for (;;) {
            int c = fgetc(f);
            if (c == EOF || c == 0) break;
            if (++area_count > MAP_AREA_TAB_SIZE) {
                DBG_LOG(1, "[MAP] map area overrun — stream desync\n");
                break;
            }
            file_read_map_area(f);
        }
        DBG_LOG(2, "[MAP] map_areas=%d eof=%d\n", area_count, feof(f) ? 1 : 0);
    }
}

static int read_name_from_stream(FILE *f, char *buf, int max_len) {
    int len = 0;
    char ch = fgetc(f);
    if (!ch) return 0;  /* end of name list */
    do {
        if (len >= max_len)
            quit("name too long");
        buf[len++] = ch;
        ch = fgetc(f);
    } while (ch);
    while (len > 1 && buf[len - 1] == ' ')
        --len;
    buf[len] = '\0';
    return len;
}

/* file_merge_sought_file  E1: 0x43AFC8 | E2: 0x445324 */
static void merge_sought_file_body(FILE *f, int quiet);

void merge_sought_file(FILE *f, int quiet) {
    PROF_BEGIN(PROF_LOAD);
    merge_sought_file_body(f, quiet);
    PROF_END(PROF_LOAD);
}

static void merge_sought_file_body(FILE *f, int quiet) {
    scene_t *scene = NULL;
    char name_buf[52];
    int name_count;

    if (getl(f) != 0x46414E54) { /* 'FANT' */
        do_info_req("Not a Fantasy file!");
        return;
    }

    file_version = getw_be(f);
    int16_t new_name_sys = 0;
    if (file_version >= 26)
        new_name_sys = getw_be(f);

    char fan_internal_name[28];
    memset(fan_internal_name, 0, sizeof(fan_internal_name));
    if (file_version >= 23) {
        for (int i = 0; i < 26; ++i)
            fan_internal_name[i] = fgetc(f);
    }

    if (file_version >= 15 && file_version <= 17) {
        for (int i = 0; i < CODE_TAB_SIZE; ++i)
            new_code_name[i] = i;
    }
    if (new_name_sys) {
        set_new_names_to_old();
    } else {
        name_count = 0;
        while (read_name_from_stream(f, name_buf, 50)) {
            new_part_name[name_count] = add_part_name(name_buf);
            ++name_count;
        }
        DBG_LOG(2, "[FILE]   part names: %d (pos=%ld)\n", name_count, ftell(f));

        name_count = 0;
        while (read_name_from_stream(f, name_buf, 50)) {
            new_thing_name[name_count] = add_thing_name(name_buf);
            ++name_count;
        }
        DBG_LOG(2, "[FILE]   thing names: %d (pos=%ld)\n", name_count, ftell(f));

        name_count = 0;
        while (read_name_from_stream(f, name_buf, 50)) {
            new_action_name[name_count] = add_action_name(name_buf);
            ++name_count;
        }
        DBG_LOG(2, "[FILE]   action names: %d (pos=%ld)\n", name_count, ftell(f));

        name_count = 0;
        while (read_name_from_stream(f, name_buf, 50)) {
            new_scene_name[name_count] = add_scene_name(name_buf);
            ++name_count;
        }
        DBG_LOG(2, "[FILE]   scene names: %d (pos=%ld)\n", name_count, ftell(f));

        /* Point names (version >= 2) */
        if (file_version >= 2) {
            name_count = 0;
            while (read_name_from_stream(f, name_buf, 50)) {
                new_point_name[name_count] = add_point_name(name_buf);
                ++name_count;
            }
            name_count = 0;
            while (read_name_from_stream(f, name_buf, 50)) {
                new_triangle_name[name_count] = add_triangle_name(name_buf);
                ++name_count;
            }
        }
        DBG_LOG(2, "[FILE]   point+tri names (pos=%ld)\n", ftell(f));

        /* Code names (version >= 6) */
        if (file_version >= 6) {
            name_count = 0;
            while (read_name_from_stream(f, name_buf, 50)) {
                new_code_name[name_count] = add_code_name(name_buf);
                ++name_count;
            }
        }

        /* Repertoire names (version >= 12) */
        if (file_version >= 12) {
            name_count = 0;
            while (read_name_from_stream(f, name_buf, 50)) {
                new_rep_name[name_count] = add_repertoire_name(name_buf);
                ++name_count;
            }
            num_rep_names = name_count;
        }
        DBG_LOG(2, "[FILE]   rep names: %d (pos=%ld)\n", name_count, ftell(f));

        /* Sound names (version >= 16) */
        if (file_version >= 16) {
            name_count = 0;
            while (read_name_from_stream(f, name_buf, 50)) {
                new_sound_name[name_count] = add_sound_name(name_buf);
                ++name_count;
            }
        }
        DBG_LOG(2, "[FILE]   sound names: %d (pos=%ld)\n", name_count, ftell(f));

        /* Map area names (version >= 20) */
        if (file_version >= 20) {
            name_count = 0;
            while (read_name_from_stream(f, name_buf, 50)) {
                new_map_area_name[name_count] = add_map_area_name(name_buf);
                ++name_count;
            }
        }
        DBG_LOG(2, "[FILE]   map area names: %d (pos=%ld)\n", name_count, ftell(f));

        /* Texture names (version >= 37) */
        if (file_version >= 37) {
            name_count = 0;
            while (read_name_from_stream(f, name_buf, 50)) {
                new_texture_name[name_count] = add_texture_name(name_buf);
                ++name_count;
            }
        }
        DBG_LOG(2, "[FILE]   texture names: %d (pos=%ld)\n", name_count, ftell(f));
    }

    DBG_LOG(2, "[FILE] merge_sought_file: name tables loaded, reading actions/actors...\n");
    DBG_LOG(2, "[FILE]   file_version=%d new_name_sys=%d\n", file_version, new_name_sys);

    /* Read actions then actors (version >= 4) */
    if (file_version < 4) {
        read_actors(f, quiet);
        read_actions(f);
    } else {
        DBG_LOG(2, "[FILE]   file pos before read_actions: %ld\n", ftell(f));
        read_actions(f);
        DBG_LOG(2, "[FILE]   file pos after read_actions: %ld\n", ftell(f));
        read_actors(f, quiet);
        DBG_LOG(2, "[FILE]   file pos after read_actors: %ld\n", ftell(f));
    }

    DBG_LOG(2, "[FILE] merge_sought_file: actions/actors loaded, reading scenes...\n");
    int16_t event_type;
    script_t *current_script = NULL;
    key_state_t *key_struct = NULL;
    ellipse_t *ellipse = NULL;

    do {
        event_t *event = read_event(f);
        merge_event_names(event);
        event_type = event->event_type;

        while (event_type == PSEUDO_SCENE) {
            if (scene_tab[event->event_index] == NULL)
                break;
            /* auto-overwrite in offset mode */
            do_delete_scene(scene_tab[event->event_index]);
            break;
        }

        switch (event_type) {
        case PSEUDO_SCENE:
            scene = add_scene();
            scene->scene_index = event->event_index;
            scene->camera_index = event->param1;
            scene->scene_music_index = event->param2;
            if (file_version < 8)
                scene->scene_code_2 = -1;
            else
                scene->scene_code_2 = event->param3;
            scene->last_scene_direction = last_scene_dir;
            scene_tab[scene->scene_index] = scene;
            free_event(event);
            break;

        case PSEUDO_SCENE_2:
            if (scene) {
                scene->scene_code_index = event->param1;
            }
            free_event(event);
            break;

        case NEXT_SCENE:
            if (scene) {
                scene->action_indices[event->event_index] = event->param1;
                if (event->event_index) {
                    /* Read next-scene name (scene_name_buf is char[100], 4 x 25-byte names) */
                    int name_len = 0;
                    int name_base = (event->event_index - 1) * 25;
                    for (char ch = fgetc(f); ch; ch = fgetc(f)) {
                        if (name_len < 25)
                            scene->scene_name_buf[name_base + name_len] = ch;
                        ++name_len;
                    }
                    scene->scene_name_buf[name_base + 24] = 0;
                }
            }
            free_event(event);
            break;

        case PSEUDO_SCRIPT:
            if (scene) {
                current_script = add_script(scene);
                current_script->script_action.act_duration = event->param1;
                current_script->script_action.action_flags = event->param2;
                current_script->script_actor_index = event->param3;
            }
            free_event(event);
            break;

        case PSEUDO_KEY:
            if (current_script) {
                key_struct = insert_key(&current_script->script_action, event->param1);
            }
            free_event(event);
            break;

        case ADD_ELLIPSE_EVT:
            ellipse = add_ellipse();
            if (ellipse) {
                ellipse->field_0 = event->param1;
                ellipse->field_2 = event->param2;
                ellipse->field_4 = event->param3;
                ellipse->field_C = event->event_index;
            }
            free_event(event);
            break;

        case ADD_ELLIPSE_TO_KEY_EVT:
            if (ellipse) {
                ellipse->field_6 = event->param1;
                ellipse->field_8 = event->param2;
                ellipse->field_A = event->param3;
                add_ellipse_to_key(ellipse, key_struct);
            }
            free_event(event);
            break;

        default:
            if (event_type) {
                add_event_to_key(event, key_struct);
            } else {
                free_event(event);
            }
            break;
        }
    } while (event_type);

    /* Read code (version >= 6) */
    if (file_version >= 6) {
        read_code(f);
        DBG_LOG(2, "[FILE]   pos after read_code: %ld\n", ftell(f));
    }

    /* Read repertoires (version >= 13) */
    if (file_version >= 13) {
        read_repertoires(f);
        DBG_LOG(2, "[FILE]   pos after read_repertoires: %ld\n", ftell(f));
    }

    /* Read sounds (version >= 16) */
    if (file_version >= 16) {
        read_sounds(f);
        DBG_LOG(2, "[FILE]   pos after read_sounds: %ld\n", ftell(f));
    }

    /* Read textures (version >= 38) */
    if (file_version >= 38) {
        read_textures(f);
        DBG_LOG(2, "[FILE]   pos after read_textures: %ld\n", ftell(f));
    }

    /* Read map (version >= 9) */
    if (file_version >= 9) {
        if (getwLoHi(f))
            merge_new_map(f);
    }

    /* Post-load: start things and fix up display list — asm at 0x4464DA
     * skips this block when quiet arg (var_2C) is non-zero. Internal
     * check_*_loaded callers pass quiet=1 so they never reset already-
     * running actors' positions/behavior. */
    if (quiet)
        return;

    DBG_LOG(2, "[FILE] merge_sought_file: starting things...\n");
    start_things();

    if (selected_thing) {
        selected_thing->flags &= 0xFFF7u;
        add_to_display_list(selected_thing);
    }

    for (action_t *action = action_list; action; action = action->next)
        action->action_flags &= 0xFDFFu;

    if (selected_thing) {
    }
}
