#include "pngenc.h"
#include "../util/pngerr.h"
#include <png.h>
#include <stdlib.h>
#include <string.h>

const char *png_filter_name(png_filter_mode_t f)
{
    switch (f) {
    case PNGF_AUTO:  return "auto";
    case PNGF_NONE:  return "none";
    case PNGF_SUB:   return "sub";
    case PNGF_UP:    return "up";
    case PNGF_AVG:   return "avg";
    case PNGF_PAETH: return "paeth";
    case PNGF_ALL:   return "all";
    case PNGF_FAST:  return "fast";
    }
    return "?";
}

static int map_color(img_color_t c)
{
    switch (c) {
    case IMG_GRAY:       return PNG_COLOR_TYPE_GRAY;
    case IMG_GRAY_ALPHA: return PNG_COLOR_TYPE_GRAY_ALPHA;
    case IMG_PALETTE:    return PNG_COLOR_TYPE_PALETTE;
    case IMG_RGB:        return PNG_COLOR_TYPE_RGB;
    case IMG_RGBA:       return PNG_COLOR_TYPE_RGBA;
    }
    return -1;
}

static int map_filter(png_filter_mode_t f)
{
    switch (f) {
    case PNGF_NONE:  return PNG_FILTER_NONE;
    case PNGF_SUB:   return PNG_FILTER_SUB;
    case PNGF_UP:    return PNG_FILTER_UP;
    case PNGF_AVG:   return PNG_FILTER_AVG;
    case PNGF_PAETH: return PNG_FILTER_PAETH;
    case PNGF_ALL:   return PNG_ALL_FILTERS;
    case PNGF_FAST:  return PNG_FAST_FILTERS;
    default:         return PNG_NO_FILTERS;
    }
}

/* png_set_text() aborts the write on a malformed keyword, so pre-screen the
 * ones that come from the source file.  PNG keywords are printable Latin-1
 * (32-126 and 161-255), 1-79 bytes, no leading/trailing/doubled spaces. */
static int text_key_ok(const char *k)
{
    size_t n = strlen(k);
    if (n < 1 || n > 79)
        return 0;
    if (k[0] == ' ' || k[n - 1] == ' ')
        return 0;
    for (size_t i = 0; i + 1 < n; i++)
        if (k[i] == ' ' && k[i + 1] == ' ')
            return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)k[i];
        if (c < 32 || c == 127 || (c > 126 && c < 161))
            return 0;
    }
    return 1;
}

/* Write the metadata collected by the decoder: pHYs, eXIf, iCCP, XMP and any
 * text chunks carried over from a PNG source.  All of these must be set before
 * png_write_info(). */
static void write_metadata(png_structp png, png_infop info,
                           const img_image_t *img)
{
    const img_meta_t *m = &img->meta;

    if (m->have_dpi && m->dpi_x > 0 && m->dpi_y > 0) {
        /* PNG stores integer pixels per metre */
        png_uint_32 rx = (png_uint_32)(m->dpi_x * 39.3700787401575 + 0.5);
        png_uint_32 ry = (png_uint_32)(m->dpi_y * 39.3700787401575 + 0.5);
        if (rx && ry)
            png_set_pHYs(png, info, rx, ry, PNG_RESOLUTION_METER);
    }

    if (m->exif && m->exif_len >= 8 &&
        ((m->exif[0] == 'I' && m->exif[1] == 'I') ||
         (m->exif[0] == 'M' && m->exif[1] == 'M')))
        png_set_eXIf_1(png, info, (png_uint_32)m->exif_len,
                       (png_bytep)m->exif);

    if (m->icc && m->icc_len)
        png_set_iCCP(png, info, "ICC profile", PNG_COMPRESSION_TYPE_BASE,
                     (png_const_bytep)m->icc, (png_uint_32)m->icc_len);

    int slots = m->ntext + (m->xmp ? 1 : 0);
    if (slots <= 0)
        return;

    png_text *txt = (png_text *)calloc((size_t)slots, sizeof(*txt));
    if (!txt)
        return;
    int n = 0;
    if (m->xmp) {
        txt[n].compression = PNG_ITXT_COMPRESSION_NONE;
        txt[n].key = (png_charp)"XML:com.adobe.xmp";
        txt[n].text = (png_charp)m->xmp;
        txt[n].itxt_length = m->xmp_len;
        txt[n].lang = (png_charp)"";
        txt[n].lang_key = (png_charp)"";
        n++;
    }
    for (int i = 0; i < m->ntext; i++) {
        const img_text_t *t = &m->text[i];
        if (!t->key || !t->value || !text_key_ok(t->key))
            continue;
        int itxt = (t->kind == IMG_TEXT_ITXT || t->kind == IMG_TEXT_ITXT_Z);
        int zlib = (t->kind == IMG_TEXT_ZTXT || t->kind == IMG_TEXT_ITXT_Z);
        if (itxt) {
            txt[n].compression = zlib ? PNG_ITXT_COMPRESSION_zTXt
                                      : PNG_ITXT_COMPRESSION_NONE;
            txt[n].itxt_length = strlen(t->value);
            txt[n].lang = (png_charp)"";
            txt[n].lang_key = (png_charp)"";
        } else {
            txt[n].compression = zlib ? PNG_TEXT_COMPRESSION_zTXt
                                      : PNG_TEXT_COMPRESSION_NONE;
            txt[n].text_length = strlen(t->value);
        }
        txt[n].key = (png_charp)t->key;
        txt[n].text = (png_charp)t->value;
        n++;
    }
    if (n > 0)
        png_set_text(png, info, txt, n);   /* libpng copies strings */
    free(txt);
}

int png_write_file(const img_image_t *img, const png_opts_t *opts,
                   const char *path, char *err, size_t errlen)
{
    FILE *f = img_fopen_write(path);
    if (!f) {
        if (err && errlen)
            snprintf(err, errlen, "cannot open output file '%s'", path);
        return -1;
    }

    png_err_t pe;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, &pe,
                                              pngerr_error, pngerr_warn);
    if (!png) {
        fclose(f);
        if (err && errlen)
            snprintf(err, errlen, "png_create_write_struct failed");
        return -1;
    }
    png_infop info = png_create_info_struct(png);
    if (!info || setjmp(pe.jb)) {
        png_destroy_write_struct(&png, info ? &info : NULL);
        fclose(f);
        if (err && errlen)
            snprintf(err, errlen, "png: %s", pe.msg[0] ? pe.msg : "encode failed");
        return -1;
    }

    png_init_io(png, f);

    int color_type = map_color(img->color);
    if (color_type < 0 || (img->bit_depth != 1 && img->bit_depth != 2 &&
                           img->bit_depth != 4 && img->bit_depth != 8 &&
                           img->bit_depth != 16)) {
        png_destroy_write_struct(&png, &info);
        fclose(f);
        if (err && errlen)
            snprintf(err, errlen, "png: unsupported color type/depth");
        return -1;
    }

    /* compression level 0-9 (default 9 set by the caller) */
    png_set_compression_level(png, opts->level);
    if (opts->filter != PNGF_AUTO)
        png_set_filter(png, PNG_FILTER_TYPE_BASE, map_filter(opts->filter));

    png_set_IHDR(png, info, (png_uint_32)img->width, (png_uint_32)img->height,
                 img->bit_depth, color_type, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

    if (img->color == IMG_PALETTE) {
        png_color plte[256];
        for (int i = 0; i < img->pal_ncolors; i++) {
            plte[i].red   = img->palette[i * 3 + 0];
            plte[i].green = img->palette[i * 3 + 1];
            plte[i].blue  = img->palette[i * 3 + 2];
        }
        png_set_PLTE(png, info, plte, img->pal_ncolors);
        if (img->has_pal_alpha) {
            png_byte alpha[256];
            for (int i = 0; i < img->pal_ncolors; i++)
                alpha[i] = img->pal_alpha[i];
            png_set_tRNS(png, info, alpha, img->pal_ncolors, NULL);
        }
    } else if (img->color == IMG_GRAY && img->has_gray_trns) {
        png_color_16 v;
        memset(&v, 0, sizeof(v));
        v.gray = img->gray_trns_value;
        png_set_tRNS(png, info, NULL, 0, &v);
    }

    write_metadata(png, info, img);

    png_write_info(png, info);

    png_bytep *rows = (png_bytep *)malloc(sizeof(png_bytep) * (size_t)img->height);
    if (!rows) {
        png_destroy_write_struct(&png, &info);
        fclose(f);
        remove(path);
        if (err && errlen)
            snprintf(err, errlen, "out of memory");
        return -1;
    }
    for (int y = 0; y < img->height; y++)
        rows[y] = img->data + (size_t)y * img->rowstride;
    png_write_image(png, rows);
    png_write_end(png, NULL);
    free(rows);
    png_destroy_write_struct(&png, &info);

    if (fclose(f) != 0) {
        if (err && errlen)
            snprintf(err, errlen, "write failed (disk full?)");
        return -1;
    }
    return 0;
}
