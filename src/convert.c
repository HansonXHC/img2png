#include "convert.h"
#include "image.h"
#include "dec/decode.h"
#include "enc/pngenc.h"
#include "util/filetime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifdef _WIN32
#define img_stricmp _stricmp
#else
#define img_stricmp strcasecmp
#endif
#include "util/platform.h"

typedef enum { FMT_UNKNOWN = 0, FMT_BMP, FMT_TGA, FMT_PNM, FMT_ICO, FMT_JPEG,
               FMT_PNG, FMT_GIF, FMT_QOI, FMT_WEBP, FMT_TIFF } fmt_t;

static fmt_t fmt_from_ext(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *slash1 = strrchr(path, '/');
    const char *slash2 = strrchr(path, '\\');
    const char *sep = slash1 > slash2 ? slash1 : slash2;
    if (!dot || (sep && dot < sep))
        return FMT_UNKNOWN;
    dot++;
    if (!img_stricmp(dot, "bmp") || !img_stricmp(dot, "dib")) return FMT_BMP;
    if (!img_stricmp(dot, "tga"))                          return FMT_TGA;
    if (!img_stricmp(dot, "pnm") || !img_stricmp(dot, "ppm") ||
        !img_stricmp(dot, "pgm") || !img_stricmp(dot, "pbm")) return FMT_PNM;
    if (!img_stricmp(dot, "ico"))                          return FMT_ICO;
    if (!img_stricmp(dot, "jpg") || !img_stricmp(dot, "jpeg") ||
        !img_stricmp(dot, "jfif"))                         return FMT_JPEG;
    if (!img_stricmp(dot, "png"))                          return FMT_PNG;
    if (!img_stricmp(dot, "gif"))                          return FMT_GIF;
    if (!img_stricmp(dot, "qoi"))                          return FMT_QOI;
    if (!img_stricmp(dot, "webp"))                         return FMT_WEBP;
    if (!img_stricmp(dot, "tif") || !img_stricmp(dot, "tiff")) return FMT_TIFF;
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
    if (n >= 6 && !memcmp(b, "GIF8", 4)) return FMT_GIF;
    if (n >= 4 && !memcmp(b, "qoif", 4)) return FMT_QOI;
    if (n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WEBP", 4)) return FMT_WEBP;
    if (n >= 4 && ((!memcmp(b, "II*", 3) && 1) || 0)) return FMT_TIFF; /* placeholder */
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
#if defined(_WIN32) && !defined(IMG2PNG_FORCE_POSIX)
    HANDLE h = CreateFileA(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    LARGE_INTEGER sz = {0};
    GetFileSizeEx(h, &sz);
    CloseHandle(h);
    return (unsigned long long)sz.QuadPart;
#else
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    return (unsigned long long)st.st_size;
#endif
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

    double t0 = img_now_sec(), t1;

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
        /* verify the magic first: a ".png"-named file may actually be some
         * other format; sniff and convert that instead when recognizable */
        static const uint8_t png_sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        uint8_t sig[8] = {0};
        size_t got = fread(sig, 1, 8, f);
        rewind(f);
        if (got < 8 || memcmp(sig, png_sig, 8) != 0) {
            if (sig[0] == 0xFF && sig[1] == 0xD8)
                fmt = FMT_JPEG;
            else if (sig[0] == 'B' && sig[1] == 'M')
                fmt = FMT_BMP;
            else if (sig[0] == 0 && sig[1] == 0 && sig[2] == 1 && sig[3] == 0)
                fmt = FMT_ICO;
            else if (sig[0] == 'P' && sig[1] >= '1' && sig[1] <= '6')
                fmt = FMT_PNM;
            else {
                fclose(f);
                snprintf(result->err, sizeof(result->err),
                         "file has a .png name but is not a PNG (and no other known format detected)");
                return -1;
            }
        }
    }

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
        case FMT_GIF:  rc = gif_decode(f, &img, result->err, sizeof(result->err)); break;
        case FMT_QOI:  rc = qoi_decode_file(f, &img, result->err, sizeof(result->err)); break;
        case FMT_WEBP: rc = webp_decode(f, &img, result->err, sizeof(result->err)); break;
        case FMT_TIFF: rc = tiff_decode(f, &img, result->err, sizeof(result->err)); break;
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

    t1 = img_now_sec();
    result->secs = t1 - t0;
    result->in_size = in_size;
    result->out_size = file_size(out_path);
    result->ok = 1;
    return 0;
}
