/*
 * bmvr_inline.h - inline (prologue) detour for ONE 32-bit x86 function, for the case where the engine reaches a function without
 * going through its vtable slot (the stage 4b real run: the patched CViewRender slot was never called).
 *
 *   bmvr_x86_prologue_len()  pure: how many whole instructions cover >= 5 bytes, or 0 when the bytes contain anything this small
 *                            decoder does not know to be position-independent (calls, jumps, rip/eip-relative, prefixes it
 *                            does not handle). It refuses rather than guesses: a refusal leaves the game untouched.
 *   bmvr_inline_hook()       i386 only: trampoline (stolen bytes + jmp back) in a fresh RWX page, then `jmp rel32` over the entry.
 *
 * The decoder covers the instructions GCC emits at the start of a function (push/pop reg, mov/lea/alu with modrm+sib+disp,
 * sub/add esp imm, mov reg imm32, push imm, movzx/movsx, endbr32). Anything else -> 0.
 */
#ifndef BMVR_INLINE_H
#define BMVR_INLINE_H
#include <stddef.h>
#include <stdint.h>

static inline int bmvr_x86_modrm_len(const uint8_t *p)
{
    const unsigned mod = p[0] >> 6, rm = p[0] & 7;
    int n = 1;
    if (mod == 3) return n;
    if (rm == 4) { n++; if (mod == 0 && (p[1] & 7) == 5) return n + 4; }
    if (mod == 0) return rm == 5 ? n + 4 : n;
    return n + (mod == 1 ? 1 : 4);
}

/* Length of the instruction at p (avail bytes readable), or 0 if unsupported. */
static inline int bmvr_x86_insn_len(const uint8_t *p, size_t avail)
{
    if (avail < 8) return 0;
    const uint8_t op = p[0];
    if ((op >= 0x50 && op <= 0x5f) || op == 0x90) return 1;
    if (op == 0x6a) return 2;
    if (op == 0x68 || (op >= 0xb8 && op <= 0xbf)) return 5;
    switch (op) {
    case 0x01: case 0x03: case 0x09: case 0x0b: case 0x21: case 0x23: case 0x29: case 0x2b: case 0x31: case 0x33:
    case 0x39: case 0x3b: case 0x84: case 0x85: case 0x88: case 0x89: case 0x8a: case 0x8b: case 0x8d:
        return 1 + bmvr_x86_modrm_len(p + 1);
    case 0x83: return 1 + bmvr_x86_modrm_len(p + 1) + 1;
    case 0x81: return 1 + bmvr_x86_modrm_len(p + 1) + 4;
    case 0xc6: if (((p[1] >> 3) & 7) != 0) return 0; return 1 + bmvr_x86_modrm_len(p + 1) + 1;
    case 0xc7: if (((p[1] >> 3) & 7) != 0) return 0; return 1 + bmvr_x86_modrm_len(p + 1) + 4;
    case 0x0f:
        if (p[1] == 0xb6 || p[1] == 0xb7 || p[1] == 0xbe || p[1] == 0xbf) return 2 + bmvr_x86_modrm_len(p + 2);
        return 0;
    case 0xf3: if (p[1] == 0x0f && p[2] == 0x1e && p[3] == 0xfb) return 4; return 0;   /* endbr32 */
    default: return 0;
    }
}

/* Bytes to steal (>= 5, whole instructions, <= 16), or 0. */
static inline int bmvr_x86_prologue_len(const uint8_t *code, size_t avail)
{
    int total = 0;
    while (total < 5) {
        const int l = bmvr_x86_insn_len(code + total, avail - (size_t)total);
        if (!l || total + l > 16) return 0;
        total += l;
    }
    return total;
}

#if defined(__i386__)
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
/* Detour `target` to `detour`; *tramp receives a callable copy of the original. 1 = done, 0 = refused (nothing changed). perms = the
 * mapping's protection as the kernel reported it ("r-xp"), restored after the write. */
static inline int bmvr_inline_hook(uintptr_t target, uintptr_t detour, const char *perms, uintptr_t *tramp)
{
    const int n = bmvr_x86_prologue_len((const uint8_t *)target, 32);
    if (!n) return 0;
    uint8_t *t = (uint8_t *)mmap(NULL, 64, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t == (uint8_t *)MAP_FAILED) return 0;
    memcpy(t, (const void *)target, (size_t)n);
    t[n] = 0xE9;
    const int32_t back = (int32_t)((uintptr_t)(target + (uintptr_t)n) - ((uintptr_t)t + (uintptr_t)n + 5));
    memcpy(t + n + 1, &back, 4);
    const long ps = sysconf(_SC_PAGESIZE);
    const uintptr_t lo = target & ~(uintptr_t)(ps - 1), hi = (target + 8 + (uintptr_t)ps - 1) & ~(uintptr_t)(ps - 1);
    const int restore = PROT_READ | (perms && perms[2] == 'x' ? PROT_EXEC : 0) | (perms && perms[1] == 'w' ? PROT_WRITE : 0);
    if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) { munmap(t, 64); return 0; }
    uint8_t patch[16];
    memset(patch, 0x90, sizeof patch);
    patch[0] = 0xE9;
    const int32_t rel = (int32_t)(detour - (target + 5));
    memcpy(patch + 1, &rel, 4);
    memcpy((void *)target, patch, (size_t)n);
    mprotect((void *)lo, hi - lo, restore);
    __builtin___clear_cache((char *)target, (char *)target + n);
    *tramp = (uintptr_t)t;
    return 1;
}
#endif
#endif
