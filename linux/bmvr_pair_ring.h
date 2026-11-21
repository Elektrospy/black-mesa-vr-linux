/*
 * Black Mesa VR - native Linux port, stage 4a, milestone X2: the pair ring.
 * Design: docs/LINUX_OPENXR_CONSUMER.md, "Decision 2".
 *
 * Pure bookkeeping, no Vulkan: it only decides WHICH slot may be written, which slot is the newest complete pair and
 * what the counters say. The GPU copies and the fences live in bmvr_xr_sink.c; this file is separate so the state machine
 * can be stress-tested with a fake driver (tests/linux/ring_test.c) without a GPU.
 *
 * Two threads use it, exactly one each:
 *   WRITER (the socket thread): ring_begin_write -> [submit the GPU copy pair -> slot] -> ring_end_write
 *   READER (the XR thread):     ring_pick, once per displayed frame
 *
 * Slot states (all changes under one mutex):
 *   FREE       nobody uses it
 *   WRITING    the writer records/submits the copy into it (the reader ignores it)
 *   SUBMITTED  the copy is submitted, the GPU may still be working on it (the reader polls `done`)
 *   READY      the copy is complete; the newest READY slot is the one to display, older READY slots are recycled
 *
 * Rules that make a pair indivisible: a slot holds BOTH eyes of one pair and is published as a whole by ring_end_write;
 * the reader only ever sees a slot after `done()` said the copy of the whole slot finished.
 *
 * Contract with the reader: ring_pick means "the GPU is done with the slot the PREVIOUS ring_pick returned" (the sink waits
 * for that frame's fence first), and marks the new slot as `reading`. A slot being read is never recycled, by either thread.
 * The reader's slot also stays available for re-display when no newer pair arrives (it stays the newest until superseded).
 * The writer recycles finished, superseded slots itself when it finds no free one (so a game that outpaces the display does
 * not lose pairs merely because the reader has not looked yet): that is why begin_write gets the same `done` callback.
 */
#ifndef BMVR_PAIR_RING_H
#define BMVR_PAIR_RING_H

#include <pthread.h>
#include <stdint.h>

#define BMVR_RING_SLOTS 3

enum { BMVR_RING_FREE = 0, BMVR_RING_WRITING, BMVR_RING_SUBMITTED, BMVR_RING_READY };

struct bmvr_ring {
    pthread_mutex_t mu;
    int state[BMVR_RING_SLOTS];
    uint64_t seq[BMVR_RING_SLOTS];  /* arrival order of the pair in the slot (1, 2, ...) */
    uint32_t tick[BMVR_RING_SLOTS]; /* the game's tick for that pair */
    int shown[BMVR_RING_SLOTS];     /* displayed at least once */
    int newest;                     /* slot of the newest READY pair, -1 = none yet */
    int reading;                    /* slot the reader's GPU work may be reading, -1 = none: never recycled */
    uint64_t next_seq;
    /* counters since the last bmvr_ring_take_counters() */
    uint64_t c_pairs;     /* pairs published by the writer */
    uint64_t c_displayed; /* frames the reader showed a pair in (fresh + reused) */
    uint64_t c_reused;    /* ... of which the pair had already been shown (the game was slower than the display) */
    uint64_t c_dropped;   /* pairs that were never shown (superseded by a newer pair, or no free slot when they arrived) */
};

static inline void bmvr_ring_init(struct bmvr_ring *r)
{
    pthread_mutex_init(&r->mu, NULL);
    for (int i = 0; i < BMVR_RING_SLOTS; i++) { r->state[i] = BMVR_RING_FREE; r->seq[i] = 0; r->tick[i] = 0; r->shown[i] = 0; }
    r->newest = -1;
    r->reading = -1;
    r->next_seq = 1;
    r->c_pairs = r->c_displayed = r->c_reused = r->c_dropped = 0;
}

static inline void bmvr_ring_destroy(struct bmvr_ring *r) { pthread_mutex_destroy(&r->mu); }

typedef int (*bmvr_ring_done_fn)(void *ctx, int slot); /* 1 = the GPU finished the copy into `slot` */

/* (mutex held) Promote finished copies to READY, make the newest READY pair `newest`, recycle every other READY slot that is
 * not being read. A pair recycled before it was ever shown counts as dropped. */
static inline void bmvr_ring_promote_locked(struct bmvr_ring *r, bmvr_ring_done_fn done, void *ctx)
{
    for (int i = 0; i < BMVR_RING_SLOTS; i++)
        if (r->state[i] == BMVR_RING_SUBMITTED && done(ctx, i)) r->state[i] = BMVR_RING_READY;
    int best = r->newest;
    for (int i = 0; i < BMVR_RING_SLOTS; i++)
        if (r->state[i] == BMVR_RING_READY && (best < 0 || r->seq[i] > r->seq[best])) best = i;
    r->newest = best;
    for (int i = 0; i < BMVR_RING_SLOTS; i++) {
        if (i != r->newest && i != r->reading && r->state[i] == BMVR_RING_READY) { /* superseded and not in use */
            if (!r->shown[i]) r->c_dropped++;
            r->state[i] = BMVR_RING_FREE;
        }
    }
}

/* WRITER: a slot to record the next pair into, or -1 when none can be had (the pair is then counted as dropped; the caller
 * must still keep the transport moving, e.g. signal vk_done without copying). */
static inline int bmvr_ring_begin_write(struct bmvr_ring *r, bmvr_ring_done_fn done, void *ctx)
{
    int got = -1;
    pthread_mutex_lock(&r->mu);
    for (int pass = 0; pass < 2 && got < 0; pass++) {
        for (int i = 0; i < BMVR_RING_SLOTS; i++) {
            if (r->state[i] == BMVR_RING_FREE) { r->state[i] = BMVR_RING_WRITING; got = i; break; }
        }
        if (got < 0 && pass == 0) bmvr_ring_promote_locked(r, done, ctx); /* recycle finished, superseded slots, then look again */
    }
    if (got < 0) r->c_dropped++;
    pthread_mutex_unlock(&r->mu);
    return got;
}

/* WRITER: the copy into `slot` is submitted (complete or in flight). Publishes the pair. */
static inline void bmvr_ring_end_write(struct bmvr_ring *r, int slot, uint32_t tick)
{
    pthread_mutex_lock(&r->mu);
    r->seq[slot] = r->next_seq++;
    r->tick[slot] = tick;
    r->shown[slot] = 0;
    r->state[slot] = BMVR_RING_SUBMITTED;
    r->c_pairs++;
    pthread_mutex_unlock(&r->mu);
}

/* WRITER: the submission failed; give the slot back (the pair is lost and counted as dropped). */
static inline void bmvr_ring_abort_write(struct bmvr_ring *r, int slot)
{
    pthread_mutex_lock(&r->mu);
    r->state[slot] = BMVR_RING_FREE;
    r->c_dropped++;
    pthread_mutex_unlock(&r->mu);
}

/* READER: the slot to display (the newest complete pair; returned again, with *fresh = 0, when nothing newer arrived), or -1
 * when no pair has ever completed. Calling it declares the GPU done with the previously returned slot.
 * *tick gets the pair's tick; *seq its arrival number. */
static inline int bmvr_ring_pick(struct bmvr_ring *r, bmvr_ring_done_fn done, void *ctx, uint32_t *tick, uint64_t *seq, int *fresh)
{
    int pick;
    pthread_mutex_lock(&r->mu);
    r->reading = -1;
    bmvr_ring_promote_locked(r, done, ctx);
    pick = r->newest;
    r->reading = pick;
    if (pick >= 0) {
        *fresh = !r->shown[pick];
        r->shown[pick] = 1;
        r->c_displayed++;
        if (!*fresh) r->c_reused++;
        if (tick) *tick = r->tick[pick];
        if (seq) *seq = r->seq[pick];
    }
    pthread_mutex_unlock(&r->mu);
    return pick;
}

static inline void bmvr_ring_take_counters(struct bmvr_ring *r, uint64_t *pairs, uint64_t *displayed, uint64_t *reused, uint64_t *dropped)
{
    pthread_mutex_lock(&r->mu);
    *pairs = r->c_pairs; *displayed = r->c_displayed; *reused = r->c_reused; *dropped = r->c_dropped;
    r->c_pairs = r->c_displayed = r->c_reused = r->c_dropped = 0;
    pthread_mutex_unlock(&r->mu);
}

#endif
