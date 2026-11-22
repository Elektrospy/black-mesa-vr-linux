/* Decoder checks for linux/bmvr_inline.h (pure, runs on any host): typical GCC i386 function starts. */
#include <stdio.h>
#include <string.h>
#include "bmvr_inline.h"
static int fails;
static void chk(const char *name, const uint8_t *b, size_t n, int want)
{
    uint8_t buf[32] = {0}; memcpy(buf, b, n);
    int got = bmvr_x86_prologue_len(buf, sizeof buf);
    printf("  %s  %s: %d (want %d)\n", got == want ? "PASS" : "FAIL", name, got, want);
    if (got != want) fails++;
}
#define C(name, want, ...) do { const uint8_t b[] = { __VA_ARGS__ }; chk(name, b, sizeof b, want); } while (0)
int main(void)
{
    C("push ebp; mov ebp,esp; push edi; push esi", 5, 0x55,0x89,0xe5,0x57,0x56);
    C("push ebp; mov ebp,esp; push ebx; sub esp,0x2c", 7, 0x55,0x89,0xe5,0x53,0x83,0xec,0x2c);
    C("push edi; push esi; push ebx; sub esp,0x1c (omit fp)", 6, 0x57,0x56,0x53,0x83,0xec,0x1c);
    C("push ebp; mov ebp,esp; sub esp,0x118 (imm32)", 9, 0x55,0x89,0xe5,0x81,0xec,0x18,0x01,0x00,0x00);
    C("sub esp,0xc; mov eax,[esp+0x10]", 7, 0x83,0xec,0x0c,0x8b,0x44,0x24,0x10);
    C("endbr32; push ebp; mov ebp,esp", 5, 0xf3,0x0f,0x1e,0xfb,0x55,0x89,0xe5);
    C("push ebp; mov ebp,esp; mov eax,[ebp+8]", 6, 0x55,0x89,0xe5,0x8b,0x45,0x08);
    C("mov ecx,[ebp+0x12345678] (disp32)", 9, 0x55,0x89,0xe5,0x8b,0x8d,0x78,0x56,0x34,0x12);
    C("call rel32 first -> refused", 0, 0xe8,0x00,0x00,0x00,0x00);
    C("jmp rel32 (thunk) -> refused", 0, 0xe9,0x10,0x00,0x00,0x00);
    C("push ebp; call -> refused", 0, 0x55,0xe8,0x00,0x00,0x00,0x00);
    C("unknown opcode -> refused", 0, 0xcc,0xcc,0xcc,0xcc,0xcc);
    printf(fails ? "INLINE TEST FAILED\n" : "inline decoder: all checks passed\n");
    return fails != 0;
}
