#ifndef IMG2PNG_FILETIME_H
#define IMG2PNG_FILETIME_H

#ifdef __cplusplus
extern "C" {
#endif

/* Copy the creation time and last-write time of src_path onto dst_path.
 * Returns 0 on success, -1 on failure (not fatal for a conversion). */
int copy_file_times(const char *src_path, const char *dst_path);

/* Read creation + last-write times into caller-provided 16-byte buffers
 * (platform timestamp payload), so they survive an in-place overwrite. */
int get_file_times(const char *path, void *ctime_out, void *mtime_out);
int set_file_times(const char *path, const void *ctime, const void *mtime);


#ifdef __cplusplus
}
#endif

#endif
