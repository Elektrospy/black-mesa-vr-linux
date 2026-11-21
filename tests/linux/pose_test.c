/* Stage 4a X4: stress test of linux/bmvr_pose_store.h (the pose table) with a writer and a reader thread. No GPU.
 *   cc -O2 -pthread -I linux -o pose_test tests/linux/pose_test.c && ./pose_test      (also with -fsanitize=thread)
 * The writer puts poses whose fields all encode the same counter; the reader looks ids up and checks that a pose is either exactly the
 * one stored under that id (all fields agree with the id) or reported stale, never a torn or wrong one. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include "bmvr_pose_store.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static struct bmvr_pose_store st;
static atomic_int stop;
static atomic_uint issued;

static void fill(struct bmvr_xfer_pose *p, uint32_t k)
{
    memset(p, 0, sizeof *p);
    p->flags = BMVR_XFER_POSE_LOCAL;
    p->display_time = (int64_t)k * 7;
    for (int e = 0; e < 2; e++) { p->eye[e].pos[0] = (float)k; p->eye[e].pos[1] = (float)(k + 1); p->eye[e].fov[0] = (float)k; p->eye[e].rot[3] = (float)e; }
}

static void *writer(void *a)
{
    (void)a;
    for (uint32_t k = 1; k <= 200000; k++) {
        struct bmvr_xfer_pose p; fill(&p, k);
        uint32_t id = bmvr_pose_store_put(&st, &p);
        if (id != k) { printf("FAIL: id %u for put %u\n", id, k); fails++; break; }
        atomic_store(&issued, id);
    }
    atomic_store(&stop, 1);
    return NULL;
}

static void *reader(void *a)
{
    (void)a;
    uint64_t ok = 0, stale = 0, none = 0;
    uint32_t lag = 0;
    while (!atomic_load(&stop)) {
        uint32_t top = atomic_load(&issued);
        if (!top) continue;
        uint32_t id = top > lag ? top - lag : 1;   /* lag 0..63: recent ids hit, old ones are stale */
        lag = (lag + 5) % 64;
        struct bmvr_xfer_pose p;
        int r = bmvr_pose_store_lookup(&st, id, &p);
        if (r == 1) {
            ok++;
            if (p.pose_id != id || p.eye[0].pos[0] != (float)id || p.eye[1].pos[1] != (float)(id + 1) || p.eye[0].fov[0] != (float)id || p.display_time != (int64_t)id * 7) {
                printf("FAIL: torn/wrong pose for id %u (stored id %u pos %f)\n", id, p.pose_id, (double)p.eye[0].pos[0]); fails++; break;
            }
        } else if (r == -1) {
            stale++;
        } else none++;
        struct bmvr_xfer_pose l;
        if (bmvr_pose_store_latest(&st, &l) && (l.eye[0].pos[0] != (float)l.pose_id)) { printf("FAIL: torn latest\n"); fails++; break; }
    }
    printf("reader: %llu hits, %llu stale, %llu none\n", (unsigned long long)ok, (unsigned long long)stale, (unsigned long long)none);
    return NULL;
}

int main(void)
{
    bmvr_pose_store_init(&st);
    struct bmvr_xfer_pose p, q; fill(&p, 1);

    CHECK(!bmvr_pose_store_latest(&st, &q), "latest on an empty store");
    CHECK(bmvr_pose_store_lookup(&st, 0, &q) == 0, "id 0 means no pose, not stale");
    CHECK(bmvr_pose_store_lookup(&st, 5, &q) == -1, "an id that was never issued is stale");
    uint32_t a = bmvr_pose_store_put(&st, &p), b = bmvr_pose_store_put(&st, &p);
    CHECK(a == 1 && b == 2, "ids start at 1 and increase (%u %u)", a, b);
    CHECK(bmvr_pose_store_lookup(&st, 1, &q) == 1 && q.pose_id == 1, "lookup of a stored id");
    for (int i = 0; i < BMVR_POSE_STORE_N; i++) bmvr_pose_store_put(&st, &p);
    CHECK(bmvr_pose_store_lookup(&st, 1, &q) == -1, "an id older than the table is stale");
    CHECK(bmvr_pose_store_lookup(&st, 2 + BMVR_POSE_STORE_N, &q) == 1, "the newest id is found");
    uint64_t put, stale; bmvr_pose_store_take_counters(&st, &put, &stale);
    CHECK(put == (uint64_t)BMVR_POSE_STORE_N + 2 && stale == 2, "counters (put %llu, stale %llu)", (unsigned long long)put, (unsigned long long)stale);
    bmvr_pose_store_take_counters(&st, &put, &stale);
    CHECK(put == 0 && stale == 0, "counters reset after take");

    /* id wrap skips 0 */
    st.next_id = 0xfffffffeu;
    uint32_t w1 = bmvr_pose_store_put(&st, &p), w2 = bmvr_pose_store_put(&st, &p), w3 = bmvr_pose_store_put(&st, &p), w4 = bmvr_pose_store_put(&st, &p);
    CHECK(w1 == 0xfffffffeu && w2 == 0xffffffffu && w3 == 1 && w4 == 2, "id wrap skips 0 (%u %u %u %u)", w1, w2, w3, w4);

    bmvr_pose_store_destroy(&st);
    bmvr_pose_store_init(&st);
    pthread_t tw, tr;
    pthread_create(&tw, NULL, writer, NULL);
    pthread_create(&tr, NULL, reader, NULL);
    pthread_join(tw, NULL);
    pthread_join(tr, NULL);
    bmvr_pose_store_destroy(&st);
    printf(fails ? "pose_test: %d FAILED\n" : "pose_test: all checks passed\n", fails);
    return fails ? 1 : 0;
}
