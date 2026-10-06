/* Self-test: generates sample images in various formats, converts them with
 * the same core code the CLI uses, then reads the PNGs back with libpng and
 * compares pixel-by-pixel.  Exits 0 when everything matches. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#endif
#include <png.h>

#include "image.h"
#include "dec/decode.h"
#include "enc/pngenc.h"
#include "util/filetime.h"
#include "util/pngerr.h"
#include <gif_lib.h>
#include <qoi.h>
#include <webp/encode.h>
#include <jpeglib.h>
#include <tiffio.h>
#include "enc/apngenc.h"

static int g_failures = 0;
static uint8_t *wp_blob = NULL;

#define CHECK(cond, name)                                                  \
    do {                                                                   \
        if (cond) printf("  PASS  %s\n", name);                            \
        else     { printf("  FAIL  %s\n", name); g_failures++; }           \
    } while (0)

static void fill_gradient_rgba(uint8_t *d, int w, int h, int alpha)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            d[(size_t)(y * w + x) * 4 + 0] = (uint8_t)(x * 255 / (w > 1 ? w - 1 : 1));
            d[(size_t)(y * w + x) * 4 + 1] = (uint8_t)(y * 255 / (h > 1 ? h - 1 : 1));
            d[(size_t)(y * w + x) * 4 + 2] = (uint8_t)((x ^ y) & 0xFF);
            d[(size_t)(y * w + x) * 4 + 3] = (uint8_t)alpha;
        }
}

static void fill_gradient_rgb(uint8_t *d, int w, int h)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            d[(size_t)(y * w + x) * 3 + 0] = (uint8_t)(x * 255 / (w > 1 ? w - 1 : 1));
            d[(size_t)(y * w + x) * 3 + 1] = (uint8_t)(y * 255 / (h > 1 ? h - 1 : 1));
            d[(size_t)(y * w + x) * 3 + 2] = (uint8_t)((x ^ y) & 0xFF);
        }
}

static void fill_gradient_gray(uint8_t *d, int w, int h)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            d[y * w + x] = (uint8_t)((x * 7 + y * 13) & 0xFF);
}

/* --- generators, all bottom-up (the common BMP layout) --- */

static int gen_bmp24(const char *path, int w, int h)
{
    long row = ((long)w * 3 + 3) / 4 * 4;
    long pix = row * h;
    uint8_t *px = (uint8_t *)calloc(1, (size_t)pix);
    uint8_t hdr[54] = {'B','M'};
    uint32_t fsz = 54 + (uint32_t)pix;
    hdr[2] = (uint8_t)fsz; hdr[3] = fsz >> 8; hdr[4] = fsz >> 16; hdr[5] = fsz >> 24;
    hdr[10] = 54;
    uint32_t hsz = 40; memcpy(hdr + 14, &hsz, 4);
    int32_t w32 = w, h32 = h; memcpy(hdr + 18, &w32, 4); memcpy(hdr + 22, &h32, 4);
    uint16_t planes = 1, bpp = 24; memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);

    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *d = px + (size_t)(h - 1 - y) * row + (size_t)x * 3;
            d[0] = (uint8_t)((x ^ y) & 0xFF);
            d[1] = (uint8_t)(y * 255 / (h > 1 ? h - 1 : 1));
            d[2] = (uint8_t)(x * 255 / (w > 1 ? w - 1 : 1));
        }

    FILE *f = img_fopen_write(path);
    int ok = f && fwrite(hdr, 1, 54, f) == 54 && fwrite(px, 1, (size_t)pix, f) == (size_t)pix;
    if (f) fclose(f);
    free(px);
    return ok ? 0 : -1;
}

static int gen_bmp32_alpha(const char *path, int w, int h)
{
    /* BITMAPV4HEADER with alpha mask so the alpha channel is real */
    uint8_t hdr[14 + 108];
    memset(hdr, 0, sizeof(hdr));
    long row = (long)w * 4;
    long pix = row * h;
    hdr[0] = 'B'; hdr[1] = 'M';
    uint32_t fsz = (uint32_t)(14 + 108 + pix);
    hdr[2] = (uint8_t)fsz; hdr[3] = fsz >> 8; hdr[4] = fsz >> 16; hdr[5] = fsz >> 24;
    hdr[10] = 14 + 108;
    uint32_t hsz = 108; memcpy(hdr + 14, &hsz, 4);
    int32_t w32 = w, h32 = h; memcpy(hdr + 18, &w32, 4); memcpy(hdr + 22, &h32, 4);
    uint16_t planes = 1, bpp = 32; memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);
    uint32_t comp = 3 /* BI_BITFIELDS */; memcpy(hdr + 30, &comp, 4);
    uint32_t rm = 0x00FF0000, gm = 0x0000FF00, bm = 0x000000FF, am = 0xFF000000;
    memcpy(hdr + 54, &rm, 4); memcpy(hdr + 58, &gm, 4);
    memcpy(hdr + 62, &bm, 4); memcpy(hdr + 66, &am, 4);

    uint8_t *px = (uint8_t *)malloc((size_t)pix);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *d = px + (size_t)(h - 1 - y) * row + (size_t)x * 4;
            d[0] = (uint8_t)((x ^ y) & 0xFF);            /* B */
            d[1] = (uint8_t)(y * 255 / (h > 1 ? h - 1 : 1));  /* G */
            d[2] = (uint8_t)(x * 255 / (w > 1 ? w - 1 : 1));  /* R */
            d[3] = (uint8_t)((x < w / 2) ? 0 : 255);     /* A */
        }

    FILE *f = img_fopen_write(path);
    int ok = f && fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr) &&
             fwrite(px, 1, (size_t)pix, f) == (size_t)pix;
    if (f) fclose(f);
    free(px);
    return ok ? 0 : -1;
}

static int gen_bmp8_pal(const char *path, int w, int h, int npal)
{
    long row = ((long)w + 3) / 4 * 4;
    long pix = row * h;
    uint8_t hdr[14 + 40 + 256 * 4];
    memset(hdr, 0, sizeof(hdr));
    uint32_t fsz = (uint32_t)(14 + 40 + npal * 4 + pix);
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (uint8_t)fsz; hdr[3] = fsz >> 8; hdr[4] = fsz >> 16; hdr[5] = fsz >> 24;
    uint32_t off = 14 + 40 + (uint32_t)npal * 4;
    hdr[10] = (uint8_t)off; hdr[11] = off >> 8; hdr[12] = off >> 16; hdr[13] = off >> 24;
    uint32_t hsz = 40; memcpy(hdr + 14, &hsz, 4);
    int32_t w32 = w, h32 = h; memcpy(hdr + 18, &w32, 4); memcpy(hdr + 22, &h32, 4);
    uint16_t planes = 1, bpp = 8; memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);
    uint32_t cu = (uint32_t)npal; memcpy(hdr + 46, &cu, 4);
    for (int i = 0; i < npal; i++) {
        uint8_t *e = hdr + 54 + i * 4;
        e[0] = (uint8_t)(i * 255 / (npal > 1 ? npal - 1 : 1));
        e[1] = (uint8_t)(255 - i * 255 / (npal > 1 ? npal - 1 : 1));
        e[2] = (uint8_t)(i * 128 / (npal > 1 ? npal - 1 : 1) + 60);
        e[3] = 0;
    }

    uint8_t *px = (uint8_t *)malloc((size_t)pix);
    memset(px, 0, (size_t)pix);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            px[(size_t)(h - 1 - y) * row + x] = (uint8_t)((x + y) % npal);

    FILE *f = img_fopen_write(path);
    int ok = f && fwrite(hdr, 1, 14 + 40 + (size_t)npal * 4, f) == 14 + 40 + (size_t)npal * 4 &&
             fwrite(px, 1, (size_t)pix, f) == (size_t)pix;
    if (f) fclose(f);
    free(px);
    return ok ? 0 : -1;
}

static int gen_tga32(const char *path, int w, int h, int top_down)
{
    uint8_t hdr[18] = {0};
    hdr[2] = 2;     /* uncompressed truecolor */
    hdr[12] = (uint8_t)(w & 0xFF); hdr[13] = (uint8_t)(w >> 8);
    hdr[14] = (uint8_t)(h & 0xFF); hdr[15] = (uint8_t)(h >> 8);
    hdr[16] = 32;
    hdr[17] = (uint8_t)(top_down ? 0x28 : 0x08);   /* alpha 8 bits; bit5 = top-down */

    FILE *f = img_fopen_write(path);
    int ok = f && fwrite(hdr, 1, 18, f) == 18;
    for (int y = 0; y < h && ok; y++) {
        int iy = top_down ? y : h - 1 - y;   /* image row this file row holds */
        for (int x = 0; x < w && ok; x++) {
            uint8_t px[4] = { (uint8_t)((x ^ iy) & 0xFF),               /* B */
                              (uint8_t)(iy * 255 / (h > 1 ? h - 1 : 1)),/* G */
                              (uint8_t)(x * 255 / (w > 1 ? w - 1 : 1)), /* R */
                              (uint8_t)((x + iy) & 1 ? 128 : 255) };    /* A */
            ok = fwrite(px, 1, 4, f) == 4;
        }
    }
    if (f) fclose(f);
    return ok ? 0 : -1;
}

static int gen_pnm(const char *path, int w, int h, int type, unsigned maxval)
{
    FILE *f = img_fopen_write(path);
    if (!f)
        return -1;
    int ok = 0;
    if (type == 6) {
        ok = fprintf(f, "P6\n%d %d\n%u\n", w, h, maxval) > 0;
        int wide = maxval > 255;
        for (int y = 0; y < h && ok; y++)
            for (int x = 0; x < w && ok; x++) {
                uint8_t rgb[3] = { (uint8_t)(x * 255 / (w - 1)),
                                   (uint8_t)(y * 255 / (h - 1)),
                                   (uint8_t)((x ^ y) & 0xFF) };
                for (int c = 0; c < 3; c++) {
                    unsigned s = (unsigned)rgb[c] * maxval / 255u;
                    if (wide) {
                        uint8_t b2[2] = { (uint8_t)(s >> 8), (uint8_t)s };
                        ok = fwrite(b2, 1, 2, f) == 2;
                    } else {
                        uint8_t b1 = (uint8_t)s;
                        ok = fwrite(&b1, 1, 1, f) == 1;
                    }
                }
            }
    } else if (type == 5) {
        ok = fprintf(f, "P5\n%d %d\n%u\n", w, h, maxval) > 0;
        int wide = maxval > 255;
        for (int y = 0; y < h && ok; y++)
            for (int x = 0; x < w && ok; x++) {
                unsigned g = (unsigned)((x * 7 + y * 13) & 0xFF) * maxval / 255u;
                if (wide) {
                    uint8_t b2[2] = { (uint8_t)(g >> 8), (uint8_t)g };
                    ok = fwrite(b2, 1, 2, f) == 2;
                } else {
                    uint8_t b1 = (uint8_t)g;
                    ok = fwrite(&b1, 1, 1, f) == 1;
                }
            }
    }
    fclose(f);
    return ok ? 0 : -1;
}

/* --- verification helpers --- */

/* read PNG back with libpng into 8-bit RGBA (or GRAY->RGBA) for comparison */
static int read_png_rgba(const char *path, int *w, int *h, uint8_t **out)
{
    FILE *f = img_fopen_read(path);
    if (!f)
        return -1;
    png_err_t pe;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &pe, pngerr_error, pngerr_warn);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info) { if (f) fclose(f); return -1; }
    if (setjmp(pe.jb)) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(f);
        return -1;
    }
    png_init_io(png, f);
    png_read_info(png, info);
    png_set_expand(png);            /* palette->rgb, gray<8->8, tRNS->alpha */
    png_set_strip_16(png);          /* 16-bit -> 8-bit (comparison only) */
    png_set_packing(png);
    png_read_update_info(png, info);
    *w = (int)png_get_image_width(png, info);
    *h = (int)png_get_image_height(png, info);
    int ch = png_get_channels(png, info);
    size_t stride = (size_t)*w * ch;
    uint8_t *data = (uint8_t *)malloc(stride * (size_t)*h);
    png_bytep *rows = (png_bytep *)malloc(sizeof(png_bytep) * (size_t)*h);
    for (int y = 0; y < *h; y++)
        rows[y] = data + (size_t)y * stride;
    png_read_image(png, rows);
    png_read_end(png, NULL);
    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(f);
    *out = data;
    return ch;
}

/* convert an 8-bit gradient RGBA buffer into the same expanded form */
static void expected_rgba(int w, int h, int alpha, uint8_t *out)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *d = out + ((size_t)y * w + x) * 4;
            d[0] = (uint8_t)(x * 255 / (w - 1));
            d[1] = (uint8_t)(y * 255 / (h - 1));
            d[2] = (uint8_t)((x ^ y) & 0xFF);
            d[3] = (uint8_t)alpha;
        }
}

/* ---------------------------------------------------------------------------
 * Native-depth PNG reader: unlike read_png_rgba() this applies no transforms,
 * so it reports the color type / bit depth actually stored in the file and
 * hands back rows at their native packing.  Used to prove that bit-depth
 * matching preserved the source depth instead of silently normalizing it.
 * ------------------------------------------------------------------------ */
typedef struct {
    int width, height, bit_depth, color_type, channels;
    size_t rowstride;
    uint8_t *data;
    int ncolors;
    uint8_t palette[256 * 3];
    int has_pal_trns;
    uint8_t pal_alpha[256];
    int gray_trns_valid;
    unsigned gray_trns;
} raw_png_t;

static int read_png_native(const char *path, raw_png_t *out)
{
    memset(out, 0, sizeof(*out));
    FILE *f = img_fopen_read(path);
    if (!f)
        return -1;
    png_err_t pe;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &pe, pngerr_error, pngerr_warn);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info) { if (f) fclose(f); return -1; }
    if (setjmp(pe.jb)) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(f);
        free(out->data);
        out->data = NULL;
        return -1;
    }
    png_init_io(png, f);
    png_read_info(png, info);

    out->width = (int)png_get_image_width(png, info);
    out->height = (int)png_get_image_height(png, info);
    out->bit_depth = png_get_bit_depth(png, info);
    out->color_type = png_get_color_type(png, info);
    out->channels = png_get_channels(png, info);

    if (out->color_type == PNG_COLOR_TYPE_PALETTE) {
        png_colorp plte = NULL;
        int n = 0;
        png_get_PLTE(png, info, &plte, &n);
        out->ncolors = n;
        for (int i = 0; i < n && i < 256; i++) {
            out->palette[i * 3 + 0] = plte[i].red;
            out->palette[i * 3 + 1] = plte[i].green;
            out->palette[i * 3 + 2] = plte[i].blue;
        }
        for (int i = 0; i < 256; i++)
            out->pal_alpha[i] = 255;
        png_bytep trans = NULL;
        int ntrans = 0;
        if (png_get_tRNS(png, info, &trans, &ntrans, NULL) && ntrans > 0) {
            out->has_pal_trns = 1;
            for (int i = 0; i < ntrans && i < 256; i++)
                out->pal_alpha[i] = trans[i];
        }
    } else {
        png_color_16p tc = NULL;
        int ntrans = 0;
        if (png_get_tRNS(png, info, NULL, &ntrans, &tc) && ntrans > 0 && tc) {
            out->gray_trns_valid = 1;
            out->gray_trns = tc->gray;
        }
    }

    out->rowstride = png_get_rowbytes(png, info);
    out->data = (uint8_t *)malloc(out->rowstride * (size_t)out->height);
    png_bytep *rows = (png_bytep *)malloc(sizeof(png_bytep) * (size_t)out->height);
    if (!out->data || !rows) {
        free(rows);
        free(out->data);
        out->data = NULL;
        png_destroy_read_struct(&png, &info, NULL);
        fclose(f);
        return -1;
    }
    for (int y = 0; y < out->height; y++)
        rows[y] = out->data + (size_t)y * out->rowstride;
    png_read_image(png, rows);
    png_read_end(png, NULL);
    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(f);
    return 0;
}

static void raw_png_free(raw_png_t *p)
{
    free(p->data);
    p->data = NULL;
}

/* one sample read at the file's native packing (channel 0 for sub-byte) */
static unsigned raw_sample(const raw_png_t *p, int x, int y, int ch)
{
    const uint8_t *row = p->data + (size_t)y * p->rowstride;
    if (p->bit_depth == 16)
        return img_ld16be(row + ((size_t)x * (size_t)p->channels + (size_t)ch) * 2);
    if (p->bit_depth == 8)
        return row[(size_t)x * (size_t)p->channels + (size_t)ch];
    return img_ld_bits(row, x, p->bit_depth);
}

/* pack `n` sample values MSB-first at `bits` per sample into dst (zeroed) */
static void pack_samples(uint8_t *dst, const uint8_t *vals, int n, int bits)
{
    memset(dst, 0, (size_t)(((size_t)n * (size_t)bits + 7) / 8));
    for (int i = 0; i < n; i++)
        img_st_bits(dst, i, bits, vals[i]);
}

/* store / load one sample in an img_image_t at its native packing */
static void put_sample(img_image_t *im, int x, int y, int c, unsigned v)
{
    uint8_t *row = im->data + (size_t)y * im->rowstride;
    int ch = img_channels(im->color);
    if (im->bit_depth == 16)
        img_st16be(row + ((size_t)x * (size_t)ch + (size_t)c) * 2, v);
    else if (im->bit_depth == 8)
        row[(size_t)x * (size_t)ch + (size_t)c] = (uint8_t)v;
    else
        img_st_bits(row, x, im->bit_depth, v);
}

static unsigned get_sample(const img_image_t *im, int x, int y, int c)
{
    const uint8_t *row = im->data + (size_t)y * im->rowstride;
    int ch = img_channels(im->color);
    if (im->bit_depth == 16)
        return img_ld16be(row + ((size_t)x * (size_t)ch + (size_t)c) * 2);
    if (im->bit_depth == 8)
        return row[(size_t)x * (size_t)ch + (size_t)c];
    return img_ld_bits(row, x, im->bit_depth);
}

/* largest sample value representable at a depth */
static unsigned depth_max(int bits) { return bits == 16 ? 65535u : ((1u << bits) - 1u); }

/* --- generators for the bit-depth regression tests --- */

/* BMP with a 1/4-bit palette; index at (x,y) is (x + y) % npal */
static int bmp_sub_index(int x, int y, int npal) { return (x + y) % npal; }

static int gen_bmp_subbyte(const char *path, int w, int h, int bpp, int npal)
{
    long row = ((long)w * bpp + 31) / 32 * 4;
    long pix = row * h;
    long palbytes = (long)npal * 4;
    uint8_t *px = (uint8_t *)calloc(1, (size_t)pix);
    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    uint32_t fsz = (uint32_t)(54 + palbytes + pix);
    hdr[2] = (uint8_t)fsz; hdr[3] = fsz >> 8; hdr[4] = fsz >> 16; hdr[5] = fsz >> 24;
    uint32_t off = (uint32_t)(54 + palbytes);
    memcpy(hdr + 10, &off, 4);
    uint32_t hsz = 40; memcpy(hdr + 14, &hsz, 4);
    int32_t w32 = w, h32 = h; memcpy(hdr + 18, &w32, 4); memcpy(hdr + 22, &h32, 4);
    uint16_t planes = 1, b16 = (uint16_t)bpp;
    memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &b16, 2);
    uint32_t clrused = (uint32_t)npal; memcpy(hdr + 46, &clrused, 4);

    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            img_st_bits(px + (size_t)(h - 1 - y) * row, x, bpp,
                        (unsigned)bmp_sub_index(x, y, npal));

    FILE *f = img_fopen_write(path);
    int ok = 0;
    if (f) {
        uint8_t pal[256 * 4];
        for (int i = 0; i < npal; i++) {
            pal[i * 4 + 0] = (uint8_t)(i * 5);      /* B */
            pal[i * 4 + 1] = (uint8_t)(i * 3);      /* G */
            pal[i * 4 + 2] = (uint8_t)(i * 7);      /* R */
            pal[i * 4 + 3] = 0;
        }
        ok = fwrite(hdr, 1, 54, f) == 54 &&
             fwrite(pal, 1, (size_t)palbytes, f) == (size_t)palbytes &&
             fwrite(px, 1, (size_t)pix, f) == (size_t)pix;
        fclose(f);
    }
    free(px);
    return ok ? 0 : -1;
}

/* palette entries used by the ICO generator */
static uint8_t ico_pal_r(int i) { return (uint8_t)(i * 7); }
static uint8_t ico_pal_g(int i) { return (uint8_t)(i * 3); }
static uint8_t ico_pal_b(int i) { return (uint8_t)(i * 5); }

/* Build a one-entry .ico holding a DIB.  bpp: 1/4/8/24/32.  `with_mask` writes
 * the 1-bit AND mask (left half transparent); `real_alpha` gives 32-bit
 * entries a genuine alpha channel (same left-half pattern). */
static int gen_ico_dib(const char *path, int w, int h, int bpp,
                       int with_mask, int real_alpha)
{
    int npal = (bpp <= 8) ? (1 << bpp) : 0;
    long palbytes = (long)npal * 4;
    long xor_row = ((long)w * bpp + 31) / 32 * 4;
    long and_row = ((long)w + 31) / 32 * 4;
    long dib = 40 + palbytes + xor_row * h + and_row * h;
    long total = 22 + dib;

    uint8_t *buf = (uint8_t *)calloc(1, (size_t)total);
    uint8_t *p = buf + 22;
    uint16_t one = 1, cnt = 1, b16 = (uint16_t)bpp;
    memcpy(buf + 2, &one, 2);                       /* ICONDIR.idType   */
    memcpy(buf + 4, &cnt, 2);                       /* ICONDIR.idCount  */
    buf[6] = (uint8_t)w;                            /* entry bWidth     */
    buf[7] = (uint8_t)h;                            /* entry bHeight    */
    memcpy(buf + 10, &one, 2);                      /* wPlanes          */
    memcpy(buf + 12, &b16, 2);                      /* wBitCount        */
    uint32_t dsz = (uint32_t)dib; memcpy(buf + 14, &dsz, 4);
    uint32_t off = 22; memcpy(buf + 18, &off, 4);

    uint32_t hsz = 40; memcpy(p + 0, &hsz, 4);
    int32_t w32 = w, h2 = h * 2;
    memcpy(p + 4, &w32, 4); memcpy(p + 8, &h2, 4);
    memcpy(p + 12, &one, 2);                        /* biPlanes         */
    memcpy(p + 14, &b16, 2);                        /* biBitCount       */
    uint32_t clrused = (uint32_t)npal; memcpy(p + 32, &clrused, 4);

    uint8_t *pal = p + 40;
    for (int i = 0; i < npal; i++) {
        pal[i * 4 + 0] = ico_pal_b(i);
        pal[i * 4 + 1] = ico_pal_g(i);
        pal[i * 4 + 2] = ico_pal_r(i);
        pal[i * 4 + 3] = 0;
    }
    uint8_t *xor = pal + palbytes;
    uint8_t *andm = xor + xor_row * h;

    for (int y = 0; y < h; y++) {
        uint8_t *xr = xor + (size_t)y * xor_row;    /* DIB rows are bottom-up */
        int ly = h - 1 - y;
        for (int x = 0; x < w; x++) {
            if (bpp <= 8) {
                img_st_bits(xr, x, bpp,
                            (unsigned)((x + ly) % (npal > 0 ? npal : 1)));
            } else {
                int step = bpp / 8;
                xr[x * step + 0] = (uint8_t)(x * 3 + ly);   /* B */
                xr[x * step + 1] = (uint8_t)(x + ly);       /* G */
                xr[x * step + 2] = (uint8_t)(x * 7 + ly);   /* R */
                if (bpp == 32)
                    xr[x * 4 + 3] = real_alpha
                        ? (uint8_t)((x < w / 2) ? 0 : 255)
                        : 0;
            }
        }
        if (with_mask) {
            uint8_t *ar = andm + (size_t)y * and_row;
            for (int x = 0; x < w / 2; x++)
                ar[x >> 3] |= (uint8_t)(0x80u >> (x & 7));
        }
    }

    FILE *f = img_fopen_write(path);
    int ok = f && fwrite(buf, 1, (size_t)total, f) == (size_t)total;
    if (f) fclose(f);
    free(buf);
    return ok ? 0 : -1;
}

/* Build a one-entry .ico whose entry payload is a PNG file (the PNG-in-ICO
 * container form), exercising png_decode_mem through the ICO decoder. */
static int gen_ico_png(const char *path, const char *png_path)
{
    FILE *pf = img_fopen_read(png_path);
    if (!pf)
        return -1;
    fseek(pf, 0, SEEK_END);
    long n = ftell(pf);
    rewind(pf);
    uint8_t *blob = (uint8_t *)malloc((size_t)n);
    if (!blob || fread(blob, 1, (size_t)n, pf) != (size_t)n) {
        if (pf) fclose(pf);
        free(blob);
        return -1;
    }
    fclose(pf);

    uint8_t hdr[22];
    memset(hdr, 0, sizeof(hdr));
    uint16_t one = 1, cnt = 1;
    memcpy(hdr + 2, &one, 2);
    memcpy(hdr + 4, &cnt, 2);
    hdr[6] = 0;                                     /* 0 = 256 */
    hdr[7] = 0;
    memcpy(hdr + 10, &one, 2);
    uint16_t b16 = 32; memcpy(hdr + 12, &b16, 2);
    uint32_t sz = (uint32_t)n; memcpy(hdr + 14, &sz, 4);
    uint32_t off = 22; memcpy(hdr + 18, &off, 4);

    FILE *f = img_fopen_write(path);
    int ok = f && fwrite(hdr, 1, 22, f) == 22 &&
             fwrite(blob, 1, (size_t)n, f) == (size_t)n;
    if (f) fclose(f);
    free(blob);
    return ok ? 0 : -1;
}

/* Sub-byte grayscale TIFF (1/2/4 bits) written through libtiff */
static int tiff_write_subbyte_gray(const char *path, int w, int h, int bits,
                                   const uint8_t *vals, int photometric)
{
    TIFF *tf = TIFFOpen(path, "w");
    if (!tf)
        return -1;
    TIFFSetField(tf, TIFFTAG_IMAGEWIDTH, (uint32_t)w);
    TIFFSetField(tf, TIFFTAG_IMAGELENGTH, (uint32_t)h);
    TIFFSetField(tf, TIFFTAG_BITSPERSAMPLE, (uint16_t)bits);
    TIFFSetField(tf, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tf, TIFFTAG_PHOTOMETRIC, (uint16_t)photometric);
    TIFFSetField(tf, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    uint8_t row[256];
    int rc = 0;
    for (int y = 0; y < h; y++) {
        pack_samples(row, vals + (size_t)y * w, w, bits);
        if (TIFFWriteScanline(tf, row, (uint32_t)y, 0) < 0) { rc = -1; break; }
    }
    TIFFClose(tf);
    return rc;
}

/* Sub-byte palette TIFF (1/2/4 bits).  Entry i decodes to 8-bit R=17i,
 * G=255-17i, B=17i. */
static int tiff_write_subbyte_palette(const char *path, int w, int h, int bits,
                                      const uint8_t *vals)
{
    int nc = 1 << bits;
    uint16_t rmap[16], gmap[16], bmap[16];
    for (int i = 0; i < nc; i++) {
        rmap[i] = (uint16_t)(i * 65535 / (nc - 1));
        gmap[i] = (uint16_t)(65535 - i * 65535 / (nc - 1));
        bmap[i] = (uint16_t)(i * 65535 / (nc - 1));
    }
    TIFF *tf = TIFFOpen(path, "w");
    if (!tf)
        return -1;
    TIFFSetField(tf, TIFFTAG_IMAGEWIDTH, (uint32_t)w);
    TIFFSetField(tf, TIFFTAG_IMAGELENGTH, (uint32_t)h);
    TIFFSetField(tf, TIFFTAG_BITSPERSAMPLE, (uint16_t)bits);
    TIFFSetField(tf, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tf, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_PALETTE);
    TIFFSetField(tf, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tf, TIFFTAG_COLORMAP, rmap, gmap, bmap);
    uint8_t row[256];
    int rc = 0;
    for (int y = 0; y < h; y++) {
        pack_samples(row, vals + (size_t)y * w, w, bits);
        if (TIFFWriteScanline(tf, row, (uint32_t)y, 0) < 0) { rc = -1; break; }
    }
    TIFFClose(tf);
    return rc;
}

/* PBM (ASCII "P1" / binary "P4"): sample written is 1 for black */
static int gen_pbm(const char *path, int w, int h, int ascii)
{
    FILE *f = img_fopen_write(path);
    if (!f)
        return -1;
    int ok = fprintf(f, ascii ? "P1\n%d %d\n" : "P4\n%d %d\n", w, h) > 0;
    for (int y = 0; y < h && ok; y++) {
        for (int x = 0; x < w && ok; x++) {
            int black = ((x + y) & 1);
            if (ascii)
                ok = fprintf(f, "%d ", black) > 0;
        }
        if (!ascii && ok) {
            /* binary rows are packed 1 bit per pixel, (w+7)/8 bytes each */
            uint8_t row[512];
            int row_bytes = (w + 7) / 8;
            memset(row, 0, sizeof(row));
            for (int x = 0; x < w; x++)
                if ((x + y) & 1)
                    img_st_bits(row, x, 1, 1);
            ok = fwrite(row, 1, (size_t)row_bytes, f) == (size_t)row_bytes;
        }
    }
    if (ascii && ok)
        ok = fputc('\n', f) != EOF;
    fclose(f);
    return ok ? 0 : -1;
}

/* ---------------------------------------------------------------------------
 * Metadata test helpers
 * ------------------------------------------------------------------------ */

/* A minimal but valid EXIF payload: TIFF header + IFD0 carrying Make,
 * X/YResolution (300 dpi) and ResolutionUnit. */
static size_t make_exif(uint8_t *buf, size_t cap)
{
    static const char make[] = "TESTCAM";
    const unsigned nent = 4;
    size_t make_off = 8 + 2 + nent * 12 + 4;
    size_t res_off = make_off + sizeof(make);
    size_t need = res_off + 16;
    if (need > cap)
        return 0;
    memset(buf, 0, need);
    buf[0] = 'I'; buf[1] = 'I'; buf[2] = 42; buf[3] = 0;
#define W16(o, v) do { buf[o] = (uint8_t)(v); buf[o + 1] = (uint8_t)((v) >> 8); } while (0)
#define W32(o, v) do { buf[o] = (uint8_t)(v); buf[o + 1] = (uint8_t)((v) >> 8); \
                       buf[o + 2] = (uint8_t)((v) >> 16); buf[o + 3] = (uint8_t)((v) >> 24); } while (0)
    W32(4, 8);
    W16(8, nent);
    size_t e = 10;
    W16(e, 271); W16(e + 2, 2); W32(e + 4, (unsigned)sizeof(make));
    W32(e + 8, (unsigned)make_off); e += 12;
    W16(e, 282); W16(e + 2, 5); W32(e + 4, 1); W32(e + 8, (unsigned)res_off); e += 12;
    W16(e, 283); W16(e + 2, 5); W32(e + 4, 1); W32(e + 8, (unsigned)(res_off + 8)); e += 12;
    W16(e, 296); W16(e + 2, 3); W32(e + 4, 1); W16(e + 8, 2); e += 12;
    W32(e, 0);
    memcpy(buf + make_off, make, sizeof(make));
    W32(res_off, 300); W32(res_off + 4, 1);
    W32(res_off + 8, 300); W32(res_off + 12, 1);
#undef W16
#undef W32
    return need;
}

/* A valid ICC profile that libpng's iCCP validation accepts: the mandatory
 * length / 'acsp' signature / 'RGB ' colour space header fields, no tags, and
 * an incompressible body.  The body matters - libpng's iCCP reader rejects a
 * chunk shorter than 92 bytes outright ("too short"), so a tiny degenerate
 * profile would never round-trip.  `tag4` just identifies the profile. */
#define TEST_ICC_SIZE 2048

static void make_icc(uint8_t *icc, size_t len, const char *tag4)
{
    memset(icc, 0, len);
    icc[0] = 0; icc[1] = 0;
    icc[2] = (uint8_t)(len >> 8); icc[3] = (uint8_t)len;
    icc[8] = 2;                                     /* version major */
    memcpy(icc + 12, "mntr", 4);                    /* display device class */
    memcpy(icc + 16, "RGB ", 4);                    /* data colour space */
    memcpy(icc + 20, "XYZ ", 4);                    /* PCS */
    memcpy(icc + 36, "acsp", 4);                    /* signature */
    icc[68] = 0x00; icc[69] = 0x00; icc[70] = 0xF6; icc[71] = 0xD6;
    icc[72] = 0x00; icc[73] = 0x01; icc[74] = 0x00; icc[75] = 0x00;
    icc[76] = 0x00; icc[77] = 0x00; icc[78] = 0xD3; icc[79] = 0x2D;  /* D50 */
    /* tag count at 128 stays 0; fill the rest so it cannot compress away */
    unsigned x = 12345;
    for (size_t i = 132; i < len; i++) {
        x = x * 1103515245u + 12345u;
        icc[i] = (uint8_t)(x >> 16);
    }
    if (tag4)
        memcpy(icc + 4, tag4, 4);
}

/* substring search over binary data (EXIF payloads are full of NULs) */
static int contains_bytes(const uint8_t *hay, size_t hlen, const char *needle)
{
    size_t nl = strlen(needle);
    if (!nl || nl > hlen)
        return 0;
    for (size_t i = 0; i + nl <= hlen; i++)
        if (!memcmp(hay + i, needle, nl))
            return 1;
    return 0;
}

/* what a PNG file carries besides pixels */
typedef struct {
    int      have_phys;
    unsigned ppm_x, ppm_y;
    size_t   exif_len;
    size_t   icc_len;
    int      ntext;
    char     keys[8][40];
} png_meta_t;

static int read_png_meta(const char *path, png_meta_t *out)
{
    memset(out, 0, sizeof(*out));
    FILE *f = img_fopen_read(path);
    if (!f)
        return -1;
    png_err_t pe;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &pe,
                                             pngerr_error, pngerr_warn);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info) { if (f) fclose(f); return -1; }
    if (setjmp(pe.jb)) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(f);
        return -1;
    }
    png_init_io(png, f);
    png_read_info(png, info);

    png_uint_32 px = 0, py = 0;
    int unit = 0;
    if (png_get_pHYs(png, info, &px, &py, &unit) &&
        unit == PNG_RESOLUTION_METER) {
        out->have_phys = 1;
        out->ppm_x = px;
        out->ppm_y = py;
    }
    png_bytep ex = NULL;
    png_uint_32 exl = 0;
    if (png_get_eXIf_1(png, info, &exl, &ex) && ex && exl)
        out->exif_len = exl;
    png_charp nm = NULL;
    png_bytep icc = NULL;
    png_uint_32 iccl = 0;
    int comp = 0;
    if (png_get_iCCP(png, info, &nm, &comp, &icc, &iccl) && icc)
        out->icc_len = iccl;
    png_textp t = NULL;
    int nt = 0;
    if (png_get_text(png, info, &t, &nt) > 0) {
        for (int i = 0; i < nt && out->ntext < 8; i++) {
            if (!t[i].key)
                continue;
            snprintf(out->keys[out->ntext], sizeof(out->keys[0]), "%s", t[i].key);
            out->ntext++;
        }
    }
    png_destroy_read_struct(&png, &info, NULL);
    fclose(f);
    return 0;
}

static int meta_has_key(const png_meta_t *m, const char *key)
{
    for (int i = 0; i < m->ntext; i++)
        if (!strcmp(m->keys[i], key))
            return 1;
    return 0;
}

/* A small JPEG carrying JFIF density, EXIF, a two-part ICC profile, XMP and a
 * comment - everything the decoder is expected to pick up. */
static int gen_jpeg_meta(const char *path, const uint8_t *exif, size_t exif_len,
                         const uint8_t *icc, size_t icc_len, const char *xmp)
{
    FILE *f = img_fopen_write(path);
    if (!f)
        return -1;
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, f);
    cinfo.image_width = 8;
    cinfo.image_height = 8;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 90, TRUE);
    cinfo.density_unit = 1;         /* dots per inch */
    cinfo.X_density = 300;          /* libjpeg writes these into the JFIF APP0 */
    cinfo.Y_density = 300;
    jpeg_start_compress(&cinfo, TRUE);

    if (exif && exif_len) {
        uint8_t *b = (uint8_t *)malloc(exif_len + 6);
        memcpy(b, "Exif\0\0", 6);
        memcpy(b + 6, exif, exif_len);
        jpeg_write_marker(&cinfo, JPEG_APP0 + 1, b, (unsigned)(exif_len + 6));
        free(b);
    }
    if (icc && icc_len) {           /* split in two to exercise reassembly */
        size_t half = icc_len / 2;
        for (int part = 0; part < 2; part++) {
            size_t off = part ? half : 0;
            size_t len = part ? icc_len - half : half;
            uint8_t *b = (uint8_t *)malloc(14 + len);
            memcpy(b, "ICC_PROFILE\0", 12);
            b[12] = (uint8_t)(part + 1);
            b[13] = 2;
            memcpy(b + 14, icc + off, len);
            jpeg_write_marker(&cinfo, JPEG_APP0 + 2, b, (unsigned)(14 + len));
            free(b);
        }
    }
    if (xmp) {
        static const char hdr[] = "http://ns.adobe.com/xap/1.0/";
        size_t hl = sizeof(hdr);
        size_t xl = strlen(xmp);
        uint8_t *b = (uint8_t *)malloc(hl + xl);
        memcpy(b, hdr, hl);
        memcpy(b + hl, xmp, xl);
        jpeg_write_marker(&cinfo, JPEG_APP0 + 1, b, (unsigned)(hl + xl));
        free(b);
    }
    jpeg_write_marker(&cinfo, JPEG_COM, (const JOCTET *)"hello comment", 13);

    uint8_t row[8 * 3];
    while (cinfo.next_scanline < cinfo.image_height) {
        for (int x = 0; x < 8; x++) {
            row[x * 3 + 0] = (uint8_t)(x * 30);
            row[x * 3 + 1] = (uint8_t)(cinfo.next_scanline * 30);
            row[x * 3 + 2] = 90;
        }
        JSAMPROW rp = row;
        jpeg_write_scanlines(&cinfo, &rp, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(f);
    return 0;
}

/* Wrap an encoded VP8L bitstream in a container that also holds ICC/EXIF/XMP,
 * which is how WebP carries metadata. */
static int gen_webp_meta(const char *path, const uint8_t *icc, size_t icc_len,
                         const uint8_t *exif, size_t exif_len, const char *xmp)
{
    int w = 0, h = 0;
    uint8_t src[8 * 8 * 3];
    for (int i = 0; i < 8 * 8; i++) {
        src[i * 3 + 0] = (uint8_t)(i * 3);
        src[i * 3 + 1] = (uint8_t)(i * 5);
        src[i * 3 + 2] = 7;
    }
    uint8_t *vp8l = NULL;
    size_t vp8l_total = WebPEncodeLosslessRGB(src, 8, 8, 8 * 3, &vp8l);
    /* WebPEncodeLosslessRGB returns a whole RIFF file: keep just the VP8L
     * chunk payload (12 byte RIFF/WEBP header + 8 byte chunk header) */
    if (!vp8l || vp8l_total < 21)
        return -1;
    size_t vp8l_len = vp8l_total - 20;
    /* re-read the canvas size from the VP8L header (14 bits each, +1) */
    const uint8_t *vh = vp8l + 20;
    unsigned bits = (unsigned)vh[1] | ((unsigned)vh[2] << 8) |
                    ((unsigned)vh[3] << 16) | ((unsigned)vh[4] << 24);
    w = (int)(bits & 0x3FFF) + 1;
    h = (int)((bits >> 14) & 0x3FFF) + 1;

    size_t xmp_len = xmp ? strlen(xmp) : 0;
    /* VP8X payload: flags, reserved, canvas size, each chunk padded to even */
    size_t body = 8 + 10;                                   /* VP8X */
    if (icc_len) body += 8 + icc_len + (icc_len & 1);
    if (exif_len) body += 8 + exif_len + (exif_len & 1);
    if (xmp_len) body += 8 + xmp_len + (xmp_len & 1);
    body += 8 + vp8l_len + (vp8l_len & 1);

    uint8_t *out = (uint8_t *)calloc(1, body + 12);
    if (!out) { WebPFree(vp8l); return -1; }
    uint8_t *p = out;
    memcpy(p, "RIFF", 4);
    uint32_t riff = (uint32_t)(body + 4);
    memcpy(p + 4, &riff, 4);
    memcpy(p + 8, "WEBP", 4);
    p += 12;
    memcpy(p, "VP8X", 4);
    uint32_t ten = 10;
    memcpy(p + 4, &ten, 4);
    p[8] = (uint8_t)((icc_len ? 0x20 : 0) | (exif_len ? 0x08 : 0) |
                     (xmp_len ? 0x04 : 0));
    uint32_t cw = (uint32_t)(w - 1) | ((uint32_t)(h - 1) << 24);
    memcpy(p + 12, &cw, 4);
    p += 18;
#define PUT_CHUNK(id, data, len)                                              \
    do {                                                                      \
        memcpy(p, id, 4);                                                      \
        uint32_t l_ = (uint32_t)(len);                                         \
        memcpy(p + 4, &l_, 4);                                                 \
        if (len) memcpy(p + 8, data, len);                                      \
        p += 8 + (len) + ((len) & 1);                                          \
    } while (0)
    if (icc_len)  PUT_CHUNK("ICCP", icc, icc_len);
    if (exif_len) PUT_CHUNK("EXIF", exif, exif_len);
    if (xmp_len)  PUT_CHUNK("XMP ", xmp, xmp_len);
    PUT_CHUNK("VP8L", vp8l + 20, vp8l_len);
#undef PUT_CHUNK

    FILE *f = img_fopen_write(path);
    int ok = f && fwrite(out, 1, body + 12, f) == body + 12;
    if (f) fclose(f);
    free(out);
    WebPFree(vp8l);
    return ok ? 0 : -1;
}

static int compare_rgba(const uint8_t *a, const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++)
        if (a[i] != b[i])
            return i;
    return -1;
}

static uint8_t pal_gif_r(int i) { static const uint8_t v[4] = {255,0,0,10};  return v[i]; }
static uint8_t pal_gif_g(int i) { static const uint8_t v[4] = {0,255,0,20};  return v[i]; }
static uint8_t pal_gif_b(int i) { static const uint8_t v[4] = {0,0,255,30};  return v[i]; }

static uint64_t file_time_stamp(const char *path, int create)
{
    uint8_t c[16] = {0}, m[16] = {0};
    if (get_file_times(path, c, m) != 0)
        return 0;
    if (create) {
        uint64_t v;
        memcpy(&v, c, 8);
        return v;
    }
    uint64_t v;
    memcpy(&v, m, 8);
    return v;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    /* usage: img2png_selftest [file.png] — just verify the file decodes */
    if (argc > 1) {
        int w, h;
        uint8_t *got = NULL;
        int ch = read_png_rgba(argv[1], &w, &h, &got);
        if (ch < 0) {
            printf("FAIL: cannot decode %s\n", argv[1]);
            return 1;
        }
        printf("OK: %s decodes (%dx%d, %d channels)\n", argv[1], w, h, ch);
        free(got);
        return 0;
    }

    printf("img2png self-test\n");
#ifdef _WIN32
    _mkdir("testout");
#else
    mkdir("testout", 0755);
#endif

    png_opts_t opts;
    opts.level = 9;
    opts.filter = PNGF_AUTO;
    opts.auto_optimize = 0;

    int W = 33, H = 17;     /* odd sizes to exercise padding paths */
    char err[256];

    /* --- BMP 24-bit --- */
    printf("BMP 24-bit -> RGB8:\n");
    CHECK(gen_bmp24("testout/t24.bmp", W, H) == 0, "generate bmp24");
    {
        FILE *f = img_fopen_read("testout/t24.bmp");
        img_image_t img;
        int ok24 = f && bmp_decode(f, &img, err, sizeof(err)) == 0;
        CHECK(ok24, "decode bmp24");
        if (f) fclose(f);
        if (!ok24) { printf("  SKIP  remaining bmp24 checks\n"); goto bmp24_done; }
        CHECK(img.color == IMG_RGB && img.bit_depth == 8, "format is RGB8");
        CHECK(png_write_file(&img, &opts, "testout/t24.png", err, sizeof(err)) == 0, "encode png");
        img_free(&img);
        {
            int w, h; uint8_t *got = NULL;
            int ch = read_png_rgba("testout/t24.png", &w, &h, &got);
            CHECK(ch == 3 && w == W && h == H, "png readable, RGB, correct size");
            if (ch == 3) {
                uint8_t *exp = (uint8_t *)malloc((size_t)W * H * 3);
                fill_gradient_rgb(exp, W, H);
                int diff = compare_rgba(got, exp, W * H * 3);
                CHECK(diff < 0, "pixels identical (lossless)");
                free(exp);
            }
            free(got);
        }
bmp24_done:;
    }

    /* --- BMP 32-bit with alpha --- */
    printf("BMP 32-bit (V4, alpha mask) -> RGBA8:\n");
    CHECK(gen_bmp32_alpha("testout/t32.bmp", W, H) == 0, "generate bmp32");
    {
        FILE *f = img_fopen_read("testout/t32.bmp");
        img_image_t img;
        int ok32 = f && bmp_decode(f, &img, err, sizeof(err)) == 0;
        CHECK(ok32, "decode bmp32");
        if (f) fclose(f);
        if (!ok32) { printf("  SKIP  remaining bmp32 checks\n"); goto bmp32_done; }
        CHECK(img.color == IMG_RGBA && img.bit_depth == 8, "format is RGBA8");
        CHECK(png_write_file(&img, &opts, "testout/t32.png", err, sizeof(err)) == 0, "encode png");
        img_free(&img);
        {
            int w, h; uint8_t *got = NULL;
            int ch = read_png_rgba("testout/t32.png", &w, &h, &got);
            CHECK(ch == 4, "png readable as RGBA");
            if (ch == 4) {
                uint8_t *exp = (uint8_t *)malloc((size_t)W * H * 4);
                expected_rgba(W, H, 255, exp);
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < W / 2; x++)
                        exp[((size_t)y * W + x) * 4 + 3] = 0;
                int diff = compare_rgba(got, exp, W * H * 4);
                CHECK(diff < 0, "pixels identical (lossless, incl. alpha)");
                free(exp);
            }
            free(got);
        }
bmp32_done:;
    }

    /* --- BMP 8-bit palette --- */
    printf("BMP 8-bit palette -> PALETTE8:\n");
    CHECK(gen_bmp8_pal("testout/t8.bmp", W, H, 200) == 0, "generate bmp8");
    {
        FILE *f = img_fopen_read("testout/t8.bmp");
        img_image_t img;
        int ok8 = f && bmp_decode(f, &img, err, sizeof(err)) == 0;
        CHECK(ok8, "decode bmp8");
        if (f) fclose(f);
        if (!ok8) { printf("  SKIP  remaining bmp8 checks\n"); goto bmp8_done; }
        CHECK(img.color == IMG_PALETTE && img.bit_depth == 8 && img.pal_ncolors == 200,
              "format is PALETTE8, 200 colors");
        CHECK(png_write_file(&img, &opts, "testout/t8.png", err, sizeof(err)) == 0, "encode png");
        img_free(&img);
        {
            int w, h; uint8_t *got = NULL;
            int ch = read_png_rgba("testout/t8.png", &w, &h, &got);
            CHECK(ch == 3, "png readable as RGB (palette expanded)");
            free(got);
        }
bmp8_done:;
    }

    /* --- TGA 32 top-down and bottom-up --- */
    printf("TGA 32-bit (both origins) -> RGBA8:\n");
    for (int td = 0; td < 2; td++) {
        char inp[64], outp[64];
        snprintf(inp, sizeof(inp), "testout/t32_%s.tga", td ? "td" : "bu");
        snprintf(outp, sizeof(outp), "testout/t32_%s.png", td ? "td" : "bu");
        CHECK(gen_tga32(inp, W, H, td) == 0, td ? "generate tga top-down" : "generate tga bottom-up");
        FILE *f = img_fopen_read(inp);
        img_image_t img;
        int oktga = f && tga_decode(f, &img, err, sizeof(err)) == 0;
        CHECK(oktga, "decode tga");
        if (f) fclose(f);
        if (!oktga) { printf("  SKIP  remaining tga checks\n"); continue; }
        CHECK(img.color == IMG_RGBA, "format is RGBA8");
        CHECK(png_write_file(&img, &opts, outp, err, sizeof(err)) == 0, "encode png");
        img_free(&img);
        {
            int w, h; uint8_t *got = NULL;
            int ch = read_png_rgba(outp, &w, &h, &got);
            CHECK(ch == 4, "png readable as RGBA");
            if (ch == 4) {
                uint8_t *exp = (uint8_t *)malloc((size_t)W * H * 4);
                fill_gradient_rgba(exp, W, H, 255);
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < W; x++)
                        exp[((size_t)y * W + x) * 4 + 3] = (uint8_t)((x + y) & 1 ? 128 : 255);
                int diff = compare_rgba(got, exp, W * H * 4);
                CHECK(diff < 0, td ? "pixels identical (top-down)" : "pixels identical (bottom-up)");
                free(exp);
            }
            free(got);
        }
    }

    /* --- PNM 16-bit --- */
    printf("PNM P6 maxval 1023 -> RGB16:\n");
    CHECK(gen_pnm("testout/t16.ppm", 9, 7, 6, 1023) == 0, "generate ppm16");
    {
        FILE *f = img_fopen_read("testout/t16.ppm");
        img_image_t img;
        int ok16 = f && pnm_decode(f, &img, err, sizeof(err)) == 0;
        CHECK(ok16, "decode ppm16");
        if (f) fclose(f);
        if (!ok16) { printf("  SKIP  remaining ppm16 checks\n"); goto ppm_done; }
        CHECK(img.color == IMG_RGB && img.bit_depth == 16, "format is RGB16");
        CHECK(png_write_file(&img, &opts, "testout/t16.png", err, sizeof(err)) == 0, "encode png");
        img_free(&img);
        {
            int w, h; uint8_t *got = NULL;
            int ch = read_png_rgba("testout/t16.png", &w, &h, &got);
            CHECK(ch == 3, "png readable as RGB");
            free(got);
        }
ppm_done:;
    }

    /* --- PNM P5 8-bit gray --- */
    printf("PNM P5 gray -> GRAY8:\n");
    CHECK(gen_pnm("testout/tgray.pgm", 11, 5, 5, 255) == 0, "generate pgm8");
    {
        FILE *f = img_fopen_read("testout/tgray.pgm");
        img_image_t img;
        int okgray = f && pnm_decode(f, &img, err, sizeof(err)) == 0;
        CHECK(okgray, "decode pgm8");
        if (f) fclose(f);
        if (!okgray) { printf("  SKIP  remaining pgm checks\n"); goto pgm_done; }
        CHECK(img.color == IMG_GRAY && img.bit_depth == 8, "format is GRAY8");
        CHECK(png_write_file(&img, &opts, "testout/tgray.png", err, sizeof(err)) == 0, "encode png");
        img_free(&img);
        {
            int w, h; uint8_t *got = NULL;
            int ch = read_png_rgba("testout/tgray.png", &w, &h, &got);
            CHECK(ch == 1, "png readable as GRAY");
            if (ch == 1) {
                uint8_t *exp = (uint8_t *)malloc((size_t)w * h);
                fill_gradient_gray(exp, w, h);
                int diff = compare_rgba(got, exp, w * h);
                CHECK(diff < 0, "gray pixels identical");
                free(exp);
            }
            free(got);
        }
pgm_done:;
    }

    /* --- JPEG round-trip (pixel values may differ slightly from other
     *     decoders but must be stable: encode->decode twice) --- */
    printf("JPEG -> RGB8 (structure check):\n");
    {
        /* use the jpeg sample shipped with jpeg-10 if present */
        FILE *jf = img_fopen_read("F:/PNG/jpeg-10/testimg.jpg");
        if (jf) {
            fclose(jf);
            FILE *f = img_fopen_read("F:/PNG/jpeg-10/testimg.jpg");
            img_image_t img;
            CHECK(f && jpeg_decode(f, &img, err, sizeof(err)) == 0, "decode jpeg");
            if (f) fclose(f);
            CHECK((img.color == IMG_RGB || img.color == IMG_GRAY) && img.bit_depth == 8,
                  "format is RGB8 or GRAY8");
            CHECK(png_write_file(&img, &opts, "testout/tjpg.png", err, sizeof(err)) == 0, "encode png");
            img_free(&img);
            int w, h; uint8_t *got = NULL;
            read_png_rgba("testout/tjpg.png", &w, &h, &got);
            CHECK(got != NULL, "png readable");
            free(got);
        } else {
            printf("  SKIP  jpeg sample not found\n");
        }
    }

    /* --- timestamp copy --- */
    printf("timestamp sync:\n");
    {
        FILE *f = img_fopen_write("testout/ts_in.bin");
        fwrite("x", 1, 1, f);
        fclose(f);
        FILE *g = img_fopen_write("testout/ts_out.bin");
        fwrite("y", 1, 1, g);
        fclose(g);
        /* make the input's times distinctly old (portable) */
        {
            /* raw payload 1e17: a very old timestamp on Windows (FILETIME),
             * harmless on POSIX where only mtime is applied */
            uint64_t old_v = 100000000000000000ULL;
            uint8_t c[16] = {0}, m[16] = {0};
            memcpy(c, &old_v, 8);
            memcpy(m, &old_v, 8);
            set_file_times("testout/ts_in.bin", c, m);
        }
        CHECK(copy_file_times("testout/ts_in.bin", "testout/ts_out.bin") == 0, "copy_file_times");
        uint64_t c_in = file_time_stamp("testout/ts_in.bin", 1);
        uint64_t c_out = file_time_stamp("testout/ts_out.bin", 1);
        uint64_t m_in = file_time_stamp("testout/ts_in.bin", 0);
        uint64_t m_out = file_time_stamp("testout/ts_out.bin", 0);
        CHECK(c_in == c_out && c_in != 0, "creation time matches");
        CHECK(m_in == m_out && m_in != 0, "modification time matches");
    }

    /* --- filter modes all produce readable output --- */
    printf("filter modes:\n");
    {
        const char *modes[] = { "none", "sub", "up", "avg", "paeth", "all", "fast" };
        png_filter_mode_t vals[] = { PNGF_NONE, PNGF_SUB, PNGF_UP, PNGF_AVG,
                                     PNGF_PAETH, PNGF_ALL, PNGF_FAST };
        for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
            FILE *f = img_fopen_read("testout/t24.bmp");
            img_image_t img;
            if (!f || bmp_decode(f, &img, err, sizeof(err)) != 0) {
                if (f) fclose(f);
                printf("  SKIP  %s (source decode failed)\n", modes[i]);
                continue;
            }
            fclose(f);
            png_opts_t o2 = opts;
            o2.filter = vals[i];
            char path[64];
            snprintf(path, sizeof(path), "testout/filt_%s.png", modes[i]);
            int w, h;
            uint8_t *got = NULL;
            CHECK(png_write_file(&img, &o2, path, err, sizeof(err)) == 0, modes[i]);
            int ch = read_png_rgba(path, &w, &h, &got);
            CHECK(ch == 3, "output readable");
            free(got);
            img_free(&img);
        }
    }

    /* --- compression levels 0 and 9 both work, 9 should be <= 0 --- */
    printf("compression levels:\n");
    {
        FILE *f = img_fopen_read("testout/t24.bmp");
        img_image_t img;
        if (!f || bmp_decode(f, &img, err, sizeof(err)) != 0) {
            if (f) fclose(f);
            printf("  SKIP  compression levels (source decode failed)\n");
        } else {
            fclose(f);
        png_opts_t o0 = opts; o0.level = 0;
        png_opts_t o9 = opts; o9.level = 9;
        png_write_file(&img, &o0, "testout/lvl0.png", err, sizeof(err));
        png_write_file(&img, &o9, "testout/lvl9.png", err, sizeof(err));
        struct stat st0 = {0}, st9 = {0};
        stat("testout/lvl0.png", &st0);
        stat("testout/lvl9.png", &st9);
        long long s0 = (long long)st0.st_size, s9 = (long long)st9.st_size;
        printf("        level 0: %lld bytes, level 9: %lld bytes\n", s0, s9);
        CHECK(s9 <= s0, "level 9 compresses at least as well as level 0");
        img_free(&img);
        }
    }


    /* --- GIF: encode with giflib (palette + transparency), decode back --- */
    printf("GIF -> PALETTE8 with transparency:\n");
    {
        int ge = 0;
        GifFileType *gf = EGifOpenFileName("testout/t.gif", 0, &ge);
        CHECK(gf != NULL, "open gif for writing");
        if (gf) {
            GifColorType pal[4] = {{255,0,0},{0,255,0},{0,0,255},{10,20,30}};
            ColorMapObject *cm = GifMakeMapObject(4, pal);
            CHECK(EGifPutScreenDesc(gf, 4, 4, 2, 0, cm) == GIF_OK, "gif screen desc");
            /* the GCB extension must precede the image descriptor */
            GraphicsControlBlock gcb;
            memset(&gcb, 0, sizeof(gcb));
            gcb.DisposalMode = DISPOSE_DO_NOT;
            gcb.TransparentColor = 3;
            uint8_t gcb_buf[8];
            int gcb_len = EGifGCBToExtension(&gcb, gcb_buf);
            CHECK(EGifPutExtension(gf, GRAPHICS_EXT_FUNC_CODE, gcb_len, gcb_buf) == GIF_OK,
                  "gif transparency extension");
            CHECK(EGifPutImageDesc(gf, 0, 0, 4, 4, 0, NULL) == GIF_OK, "gif image desc");
            uint8_t raster[16];
            for (int i = 0; i < 16; i++) raster[i] = (uint8_t)(i % 4);
            CHECK(EGifPutLine(gf, raster, 16) == GIF_OK, "gif pixels");
            CHECK(EGifCloseFile(gf, &ge) == GIF_OK, "close gif");
            GifFreeMapObject(cm);
        }

        FILE *f = img_fopen_read("testout/t.gif");
        img_image_t img;
        int okgif = f && gif_decode(f, &img, err, sizeof(err)) == 0;
        CHECK(okgif, "decode gif");
        if (f) fclose(f);
        if (okgif) {
            CHECK(img.color == IMG_PALETTE && img.bit_depth == 8, "format is PALETTE8");
            CHECK(img.has_pal_alpha && img.pal_alpha[3] == 0, "transparency preserved");
            int idx_ok = 1;
            for (int i = 0; i < 16; i++)
                if (img.data[i] != (uint8_t)(i % 4)) { idx_ok = 0; break; }
            CHECK(idx_ok, "indices identical");
            CHECK(png_write_file(&img, &opts, "testout/tgif.png", err, sizeof(err)) == 0, "encode png");
            img_free(&img);
            int w, h; uint8_t *got = NULL;
            int ch = read_png_rgba("testout/tgif.png", &w, &h, &got);
            CHECK(ch == 4, "png readable as RGBA");
            if (ch == 4) {
                uint8_t *exp = (uint8_t *)malloc((size_t)w * h * 4);
                for (int i = 0; i < w * h; i++) {
                    int idx = i % 4;
                    exp[i*4+0] = pal_gif_r(idx); exp[i*4+1] = pal_gif_g(idx); exp[i*4+2] = pal_gif_b(idx);
                    exp[i*4+3] = (idx == 3) ? 0 : 255;
                }
                CHECK(compare_rgba(got, exp, w * h * 4) < 0, "pixels identical (lossless)");
                free(exp);
            }
            free(got);
        }
    }

    /* --- QOI --- */
    printf("QOI -> RGBA8:\n");
    {
        qoi_desc desc;
        desc.width = 6; desc.height = 4; desc.channels = 4; desc.colorspace = QOI_SRGB;
        uint8_t px[6 * 4 * 4];
        for (int i = 0; i < 6 * 4; i++) {
            px[i*4+0] = (uint8_t)(i * 7); px[i*4+1] = (uint8_t)(i * 3);
            px[i*4+2] = (uint8_t)(i * 11); px[i*4+3] = (uint8_t)(i & 1 ? 128 : 255);
        }
        int enclen = 0;
        void *enc = qoi_encode(px, &desc, &enclen);
        CHECK(enc != NULL, "qoi_encode");
        if (enc) {
            FILE *f = img_fopen_write("testout/t.qoi");
            int wok = f && fwrite(enc, 1, (size_t)enclen, f) == (size_t)enclen;
            if (f) fclose(f);
            free(enc);
            CHECK(wok, "write qoi");
            f = img_fopen_read("testout/t.qoi");
            img_image_t img;
            int okq = f && qoi_decode_file(f, &img, err, sizeof(err)) == 0;
            CHECK(okq, "decode qoi");
            if (f) fclose(f);
            if (okq) {
                CHECK(img.color == IMG_RGBA && img.width == 6 && img.height == 4, "format is RGBA8 6x4");
                int diff = compare_rgba(img.data, px, 6 * 4 * 4);
                CHECK(diff < 0, "pixels identical (lossless)");
                png_write_file(&img, &opts, "testout/tqoi.png", err, sizeof(err));
                img_free(&img);
            }
        }
    }

    /* --- WebP (encode sample, decode, verify round-trip readability) --- */
    printf("WebP -> RGBA8:\n");
    {
        uint8_t px[8 * 5 * 4];
        for (int i = 0; i < 8 * 5; i++) {
            px[i*4+0] = (uint8_t)(i * 5); px[i*4+1] = (uint8_t)(255 - i * 5);
            px[i*4+2] = (uint8_t)((i * 13) & 0xFF); px[i*4+3] = (uint8_t)(i & 1 ? 200 : 255);
        }
        int wpsize = WebPEncodeRGBA(px, 8, 5, 8 * 4, 70.0f, &wp_blob);
        CHECK(wpsize > 0, "WebPEncodeRGBA");
        if (wpsize > 0) {
            FILE *f = img_fopen_write("testout/t.webp");
            int wok = f && fwrite(wp_blob, 1, (size_t)wpsize, f) == (size_t)wpsize;
            if (f) fclose(f);
            free(wp_blob);
            wp_blob = NULL;
            CHECK(wok, "write webp");
            f = img_fopen_read("testout/t.webp");
            img_image_t img;
            int okw = f && webp_decode(f, &img, err, sizeof(err)) == 0;
            CHECK(okw, "decode webp");
            if (f) fclose(f);
            if (okw) {
                CHECK(img.color == IMG_RGBA && img.width == 8 && img.height == 5, "format is RGBA8 8x5");
                CHECK(png_write_file(&img, &opts, "testout/twebp.png", err, sizeof(err)) == 0, "encode png");
                img_free(&img);
                int w, h; uint8_t *got = NULL;
                int ch = read_png_rgba("testout/twebp.png", &w, &h, &got);
                CHECK(ch == 4, "png readable as RGBA");
                free(got);
            }
        }
    }

    /* --- TIFF (RGB, written with libtiff) --- */
    printf("TIFF -> RGBA8:\n");
    {
        TIFF *tf = TIFFOpen("testout/t.tiff", "w");
        CHECK(tf != NULL, "open tiff for writing");
        if (tf) {
            TIFFSetField(tf, TIFFTAG_IMAGEWIDTH, 5);
            TIFFSetField(tf, TIFFTAG_IMAGELENGTH, 3);
            TIFFSetField(tf, TIFFTAG_BITSPERSAMPLE, 8);
            TIFFSetField(tf, TIFFTAG_SAMPLESPERPIXEL, 3);
            TIFFSetField(tf, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
            TIFFSetField(tf, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
            uint8_t row[5 * 3];
            int wok = 1;
            for (int y = 0; y < 3 && wok; y++) {
                for (int x = 0; x < 5; x++) {
                    row[x*3+0] = (uint8_t)(x * 40); row[x*3+1] = (uint8_t)(y * 80); row[x*3+2] = 77;
                }
                wok = TIFFWriteScanline(tf, row, (uint32_t)y, 0) >= 0;
            }
            CHECK(wok, "write tiff scanlines");
            TIFFClose(tf);
        }
        FILE *f = img_fopen_read("testout/t.tiff");
        img_image_t img;
        int okt = f && tiff_decode(f, &img, err, sizeof(err)) == 0;
        CHECK(okt, "decode tiff");
        if (f) fclose(f);
        if (okt) {
            CHECK(img.color == IMG_RGB && img.bit_depth == 8 &&
                  img.width == 5 && img.height == 3, "format is RGB8 5x3");
            int diff = -1;
            for (int y = 0; y < 3 && diff < 0; y++)
                for (int x = 0; x < 5 && diff < 0; x++) {
                    uint8_t *d = img.data + ((size_t)y * 5 + x) * 3;
                    if (d[0] != (uint8_t)(x * 40) || d[1] != (uint8_t)(y * 80) || d[2] != 77)
                        diff = y * 5 + x;
                }
            CHECK(diff < 0, "pixels identical (lossless)");
            png_write_file(&img, &opts, "testout/ttiff.png", err, sizeof(err));
            img_free(&img);
            int w, h; uint8_t *got = NULL;
            int ch = read_png_rgba("testout/ttiff.png", &w, &h, &got);
            CHECK(ch == 3, "png readable as RGB");
            free(got);
        }
    }

    /* --- TIFF bit-depth matching: gray 16-bit --- */
    printf("TIFF gray 16-bit -> GRAY16:\n");
    {
        TIFF *tf = TIFFOpen("testout/tg16.tiff", "w");
        CHECK(tf != NULL, "open tiff for writing");
        if (tf) {
            TIFFSetField(tf, TIFFTAG_IMAGEWIDTH, 4);
            TIFFSetField(tf, TIFFTAG_IMAGELENGTH, 2);
            TIFFSetField(tf, TIFFTAG_BITSPERSAMPLE, 16);
            TIFFSetField(tf, TIFFTAG_SAMPLESPERPIXEL, 1);
            TIFFSetField(tf, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
            TIFFSetField(tf, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
            int wok = 1;
            for (int y = 0; y < 2 && wok; y++) {
                uint16_t row[4];
                for (int x = 0; x < 4; x++)
                    row[x] = (uint16_t)((y * 4 + x) * 12345);
                wok = TIFFWriteScanline(tf, row, (uint32_t)y, 0) >= 0;
            }
            CHECK(wok, "write 16-bit scanlines");
            TIFFClose(tf);
        }
        FILE *f = img_fopen_read("testout/tg16.tiff");
        img_image_t img;
        int okg = f && tiff_decode(f, &img, err, sizeof(err)) == 0;
        CHECK(okg, "decode gray16 tiff");
        if (f) fclose(f);
        if (okg) {
            CHECK(img.color == IMG_GRAY && img.bit_depth == 16, "format is GRAY16");
            int diff = -1;
            for (int y = 0; y < 2 && diff < 0; y++)
                for (int x = 0; x < 4 && diff < 0; x++) {
                    unsigned v = img_ld16be(img.data +
                        (size_t)y * img.rowstride + (size_t)x * 2);
                    if (v != (((unsigned)((y * 4 + x) * 12345)) & 0xFFFFu))
                        diff = y * 4 + x;
                }
            CHECK(diff < 0, "16-bit values identical (lossless)");
            img_free(&img);
        }
    }

    /* --- TIFF bit-depth matching: palette 8-bit --- */
    printf("TIFF palette 8-bit -> PALETTE8:\n");
    {
        TIFF *tf = TIFFOpen("testout/tp8.tiff", "w");
        CHECK(tf != NULL, "open tiff for writing");
        if (tf) {
            TIFFSetField(tf, TIFFTAG_IMAGEWIDTH, 5);
            TIFFSetField(tf, TIFFTAG_IMAGELENGTH, 2);
            TIFFSetField(tf, TIFFTAG_BITSPERSAMPLE, 8);
            TIFFSetField(tf, TIFFTAG_SAMPLESPERPIXEL, 1);
            TIFFSetField(tf, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_PALETTE);
            TIFFSetField(tf, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
            uint16_t rmap[256], gmap[256], bmap[256];
            for (int i = 0; i < 256; i++) {
                rmap[i] = (uint16_t)(i * 257);        /* 16->8 scales to i     */
                gmap[i] = (uint16_t)(65535 - i * 257); /* scales to 255 - i    */
                bmap[i] = (uint16_t)(i * 257);
            }
            TIFFSetField(tf, TIFFTAG_COLORMAP, rmap, gmap, bmap);
            int wok = 1;
            for (int y = 0; y < 2 && wok; y++) {
                uint8_t row[5];
                for (int x = 0; x < 5; x++)
                    row[x] = (uint8_t)((x + y) % 256);
                wok = TIFFWriteScanline(tf, row, (uint32_t)y, 0) >= 0;
            }
            CHECK(wok, "write palette scanlines");
            TIFFClose(tf);
        }
        FILE *f = img_fopen_read("testout/tp8.tiff");
        img_image_t img;
        int okp = f && tiff_decode(f, &img, err, sizeof(err)) == 0;
        CHECK(okp, "decode palette tiff");
        if (f) fclose(f);
        if (okp) {
            CHECK(img.color == IMG_PALETTE && img.bit_depth == 8, "format is PALETTE8");
            CHECK(img.pal_ncolors == 256, "256-entry palette");
            int cmap_ok = 1;
            for (int i = 0; i < 256 && cmap_ok; i++) {
                if (img.palette[i*3+0] != (uint8_t)i ||
                    img.palette[i*3+1] != (uint8_t)(255 - i) ||
                    img.palette[i*3+2] != (uint8_t)i)
                    cmap_ok = 0;
            }
            CHECK(cmap_ok, "colormap scaled 16->8 correctly");
            int idx_ok = 1;
            for (int y = 0; y < 2 && idx_ok; y++)
                for (int x = 0; x < 5 && idx_ok; x++)
                    if (img.data[(size_t)y * img.rowstride + x] !=
                        (uint8_t)((x + y) % 256))
                        idx_ok = 0;
            CHECK(idx_ok, "indices identical");
            img_free(&img);
        }
    }

    /* --- TIFF bit-depth matching: bilevel 1-bit --- */
    printf("TIFF bilevel 1-bit -> GRAY1:\n");
    {
        TIFF *tf = TIFFOpen("testout/tb1.tiff", "w");
        CHECK(tf != NULL, "open tiff for writing");
        if (tf) {
            TIFFSetField(tf, TIFFTAG_IMAGEWIDTH, 16);
            TIFFSetField(tf, TIFFTAG_IMAGELENGTH, 2);
            TIFFSetField(tf, TIFFTAG_BITSPERSAMPLE, 1);
            TIFFSetField(tf, TIFFTAG_SAMPLESPERPIXEL, 1);
            TIFFSetField(tf, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
            TIFFSetField(tf, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
            uint8_t rows[2][2] = {{0x55, 0x55}, {0xAA, 0xAA}};
            int wok = 1;
            for (int y = 0; y < 2 && wok; y++)
                wok = TIFFWriteScanline(tf, rows[y], (uint32_t)y, 0) >= 0;
            CHECK(wok, "write bilevel scanlines");
            TIFFClose(tf);
        }
        FILE *f = img_fopen_read("testout/tb1.tiff");
        img_image_t img;
        int okb = f && tiff_decode(f, &img, err, sizeof(err)) == 0;
        CHECK(okb, "decode bilevel tiff");
        if (f) fclose(f);
        if (okb) {
            CHECK(img.color == IMG_GRAY && img.bit_depth == 1, "format is GRAY1");
            int s0 = (img.data[0] >> 7) & 1, s1 = (img.data[0] >> 6) & 1;
            CHECK(s0 == 0 && s1 == 1, "row 0 samples");
            s0 = (img.data[img.rowstride] >> 7) & 1;
            s1 = (img.data[img.rowstride] >> 6) & 1;
            CHECK(s0 == 1 && s1 == 0, "row 1 samples");
            img_free(&img);
        }
    }


    /* --- animated GIF -> APNG (3 frames, offsets, disposal, loop) --- */
    printf("animated GIF -> APNG:\n");
    {
        /* write a 6x4 3-frame GIF: frame0 full canvas (opaque), frame1
         * 4x2 sub-rect at (1,1) with disposal BACKGROUND, frame2 full
         * canvas with transparency index 3; NETSCAPE loop = 0 (infinite) */
        int ge = 0;
        GifFileType *gf = EGifOpenFileName("testout/anim.gif", 0, &ge);
        CHECK(gf != NULL, "open animated gif for writing");
        if (gf) {
            GifColorType pal[4] = {{255,0,0},{0,255,0},{0,0,255},{10,20,30}};
            ColorMapObject *cm = GifMakeMapObject(4, pal);
            CHECK(EGifPutScreenDesc(gf, 6, 4, 2, 0, cm) == GIF_OK, "gif screen desc");
            /* NETSCAPE loop = infinite */
            uint8_t ns_data[3] = {1, 0, 0};
            CHECK(EGifPutExtensionLeader(gf, APPLICATION_EXT_FUNC_CODE) == GIF_OK,
                  "netscape leader");
            CHECK(EGifPutExtensionBlock(gf, 11, "NETSCAPE2.0") == GIF_OK,
                  "netscape id");
            CHECK(EGifPutExtensionBlock(gf, 3, ns_data) == GIF_OK,
                  "netscape loop data");
            CHECK(EGifPutExtensionTrailer(gf) == GIF_OK, "netscape trailer");

            struct {
                int x, y, w, h, delay, dispose, trans;
                uint8_t fill;
            } frames[3] = {
                {0, 0, 6, 4, 10, 1, -1, 0},
                {1, 1, 4, 2, 20, 2,  3, 1},
                {0, 0, 6, 4,  5, 1,  3, 2},
            };
            int all_ok = 1;
            for (int i = 0; i < 3 && all_ok; i++) {
                GraphicsControlBlock gcb;
                memset(&gcb, 0, sizeof(gcb));
                gcb.DisposalMode = frames[i].dispose;
                gcb.UserInputFlag = 0;
                gcb.DelayTime = frames[i].delay;
                gcb.TransparentColor = frames[i].trans;
                uint8_t gcb_buf[8];
                int gcb_len = EGifGCBToExtension(&gcb, gcb_buf);
                all_ok &= EGifPutExtension(gf, GRAPHICS_EXT_FUNC_CODE, gcb_len, gcb_buf) == GIF_OK;
                all_ok &= EGifPutImageDesc(gf, frames[i].x, frames[i].y,
                                           frames[i].w, frames[i].h, 0, NULL) == GIF_OK;
                int npix = frames[i].w * frames[i].h;
                uint8_t raster[64];
                for (int j = 0; j < npix; j++)
                    raster[j] = (uint8_t)((frames[i].fill + j) % 4);
                all_ok &= EGifPutLine(gf, raster, npix) == GIF_OK;
            }
            CHECK(all_ok, "write 3 gif frames");
            CHECK(EGifCloseFile(gf, &ge) == GIF_OK, "close gif");
            GifFreeMapObject(cm);

            /* decode animation */
            FILE *f = img_fopen_read("testout/anim.gif");
            img_animation_t anim;
            int oka = f && gif_decode_anim(f, &anim, err, sizeof(err)) == 0;
            CHECK(oka, "gif_decode_anim");
            if (f) fclose(f);
            if (oka) {
                CHECK(anim.nframes == 3 && anim.width == 6 && anim.height == 4,
                      "3 frames, 6x4 canvas");
                CHECK(anim.delays_cs[0] == 10 && anim.delays_cs[1] == 20 &&
                      anim.delays_cs[2] == 5, "per-frame delays preserved");
                CHECK(anim.dispose[0] == 1 && anim.dispose[1] == 2 &&
                      anim.dispose[2] == 1, "disposal modes preserved");
                CHECK(anim.x[1] == 1 && anim.y[1] == 1 &&
                      anim.frames[1].width == 4 && anim.frames[1].height == 2,
                      "frame 1 sub-rect offset preserved");
                CHECK(anim.loops == 0, "NETSCAPE loop=infinite mapped to 0");
                CHECK(anim.frames[0].color == IMG_RGBA, "frames are RGBA");

                /* write APNG */
                png_opts_t ao = opts;
                ao.filter = PNGF_AUTO;
                CHECK(png_write_apng(&anim, &ao, "testout/anim.png",
                                     err, sizeof(err)) == 0, "png_write_apng");
                img_free_anim(&anim);

                /* structural check: walk chunks, verify acTL/fcTL/fdAT */
                FILE *pf = img_fopen_read("testout/anim.png");
                CHECK(pf != NULL, "apng readable");
                if (pf) {
                    uint8_t sig[8];
                    int sig_ok = fread(sig, 1, 8, pf) == 8 &&
                                 !memcmp(sig, "\x89PNG\r\n\x1a\n", 8);
                    CHECK(sig_ok, "png signature");
                    int n_actl = 0, n_fctl = 0, n_fdat = 0, n_idat = 0;
                    uint32_t actl_frames = 0, actl_plays = 999;
                    int seq_ok = 1;
                    uint32_t expect_seq = 0;
                    unsigned char ch[4];
                    long read_n = 0;
                    long pos = 8;
                    fseek(pf, 0, SEEK_END);
                    long fsize = ftell(pf);
                    fseek(pf, 8, SEEK_SET);
                    while (pos + 8 <= fsize) {
                        uint32_t len = 0;
                        unsigned char lb[4];
                        if (fread(lb, 1, 4, pf) != 4) break;
                        len = ((uint32_t)lb[0] << 24) | ((uint32_t)lb[1] << 16) |
                              ((uint32_t)lb[2] << 8) | lb[3];
                        if (fread(ch, 1, 4, pf) != 4) break;
                        if (!memcmp(ch, "acTL", 4)) {
                            n_actl++;
                            unsigned char d[8];
                            read_n = (fread(d, 1, 8, pf) == 8) ? 8 : 0;
                            if (read_n == 8) {
                                actl_frames = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) |
                                              ((uint32_t)d[2] << 8) | d[3];
                                actl_plays  = ((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) |
                                              ((uint32_t)d[6] << 8) | d[7];
                            }
                        } else if (!memcmp(ch, "fcTL", 4)) {
                            n_fctl++;
                            unsigned char d[26];
                            read_n = (fread(d, 1, 26, pf) == 26) ? 26 : 0;
                            if (read_n == 26) {
                                uint32_t seq = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) |
                                               ((uint32_t)d[2] << 8) | d[3];
                                if (seq != expect_seq) seq_ok = 0;
                                expect_seq++;
                            }
                        } else if (!memcmp(ch, "fdAT", 4)) {
                            n_fdat++;
                            unsigned char d[4];
                            read_n = (fread(d, 1, 4, pf) == 4) ? 4 : 0;
                            if (read_n == 4) {
                                uint32_t seq = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) |
                                               ((uint32_t)d[2] << 8) | d[3];
                                if (seq != expect_seq) seq_ok = 0;
                                expect_seq++;
                            }
                        } else if (!memcmp(ch, "IDAT", 4)) {
                            n_idat++;
                            read_n = 0;
                        } else if (!memcmp(ch, "IEND", 4)) {
                            break;
                        }
                        /* skip the unread remainder of data + crc */
                        fseek(pf, (long)len - (long)read_n + 4, SEEK_CUR);
                        pos += (long)len + 12;
                    }
                    fclose(pf);
                    CHECK(n_actl == 1, "exactly one acTL");
                    CHECK(actl_frames == 3, "acTL num_frames = 3");
                    CHECK(actl_plays == 0, "acTL num_plays = 0 (infinite)");
                    CHECK(n_fctl == 3, "three fcTL chunks");
                    CHECK(n_idat == 1, "first frame in IDAT");
                    CHECK(n_fdat >= 1, "subsequent frames in fdAT");
                    CHECK(seq_ok, "fcTL/fdAT sequence numbers contiguous");
                }
            }
        }
    }


    /* --- AVIF: decode the vendored 1x1 sample --- */
    printf("AVIF -> RGBA8:\n");
    {
        FILE *f = img_fopen_read("tests/data/white_1x1.avif");
        img_image_t img;
        int okv = f && avif_decode(f, &img, err, sizeof(err)) == 0;
        CHECK(okv, "decode avif sample");
        if (f) fclose(f);
        if (okv) {
            CHECK(img.width == 1 && img.height == 1 && img.color == IMG_RGBA &&
                  img.bit_depth == 8, "format is RGBA8 1x1");
            CHECK(img.data[0] > 200 && img.data[1] > 200 && img.data[2] > 200 &&
                  img.data[3] == 255, "white pixel decoded");
            png_write_file(&img, &opts, "testout/tavif.png", err, sizeof(err));
            img_free(&img);
            int w, h; uint8_t *got = NULL;
            int ch = read_png_rgba("testout/tavif.png", &w, &h, &got);
            CHECK(ch == 4 && w == 1 && h == 1, "png readable as RGBA 1x1");
            free(got);
        }
    }

    /* --- HEIF: graceful error path on garbage input --- */
    printf("HEIF error path:\n");
    {
        FILE *f = img_fopen_write("testout/fake.heic");
        if (f) {
            fwrite("{garbage-not-a-heif}", 1, 20, f);
            fclose(f);
        }
        f = img_fopen_read("testout/fake.heic");
        img_image_t img;
        int rc = f ? heif_decode(f, &img, err, sizeof(err)) : -1;
        CHECK(rc != 0, "heif rejects garbage");
        CHECK(err[0] != 0, "error message present");
        if (f) fclose(f);
    }

    /* --- BMP sub-byte palettes (1/4-bit): these used to be written one byte
     *     per pixel into a bit-packed row, corrupting the image and writing
     *     past the row stride --- */
    printf("BMP sub-byte palette (1/4-bit):\n");
    {
        static const int bpps[2] = {1, 4};
        static const int ncols[2] = {2, 16};
        for (int k = 0; k < 2; k++) {
            int bpp = bpps[k], npal = ncols[k];
            char in[64], out[64];
            snprintf(in, sizeof(in), "testout/sb%d.bmp", bpp);
            snprintf(out, sizeof(out), "testout/sb%d.png", bpp);
            CHECK(gen_bmp_subbyte(in, W, H, bpp, npal) == 0, "generate bmp sub-byte");
            FILE *f = img_fopen_read(in);
            img_image_t img;
            int okd = f && bmp_decode(f, &img, err, sizeof(err)) == 0;
            if (f) fclose(f);
            CHECK(okd, "decode bmp sub-byte");
            if (!okd) continue;
            CHECK(img.color == IMG_PALETTE && img.bit_depth == bpp,
                  "palette at source depth");
            int idx_ok = 1;
            for (int y = 0; y < H && idx_ok; y++)
                for (int x = 0; x < W && idx_ok; x++)
                    if ((int)get_sample(&img, x, y, 0) != bmp_sub_index(x, y, npal))
                        idx_ok = 0;
            CHECK(idx_ok, "indices identical (packed at source depth)");
            CHECK(png_write_file(&img, &opts, out, err, sizeof(err)) == 0, "encode png");
            img_free(&img);

            raw_png_t rp;
            int okr = read_png_native(out, &rp) == 0;
            CHECK(okr, "png readable");
            if (okr) {
                CHECK(rp.bit_depth == bpp && rp.color_type == PNG_COLOR_TYPE_PALETTE,
                      "png keeps palette depth");
                int px_ok = 1;
                for (int y = 0; y < H && px_ok; y++)
                    for (int x = 0; x < W && px_ok; x++)
                        if ((int)raw_sample(&rp, x, y, 0) != bmp_sub_index(x, y, npal))
                            px_ok = 0;
                CHECK(px_ok, "png indices identical");
                raw_png_free(&rp);
            }
        }
    }

    /* --- ICO entries: every depth plus AND-mask transparency --- */
    printf("ICO entries (1/4/8/24/32-bit + AND mask):\n");
    {
        static const struct { int bpp, mask, alpha; const char *name; } cases[] = {
            {1,  1, 0, "1-bit palette + AND mask"},
            {4,  1, 0, "4-bit palette + AND mask"},
            {8,  0, 0, "8-bit palette opaque"},
            {24, 1, 0, "24-bit + AND mask (promotes to RGBA)"},
            {24, 0, 0, "24-bit opaque"},
            {32, 0, 1, "32-bit real alpha channel"},
            {32, 1, 0, "32-bit zero alpha + AND mask"},
            {32, 0, 0, "32-bit opaque"},
        };
        const int w = 32, h = 16;
        for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
            printf("  [%s]\n", cases[c].name);
            char in[64];
            snprintf(in, sizeof(in), "testout/ic_%d_%d_%d.ico",
                     cases[c].bpp, cases[c].mask, cases[c].alpha);
            CHECK(gen_ico_dib(in, w, h, cases[c].bpp, cases[c].mask,
                              cases[c].alpha) == 0, "generate ico");
            FILE *f = img_fopen_read(in);
            img_image_t img;
            int okd = f && ico_decode(f, &img, err, sizeof(err)) == 0;
            if (f) fclose(f);
            CHECK(okd, "decode ico");
            if (!okd) continue;

            if (cases[c].bpp <= 8) {
                int npal = 1 << cases[c].bpp;
                CHECK(img.color == IMG_PALETTE && img.bit_depth == cases[c].bpp,
                      "palette at entry depth");
                int idx_ok = 1;
                for (int y = 0; y < h && idx_ok; y++)
                    for (int x = 0; x < w && idx_ok; x++)
                        if ((int)get_sample(&img, x, y, 0) != (x + y) % npal)
                            idx_ok = 0;
                CHECK(idx_ok, "indices identical");
                if (cases[c].mask) {
                    int mask_ok = img.has_pal_alpha;
                    for (int y = 0; y < h && mask_ok; y++)
                        for (int x = 0; x < w / 2; x++)
                            if (img.pal_alpha[(x + y) % npal] != 0) { mask_ok = 0; break; }
                    CHECK(mask_ok, "AND mask clears those palette entries");
                }
            } else {
                int want_rgba = (cases[c].bpp == 32) ? (cases[c].alpha || cases[c].mask)
                                                     : cases[c].mask;
                CHECK(img.color == (want_rgba ? IMG_RGBA : IMG_RGB) && img.bit_depth == 8,
                      want_rgba ? "decodes as RGBA8" : "decodes as RGB8");
                int ch = img_channels(img.color);
                int px_ok = 1;
                for (int y = 0; y < h && px_ok; y++)
                    for (int x = 0; x < w && px_ok; x++) {
                        const uint8_t *d = img.data + (size_t)y * img.rowstride +
                                           (size_t)x * (size_t)ch;
                        if (d[0] != (uint8_t)(x * 7 + y) || d[1] != (uint8_t)(x + y) ||
                            d[2] != (uint8_t)(x * 3 + y))
                            px_ok = 0;
                    }
                CHECK(px_ok, "RGB samples identical");
                if (want_rgba) {
                    int a_ok = 1;
                    for (int y = 0; y < h && a_ok; y++)
                        for (int x = 0; x < w; x++) {
                            uint8_t a = img.data[(size_t)y * img.rowstride +
                                                 (size_t)x * 4 + 3];
                            uint8_t want = (x < w / 2) ? 0 : 255;
                            if (a != want) { a_ok = 0; break; }
                        }
                    CHECK(a_ok, "alpha correct (left half transparent)");
                }
            }
            img_free(&img);
        }
    }

    /* --- ICO whose entry payload is a PNG file (png_decode_mem path) --- */
    printf("ICO with PNG-compressed entry:\n");
    {
        img_image_t src;
        memset(&src, 0, sizeof(src));
        src.width = 16; src.height = 12; src.bit_depth = 8; src.color = IMG_RGBA;
        src.rowstride = img_rowstride(src.width, 8, 4);
        src.data = (uint8_t *)malloc(src.rowstride * (size_t)src.height);
        for (int y = 0; y < src.height; y++)
            for (int x = 0; x < src.width; x++) {
                uint8_t *d = src.data + (size_t)y * src.rowstride + (size_t)x * 4;
                d[0] = (uint8_t)(x * 7);
                d[1] = (uint8_t)(y * 5);
                d[2] = (uint8_t)((x ^ y) & 0xFF);
                d[3] = (uint8_t)(x < 8 ? 0 : 255);
            }
        CHECK(png_write_file(&src, &opts, "testout/ico_payload.png", err, sizeof(err)) == 0,
              "write png payload");
        img_free(&src);
        CHECK(gen_ico_png("testout/icopng.ico", "testout/ico_payload.png") == 0,
              "wrap png as ico entry");

        FILE *f = img_fopen_read("testout/icopng.ico");
        img_image_t img;
        int okd = f && ico_decode(f, &img, err, sizeof(err)) == 0;
        if (f) fclose(f);
        CHECK(okd, "decode ico(png)");
        if (okd) {
            CHECK(img.color == IMG_RGBA && img.bit_depth == 8 &&
                  img.width == 16 && img.height == 12,
                  "png entry decoded at native depth");
            raw_png_t rp;
            if (read_png_native("testout/ico_payload.png", &rp) == 0) {
                int same = (rp.width == img.width && rp.height == img.height &&
                            rp.bit_depth == 8);
                for (int y = 0; y < img.height && same; y++)
                    for (int x = 0; x < img.width * 4; x++)
                        if (rp.data[(size_t)y * rp.rowstride + x] !=
                            img.data[(size_t)y * img.rowstride + x])
                            same = 0;
                CHECK(same, "pixels match the source PNG");
                raw_png_free(&rp);
            } else {
                CHECK(0, "re-read payload png");
            }
            img_free(&img);
        }
    }

    /* --- TIFF sub-byte grayscale (2/4-bit): the destination used to be
     *     written 1 bit per pixel regardless of the source depth --- */
    printf("TIFF sub-byte gray (2/4-bit):\n");
    {
        static const int bits_tab[2] = {2, 4};
        const int w = 16, h = 2;
        for (int k = 0; k < 2; k++) {
            int bits = bits_tab[k];
            uint8_t vals[16 * 2];
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    vals[y * w + x] = (uint8_t)((x + y) % (depth_max(bits) + 1));
            char in[64];
            snprintf(in, sizeof(in), "testout/g%d.tiff", bits);
            CHECK(tiff_write_subbyte_gray(in, w, h, bits, vals,
                                          PHOTOMETRIC_MINISBLACK) == 0,
                  "write gray tiff");
            FILE *f = img_fopen_read(in);
            img_image_t img;
            int okd = f && tiff_decode(f, &img, err, sizeof(err)) == 0;
            if (f) fclose(f);
            CHECK(okd, "decode gray tiff");
            if (okd) {
                CHECK(img.color == IMG_GRAY && img.bit_depth == bits,
                      "GRAY at source depth");
                int ok = 1;
                for (int y = 0; y < h && ok; y++)
                    for (int x = 0; x < w && ok; x++)
                        if (get_sample(&img, x, y, 0) != vals[y * w + x]) ok = 0;
                CHECK(ok, "samples identical");
                img_free(&img);
            }
        }
        /* MINISWHITE inversion must survive the repacking too */
        {
            int bits = 4;
            uint8_t vals[16];
            for (int x = 0; x < 16; x++)
                vals[x] = (uint8_t)x;
            CHECK(tiff_write_subbyte_gray("testout/gw4.tiff", 16, 1, bits, vals,
                                          PHOTOMETRIC_MINISWHITE) == 0,
                  "write gray tiff (MINISWHITE)");
            FILE *f = img_fopen_read("testout/gw4.tiff");
            img_image_t img;
            int okd = f && tiff_decode(f, &img, err, sizeof(err)) == 0;
            if (f) fclose(f);
            CHECK(okd, "decode gray tiff (MINISWHITE)");
            if (okd) {
                int ok = (img.color == IMG_GRAY && img.bit_depth == bits);
                for (int x = 0; x < 16 && ok; x++)
                    if (get_sample(&img, x, 0, 0) != (unsigned)(15 - vals[x])) ok = 0;
                CHECK(ok, "inverted samples identical");
                img_free(&img);
            }
        }
    }

    /* --- TIFF sub-byte palette (2/4-bit) --- */
    printf("TIFF sub-byte palette (2/4-bit):\n");
    {
        static const int bits_tab[2] = {2, 4};
        const int w = 16, h = 2;
        for (int k = 0; k < 2; k++) {
            int bits = bits_tab[k];
            int nc = 1 << bits;
            uint8_t vals[16 * 2];
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    vals[y * w + x] = (uint8_t)((x + y) % nc);
            char in[64];
            snprintf(in, sizeof(in), "testout/p%d.tiff", bits);
            CHECK(tiff_write_subbyte_palette(in, w, h, bits, vals) == 0,
                  "write palette tiff");
            FILE *f = img_fopen_read(in);
            img_image_t img;
            int okd = f && tiff_decode(f, &img, err, sizeof(err)) == 0;
            if (f) fclose(f);
            CHECK(okd, "decode palette tiff");
            if (okd) {
                CHECK(img.color == IMG_PALETTE && img.bit_depth == bits,
                      "PALETTE at source depth");
                CHECK(img.pal_ncolors == nc, "colormap size");
                int cmap_ok = 1;
                int step = (nc > 1) ? 255 / (nc - 1) : 255;   /* 16->8 scaled */
                for (int i = 0; i < nc && cmap_ok; i++)
                    if (img.palette[i * 3 + 0] != (uint8_t)(step * i) ||
                        img.palette[i * 3 + 1] != (uint8_t)(255 - step * i) ||
                        img.palette[i * 3 + 2] != (uint8_t)(step * i))
                        cmap_ok = 0;
                CHECK(cmap_ok, "colormap scaled 16->8 correctly");
                int idx_ok = 1;
                for (int y = 0; y < h && idx_ok; y++)
                    for (int x = 0; x < w && idx_ok; x++)
                        if (get_sample(&img, x, y, 0) != vals[y * w + x]) idx_ok = 0;
                CHECK(idx_ok, "indices identical");
                img_free(&img);
            }
        }
    }

    /* --- PNM P1/P4 bilevel --- */
    printf("PNM P1/P4 bilevel -> GRAY1:\n");
    {
        const int w = 16, h = 4;
        for (int ascii = 1; ascii >= 0; ascii--) {
            const char *name = ascii ? "testout/tp1.pbm" : "testout/tp4.pbm";
            CHECK(gen_pbm(name, w, h, ascii) == 0, "generate pbm");
            FILE *f = img_fopen_read(name);
            img_image_t img;
            int okd = f && pnm_decode(f, &img, err, sizeof(err)) == 0;
            if (f) fclose(f);
            CHECK(okd, "decode pbm");
            if (okd) {
                CHECK(img.color == IMG_GRAY && img.bit_depth == 1, "format is GRAY1");
                int ok = 1;
                for (int y = 0; y < h && ok; y++)
                    for (int x = 0; x < w && ok; x++) {
                        /* PBM: 1 = black; PNG 1-bit gray: 1 = white */
                        unsigned want = ((x + y) & 1) ? 0u : 1u;
                        if (get_sample(&img, x, y, 0) != want) ok = 0;
                    }
                CHECK(ok, "black/white mapping and 1-bit packing");
                img_free(&img);
            }
        }
    }

    /* --- PNM 16-bit: verify the actual sample values, not just the format --- */
    printf("PNM P6 maxval 1023 -> RGB16 samples:\n");
    {
        const int w = 9, h = 7, maxval = 1023;
        CHECK(gen_pnm("testout/t16v.ppm", w, h, 6, maxval) == 0, "generate ppm16");
        FILE *f = img_fopen_read("testout/t16v.ppm");
        img_image_t img;
        int okd = f && pnm_decode(f, &img, err, sizeof(err)) == 0;
        if (f) fclose(f);
        CHECK(okd, "decode ppm16");
        if (okd) {
            CHECK(img.color == IMG_RGB && img.bit_depth == 16, "format is RGB16");
            CHECK(png_write_file(&img, &opts, "testout/t16v.png", err, sizeof(err)) == 0,
                  "encode png");
            img_free(&img);
            raw_png_t rp;
            if (read_png_native("testout/t16v.png", &rp) == 0) {
                int ok = (rp.bit_depth == 16 && rp.color_type == PNG_COLOR_TYPE_RGB);
                for (int y = 0; y < h && ok; y++)
                    for (int x = 0; x < w && ok; x++) {
                        uint8_t v8[3] = { (uint8_t)(x * 255 / (w - 1)),
                                          (uint8_t)(y * 255 / (h - 1)),
                                          (uint8_t)((x ^ y) & 0xFF) };
                        for (int c = 0; c < 3; c++) {
                            unsigned s = (unsigned)v8[c] * (unsigned)maxval / 255u;
                            unsigned want = (unsigned)((unsigned long long)s * 65535 /
                                                       (unsigned)maxval);
                            if (raw_sample(&rp, x, y, c) != want) ok = 0;
                        }
                    }
                CHECK(ok, "16-bit samples match the source scaling");
                raw_png_free(&rp);
            } else {
                CHECK(0, "read back 16-bit png");
            }
        }
    }

    /* --- PNG as input: native color type and depth must survive --- */
    printf("PNG input round-trip (native depth preserved):\n");
    {
        static const struct {
            img_color_t color; int depth; const char *name; int pal_trns;
        } cs[] = {
            {IMG_GRAY,        1,  "gray1",   0},
            {IMG_GRAY,        2,  "gray2",   0},
            {IMG_GRAY,        4,  "gray4",   0},
            {IMG_GRAY,        8,  "gray8",   0},
            {IMG_GRAY,        16, "gray16",  0},
            {IMG_GRAY_ALPHA,  8,  "ga8",     0},
            {IMG_GRAY_ALPHA,  16, "ga16",    0},
            {IMG_PALETTE,     1,  "pal1",    1},
            {IMG_PALETTE,     4,  "pal4",    1},
            {IMG_PALETTE,     8,  "pal8",    0},
            {IMG_RGB,         8,  "rgb8",    0},
            {IMG_RGB,         16, "rgb16",   0},
            {IMG_RGBA,        8,  "rgba8",   0},
            {IMG_RGBA,        16, "rgba16",  0},
        };
        const int w = 13, h = 5;
        for (size_t c = 0; c < sizeof(cs) / sizeof(cs[0]); c++) {
            img_image_t src;
            memset(&src, 0, sizeof(src));
            src.width = w; src.height = h;
            src.color = cs[c].color;
            src.bit_depth = cs[c].depth;
            int ch = img_channels(cs[c].color);
            src.rowstride = img_rowstride(w, cs[c].depth, ch);
            src.data = (uint8_t *)calloc((size_t)h, src.rowstride);
            unsigned dmax = depth_max(cs[c].depth);
            unsigned span = dmax + 1;
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    for (int k = 0; k < ch; k++)
                        put_sample(&src, x, y, k,
                                   (unsigned)((x * 3 + y * 5 + k * 7) % span));
            if (cs[c].color == IMG_PALETTE) {
                src.pal_ncolors = 1 << cs[c].depth;
                for (int i = 0; i < src.pal_ncolors; i++) {
                    src.palette[i * 3 + 0] = (uint8_t)(i * 11);
                    src.palette[i * 3 + 1] = (uint8_t)(i * 23);
                    src.palette[i * 3 + 2] = (uint8_t)(i * 31);
                    src.pal_alpha[i] = (uint8_t)(cs[c].pal_trns ? (i * 37) & 0xFF : 255);
                }
                src.has_pal_alpha = cs[c].pal_trns;
            }

            char path[64];
            snprintf(path, sizeof(path), "testout/pngrt_%s.png", cs[c].name);
            CHECK(png_write_file(&src, &opts, path, err, sizeof(err)) == 0, "write png");

            /* feed the bytes back through the PNG-input decoder */
            long n = -1;
            uint8_t *buf = NULL;
            FILE *f = img_fopen_read(path);
            if (f) {
                fseek(f, 0, SEEK_END);
                n = ftell(f);
                rewind(f);
                buf = (uint8_t *)malloc((size_t)n);
                if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
                    free(buf);
                    buf = NULL;
                }
                fclose(f);
            }
            img_image_t got;
            int okd = buf && png_decode_mem(buf, (size_t)n, &got, err, sizeof(err)) == 0;
            free(buf);
            CHECK(okd, "png_decode_mem");
            if (okd) {
                CHECK(got.color == src.color && got.bit_depth == src.bit_depth,
                      "color type and depth preserved");
                CHECK(got.width == w && got.height == h, "size preserved");
                int ok = 1;
                for (int y = 0; y < h && ok; y++)
                    for (int x = 0; x < w && ok; x++)
                        for (int k = 0; k < ch; k++)
                            if (get_sample(&got, x, y, k) != get_sample(&src, x, y, k))
                                ok = 0;
                CHECK(ok, "samples identical");
                if (cs[c].color == IMG_PALETTE) {
                    int pok = (got.pal_ncolors == src.pal_ncolors);
                    for (int i = 0; i < src.pal_ncolors && pok; i++) {
                        if (got.palette[i * 3 + 0] != src.palette[i * 3 + 0] ||
                            got.palette[i * 3 + 1] != src.palette[i * 3 + 1] ||
                            got.palette[i * 3 + 2] != src.palette[i * 3 + 2])
                            pok = 0;
                        if (cs[c].pal_trns && got.pal_alpha[i] != src.pal_alpha[i])
                            pok = 0;
                    }
                    CHECK(pok, "palette (and tRNS) preserved");
                }
                img_free(&got);
            }
            img_free(&src);
        }
    }

    /* --- PNG input: gray+tRNS is kept, RGB+tRNS expands to RGBA --- */
    printf("PNG input tRNS handling:\n");
    {
        img_image_t src;
        memset(&src, 0, sizeof(src));
        src.width = 8; src.height = 4; src.color = IMG_GRAY; src.bit_depth = 8;
        src.rowstride = img_rowstride(8, 8, 1);
        src.data = (uint8_t *)calloc(4, src.rowstride);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 8; x++)
                put_sample(&src, x, y, 0, (unsigned)(x * 8 + y));
        src.has_gray_trns = 1;
        src.gray_trns_value = 42;
        CHECK(png_write_file(&src, &opts, "testout/gray_trns.png", err, sizeof(err)) == 0,
              "write gray+tRNS png");
        img_free(&src);

        long n = -1;
        uint8_t *buf = NULL;
        FILE *f = img_fopen_read("testout/gray_trns.png");
        if (f) {
            fseek(f, 0, SEEK_END); n = ftell(f); rewind(f);
            buf = (uint8_t *)malloc((size_t)n);
            if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
            fclose(f);
        }
        img_image_t got;
        int okd = buf && png_decode_mem(buf, (size_t)n, &got, err, sizeof(err)) == 0;
        free(buf);
        CHECK(okd, "decode gray+tRNS");
        if (okd) {
            CHECK(got.color == IMG_GRAY && got.bit_depth == 8 &&
                  got.has_gray_trns && got.gray_trns_value == 42,
                  "gray + tRNS preserved");
            img_free(&got);
        }

        /* RGB + tRNS: transparency is kept by expanding to RGBA.  The project
         * encoder never writes RGB tRNS, so build that PNG with libpng. */
        {
            const int rw = 4, rh = 2;
            FILE *wf = img_fopen_write("testout/rgb_trns.png");
            png_err_t pe2;
            png_structp png = wf ? png_create_write_struct(PNG_LIBPNG_VER_STRING, &pe2,
                                                           pngerr_error, pngerr_warn) : NULL;
            png_infop info = png ? png_create_info_struct(png) : NULL;
            int wrote = 0;
            if (png && info && setjmp(pe2.jb) == 0) {
                png_init_io(png, wf);
                png_set_IHDR(png, info, (png_uint_32)rw, (png_uint_32)rh, 8,
                             PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                             PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
                png_color_16 trns;
                memset(&trns, 0, sizeof(trns));
                trns.red = 0; trns.green = 0; trns.blue = 7;
                png_set_tRNS(png, info, NULL, 0, &trns);
                png_write_info(png, info);
                uint8_t raw[4 * 3 * 2];
                png_bytep rows[2];
                for (int y = 0; y < rh; y++) {
                    for (int x = 0; x < rw; x++) {
                        raw[(y * rw + x) * 3 + 0] = (uint8_t)(x * 20);
                        raw[(y * rw + x) * 3 + 1] = (uint8_t)(y * 30);
                        raw[(y * rw + x) * 3 + 2] = 7;
                    }
                    rows[y] = raw + y * rw * 3;
                }
                png_write_image(png, rows);
                png_write_end(png, NULL);
                wrote = 1;
            }
            if (png) png_destroy_write_struct(&png, &info);
            if (wf) fclose(wf);
            CHECK(wrote, "write rgb+tRNS png");

            long rn = -1;
            uint8_t *rbuf = NULL;
            FILE *rf = img_fopen_read("testout/rgb_trns.png");
            if (rf) {
                fseek(rf, 0, SEEK_END); rn = ftell(rf); rewind(rf);
                rbuf = (uint8_t *)malloc((size_t)rn);
                if (!rbuf || fread(rbuf, 1, (size_t)rn, rf) != (size_t)rn) {
                    free(rbuf); rbuf = NULL;
                }
                fclose(rf);
            }
            img_image_t rt;
            int rok = rbuf && png_decode_mem(rbuf, (size_t)rn, &rt, err, sizeof(err)) == 0;
            free(rbuf);
            CHECK(rok, "decode rgb+tRNS");
            if (rok) {
                CHECK(rt.color == IMG_RGBA && rt.bit_depth == 8,
                      "RGB+tRNS expands to RGBA8");
                CHECK(get_sample(&rt, 0, 0, 3) == 0 &&
                      get_sample(&rt, 1, 0, 3) == 255,
                      "tRNS colour becomes transparent");
                img_free(&rt);
            }
        }
    }

    /* --- metadata round trips ------------------------------------------- */
    printf("Metadata: JPEG -> PNG (EXIF, split ICC, XMP, comment, density):\n");
    {
        uint8_t exif[512];
        size_t exif_len = make_exif(exif, sizeof(exif));
        CHECK(exif_len > 0, "build EXIF payload");

        uint8_t icc[TEST_ICC_SIZE];
        make_icc(icc, sizeof(icc), "JPEG");

        const char *xmp = "<x:xmpmeta>meta-test</x:xmpmeta>";
        CHECK(gen_jpeg_meta("testout/meta.jpg", exif, exif_len,
                            icc, sizeof(icc), xmp) == 0, "generate jpeg with markers");

        FILE *f = img_fopen_read("testout/meta.jpg");
        img_image_t img;
        int okd = f && jpeg_decode(f, &img, err, sizeof(err)) == 0;
        if (f) fclose(f);
        CHECK(okd, "decode jpeg");
        if (okd) {
            CHECK(img.meta.have_dpi && (int)(img.meta.dpi_x + 0.5) == 300 &&
                  (int)(img.meta.dpi_y + 0.5) == 300, "JFIF density read (300 dpi)");
            CHECK(img.meta.exif && img.meta.exif_len == exif_len, "EXIF captured");
            CHECK(img.meta.exif && !memcmp(img.meta.exif, exif, exif_len),
                  "EXIF bytes intact");
            CHECK(img.meta.icc && img.meta.icc_len == sizeof(icc),
                  "ICC reassembled from two APP2 parts");
            CHECK(img.meta.icc && !memcmp(img.meta.icc, icc, sizeof(icc)),
                  "ICC bytes intact across the split");
            CHECK(img.meta.xmp && img.meta.xmp_len == strlen(xmp) &&
                  !memcmp(img.meta.xmp, xmp, strlen(xmp)), "XMP captured");
            CHECK(img.meta.ntext == 1 && img.meta.text[0].key &&
                  !strcmp(img.meta.text[0].key, "Comment"), "JPEG comment captured");

            CHECK(png_write_file(&img, &opts, "testout/meta_jpg.png",
                                 err, sizeof(err)) == 0, "encode png");
            img_free(&img);

            png_meta_t pm;
            if (read_png_meta("testout/meta_jpg.png", &pm) == 0) {
                CHECK(pm.have_phys && pm.ppm_x == 11811 && pm.ppm_y == 11811,
                      "pHYs = 300 dpi (11811 px/m)");
                CHECK(pm.exif_len == exif_len, "eXIf carried into the PNG");
                CHECK(pm.icc_len == sizeof(icc), "iCCP carried into the PNG");
                CHECK(meta_has_key(&pm, "XML:com.adobe.xmp"), "XMP written as iTXt");
                CHECK(meta_has_key(&pm, "Comment"), "comment written as tEXt");
            } else {
                CHECK(0, "read back jpeg metadata");
            }
        }
    }

    printf("Metadata: PNG -> PNG (eXIf, iCCP, four text chunk kinds, pHYs):\n");
    {
        uint8_t exif[512];
        size_t exif_len = make_exif(exif, sizeof(exif));
        uint8_t icc[TEST_ICC_SIZE];
        make_icc(icc, sizeof(icc), "PNGR");
        const char *xmp = "<x:xmpmeta>roundtrip</x:xmpmeta>";

        FILE *wf = img_fopen_write("testout/meta_in.png");
        png_err_t pe;
        png_structp png = wf ? png_create_write_struct(PNG_LIBPNG_VER_STRING, &pe,
                                                       pngerr_error, pngerr_warn) : NULL;
        png_infop info = png ? png_create_info_struct(png) : NULL;
        int wrote = 0;
        if (png && info && setjmp(pe.jb) == 0) {
            png_init_io(png, wf);
            png_set_IHDR(png, info, 4, 2, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                         PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
            png_set_pHYs(png, info, 11811, 11811, PNG_RESOLUTION_METER);
            png_set_eXIf_1(png, info, (png_uint_32)exif_len, (png_bytep)exif);
            png_set_iCCP(png, info, "test profile", PNG_COMPRESSION_TYPE_BASE,
                         (png_const_bytep)icc, (png_uint_32)sizeof(icc));
            png_text tx[4];
            memset(tx, 0, sizeof(tx));
            tx[0].compression = PNG_TEXT_COMPRESSION_NONE;
            tx[0].key = (png_charp)"Title";  tx[0].text = (png_charp)"plain text";
            tx[1].compression = PNG_TEXT_COMPRESSION_zTXt;
            tx[1].key = (png_charp)"Author"; tx[1].text = (png_charp)"compressed text";
            tx[2].compression = PNG_ITXT_COMPRESSION_NONE;
            tx[2].key = (png_charp)"Comment"; tx[2].text = (png_charp)"utf-8 text";
            tx[2].lang = (png_charp)"en"; tx[2].lang_key = (png_charp)"";
            tx[3].compression = PNG_ITXT_COMPRESSION_NONE;
            tx[3].key = (png_charp)"XML:com.adobe.xmp"; tx[3].text = (png_charp)xmp;
            tx[3].lang = (png_charp)""; tx[3].lang_key = (png_charp)"";
            png_set_text(png, info, tx, 4);
            png_write_info(png, info);
            uint8_t raw[4 * 3 * 2];
            png_bytep rows[2];
            for (int y = 0; y < 2; y++) {
                for (int x = 0; x < 4; x++) {
                    raw[(y * 4 + x) * 3 + 0] = (uint8_t)(x * 40);
                    raw[(y * 4 + x) * 3 + 1] = (uint8_t)(y * 80);
                    raw[(y * 4 + x) * 3 + 2] = 5;
                }
                rows[y] = raw + y * 4 * 3;
            }
            png_write_image(png, rows);
            png_write_end(png, NULL);
            wrote = 1;
        }
        if (png) png_destroy_write_struct(&png, &info);
        if (wf) fclose(wf);
        CHECK(wrote, "write source png carrying metadata");

        long n = -1;
        uint8_t *buf = NULL;
        FILE *f = img_fopen_read("testout/meta_in.png");
        if (f) {
            fseek(f, 0, SEEK_END); n = ftell(f); rewind(f);
            buf = (uint8_t *)malloc((size_t)n);
            if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
            fclose(f);
        }
        img_image_t img;
        int okd = buf && png_decode_mem(buf, (size_t)n, &img, err, sizeof(err)) == 0;
        free(buf);
        CHECK(okd, "decode png");
        if (okd) {
            CHECK(img.meta.have_dpi && (int)(img.meta.dpi_x + 0.5) == 300,
                  "pHYs -> 300 dpi");
            CHECK(img.meta.exif && img.meta.exif_len == exif_len, "eXIf read");
            CHECK(img.meta.icc && img.meta.icc_len == sizeof(icc), "iCCP read");
            CHECK(img.meta.xmp && img.meta.xmp_len == strlen(xmp), "XMP read");
            CHECK(img.meta.ntext == 3, "three text chunks kept (XMP split out)");
            int kinds_ok = 1;
            for (int i = 0; i < img.meta.ntext; i++) {
                const img_text_t *t = &img.meta.text[i];
                if (!t->key) { kinds_ok = 0; continue; }
                if (!strcmp(t->key, "Title") && t->kind != IMG_TEXT_TEXT) kinds_ok = 0;
                if (!strcmp(t->key, "Author") && t->kind != IMG_TEXT_ZTXT) kinds_ok = 0;
                if (!strcmp(t->key, "Comment") && t->kind != IMG_TEXT_ITXT) kinds_ok = 0;
            }
            CHECK(kinds_ok, "tEXt/zTXt/iTXt kinds preserved");

            CHECK(png_write_file(&img, &opts, "testout/meta_out.png",
                                 err, sizeof(err)) == 0, "re-encode png");
            img_free(&img);
            png_meta_t pm;
            if (read_png_meta("testout/meta_out.png", &pm) == 0) {
                CHECK(pm.have_phys && pm.ppm_x == 11811, "pHYs preserved");
                CHECK(pm.exif_len == exif_len, "eXIf preserved");
                CHECK(pm.icc_len == sizeof(icc), "iCCP preserved");
                CHECK(pm.ntext == 4, "all four text chunks present");
                CHECK(meta_has_key(&pm, "XML:com.adobe.xmp") &&
                      meta_has_key(&pm, "Title") && meta_has_key(&pm, "Author") &&
                      meta_has_key(&pm, "Comment"), "keys preserved");
            } else {
                CHECK(0, "read back png metadata");
            }
        }
    }

    printf("Metadata: TIFF -> PNG (resolution tags, ICC, XMP, EXIF rebuilt):\n");
    {
        uint8_t icc[TEST_ICC_SIZE];
        make_icc(icc, sizeof(icc), "TIFF");
        const char *xmp = "<x:xmpmeta>tiff</x:xmpmeta>";

        TIFF *tf = TIFFOpen("testout/meta.tiff", "w");
        CHECK(tf != NULL, "open tiff for writing");
        if (tf) {
            TIFFSetField(tf, TIFFTAG_IMAGEWIDTH, 4);
            TIFFSetField(tf, TIFFTAG_IMAGELENGTH, 2);
            TIFFSetField(tf, TIFFTAG_BITSPERSAMPLE, 8);
            TIFFSetField(tf, TIFFTAG_SAMPLESPERPIXEL, 1);
            TIFFSetField(tf, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
            TIFFSetField(tf, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
            TIFFSetField(tf, TIFFTAG_XRESOLUTION, (float)300.0);
            TIFFSetField(tf, TIFFTAG_YRESOLUTION, (float)300.0);
            TIFFSetField(tf, TIFFTAG_RESOLUTIONUNIT, (uint16_t)RESUNIT_INCH);
            TIFFSetField(tf, TIFFTAG_MAKE, "TESTCAM");
            TIFFSetField(tf, TIFFTAG_MODEL, "MODEL-1");
            TIFFSetField(tf, TIFFTAG_DATETIME, "2026:10:06 12:00:00");
            TIFFSetField(tf, TIFFTAG_ORIENTATION, (uint16_t)1);
            TIFFSetField(tf, TIFFTAG_ICCPROFILE, (uint32_t)sizeof(icc), icc);
            TIFFSetField(tf, TIFFTAG_XMLPACKET, (uint32_t)strlen(xmp), (void *)xmp);
            uint8_t rows[2][4] = { { 1, 2, 3, 4 }, { 5, 6, 7, 8 } };
            int wok = 1;
            for (int y = 0; y < 2 && wok; y++)
                wok = TIFFWriteScanline(tf, rows[y], (uint32_t)y, 0) >= 0;
            CHECK(wok, "write tiff with metadata tags");
            TIFFClose(tf);
        }
        FILE *f = img_fopen_read("testout/meta.tiff");
        img_image_t img;
        int okd = f && tiff_decode(f, &img, err, sizeof(err)) == 0;
        if (f) fclose(f);
        CHECK(okd, "decode tiff");
        if (okd) {
            CHECK(img.meta.have_dpi && (int)(img.meta.dpi_x + 0.5) == 300,
                  "resolution tags -> 300 dpi");
            CHECK(img.meta.icc && img.meta.icc_len == sizeof(icc), "ICCPROFILE tag read");
            CHECK(img.meta.xmp && img.meta.xmp_len == strlen(xmp), "XMLPACKET tag read");
            CHECK(img.meta.exif && img.meta.exif_len > 0, "EXIF rebuilt from IFD0");
            double rx = 0, ry = 0;
            int unit = 0;
            CHECK(img.meta.exif &&
                  img_exif_resolution(img.meta.exif, img.meta.exif_len, &rx, &ry, &unit) &&
                  (int)(rx + 0.5) == 300 && unit == 2,
                  "rebuilt EXIF carries the resolution");
            CHECK(img.meta.exif &&
                  contains_bytes(img.meta.exif, img.meta.exif_len, "TESTCAM") &&
                  contains_bytes(img.meta.exif, img.meta.exif_len, "MODEL-1") &&
                  contains_bytes(img.meta.exif, img.meta.exif_len, "2026:10:06 12:00:00"),
                  "rebuilt EXIF carries Make/Model/DateTime");
            img_free(&img);
        }
    }

    printf("Metadata: WebP -> PNG (ICCP / EXIF / XMP chunks):\n");
    {
        uint8_t exif[512];
        size_t exif_len = make_exif(exif, sizeof(exif));
        uint8_t icc[TEST_ICC_SIZE];
        make_icc(icc, sizeof(icc), "WEBP");
        const char *xmp = "<x:xmpmeta>webp</x:xmpmeta>";

        CHECK(gen_webp_meta("testout/meta.webp", icc, sizeof(icc),
                            exif, exif_len, xmp) == 0,
              "generate webp with metadata chunks");
        FILE *f = img_fopen_read("testout/meta.webp");
        img_image_t img;
        int okd = f && webp_decode(f, &img, err, sizeof(err)) == 0;
        if (f) fclose(f);
        CHECK(okd, "decode webp");
        if (okd) {
            CHECK(img.meta.exif && img.meta.exif_len == exif_len, "EXIF chunk read");
            CHECK(img.meta.icc && img.meta.icc_len == sizeof(icc), "ICCP chunk read");
            CHECK(img.meta.xmp && img.meta.xmp_len == strlen(xmp), "XMP chunk read");
            CHECK(png_write_file(&img, &opts, "testout/meta_webp.png",
                                 err, sizeof(err)) == 0, "encode png");
            img_free(&img);
            png_meta_t pm;
            if (read_png_meta("testout/meta_webp.png", &pm) == 0) {
                CHECK(pm.exif_len == exif_len && pm.icc_len == sizeof(icc) &&
                      meta_has_key(&pm, "XML:com.adobe.xmp"),
                      "all three carried into the PNG");
            } else {
                CHECK(0, "read back webp metadata");
            }
        }
    }

    printf("\n%s (%d failures)\n", g_failures ? "SELF-TEST FAILED" : "SELF-TEST PASSED", g_failures);
    return g_failures ? 1 : 0;
}
