#include "decode.h"
#include <stdlib.h>
#include <string.h>
#include <libheif/heif.h>

static int fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen)
        snprintf(err, errlen, "%s", msg);
    return -1;
}

int heif_decode(FILE *f, img_image_t *img, char *err, size_t errlen)
{
    if (fseek(f, 0, SEEK_END) != 0)
        return fail(err, errlen, "heif: seek failed");
    long fsize = ftell(f);
    rewind(f);
    if (fsize < 12)
        return fail(err, errlen, "heif: file too small");
    uint8_t *buf = (uint8_t *)malloc((size_t)fsize);
    if (!buf)
        return fail(err, errlen, "heif: out of memory");
    if (fread(buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        free(buf);
        return fail(err, errlen, "heif: read failed");
    }

    struct heif_error derr;
    struct heif_context *ctx = heif_context_alloc();
    if (!ctx) {
        free(buf);
        return fail(err, errlen, "heif: out of memory");
    }
    derr = heif_context_read_from_memory(ctx, buf, (size_t)fsize, NULL);
    free(buf);
    if (derr.code != heif_error_Ok) {
        if (err && errlen)
            snprintf(err, errlen, "heif: %s", derr.message);
        heif_context_free(ctx);
        return -1;
    }

    struct heif_image_handle *handle = NULL;
    derr = heif_context_get_primary_image_handle(ctx, &handle);
    if (derr.code != heif_error_Ok) {
        if (err && errlen)
            snprintf(err, errlen, "heif: %s", derr.message);
        heif_context_free(ctx);
        return -1;
    }

    int luma_bits = heif_image_handle_get_luma_bits_per_pixel(handle);
    int w = heif_image_handle_get_width(handle);
    int h = heif_image_handle_get_height(handle);

    /* bit-depth matching: 8-bit sources -> 8-bit RGBA,
     * 10/12-bit sources -> 16-bit RGBA (RRGGBBAA_LE, converted to BE) */
    struct heif_image *image = NULL;
    if (luma_bits > 8)
        derr = heif_decode_image(handle, &image, heif_colorspace_RGB,
                                 heif_chroma_interleaved_RRGGBBAA_LE, NULL);
    else
        derr = heif_decode_image(handle, &image, heif_colorspace_RGB,
                                 heif_chroma_interleaved_RGBA, NULL);
    if (derr.code != heif_error_Ok) {
        if (err && errlen)
            snprintf(err, errlen, "heif: %s", derr.message);
        heif_image_handle_release(handle);
        heif_context_free(ctx);
        return -1;
    }

    int stride = 0;
    const uint8_t *plane = heif_image_get_plane_readonly(
        image, heif_channel_interleaved, &stride);
    int out_w = heif_image_get_width(image, heif_channel_interleaved);
    int out_h = heif_image_get_height(image, heif_channel_interleaved);
    if (!plane || out_w <= 0 || out_h <= 0) {
        heif_image_release(image);
        heif_image_handle_release(handle);
        heif_context_free(ctx);
        return fail(err, errlen, "heif: no image plane");
    }

    memset(img, 0, sizeof(*img));
    img->width = out_w;
    img->height = out_h;
    img->color = IMG_RGBA;
    if (luma_bits > 8) {
        /* 16-bit RRGGBBAA_LE (host order) -> our big-endian 16-bit layout */
        img->bit_depth = 16;
        img->rowstride = img_rowstride(out_w, 16, 4);
        img->data = (uint8_t *)malloc((size_t)out_h * img->rowstride);
        if (!img->data) { /* cleanup below */ }
        else {
            for (int y = 0; y < out_h; y++) {
                const uint16_t *srow = (const uint16_t *)(plane +
                    (size_t)y * stride);
                uint8_t *drow = img->data + (size_t)y * img->rowstride;
                for (int x = 0; x < out_w; x++) {
                    img_st16be(drow + (size_t)x * 8 + 0, srow[x * 4 + 0]);
                    img_st16be(drow + (size_t)x * 8 + 2, srow[x * 4 + 1]);
                    img_st16be(drow + (size_t)x * 8 + 4, srow[x * 4 + 2]);
                    img_st16be(drow + (size_t)x * 8 + 6, srow[x * 4 + 3]);
                }
            }
        }
    } else {
        img->bit_depth = 8;
        img->rowstride = img_rowstride(out_w, 8, 4);
        img->data = (uint8_t *)malloc((size_t)out_h * img->rowstride);
        if (img->data) {
            for (int y = 0; y < out_h; y++)
                memcpy(img->data + (size_t)y * img->rowstride,
                       plane + (size_t)y * stride, (size_t)out_w * 4);
        }
    }

    heif_image_release(image);
    heif_image_handle_release(handle);
    heif_context_free(ctx);
    if (!img->data) {
        img_free(img);
        return fail(err, errlen, "heif: out of memory");
    }
    return 0;
}
