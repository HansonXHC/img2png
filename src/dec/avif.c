#include "decode.h"
#include <stdlib.h>
#include <string.h>
#include <avif/avif.h>

static int fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen)
        snprintf(err, errlen, "%s", msg);
    return -1;
}

int avif_decode(FILE *f, img_image_t *img, char *err, size_t errlen)
{
    if (fseek(f, 0, SEEK_END) != 0)
        return fail(err, errlen, "avif: seek failed");
    long fsize = ftell(f);
    rewind(f);
    if (fsize < 12)
        return fail(err, errlen, "avif: file too small");
    uint8_t *buf = (uint8_t *)malloc((size_t)fsize);
    if (!buf)
        return fail(err, errlen, "avif: out of memory");
    if (fread(buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        free(buf);
        return fail(err, errlen, "avif: read failed");
    }

    avifDecoder *dec = avifDecoderCreate();
    if (!dec) {
        free(buf);
        return fail(err, errlen, "avif: out of memory");
    }
    avifImage *image = avifImageCreateEmpty();
    avifResult res = avifDecoderReadMemory(dec, image, buf, (size_t)fsize);
    if (res != AVIF_RESULT_OK) {
        if (err && errlen)
            snprintf(err, errlen, "avif: %s", avifResultToString(res));
        avifImageDestroy(image);
        avifDecoderDestroy(dec);
        free(buf);
        return -1;
    }

    /* bit-depth matching: 8-bit sources -> 8-bit RGBA,
     * 10/12-bit sources -> 16-bit RGBA */
    int depth = image->depth;
    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, image);
    rgb.format = AVIF_RGB_FORMAT_RGBA;
    rgb.depth = (depth > 8) ? 16 : 8;
    rgb.chromaUpsampling = AVIF_CHROMA_UPSAMPLING_AUTOMATIC;
    res = avifRGBImageAllocatePixels(&rgb);
    if (res != AVIF_RESULT_OK) {
        if (err && errlen)
            snprintf(err, errlen, "avif: out of memory");
        avifImageDestroy(image);
        avifDecoderDestroy(dec);
        free(buf);
        return -1;
    }
    res = avifImageYUVToRGB(image, &rgb);
    if (res != AVIF_RESULT_OK) {
        if (err && errlen)
            snprintf(err, errlen, "avif: %s", avifResultToString(res));
        avifImageDestroy(image);
        avifDecoderDestroy(dec);
        free(buf);
        return -1;
    }

    memset(img, 0, sizeof(*img));
    img->width = (int)image->width;
    img->height = (int)image->height;
    img->color = IMG_RGBA;
    img->bit_depth = rgb.depth;
    int src_sample = (rgb.depth > 8) ? 2 : 1;
    img->rowstride = img_rowstride(img->width, rgb.depth, 4);
    size_t bytes = (size_t)img->height * img->rowstride;
    img->data = (uint8_t *)malloc(bytes ? bytes : 1);
    if (!img->data) {
        avifImageDestroy(image);
        avifDecoderDestroy(dec);
        free(buf);
        return fail(err, errlen, "avif: out of memory");
    }
    if (rgb.depth > 8) {
        /* 16-bit host-order samples -> our big-endian 16-bit layout */
        for (int y = 0; y < img->height; y++) {
            const uint8_t *srow = rgb.pixels + (size_t)y * rgb.rowBytes;
            uint8_t *drow = img->data + (size_t)y * img->rowstride;
            for (int x = 0; x < img->width; x++) {
                const uint16_t *s = (const uint16_t *)(srow +
                    (size_t)x * 8);
                img_st16be(drow + (size_t)x * 8 + 0, s[0]);
                img_st16be(drow + (size_t)x * 8 + 2, s[1]);
                img_st16be(drow + (size_t)x * 8 + 4, s[2]);
                img_st16be(drow + (size_t)x * 8 + 6, s[3]);
            }
        }
    } else {
        for (int y = 0; y < img->height; y++)
            memcpy(img->data + (size_t)y * img->rowstride,
                   rgb.pixels + (size_t)y * rgb.rowBytes,
                   (size_t)img->width * 4);
    }

    avifRGBImageFreePixels(&rgb);
    avifImageDestroy(image);
    avifDecoderDestroy(dec);
    free(buf);
    return 0;
}
