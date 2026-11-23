/* The transparent probe thunk (linux/bmvr_probe.h), run for real as a 32-bit freestanding program (no libc, so no multilib is needed):
 *   gcc -m32 -O2 -fomit-frame-pointer -ffreestanding -nostdlib -static -fno-pic -no-pie -fno-stack-protector -I linux -o probe_test tests/linux/probe_test.c && ./probe_test
 * The original must see the same stack and registers it would have had, whatever its shape: plain int return, a float in st(0), a struct returned by
 * value (the callee pops the hidden pointer with `ret 4`), and more arguments than the probe looks at. */
#include "bmvr_probe.h"

static long sys3(long n, long a, long b, long c) { long r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory"); return r; }
static void out(const char *s) { int n = 0; while (s[n]) n++; sys3(4, 1, (long)s, n); }
static void num(const char *label, int v) { char b[16]; int i = 15; b[i] = 0; unsigned u = v < 0 ? (unsigned)-v : (unsigned)v; do { b[--i] = '0' + u % 10; u /= 10; } while (u); if (v < 0) b[--i] = '-'; out(label); out(b + i); out("\n"); }
static int fails, checks;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; out("  FAIL  "); out(msg); out("\n"); } } while (0)

static int g_calls[4]; static uint32_t g_args[4][6];
static void probe(int k, const uint32_t *a) { g_calls[k]++; for (int i = 0; i < 6; i++) g_args[k][i] = a[i]; }

struct big { int a, b, c, d; };
__attribute__((noinline)) static int f_int(void *self, int a, int b, int c) { return (int)(long)self + a * 100 + b * 10 + c; }
__attribute__((noinline)) static float f_float(void *self, float x, int y) { (void)self; return x * 2.0f + (float)y; }
__attribute__((noinline)) static struct big f_sret(void *self, int a) { struct big r = { (int)(long)self, a, a + 1, a + 2 }; return r; }
__attribute__((noinline)) static int f_many(void *self, int a, int b, int c, int d, int e) { (void)self; return a + b * 2 + c * 3 + d * 4 + e * 5; }
__attribute__((noinline)) static void f_void(void *self, void *owner, const float *p, const float *q) { (void)self; (void)owner; ((float *)q)[0] = p[0] + 1.0f; }

static uint8_t *alloc_exec(void)   /* mmap2(NULL, 4096, RWX, PRIVATE|ANON) */
{
    long r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(192), "b"(0), "c"(4096), "d"(7), "S"(0x22), "D"(-1), "m"(r) : "memory");
    return (uint8_t *)r;
}

void _start(void)
{
    uint8_t *page = alloc_exec();
    CHECK((long)page > 0 || (long)page < -4096, "mmap");
    typedef int (*t_int)(void *, int, int, int);
    typedef float (*t_float)(void *, float, int);
    typedef struct big (*t_sret)(void *, int);
    typedef int (*t_many)(void *, int, int, int, int, int);
    typedef void (*t_void)(void *, void *, const float *, const float *);
    uintptr_t orig[5] = { (uintptr_t)f_int, (uintptr_t)f_float, (uintptr_t)f_sret, (uintptr_t)f_many, (uintptr_t)f_void };
    int k_of[5] = { 0, 1, 2, 3, 0 };
    for (int i = 0; i < 5; i++) CHECK(bmvr_probe_thunk(page + i * BMVR_PROBE_THUNK_SIZE, (uint32_t)k_of[i], probe, orig[i]) == BMVR_PROBE_THUNK_SIZE, "thunk size");
    t_int ti = (t_int)(page + 0 * BMVR_PROBE_THUNK_SIZE);
    t_float tf = (t_float)(page + 1 * BMVR_PROBE_THUNK_SIZE);
    t_sret ts = (t_sret)(page + 2 * BMVR_PROBE_THUNK_SIZE);
    t_many tm = (t_many)(page + 3 * BMVR_PROBE_THUNK_SIZE);
    t_void tv = (t_void)(page + 4 * BMVR_PROBE_THUNK_SIZE);

    for (int rep = 0; rep < 3; rep++) {   /* repeated: a stack imbalance would show up as a crash or a wrong value */
        CHECK(ti((void *)7, 1, 2, 3) == 7 + 100 + 20 + 3, "int return and arguments pass through");
        CHECK(tf((void *)1, 1.5f, 4) == 7.0f, "float return (st0) survives the probe call");
        struct big b = ts((void *)9, 5);
        CHECK(b.a == 9 && b.b == 5 && b.c == 6 && b.d == 7, "struct return by value (hidden pointer popped by the callee)");
        CHECK(tm((void *)1, 1, 2, 3, 4, 5) == 1 + 4 + 9 + 16 + 25, "five arguments");
        float p[3] = { 41.0f, 0, 0 }, q[3] = { 0, 0, 0 };
        tv((void *)3, (void *)5, p, q);
        CHECK(q[0] == 42.0f, "void function writing through its pointer arguments");
    }
    CHECK(g_calls[0] == 3 + 3 && g_calls[1] == 3 && g_calls[3] == 3, "the probe ran once per call");
    CHECK(g_args[0][0] == 3 && g_args[0][1] == 5, "the probe sees this and the first argument (the last call through k=0 was f_void)");
    CHECK(g_args[3][0] == 1 && g_args[3][1] == 1 && g_args[3][5] == 5, "the probe sees every stack argument it asks for");
    out("probe_test: "); num("checks=", checks); num("failed=", fails);
    sys3(1, fails ? 1 : 0, 0, 0);
    for (;;) { }
}
