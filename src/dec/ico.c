#include "decode.h"
#include <stdlib.h>
#include <string.h>

static int fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen)
        snprintf(err, errlen, "ico: %s", msg);
    return -1;
}

static unsigned rd16(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned rd32(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8) |
                                              ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24); }

/* Decode a DIB stored inside an ICO entry (BITMAPINFOHEADER, no file header,
 * XOR rows followed by a 1-bit AND mask). */
static int ico_bmp_decode(const uint8_t *buf, size_t size, int entry_w, int entry_h,
                          img_image_t *img, char *err, size_t errlen)
{
    if (size < 40)
        return fail(err, errlen, "truncated DIB header");
    uint32_t hsize = rd32(buf);
    if (hsize < 40)
        return fail(err, errlen, "unsupported DIB header");

    int32_t width  = (int32_t)rd32(buf + 4);
    int32_t height2 = (int32_t)rd32(buf + 8);   /* XOR + AND rows */
    uint16_t bpp   = rd16(buf + 14);
    uint32_t clr_used = rd32(buf + 32);

    int h = height2 / 2;
    if (width <= 0 || h <= 0 || height2 % 2 != 0)
        return fail(err, errlen, "bad dimensions");
    if (entry_w > 0 && entry_w != width)
        width = entry_w;    /* some writers disagree; trust the entry */
    if (entry_h > 0 && entry_h != h)
        h = entry_h;

    if (bpp == 16)
        return fail(err, errlen, "16-bit ICO entries not supported");
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32)
        return fail(err, errlen, "unsupported bit depth");

    /* palette */
    uint8_t pal[256][3];
    int npal = 0;
    size_t pos = hsize;
    if (bpp <= 8) {
        npal = clr_used ? (int)clr_used : (1 << bpp);
        if (npal > (1 << bpp))
            return fail(err, errlen, "palette too large");
        if (pos + (size_t)npal * 4 > size)
            return fail(err, errlen, "truncated palette");
        for (int i = 0; i < npal; i++) {
            pal[i][0] = buf[pos + i * 4 + 2];
            pal[i][1] = buf[pos + i * 4 + 1];
            pal[i][2] = buf[pos + i * 4 + 0];
        }
        pos += (size_t)npal * 4;
    }

    size_t xor_row = ((size_t)width * bpp + 31) / 32 * 4;
    size_t and_row = ((size_t)width + 31) / 32 * 4;
    if (pos + xor_row * h + and_row * h > size)
        return fail(err, errlen, "truncated pixel data");
    const uint8_t *xor_data = buf + pos;
    const uint8_t *and_data = buf + pos + xor_row * h;

    /* Does the 1-bit AND mask carry any transparency?  Legacy icons express
     * transparency this way instead of with an alpha channel. */
    int mask_any = 0;
    for (size_t i = 0; i < and_row * (size_t)h && !mask_any; i++)
        if (and_data[i])
            mask_any = 1;

    memset(img, 0, sizeof(*img));
    img->width = (int)width;
    img->height = h;

    /* set when the AND mask supplies the alpha for a 24/32-bit entry */
    int alpha_from_mask = 0;

    if (bpp <= 8) {
        img->color = IMG_PALETTE;
        img->bit_depth = (int)bpp;
        img->pal_ncolors = npal;
        memcpy(img->palette, pal, (size_t)npal * 3);
        for (int i = 0; i < 256; i++)
            img->pal_alpha[i] = 255;
    } else if (bpp == 24) {
        img->color = mask_any ? IMG_RGBA : IMG_RGB;
        alpha_from_mask = mask_any;
        img->bit_depth = 8;
    } else {
        /* 32-bit: if every alpha byte is 0 the file is opaque despite the
         * 4th channel (common in legacy icons), so use the AND mask instead */
        int any_alpha = 0;
        for (size_t i = 0; i < xor_row * (size_t)h; i += 4)
            if (xor_data[i + 3]) { any_alpha = 1; break; }
        alpha_from_mask = !any_alpha && mask_any;
        img->color = (any_alpha || mask_any) ? IMG_RGBA : IMG_RGB;
        img->bit_depth = 8;
    }

    int ch = img_channels(img->color);
    img->rowstride = img_rowstride(img->width, img->bit_depth, ch);
    img->data = (uint8_t *)calloc((size_t)h, img->rowstride);
    if (!img->data)
        return fail(err, errlen, "out of memory");

    int mask_used = 0;
    for (int y = 0; y < h; y++) {
        const uint8_t *srow = xor_data + (size_t)y * xor_row;   /* bottom-up */
        int dy = h - 1 - y;
        const uint8_t *arow = and_data + (size_t)y * and_row;
        uint8_t *d = img->data + (size_t)dy * img->rowstride;
        int x;

        switch (img->color) {
        case IMG_PALETTE:
            for (x = 0; x < width; x++) {
                unsigned idx = (bpp == 8) ? srow[x]
                                          : img_ld_bits(srow, x, (int)bpp);
                if (idx >= (unsigned)npal)
                    idx = 0;
                if (bpp == 8)
                    d[x] = (uint8_t)idx;
                else
                    img_st_bits(d, x, (int)bpp, idx);
                /* AND mask: bit set = transparent -> zero that entry's alpha */
                if (arow[x >> 3] & (0x80u >> (x & 7))) {
                    img->pal_alpha[idx] = 0;
                    mask_used = 1;
                }
            }
            break;
        case IMG_RGB:
            if (bpp == 24) {
                for (x = 0; x < width; x++) {
                    d[x * 3 + 0] = srow[x * 3 + 2];
                    d[x * 3 + 1] = srow[x * 3 + 1];
                    d[x * 3 + 2] = srow[x * 3 + 0];
                }
            } else {    /* opaque 32-bit */
                for (x = 0; x < width; x++) {
                    d[x * 3 + 0] = srow[x * 4 + 2];
                    d[x * 3 + 1] = srow[x * 4 + 1];
                    d[x * 3 + 2] = srow[x * 4 + 0];
                }
            }
            break;
        case IMG_RGBA:
            if (bpp == 24) {    /* promoted for AND-mask transparency */
                for (x = 0; x < width; x++) {
                    d[x * 4 + 0] = srow[x * 3 + 2];
                    d[x * 4 + 1] = srow[x * 3 + 1];
                    d[x * 4 + 2] = srow[x * 3 + 0];
                    d[x * 4 + 3] = (arow[x >> 3] & (0x80u >> (x & 7))) ? 0 : 255;
                }
            } else {
                for (x = 0; x < width; x++) {
                    d[x * 4 + 0] = srow[x * 4 + 2];
                    d[x * 4 + 1] = srow[x * 4 + 1];
                    d[x * 4 + 2] = srow[x * 4 + 0];
                    d[x * 4 + 3] = alpha_from_mask
                        ? ((arow[x >> 3] & (0x80u >> (x & 7))) ? 0 : 255)
                        : srow[x * 4 + 3];
                }
            }
            break;
        default:
            break;
        }
    }
    if (mask_used)
        img->has_pal_alpha = 1;
    return 0;
}

int ico_decode(FILE *f, img_image_t *img, char *err, size_t errlen)
{
    uint8_t dir[6];
    if (fread(dir, 1, 6, f) != 6 || rd16(dir + 2) != 1)
        return fail(err, errlen, "not an ICO file");
    int count = (int)rd16(dir + 4);
    if (count <= 0 || count > 64)
        return fail(err, errlen, "bad icon directory");

    long best = -1;
    long best_area = -1;
    unsigned best_bits = 0;
    int best_w = 0, best_h = 0;
    for (int i = 0; i < count; i++) {
        uint8_t e[16];
        if (fread(e, 1, 16, f) != 16)
            return fail(err, errlen, "truncated icon directory");
        int w = e[0] ? e[0] : 256;
        int h = e[1] ? e[1] : 256;
        unsigned bitcount = rd16(e + 6);
        long offset = (long)rd32(e + 12);
        long area = (long)w * h;
        /* prefer larger area, then higher bit count */
        if (area > best_area || (area == best_area && bitcount > best_bits)) {
            best_area = area;
            best_bits = bitcount;
            best = offset;
            best_w = w;
            best_h = h;
        }
    }
    if (best < 0)
        return fail(err, errlen, "no usable entry");

    if (fseek(f, 0, SEEK_END) != 0)
        return fail(err, errlen, "seek failed");
    long fsize = ftell(f);
    long limit = fsize - best;
    if (limit <= 0)
        return fail(err, errlen, "entry out of range");
    if (fseek(f, best, SEEK_SET) != 0)
        return fail(err, errlen, "seek failed");

    uint8_t *buf = (uint8_t *)malloc((size_t)limit);
    if (!buf)
        return fail(err, errlen, "out of memory");
    size_t got = fread(buf, 1, (size_t)limit, f);

    int rc;
    if (got >= 8 && buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' && buf[3] == 'G')
        rc = png_decode_mem(buf, got, img, err, errlen);
    else
        rc = ico_bmp_decode(buf, got, best_w, best_h, img, err, errlen);
    free(buf);
    return rc;
}
