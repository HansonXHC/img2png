#include "decode.h"
#include <stdlib.h>
#include <string.h>

static int fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen)
        snprintf(err, errlen, "bmp: %s", msg);
    return -1;
}

static unsigned rd16(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned rd32(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8) |
                                              ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24); }

/* Expand a channel sample extracted with the given mask to 8 bits. */
static uint8_t mask_to_8(uint32_t v, uint32_t mask)
{
    if (!mask)
        return 255;
    int shift = 0;
    while (!(mask & 1u)) { mask >>= 1; shift++; }
    int bits = 0;
    uint32_t m = mask;
    while (m) { bits += (m & 1u); m >>= 1; }
    uint32_t val = (v >> shift) & ((1u << bits) - 1u);
    if (bits == 8)
        return (uint8_t)val;
    if (bits == 0)
        return 255;
    return (uint8_t)(val * 255u / ((1u << bits) - 1u));
}

int bmp_decode(FILE *f, img_image_t *img, char *err, size_t errlen)
{
    uint8_t hdr[14];
    if (fread(hdr, 1, 14, f) != 14 || hdr[0] != 'B' || hdr[1] != 'M')
        return fail(err, errlen, "not a BMP file");
    uint32_t pixel_offset = rd32(hdr + 10);

    uint8_t dh[124];
    if (fread(dh, 1, 4, f) != 4)
        return fail(err, errlen, "truncated header");
    uint32_t hsize = rd32(dh);
    if (hsize > 124 - 4 || fread(dh + 4, 1, hsize - 4, f) != hsize - 4)
        return fail(err, errlen, "truncated DIB header");

    int32_t width, height;
    uint16_t bpp;
    uint32_t compression = 0, clr_used = 0;
    int core = 0;

    if (hsize == 12) {                      /* BITMAPCOREHEADER */
        core = 1;
        width  = (int16_t)rd16(dh + 4);
        height = (int16_t)rd16(dh + 6);
        bpp    = rd16(dh + 10);
    } else {
        width  = (int32_t)rd32(dh + 4);
        height = (int32_t)rd32(dh + 8);
        bpp    = rd16(dh + 14);
        compression = rd32(dh + 16);
        clr_used    = rd32(dh + 32);
    }

    if (width <= 0 || height == 0 || (height > 0 && height > 0x7FFFFFF))
        return fail(err, errlen, "bad dimensions");
    int top_down = (height < 0);
    int h = top_down ? -height : height;

    if (compression == 1 || compression == 2 || compression == 4 || compression == 5)
        return fail(err, errlen, "RLE/PCM/JPEG-compressed BMP not supported");
    if (compression != 0 && compression != 3 && compression != 6)
        return fail(err, errlen, "unsupported BMP compression");
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32)
        return fail(err, errlen, "unsupported bit depth (16-bit BMP not supported)");

    /* color masks */
    uint32_t rmask = 0, gmask = 0, bmask = 0, amask = 0;
    if (compression == 3 || compression == 6) {
        if (hsize >= 108) {                 /* V4/V5: masks live in the header */
            rmask = rd32(dh + 40); gmask = rd32(dh + 44);
            bmask = rd32(dh + 48); amask = rd32(dh + 52);
        } else {                            /* 40-byte header: 3 DWORDs follow */
            uint8_t m[12];
            if (fread(m, 1, 12, f) != 12)
                return fail(err, errlen, "truncated color masks");
            rmask = rd32(m); gmask = rd32(m + 4); bmask = rd32(m + 8);
        }
    }

    /* palette */
    uint8_t pal[256][3];
    uint8_t pal_a[256];
    int npal = 0;
    if (bpp <= 8) {
        npal = clr_used ? (int)clr_used : (1 << bpp);
        if (npal > (1 << bpp))
            return fail(err, errlen, "palette larger than color depth allows");
        uint32_t ent = core ? 3 : 4;
        for (int i = 0; i < npal; i++) {
            uint8_t e[4];
            if (fread(e, 1, ent, f) != ent)
                return fail(err, errlen, "truncated palette");
            pal[i][0] = e[2]; pal[i][1] = e[1]; pal[i][2] = e[0];   /* BGR -> RGB */
            pal_a[i] = 255;
        }
    }

    long row_bytes_src = ((long)width * bpp + 31) / 32 * 4;
    uint8_t *src = (uint8_t *)malloc((size_t)row_bytes_src * h);
    if (!src)
        return fail(err, errlen, "out of memory");
    if (fseek(f, (long)pixel_offset, SEEK_SET) != 0 ||
        fread(src, 1, (size_t)row_bytes_src * h, f) != (size_t)row_bytes_src * h) {
        free(src);
        return fail(err, errlen, "truncated pixel data");
    }

    memset(img, 0, sizeof(*img));
    img->width = width;
    img->height = h;

    /* decide target representation */
    if (bpp <= 8) {
        img->color = IMG_PALETTE;
        img->bit_depth = (int)bpp;
        img->pal_ncolors = npal;
        memcpy(img->palette, pal, (size_t)npal * 3);
        memcpy(img->pal_alpha, pal_a, (size_t)npal);
    } else if (bpp == 24) {
        img->color = IMG_RGB;
        img->bit_depth = 8;
    } else { /* 32 */
        if (amask) {
            img->color = IMG_RGBA;
        } else {
            img->color = IMG_RGB;   /* BI_RGB 32-bit: 4th byte undefined, not alpha */
        }
        img->bit_depth = 8;
    }
    int ch = img_channels(img->color);
    img->rowstride = img_rowstride(width, img->bit_depth, ch);
    img->data = (uint8_t *)calloc((size_t)h, img->rowstride);
    if (!img->data) { free(src); return fail(err, errlen, "out of memory"); }

    for (int y = 0; y < h; y++) {
        const uint8_t *srow = src + (size_t)y * row_bytes_src;
        int dy = top_down ? y : h - 1 - y;
        uint8_t *d = img->data + (size_t)dy * img->rowstride;
        int x;

        switch (img->color) {
        case IMG_PALETTE: {
            if (bpp == 8) {
                for (x = 0; x < width; x++) d[x] = srow[x];
            } else {
                memset(d, 0, img->rowstride);
                for (x = 0; x < width; x++) {
                    int bitpos = x * (int)bpp;
                    int byte = srow[bitpos >> 3];
                    int shift = 8 - (bitpos & 7) - (int)bpp;
                    unsigned idx = (byte >> shift) & ((1u << bpp) - 1u);
                    d[x] = (uint8_t)idx;
                }
            }
            break;
        }
        case IMG_RGB: {
            if (bpp == 24) {
                for (x = 0; x < width; x++) {
                    d[x * 3 + 0] = srow[x * 3 + 2];
                    d[x * 3 + 1] = srow[x * 3 + 1];
                    d[x * 3 + 2] = srow[x * 3 + 0];
                }
            } else { /* 32-bit, no alpha */
                for (x = 0; x < width; x++) {
                    uint32_t px = rd32(srow + x * 4);
                    d[x * 3 + 0] = mask_to_8(px, rmask ? rmask : 0x000000FFu);
                    d[x * 3 + 1] = mask_to_8(px, gmask ? gmask : 0x0000FF00u);
                    d[x * 3 + 2] = mask_to_8(px, bmask ? bmask : 0x00FF0000u);
                }
            }
            break;
        }
        case IMG_RGBA: {
            for (x = 0; x < width; x++) {
                uint32_t px = rd32(srow + x * 4);
                d[x * 4 + 0] = mask_to_8(px, rmask);
                d[x * 4 + 1] = mask_to_8(px, gmask);
                d[x * 4 + 2] = mask_to_8(px, bmask);
                d[x * 4 + 3] = mask_to_8(px, amask);
            }
            break;
        }
        default:
            break;
        }
    }
    free(src);
    return 0;
}
