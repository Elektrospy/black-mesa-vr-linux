/*
 * Black Mesa VR - native Linux port, stage 4b: finding a class's vtable in a stripped module, and patching one slot.
 *
 * The native client is stripped (docs/LINUX_PORT.md: `client.so` exports 383 symbols, but its RTTI names survive: `11CViewRender`,
 * `15C_BaseViewModel`). There is no symbol to dlsym and no address from a static analysis that would survive a game update, so
 * the vtable is found at RUN TIME from the Itanium C++ ABI's RTTI layout, with no offsets:
 *
 *   1. the type's mangled name string "<len><name>\0" sits in .rodata (preceded by the NUL of the previous string);
 *   2. the std::type_info object is { vptr, const char *name, ... }: a pointer-aligned word equal to the string's address, one
 *      word after the object's start;
 *   3. the vtable is { offset_to_top, type_info *, virtual function 0, ... }: a word equal to the type_info's address, preceded
 *      by offset_to_top (0 for the primary vtable, a small negative number for a secondary one) and followed by a pointer into
 *      executable memory. The address a C++ object's vptr holds is the address of that word's successor (function 0).
 *
 * Everything is read from /proc/self/maps ranges (no dl_iterate_phdr, no glibc symbol newer than the shim already uses), so the
 * header is usable from the 32-bit shim exactly as from the 64-bit host tests. It is plain C that is also valid C++ (the test
 * tests/linux/rtti_test.cpp uses real g++ RTTI as ground truth).
 *
 * Reading memory: only ranges the kernel reports as readable are touched, and never past their end.
 */
#ifndef BMVR_RTTI_H
#define BMVR_RTTI_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

struct bmvr_map {
    uintptr_t lo, hi;      /* [lo, hi) */
    char perms[5];         /* "r-xp" */
    char path[192];        /* may be empty (anonymous) */
};

#define BMVR_RTTI_MAX_RANGES 96

static inline int bmvr_hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* One line of /proc/self/maps: "7f..-7f.. r-xp 00000000 08:01 1234    /path". 1 on success. */
static inline int bmvr_map_parse(const char *s, struct bmvr_map *m)
{
    uint64_t v = 0;
    int d, n = 0;
    while ((d = bmvr_hexval(*s)) >= 0) { v = (v << 4) | (uint64_t)d; s++; n++; }
    if (!n || *s != '-') return 0;
    m->lo = (uintptr_t)v;
    s++; v = 0; n = 0;
    while ((d = bmvr_hexval(*s)) >= 0) { v = (v << 4) | (uint64_t)d; s++; n++; }
    if (!n || *s != ' ') return 0;
    m->hi = (uintptr_t)v;
    s++;
    for (int i = 0; i < 4; i++) { if (!s[i]) return 0; m->perms[i] = s[i]; }
    m->perms[4] = 0;
    s += 4;
    for (int field = 0; field < 3; field++) {            /* offset, dev, inode */
        while (*s == ' ') s++;
        while (*s && *s != ' ') s++;
    }
    while (*s == ' ') s++;
    size_t i = 0;
    while (*s && *s != '\n' && i + 1 < sizeof m->path) m->path[i++] = *s++;
    m->path[i] = 0;
    return 1;
}

/* Does the mapping belong to `module`? "exe" = the main program; otherwise a substring of the path ("/client.so"). */
static inline int bmvr_map_in_module(const struct bmvr_map *m, const char *module, const char *exe_path)
{
    if (!module || !*module || !m->path[0]) return 0;
    if (!strcmp(module, "exe")) return exe_path && !strcmp(m->path, exe_path);
    return strstr(m->path, module) != NULL;
}

static inline int bmvr_exe_path(char *out, size_t cap)
{
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) return 0;
    out[n] = 0;
    return 1;
}

/* The readable mappings of a module, in address order. Returns how many were stored (up to max). */
static inline int bmvr_maps_module(const char *module, struct bmvr_map *out, int max)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char exe[512] = "", line[640];
    bmvr_exe_path(exe, sizeof exe);
    int n = 0;
    while (n < max && fgets(line, sizeof line, f)) {
        struct bmvr_map m;
        if (!bmvr_map_parse(line, &m) || m.perms[0] != 'r') continue;
        if (!bmvr_map_in_module(&m, module, exe)) continue;
        out[n++] = m;
    }
    fclose(f);
    return n;
}

/* The mapping that holds `addr`. 1 and *out filled, or 0. */
static inline int bmvr_maps_find(uintptr_t addr, struct bmvr_map *out)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[640];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        struct bmvr_map m;
        if (!bmvr_map_parse(line, &m)) continue;
        if (addr >= m.lo && addr < m.hi) { *out = m; found = 1; break; }
    }
    fclose(f);
    return found;
}

static inline int bmvr_addr_executable(uintptr_t addr)
{
    struct bmvr_map m;
    return addr && bmvr_maps_find(addr, &m) && m.perms[0] == 'r' && m.perms[2] == 'x';
}

/* ------------------------------------------------------------------ finding a class by its RTTI */
struct bmvr_vt_hit {
    uintptr_t typeinfo;       /* the std::type_info object */
    uintptr_t vtable;         /* the address a vptr holds: virtual function 0 */
    intptr_t  offset_to_top;  /* 0 = primary vtable of the class; small and negative = a secondary base's vtable */
    int       primary;
};

/* Calls found(addr) for each copy of the `len` bytes `s` in [lo, hi); with `boundary`, only copies that start a string
 * (at the range start, or right after a NUL). */
typedef void (*bmvr_found_fn)(uintptr_t addr, void *ud);
static inline void bmvr_scan_string(uintptr_t lo, uintptr_t hi, const char *s, size_t len, bmvr_found_fn found, void *ud, int boundary)
{
    const unsigned char *p = (const unsigned char *)lo, *end = (const unsigned char *)hi;
    while (p + len <= end) {
        const unsigned char *q = (const unsigned char *)memchr(p, (unsigned char)s[0], (size_t)(end - p));
        if (!q || q + len > end) break;
        if (!memcmp(q, s, len) && (!boundary || q == (const unsigned char *)lo || q[-1] == 0)) found((uintptr_t)q, ud);
        p = q + 1;
    }
}

struct bmvr_word_hits { uintptr_t addr[16]; int n; };
static inline void bmvr_scan_word(uintptr_t lo, uintptr_t hi, uintptr_t value, struct bmvr_word_hits *h)
{
    const size_t w = sizeof(uintptr_t);
    uintptr_t p = (lo + (w - 1)) & ~(uintptr_t)(w - 1);
    for (; p + w <= hi && h->n < 16; p += w)
        if (*(const uintptr_t *)p == value) h->addr[h->n++] = p;
}

struct bmvr_name_ctx { uintptr_t addr[8]; int n; };
static inline void bmvr_name_found(uintptr_t a, void *ud)
{
    struct bmvr_name_ctx *c = (struct bmvr_name_ctx *)ud;
    if (c->n < 8) c->addr[c->n++] = a;
}

/*
 * Every vtable (primary and secondary) of the class whose Itanium mangled name is `mangled` ("11CViewRender") in `module`.
 * Returns how many were stored. The primary vtable of a class that has virtual functions appears exactly once.
 */
static inline int bmvr_rtti_find(const char *module, const char *mangled, struct bmvr_vt_hit *out, int max)
{
    struct bmvr_map mod[BMVR_RTTI_MAX_RANGES];
    const int nm = bmvr_maps_module(module, mod, BMVR_RTTI_MAX_RANGES);
    if (!nm || !mangled || !*mangled) return 0;
    const size_t w = sizeof(uintptr_t), len = strlen(mangled) + 1;   /* with the NUL */

    struct bmvr_name_ctx names;
    names.n = 0;
    for (int i = 0; i < nm; i++) bmvr_scan_string(mod[i].lo, mod[i].hi, mangled, len, bmvr_name_found, &names, 0);   /* no boundary: GCC puts the name in its own section, and the data before it need not end in NUL */

    int count = 0;
    for (int ni = 0; ni < names.n; ni++) {
        struct bmvr_word_hits th;
        th.n = 0;
        for (int i = 0; i < nm; i++) bmvr_scan_word(mod[i].lo, mod[i].hi, names.addr[ni], &th);
        for (int ti = 0; ti < th.n; ti++) {
            const uintptr_t typeinfo = th.addr[ti] - w;     /* { vptr, name }: the name pointer is the SECOND word */
            int in_mod = 0;
            for (int i = 0; i < nm; i++) if (typeinfo >= mod[i].lo && typeinfo + 2 * w <= mod[i].hi) in_mod = 1;
            if (!in_mod || !*(const uintptr_t *)typeinfo) continue;     /* its vptr (to __class_type_info's vtable) must exist */

            struct bmvr_word_hits vh;
            vh.n = 0;
            for (int i = 0; i < nm; i++) bmvr_scan_word(mod[i].lo, mod[i].hi, typeinfo, &vh);
            for (int vi = 0; vi < vh.n && count < max; vi++) {
                const uintptr_t at = vh.addr[vi];
                int ok_range = 0;
                for (int i = 0; i < nm; i++) if (at - w >= mod[i].lo && at + 2 * w <= mod[i].hi) ok_range = 1;
                if (!ok_range) continue;
                const intptr_t ott = *(const intptr_t *)(at - w);
                const uintptr_t slot0 = *(const uintptr_t *)(at + w);
                /* A __si_class_type_info's base pointer also equals the base's type_info, but it is preceded by a name
                 * pointer (huge), not an offset_to_top (0 or small). */
                const int small = ott == 0 || (ott < 0 && ott > -65536) || (ott > 0 && ott < 65536);
                if (!small || !bmvr_addr_executable(slot0)) continue;
                out[count].typeinfo = typeinfo;
                out[count].vtable = at + w;
                out[count].offset_to_top = ott;
                out[count].primary = ott == 0;
                count++;
            }
        }
    }
    return count;
}

/*
 * Every class that derives from the class whose std::type_info is `base` through SINGLE inheritance (a __si_class_type_info: { vptr, name, base_type }),
 * transitively (three levels), with its primary vtable. A derived class has a vtable of its own, so a patch of the base class's vtable slot is never seen
 * when the program instantiates the subclass: this is how those subclasses are found without knowing their names. out[].typeinfo is the subclass's
 * type_info (its name is the second word). Returns how many were stored.
 */
static inline int bmvr_rtti_derived(const char *module, uintptr_t base, struct bmvr_vt_hit *out, int max)
{
    struct bmvr_map mod[BMVR_RTTI_MAX_RANGES];
    const int nm = bmvr_maps_module(module, mod, BMVR_RTTI_MAX_RANGES);
    if (!nm) return 0;
    const size_t w = sizeof(uintptr_t);
#define BMVR_IN_MOD(a, len, r) ({ int _in = 0; for (int _i = 0; _i < nm; _i++) if ((a) >= mod[_i].lo && (a) + (len) <= mod[_i].hi) _in = 1; _in; })
    uintptr_t level[16], seen[32];
    int nl = 1, nseen = 0, count = 0;
    level[0] = base;
    seen[nseen++] = base;
    for (int depth = 0; depth < 3 && nl; depth++) {
        uintptr_t next[16];
        int nn = 0;
        for (int li = 0; li < nl; li++) {
            struct bmvr_word_hits bh;
            bh.n = 0;
            for (int i = 0; i < nm; i++) bmvr_scan_word(mod[i].lo, mod[i].hi, level[li], &bh);
            for (int hi = 0; hi < bh.n; hi++) {
                if (bh.addr[hi] < 2 * w) continue;
                const uintptr_t der = bh.addr[hi] - 2 * w;                  /* the base pointer is the THIRD word of a __si_class_type_info */
                if (!BMVR_IN_MOD(der, 3 * w, 0)) continue;
                const uintptr_t name = *(const uintptr_t *)(der + w);
                if (!name || !BMVR_IN_MOD(name, 2, 0) || *(const char *)name < '0' || *(const char *)name > '9') continue;   /* a vtable's offset_to_top is not a name */
                if (!*(const uintptr_t *)der) continue;
                int dup = 0;
                for (int k = 0; k < nseen; k++) if (seen[k] == der) dup = 1;
                if (dup || nseen >= 32) continue;
                seen[nseen++] = der;
                if (nn < 16) next[nn++] = der;
                struct bmvr_word_hits vh;
                vh.n = 0;
                for (int i = 0; i < nm; i++) bmvr_scan_word(mod[i].lo, mod[i].hi, der, &vh);
                for (int vi = 0; vi < vh.n && count < max; vi++) {
                    const uintptr_t at = vh.addr[vi];
                    if (!BMVR_IN_MOD(at - w, 3 * w, 0)) continue;
                    const intptr_t ott = *(const intptr_t *)(at - w);
                    const uintptr_t slot0 = *(const uintptr_t *)(at + w);
                    if (ott != 0 || !bmvr_addr_executable(slot0)) continue;
                    out[count].typeinfo = der;
                    out[count].vtable = at + w;
                    out[count].offset_to_top = 0;
                    out[count].primary = 1;
                    count++;
                }
            }
        }
        for (int i = 0; i < nn; i++) level[i] = next[i];
        nl = nn;
    }
#undef BMVR_IN_MOD
    return count;
}

/* Every class in `module` whose Itanium name is "<len><identifier>" and ends with `suffix` ("ViewRender"): the diagnostic that
 * tells which class names exist when the configured one is not found. cb(mangled name, address of the string). */
typedef void (*bmvr_rtti_cb)(const char *mangled, uintptr_t name_addr, void *ud);
struct bmvr_list_ctx { bmvr_rtti_cb cb; void *ud; uintptr_t lo; int n; };
static inline void bmvr_list_found(uintptr_t a, void *ud)
{
    struct bmvr_list_ctx *c = (struct bmvr_list_ctx *)ud;
    const char *e = (const char *)a;                            /* points at the suffix; walk back over [A-Za-z0-9_] */
    const char *s = e;
    int steps = 0;
    while (s > (const char *)c->lo && steps < 160 && ((s[-1] >= '0' && s[-1] <= '9') || (s[-1] >= 'a' && s[-1] <= 'z') ||
                                                      (s[-1] >= 'A' && s[-1] <= 'Z') || s[-1] == '_')) { s--; steps++; }
    /* The name may be glued to stray bytes before it ("4B11CViewRender"): take the first digit run whose value is exactly the
     * length of what follows ("<len><identifier>"). */
    for (const char *p = s; p < e; p++) {
        if (*p < '0' || *p > '9' || (p > s && p[-1] >= '0' && p[-1] <= '9')) continue;
        unsigned len = 0;
        const char *d = p;
        while (*d >= '0' && *d <= '9' && len < 1000) { len = len * 10u + (unsigned)(*d - '0'); d++; }
        if (len && strlen(d) == len) { c->n++; c->cb(p, (uintptr_t)p, c->ud); return; }
    }
}
static inline int bmvr_rtti_list(const char *module, const char *suffix, bmvr_rtti_cb cb, void *ud)
{
    struct bmvr_map mod[BMVR_RTTI_MAX_RANGES];
    const int nm = bmvr_maps_module(module, mod, BMVR_RTTI_MAX_RANGES);
    struct bmvr_list_ctx c;
    c.cb = cb; c.ud = ud; c.n = 0;
    const size_t len = strlen(suffix) + 1;
    for (int i = 0; i < nm; i++) {
        c.lo = mod[i].lo;
        bmvr_scan_string(mod[i].lo, mod[i].hi, suffix, len, bmvr_list_found, &c, 0);
    }
    return c.n;
}

/* ------------------------------------------------------------------ patching one vtable slot */
/*
 * Store `newfn` into *slot, returning the old value in *oldfn. A vtable normally lives in .data.rel.ro, which the dynamic loader
 * made read-only after relocation (RELRO), so the page is made writable for the store and put back EXACTLY as the kernel
 * reported it (never a guess: a writable vtable page must stay writable). One aligned pointer store: a thread that is calling
 * through the slot right now sees either the old or the new function. 1 on success; 0 and nothing changed otherwise.
 */
static inline int bmvr_vtable_patch(uintptr_t *slot, uintptr_t newfn, uintptr_t *oldfn)
{
    struct bmvr_map m;
    if (!bmvr_maps_find((uintptr_t)slot, &m) || m.perms[0] != 'r') return 0;
    *oldfn = *slot;
    if (m.perms[1] == 'w') { __atomic_store_n(slot, newfn, __ATOMIC_SEQ_CST); return 1; }
    const long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) return 0;
    void *page = (void *)((uintptr_t)slot & ~(uintptr_t)(ps - 1));
    const int restore = PROT_READ | (m.perms[2] == 'x' ? PROT_EXEC : 0);
    if (mprotect(page, (size_t)ps, PROT_READ | PROT_WRITE) != 0) return 0;
    __atomic_store_n(slot, newfn, __ATOMIC_SEQ_CST);
    mprotect(page, (size_t)ps, restore);
    return 1;
}

#endif