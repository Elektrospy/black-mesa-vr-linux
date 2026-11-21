/*
 * Stage 4a, X2: stress test of the pair ring (linux/bmvr_pair_ring.h) with a fake GPU. No Vulkan, no GPU.
 *
 *   cc -O2 -g -pthread -I linux -o ring_test tests/linux/ring_test.c && ./ring_test
 *   (also run with -fsanitize=thread: the ring and the "GPU" hand-over must be race free)
 *
 * Roles: a WRITER thread publishes pairs (counter n in both "eyes"), a GPU thread performs the "copy" late and at random
 * (eye 0 first, a pause, then eye 1, then sets done), a READER thread plays the XR thread: pick, then "display" the slot for
 * a while by reading both eyes repeatedly. Checked, for several speed ratios:
 *   - a displayed pair never has eye 0 != eye 1 (no half pair), at any point of the display period
 *   - the contents of a displayed slot never change while it is displayed (no slot is rewritten while read)
 *   - the arrival sequence never goes backwards (an older pair is never shown after a newer one)
 *   - a pair is "fresh" at most once; everything is accounted for: published = shown fresh + dropped + still in the ring
 *   - the writer is never blocked: begin_write returns a slot or -1 at once
 */
#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bmvr_pair_ring.h"

struct fake_slot {
    _Atomic uint64_t eye[2];
    atomic_int done;
};

static struct bmvr_ring ring;
static struct fake_slot fs[BMVR_RING_SLOTS];
static atomic_int stop_all;

struct gpu_job { int slot; uint64_t value; int valid; };
static struct gpu_job job_q[16];
static pthread_mutex_t job_mu = PTHREAD_MUTEX_INITIALIZER;

static int g_write_gap_us, g_gpu_delay_us, g_display_us, g_seconds;
static uint64_t g_published, g_noslot, g_fresh, g_reused_frames, g_errors;

static void sleep_us(int us) { if (us > 0) { struct timespec t = { us / 1000000, (us % 1000000) * 1000L }; nanosleep(&t, NULL); } }

static int done_fn(void *ctx, int slot) { (void)ctx; return atomic_load_explicit(&fs[slot].done, memory_order_acquire); }

static void *gpu_thread(void *arg)
{
    (void)arg;
    unsigned seed = 7;
    for (;;) {
        struct gpu_job j = { 0, 0, 0 };
        pthread_mutex_lock(&job_mu);
        for (int i = 0; i < 16; i++) if (job_q[i].valid) { j = job_q[i]; job_q[i].valid = 0; break; }
        pthread_mutex_unlock(&job_mu);
        if (!j.valid) {
            if (atomic_load(&stop_all)) return NULL;
            sleep_us(20);
            continue;
        }
        sleep_us(g_gpu_delay_us ? (int)(rand_r(&seed) % (unsigned)(g_gpu_delay_us * 2)) : 0);
        atomic_store_explicit(&fs[j.slot].eye[0], j.value, memory_order_relaxed);
        sleep_us(g_gpu_delay_us ? (int)(rand_r(&seed) % (unsigned)(g_gpu_delay_us + 1)) : 0); /* a half-copied pair exists for a while */
        atomic_store_explicit(&fs[j.slot].eye[1], j.value, memory_order_relaxed);
        atomic_store_explicit(&fs[j.slot].done, 1, memory_order_release); /* the fence signals */
    }
}

static void *writer_thread(void *arg)
{
    (void)arg;
    uint64_t n = 0;
    while (!atomic_load(&stop_all)) {
        int s = bmvr_ring_begin_write(&ring, done_fn, NULL);
        if (s < 0) { g_noslot++; sleep_us(g_write_gap_us); continue; }
        n++;
        atomic_store(&fs[s].done, 0);
        atomic_store_explicit(&fs[s].eye[0], 0, memory_order_relaxed); /* garbage until the GPU copy lands */
        atomic_store_explicit(&fs[s].eye[1], 0, memory_order_relaxed);
        pthread_mutex_lock(&job_mu);
        int queued = 0;
        for (int i = 0; i < 16 && !queued; i++) if (!job_q[i].valid) { job_q[i] = (struct gpu_job){ s, n, 1 }; queued = 1; }
        pthread_mutex_unlock(&job_mu);
        if (!queued) { fprintf(stderr, "ring_test: job queue overflow (test bug)\n"); exit(2); }
        bmvr_ring_end_write(&ring, s, (uint32_t)n); /* published while the GPU copy is still pending */
        g_published++;
        sleep_us(g_write_gap_us);
    }
    return NULL;
}

static void *reader_thread(void *arg)
{
    (void)arg;
    uint64_t last_seq = 0;
    uint64_t frame_dt_us = 11111;
    while (!atomic_load(&stop_all)) {
        uint32_t tick = 0; uint64_t seq = 0; int fresh = 0;
        int s = bmvr_ring_pick(&ring, done_fn, NULL, &tick, &seq, &fresh);
        if (s >= 0) {
            if (seq < last_seq) { g_errors++; fprintf(stderr, "ring_test: ERROR sequence went backwards (%llu after %llu)\n", (unsigned long long)seq, (unsigned long long)last_seq); }
            if (fresh && seq <= last_seq) { g_errors++; fprintf(stderr, "ring_test: ERROR a pair was fresh twice (seq %llu)\n", (unsigned long long)seq); }
            last_seq = seq;
            if (fresh) g_fresh++; else g_reused_frames++;
            /* "display": the GPU reads the slot for g_display_us; both eyes must stay equal to the tick the whole time */
            for (int k = 0; k < 6; k++) {
                uint64_t a = atomic_load_explicit(&fs[s].eye[0], memory_order_relaxed);
                uint64_t b = atomic_load_explicit(&fs[s].eye[1], memory_order_relaxed);
                if (a != b || a != tick) {
                    g_errors++;
                    fprintf(stderr, "ring_test: ERROR displayed slot %d tick %u has eye0=%llu eye1=%llu (half pair, or the slot was rewritten while displayed)\n",
                            s, tick, (unsigned long long)a, (unsigned long long)b);
                    break;
                }
                sleep_us(g_display_us / 6);
            }
        }
        sleep_us((int)frame_dt_us > g_display_us ? (int)frame_dt_us - g_display_us : 0);
    }
    return NULL;
}

static int run_case(const char *name, int write_gap_us, int gpu_delay_us, int display_us)
{
    bmvr_ring_init(&ring);
    memset((void *)fs, 0, sizeof fs);
    memset(job_q, 0, sizeof job_q);
    atomic_store(&stop_all, 0);
    g_write_gap_us = write_gap_us; g_gpu_delay_us = gpu_delay_us; g_display_us = display_us;
    g_published = g_noslot = g_fresh = g_reused_frames = g_errors = 0;
    pthread_t tw, tr, tg;
    pthread_create(&tg, NULL, gpu_thread, NULL);
    pthread_create(&tw, NULL, writer_thread, NULL);
    pthread_create(&tr, NULL, reader_thread, NULL);
    sleep_us(g_seconds * 1000000);
    atomic_store(&stop_all, 1);
    pthread_join(tw, NULL); pthread_join(tr, NULL); pthread_join(tg, NULL);

    uint64_t pairs, disp, reused, dropped;
    bmvr_ring_take_counters(&ring, &pairs, &disp, &reused, &dropped);
    uint64_t dropped_pairs = dropped - g_noslot; /* the part of `dropped` that is a published pair never shown */
    uint64_t in_ring = pairs - g_fresh - dropped_pairs; /* published but never shown and not dropped: still in the ring */
    int ok = g_errors == 0;
    if (pairs != g_published) { ok = 0; printf("  counter mismatch: ring says %llu pairs, writer published %llu\n", (unsigned long long)pairs, (unsigned long long)g_published); }
    if (disp != g_fresh + g_reused_frames || reused != g_reused_frames) { ok = 0; printf("  display counters disagree with the reader's own count\n"); }
    if ((int64_t)in_ring < 0 || in_ring > BMVR_RING_SLOTS) { ok = 0; printf("  accounting: %llu pairs published, %llu shown fresh, %llu dropped: %lld unaccounted\n",
                                                                   (unsigned long long)pairs, (unsigned long long)g_fresh, (unsigned long long)dropped_pairs, (long long)in_ring); }
    if (pairs == 0 || g_fresh == 0) { ok = 0; printf("  nothing flowed (pairs %llu, shown %llu)\n", (unsigned long long)pairs, (unsigned long long)g_fresh); }
    printf("%s %-38s published %7llu, shown fresh %6llu, reused %6llu, dropped %7llu (no free slot %llu), errors %llu\n", ok ? "PASS" : "FAIL", name,
           (unsigned long long)pairs, (unsigned long long)g_fresh, (unsigned long long)g_reused_frames, (unsigned long long)dropped_pairs,
           (unsigned long long)g_noslot, (unsigned long long)g_errors);
    bmvr_ring_destroy(&ring);
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    g_seconds = argc > 1 ? atoi(argv[1]) : 2;
    int fail = 0;
    fail |= run_case("game faster than display (200/s vs 90/s)", 5000, 300, 2000);
    fail |= run_case("game as fast as display", 11000, 300, 2000);
    fail |= run_case("game slower than display (30/s: reuse)", 33000, 300, 2000);
    fail |= run_case("slow GPU copies (writer outpaces GPU)", 1000, 8000, 2000);
    fail |= run_case("no delays at all (max contention)", 0, 0, 12);
    printf(fail ? "ring_test: FAILED\n" : "ring_test: PASS\n");
    return fail;
}
