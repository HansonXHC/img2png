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
        img_meta_free(&img->meta);
    }
}

/* ------------------------------- metadata ------------------------------- */

void img_meta_free(img_meta_t *m)
{
    if (!m)
        return;
    free(m->exif);
    free(m->icc);
    free(m->xmp);
    for (int i = 0; i < m->ntext; i++) {
        free(m->text[i].key);
        free(m->text[i].value);
    }
    free(m->text);
    memset(m, 0, sizeof(*m));
}

int img_meta_set_blob(uint8_t **dst, size_t *dstlen, const void *src, size_t len)
{
    uint8_t *copy = NULL;
    if (len) {
        copy = (uint8_t *)malloc(len);
        if (!copy)
            return -1;
        memcpy(copy, src, len);
    }
    free(*dst);
    *dst = copy;
    *dstlen = len;
    return 0;
}

/* Little/big-endian reader over a TIFF-format EXIF payload. */
typedef struct {
    const uint8_t *p;
    size_t         len;
    int            le;
} exif_rd_t;

static unsigned exif_u16(const exif_rd_t *r, size_t off)
{
    if (off + 2 > r->len)
        return 0;
    return r->le ? ((unsigned)r->p[off] | ((unsigned)r->p[off + 1] << 8))
                 : (((unsigned)r->p[off] << 8) | (unsigned)r->p[off + 1]);
}

static unsigned exif_u32(const exif_rd_t *r, size_t off)
{
    if (off + 4 > r->len)
        return 0;
    if (r->le)
        return (unsigned)r->p[off] | ((unsigned)r->p[off + 1] << 8) |
               ((unsigned)r->p[off + 2] << 16) | ((unsigned)r->p[off + 3] << 24);
    return ((unsigned)r->p[off] << 24) | ((unsigned)r->p[off + 1] << 16) |
           ((unsigned)r->p[off + 2] << 8) | (unsigned)r->p[off + 3];
}

static int exif_open(const uint8_t *exif, size_t len, exif_rd_t *r)
{
    if (!exif || len < 8)
        return 0;
    if (exif[0] == 'I' && exif[1] == 'I' && exif[2] == 42 && exif[3] == 0)
        r->le = 1;
    else if (exif[0] == 'M' && exif[1] == 'M' && exif[2] == 0 && exif[3] == 42)
        r->le = 0;
    else
        return 0;
    r->p = exif;
    r->len = len;
    return 1;
}

/* Walk IFD0, handing each entry to the callback. */
static void exif_walk_ifd0(const exif_rd_t *r,
                           void (*cb)(void *ctx, unsigned tag, unsigned type,
                                      unsigned count, size_t value_off,
                                      const exif_rd_t *rd),
                           void *ctx)
{
    size_t ifd = exif_u32(r, 4);
    if (ifd + 2 > r->len)
        return;
    unsigned n = exif_u16(r, ifd);
    for (unsigned i = 0; i < n; i++) {
        size_t e = ifd + 2 + (size_t)i * 12;
        if (e + 12 > r->len)
            return;
        unsigned tag = exif_u16(r, e);
        unsigned type = exif_u16(r, e + 2);
        unsigned count = exif_u32(r, e + 4);
        /* values longer than 4 bytes live at an offset */
        size_t vlen = 0;
        switch (type) {
        case 1: case 2: case 6: case 7: vlen = 1u * count; break;
        case 3: case 8:                 vlen = 2u * count; break;
        case 4: case 9: case 11:        vlen = 4u * count; break;
        case 5: case 10: case 12:       vlen = 8u * count; break;
        default:                        vlen = 0; break;
        }
        size_t voff = (vlen > 4) ? exif_u32(r, e + 8) : e + 8;
        cb(ctx, tag, type, count, voff, r);
    }
}

typedef struct {
    double  x, y;
    int     unit;
    int     got_x, got_y, got_unit;
} res_ctx_t;

static void res_cb(void *vctx, unsigned tag, unsigned type, unsigned count,
                   size_t voff, const exif_rd_t *rd)
{
    res_ctx_t *c = (res_ctx_t *)vctx;
    (void)count;
    if (tag == 282 && type == 5) {          /* XResolution, RATIONAL */
        unsigned num = exif_u32(rd, voff), den = exif_u32(rd, voff + 4);
        if (den) { c->x = (double)num / den; c->got_x = 1; }
    } else if (tag == 283 && type == 5) {   /* YResolution */
        unsigned num = exif_u32(rd, voff), den = exif_u32(rd, voff + 4);
        if (den) { c->y = (double)num / den; c->got_y = 1; }
    } else if (tag == 296 && type == 3) {   /* ResolutionUnit */
        c->unit = (int)exif_u16(rd, voff);
        c->got_unit = 1;
    }
}

int img_exif_resolution(const uint8_t *exif, size_t len,
                        double *x, double *y, int *unit)
{
    exif_rd_t r;
    if (!exif_open(exif, len, &r))
        return 0;
    res_ctx_t c;
    memset(&c, 0, sizeof(c));
    exif_walk_ifd0(&r, res_cb, &c);
    if (!c.got_x || !c.got_y)
        return 0;
    /* EXIF defaults to inches when ResolutionUnit is absent */
    int u = c.got_unit ? c.unit : 2;
    if (u == 3) {                           /* centimetres -> inches */
        c.x *= 2.54;
        c.y *= 2.54;
        u = 2;
    }
    if (u != 2 || c.x <= 0 || c.y <= 0)
        return 0;
    if (x) *x = c.x;
    if (y) *y = c.y;
    if (unit) *unit = u;
    return 1;
}

typedef struct { int orientation; } orient_ctx_t;

static void orient_cb(void *vctx, unsigned tag, unsigned type, unsigned count,
                      size_t voff, const exif_rd_t *rd)
{
    orient_ctx_t *c = (orient_ctx_t *)vctx;
    (void)count;
    if (tag == 274 && type == 3)
        c->orientation = (int)exif_u16(rd, voff);
}

int img_exif_orientation(const uint8_t *exif, size_t len)
{
    exif_rd_t r;
    if (!exif_open(exif, len, &r))
        return 0;
    orient_ctx_t c = { 0 };
    exif_walk_ifd0(&r, orient_cb, &c);
    return c.orientation;
}

int img_meta_set_exif(img_meta_t *m, const void *src, size_t len)
{
    const uint8_t *p = (const uint8_t *)src;
    if (!p)
        return 0;
    if (len >= 6 && !memcmp(p, "Exif\0\0", 6)) {
        p += 6;
        len -= 6;
    }
    /* HEIF/AVIF prepend a 4-byte offset to the TIFF header */
    if (len >= 4 && !((p[0] == 'I' && p[1] == 'I') || (p[0] == 'M' && p[1] == 'M'))) {
        p += 4;
        len -= 4;
    }
    exif_rd_t r;
    if (!exif_open(p, len, &r))             /* must be a valid TIFF header */
        return 0;
    if (img_meta_set_blob(&m->exif, &m->exif_len, p, len) != 0)
        return 0;
    return 1;
}

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

int img_meta_add_text(img_meta_t *m, const char *key, const char *value,
                      int kind)
{
    img_text_t *nt = (img_text_t *)realloc(m->text,
                                           ((size_t)m->ntext + 1) * sizeof(*nt));
    if (!nt)
        return -1;
    m->text = nt;
    img_text_t *e = &m->text[m->ntext];
    e->key = key ? dup_str(key) : NULL;
    e->value = value ? dup_str(value) : NULL;
    e->kind = kind;
    if (!e->key || !e->value) {
        free(e->key);
        free(e->value);
        return -1;
    }
    m->ntext++;
    return 0;
}


void img_free_anim(img_animation_t *anim)
{
    if (!anim)
        return;
    for (int i = 0; i < anim->nframes; i++)
        img_free(&anim->frames[i]);
    free(anim->frames);
    free(anim->x);
    free(anim->y);
    free(anim->delays_cs);
    free(anim->dispose);
    memset(anim, 0, sizeof(*anim));
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

