/*
 * Black Mesa VR - native Linux port, stage 3c: one helper per game folder.
 *
 * The helper used to unlink() "<game folder>/bmvr_xfer.sock" and bind its own. A second helper therefore
 * silently took the path over while the game stayed connected to the first one: the new helper listened for
 * ever, the shim never saw it (found by the kill-loop test, which then "never connected").
 *
 * Fix: an exclusive, non-blocking flock() on "<game folder>/bmvr_xfer.lock", held for the life of the process.
 * The kernel drops it when the process exits, however it exits (kill -9 included), so there is no stale-lock
 * problem and nothing to clean up. The holder's pid is written into the file for the error message only.
 * Header-only so tests/linux can exercise exactly this code without Vulkan.
 */
#ifndef BMVR_SINGLE_INSTANCE_H
#define BMVR_SINGLE_INSTANCE_H

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#define BMVR_XFER_LOCK_NAME "bmvr_xfer.lock"

/* Returns the lock fd (keep it open; never close it) or -1 with a message in err. If another process holds the lock,
 * *holder_pid is its pid when readable, else 0. */
static inline int bmvr_acquire_helper_lock(const char *gamedir, char *err, size_t errsz, long *holder_pid)
{
    char path[1100];
    if (holder_pid) *holder_pid = 0;
    snprintf(path, sizeof path, "%s/%s", gamedir, BMVR_XFER_LOCK_NAME);
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) { snprintf(err, errsz, "cannot open %s: %s", path, strerror(errno)); return -1; }
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        int e = errno;
        char buf[32] = "";
        ssize_t n = pread(fd, buf, sizeof buf - 1, 0);
        if (n > 0) { buf[n] = 0; if (holder_pid) *holder_pid = strtol(buf, NULL, 10); }
        if (e == EWOULDBLOCK) snprintf(err, errsz, "another bmvr_xfer_helper already serves this game folder (%s is locked)", path);
        else snprintf(err, errsz, "cannot lock %s: %s", path, strerror(e));
        close(fd);
        return -1;
    }
    char me[32];
    int len = snprintf(me, sizeof me, "%ld\n", (long)getpid());
    if (ftruncate(fd, 0) == 0) (void)!pwrite(fd, me, (size_t)len, 0);
    return fd;
}

#endif
