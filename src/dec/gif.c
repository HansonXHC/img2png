#include "decode.h"
#include <stdlib.h>
#include <string.h>
#include <gif_lib.h>

static int fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen)
        snprintf(err, errlen, "%s", msg);
    return -1;
}

static int gif_read_fn(GifFileType *gif, GifByteType *buf, int len)
{
    return (int)fread(buf, 1, (size_t)len, (FILE *)gif->UserData);
}

int gif_decode(FILE *f, img_image_t *img, char *err, size_t errlen)
{
    int gif_err = 0;
    GifFileType *gif = DGifOpen(f, gif_read_fn, &gif_err);
    if (!gif) {
        if (err && errlen)
            snprintf(err, errlen, "gif: %s", GifErrorString(gif_err));
        return -1;
    }
    if (DGifSlurp(gif) == GIF_ERROR || gif->ImageCount < 1) {
        DGifCloseFile(gif, &gif_err);
        if (err && errlen)
            snprintf(err, errlen, "gif: invalid or truncated GIF");
        return -1;
    }

    /* v1: convert the first frame */
    SavedImage *im = &gif->SavedImages[0];
    int w = im->ImageDesc.Width, h = im->ImageDesc.Height;
    ColorMapObject *cmap = im->ImageDesc.ColorMap ? im->ImageDesc.ColorMap
                                                  : gif->SColorMap;
    if (!cmap || w <= 0 || h <= 0) {
        DGifCloseFile(gif, &gif_err);
        if (err && errlen)
            snprintf(err, errlen, "gif: missing color table");
        return -1;
    }

    GraphicsControlBlock gcb;
    DGifSavedExtensionToGCB(gif, 0, &gcb);
    int transparent = gcb.TransparentColor;   /* -1 when absent */

    memset(img, 0, sizeof(*img));
    img->width = w;
    img->height = h;
    img->color = IMG_PALETTE;
    img->bit_depth = 8;
    img->pal_ncolors = cmap->ColorCount;
    for (int i = 0; i < img->pal_ncolors; i++) {
        img->palette[i * 3 + 0] = cmap->Colors[i].Red;
        img->palette[i * 3 + 1] = cmap->Colors[i].Green;
        img->palette[i * 3 + 2] = cmap->Colors[i].Blue;
        img->pal_alpha[i] = 255;
    }
    if (transparent >= 0 && transparent < img->pal_ncolors) {
        img->pal_alpha[transparent] = 0;
        img->has_pal_alpha = 1;
    }

    img->rowstride = img_rowstride(w, 8, 1);
    img->data = (uint8_t *)calloc((size_t)h, img->rowstride);
    if (!img->data) {
        DGifCloseFile(gif, &gif_err);
        return fail(err, errlen, "gif: out of memory");
    }

    /* deinterlace if needed (GIF stores passes 8/8/4/4/2/2) */
    const uint8_t *src = (const uint8_t *)im->RasterBits;
    if (im->ImageDesc.Interlace) {
        static const int offs[4] = {0, 4, 2, 1};
        static const int step[4] = {8, 8, 4, 2};
        int y = 0;
        for (int pass = 0; pass < 4; pass++)
            for (int row = offs[pass]; row < h; row += step[pass], y++)
                memcpy(img->data + (size_t)row * img->rowstride,
                       src + (size_t)y * w, (size_t)w);
    } else {
        for (int row = 0; row < h; row++)
            memcpy(img->data + (size_t)row * img->rowstride,
                   src + (size_t)row * w, (size_t)w);
    }

    DGifCloseFile(gif, &gif_err);
    return 0;
}
