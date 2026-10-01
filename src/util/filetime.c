#include "filetime.h"
#include "../image.h"
#include <string.h>
#include <stdint.h>
#if defined(_WIN32) && !defined(IMG2PNG_FORCE_POSIX)
#include <windows.h>
#else
#include <sys/stat.h>
#include <fcntl.h>
#if defined(IMG2PNG_FORCE_POSIX) && defined(_WIN32)
/* smoke-build shims: MinGW lacks these POSIX declarations */
#ifndef UTIME_OMIT
#define UTIME_OMIT ((1 << 30) - 2U)
#endif
#ifndef AT_FDCWD
#define AT_FDCWD -100
#endif
int utimensat(int dirfd, const char *pathname, const struct timespec *times, int flags);
#endif
#endif

/* Timestamp payload layout (both platforms):
 *   bytes 0..7  : creation time (Windows FILETIME bits; POSIX: sec+0)
 *   bytes 8..15 : modification time (Windows FILETIME bits; POSIX: sec<<32|nsec)
 * POSIX creation time cannot be set portably and is ignored on set. */

int copy_file_times(const char *src_path, const char *dst_path)
{
    uint8_t ctime[16] = {0}, mtime[16] = {0};
    if (get_file_times(src_path, ctime, mtime) != 0)
        return -1;
    return set_file_times(dst_path, ctime, mtime);
}

#if defined(_WIN32) && !defined(IMG2PNG_FORCE_POSIX)

int get_file_times(const char *path, void *ctime_out, void *mtime_out)
{
    void *h = img_open_read_attrs(path);
    if (!h)
        return -1;
    FILETIME c, m;
    BOOL ok = GetFileTime((HANDLE)h, &c, NULL, &m);
    img_close_handle(h);
    if (!ok)
        return -1;
    memset(ctime_out, 0, 16);
    memset(mtime_out, 0, 16);
    memcpy(ctime_out, &c, 8);
    memcpy(mtime_out, &m, 8);
    return 0;
}

int set_file_times(const char *path, const void *ctime, const void *mtime)
{
    void *h = img_open_write_attrs(path);
    if (!h)
        return -1;
    FILETIME ct, mt;
    memcpy(&ct, ctime, 8);
    memcpy(&mt, mtime, 8);
    BOOL ok = SetFileTime((HANDLE)h, &ct, NULL, &mt);
    img_close_handle(h);
    return ok ? 0 : -1;
}

#else /* POSIX */

static void pack_timespec(uint8_t *out, time_t sec, long nsec)
{
    uint64_t s = (uint64_t)sec;
    uint32_t ns = (uint32_t)nsec;
    memset(out, 0, 16);
    memcpy(out, &s, 8);
    memcpy(out + 8, &ns, 4);
}

static void unpack_timespec(const uint8_t *in, struct timespec *ts)
{
    uint64_t s;
    uint32_t ns;
    memcpy(&s, in, 8);
    memcpy(&ns, in + 8, 4);
    ts->tv_sec = (time_t)s;
    ts->tv_nsec = (long)ns;
}

int get_file_times(const char *path, void *ctime_out, void *mtime_out)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
#if defined(__APPLE__)
    pack_timespec(ctime_out, st.st_birthtime, 0);
#else
    pack_timespec(ctime_out, st.st_mtime, 0);
#endif
    long nsec = 0;
#if defined(__linux__) || defined(__APPLE__)
    nsec = (long)st.st_mtim.tv_nsec;
#endif
    pack_timespec(mtime_out, st.st_mtime, nsec);
    return 0;
}

int set_file_times(const char *path, const void *ctime, const void *mtime)
{
    (void)ctime;    /* POSIX has no portable way to set the creation time */
    struct timespec times[2];
    times[0].tv_sec = 0;            /* atime: leave unchanged? utimensat with
                                       UTIME_OMIT keeps it */
    times[0].tv_nsec = UTIME_OMIT;
    unpack_timespec(mtime, &times[1]);
    return utimensat(AT_FDCWD, path, times, 0) == 0 ? 0 : -1;
}

#endif
