#include "decode.h"
#include "../util/pngerr.h"
#include <png.h>
#include <stdlib.h>
#include <string.h>

static int fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen)
        snprintf(err, errlen, "%s", msg);
    return -1;
}

/* Read a PNG while preserving its native color type and bit depth so that
 * ICO entries (and general round-trips) keep their original structure. */

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
} mem_reader_t;

static void mem_read_fn(png_structp png, png_bytep out, png_size_t count)
{
    mem_reader_t *r = (mem_reader_t *)png_get_io_ptr(png);
    if (r->pos + count > r->size)
        png_error(png, "truncated PNG data");
    memcpy(out, r->data + r->pos, count);
    r->pos += count;
}

int png_decode_mem(const uint8_t *data, size_t size, img_image_t *img,
                   char *err, size_t errlen)
{
    png_err_t pe;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &pe,
                                             pngerr_error, pngerr_warn);
    if (!png)
        return fail(err, errlen, "png_create_read_struct failed");
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, NULL, NULL);
        return fail(err, errlen, "out of memory");
    }
    if (setjmp(pe.jb)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "png: %s", pe.msg[0] ? pe.msg : "decode failed");
        png_destroy_read_struct(&png, &info, NULL);
        if (err && errlen)
            snprintf(err, errlen, "%s", msg);
        return -1;
    }

    mem_reader_t reader = { data, size, 0 };
    png_set_read_fn(png, &reader, mem_read_fn);
    png_read_info(png, info);

    png_uint_32 w = png_get_image_width(png, info);
    png_uint_32 h = png_get_image_height(png, info);
    png_byte color = png_get_color_type(png, info);
    png_byte depth = png_get_bit_depth(png, info);

    memset(img, 0, sizeof(*img));
    img->width = (int)w;
    img->height = (int)h;

    int expand_rgb_trns = 0;
    switch (color) {
    case PNG_COLOR_TYPE_PALETTE:
        img->color = IMG_PALETTE;
        img->bit_depth = depth;
        break;
    case PNG_COLOR_TYPE_GRAY:
        img->color = IMG_GRAY;
        img->bit_depth = depth;
        break;
    case PNG_COLOR_TYPE_GRAY_ALPHA:
        img->color = IMG_GRAY_ALPHA;
        img->bit_depth = depth;
        break;
    case PNG_COLOR_TYPE_RGB:
        img->color = IMG_RGB;
        img->bit_depth = depth;
        expand_rgb_trns = png_get_valid(png, info, PNG_INFO_tRNS) != 0;
        break;
    case PNG_COLOR_TYPE_RGB_ALPHA:
        img->color = IMG_RGBA;
        img->bit_depth = depth;
        break;
    default:
        png_destroy_read_struct(&png, &info, NULL);
        return fail(err, errlen, "unsupported PNG color type");
    }

    if (expand_rgb_trns) {
        /* RGB + tRNS: preserve transparency by expanding to RGBA */
        png_set_expand(png);
        img->color = IMG_RGBA;
    }

    png_read_update_info(png, info);
    img->rowstride = png_get_rowbytes(png, info);

    if (img->color == IMG_PALETTE) {
        png_colorp plte = NULL;
        int n = 0;
        png_get_PLTE(png, info, &plte, &n);
        img->pal_ncolors = n;
        for (int i = 0; i < n && i < 256; i++) {
            img->palette[i * 3 + 0] = plte[i].red;
            img->palette[i * 3 + 1] = plte[i].green;
            img->palette[i * 3 + 2] = plte[i].blue;
        }
        png_bytep trans = NULL;
        int ntrans = 0;
        if (png_get_tRNS(png, info, &trans, &ntrans, NULL) && ntrans > 0) {
            img->has_pal_alpha = 1;
            for (int i = 0; i < 256; i++)
                img->pal_alpha[i] = (i < ntrans) ? trans[i] : 255;
        } else {
            for (int i = 0; i < 256; i++)
                img->pal_alpha[i] = 255;
        }
    } else if (img->color == IMG_GRAY) {
        png_color_16p trans_color = NULL;
        int ntrans = 0;
        if (png_get_tRNS(png, info, NULL, &ntrans, &trans_color) &&
            ntrans > 0 && trans_color != NULL) {
            img->has_gray_trns = 1;
            img->gray_trns_value = trans_color->gray;
        }
    }

    img->data = (uint8_t *)malloc(img->rowstride * h);
    png_bytep *rows = (png_bytep *)malloc(sizeof(png_bytep) * h);
    if (!img->data || !rows) {
        free(rows);
        png_destroy_read_struct(&png, &info, NULL);
        return fail(err, errlen, "out of memory");
    }
    for (png_uint_32 y = 0; y < h; y++)
        rows[y] = img->data + (size_t)y * img->rowstride;

    png_read_image(png, rows);
    png_read_end(png, NULL);
    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    return 0;
}
