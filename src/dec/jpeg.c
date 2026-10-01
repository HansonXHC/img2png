#include "decode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>
#include <setjmp.h>

typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
    char msg[JMSG_LENGTH_MAX + 1];
} jpg_err_t;

static void jpg_error_exit(j_common_ptr cinfo)
{
    jpg_err_t *e = (jpg_err_t *)cinfo->err;
    (*cinfo->err->format_message)(cinfo, e->msg);
    longjmp(e->jb, 1);
}

static void jpg_emit_message(j_common_ptr cinfo, int msg_level)
{
    (void)msg_level;    /* warnings and trace messages are ignored */
}

int jpeg_decode(FILE *f, img_image_t *img, char *err, size_t errlen)
{
    struct jpeg_decompress_struct cinfo;
    jpg_err_t jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpg_error_exit;
    jerr.pub.emit_message = jpg_emit_message;

    if (setjmp(jerr.jb)) {
        jpeg_destroy_decompress(&cinfo);
        if (err && errlen)
            snprintf(err, errlen, "jpeg: %s", jerr.msg);
        return -1;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, f);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        if (err && errlen)
            snprintf(err, errlen, "jpeg: not a JPEG file");
        return -1;
    }

    /* normalize: grayscale stays gray, everything else (YCbCr, CMYK, ...)
     * comes out as RGB */
    int gray = (cinfo.jpeg_color_space == JCS_GRAYSCALE);
    cinfo.out_color_space = gray ? JCS_GRAYSCALE : JCS_RGB;

    jpeg_start_decompress(&cinfo);

    memset(img, 0, sizeof(*img));
    img->width = (int)cinfo.output_width;
    img->height = (int)cinfo.output_height;
    img->color = gray ? IMG_GRAY : IMG_RGB;
    img->bit_depth = 8;     /* baseline JPEG samples are 8-bit */
    int ch = gray ? 1 : 3;
    img->rowstride = img_rowstride(img->width, 8, ch);
    img->data = (uint8_t *)malloc((size_t)img->height * img->rowstride);
    if (!img->data) {
        jpeg_destroy_decompress(&cinfo);
        if (err && errlen)
            snprintf(err, errlen, "jpeg: out of memory");
        return -1;
    }

    while (cinfo.output_scanline < cinfo.output_height) {
        uint8_t *row = img->data + (size_t)cinfo.output_scanline * img->rowstride;
        JSAMPROW rp = row;
        if (jpeg_read_scanlines(&cinfo, &rp, 1) != 1) {
            jpeg_destroy_decompress(&cinfo);
            img_free(img);
            if (err && errlen)
                snprintf(err, errlen, "jpeg: decode failed");
            return -1;
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return 0;
}
