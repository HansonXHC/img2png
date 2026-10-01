#include "decode.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static int fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen)
        snprintf(err, errlen, "pnm: %s", msg);
    return -1;
}

typedef struct {
    const uint8_t *buf;
    size_t size;
    size_t pos;
} scanner_t;

static void skip_ws_comments(scanner_t *s)
{
    for (;;) {
        while (s->pos < s->size && isspace(s->buf[s->pos]))
            s->pos++;
        if (s->pos < s->size && s->buf[s->pos] == '#') {
            while (s->pos < s->size && s->buf[s->pos] != '\n')
                s->pos++;
        } else {
            return;
        }
    }
}

static int next_uint(scanner_t *s, unsigned *out)
{
    skip_ws_comments(s);
    if (s->pos >= s->size || !isdigit(s->buf[s->pos]))
        return -1;
    unsigned v = 0;
    while (s->pos < s->size && isdigit(s->buf[s->pos])) {
        v = v * 10u + (unsigned)(s->buf[s->pos] - '0');
        if (v > 0x7FFFFFFFu)
            return -1;
        s->pos++;
    }
    *out = v;
    return 0;
}

int pnm_decode(FILE *f, img_image_t *img, char *err, size_t errlen)
{
    /* load the whole file: header tokens and (for P4-P6) raw data share it */
    if (fseek(f, 0, SEEK_END) != 0)
        return fail(err, errlen, "seek failed");
    long fsize = ftell(f);
    if (fsize < 2)
        return fail(err, errlen, "file too small");
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)fsize);
    if (!buf)
        return fail(err, errlen, "out of memory");
    if (fread(buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        free(buf);
        return fail(err, errlen, "read failed");
    }

    scanner_t s = { buf, (size_t)fsize, 0 };
    if (s.buf[s.pos] != 'P' || s.buf[s.pos + 1] < '1' || s.buf[s.pos + 1] > '6') {
        free(buf);
        return fail(err, errlen, "not a PNM file");
    }
    int type = s.buf[s.pos + 1] - '0';
    s.pos += 2;

    unsigned width = 0, height = 0, maxval = 1;
    if (type != 1 && type != 4) {
        if (next_uint(&s, &width) || next_uint(&s, &height) || next_uint(&s, &maxval)) {
            free(buf);
            return fail(err, errlen, "bad header");
        }
        if (maxval == 0 || maxval > 65535) {
            free(buf);
            return fail(err, errlen, "unsupported maxval");
        }
    } else {
        if (next_uint(&s, &width) || next_uint(&s, &height)) {
            free(buf);
            return fail(err, errlen, "bad header");
        }
    }
    if (width == 0 || height == 0) {
        free(buf);
        return fail(err, errlen, "bad dimensions");
    }

    memset(img, 0, sizeof(*img));
    img->width = (int)width;
    img->height = (int)height;
    int ascii = (type <= 3);

    int gray = (type == 1 || type == 2 || type == 4 || type == 5);
    if (gray) {
        img->color = IMG_GRAY;
        if (type == 1 || type == 4)
            img->bit_depth = 1;             /* PBM stays 1-bit */
        else
            img->bit_depth = (maxval > 255) ? 16 : 8;
    } else {
        img->color = IMG_RGB;
        img->bit_depth = (maxval > 255) ? 16 : 8;
    }

    int ch = img_channels(img->color);
    img->rowstride = img_rowstride(img->width, img->bit_depth, ch);
    img->data = (uint8_t *)calloc((size_t)height, img->rowstride);
    if (!img->data) { free(buf); return fail(err, errlen, "out of memory"); }

    int rc = 0;
    if (ascii) {
        unsigned nsamples = width * height * (gray ? 1u : 3u);
        unsigned depth_scale = (img->bit_depth == 16) ? 65535u : 255u;
        unsigned samples_per_px = gray ? 1u : 3u;
        for (unsigned i = 0; i < nsamples; i++) {
            unsigned v;
            if (next_uint(&s, &v)) { rc = -1; fail(err, errlen, "truncated pixel data"); break; }
            unsigned sample;
            if (type == 1 || type == 4)
                sample = v ? 0u : 1u;       /* 1 = black in PBM */
            else if (v > maxval) { rc = -1; fail(err, errlen, "sample exceeds maxval"); break; }
            else
                sample = (unsigned)((unsigned long long)v * depth_scale / maxval);
            size_t px_index = i / samples_per_px;
            size_t row = px_index / width;
            size_t col = px_index % width;
            unsigned cchan = gray ? 0u : i % 3u;
            uint8_t *d = img->data + row * img->rowstride;
            if (img->bit_depth == 16)
                img_st16be(d + col * (size_t)ch * 2 + cchan * 2, sample);
            else if (img->bit_depth == 1) {
                size_t byte = col >> 3;
                int shift = 7 - (int)(col & 7);
                if (sample)
                    d[byte] |= (uint8_t)(1u << shift);
                else
                    d[byte] &= (uint8_t)~(1u << shift);
            } else
                d[col * (size_t)ch + cchan] = (uint8_t)sample;
        }
    } else {
        /* binary: the single whitespace byte after maxval is the boundary,
         * raw pixel data starts immediately after it */
        s.pos++;
        if (type == 4) {
            size_t row_bytes = ((size_t)width + 7) / 8;
            if (s.size - s.pos < row_bytes * height) {
                free(buf); img_free(img);
                return fail(err, errlen, "truncated pixel data");
            }
            for (unsigned y = 0; y < height; y++) {
                const uint8_t *srow = buf + s.pos + (size_t)y * row_bytes;
                uint8_t *d = img->data + (size_t)y * img->rowstride;
                for (size_t b = 0; b < row_bytes; b++) {
                    uint8_t v = srow[b], o = 0;
                    for (int bit = 0; bit < 8; bit++) {
                        unsigned set = (v >> (7 - bit)) & 1u;
                        if (!set)       /* 1 = black -> sample 0 */
                            o |= (uint8_t)(1u << (7 - bit));
                    }
                    d[b] = o;
                }
            }
        } else {
            unsigned bytes_per = (maxval > 255) ? 2u : 1u;
            size_t nsamples = (size_t)width * height * (gray ? 1u : 3u);
            if (s.size - s.pos < nsamples * bytes_per) {
                free(buf); img_free(img);
                return fail(err, errlen, "truncated pixel data");
            }
            const uint8_t *p = buf + s.pos;
            unsigned depth_scale = (maxval > 255) ? 65535u : 255u;
            for (size_t i = 0; i < nsamples; i++) {
                unsigned v;
                if (bytes_per == 2) {
                    v = ((unsigned)p[0] << 8) | p[1];   /* PNM 16-bit is big-endian */
                    p += 2;
                } else {
                    v = p[0];
                    p += 1;
                }
                unsigned sample = (unsigned)((unsigned long long)v * depth_scale / maxval);
                size_t row = i / ((size_t)width * (gray ? 1u : 3u));
                size_t idx_in_row = i % ((size_t)width * (gray ? 1u : 3u));
                size_t col = idx_in_row / (gray ? 1u : 3u);
                unsigned cchan = gray ? 0u : (unsigned)(idx_in_row % 3u);
                uint8_t *d = img->data + row * img->rowstride;
                if (bytes_per == 2)
                    img_st16be(d + col * (size_t)ch * 2 + cchan * 2, sample);
                else
                    d[col * (size_t)ch + cchan] = (uint8_t)sample;
            }
        }
    }

    free(buf);
    return rc;
}
