#define QOI_IMPLEMENTATION
#include <qoi.h>

#include "decode.h"
#include <stdlib.h>
#include <string.h>

static int fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen)
        snprintf(err, errlen, "%s", msg);
    return -1;
}

int qoi_decode_file(FILE *f, img_image_t *img, char *err, size_t errlen)
{
    if (fseek(f, 0, SEEK_END) != 0)
        return fail(err, errlen, "qoi: seek failed");
    long fsize = ftell(f);
    rewind(f);
    if (fsize < 14)
        return fail(err, errlen, "qoi: file too small");
    uint8_t *buf = (uint8_t *)malloc((size_t)fsize);
    if (!buf)
        return fail(err, errlen, "qoi: out of memory");
    if (fread(buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        free(buf);
        return fail(err, errlen, "qoi: read failed");
    }

    qoi_desc desc;
    void *pixels = qoi_decode(buf, (int)fsize, &desc, 0);
    free(buf);
    if (!pixels) {
        return fail(err, errlen, "qoi: invalid QOI data");
    }

    memset(img, 0, sizeof(*img));
    img->width = (int)desc.width;
    img->height = (int)desc.height;
    img->bit_depth = 8;
    img->color = (desc.channels == 4) ? IMG_RGBA : IMG_RGB;
    int ch = (desc.channels == 4) ? 4 : 3;
    img->rowstride = img_rowstride(img->width, 8, ch);
    size_t bytes = (size_t)img->width * img->height * (size_t)ch;
    img->data = (uint8_t *)malloc(bytes);
    if (!img->data) {
        free(pixels);
        return fail(err, errlen, "qoi: out of memory");
    }
    memcpy(img->data, pixels, bytes);
    free(pixels);
    return 0;
}
