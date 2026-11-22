// Stage 4b: finding a class's vtable from its Itanium RTTI, and patching a slot (linux/bmvr_rtti.h). No GPU, no game.
//   g++ -O0 -Wall -Wextra -I linux -o rtti_test tests/linux/rtti_test.cpp && ./rtti_test
// Ground truth is the compiler itself: typeid(T).name() is the mangled name string, and *(void**)obj is the real vptr.
// -O0 on purpose: the patch test must call through the vtable, and an optimiser may devirtualise.
#include <stdio.h>
#include <stdlib.h>
#include <typeinfo>
#include "bmvr_rtti.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct IView { virtual void Init() = 0; virtual void LevelInit() = 0; virtual void Shutdown() = 0; virtual void OnRenderStart() = 0;
               virtual void Render() = 0; virtual int RenderView(int a, int b) = 0; virtual int Other() = 0; virtual ~IView() {} };
struct FooViewRender : IView {                        // stands in for CViewRender: the slot of interest is RenderView
    void Init() override {} void LevelInit() override {} void Shutdown() override {} void OnRenderStart() override {} void Render() override {}
    int RenderView(int a, int b) override { return a + b; }
    int Other() override { return 7; }
};
struct BarViewRender : IView {
    void Init() override {} void LevelInit() override {} void Shutdown() override {} void OnRenderStart() override {} void Render() override {}
    int RenderView(int a, int b) override { return a * b; }
    int Other() override { return 8; }
};
struct ViewRenderer { virtual void f() {} };
struct SubFoo : FooViewRender { int RenderView(int a, int b) override { return a - b; } };      // single inheritance: found without its name
struct SubSubFoo : SubFoo { void Init() override {} };
__attribute__((noinline)) static IView *make_sub() { return new SubFoo(); }
__attribute__((noinline)) static IView *make_subsub() { return new SubSubFoo(); }           // a name that merely CONTAINS ViewRender: must not be listed
struct A { virtual void a() {} int x = 1; };
struct B { virtual void b() {} int y = 2; };
struct C : A, B { void a() override {} void b() override {} };   // C has a primary vtable and a secondary one for B

__attribute__((noinline)) static IView *make_foo() { return new FooViewRender(); }
__attribute__((noinline)) static C *make_c() { return new C(); }
__attribute__((noinline)) static ViewRenderer *make_vr() { return new ViewRenderer(); }
__attribute__((noinline)) static IView *make_bar() { return new BarViewRender(); }

struct Collected { char names[8][64]; int n; };
static void collect(const char *mangled, uintptr_t, void *ud)
{
    Collected *c = (Collected *)ud;
    if (c->n < 8) snprintf(c->names[c->n++], 64, "%s", mangled);
}

int main()
{
    IView *foo = make_foo();
    IView *bar = make_bar();
    C *c = make_c();
    ViewRenderer *vr = make_vr();
    (void)vr;

    // 0. subclasses are found from the base's type_info alone, with their own primary vtables (what the CreateMove hook needs)
    {
        IView *sub = make_sub(), *subsub = make_subsub();
        struct bmvr_vt_hit b[8], d[8];
        int nb = bmvr_rtti_find("exe", typeid(FooViewRender).name(), b, 8), pb = -1;
        for (int i = 0; i < nb; i++) if (b[i].primary) pb = i;
        CHECK(pb >= 0, "base not found");
        int nd = pb >= 0 ? bmvr_rtti_derived("exe", b[pb].typeinfo, d, 8) : 0, got_sub = 0, got_subsub = 0;
        for (int i = 0; i < nd; i++) { if (d[i].vtable == *(uintptr_t *)sub) got_sub = 1; if (d[i].vtable == *(uintptr_t *)subsub) got_subsub = 1; }
        CHECK(nd == 2, "derived classes: %d, want exactly SubFoo and SubSubFoo", nd);
        CHECK(got_sub && got_subsub, "derived vtables match the objects' vptrs (sub %d, subsub %d)", got_sub, got_subsub);
        int nb2 = bmvr_rtti_find("exe", typeid(BarViewRender).name(), b, 8), pb2 = -1;
        for (int i = 0; i < nb2; i++) if (b[i].primary) pb2 = i;
        CHECK(pb2 >= 0 && bmvr_rtti_derived("exe", b[pb2].typeinfo, d, 8) == 0, "a class with no subclass reports none");
    }

    // 1. the primary vtable of a class is found, and it is the one the object's vptr holds
    {
        struct bmvr_vt_hit h[8];
        int n = bmvr_rtti_find("exe", typeid(FooViewRender).name(), h, 8);
        int primaries = 0; uintptr_t pv = 0;
        for (int i = 0; i < n; i++) if (h[i].primary) { primaries++; pv = h[i].vtable; }
        CHECK(primaries == 1, "FooViewRender: %d primary vtable(s), want exactly 1 (hits=%d)", primaries, n);
        CHECK(pv == *(uintptr_t *)foo, "FooViewRender vtable %#lx, the object's vptr is %#lx", (unsigned long)pv, (unsigned long)*(uintptr_t *)foo);
        CHECK(pv && ((void **)pv)[5] != nullptr, "slot 5 (RenderView) empty");
        // slot 5 is RenderView: the address the compiler put there
        int (FooViewRender::*pm)(int, int) = &FooViewRender::RenderView; (void)pm;
        CHECK(bmvr_addr_executable(((uintptr_t *)pv)[5]), "slot 5 is not executable memory");
    }

    // 2. the sibling class has its own, different vtable
    {
        struct bmvr_vt_hit h[8], g[8];
        int n = bmvr_rtti_find("exe", typeid(FooViewRender).name(), h, 8);
        int m = bmvr_rtti_find("exe", typeid(BarViewRender).name(), g, 8);
        uintptr_t fv = 0, bv = 0;
        for (int i = 0; i < n; i++) if (h[i].primary) fv = h[i].vtable;
        for (int i = 0; i < m; i++) if (g[i].primary) bv = g[i].vtable;
        CHECK(fv && bv && fv != bv, "the two classes share a vtable (%#lx %#lx)", (unsigned long)fv, (unsigned long)bv);
        CHECK(bv == *(uintptr_t *)bar, "BarViewRender vtable does not match its object");
    }

    // 3. multiple inheritance: a primary and a secondary vtable, the secondary one flagged and equal to the B subobject's vptr
    {
        struct bmvr_vt_hit h[8];
        int n = bmvr_rtti_find("exe", typeid(C).name(), h, 8);
        int prim = 0, sec = 0; uintptr_t pv = 0, sv = 0;
        for (int i = 0; i < n; i++) { if (h[i].primary) { prim++; pv = h[i].vtable; } else { sec++; sv = h[i].vtable; CHECK(h[i].offset_to_top < 0, "secondary offset_to_top %ld is not negative", (long)h[i].offset_to_top); } }
        CHECK(prim == 1 && sec == 1, "class C: %d primary / %d secondary vtables, want 1 / 1", prim, sec);
        B *b = c;
        CHECK(pv == *(uintptr_t *)c, "C primary vtable does not match the object's vptr");
        CHECK(sv == *(uintptr_t *)b, "C secondary vtable does not match the B subobject's vptr");
    }

    // 4. things that must NOT be found
    {
        struct bmvr_vt_hit h[8];
        CHECK(bmvr_rtti_find("exe", "11NoSuchClass", h, 8) == 0, "a class that does not exist was found");
        CHECK(bmvr_rtti_find("exe", "FooViewRender", h, 8) == 0, "an unmangled name (no length prefix) matched a substring");
        CHECK(bmvr_rtti_find("/no/such/module.so", typeid(FooViewRender).name(), h, 8) == 0, "a module that is not loaded produced hits");
        CHECK(bmvr_rtti_find("exe", "", h, 8) == 0, "an empty name produced hits");
    }

    // 5. the name lister: both ViewRender classes, not ViewRenderer
    {
        Collected col; col.n = 0;
        int n = bmvr_rtti_list("exe", "ViewRender", collect, &col);
        int foo_seen = 0, bar_seen = 0, bad = 0;
        for (int i = 0; i < col.n; i++) {
            if (!strcmp(col.names[i], typeid(FooViewRender).name())) foo_seen = 1;
            else if (!strcmp(col.names[i], typeid(BarViewRender).name())) bar_seen = 1;
            else if (strstr(col.names[i], "ViewRenderer")) bad = 1;
        }
        CHECK(n >= 2 && foo_seen && bar_seen, "lister: n=%d foo=%d bar=%d", n, foo_seen, bar_seen);
        CHECK(!bad, "lister reported ViewRenderer, which does not end in ViewRender");
    }

    // 5b. a name glued to stray bytes (GCC emits the string in its own section; what precedes it need not end in NUL)
    {
        static const char buf[] = "\x34\x42" "11CViewRender";   // like "4B11CViewRender" seen in a real binary
        Collected col; col.n = 0;
        struct bmvr_list_ctx lc; lc.cb = collect; lc.ud = &col; lc.lo = (uintptr_t)buf; lc.n = 0;
        bmvr_list_found((uintptr_t)(buf + 5), &lc);   // the scanner hands over the address of the suffix "ViewRender" (offset 2 + "11C")
        CHECK(col.n == 1 && !strcmp(col.names[0], "11CViewRender"), "glued name: %d names, first '%s'", col.n, col.n ? col.names[0] : "-");
    }

    // 6. patch a slot of a real (RELRO) vtable: calls through the object change, and unpatching restores them
    {
        struct bmvr_vt_hit h[8];
        int n = bmvr_rtti_find("exe", typeid(FooViewRender).name(), h, 8);
        uintptr_t vt = 0;
        for (int i = 0; i < n; i++) if (h[i].primary) vt = h[i].vtable;
        uintptr_t *slot = (uintptr_t *)vt + 5;
        struct bmvr_map m;
        CHECK(bmvr_maps_find((uintptr_t)slot, &m), "the vtable's mapping was not found");
        const int was_ro = m.perms[1] != 'w';
        static int (*repl)(void *, int, int) = [](void *, int a, int b) { return a - b; };
        CHECK(foo->RenderView(10, 3) == 13, "baseline call broke");
        uintptr_t old = 0;
        CHECK(bmvr_vtable_patch(slot, (uintptr_t)repl, &old), "patch failed");
        CHECK(foo->RenderView(10, 3) == 7, "after the patch RenderView(10,3) = %d, want 7 (the replacement)", foo->RenderView(10, 3));
        CHECK(bar->RenderView(10, 3) == 30, "another class was affected by the patch");
        CHECK(foo->Other() == 7, "a neighbouring slot was affected by the patch");
        // the page protection is exactly what it was (read-only stays read-only)
        struct bmvr_map after;
        CHECK(bmvr_maps_find((uintptr_t)slot, &after) && !strcmp(after.perms, m.perms), "protection changed from %s to %s", m.perms, after.perms);
        uintptr_t tmp = 0;
        CHECK(bmvr_vtable_patch(slot, old, &tmp) && tmp == (uintptr_t)repl, "restore failed");
        CHECK(foo->RenderView(10, 3) == 13, "the original behaviour did not come back");
        printf("  (vtable page was %s: %s)\n", m.perms, was_ro ? "read-only, patched through mprotect" : "writable");
    }

    // 7. an address that is not mapped is refused
    {
        uintptr_t old = 0;
        CHECK(!bmvr_vtable_patch((uintptr_t *)(uintptr_t)0x10, 1, &old), "patching an unmapped address succeeded");
    }

    printf("rtti_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
