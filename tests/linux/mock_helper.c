/* Mock of bmvr_xfer_helper: real unix socket + SCM_RIGHTS, protocol v3, no Vulkan. Writes to $MOCK_TRACE. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include "bmvr_xfer.h"
static int tfd = -2;
static void T(const char *fmt, ...) {
  if (tfd == -2) { const char *p = getenv("MOCK_TRACE"); tfd = open(p ? p : "mock_trace.txt", O_WRONLY | O_CREAT | O_APPEND, 0644); }
  char b[512]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b - 1, fmt, ap); va_end(ap); b[n++] = '\n'; if (write(tfd, b, (size_t)n) < 0) {} }
int main(int argc, char **argv) {
  const char *dir = argv[1]; int sem = 0, ver = BMVR_XFER_VERSION, die_after = -1;
  for (int i = 2; i < argc; i++) { if (!strcmp(argv[i], "--sem")) sem = 1; else if (!strcmp(argv[i], "--version")) ver = atoi(argv[++i]); else if (!strcmp(argv[i], "--die-after")) die_after = atoi(argv[++i]); }
  signal(SIGPIPE, SIG_IGN);
  char path[512]; snprintf(path, sizeof path, "%s/%s", dir, BMVR_XFER_SOCKET_NAME);
  struct sockaddr_un sa = { .sun_family = AF_UNIX }; strcpy(sa.sun_path, path); unlink(path);
  int srv = socket(AF_UNIX, SOCK_STREAM, 0);
  if (bind(srv, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(srv, 1) < 0) { perror("bind"); return 1; }
  printf("mock helper listening\n"); fflush(stdout);
  int cl = accept(srv, NULL, NULL); if (cl < 0) return 1;
  int fds[3], nfds = sem ? 3 : 1;
  for (int i = 0; i < nfds; i++) fds[i] = open("/dev/null", O_RDWR);
  struct bmvr_xfer_hello h = { .magic = BMVR_XFER_MAGIC_HELLO, .version = (uint32_t)ver, .width = 1920, .height = 1080, .gl_internal_format = 0x8058,
                               .dedicated = 1, .optimal_tiling = 1, .caps = sem ? BMVR_XFER_CAP_SEMAPHORES : 0, .memory_size = 8294400 };
  struct iovec iov = { &h, sizeof h }; char ctl[CMSG_SPACE(3 * sizeof(int))]; memset(ctl, 0, sizeof ctl);
  struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl, .msg_controllen = CMSG_SPACE((size_t)nfds * sizeof(int)) };
  struct cmsghdr *c = CMSG_FIRSTHDR(&m); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN((size_t)nfds * sizeof(int));
  memcpy(CMSG_DATA(c), fds, (size_t)nfds * sizeof(int));
  sendmsg(cl, &m, MSG_NOSIGNAL); for (int i = 0; i < nfds; i++) close(fds[i]);
  T("HELPER hello version=%d caps=%u fds=%d", ver, h.caps, nfds);
  int n = 0;
  for (;;) {
    struct bmvr_xfer_frame fr; ssize_t g = recv(cl, &fr, sizeof fr, MSG_WAITALL);
    if (g != (ssize_t)sizeof fr) break;
    T("HELPER frame tick=%u flags=0x%x name=%s src=%ux%u", fr.tick, fr.flags, fr.name, fr.src_w, fr.src_h);
    if (die_after >= 0 && n >= die_after) { T("HELPER dying"); _exit(0); }
    struct bmvr_xfer_ack a = { BMVR_XFER_MAGIC_ACK, 0 };
    T("HELPER ack");
    if (send(cl, &a, sizeof a, MSG_NOSIGNAL) != (ssize_t)sizeof a) break;
    n++;
  }
  T("HELPER end"); return 0; }
