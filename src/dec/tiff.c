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
            } else { /* 1/2/4-bit packed MSB-first, same depth out */
                for (uint32_t x = 0; x < w; x++) {
                    unsigned v = img_ld_bits(sbuf, (int)x, (int)bits);
                    if (invert) v = maxval - v;
                    img_st_bits(drow, (int)x, (int)bits, v);
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
                /* 1/2/4-bit indices, same depth out (img->data is calloc'd) */
                for (uint32_t x = 0; x < w; x++)
                    img_st_bits(drow, (int)x, (int)bits,
                                img_ld_bits(sbuf, (int)x, (int)bits));
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

/* ---------------------------------------------------------------------------
 * Metadata
 *
 * A TIFF's IFD0 already holds the tags EXIF cares about, so the ones worth
 * keeping are copied into a small EXIF payload that the PNG encoder writes as
 * an eXIf chunk.  Only IFD0 tags are mirrored - the EXIF sub-IFD (exposure
 * time, F-number, ISO, ...) is not rebuilt.
 * ------------------------------------------------------------------------ */

typedef struct {
    uint8_t *buf;
    size_t   len, cap;
} eb_t;

static int eb_put(eb_t *b, const void *src, size_t n)
{
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n)
            cap *= 2;
        uint8_t *p = (uint8_t *)realloc(b->buf, cap);
        if (!p)
            return -1;
        b->buf = p;
        b->cap = cap;
    }
    memcpy(b->buf + b->len, src, n);
    b->len += n;
    return 0;
}

static int eb_u16(eb_t *b, unsigned v)
{
    uint8_t x[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    return eb_put(b, x, 2);
}

static int eb_u32(eb_t *b, unsigned v)
{
    uint8_t x[4] = { (uint8_t)v, (uint8_t)(v >> 8),
                     (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return eb_put(b, x, 4);
}

#define EXIF_MAX_ENT 12

typedef struct {
    unsigned tag, type, count;
    uint8_t *data;      /* value bytes (little-endian, as EXIF requires here) */
    size_t   dlen;
} eb_ent_t;

static int ent_add(eb_ent_t *e, int *n, unsigned tag, unsigned type,
                   unsigned count, const void *data, size_t dlen)
{
    if (*n >= EXIF_MAX_ENT)
        return -1;
    eb_ent_t *x = &e[*n];
    memset(x, 0, sizeof(*x));
    x->tag = tag;
    x->type = type;
    x->count = count;
    if (dlen) {
        x->data = (uint8_t *)malloc(dlen);
        if (!x->data)
            return -1;
        memcpy(x->data, data, dlen);
        x->dlen = dlen;
    }
    (*n)++;
    return 0;
}

static void rational_le(double v, uint8_t out[8])
{
    unsigned num, den;
    if (v > 0 && v < 4294967295.0 && v == (double)(unsigned)v) {
        num = (unsigned)v;
        den = 1;
    } else if (v > 0 && v * 1000.0 < 4294967295.0) {
        num = (unsigned)(v * 1000.0 + 0.5);
        den = 1000;
    } else {
        num = 0;
        den = 1;
    }
    out[0] = (uint8_t)num;        out[1] = (uint8_t)(num >> 8);
    out[2] = (uint8_t)(num >> 16); out[3] = (uint8_t)(num >> 24);
    out[4] = (uint8_t)den;        out[5] = (uint8_t)(den >> 8);
    out[6] = (uint8_t)(den >> 16); out[7] = (uint8_t)(den >> 24);
}

/* Serialise the collected entries as a little-endian TIFF/EXIF payload. */
static uint8_t *exif_build(const eb_ent_t *e, int n, size_t *out_len)
{
    eb_t b;
    memset(&b, 0, sizeof(b));
    if (eb_put(&b, "II", 2) || eb_u16(&b, 42) || eb_u32(&b, 8) ||
        eb_u16(&b, (unsigned)n))
        goto fail;

    size_t run = 8 + 2 + (size_t)n * 12 + 4;    /* where out-of-line data goes */
    for (int i = 0; i < n; i++) {
        if (eb_u16(&b, e[i].tag) || eb_u16(&b, e[i].type) || eb_u32(&b, e[i].count))
            goto fail;
        if (e[i].dlen > 4) {
            if (eb_u32(&b, (unsigned)run))
                goto fail;
            run += e[i].dlen + (e[i].dlen & 1);
        } else {
            uint8_t v[4] = { 0, 0, 0, 0 };
            memcpy(v, e[i].data, e[i].dlen);
            if (eb_put(&b, v, 4))
                goto fail;
        }
    }
    if (eb_u32(&b, 0))                          /* no next IFD */
        goto fail;
    for (int i = 0; i < n; i++) {
        if (e[i].dlen > 4) {
            if (eb_put(&b, e[i].data, e[i].dlen))
                goto fail;
            if (e[i].dlen & 1) {                    /* keep offsets even */
                uint8_t z = 0;
                if (eb_put(&b, &z, 1))
                    goto fail;
            }
        }
    }
    *out_len = b.len;
    return b.buf;
fail:
    free(b.buf);
    return NULL;
}

static void tiff_meta(TIFF *tif, img_meta_t *m)
{
    float xr = 0, yr = 0;
    uint16_t ru = RESUNIT_INCH;
    int have_x = TIFFGetField(tif, TIFFTAG_XRESOLUTION, &xr) == 1;
    int have_y = TIFFGetField(tif, TIFFTAG_YRESOLUTION, &yr) == 1;
    TIFFGetField(tif, TIFFTAG_RESOLUTIONUNIT, &ru);
    if (have_x && have_y && xr > 0 && yr > 0) {
        double scale = (ru == RESUNIT_CENTIMETER) ? 2.54 : 1.0;
        m->have_dpi = 1;
        m->dpi_x = xr * scale;
        m->dpi_y = yr * scale;
    }

    /* ICC and XMP are ordinary TIFF tags */
    uint32_t taglen = 0;
    void *tagdata = NULL;
    if (TIFFGetField(tif, TIFFTAG_ICCPROFILE, &taglen, &tagdata) == 1 &&
        taglen && tagdata)
        img_meta_set_blob(&m->icc, &m->icc_len, tagdata, taglen);
    if (TIFFGetField(tif, TIFFTAG_XMLPACKET, &taglen, &tagdata) == 1 &&
        taglen && tagdata)
        img_meta_set_blob((uint8_t **)&m->xmp, &m->xmp_len, tagdata, taglen);

    static const struct { unsigned exif_tag, tiff_tag; } ascii[] = {
        { 270,   TIFFTAG_IMAGEDESCRIPTION },
        { 271,   TIFFTAG_MAKE },
        { 272,   TIFFTAG_MODEL },
        { 305,   TIFFTAG_SOFTWARE },
        { 306,   TIFFTAG_DATETIME },
        { 315,   TIFFTAG_ARTIST },
        { 33432, TIFFTAG_COPYRIGHT },
    };

    eb_ent_t ent[EXIF_MAX_ENT];
    int n = 0;
    memset(ent, 0, sizeof(ent));

    for (size_t i = 0; i < sizeof(ascii) / sizeof(ascii[0]); i++) {
        char *s = NULL;
        if (TIFFGetField(tif, ascii[i].tiff_tag, &s) == 1 && s && *s)
            ent_add(ent, &n, ascii[i].exif_tag, 2, (unsigned)strlen(s) + 1,
                    s, strlen(s) + 1);
    }
    {
        uint16_t o = 0;
        if (TIFFGetField(tif, TIFFTAG_ORIENTATION, &o) == 1 && o) {
            uint8_t v[2] = { (uint8_t)o, (uint8_t)(o >> 8) };
            ent_add(ent, &n, 274, 3, 1, v, 2);
        }
    }
    if (have_x && have_y && xr > 0 && yr > 0) {
        uint8_t v[2] = { (uint8_t)ru, (uint8_t)(ru >> 8) };
        uint8_t rx[8], ry[8];
        rational_le(xr, rx);
        rational_le(yr, ry);
        ent_add(ent, &n, 296, 3, 1, v, 2);
        ent_add(ent, &n, 282, 5, 1, rx, 8);
        ent_add(ent, &n, 283, 5, 1, ry, 8);
    }

    if (n > 0) {
        size_t elen = 0;
        uint8_t *blob = exif_build(ent, n, &elen);
        if (blob && elen) {
            m->exif = blob;
            m->exif_len = elen;
        } else {
            free(blob);
        }
    }
    for (int i = 0; i < n; i++)
        free(ent[i].data);
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
    tiff_meta(tif, &img->meta);

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

    /* keep the metadata across the reset below */
    img_meta_t keep = img->meta;
    memset(&img->meta, 0, sizeof(img->meta));
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
    img->meta = keep;
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
