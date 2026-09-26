/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gm_heartbeat.c — opt-in heartbeat thread emitting CMD_GET_STATUS at interval_ms cadence.
 *
 * Per caller-owned threading: callers that don't need this never call
 * gm_heartbeat_start. The heartbeat is useful during idle periods; active BLIT
 * traffic serves as the implicit heartbeat during gameplay.
 *
 * Default cadence = 100 ms idle ping.
 *
 * Threading model (POSIX port): Win32 CreateThread (MinGW-w64 native, no
 * pthreads-win32 dependency) on one side, pthread_create on the other. The
 * loop body is platform-neutral (heartbeat_loop); only the thread entry
 * point and the stop path differ by platform:
 *   1. Send CMD_GET_STATUS (0x05) via gm_send_get_status_internal
 *   2. Sleep interval_ms in 10ms slices to allow timely shutdown
 *
 * Mitigations:
 *   - hb_stop is a gm_hb_flag (gm_hb_compat.h), read/written via
 *     GM_HB_FLAG_GET/SET on both sides
 *   - gm_heartbeat_stop signals, then WAITS WITH A BOUND
 *     (interval_ms * 2 + 100ms) before returning — never an unbounded block
 *     on the caller
 *   - THREADING CONTRACT documented in gm.h
 *
 * Three POSIX prohibitions (a two-speed library is refused):
 *   - never pthread_timedjoin_np (a glibc extension, ABSENT from macOS);
 *   - never pthread_condattr_setclock (absent from macOS);
 *   - never a bare, unbounded pthread_join: it would compile and work almost
 *     every time, and would lose the file's guarantee in the one case where
 *     it matters. See gm_heartbeat_stop below: the deadline is measured
 *     against WALL-CLOCK time (CLOCK_REALTIME), the only common denominator
 *     between Linux and macOS.
 */

/* winsock2.h MUST precede windows.h on MinGW-w64 to avoid redefinition
 * conflicts (ws2def.h vs winsock.h macro clash). gm_internal.h includes
 * winsock2.h, so including it first is sufficient — but we make the order
 * explicit here in case the TU is ever compiled in isolation. */
#include "gm.h"
#include "gm_internal.h"   /* includes winsock2.h then windows.h in correct order */

#include <stdlib.h>
#ifndef _WIN32
#  include <time.h>        /* nanosleep, clock_gettime, CLOCK_REALTIME */
#endif

/* ---------------------------------------------------------------------------
 * Platform-neutral loop body, shared by both platforms (same pattern as the
 * software receiver's network thread).
 * ------------------------------------------------------------------------- */
static void heartbeat_loop(gm_handle *h)
{
    while (!GM_HB_FLAG_GET(&h->hb_stop)) {
        /* Tolerate a transient send error: a network hiccup must not kill the thread. */
        (void)gm_send_get_status_internal(h);

        /* 10 ms slices so gm_heartbeat_stop doesn't wait up to interval_ms. */
        unsigned remaining = h->hb_interval_ms;
        while (remaining > 0 && !GM_HB_FLAG_GET(&h->hb_stop)) {
            unsigned slice = (remaining > 10u) ? 10u : remaining;
#ifdef _WIN32
            Sleep((DWORD)slice);
#else
            /* RELATIVE nanosleep: clock_nanosleep does NOT exist on macOS (no
             * version has ever shipped it), and TIMER_ABSTIME would solve a
             * problem this loop doesn't have -- a 10 ms slice needs no
             * particular precision. */
            struct timespec ts;
            ts.tv_sec  = 0;
            ts.tv_nsec = (long)slice * 1000000L;
            nanosleep(&ts, NULL);
#endif
            remaining -= slice;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Thread entry point, per platform — the POSIX signature void *(*)(void*) is
 * incompatible with DWORD WINAPI.
 * ------------------------------------------------------------------------- */
#ifdef _WIN32
static DWORD WINAPI heartbeat_proc(LPVOID arg)
{
    heartbeat_loop((gm_handle *)arg);
    return 0;
}
#else
static void *heartbeat_thread_entry(void *arg)
{
    gm_handle *h = (gm_handle *)arg;
    heartbeat_loop(h);
    /* Signal completion BEFORE returning: this is what gm_heartbeat_stop waits for, bounded. */
    pthread_mutex_lock(&h->hb_done_mtx);
    h->hb_done = 1;
    pthread_cond_signal(&h->hb_done_cond);
    pthread_mutex_unlock(&h->hb_done_mtx);
    return NULL;
}
#endif

/* ---------------------------------------------------------------------------
 * gm_heartbeat_start — spawn the heartbeat thread.
 *
 * Returns 0 on success, -1 on error (already running, NULL handle, or
 * CreateThread/pthread_create failure). interval_ms == 0 → use the default
 * of 100ms.
 * ------------------------------------------------------------------------- */
int gm_heartbeat_start(gm_handle *h, unsigned interval_ms)
{
    if (!h) return -1;
    if (h->hb_running) return -1;              /* already running */

    if (interval_ms == 0) interval_ms = 100;   /* default cadence */
    h->hb_interval_ms = interval_ms;

    /* Signal NOT-stop before spawning (caller may have called stop/start). */
    GM_HB_FLAG_SET(&h->hb_stop, 0);

#ifdef _WIN32
    h->hb_thread = CreateThread(
        NULL,           /* default security */
        0,              /* default stack */
        heartbeat_proc,
        h,
        0,              /* run immediately */
        NULL            /* thread ID not needed */
    );
    if (h->hb_thread == NULL) return -1;
#else
    pthread_mutex_init(&h->hb_done_mtx, NULL);
    pthread_cond_init(&h->hb_done_cond, NULL);
    h->hb_done = 0;
    if (pthread_create(&h->hb_thread, NULL, heartbeat_thread_entry, h) != 0) {
        pthread_cond_destroy(&h->hb_done_cond);
        pthread_mutex_destroy(&h->hb_done_mtx);
        return -1;
    }
#endif
    h->hb_running = 1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * gm_heartbeat_stop — signal the thread and join with bounded timeout.
 *
 * Returns 0 if the thread was running and joined/detached, -1 if not running
 * or NULL handle. Timeout is interval_ms * 2 + 100ms so the 10ms-slice loop
 * in the thread has enough iterations to notice hb_stop.
 *
 * POSIX: the deadline is measured against CLOCK_REALTIME (wall-clock time)
 * — the ONLY common denominator between Linux and macOS.
 * pthread_condattr_setclock, which would allow CLOCK_MONOTONIC, does not
 * exist on Darwin. The residual risk (an NTP step during the wait) affects
 * 100 to 300 ms of a STOP path, never a real-time path.
 * ------------------------------------------------------------------------- */
int gm_heartbeat_stop(gm_handle *h)
{
    if (!h || !h->hb_running) return -1;

    /* Signal thread to exit. */
    GM_HB_FLAG_SET(&h->hb_stop, 1);

    /* Wait with bounded timeout. interval_ms * 2 + 100ms accounts for:
     *   - up to interval_ms of sliced sleep remaining
     *   - one full send latency
     *   - 100ms safety margin for scheduler jitter */
    const unsigned timeout_ms = h->hb_interval_ms * 2u + 100u;

#ifdef _WIN32
    WaitForSingleObject(h->hb_thread, (DWORD)timeout_ms);
    CloseHandle(h->hb_thread);
    h->hb_thread = NULL;
#else
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec  += (time_t)(timeout_ms / 1000u);
    deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&h->hb_done_mtx);
    int rc = 0;
    while (!h->hb_done && rc == 0)
        rc = pthread_cond_timedwait(&h->hb_done_cond, &h->hb_done_mtx, &deadline);
    const int done = h->hb_done;
    pthread_mutex_unlock(&h->hb_done_mtx);

    if (done) {
        pthread_join(h->hb_thread, NULL);      /* immediate: the thread has just signaled its end */
        /* Destroy ONLY in this case: a detached thread might still touch the pair. */
        pthread_cond_destroy(&h->hb_done_cond);
        pthread_mutex_destroy(&h->hb_done_mtx);
    } else {
        /* Bound reached: stop waiting. Detach rather than abandon, so as not
         * to leak the thread's resources if it eventually does exit. */
        pthread_detach(h->hb_thread);
    }
#endif
    h->hb_running = 0;
    return 0;
}
