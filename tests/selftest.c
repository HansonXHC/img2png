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
#include <tiffio.h>

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
            CHECK(img.color == IMG_RGBA && img.width == 5 && img.height == 3, "format is RGBA8 5x3");
            int diff = -1;
            for (int y = 0; y < 3 && diff < 0; y++)
                for (int x = 0; x < 5 && diff < 0; x++) {
                    uint8_t *d = img.data + ((size_t)y * 5 + x) * 4;
                    if (d[0] != (uint8_t)(x * 40) || d[1] != (uint8_t)(y * 80) || d[2] != 77 || d[3] != 255)
                        diff = y * 5 + x;
                }
            CHECK(diff < 0, "pixels identical (lossless)");
            png_write_file(&img, &opts, "testout/ttiff.png", err, sizeof(err));
            img_free(&img);
        }
    }

    printf("\n%s (%d failures)\n", g_failures ? "SELF-TEST FAILED" : "SELF-TEST PASSED", g_failures);
    return g_failures ? 1 : 0;
}
