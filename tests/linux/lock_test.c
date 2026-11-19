/* Exercises bmvr_acquire_helper_lock() (the real header the helper uses): second holder refused with the first's pid,
 * lock freed by kill -9, selftest-style no-lock path untouched. No Vulkan needed. usage: lock_test <dir> */
#include "bmvr_single_instance.h"
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
static int fails;
static void check(const char *l, int ok) { printf("  %s  %s\n", ok ? "PASS" : "FAIL", l); if (!ok) fails++; }
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    char err[1400]; long hp;
    pid_t child = fork();
    if (child == 0) { /* first helper */
        int fd = bmvr_acquire_helper_lock(argv[1], err, sizeof err, &hp);
        if (fd < 0) _exit(3);
        pause(); _exit(0);
    }
    usleep(300000);
    int fd = bmvr_acquire_helper_lock(argv[1], err, sizeof err, &hp);
    check("second helper is refused while the first lives", fd < 0);
    check("message names the lock file", strstr(err, "already serves this game folder") != NULL);
    check("holder pid reported", hp == (long)child);
    kill(child, SIGKILL); waitpid(child, NULL, 0);
    fd = bmvr_acquire_helper_lock(argv[1], err, sizeof err, &hp);
    check("lock is free again right after kill -9 (no stale lock)", fd >= 0);
    int fd2 = bmvr_acquire_helper_lock(argv[1], err, sizeof err, &hp);
    check("same process opening twice is refused too (separate open file description)", fd2 < 0 && hp == (long)getpid());
    close(fd);
    fd = bmvr_acquire_helper_lock("/nonexistent-dir", err, sizeof err, &hp);
    check("unwritable folder gives a clear error", fd < 0 && strstr(err, "cannot open") != NULL);
    return fails ? 1 : 0;
}
