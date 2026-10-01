#ifndef IMG2PNG_PLATFORM_H
#define IMG2PNG_PLATFORM_H

/* Small cross-platform helpers shared by the CLI and core.
 * IMG2PNG_FORCE_POSIX lets the POSIX branches be syntax-checked on Windows. */

#if defined(_WIN32) && !defined(IMG2PNG_FORCE_POSIX)

#include <windows.h>

static inline int img_num_cpus(void)
{
    return (int)GetCurrentProcessorNumber();
}

static inline void img_msleep(int ms)
{
    Sleep(ms);
}

static inline double img_now_sec(void)
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

static inline void img_mkdir(const char *path)
{
    CreateDirectoryA(path, NULL);   /* ok if it already exists */
}

#else /* POSIX (Linux / macOS) */

#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(IMG2PNG_FORCE_POSIX) && defined(_WIN32)
/* smoke-build shims: MinGW lacks these POSIX declarations */
#ifndef _SC_NPROCESSORS_ONLN
#define _SC_NPROCESSORS_ONLN 84
#endif
long sysconf(int name);
#define img2png_raw_mkdir(p) mkdir(p)
#else
#define img2png_raw_mkdir(p) mkdir(p, 0755)
#endif

static inline int img_num_cpus(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

static inline void img_msleep(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static inline double img_now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static inline void img_mkdir(const char *path)
{
    img2png_raw_mkdir(path);        /* ok if it already exists */
}

#endif

#endif
