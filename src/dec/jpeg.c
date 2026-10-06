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

/* Values in a JPEG COM marker are arbitrary bytes, but a PNG tEXt chunk only
 * takes Latin-1; keep comments that are plainly safe ASCII. */
static int comment_is_text(const uint8_t *p, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (p[i] == '\t' || p[i] == '\n' || p[i] == '\r')
            continue;
        if (p[i] < 32 || p[i] > 126)
            return 0;
    }
    return 1;
}

/* Collect EXIF / XMP / ICC / comments / density from the APPn markers that
 * jpeg_save_markers() kept.  libjpeg drops every APPn marker unless they are
 * explicitly saved, which is why this all used to be lost. */
static void jpeg_meta(struct jpeg_decompress_struct *cinfo, img_meta_t *m)
{
    static const char xmp_id[] = "http://ns.adobe.com/xap/1.0/";
    const uint8_t *icc_part[256];
    size_t icc_len[256];
    unsigned icc_total = 0, icc_seen = 0;
    memset(icc_part, 0, sizeof(icc_part));
    memset(icc_len, 0, sizeof(icc_len));

    for (jpeg_saved_marker_ptr mk = cinfo->marker_list; mk; mk = mk->next) {
        const uint8_t *p = (const uint8_t *)mk->data;
        size_t len = mk->data_length;
        if (!p || !len)
            continue;

        if (mk->marker == JPEG_APP0 && len >= 14 && !memcmp(p, "JFIF\0", 5)) {
            unsigned unit = p[7];
            unsigned dx = ((unsigned)p[8] << 8) | p[9];
            unsigned dy = ((unsigned)p[10] << 8) | p[11];
            if (dx && dy && unit >= 1 && unit <= 2) {   /* 1 = dpi, 2 = dots/cm */
                m->have_dpi = 1;
                m->dpi_x = (unit == 2) ? dx * 2.54 : (double)dx;
                m->dpi_y = (unit == 2) ? dy * 2.54 : (double)dy;
            }
        } else if (mk->marker == JPEG_APP0 + 1 && len > 6 &&
                   !memcmp(p, "Exif\0\0", 6)) {
            if (!m->exif)
                img_meta_set_exif(m, p + 6, len - 6);
        } else if (mk->marker == JPEG_APP0 + 1 &&
                   len > sizeof(xmp_id) &&
                   !memcmp(p, xmp_id, sizeof(xmp_id))) {
            if (!m->xmp)
                img_meta_set_blob((uint8_t **)&m->xmp, &m->xmp_len,
                                  p + sizeof(xmp_id), len - sizeof(xmp_id));
        } else if (mk->marker == JPEG_APP0 + 2 && len > 14 &&
                   !memcmp(p, "ICC_PROFILE\0", 12)) {
            unsigned seq = p[12], total = p[13];
            if (total >= 1 && seq >= 1 && seq <= total) {
                if (icc_len[seq - 1] == 0) {        /* first occurrence wins */
                    icc_part[seq - 1] = p + 14;
                    icc_len[seq - 1] = len - 14;
                    icc_seen++;
                }
                if (total > icc_total)
                    icc_total = total;
            }
        } else if (mk->marker == JPEG_COM && !m->text &&
                   comment_is_text(p, len)) {
            char *tmp = (char *)malloc(len + 1);
            if (tmp) {
                memcpy(tmp, p, len);
                tmp[len] = 0;
                img_meta_add_text(m, "Comment", tmp, 0);
                free(tmp);
            }
        }
    }

    /* reassemble an ICC profile split over several APP2 segments */
    if (icc_total > 0 && icc_seen == icc_total) {
        size_t total = 0;
        for (unsigned i = 0; i < icc_total; i++)
            total += icc_len[i];
        if (total >= 128) {                 /* ICC header is 128 bytes */
            uint8_t *buf = (uint8_t *)malloc(total);
            if (buf) {
                size_t off = 0;
                for (unsigned i = 0; i < icc_total; i++) {
                    memcpy(buf + off, icc_part[i], icc_len[i]);
                    off += icc_len[i];
                }
                m->icc = buf;
                m->icc_len = total;
            }
        }
    }

    /* no JFIF header: the resolution may live in EXIF instead */
    if (!m->have_dpi && m->exif) {
        double x, y;
        int unit;
        if (img_exif_resolution(m->exif, m->exif_len, &x, &y, &unit)) {
            m->have_dpi = 1;
            m->dpi_x = x;
            m->dpi_y = y;
        }
    }
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
    /* keep APP0/APP1/APP2 and comments: libjpeg discards them by default */
    jpeg_save_markers(&cinfo, JPEG_APP0 + 0, 0xFFFF);
    jpeg_save_markers(&cinfo, JPEG_APP0 + 1, 0xFFFF);
    jpeg_save_markers(&cinfo, JPEG_APP0 + 2, 0xFFFF);
    jpeg_save_markers(&cinfo, JPEG_COM, 0xFFFF);
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
    jpeg_meta(&cinfo, &img->meta);
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
