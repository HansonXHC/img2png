#include "decode.h"
#include <stdlib.h>
#include <string.h>

static int fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen)
        snprintf(err, errlen, "tga: %s", msg);
    return -1;
}

static unsigned rd16(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }

/* expand a 5-bit channel to 8 bits */
static uint8_t c5_to_8(unsigned v) { return (uint8_t)((v * 255u + 15u) / 31u); }

int tga_decode(FILE *f, img_image_t *img, char *err, size_t errlen)
{
    uint8_t h[18];
    if (fread(h, 1, 18, f) != 18)
        return fail(err, errlen, "not a TGA file");

    unsigned id_len    = h[0];
    unsigned cmap_type = h[1];
    unsigned img_type  = h[2];
    unsigned cmap_first = rd16(h + 3);
    unsigned cmap_len  = rd16(h + 5);
    unsigned cmap_bpp  = h[7];
    unsigned width     = rd16(h + 12);
    unsigned height    = rd16(h + 14);
    unsigned depth     = h[16];
    unsigned desc      = h[17];
    int top_down   = (desc & 0x20) != 0;
    unsigned alpha_bits = desc & 0x0F;

    int rle = 0;
    switch (img_type) {
    case 1: case 2: case 3:              break;
    case 9: case 10: case 11: rle = 1;   break;
    case 0:  return fail(err, errlen, "no image data");
    case 32: case 33: return fail(err, errlen, "compressed TGA (Huffman/VQ) not supported");
    default: return fail(err, errlen, "unknown image type");
    }

    if (img_type == 1 || img_type == 9) {          /* colormapped */
        if (cmap_type != 1)
            return fail(err, errlen, "colormapped image without color map");
        if (depth != 8)
            return fail(err, errlen, "only 8-bit color map indices supported");
        if (cmap_bpp != 15 && cmap_bpp != 16 && cmap_bpp != 24 && cmap_bpp != 32)
            return fail(err, errlen, "unsupported color map entry size");
    } else {
        if (cmap_type != 0)
            return fail(err, errlen, "unexpected color map");
    }

    switch (depth) {
    case 8:
        if (img_type != 1 && img_type != 9 && img_type != 3 && img_type != 11)
            return fail(err, errlen, "8-bit truecolor not supported");
        break;
    case 15: case 16:
        if (img_type == 2 || img_type == 10)
            return fail(err, errlen, "15/16-bit truecolor not supported");
        break;
    case 24: case 32:
        break;
    default:
        return fail(err, errlen, "unsupported pixel depth");
    }

    if (width == 0 || height == 0)
        return fail(err, errlen, "bad dimensions");

    if (id_len && fseek(f, id_len, SEEK_CUR) != 0)
        return fail(err, errlen, "truncated image id");

    /* color map */
    uint8_t pal[256][4];
    int npal = 0, pal_alpha = 0;
    if (cmap_type == 1) {
        if (cmap_len == 0 || cmap_first + cmap_len > 256)
            return fail(err, errlen, "color map too large");
        unsigned ent_bytes = (cmap_bpp == 15 || cmap_bpp == 16) ? 2 : cmap_bpp / 8;
        for (unsigned i = 0; i < cmap_len; i++) {
            uint8_t e[4] = {0, 0, 0, 255};
            if (fread(e, 1, ent_bytes, f) != ent_bytes)
                return fail(err, errlen, "truncated color map");
            unsigned slot = cmap_first + i;
            if (cmap_bpp == 15 || cmap_bpp == 16) {
                unsigned v = e[0] | (e[1] << 8);        /* 1:5:5:5 ARGB little-endian */
                pal[slot][0] = c5_to_8((v >> 10) & 31);
                pal[slot][1] = c5_to_8((v >> 5) & 31);
                pal[slot][2] = c5_to_8(v & 31);
                pal[slot][3] = 255;
            } else {
                pal[slot][0] = e[2]; pal[slot][1] = e[1]; pal[slot][2] = e[0];
                pal[slot][3] = cmap_bpp == 32 ? e[3] : 255;
            }
            if (pal[slot][3] != 255)
                pal_alpha = 1;
        }
        npal = (int)(cmap_first + cmap_len);
    }

    unsigned px_bytes = (depth == 15 || depth == 16) ? 2 : depth / 8;
    size_t raw_size = (size_t)width * height * px_bytes;
    uint8_t *raw = (uint8_t *)malloc(raw_size ? raw_size : 1);
    if (!raw)
        return fail(err, errlen, "out of memory");

    if (!rle) {
        if (fread(raw, 1, raw_size, f) != raw_size) {
            free(raw);
            return fail(err, errlen, "truncated pixel data");
        }
    } else {
        uint8_t *out = raw, *end = raw + raw_size;
        while (out < end) {
            uint8_t pkt;
            if (fread(&pkt, 1, 1, f) != 1) {
                free(raw);
                return fail(err, errlen, "truncated RLE data");
            }
            unsigned count = (pkt & 0x7F) + 1;
            size_t need = (size_t)count * px_bytes;
            if (pkt & 0x80) {
                uint8_t px[4];
                if (fread(px, 1, px_bytes, f) != px_bytes) {
                    free(raw);
                    return fail(err, errlen, "truncated RLE data");
                }
                if ((size_t)(end - out) < need) { free(raw); return fail(err, errlen, "corrupt RLE data"); }
                for (unsigned i = 0; i < count; i++, out += px_bytes)
                    memcpy(out, px, px_bytes);
            } else {
                if ((size_t)(end - out) < need || fread(out, 1, need, f) != need) {
                    free(raw);
                    return fail(err, errlen, "truncated RLE data");
                }
                out += need;
            }
        }
    }

    memset(img, 0, sizeof(*img));
    img->width = (int)width;
    img->height = (int)height;

    if (img_type == 1 || img_type == 9) {
        img->color = IMG_PALETTE;
        img->bit_depth = 8;
        img->pal_ncolors = npal;
        for (int i = 0; i < npal; i++) {
            img->palette[i * 3 + 0] = pal[i][0];
            img->palette[i * 3 + 1] = pal[i][1];
            img->palette[i * 3 + 2] = pal[i][2];
            img->pal_alpha[i] = pal_alpha ? pal[i][3] : 255;
        }
        img->has_pal_alpha = pal_alpha;
    } else if (img_type == 3 || img_type == 11) {
        img->color = (depth == 16) ? IMG_GRAY_ALPHA : IMG_GRAY;
        img->bit_depth = 8;
    } else {
        img->color = (depth == 32 && alpha_bits == 8) ? IMG_RGBA : IMG_RGB;
        img->bit_depth = 8;
    }

    int ch = img_channels(img->color);
    img->rowstride = img_rowstride(img->width, img->bit_depth, ch);
    img->data = (uint8_t *)calloc((size_t)height, img->rowstride);
    if (!img->data) { free(raw); return fail(err, errlen, "out of memory"); }

    for (unsigned y = 0; y < height; y++) {
        const uint8_t *srow = raw + (size_t)y * width * px_bytes;
        int dy = top_down ? (int)y : (int)height - 1 - (int)y;
        uint8_t *d = img->data + (size_t)dy * img->rowstride;

        for (unsigned x = 0; x < width; x++) {
            const uint8_t *s = srow + x * px_bytes;
            switch (img->color) {
            case IMG_PALETTE:
                d[x] = s[0];
                break;
            case IMG_GRAY:
                d[x] = s[0];
                break;
            case IMG_GRAY_ALPHA:
                d[x * 2 + 0] = s[0];
                d[x * 2 + 1] = s[1];
                break;
            case IMG_RGB:
                d[x * 3 + 0] = s[2]; d[x * 3 + 1] = s[1]; d[x * 3 + 2] = s[0];
                break;
            case IMG_RGBA:
                d[x * 4 + 0] = s[2]; d[x * 4 + 1] = s[1];
                d[x * 4 + 2] = s[0]; d[x * 4 + 3] = s[3];
                break;
            }
        }
    }
    free(raw);
    return 0;
}
