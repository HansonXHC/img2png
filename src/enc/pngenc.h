#ifndef IMG2PNG_PNGENC_H
#define IMG2PNG_PNGENC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stddef.h>
#include "image.h"

typedef enum {
    PNGF_AUTO = 0,   /* libpng default: adaptive filtering */
    PNGF_NONE,
    PNGF_SUB,
    PNGF_UP,
    PNGF_AVG,
    PNGF_PAETH,
    PNGF_ALL,
    PNGF_FAST
} png_filter_mode_t;

typedef struct {
    int level;                  /* 0-9, zlib compression level */
    png_filter_mode_t filter;   /* PNGF_AUTO = libpng adaptive default */
    int auto_optimize;          /* drop useless alpha / gray detection */
    int recompress_png;         /* decode PNG inputs and re-encode them too */
} png_opts_t;

/* Encode *img as a PNG file.  Returns 0 on success, -1 with a reason in err. */
int png_write_file(const img_image_t *img, const png_opts_t *opts,
                   const char *path, char *err, size_t errlen);

const char *png_filter_name(png_filter_mode_t f);


#ifdef __cplusplus
}
#endif

#endif
