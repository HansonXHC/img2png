#ifndef IMG2PNG_IMAGE_H
#define IMG2PNG_IMAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

/* Color types mirror the PNG model; every supported source format is
 * normalized into one of these while preserving its native bit depth. */
typedef enum {
    IMG_GRAY = 0,    /* 1/2/4/8/16-bit, 1 channel            */
    IMG_GRAY_ALPHA,  /* 8/16-bit, 2 channels                 */
    IMG_PALETTE,     /* 1/2/4/8-bit indices + palette        */
    IMG_RGB,         /* 8/16-bit, 3 channels                 */
    IMG_RGBA         /* 8/16-bit, 4 channels                 */
} img_color_t;

typedef struct {
    int width;
    int height;
    int bit_depth;          /* 1, 2, 4, 8 or 16 */
    img_color_t color;

    /* Rows packed exactly like PNG scanlines: each row starts on a byte
     * boundary, sub-byte depths packed MSB-first, 16-bit samples stored
     * big-endian.  The encoder can write rows directly from this buffer. */
    uint8_t *data;
    size_t rowstride;       /* bytes per row */

    /* PALETTE only */
    uint8_t  palette[256 * 3];
    uint8_t  pal_alpha[256];    /* per-entry alpha (tRNS) */
    int      pal_ncolors;
    int      has_pal_alpha;

    /* GRAY only: optional single transparent-gray value (tRNS) */
    int      has_gray_trns;
    uint16_t gray_trns_value;   /* sample value at source depth */
} img_image_t;

/* Bytes per output row for the given geometry. */
size_t img_rowstride(int width, int bit_depth, int channels);
int    img_channels(img_color_t color);

void   img_free(img_image_t *img);

/* Multi-frame image (animated GIF -> APNG).  Each frame is an RGBA
 * "region" image; x/y are its offset on the canvas.  delays_cs are GIF
 * native centiseconds; dispose is the raw GIF disposal mode (0..3);
 * loops: -1 = play once, 0 = infinite, n = n times. */
typedef struct {
    int width, height;      /* canvas size */
    int nframes;
    img_image_t *frames;    /* nframes entries, RGBA, region-sized */
    int *x, *y;
    int *delays_cs;
    int *dispose;
    int loops;
} img_animation_t;

void   img_free_anim(img_animation_t *anim);

/* big-endian 16-bit sample access (matches PNG sample order) */
static inline void img_st16be(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline unsigned img_ld16be(const uint8_t *p)   { return ((unsigned)p[0] << 8) | p[1]; }

/* Sub-byte (1/2/4-bit) sample access for rows packed MSB-first, the layout
 * PNG and BMP/ICO/TIFF all use.  Use these instead of open-coding the shift
 * arithmetic; the source and destination rows must share the same depth. */
static inline unsigned img_ld_bits(const uint8_t *row, int index, int bits)
{
    size_t bit = (size_t)index * (size_t)bits;
    return ((unsigned)row[bit >> 3] >> (8 - (int)(bit & 7) - bits)) &
           ((1u << bits) - 1u);
}

/* OR `v` into a zeroed destination row (rows are allocated with calloc). */
static inline void img_st_bits(uint8_t *row, int index, int bits, unsigned v)
{
    size_t bit = (size_t)index * (size_t)bits;
    row[bit >> 3] |= (uint8_t)((v & ((1u << bits) - 1u)) <<
                               (8 - (int)(bit & 7) - bits));
}

/* Open a file for reading/writing; paths come from the ANSI command line,
 * so they are widened with the ANSI codepage.  Returns NULL on failure. */
FILE *img_fopen_read(const char *path);
FILE *img_fopen_write(const char *path);
/* Win32 handles for GetFileTime/SetFileTime (wide, from ANSI path). */
void *img_open_read_attrs(const char *path);
void *img_open_write_attrs(const char *path);
void  img_close_handle(void *handle);


#ifdef __cplusplus
}
#endif

#endif
