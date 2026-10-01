#include "image.h"
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && !defined(IMG2PNG_FORCE_POSIX)
#include <windows.h>
#define IMG2PNG_WINDOWS 1
#endif

size_t img_rowstride(int width, int bit_depth, int channels)
{
    size_t bits = (size_t)width * (size_t)bit_depth * (size_t)channels;
    return (bits + 7) / 8;
}

int img_channels(img_color_t color)
{
    switch (color) {
    case IMG_GRAY:       return 1;
    case IMG_GRAY_ALPHA: return 2;
    case IMG_PALETTE:    return 1;
    case IMG_RGB:        return 3;
    case IMG_RGBA:       return 4;
    }
    return 0;
}

void img_free(img_image_t *img)
{
    if (img) {
        free(img->data);
        img->data = NULL;
    }
}

#ifdef IMG2PNG_WINDOWS
static wchar_t *acp_to_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w)
        return NULL;
    MultiByteToWideChar(CP_ACP, 0, s, -1, w, n);
    return w;
}

FILE *img_fopen_read(const char *path)
{
    wchar_t *w = acp_to_wide(path);
    FILE *f = w ? _wfopen(w, L"rb") : NULL;
    free(w);
    return f;
}

FILE *img_fopen_write(const char *path)
{
    wchar_t *w = acp_to_wide(path);
    FILE *f = w ? _wfopen(w, L"wb") : NULL;
    free(w);
    return f;
}

void *img_open_read_attrs(const char *path)
{
    wchar_t *w = acp_to_wide(path);
    HANDLE h = w ? CreateFileW(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
                 : NULL;
    free(w);
    return h;
}

void *img_open_write_attrs(const char *path)
{
    wchar_t *w = acp_to_wide(path);
    HANDLE h = w ? CreateFileW(w, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
                 : NULL;
    free(w);
    return h;
}

void img_close_handle(void *handle)
{
    if (handle)
        CloseHandle((HANDLE)handle);
}
#else /* POSIX: plain byte paths */

FILE *img_fopen_read(const char *path)
{
    return fopen(path, "rb");
}

FILE *img_fopen_write(const char *path)
{
    return fopen(path, "wb");
}

void *img_open_read_attrs(const char *path)
{
    return fopen(path, "rb");
}

void *img_open_write_attrs(const char *path)
{
    return fopen(path, "r+b");
}

void img_close_handle(void *handle)
{
    if (handle)
        fclose((FILE *)handle);
}

#endif

/* --auto mode: drop a fully opaque alpha channel and collapse gray-looking
 * RGB to GRAY.  Returns 1 if anything was changed. */
static int row_all_opaque_rgba(const uint8_t *d, int w)
{
    for (int x = 0; x < w; x++)
        if (d[x * 4 + 3] != 255)
            return 0;
    return 1;
}

static int row_all_gray(const uint8_t *d, int w, int ch, int sixteen)
{
    for (int x = 0; x < w; x++) {
        const uint8_t *p = d + (size_t)x * ch;
        unsigned r, g, b;
        if (sixteen) {
            r = img_ld16be(p); g = img_ld16be(p + 2); b = img_ld16be(p + 4);
        } else {
            r = p[0]; g = p[1]; b = p[2];
        }
        if (r != g || g != b)
            return 0;
    }
    return 1;
}

int img_auto_optimize(img_image_t *img)
{
    if (!img || !img->data)
        return 0;
    int changed = 0;
    int w = img->width, h = img->height;

    if (img->color == IMG_RGBA && img->bit_depth == 8) {
        int opaque = 1;
        for (int y = 0; y < h && opaque; y++)
            opaque = row_all_opaque_rgba(img->data + (size_t)y * img->rowstride, w);
        if (opaque) {
            for (int y = 0; y < h; y++) {
                const uint8_t *s = img->data + (size_t)y * img->rowstride;
                uint8_t *d = img->data + (size_t)y * (size_t)w * 3;
                for (int x = 0; x < w; x++)
                    memcpy(d + (size_t)x * 3, s + (size_t)x * 4, 3);
            }
            img->color = IMG_RGB;
            img->rowstride = (size_t)w * 3;
            changed = 1;
        }
    }

    if ((img->color == IMG_RGB) && (img->bit_depth == 8 || img->bit_depth == 16)) {
        int gray = 1;
        for (int y = 0; y < h && gray; y++)
            gray = row_all_gray(img->data + (size_t)y * img->rowstride, w,
                                3, img->bit_depth == 16);
        if (gray) {
            size_t newstride = (size_t)w * (img->bit_depth == 16 ? 2 : 1);
            for (int y = 0; y < h; y++) {
                const uint8_t *s = img->data + (size_t)y * img->rowstride;
                uint8_t *d = img->data + (size_t)y * newstride;
                if (img->bit_depth == 16) {
                    for (int x = 0; x < w; x++) {
                        uint16_t v = (uint16_t)img_ld16be(s + (size_t)x * 6);
                        img_st16be(d + (size_t)x * 2, v);
                    }
                } else {
                    for (int x = 0; x < w; x++)
                        d[x] = s[(size_t)x * 3];
                }
            }
            img->color = IMG_GRAY;
            img->rowstride = newstride;
            changed = 1;
        }
    }

    if (img->color == IMG_PALETTE && img->has_pal_alpha) {
        int any_trans = 0;
        for (int i = 0; i < img->pal_ncolors; i++)
            if (img->pal_alpha[i] != 255) { any_trans = 1; break; }
        if (!any_trans) {
            img->has_pal_alpha = 0;
            changed = 1;
        }
    }

    return changed;
}

