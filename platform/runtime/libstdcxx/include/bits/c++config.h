#ifndef RUNTIME_LIBSTDCXX_CXXCONFIG_H
#define RUNTIME_LIBSTDCXX_CXXCONFIG_H

#include_next <bits/c++config.h>

#if defined(THREADX_STD_ENABLED)

#if defined(HAL_PLATFORM_STM32)
/* The FileX Newlib retarget supplies these POSIX interfaces. */
#define _GLIBCXX_HAVE_DIRENT_H 1
#define _GLIBCXX_HAVE_STRUCT_DIRENT_D_TYPE 1
#define _GLIBCXX_USE_MKDIR 1
#define _GLIBCXX_USE_GETCWD 1
#define _GLIBCXX_USE_CHDIR 1
#define _GLIBCXX_HAVE_SYS_STATVFS_H 1

/* GCC 16's Newlib configuration advertises the *at interfaces, but this
 * FileX adapter has no directory file descriptors. Keep libstdc++ on its
 * path-based fallback, as GCC 15 already did for this target. */
#undef _GLIBCXX_HAVE_OPENAT
#undef _GLIBCXX_HAVE_UNLINKAT

/* FileX has no ownership or permission model. Do not select libstdc++ paths
 * that call Newlib's unsupported chmod family. */
#undef _GLIBCXX_USE_CHMOD
#undef _GLIBCXX_USE_FCHMOD
#undef _GLIBCXX_USE_FCHMODAT

/* The project supplies an Arm TLS ABI implementation in tls.cpp. Enable
 * libstdc++'s thread-local call_once trampoline instead of its global-functor
 * fallback, which has known reentrancy limitations. */
#define _GLIBCXX_HAVE_TLS 1
#endif

/* The Linux libstdc++ configuration enables pthread and futex fast paths
 * that bypass gthreads. They are incompatible with the ThreadX handle types
 * supplied by this overlay, so force the generic gthread implementations. */
#undef _GLIBCXX_HAVE_LINUX_FUTEX
#undef _GLIBCXX_HAVE_LINUX_FUTEX_PRIVATE
#undef _GLIBCXX_NATIVE_THREAD_ID
#undef _GLIBCXX_USE_PTHREAD_COND_CLOCKWAIT
#undef _GLIBCXX_USE_PTHREAD_RWLOCK_CLOCKLOCK

/* Keep sleep_for in the same ThreadX clock domain as steady_clock. The Linux
 * libstdc++ configuration otherwise calls nanosleep directly. */
#undef _GLIBCXX_USE_NANOSLEEP

#undef _GLIBCXX_USE_PTHREAD_MUTEX_CLOCKLOCK
#define _GLIBCXX_USE_PTHREAD_MUTEX_CLOCKLOCK 0

#undef _GLIBCXX_USE_PTHREAD_RWLOCK_T
#define _GLIBCXX_USE_PTHREAD_RWLOCK_T 0

#endif /* THREADX_STD_ENABLED */

#endif /* RUNTIME_LIBSTDCXX_CXXCONFIG_H */
