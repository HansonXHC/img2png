#include "apngenc.h"
#include "../util/pngerr.h"
#include <png.h>
#include <stdlib.h>
#include <string.h>

int png_write_apng(const img_animation_t *anim, const png_opts_t *opts,
                   const char *path, char *err, size_t errlen)
{
    if (anim->nframes < 1 || !anim->frames) {
        if (err && errlen)
            snprintf(err, errlen, "apng: no frames");
        return -1;
    }

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
            snprintf(err, errlen, "apng: %s", pe.msg[0] ? pe.msg : "encode failed");
        return -1;
    }

    png_init_io(png, f);
    png_set_compression_level(png, opts->level);
    if (opts->filter != PNGF_AUTO)
        png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_ALL_FILTERS);

    /* animated output is always 8-bit RGBA (documented bit-depth exception) */
    png_set_IHDR(png, info, (png_uint_32)anim->width, (png_uint_32)anim->height,
                 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

    /* num_plays: GIF NETSCAPE 0 = infinite (matches APNG 0),
     * absent = play once (APNG 1) */
    png_set_acTL(png, info, (png_uint_32)anim->nframes,
                 (png_uint_32)(anim->loops < 0 ? 1 : anim->loops));

    png_write_info(png, info);

    png_bytep *rows = (png_bytep *)malloc(sizeof(png_bytep) *
                                          (size_t)(anim->height > 0 ? anim->height : 1));
    if (!rows) {
        png_destroy_write_struct(&png, &info);
        fclose(f);
        remove(path);
        if (err && errlen)
            snprintf(err, errlen, "out of memory");
        return -1;
    }

    for (int i = 0; i < anim->nframes; i++) {
        const img_image_t *fr = &anim->frames[i];
        if (fr->color != IMG_RGBA || fr->bit_depth != 8) {
            free(rows);
            png_destroy_write_struct(&png, &info);
            fclose(f);
            remove(path);
            if (err && errlen)
                snprintf(err, errlen, "apng: frame %d has unexpected format", i);
            return -1;
        }
        for (int y = 0; y < fr->height; y++)
            rows[y] = fr->data + (size_t)y * fr->rowstride;

        /* GIF delay is in centiseconds; APNG stores it as a fraction */
        png_uint_16 dnum = (png_uint_16)(anim->delays_cs[i] & 0xFFFF);
        png_uint_16 dden = 100;
        png_byte dispose = (anim->dispose[i] == 2) ? PNG_DISPOSE_OP_BACKGROUND
                          : (anim->dispose[i] == 3) ? PNG_DISPOSE_OP_PREVIOUS
                          : PNG_DISPOSE_OP_NONE;
        png_write_frame_head(png, info, rows,
                             (png_uint_32)fr->width, (png_uint_32)fr->height,
                             (png_uint_32)anim->x[i], (png_uint_32)anim->y[i],
                             dnum, dden, dispose, PNG_BLEND_OP_SOURCE);
        png_write_image(png, rows);
        png_write_frame_tail(png, info);
    }
    free(rows);

    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);

    if (fclose(f) != 0) {
        if (err && errlen)
            snprintf(err, errlen, "write failed (disk full?)");
        return -1;
    }
    return 0;
}
