/*
 * Black Mesa VR - native Linux port: a transparent call probe for one function.
 *
 * Used to FIND a virtual function whose vtable slot is not known (CalcViewModelView on the Linux build). A normal C detour is not safe for
 * a function of unknown shape: a callee that returns a struct by value pops a hidden pointer (`ret 4`), one that returns a float leaves it
 * in st(0), one with more arguments than the detour forwards loses them. A thunk that never touches the stack and ends in a plain `jmp` to the original is
 * indifferent to all of that:
 *
 *     lea  eax, [esp+4]        ; eax -> the first argument (`this`); [esp] is the return address, untouched
 *     push eax
 *     push k
 *     mov  eax, probe          ; probe(int k, const uint32_t *args) is an ordinary cdecl C function
 *     call eax
 *     add  esp, 8
 *     mov  eax, orig
 *     jmp  eax                 ; the original runs with exactly the stack and registers it would have had (eax / ecx / edx are dead at a
 *                              ; cdecl function's entry; this shim does not run on regparm / fastcall code)
 *
 * 32-bit x86 only (the native Black Mesa client is 32-bit). tests/linux/probe_test.c runs it for real as a -m32 freestanding program.
 */
#ifndef BMVR_PROBE_H
#define BMVR_PROBE_H

#include <stddef.h>
#include <stdint.h>

#define BMVR_PROBE_THUNK_SIZE 32

typedef void (*bmvr_probe_fn)(int k, const uint32_t *args);

/* Writes the thunk into buf (BMVR_PROBE_THUNK_SIZE bytes, executable). Returns the number of bytes used, or 0 on a non-32-bit build. */
static inline void bmvr_probe_put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static inline size_t bmvr_probe_thunk(uint8_t *buf, uint32_t k, bmvr_probe_fn probe, uintptr_t orig)
{
#if defined(__i386__)
    uint8_t *p = buf;
    const uint32_t pr = (uint32_t)(uintptr_t)probe, og = (uint32_t)orig;
    *p++ = 0x8D; *p++ = 0x44; *p++ = 0x24; *p++ = 0x04;          /* lea eax,[esp+4] */
    *p++ = 0x50;                                                  /* push eax */
    *p++ = 0x68; bmvr_probe_put32(p, k); p += 4;                        /* push imm32 */
    *p++ = 0xB8; bmvr_probe_put32(p, pr); p += 4;                       /* mov eax, probe */
    *p++ = 0xFF; *p++ = 0xD0;                                     /* call eax */
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;                        /* add esp,8 */
    *p++ = 0xB8; bmvr_probe_put32(p, og); p += 4;                       /* mov eax, orig */
    *p++ = 0xFF; *p++ = 0xE0;                                     /* jmp eax */
    while ((size_t)(p - buf) < BMVR_PROBE_THUNK_SIZE) *p++ = 0xCC;
    return (size_t)(p - buf);
#else
    (void)buf; (void)k; (void)probe; (void)orig;
    return 0;
#endif
}

#endif
