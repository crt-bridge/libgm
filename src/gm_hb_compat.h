/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gm_hb_compat.h — portable heartbeat thread and stop flag.
 *
 * Model: the software receiver's mutex wrapper — same shape, same reason. A
 * file OF ITS OWN for libgm: the library must stay buildable ALONE, without
 * the software receiver's sources or
 * RetroArch, so the shape is reproduced rather than the file shared. This
 * rules out both a gm_plat.h that would centralize all the porting and
 * separate files per platform.
 *
 * MUST BE INCLUDED AFTER <winsock2.h> on Windows: <windows.h> pulls in
 * <winsock.h> if winsock2 isn't already there, and the two contradict each
 * other (same warning as the software receiver's mutex wrapper).
 */
#ifndef LIBGM_GM_HB_COMPAT_H
#define LIBGM_GM_HB_COMPAT_H

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
typedef HANDLE        gm_hb_thread;
typedef volatile LONG gm_hb_flag;
/* InterlockedCompareExchange(&x, 0, 0) is not a compare-and-swap: it is the
 * pre-C11 Win32 idiom for an atomic READ with a full barrier. Kept as is. */
#  define GM_HB_FLAG_SET(p, v)  InterlockedExchange((p), (v))
#  define GM_HB_FLAG_GET(p)     InterlockedCompareExchange((p), 0, 0)
#else
#  include <pthread.h>
#  include <stdatomic.h>
typedef pthread_t  gm_hb_thread;
typedef atomic_int gm_hb_flag;
/* C11, not a compiler extension: both Makefiles already compile with
 * -std=c11, and memory_order_seq_cst (the default for these two functions)
 * is at least as strong as InterlockedExchange's implicit barrier. */
#  define GM_HB_FLAG_SET(p, v)  atomic_store((p), (v))
#  define GM_HB_FLAG_GET(p)     atomic_load((p))
#endif

#endif /* LIBGM_GM_HB_COMPAT_H */
