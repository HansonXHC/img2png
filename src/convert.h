#ifndef IMG2PNG_CONVERT_H
#define IMG2PNG_CONVERT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include "enc/pngenc.h"

typedef struct {
    int ok;
    int skipped_png;        /* input was already a PNG */
    int out_w, out_h;
    int out_depth;
    int out_color;          /* img_color_t of the written PNG */
    unsigned long long in_size, out_size;
    double secs;
    char err[256];
} img2png_result_t;

/* Decode one image file and write it out as PNG.  Returns 0 on success
 * (or when skipped) and fills *result; -1 on failure with result->err. */
int img2png_convert(const char *in_path, const char *out_path,
                    const png_opts_t *opts, int keep_time,
                    img2png_result_t *result);

const char *img2png_color_name(int color);

/* Extension-based check for BMP/TGA/PNM/ICO/JPEG/PNG (directory scanners). */
int img2png_is_supported_file(const char *path);


#ifdef __cplusplus
}
#endif

#endif
