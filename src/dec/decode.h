#ifndef IMG2PNG_DECODE_H
#define IMG2PNG_DECODE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stddef.h>
#include "image.h"

typedef struct {
    img_color_t color;
    int bit_depth;
    int has_alpha;       /* source genuinely carries alpha */
    int has_gray;        /* source is grayscale */
} src_format_t;

/* All decoders fill *img on success (return 0) and leave the caller to
 * img_free().  On failure (return -1) they write a reason into err. */

int bmp_decode(FILE *f, img_image_t *img, char *err, size_t errlen);
int tga_decode(FILE *f, img_image_t *img, char *err, size_t errlen);
int pnm_decode(FILE *f, img_image_t *img, char *err, size_t errlen);
int ico_decode(FILE *f, img_image_t *img, char *err, size_t errlen);
int jpeg_decode(FILE *f, img_image_t *img, char *err, size_t errlen);
/* PNG reader used for PNG-compressed entries inside .ico files */
int png_decode_mem(const uint8_t *data, size_t size, img_image_t *img,
                   char *err, size_t errlen);

/* Newer formats (vendored codecs) */
int gif_decode(FILE *f, img_image_t *img, char *err, size_t errlen);
/* Decode all frames of an animated GIF into canvas-composited RGBA
 * regions (see img_animation_t).  Single-frame GIFs can go through
 * gif_decode() instead. */
int gif_decode_anim(FILE *f, img_animation_t *anim, char *err, size_t errlen);
int qoi_decode_file(FILE *f, img_image_t *img, char *err, size_t errlen);
int webp_decode(FILE *f, img_image_t *img, char *err, size_t errlen);
int tiff_decode(FILE *f, img_image_t *img, char *err, size_t errlen);
int heif_decode(FILE *f, img_image_t *img, char *err, size_t errlen);
int avif_decode(FILE *f, img_image_t *img, char *err, size_t errlen);

/* Image-level optimizations for --auto mode: drops a fully opaque alpha
 * channel and converts gray-looking RGB to GRAY in place.  Returns 1 if
 * the image was modified. */
int img_auto_optimize(img_image_t *img);


#ifdef __cplusplus
}
#endif

#endif
