#ifndef IMG2PNG_APNGENC_H
#define IMG2PNG_APNGENC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stddef.h>
#include "image.h"
#include "pngenc.h"

/* Encode an animation as an APNG file (acTL/fcTL/fdAT, via the libpng
 * APNG patch).  Frame 0 is stored as the default IDAT image; subsequent
 * frames become fcTL + fdAT chunks.  Returns 0 on success, -1 with a
 * reason in err. */
int png_write_apng(const img_animation_t *anim, const png_opts_t *opts,
                   const char *path, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif
