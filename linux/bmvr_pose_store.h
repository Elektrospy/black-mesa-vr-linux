/*
 * Black Mesa VR - native Linux port, stage 4a, milestone X4: the pose table.
 * Design: docs/LINUX_OPENXR_CONSUMER.md, "Decision 5" and "X4".
 *
 * Pure bookkeeping, no Vulkan/OpenXR (same style as bmvr_pair_ring.h, so tests/linux/pose_test.c can stress it without a GPU).
 * The XR thread puts a pose per frame (the table assigns a fresh, non-zero id); the transport thread reads the latest one for the
 * ack; the XR thread looks a pose up by the id a pair was rendered with. Poses are kept by id for the last BMVR_POSE_STORE_N puts:
 * a lookup of an older id is "stale" and counted. Id 0 means "no pose" (the game had none): never stored, never stale.
 */
#ifndef BMVR_POSE_STORE_H
#define BMVR_POSE_STORE_H

#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include "bmvr_xfer.h"

#define BMVR_POSE_STORE_N 32

struct bmvr_pose_store {
    pthread_mutex_t mu;
    struct bmvr_xfer_pose slot[BMVR_POSE_STORE_N];
    uint32_t next_id;     /* the id the next put gets (starts at 1, skips 0 on wrap) */
    uint32_t latest_id;   /* 0 = nothing stored yet */
    uint64_t c_put, c_stale;   /* since the last take_counters */
};

static inline void bmvr_pose_store_init(struct bmvr_pose_store *p)
{
    memset(p, 0, sizeof *p);
    pthread_mutex_init(&p->mu, NULL);
    p->next_id = 1;
}

static inline void bmvr_pose_store_destroy(struct bmvr_pose_store *p) { pthread_mutex_destroy(&p->mu); }

/* Store a copy of *in (its pose_id is ignored) under a fresh id. Returns that id. */
static inline uint32_t bmvr_pose_store_put(struct bmvr_pose_store *p, const struct bmvr_xfer_pose *in)
{
    pthread_mutex_lock(&p->mu);
    uint32_t id = p->next_id++;
    if (p->next_id == 0) p->next_id = 1;
    struct bmvr_xfer_pose *s = &p->slot[id % BMVR_POSE_STORE_N];
    *s = *in;
    s->pose_id = id;
    p->latest_id = id;
    p->c_put++;
    pthread_mutex_unlock(&p->mu);
    return id;
}

/* The newest pose. 1 and *out filled, or 0 when nothing was stored yet. */
static inline int bmvr_pose_store_latest(struct bmvr_pose_store *p, struct bmvr_xfer_pose *out)
{
    int ok = 0;
    pthread_mutex_lock(&p->mu);
    if (p->latest_id) { *out = p->slot[p->latest_id % BMVR_POSE_STORE_N]; ok = 1; }
    pthread_mutex_unlock(&p->mu);
    return ok;
}

/* The pose stored under `id`. 1 = found (*out filled); 0 = id is 0 ("no pose", not an error); -1 = stale or never issued (counted). */
static inline int bmvr_pose_store_lookup(struct bmvr_pose_store *p, uint32_t id, struct bmvr_xfer_pose *out)
{
    if (id == 0) return 0;
    int r = -1;
    pthread_mutex_lock(&p->mu);
    const struct bmvr_xfer_pose *s = &p->slot[id % BMVR_POSE_STORE_N];
    if (s->pose_id == id) { *out = *s; r = 1; } else p->c_stale++;
    pthread_mutex_unlock(&p->mu);
    return r;
}

static inline void bmvr_pose_store_take_counters(struct bmvr_pose_store *p, uint64_t *put, uint64_t *stale)
{
    pthread_mutex_lock(&p->mu);
    *put = p->c_put; *stale = p->c_stale;
    p->c_put = p->c_stale = 0;
    pthread_mutex_unlock(&p->mu);
}

#endif
