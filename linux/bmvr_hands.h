/*
 * Black Mesa VR - native Linux port, stage 5 slice 5: the hand models (the CPU half).
 *
 * The Windows mod draws the ripped HEV-glove / bare-hand GLB meshes (VR/hands/NAME.glb) at the controllers, bends the fingers with the
 * "summary curl" mechanism (src/vr_hands.cpp, BuildSummaryCurlPalette) and places them with BuildControllerWorld (vr_hand_math.h).
 * This header is the same logic without the engine, D3D or OpenVR, so tests/linux/hands_test.c can check it against the real files:
 *   - a small inflate + PNG decoder (the GLB embeds its texture as PNG; 8-bit RGB / RGBA, no interlace),
 *   - a small JSON reader and the GLB loader (one skinned mesh, one skin; POSITION / NORMAL / TEXCOORD_0 / JOINTS_0 / WEIGHTS_0),
 *   - the finger rig: joint lookup by name, bind-local matrices, curl palette, the controller -> world matrix.
 * The GL drawing lives in bmvr_preload.c (section "hands").
 *
 * Matrices are row-major float[16] (m[row * 4 + col]) for column vectors, like the Windows code's Get(row, col). glTF stores them
 * column-major, so they are transposed on load.
 */
#ifndef BMVR_HANDS_H
#define BMVR_HANDS_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BH_MAXJ 32

/* ------------------------------------------------------------------ inflate (RFC 1951), puff-style */
struct bh_inf { const uint8_t *in; size_t inlen, incnt; uint32_t bitbuf; int bitcnt; uint8_t *out; size_t outlen, outcnt; int err; };
struct bh_huff { uint16_t count[16]; uint16_t symbol[320]; };

static inline int bh_bits(struct bh_inf *s, int need)
{
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt >= s->inlen) { s->err = 1; return 0; }
        val |= (uint32_t)s->in[s->incnt++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1u));
}

static inline int bh_decode(struct bh_inf *s, const struct bh_huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= bh_bits(s, 1);
        if (s->err) return -1;
        const int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}

static inline int bh_construct(struct bh_huff *h, const short *length, int n)
{
    short offs[16];
    for (int i = 0; i <= 15; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[length[i]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= 15; len++) { left <<= 1; left -= h->count[len]; if (left < 0) return left; }
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++) if (length[i] != 0) h->symbol[offs[length[i]]++] = (uint16_t)i;
    return left;
}

static inline int bh_codes(struct bh_inf *s, const struct bh_huff *lc, const struct bh_huff *dc)
{
    static const short lens[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const short dists[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    for (;;) {
        int sym = bh_decode(s, lc);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s->outcnt >= s->outlen) return -1;
            s->out[s->outcnt++] = (uint8_t)sym;
        } else if (sym == 256) return 0;
        else {
            sym -= 257;
            if (sym >= 29) return -1;
            int len = lens[sym] + bh_bits(s, lext[sym]);
            int ds = bh_decode(s, dc);
            if (ds < 0 || ds >= 30) return -1;
            size_t dist = (size_t)dists[ds] + (size_t)bh_bits(s, dext[ds]);
            if (s->err || dist > s->outcnt || s->outcnt + (size_t)len > s->outlen) return -1;
            for (int i = 0; i < len; i++) { s->out[s->outcnt] = s->out[s->outcnt - dist]; s->outcnt++; }
        }
    }
}

/* Raw deflate stream -> out (exactly outlen bytes expected). Returns the number of bytes written, or -1. */
static inline long bh_inflate(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen)
{
    struct bh_inf s; memset(&s, 0, sizeof s);
    s.in = in; s.inlen = inlen; s.out = out; s.outlen = outlen;
    int last;
    do {
        last = bh_bits(&s, 1);
        const int type = bh_bits(&s, 2);
        if (s.err) return -1;
        if (type == 0) {
            s.bitbuf = 0; s.bitcnt = 0;
            if (s.incnt + 4 > s.inlen) return -1;
            unsigned len = s.in[s.incnt] | (s.in[s.incnt + 1] << 8);
            unsigned nlen = s.in[s.incnt + 2] | (s.in[s.incnt + 3] << 8);
            s.incnt += 4;
            if (len != (~nlen & 0xffffu) || s.incnt + len > s.inlen || s.outcnt + len > s.outlen) return -1;
            memcpy(s.out + s.outcnt, s.in + s.incnt, len);
            s.incnt += len; s.outcnt += len;
        } else if (type == 1) {
            static struct bh_huff lc, dc; static int built;
            if (!built) {
                short l[320]; int i;
                for (i = 0; i < 144; i++) l[i] = 8;
                for (; i < 256; i++) l[i] = 9;
                for (; i < 280; i++) l[i] = 7;
                for (; i < 288; i++) l[i] = 8;
                bh_construct(&lc, l, 288);
                for (i = 0; i < 30; i++) l[i] = 5;
                bh_construct(&dc, l, 30);
                built = 1;
            }
            if (bh_codes(&s, &lc, &dc)) return -1;
        } else if (type == 2) {
            static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            short lengths[320]; struct bh_huff lc, dc;
            int nlen = bh_bits(&s, 5) + 257, ndist = bh_bits(&s, 5) + 1, ncode = bh_bits(&s, 4) + 4;
            if (s.err || nlen > 286 || ndist > 30) return -1;
            int i;
            for (i = 0; i < ncode; i++) lengths[order[i]] = (short)bh_bits(&s, 3);
            for (; i < 19; i++) lengths[order[i]] = 0;
            if (bh_construct(&lc, lengths, 19) != 0) return -1;
            int idx = 0;
            while (idx < nlen + ndist) {
                int sym = bh_decode(&s, &lc);
                if (sym < 0) return -1;
                if (sym < 16) lengths[idx++] = (short)sym;
                else {
                    int len = 0, rep;
                    if (sym == 16) { if (idx == 0) return -1; len = lengths[idx - 1]; rep = 3 + bh_bits(&s, 2); }
                    else if (sym == 17) rep = 3 + bh_bits(&s, 3);
                    else rep = 11 + bh_bits(&s, 7);
                    if (idx + rep > nlen + ndist) return -1;
                    while (rep--) lengths[idx++] = (short)len;
                }
            }
            if (lengths[256] == 0) return -1;
            if (bh_construct(&lc, lengths, nlen) < 0) return -1;
            if (bh_construct(&dc, lengths + nlen, ndist) < 0) return -1;
            if (bh_codes(&s, &lc, &dc)) return -1;
        } else return -1;
    } while (!last);
    return (long)s.outcnt;
}

/* ------------------------------------------------------------------ PNG -> RGBA8 */
static inline uint32_t bh_be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

/* Decodes an 8-bit RGB or RGBA, non-interlaced PNG. *out is malloc'd (w * h * 4 bytes). Returns 1 on success. */
static inline int bh_png_decode(const uint8_t *d, size_t n, int *w, int *h, uint8_t **out)
{
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (n < 33 || memcmp(d, sig, 8)) return 0;
    uint32_t W = 0, H = 0; int ct = -1, bd = 0, il = 0;
    uint8_t *z = NULL; size_t zlen = 0;
    size_t p = 8;
    while (p + 12 <= n) {
        const uint32_t len = bh_be32(d + p);
        const uint8_t *t = d + p + 4;
        if (p + 12 + (size_t)len > n) break;
        if (!memcmp(t, "IHDR", 4) && len >= 13) { W = bh_be32(t + 4); H = bh_be32(t + 8); bd = t[12]; ct = t[13]; il = t[16]; }
        else if (!memcmp(t, "IDAT", 4)) {
            uint8_t *nz = (uint8_t *)realloc(z, zlen + len);
            if (!nz) { free(z); return 0; }
            z = nz; memcpy(z + zlen, t + 4, len); zlen += len;
        } else if (!memcmp(t, "IEND", 4)) break;
        p += 12 + (size_t)len;
    }
    if (!z || bd != 8 || il != 0 || (ct != 2 && ct != 6) || W < 1 || H < 1 || W > 8192 || H > 8192 || zlen < 6) { free(z); return 0; }
    const size_t bpp = ct == 6 ? 4 : 3, stride = (size_t)W * bpp, rawlen = (stride + 1) * H;
    uint8_t *raw = (uint8_t *)malloc(rawlen);
    if (!raw) { free(z); return 0; }
    const long got = bh_inflate(z + 2, zlen - 2, raw, rawlen);   /* skip the 2-byte zlib header */
    free(z);
    if (got != (long)rawlen) { free(raw); return 0; }
    uint8_t *img = (uint8_t *)malloc((size_t)W * H * 4);
    if (!img) { free(raw); return 0; }
    for (uint32_t y = 0; y < H; y++) {
        uint8_t *row = raw + (size_t)y * (stride + 1) + 1;
        const uint8_t *prev = y ? raw + (size_t)(y - 1) * (stride + 1) + 1 : NULL;
        const int f = raw[(size_t)y * (stride + 1)];
        for (size_t i = 0; i < stride; i++) {
            const int a = i >= bpp ? row[i - bpp] : 0, b = prev ? prev[i] : 0, c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            int v = row[i];
            switch (f) {
            case 0: break;
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: { int pp = a + b - c, pa = abs(pp - a), pb = abs(pp - b), pc = abs(pp - c); v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c); break; }
            default: free(raw); free(img); return 0;
            }
            row[i] = (uint8_t)v;
        }
        for (uint32_t x = 0; x < W; x++) {
            uint8_t *o = img + ((size_t)y * W + x) * 4;
            o[0] = row[x * bpp]; o[1] = row[x * bpp + 1]; o[2] = row[x * bpp + 2]; o[3] = bpp == 4 ? row[x * bpp + 3] : 255;
        }
    }
    free(raw);
    *w = (int)W; *h = (int)H; *out = img;
    return 1;
}

/* ------------------------------------------------------------------ a small JSON reader (enough for glTF) */
#define BJ_MAX 6000
enum { BJ_NULL, BJ_BOOL, BJ_NUM, BJ_STR, BJ_ARR, BJ_OBJ };
struct bj_val { int type, first, next, count; const char *s; int slen; double num; const char *key; int klen; };
struct bj_doc { struct bj_val v[BJ_MAX]; int n; const char *p, *end; int err; };

static inline void bj_ws(struct bj_doc *d) { while (d->p < d->end && (*d->p == ' ' || *d->p == '\n' || *d->p == '\r' || *d->p == '\t')) d->p++; }

static inline int bj_value(struct bj_doc *d, int depth);

static inline int bj_string(struct bj_doc *d, const char **s, int *len)
{
    if (d->p >= d->end || *d->p != '"') return 0;
    d->p++;
    *s = d->p;
    while (d->p < d->end && *d->p != '"') { if (*d->p == '\\') d->p++; d->p++; }
    if (d->p >= d->end) return 0;
    *len = (int)(d->p - *s);
    d->p++;
    return 1;
}

static inline int bj_value(struct bj_doc *d, int depth)
{
    if (d->n >= BJ_MAX || depth > 24) { d->err = 1; return -1; }
    bj_ws(d);
    if (d->p >= d->end) { d->err = 1; return -1; }
    const int me = d->n++;
    struct bj_val *v = &d->v[me];
    memset(v, 0, sizeof *v);
    v->first = v->next = -1;
    const char c = *d->p;
    if (c == '{' || c == '[') {
        v->type = c == '{' ? BJ_OBJ : BJ_ARR;
        d->p++;
        int last = -1;
        for (;;) {
            bj_ws(d);
            if (d->p < d->end && *d->p == (c == '{' ? '}' : ']')) { d->p++; break; }
            const char *k = NULL; int kl = 0;
            if (c == '{') { if (!bj_string(d, &k, &kl)) { d->err = 1; return -1; } bj_ws(d); if (d->p >= d->end || *d->p != ':') { d->err = 1; return -1; } d->p++; }
            const int ch = bj_value(d, depth + 1);
            if (ch < 0) return -1;
            d->v[ch].key = k; d->v[ch].klen = kl;
            if (last < 0) d->v[me].first = ch; else d->v[last].next = ch;
            last = ch; d->v[me].count++;
            bj_ws(d);
            if (d->p < d->end && *d->p == ',') d->p++;
        }
    } else if (c == '"') {
        v->type = BJ_STR;
        if (!bj_string(d, &v->s, &v->slen)) { d->err = 1; return -1; }
    } else if (c == 't' || c == 'f') { v->type = BJ_BOOL; v->num = c == 't'; d->p += c == 't' ? 4 : 5; }
    else if (c == 'n') { v->type = BJ_NULL; d->p += 4; }
    else { char *e; v->type = BJ_NUM; v->num = strtod(d->p, &e); if (e == d->p) { d->err = 1; return -1; } d->p = e; }
    return me;
}

static inline int bj_parse(struct bj_doc *d, const char *s, size_t n)
{
    d->n = 0; d->p = s; d->end = s + n; d->err = 0;
    return bj_value(d, 0) == 0 && !d->err;
}
static inline int bj_get(const struct bj_doc *d, int obj, const char *key)
{
    if (obj < 0 || d->v[obj].type != BJ_OBJ) return -1;
    const size_t kl = strlen(key);
    for (int c = d->v[obj].first; c >= 0; c = d->v[c].next) if ((size_t)d->v[c].klen == kl && !memcmp(d->v[c].key, key, kl)) return c;
    return -1;
}
static inline int bj_at(const struct bj_doc *d, int arr, int i)
{
    if (arr < 0 || d->v[arr].type != BJ_ARR) return -1;
    int c = d->v[arr].first;
    while (c >= 0 && i-- > 0) c = d->v[c].next;
    return c;
}
static inline double bj_num(const struct bj_doc *d, int v, double def) { return (v >= 0 && d->v[v].type == BJ_NUM) ? d->v[v].num : def; }

/* ------------------------------------------------------------------ 4x4 matrices, row-major, column vectors */
static inline void bh_ident(float m[16]) { memset(m, 0, 16 * sizeof(float)); m[0] = m[5] = m[10] = m[15] = 1.0f; }
static inline void bh_mul(const float a[16], const float b[16], float o[16])
{
    float t[16];
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
        float v = 0.0f;
        for (int k = 0; k < 4; k++) v += a[r * 4 + k] * b[k * 4 + c];
        t[r * 4 + c] = v;
    }
    memcpy(o, t, sizeof t);
}
static inline int bh_inv(const float in[16], float out[16])
{
    float a[4][8];
    for (int r = 0; r < 4; r++) { for (int c = 0; c < 4; c++) a[r][c] = in[r * 4 + c]; for (int c = 0; c < 4; c++) a[r][4 + c] = r == c; }
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (fabsf(a[r][c]) > fabsf(a[p][c])) p = r;
        if (fabsf(a[p][c]) < 1e-9f) return 0;
        if (p != c) for (int k = 0; k < 8; k++) { float t = a[c][k]; a[c][k] = a[p][k]; a[p][k] = t; }
        const float iv = 1.0f / a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] *= iv;
        for (int r = 0; r < 4; r++) if (r != c) { const float f = a[r][c]; if (f != 0.0f) for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k]; }
    }
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) out[r * 4 + c] = a[r][4 + c];
    return 1;
}
static inline void bh_rotz(float rad, float o[16]) { bh_ident(o); o[0] = cosf(rad); o[1] = -sinf(rad); o[4] = sinf(rad); o[5] = cosf(rad); }
static inline void bh_roty(float rad, float o[16]) { bh_ident(o); o[0] = cosf(rad); o[2] = sinf(rad); o[8] = -sinf(rad); o[10] = cosf(rad); }

/* ------------------------------------------------------------------ the GLB asset */
struct bh_asset {
    int nv, ni, nj;
    float *vert;                    /* interleaved, 16 floats per vertex: pos3 nrm3 uv2 joints4 (as floats) weights4 */
    uint32_t *idx;
    int parent[BH_MAXJ];
    char name[BH_MAXJ][40];
    float ibm[BH_MAXJ][16];         /* inverse bind matrices */
    float bind[BH_MAXJ][16];        /* their inverses: the joints' model-space bind poses */
    int tw, th; uint8_t *tex;       /* RGBA8 texture, row 0 = top (glTF UV origin) */
    int right;                      /* a wrist_r joint exists */
    int finger[5][3];               /* thumb index middle ring pinky, three segments each */
    float bind_local[BH_MAXJ][16];
    int order[BH_MAXJ];             /* parents first */
    int rig_ok;
};

static inline void bh_free(struct bh_asset *a) { free(a->vert); free(a->idx); free(a->tex); memset(a, 0, sizeof *a); }

static inline int bh_name_index(const struct bh_asset *a, const char *n)
{
    for (int i = 0; i < a->nj; i++) if (!strcmp(a->name[i], n)) return i;
    return -1;
}

/* Reads the accessor `acc` as floats (ncomp per element, normalised ints are not used by this asset). */
static inline int bh_read_acc(const struct bj_doc *d, int accessors, int views, const uint8_t *bin, size_t binlen, int acc, int ncomp, int want_uint, float *outf, uint32_t *outu, int count)
{
    const int a = bj_at(d, accessors, acc);
    if (a < 0) return 0;
    const int bv = bj_at(d, views, (int)bj_num(d, bj_get(d, a, "bufferView"), -1));
    const int ct = (int)bj_num(d, bj_get(d, a, "componentType"), 0);
    const int cnt = (int)bj_num(d, bj_get(d, a, "count"), 0);
    if (bv < 0 || cnt != count) return 0;
    const size_t off = (size_t)bj_num(d, bj_get(d, bv, "byteOffset"), 0) + (size_t)bj_num(d, bj_get(d, a, "byteOffset"), 0);
    const size_t csz = (ct == 5126 || ct == 5125) ? 4 : (ct == 5123 ? 2 : 1);
    size_t stride = (size_t)bj_num(d, bj_get(d, bv, "byteStride"), 0);
    if (!stride) stride = csz * (size_t)ncomp;
    if (off + stride * (size_t)(cnt - 1) + csz * (size_t)ncomp > binlen) return 0;
    for (int i = 0; i < cnt; i++) for (int c = 0; c < ncomp; c++) {
        const uint8_t *p = bin + off + stride * (size_t)i + csz * (size_t)c;
        float f = 0.0f; uint32_t u = 0;
        if (ct == 5126) { memcpy(&f, p, 4); u = (uint32_t)f; }
        else if (ct == 5125) { memcpy(&u, p, 4); f = (float)u; }
        else if (ct == 5123) { uint16_t s; memcpy(&s, p, 2); u = s; f = s; }
        else if (ct == 5121) { u = *p; f = *p; }
        else return 0;
        if (want_uint) outu[i * ncomp + c] = u; else outf[i * ncomp + c] = f;
    }
    return 1;
}

/* Loads one skinned hand GLB from memory. Returns 1 on success; *err says what failed otherwise. */
static inline int bh_load_glb(struct bh_asset *a, const uint8_t *d, size_t n, const char **err)
{
    static struct bj_doc doc;       /* the JSON pool is big: one shared copy (loading happens once, on one thread) */
    const char *e = "?";
    memset(a, 0, sizeof *a);
    if (n < 28 || memcmp(d, "glTF", 4)) { *err = "not a GLB"; return 0; }
    uint32_t jl; memcpy(&jl, d + 12, 4);
    if (memcmp(d + 16, "JSON", 4) || 20 + (size_t)jl + 8 > n) { *err = "no JSON chunk"; return 0; }
    const uint8_t *bin = NULL; size_t binlen = 0;
    { uint32_t bl; memcpy(&bl, d + 20 + jl, 4); if (!memcmp(d + 20 + jl + 4, "BIN\0", 4) && 20 + (size_t)jl + 8 + bl <= n) { bin = d + 20 + jl + 8; binlen = bl; } }
    if (!bin) { *err = "no BIN chunk"; return 0; }
    if (!bj_parse(&doc, (const char *)d + 20, jl)) { *err = "bad JSON"; return 0; }
    const int accessors = bj_get(&doc, 0, "accessors"), views = bj_get(&doc, 0, "bufferViews"), nodes = bj_get(&doc, 0, "nodes");
    const int prim = bj_at(&doc, bj_get(&doc, bj_at(&doc, bj_get(&doc, 0, "meshes"), 0), "primitives"), 0);
    const int skin = bj_at(&doc, bj_get(&doc, 0, "skins"), 0);
    if (accessors < 0 || views < 0 || nodes < 0 || prim < 0 || skin < 0) { *err = "missing mesh / skin"; return 0; }
    const int at = bj_get(&doc, prim, "attributes");
    const int aP = (int)bj_num(&doc, bj_get(&doc, at, "POSITION"), -1), aN = (int)bj_num(&doc, bj_get(&doc, at, "NORMAL"), -1),
              aT = (int)bj_num(&doc, bj_get(&doc, at, "TEXCOORD_0"), -1), aJ = (int)bj_num(&doc, bj_get(&doc, at, "JOINTS_0"), -1),
              aW = (int)bj_num(&doc, bj_get(&doc, at, "WEIGHTS_0"), -1), aI = (int)bj_num(&doc, bj_get(&doc, prim, "indices"), -1);
    if (aP < 0 || aN < 0 || aT < 0 || aJ < 0 || aW < 0 || aI < 0) { *err = "an attribute is missing"; return 0; }
    const int nv = (int)bj_num(&doc, bj_get(&doc, bj_at(&doc, accessors, aP), "count"), 0), ni = (int)bj_num(&doc, bj_get(&doc, bj_at(&doc, accessors, aI), "count"), 0);
    if (nv < 3 || nv > 200000 || ni < 3 || ni > 600000) { *err = "implausible mesh size"; return 0; }
    float *p = (float *)malloc(sizeof(float) * 3 * (size_t)nv), *nm = (float *)malloc(sizeof(float) * 3 * (size_t)nv), *uv = (float *)malloc(sizeof(float) * 2 * (size_t)nv),
          *jf = (float *)malloc(sizeof(float) * 4 * (size_t)nv), *wf = (float *)malloc(sizeof(float) * 4 * (size_t)nv);
    a->vert = (float *)malloc(sizeof(float) * 16 * (size_t)nv);
    a->idx = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)ni);
    int ok = p && nm && uv && jf && wf && a->vert && a->idx;
    if (!ok) e = "out of memory";
    if (ok && !(bh_read_acc(&doc, accessors, views, bin, binlen, aP, 3, 0, p, NULL, nv) && bh_read_acc(&doc, accessors, views, bin, binlen, aN, 3, 0, nm, NULL, nv) &&
                bh_read_acc(&doc, accessors, views, bin, binlen, aT, 2, 0, uv, NULL, nv) && bh_read_acc(&doc, accessors, views, bin, binlen, aJ, 4, 0, jf, NULL, nv) &&
                bh_read_acc(&doc, accessors, views, bin, binlen, aW, 4, 0, wf, NULL, nv) && bh_read_acc(&doc, accessors, views, bin, binlen, aI, 1, 1, NULL, a->idx, ni))) { ok = 0; e = "cannot read a mesh accessor"; }
    if (ok) {
        for (int i = 0; i < nv; i++) {
            float *o = a->vert + 16 * (size_t)i;
            memcpy(o, p + 3 * i, 12); memcpy(o + 3, nm + 3 * i, 12); memcpy(o + 6, uv + 2 * i, 8); memcpy(o + 8, jf + 4 * i, 16); memcpy(o + 12, wf + 4 * i, 16);
        }
        for (int i = 0; i < ni; i++) if (a->idx[i] >= (uint32_t)nv) { ok = 0; e = "an index is out of range"; break; }
    }
    free(p); free(nm); free(uv); free(jf); free(wf);
    a->nv = nv; a->ni = ni;

    /* skin: joint names, parents (from the nodes' children lists), inverse bind matrices */
    const int joints = bj_get(&doc, skin, "joints");
    const int nj = joints >= 0 ? doc.v[joints].count : 0;
    if (ok && (nj < 1 || nj > BH_MAXJ)) { ok = 0; e = "the skin has no joints or too many"; }
    if (ok) {
        a->nj = nj;
        int jnode[BH_MAXJ];
        for (int j = 0; j < nj; j++) {
            jnode[j] = (int)bj_num(&doc, bj_at(&doc, joints, j), -1);
            const int nd = bj_at(&doc, nodes, jnode[j]);
            const int nn = bj_get(&doc, nd, "name");
            if (nd < 0 || nn < 0) { ok = 0; e = "a joint node is missing"; break; }
            snprintf(a->name[j], sizeof a->name[j], "%.*s", doc.v[nn].slen, doc.v[nn].s);
            a->parent[j] = -1;
        }
        for (int j = 0; ok && j < nj; j++) {
            const int ch = bj_get(&doc, bj_at(&doc, nodes, jnode[j]), "children");
            for (int c = ch >= 0 ? doc.v[ch].first : -1; c >= 0; c = doc.v[c].next)
                for (int k = 0; k < nj; k++) if (jnode[k] == (int)doc.v[c].num) a->parent[k] = j;
        }
        float *ib = (float *)malloc(sizeof(float) * 16 * (size_t)nj);
        const int ibacc = (int)bj_num(&doc, bj_get(&doc, skin, "inverseBindMatrices"), -1);
        if (ok && ib && ibacc >= 0 && bh_read_acc(&doc, accessors, views, bin, binlen, ibacc, 16, 0, ib, NULL, nj)) {
            for (int j = 0; j < nj; j++) for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) a->ibm[j][r * 4 + c] = ib[j * 16 + c * 4 + r];   /* column-major -> row-major */
            for (int j = 0; j < nj && ok; j++) if (!bh_inv(a->ibm[j], a->bind[j])) { ok = 0; e = "a bind matrix is singular"; }
        } else if (ok) { ok = 0; e = "no inverse bind matrices"; }
        free(ib);
    }
    /* texture */
    if (ok) {
        const int img = bj_at(&doc, bj_get(&doc, 0, "images"), 0);
        const int bv = bj_at(&doc, views, (int)bj_num(&doc, bj_get(&doc, img, "bufferView"), -1));
        const size_t off = (size_t)bj_num(&doc, bj_get(&doc, bv, "byteOffset"), 0), len = (size_t)bj_num(&doc, bj_get(&doc, bv, "byteLength"), 0);
        if (bv < 0 || off + len > binlen || !bh_png_decode(bin + off, len, &a->tw, &a->th, &a->tex)) { a->tex = NULL; a->tw = a->th = 0; }   /* no texture: drawn in a flat skin colour */
    }
    if (!ok) { *err = e; bh_free(a); return 0; }

    /* the finger rig (src/vr_hands.cpp BuildHandRig) */
    a->right = bh_name_index(a, "wrist_r") >= 0;
    const int left = bh_name_index(a, "wrist_l") >= 0;
    if (a->right || left) {
        static const char *const fn[5] = { "thumb", "index", "middle", "ring", "pinky" };
        int good = 1;
        for (int f = 0; f < 5 && good; f++) for (int s = 0; s < 3; s++) {
            char nm[48]; snprintf(nm, sizeof nm, "finger_%s_%d_%c", fn[f], s, a->right ? 'r' : 'l');
            a->finger[f][s] = bh_name_index(a, nm);
            if (a->finger[f][s] < 0) { good = 0; break; }
        }
        if (good) {
            for (int j = 0; j < nj; j++) {
                if (a->parent[j] >= 0) bh_mul(a->ibm[a->parent[j]], a->bind[j], a->bind_local[j]);
                else memcpy(a->bind_local[j], a->bind[j], sizeof a->bind_local[j]);
            }
            int done[BH_MAXJ] = { 0 }, cnt = 0;
            for (int pass = 0; pass < nj && cnt < nj; pass++)
                for (int j = 0; j < nj; j++) if (!done[j] && (a->parent[j] < 0 || done[a->parent[j]])) { a->order[cnt++] = j; done[j] = 1; }
            a->rig_ok = cnt == nj;
        }
    }
    return 1;
}

/* Reads a whole file. Returns a malloc'd buffer or NULL. */
static inline uint8_t *bh_read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (64L << 20)) { fclose(f); return NULL; }
    uint8_t *b = (uint8_t *)malloc((size_t)sz);
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = NULL; }
    fclose(f);
    if (b) *n = (size_t)sz;
    return b;
}

/* ------------------------------------------------------------------ finger curls -> skin palette */
/* curls: thumb, index, middle, ring, pinky; 0 = open, 1 = closed (-0.5 .. 1). pal[j] = joint-model matrix * inverse bind (row-major).
 * Same numbers as the Windows build: the ValveBiped hinges of the ripped gloves are opposite the SteamVR glove's, hence the sign. */
static inline int bh_palette(const struct bh_asset *a, const float curls_in[5], float pal[BH_MAXJ][16])
{
    static const float maxr[5][3] = { { 0.75f, 0.90f, 0.65f }, { 1.15f, 1.25f, 0.90f }, { 1.15f, 1.25f, 0.90f }, { 1.15f, 1.25f, 0.90f }, { 1.15f, 1.25f, 0.90f } };
    if (!a->rig_ok) return 0;
    float local[BH_MAXJ][16], model[BH_MAXJ][16];
    for (int j = 0; j < a->nj; j++) memcpy(local[j], a->bind_local[j], sizeof local[j]);
    for (int f = 0; f < 5; f++) {
        float c = curls_in[f];
        if (c < -0.5f) c = -0.5f;
        if (c > 1.0f) c = 1.0f;
        const float sign = f > 0 ? -1.0f : 1.0f;
        for (int s = 0; s < 3; s++) {
            float r[16];
            bh_rotz(sign * c * maxr[f][s], r);
            bh_mul(local[a->finger[f][s]], r, local[a->finger[f][s]]);
        }
    }
    for (int k = 0; k < a->nj; k++) {
        const int j = a->order[k];
        if (a->parent[j] >= 0) bh_mul(model[a->parent[j]], local[j], model[j]); else memcpy(model[j], local[j], sizeof model[j]);
        bh_mul(model[j], a->ibm[j], pal[j]);
    }
    return 1;
}

/* Curls from the two analog values the Linux input channel has (the Windows OpenXR fallback: no hand tracking, trigger + grip). */
static inline void bh_curls_from_input(float trigger, float grip, float curls[5])
{
    const float rest = 0.10f;
    float t = trigger < 0.0f ? 0.0f : trigger > 1.0f ? 1.0f : trigger, g = grip < 0.0f ? 0.0f : grip > 1.0f ? 1.0f : grip;
    float v;
    v = t * 0.15f; if (g * 0.10f > v) v = g * 0.10f; curls[0] = v > rest ? v : rest;
    v = g * 0.35f; if (t > v) v = t; curls[1] = v > rest ? v : rest;
    for (int i = 2; i < 5; i++) curls[i] = g > rest ? g : rest;
}

/* BuildControllerWorld (vr_hand_math.h): model -> Source world. right / up / forward are the controller's axes in Source (unit),
 * rot_deg = local correction (Rz * Ry * Rx), pos_m = local offset in metres, scale = model scale * Source units per metre. */
static inline void bh_controller_world(const float origin[3], const float right[3], const float up[3], const float fwd[3],
                                       float units_per_m, float model_scale, const float pos_m[3], const float rot_deg[3], float out[16])
{
    const float k = 3.14159265358979f / 180.0f;
    const float sx = sinf(rot_deg[0] * k), cx = cosf(rot_deg[0] * k), sy = sinf(rot_deg[1] * k), cy = cosf(rot_deg[1] * k), sz = sinf(rot_deg[2] * k), cz = cosf(rot_deg[2] * k);
    float basis[16], corr[16], sc[16], t[16];
    bh_ident(basis);
    for (int i = 0; i < 3; i++) { basis[i * 4 + 0] = right[i]; basis[i * 4 + 1] = up[i]; basis[i * 4 + 2] = -fwd[i]; basis[i * 4 + 3] = origin[i]; }
    bh_ident(corr);
    corr[0] = cz * cy; corr[1] = cz * sy * sx - sz * cx; corr[2] = cz * sy * cx + sz * sx;
    corr[4] = sz * cy; corr[5] = sz * sy * sx + cz * cx; corr[6] = sz * sy * cx - cz * sx;
    corr[8] = -sy;     corr[9] = cy * sx;                corr[10] = cy * cx;
    for (int i = 0; i < 3; i++) corr[i * 4 + 3] = pos_m[i] * units_per_m;
    bh_ident(sc);
    sc[0] = sc[5] = sc[10] = model_scale * units_per_m;
    bh_mul(basis, corr, t);
    bh_mul(t, sc, out);
}

/* Off-centre perspective for one eye, OpenGL clip space (z -1..1), from a Source view (x right, y up, z forward in view space).
 * fov_deg / aspect / frac as in struct bmvr_view. flip_y negates clip y (render targets stored top row first). Returns row-major P. */
static inline void bh_projection(float fov_deg, float aspect, const float frac[4], float znear, float zfar, int flip_y, float P[16])
{
    const float hw = tanf(fov_deg * 0.5f * 3.14159265358979f / 180.0f), hh = hw / aspect;
    const float l = -hw + frac[0] * 2.0f * hw, r = -hw + frac[1] * 2.0f * hw, b = -hh + frac[2] * 2.0f * hh, t = -hh + frac[3] * 2.0f * hh;
    memset(P, 0, 16 * sizeof(float));
    P[0] = 2.0f / (r - l);  P[2] = -(r + l) / (r - l);
    P[5] = 2.0f / (t - b);  P[6] = -(t + b) / (t - b);
    P[10] = (zfar + znear) / (zfar - znear); P[11] = -2.0f * zfar * znear / (zfar - znear);
    P[14] = 1.0f;
    if (flip_y) { P[5] = -P[5]; P[6] = -P[6]; }
}

/* View matrix of a Source camera (BuildSourceView): rows right, up, forward. */
static inline void bh_view(const float origin[3], const float right[3], const float up[3], const float fwd[3], float V[16])
{
    bh_ident(V);
    for (int c = 0; c < 3; c++) { V[0 * 4 + c] = right[c]; V[1 * 4 + c] = up[c]; V[2 * 4 + c] = fwd[c]; }
    V[3] = -(right[0] * origin[0] + right[1] * origin[1] + right[2] * origin[2]);
    V[7] = -(up[0] * origin[0] + up[1] * origin[1] + up[2] * origin[2]);
    V[11] = -(fwd[0] * origin[0] + fwd[1] * origin[1] + fwd[2] * origin[2]);
}

#endif
