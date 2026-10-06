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

/* ---------------------------------------------------------------------------
 * Metadata
 *
 * Sources carry their EXIF / ICC / XMP / text / resolution data in different
 * containers; all of it is normalised into this struct so the PNG encoder can
 * write it back out as eXIf / iCCP / iTXt / tEXt / pHYs.  Pointers are owned
 * by the struct; zeroing it (memset) yields "no metadata".
 * ------------------------------------------------------------------------ */

/* Which PNG text chunk the value came from, so it is written back the same
 * way (an iTXt carrying non-Latin-1 text must not become a tEXt). */
typedef enum {
    IMG_TEXT_TEXT   = 0,    /* tEXt   - Latin-1, uncompressed        */
    IMG_TEXT_ZTXT   = 1,    /* zTXt   - Latin-1, deflate-compressed  */
    IMG_TEXT_ITXT   = 2,    /* iTXt   - UTF-8, uncompressed          */
    IMG_TEXT_ITXT_Z = 3     /* iTXt   - UTF-8, deflate-compressed    */
} img_text_kind_t;

typedef struct {
    char *key;              /* 1-79 bytes, no leading/trailing/adjacent space */
    char *value;            /* UTF-8 */
    int   kind;             /* img_text_kind_t */
} img_text_t;

typedef struct img_meta {
    int      have_dpi;
    double   dpi_x, dpi_y;      /* dots per inch */

    uint8_t *exif;              /* TIFF-format EXIF payload, no "Exif\0\0" */
    size_t   exif_len;

    uint8_t *icc;               /* ICC profile, uncompressed */
    size_t   icc_len;

    char    *xmp;               /* XMP packet (UTF-8 XML), NUL-terminated */
    size_t   xmp_len;

    img_text_t *text;           /* text chunks kept from a PNG source */
    int         ntext;
} img_meta_t;

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

    /* everything that is not pixel data */
    img_meta_t meta;
} img_image_t;

void img_meta_free(img_meta_t *m);

/* Copy a blob into the metadata (replacing any previous value).  Returns 0 on
 * success, -1 on out-of-memory (the metadata is left unchanged). */
int img_meta_set_blob(uint8_t **dst, size_t *dstlen, const void *src, size_t len);

/* Store EXIF, stripping a "Exif\0\0" identifier and/or a leading 4-byte
 * offset (HEIF/AVIF wrap it that way) and validating the TIFF header.
 * Returns 1 if usable EXIF was stored, 0 if the payload was not EXIF. */
int img_meta_set_exif(img_meta_t *m, const void *src, size_t len);

/* Read the resolution out of a TIFF-format EXIF payload.  unit: 2 = inch,
 * 3 = cm.  Returns 1 when both values were found. */
int img_exif_resolution(const uint8_t *exif, size_t len,
                        double *x, double *y, int *unit);

/* Read the Orientation tag (1 = normal) out of an EXIF payload; 0 if absent. */
int img_exif_orientation(const uint8_t *exif, size_t len);

/* Append a text key/value pair (used by the PNG reader).  Returns 0/-1. */
int img_meta_add_text(img_meta_t *m, const char *key, const char *value,
                      int kind);

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
