#include "decode.h"
#include <stdlib.h>
#include <string.h>
#include <webp/decode.h>
#include <webp/demux.h>

static int fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen)
        snprintf(err, errlen, "%s", msg);
    return -1;
}

/* EXIF / ICC / XMP live in RIFF chunks next to the bitstream, so the container
 * has to be demuxed separately from the pixel decode. */
static void webp_meta(const uint8_t *buf, size_t size, img_meta_t *m)
{
    WebPData wd;
    wd.bytes = buf;
    wd.size = size;
    WebPDemuxer *dm = WebPDemux(&wd);
    if (!dm)
        return;
    WebPChunkIterator it;
    if (WebPDemuxGetChunk(dm, "EXIF", 1, &it)) {
        img_meta_set_exif(m, it.chunk.bytes, it.chunk.size);
        WebPDemuxReleaseChunkIterator(&it);
    }
    if (WebPDemuxGetChunk(dm, "ICCP", 1, &it)) {
        img_meta_set_blob(&m->icc, &m->icc_len, it.chunk.bytes, it.chunk.size);
        WebPDemuxReleaseChunkIterator(&it);
    }
    if (WebPDemuxGetChunk(dm, "XMP ", 1, &it)) {
        img_meta_set_blob((uint8_t **)&m->xmp, &m->xmp_len,
                          it.chunk.bytes, it.chunk.size);
        WebPDemuxReleaseChunkIterator(&it);
    }
    WebPDemuxDelete(dm);
}

int webp_decode(FILE *f, img_image_t *img, char *err, size_t errlen)
{
    if (fseek(f, 0, SEEK_END) != 0)
        return fail(err, errlen, "webp: seek failed");
    long fsize = ftell(f);
    rewind(f);
    if (fsize < 16)
        return fail(err, errlen, "webp: file too small");
    uint8_t *buf = (uint8_t *)malloc((size_t)fsize);
    if (!buf)
        return fail(err, errlen, "webp: out of memory");
    if (fread(buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        free(buf);
        return fail(err, errlen, "webp: read failed");
    }

    WebPBitstreamFeatures features;
    VP8StatusCode st = WebPGetFeatures(buf, (size_t)fsize, &features);
    if (st != VP8_STATUS_OK) {
        free(buf);
        return fail(err, errlen, "webp: invalid WebP data");
    }

    int w = 0, h = 0;
    int has_alpha = features.has_alpha;
    uint8_t *pixels = has_alpha
        ? WebPDecodeRGBA(buf, (size_t)fsize, &w, &h)
        : WebPDecodeRGB(buf, (size_t)fsize, &w, &h);
    if (!pixels) {
        free(buf);
        return fail(err, errlen, "webp: decode failed");
    }

    memset(img, 0, sizeof(*img));
    webp_meta(buf, (size_t)fsize, &img->meta);
    free(buf);

    img->width = w;
    img->height = h;
    img->bit_depth = 8;
    img->color = has_alpha ? IMG_RGBA : IMG_RGB;
    int ch = has_alpha ? 4 : 3;
    img->rowstride = img_rowstride(w, 8, ch);
    img->data = (uint8_t *)malloc((size_t)w * h * (size_t)ch);
    if (!img->data) {
        WebPFree(pixels);
        return fail(err, errlen, "webp: out of memory");
    }
    memcpy(img->data, pixels, (size_t)w * h * (size_t)ch);
    WebPFree(pixels);
    return 0;
}
