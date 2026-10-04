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

int tiff_decode(FILE *f, img_image_t *img, char *err, size_t errlen)
{
    TIFF *tif = TIFFClientOpen("memory", "r", (thandle_t)f,
                               tiff_read_proc, tiff_write_proc,
                               tiff_seek_proc, tiff_close_proc,
                               tiff_size_proc, tiff_map_proc, tiff_unmap_proc);
    if (!tif)
        return fail(err, errlen, "tiff: not a readable TIFF");

    uint32_t w = 0, h = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    if (w == 0 || h == 0) {
        TIFFClose(tif);
        return fail(err, errlen, "tiff: bad dimensions");
    }

    /* the RGBA interface handles photometric variants, strips/tiles and
     * bit depths, producing 8-bit RGBA (A=255 when opaque) */
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
