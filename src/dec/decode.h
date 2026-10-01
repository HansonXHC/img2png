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

/* Image-level optimizations for --auto mode: drops a fully opaque alpha
 * channel and converts gray-looking RGB to GRAY in place.  Returns 1 if
 * the image was modified. */
int img_auto_optimize(img_image_t *img);


#ifdef __cplusplus
}
#endif

#endif
