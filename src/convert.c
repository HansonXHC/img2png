#include "convert.h"
#include "image.h"
#include "dec/decode.h"
#include "enc/pngenc.h"
#include "util/filetime.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

typedef enum { FMT_UNKNOWN = 0, FMT_BMP, FMT_TGA, FMT_PNM, FMT_ICO, FMT_JPEG, FMT_PNG } fmt_t;

static fmt_t fmt_from_ext(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *slash1 = strrchr(path, '/');
    const char *slash2 = strrchr(path, '\\');
    const char *sep = slash1 > slash2 ? slash1 : slash2;
    if (!dot || (sep && dot < sep))
        return FMT_UNKNOWN;
    dot++;
    if (!_stricmp(dot, "bmp") || !_stricmp(dot, "dib")) return FMT_BMP;
    if (!_stricmp(dot, "tga"))                          return FMT_TGA;
    if (!_stricmp(dot, "pnm") || !_stricmp(dot, "ppm") ||
        !_stricmp(dot, "pgm") || !_stricmp(dot, "pbm")) return FMT_PNM;
    if (!_stricmp(dot, "ico"))                          return FMT_ICO;
    if (!_stricmp(dot, "jpg") || !_stricmp(dot, "jpeg") ||
        !_stricmp(dot, "jfif"))                         return FMT_JPEG;
    if (!_stricmp(dot, "png"))                          return FMT_PNG;
    return FMT_UNKNOWN;
}

static fmt_t sniff_format(const char *path)
{
    FILE *f = img_fopen_read(path);
    if (!f)
        return FMT_UNKNOWN;
    uint8_t b[8] = {0};
    size_t n = fread(b, 1, 8, f);
    fclose(f);
    if (n >= 2 && b[0] == 'B' && b[1] == 'M') return FMT_BMP;
    if (n >= 2 && b[0] == 0xFF && b[1] == 0xD8) return FMT_JPEG;
    if (n >= 4 && b[0] == 0 && b[1] == 0 && b[2] == 1 && b[3] == 0) return FMT_ICO;
    if (n >= 2 && b[0] == 'P' && b[1] >= '1' && b[1] <= '6') return FMT_PNM;
    return FMT_UNKNOWN;
}

const char *img2png_color_name(int color)
{
    static const char *names[] = { "GRAY", "GRAY_ALPHA", "PALETTE", "RGB", "RGBA" };
    if (color < 0 || color > 4)
        return "?";
    return names[color];
}

/* Extension-based test used by directory scanners (CLI batch and GUI drop). */
int img2png_is_supported_file(const char *path)
{
    return fmt_from_ext(path) != FMT_UNKNOWN;
}

static unsigned long long file_size(const char *path)
{
    HANDLE h = CreateFileA(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    LARGE_INTEGER sz = {0};
    GetFileSizeEx(h, &sz);
    CloseHandle(h);
    return (unsigned long long)sz.QuadPart;
}

int img2png_convert(const char *in_path, const char *out_path,
                    const png_opts_t *opts, int keep_time,
                    img2png_result_t *result)
{
    memset(result, 0, sizeof(*result));

    fmt_t fmt = fmt_from_ext(in_path);
    if (fmt == FMT_UNKNOWN)
        fmt = sniff_format(in_path);
    if (fmt == FMT_UNKNOWN) {
        snprintf(result->err, sizeof(result->err), "unsupported input format");
        return -1;
    }

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    /* save the source's times before any (possibly in-place) write */
    uint8_t ctime[8] = {0}, mtime[8] = {0};
    int have_times = keep_time && get_file_times(in_path, ctime, mtime) == 0;
    unsigned long long in_size = file_size(in_path);

    FILE *f = img_fopen_read(in_path);
    if (!f) {
        snprintf(result->err, sizeof(result->err), "cannot open input file");
        return -1;
    }

    img_image_t img;
    int rc;
    if (fmt == FMT_PNG) {
        /* PNG input: read the whole file, decode it, re-encode with new settings */
        long fsize = -1;
        if (fseek(f, 0, SEEK_END) == 0) {
            fsize = ftell(f);
            rewind(f);
        }
        uint8_t *buf = (fsize > 0) ? (uint8_t *)malloc((size_t)fsize) : NULL;
        if (!buf || fread(buf, 1, (size_t)fsize, f) != (size_t)fsize) {
            free(buf);
            fclose(f);
            snprintf(result->err, sizeof(result->err), "cannot read PNG input");
            return -1;
        }
        fclose(f);
        rc = png_decode_mem(buf, (size_t)fsize, &img,
                            result->err, sizeof(result->err));
        free(buf);
    } else {
        switch (fmt) {
        case FMT_BMP:  rc = bmp_decode(f, &img, result->err, sizeof(result->err)); break;
        case FMT_TGA:  rc = tga_decode(f, &img, result->err, sizeof(result->err)); break;
        case FMT_PNM:  rc = pnm_decode(f, &img, result->err, sizeof(result->err)); break;
        case FMT_ICO:  rc = ico_decode(f, &img, result->err, sizeof(result->err)); break;
        case FMT_JPEG: rc = jpeg_decode(f, &img, result->err, sizeof(result->err)); break;
        default:
            snprintf(result->err, sizeof(result->err), "unsupported input format");
            rc = -1;
            break;
        }
        fclose(f);
    }
    if (rc != 0)
        return -1;

    if (opts->auto_optimize)
        img_auto_optimize(&img);

    result->out_w = img.width;
    result->out_h = img.height;
    result->out_depth = img.bit_depth;
    result->out_color = img.color;

    rc = png_write_file(&img, opts, out_path, result->err, sizeof(result->err));
    img_free(&img);
    if (rc != 0)
        return -1;

    if (have_times)
        set_file_times(out_path, ctime, mtime);

    QueryPerformanceCounter(&t1);
    result->secs = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    result->in_size = in_size;
    result->out_size = file_size(out_path);
    result->ok = 1;
    return 0;
}
