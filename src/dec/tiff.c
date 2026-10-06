#include "decode.h"
#include <stdlib.h>
#include <string.h>
#include <tiffio.h>

static int fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen)
        snprintf(err, errlen, "%s", msg);
    return -1;
}

/* libtiff client callbacks operating on our FILE* (portable, avoids
 * fd/stdio buffering conflicts) */
static tsize_t tiff_read_proc(thandle_t h, tdata_t buf, tsize_t n)
{
    return (tsize_t)fread(buf, 1, (size_t)n, (FILE *)h);
}
static tsize_t tiff_write_proc(thandle_t h, tdata_t buf, tsize_t n)
{
    (void)h; (void)buf; (void)n;
    return 0;                       /* read-only */
}
static toff_t tiff_seek_proc(thandle_t h, toff_t off, int whence)
{
    if (fseek((FILE *)h, (long)off, whence) != 0)
        return (toff_t)-1;
    return (toff_t)ftell((FILE *)h);
}
static int tiff_close_proc(thandle_t h)
{
    (void)h;
    return 0;                       /* do not close our FILE* */
}
static toff_t tiff_size_proc(thandle_t h)
{
    long cur = ftell((FILE *)h);
    fseek((FILE *)h, 0, SEEK_END);
    long size = ftell((FILE *)h);
    fseek((FILE *)h, cur, SEEK_SET);
    return (toff_t)size;
}
static int tiff_map_proc(thandle_t h, tdata_t *b, toff_t *s)
{
    (void)h; (void)b; (void)s;
    return 0;
}
static void tiff_unmap_proc(thandle_t h, tdata_t b, toff_t s)
{
    (void)h; (void)b; (void)s;
}

/* bit-depth-matched decode for the simple contiguous variants:
 *   MINISBLACK / MINISWHITE gray (1/2/4/8/16-bit)
 *   PALETTE (1/2/4/8-bit)
 *   RGB (8/16-bit, 3 or 4 samples, contiguous)
 * Everything else (CMYK, YCbCr, tiled, float, separate planes, ...)
 * is handled by the TIFFReadRGBAImage fallback in tiff_decode(). */
static int tiff_decode_matched(TIFF *tif, img_image_t *img,
                               char *err, size_t errlen)
{
    uint32_t w = 0, h = 0;
    uint16_t bits = 0, samples = 0, photometric = 0, planar = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    TIFFGetField(tif, TIFFTAG_BITSPERSAMPLE, &bits);
    TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &samples);
    TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &photometric);
    TIFFGetField(tif, TIFFTAG_PLANARCONFIG, &planar);
    if (w == 0 || h == 0 || w > 65535 || h > 65535)
        return fail(err, errlen, "tiff: bad dimensions");
    if (planar != PLANARCONFIG_CONTIG)
        return -1;                              /* caller falls back */

    memset(img, 0, sizeof(*img));
    img->width = (int)w;
    img->height = (int)h;

    int gray = (photometric == PHOTOMETRIC_MINISBLACK ||
                photometric == PHOTOMETRIC_MINISWHITE);
    int palette = (photometric == PHOTOMETRIC_PALETTE);
    int rgb = (photometric == PHOTOMETRIC_RGB);
    if (!gray && !palette && !rgb)
        return -1;                              /* caller falls back */

    tsize_t ssize = TIFFScanlineSize(tif);
    uint8_t *sbuf = (uint8_t *)_TIFFmalloc(ssize > 0 ? (size_t)ssize : 1);
    if (!sbuf)
        return fail(err, errlen, "tiff: out of memory");

    /* ---- gray (1/2/4/8/16-bit) ---- */
    if (gray && samples == 1 &&
        (bits == 1 || bits == 2 || bits == 4 || bits == 8 || bits == 16)) {
        img->color = IMG_GRAY;
        img->bit_depth = (int)bits;
        int invert = (photometric == PHOTOMETRIC_MINISWHITE);
        unsigned maxval = (1u << bits) - 1u;
        img->rowstride = img_rowstride(w, bits, 1);
        img->data = (uint8_t *)calloc((size_t)h, img->rowstride);
        if (!img->data) {
            _TIFFfree(sbuf);
            return fail(err, errlen, "tiff: out of memory");
        }
        for (uint32_t y = 0; y < h; y++) {
            if (TIFFReadScanline(tif, sbuf, y, 0) < 0) {
                _TIFFfree(sbuf);
                img_free(img);
                return fail(err, errlen, "tiff: scanline read failed");
            }
            uint8_t *drow = img->data + (size_t)y * img->rowstride;
            if (bits == 16) {
                for (uint32_t x = 0; x < w; x++) {
                    unsigned v = ((const uint16_t *)sbuf)[x];
                    if (invert) v = maxval - v;
                    img_st16be(drow + (size_t)x * 2, v);
                }
            } else if (bits == 8) {
                for (uint32_t x = 0; x < w; x++)
                    drow[x] = invert ? (uint8_t)(255 - sbuf[x]) : sbuf[x];
            } else { /* 1/2/4-bit packed MSB-first */
                for (uint32_t x = 0; x < w; x++) {
                    int shift = 8 - (int)bits - (int)((x * bits) % 8);
                    unsigned v = (sbuf[(x * bits) >> 3] >> shift) & maxval;
                    if (invert) v = maxval - v;
                    size_t byte = x >> 3;
                    int dshift = 7 - (int)(x & 7);
                    if (v)
                        drow[byte] |= (uint8_t)(1u << dshift);
                }
            }
        }
        _TIFFfree(sbuf);
        return 0;
    }

    /* ---- palette (1/2/4/8-bit) ---- */
    if (palette && samples == 1 &&
        (bits == 1 || bits == 2 || bits == 4 || bits == 8)) {
        uint16_t *rmap = NULL, *gmap = NULL, *bmap = NULL;
        if (!TIFFGetField(tif, TIFFTAG_COLORMAP, &rmap, &gmap, &bmap) ||
            !rmap || !gmap || !bmap) {
            _TIFFfree(sbuf);
            return fail(err, errlen, "tiff: palette without colormap");
        }
        int nc = (1 << bits);
        img->color = IMG_PALETTE;
        img->bit_depth = (int)bits;
        img->pal_ncolors = nc;
        for (int i = 0; i < nc; i++) {
            /* libtiff colormaps are 16-bit; scale to 8-bit (rounded) */
            img->palette[i * 3 + 0] =
                (uint8_t)((rmap[i] * 255u + 32767u) / 65535u);
            img->palette[i * 3 + 1] =
                (uint8_t)((gmap[i] * 255u + 32767u) / 65535u);
            img->palette[i * 3 + 2] =
                (uint8_t)((bmap[i] * 255u + 32767u) / 65535u);
            img->pal_alpha[i] = 255;
        }
        img->rowstride = img_rowstride(w, bits, 1);
        img->data = (uint8_t *)calloc((size_t)h, img->rowstride);
        if (!img->data) {
            _TIFFfree(sbuf);
            return fail(err, errlen, "tiff: out of memory");
        }
        for (uint32_t y = 0; y < h; y++) {
            if (TIFFReadScanline(tif, sbuf, y, 0) < 0) {
                _TIFFfree(sbuf);
                img_free(img);
                return fail(err, errlen, "tiff: scanline read failed");
            }
            uint8_t *drow = img->data + (size_t)y * img->rowstride;
            if (bits == 8) {
                memcpy(drow, sbuf, w);
            } else {
                for (uint32_t x = 0; x < w; x++) {
                    int shift = 8 - (int)bits - (int)((x * bits) % 8);
                    unsigned idx = (sbuf[(x * bits) >> 3] >> shift) &
                                   ((1u << bits) - 1u);
                    size_t byte = x >> 3;
                    int dshift = 7 - (int)(x & 7);
                    if (idx)
                        drow[byte] |= (uint8_t)(1u << dshift);
                }
            }
        }
        _TIFFfree(sbuf);
        return 0;
    }

    /* ---- RGB / RGBA (contiguous, 8/16-bit) ---- */
    if (rgb && (bits == 8 || bits == 16) &&
        (samples == 3 || samples == 4)) {
        int has_alpha = (samples == 4);
        img->color = has_alpha ? IMG_RGBA : IMG_RGB;
        img->bit_depth = (int)bits;
        int ch = has_alpha ? 4 : 3;
        img->rowstride = img_rowstride(w, bits, ch);
        img->data = (uint8_t *)calloc((size_t)h, img->rowstride);
        if (!img->data) {
            _TIFFfree(sbuf);
            return fail(err, errlen, "tiff: out of memory");
        }
        uint16_t extra = 0;
        uint16_t *extras = NULL;
        uint16_t nextras = 0;
        if (has_alpha &&
            TIFFGetField(tif, TIFFTAG_EXTRASAMPLES, &nextras, &extras) &&
            nextras > 0)
            extra = extras[0];
        for (uint32_t y = 0; y < h; y++) {
            if (TIFFReadScanline(tif, sbuf, y, 0) < 0) {
                _TIFFfree(sbuf);
                img_free(img);
                return fail(err, errlen, "tiff: scanline read failed");
            }
            uint8_t *drow = img->data + (size_t)y * img->rowstride;
            if (bits == 8) {
                memcpy(drow, sbuf, (size_t)w * samples);
                if (has_alpha && extra == EXTRASAMPLE_UNSPECIFIED)
                    for (uint32_t x = 0; x < w; x++)
                        drow[x * 4 + 3] = 255;
            } else { /* 16-bit: libtiff yields host order -> big-endian out */
                for (uint32_t x = 0; x < w; x++) {
                    const uint16_t *s = &((const uint16_t *)sbuf)[x * samples];
                    for (int c = 0; c < ch; c++) {
                        unsigned v = s[c];
                        if (has_alpha && c == 3 &&
                            extra == EXTRASAMPLE_UNSPECIFIED)
                            v = 65535;
                        img_st16be(drow + (size_t)x * ch * 2 + c * 2, v);
                    }
                }
            }
        }
        _TIFFfree(sbuf);
        return 0;
    }

    _TIFFfree(sbuf);
    return -1;                                  /* caller falls back */
}

int tiff_decode(FILE *f, img_image_t *img, char *err, size_t errlen)
{
    int fd = fileno(f);
    if (fd < 0)
        return fail(err, errlen, "tiff: invalid file descriptor");
    TIFF *tif = TIFFClientOpen("memory", "r", (thandle_t)f,
                               tiff_read_proc, tiff_write_proc,
                               tiff_seek_proc, tiff_close_proc,
                               tiff_size_proc, tiff_map_proc, tiff_unmap_proc);
    if (!tif)
        return fail(err, errlen, "tiff: not a readable TIFF");

    memset(img, 0, sizeof(*img));

    /* try the bit-depth-matched path first */
    int rc = tiff_decode_matched(tif, img, err, errlen);
    if (rc == 0) {
        TIFFClose(tif);
        return 0;
    }

    /* fallback: the RGBA interface handles photometric variants, strips/
     * tiles and bit depths, producing 8-bit RGBA (A=255 when opaque) */
    uint32_t w = 0, h = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    if (w == 0 || h == 0) {
        TIFFClose(tif);
        return fail(err, errlen, "tiff: bad dimensions");
    }

    img_free(img);
    uint32_t *raster = (uint32_t *)_TIFFmalloc((size_t)w * h * sizeof(uint32_t));
    if (!raster) {
        TIFFClose(tif);
        return fail(err, errlen, "tiff: out of memory");
    }
    if (!TIFFReadRGBAImageOriented(tif, w, h, raster, ORIENTATION_TOPLEFT, 1)) {
        _TIFFfree(raster);
        TIFFClose(tif);
        return fail(err, errlen, "tiff: unsupported TIFF variant or read error");
    }
    TIFFClose(tif);

    memset(img, 0, sizeof(*img));
    img->width = (int)w;
    img->height = (int)h;
    img->bit_depth = 8;
    img->color = IMG_RGBA;   /* --auto can drop a fully-opaque alpha channel */
    img->rowstride = img_rowstride(img->width, 8, 4);
    img->data = (uint8_t *)malloc((size_t)w * h * 4);
    if (!img->data) {
        _TIFFfree(raster);
        return fail(err, errlen, "tiff: out of memory");
    }
    /* raster is packed ABGR uint32 on little-endian -> memory bytes R,G,B,A */
    memcpy(img->data, raster, (size_t)w * h * 4);
    _TIFFfree(raster);
    return 0;
}
