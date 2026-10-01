#include "filetime.h"
#include "../image.h"
#include <string.h>
#include <windows.h>

int copy_file_times(const char *src_path, const char *dst_path)
{
    uint8_t ctime[8], mtime[8];
    if (get_file_times(src_path, ctime, mtime) != 0)
        return -1;
    return set_file_times(dst_path, ctime, mtime);
}

int get_file_times(const char *path, void *ctime_out, void *mtime_out)
{
    void *h = img_open_read_attrs(path);
    if (!h)
        return -1;
    FILETIME ctime, mtime;
    BOOL ok = GetFileTime((HANDLE)h, &ctime, NULL, &mtime);
    img_close_handle(h);
    if (!ok)
        return -1;
    memcpy(ctime_out, &ctime, 8);
    memcpy(mtime_out, &mtime, 8);
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
