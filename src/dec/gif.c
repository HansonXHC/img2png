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

/* ---------------- animated GIF ---------------- */

/* NETSCAPE2.0 application extension: loop count (0 = infinite).
 * giflib stores the app id ("NETSCAPE2.0") and its data sub-blocks as
 * chained ExtensionBlocks; the loop value lives in the following CONTINUE
 * block as {0x01, lo, hi}.  Scans top-level and per-image chains. */
static int gif_parse_loops(const GifFileType *gif)
{
    for (int i = 0; i < gif->ImageCount; i++) {
        const ExtensionBlock *eb = gif->SavedImages[i].ExtensionBlocks;
        int n = gif->SavedImages[i].ExtensionBlockCount;
        for (int b = 0; b < n; b++) {
            if (eb[b].Function == APPLICATION_EXT_FUNC_CODE &&
                eb[b].ByteCount >= 11 &&
                !memcmp(eb[b].Bytes, "NETSCAPE2.0", 11)) {
                if (b + 1 < n && eb[b + 1].ByteCount >= 3 &&
                    eb[b + 1].Bytes[0] == 1)
                    return eb[b + 1].Bytes[1] |
                           ((int)eb[b + 1].Bytes[2] << 8);
                return -1;
            }
        }
    }
    for (int b = 0; b < gif->ExtensionBlockCount; b++) {
        const ExtensionBlock *eb = gif->ExtensionBlocks;
        if (eb[b].Function == APPLICATION_EXT_FUNC_CODE &&
            eb[b].ByteCount >= 11 &&
            !memcmp(eb[b].Bytes, "NETSCAPE2.0", 11)) {
            if (b + 1 < gif->ExtensionBlockCount &&
                eb[b + 1].ByteCount >= 3 && eb[b + 1].Bytes[0] == 1)
                return eb[b + 1].Bytes[1] |
                       ((int)eb[b + 1].Bytes[2] << 8);
            return -1;
        }
    }
    return -1;   /* no NETSCAPE extension: play once */
}

/* map frame row (0..rh-1, canvas order) -> row index within RasterBits
 * (GIF interlace stores rows in 8/8/4/4/2/2 pass order) */
static int *gif_row_map(int rh, int interlace)
{
    int *map = (int *)malloc((size_t)rh * sizeof(int));
    if (!map)
        return NULL;
    if (!interlace) {
        for (int y = 0; y < rh; y++)
            map[y] = y;
        return map;
    }
    static const int offs[4] = {0, 4, 2, 1};
    static const int step[4] = {8, 8, 4, 2};
    int y = 0;
    for (int pass = 0; pass < 4; pass++)
        for (int row = offs[pass]; row < rh; row += step[pass], y++)
            map[row] = y;
    return map;
}

int gif_decode_anim(FILE *f, img_animation_t *anim, char *err, size_t errlen)
{
    int gif_err = 0;
    memset(anim, 0, sizeof(*anim));

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

    int W = gif->SWidth, H = gif->SHeight;
    if (W <= 0 || H <= 0 || W > 32767 || H > 32767) {
        DGifCloseFile(gif, &gif_err);
        return fail(err, errlen, "gif: bad canvas size");
    }

    anim->width = W;
    anim->height = H;
    anim->nframes = gif->ImageCount;
    anim->loops = gif_parse_loops(gif);
    anim->frames = (img_image_t *)calloc((size_t)anim->nframes, sizeof(img_image_t));
    anim->x = (int *)calloc((size_t)anim->nframes, sizeof(int));
    anim->y = (int *)calloc((size_t)anim->nframes, sizeof(int));
    anim->delays_cs = (int *)calloc((size_t)anim->nframes, sizeof(int));
    anim->dispose = (int *)calloc((size_t)anim->nframes, sizeof(int));
    if (!anim->frames || !anim->x || !anim->y || !anim->delays_cs || !anim->dispose) {
        DGifCloseFile(gif, &gif_err);
        img_free_anim(anim);
        return fail(err, errlen, "gif: out of memory");
    }

    /* RGBA canvas (R,G,B,A bytes), fully transparent */
    uint8_t *canvas = (uint8_t *)calloc((size_t)W * H, 4);
    uint8_t *snapshot = (uint8_t *)malloc((size_t)W * H * 4);
    if (!canvas || !snapshot) {
        free(canvas);
        free(snapshot);
        DGifCloseFile(gif, &gif_err);
        img_free_anim(anim);
        return fail(err, errlen, "gif: out of memory");
    }

    for (int i = 0; i < anim->nframes; i++) {
        SavedImage *im = &gif->SavedImages[i];
        GraphicsControlBlock gcb;
        if (DGifSavedExtensionToGCB(gif, i, &gcb) == GIF_ERROR) {
            memset(&gcb, 0, sizeof(gcb));
            gcb.TransparentColor = -1;
            gcb.DelayTime = 0;
            gcb.DisposalMode = 0;
        }
        anim->delays_cs[i] = gcb.DelayTime;
        anim->dispose[i] = gcb.DisposalMode;

        /* frame rect, clamped to the canvas */
        int rx = im->ImageDesc.Left, ry = im->ImageDesc.Top;
        int rw = im->ImageDesc.Width, rh = im->ImageDesc.Height;
        if (rx < 0) { rw += rx; rx = 0; }
        if (ry < 0) { rh += ry; ry = 0; }
        if (rx + rw > W) rw = W - rx;
        if (ry + rh > H) rh = H - ry;
        if (rw <= 0 || rh <= 0) {
            free(canvas);
            free(snapshot);
            DGifCloseFile(gif, &gif_err);
            img_free_anim(anim);
            return fail(err, errlen, "gif: frame rect outside canvas");
        }
        anim->x[i] = rx;
        anim->y[i] = ry;
        anim->frames[i].width = rw;
        anim->frames[i].height = rh;
        anim->frames[i].bit_depth = 8;
        anim->frames[i].color = IMG_RGBA;
        anim->frames[i].rowstride = img_rowstride(rw, 8, 4);
        anim->frames[i].data = (uint8_t *)malloc((size_t)rw * rh * 4);
        if (!anim->frames[i].data) {
            free(canvas);
            free(snapshot);
            DGifCloseFile(gif, &gif_err);
            img_free_anim(anim);
            return fail(err, errlen, "gif: out of memory");
        }

        /* disposal 3 needs the pre-frame canvas snapshot */
        if (gcb.DisposalMode == 3)
            memcpy(snapshot, canvas, (size_t)W * H * 4);

        ColorMapObject *cmap = im->ImageDesc.ColorMap ? im->ImageDesc.ColorMap
                                                      : gif->SColorMap;
        if (!cmap) {
            free(canvas);
            free(snapshot);
            DGifCloseFile(gif, &gif_err);
            img_free_anim(anim);
            return fail(err, errlen, "gif: missing color table");
        }

        int *rowmap = gif_row_map(rh, im->ImageDesc.Interlace);
        if (!rowmap) {
            free(canvas);
            free(snapshot);
            DGifCloseFile(gif, &gif_err);
            img_free_anim(anim);
            return fail(err, errlen, "gif: out of memory");
        }

        /* compose the frame onto the canvas region; the stored region is
         * the canvas state AFTER painting (APNG blend ALPHA reproduces
         * exactly this at decode time) */
        const uint8_t *raster = (const uint8_t *)im->RasterBits;
        uint8_t *dst = anim->frames[i].data;
        for (int yy = 0; yy < rh; yy++) {
            int cy = ry + yy;
            const uint8_t *srow = raster + (size_t)rowmap[yy] * rw;
            uint8_t *drow = dst + (size_t)yy * anim->frames[i].rowstride;
            uint8_t *crow = canvas + ((size_t)cy * W + rx) * 4;
            for (int xx = 0; xx < rw; xx++) {
                int idx = srow[xx];
                uint32_t *cp;
                if (idx == gcb.TransparentColor) {
                    /* keep canvas content */
                } else if (idx < cmap->ColorCount) {
                    cp = (uint32_t *)(crow + xx * 4);
                    *cp = 0xFF000000u |
                          (uint32_t)cmap->Colors[idx].Red |
                          ((uint32_t)cmap->Colors[idx].Green << 8) |
                          ((uint32_t)cmap->Colors[idx].Blue << 16);
                }
                drow[xx * 4 + 0] = crow[xx * 4 + 0];
                drow[xx * 4 + 1] = crow[xx * 4 + 1];
                drow[xx * 4 + 2] = crow[xx * 4 + 2];
                drow[xx * 4 + 3] = crow[xx * 4 + 3];
            }
        }
        free(rowmap);

        /* apply disposal for the next frame */
        if (gcb.DisposalMode == 2) {           /* restore to background */
            for (int yy = ry; yy < ry + rh; yy++)
                memset(canvas + ((size_t)yy * W + rx) * 4, 0, (size_t)rw * 4);
        } else if (gcb.DisposalMode == 3) {    /* restore to previous */
            memcpy(canvas, snapshot, (size_t)W * H * 4);
        }
    }

    free(canvas);
    free(snapshot);
    DGifCloseFile(gif, &gif_err);
    return 0;
}
