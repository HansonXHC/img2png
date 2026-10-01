#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "convert.h"
#include "enc/pngenc.h"
#include "util/threads.h"

typedef struct {
    char **items;
    int count;
    int cap;
} strlist_t;

static void sl_append(strlist_t *l, const char *s)
{
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 256;
        l->items = (char **)realloc(l->items, (size_t)l->cap * sizeof(char *));
        if (!l->items) {
            fprintf(stderr, "img2png: out of memory\n");
            exit(1);
        }
    }
    l->items[l->count++] = _strdup(s);
}

static void sl_free(strlist_t *l)
{
    for (int i = 0; i < l->count; i++)
        free(l->items[i]);
    free(l->items);
}

/* Collect supported images from a directory, recursively. */
static void scan_dir(const char *dir, strlist_t *out)
{
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        char full[MAX_PATH];
        snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            scan_dir(full, out);
        else if (img2png_is_supported_file(full))
            sl_append(out, full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void add_input(const char *path, strlist_t *out)
{
    DWORD attr = GetFileAttributesA(path);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
        scan_dir(path, out);
    else
        sl_append(out, path);
}

typedef struct {
    char in_path[MAX_PATH];
    char out_path[MAX_PATH];
    int done;
    img2png_result_t result;
} job_t;

typedef struct {
    job_t *job;
    const png_opts_t *opts;
    int keep_time;
} job_arg_t;

static void job_run(void *arg)
{
    job_arg_t *a = (job_arg_t *)arg;
    job_t *j = a->job;
    img2png_convert(j->in_path, j->out_path, a->opts, a->keep_time, &j->result);
    j->done = 1;
}

static void usage(void)
{
    fprintf(stderr,
        "img2png - convert BMP/TGA/PNM/ICO/JPEG/PNG images to PNG (lossless)\n\n"
        "usage: img2png [options] file-or-folder...\n"
        "  -o PATH        output file (single input) or output directory (batch)\n"
        "  -l 0-9         zlib compression level (default 9 = best)\n"
        "  -f MODE        row filter: auto|none|sub|up|avg|paeth|all|fast (default auto)\n"
        "  -j N           parallel worker threads (default: CPU count)\n"
        "  --auto         optimize: drop useless alpha / detect gray\n"
        "  --no-keep-time do not copy creation/modification times from input\n"
        "  -h             this help\n\n"
        "folders are scanned recursively; PNG inputs are decoded and re-encoded\n"
        "with the chosen settings (in place without -o)\n"
        "default: level 9, adaptive filter, timestamps kept, bit depth matched to source\n");
}

static void make_out_name(const char *in, char *out, size_t outlen)
{
    const char *slash1 = strrchr(in, '/');
    const char *slash2 = strrchr(in, '\\');
    const char *sep = slash1 > slash2 ? slash1 : slash2;
    const char *base = sep ? sep + 1 : in;
    const char *dot = strrchr(base, '.');
    if (dot && dot != base)
        snprintf(out, outlen, "%.*s.png", (int)(dot - in), in);
    else
        snprintf(out, outlen, "%s.png", in);
}

static int out_path_is_png(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *slash1 = strrchr(path, '/');
    const char *slash2 = strrchr(path, '\\');
    const char *sep = slash1 > slash2 ? slash1 : slash2;
    return dot && (!sep || dot > sep) && !_stricmp(dot + 1, "png");
}

int main(int argc, char **argv)
{
    png_opts_t opts;
    opts.level = 9;
    opts.filter = PNGF_AUTO;
    opts.auto_optimize = 0;
    opts.recompress_png = 1;
    int keep_time = 1;
    int nthreads = (int)GetCurrentProcessorNumber();
    const char *out_spec = NULL;
    strlist_t inputs = {0};

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage();
            return 0;
        } else if (!strcmp(a, "-o") && i + 1 < argc) {
            out_spec = argv[++i];
        } else if (!strcmp(a, "-l") && i + 1 < argc) {
            opts.level = atoi(argv[++i]);
            if (opts.level < 0 || opts.level > 9) {
                fprintf(stderr, "img2png: -l must be 0-9\n");
                return 2;
            }
        } else if (!strcmp(a, "-f") && i + 1 < argc) {
            const char *m = argv[++i];
            if      (!strcmp(m, "auto"))  opts.filter = PNGF_AUTO;
            else if (!strcmp(m, "none"))  opts.filter = PNGF_NONE;
            else if (!strcmp(m, "sub"))   opts.filter = PNGF_SUB;
            else if (!strcmp(m, "up"))    opts.filter = PNGF_UP;
            else if (!strcmp(m, "avg"))   opts.filter = PNGF_AVG;
            else if (!strcmp(m, "paeth")) opts.filter = PNGF_PAETH;
            else if (!strcmp(m, "all"))   opts.filter = PNGF_ALL;
            else if (!strcmp(m, "fast"))  opts.filter = PNGF_FAST;
            else { fprintf(stderr, "img2png: unknown filter '%s'\n", m); return 2; }
        } else if (!strcmp(a, "-j") && i + 1 < argc) {
            nthreads = atoi(argv[++i]);
            if (nthreads < 1 || nthreads > 256) {
                fprintf(stderr, "img2png: -j must be 1-256\n");
                return 2;
            }
        } else if (!strcmp(a, "-r") || !strcmp(a, "--recompress")) {
            /* PNG re-encoding is now the default; accepted for compatibility */
        } else if (!strcmp(a, "--auto")) {
            opts.auto_optimize = 1;
        } else if (!strcmp(a, "--keep-time")) {
            keep_time = 1;
        } else if (!strcmp(a, "--no-keep-time")) {
            keep_time = 0;
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "img2png: unknown option '%s'\n", a);
            usage();
            return 2;
        } else {
            add_input(a, &inputs);
        }
    }

    if (inputs.count == 0) {
        usage();
        return 2;
    }

    int nfiles = inputs.count;

    /* resolve output paths: explicit .png = single output, otherwise a dir;
     * no -o at all = next to each input */
    char out_dir[MAX_PATH] = "";
    char single_out[MAX_PATH] = "";
    int out_is_dir = 0;
    if (out_spec) {
        if (nfiles == 1 && out_path_is_png(out_spec)) {
            strcpy_s(single_out, sizeof(single_out), out_spec);
        } else {
            strcpy_s(out_dir, sizeof(out_dir), out_spec);
            CreateDirectoryA(out_dir, NULL);    /* ok if it already exists */
            out_is_dir = 1;
        }
    }

    job_t *jobs = (job_t *)calloc((size_t)nfiles, sizeof(job_t));
    job_arg_t *args = (job_arg_t *)calloc((size_t)nfiles, sizeof(job_arg_t));
    if (!jobs || !args) {
        fprintf(stderr, "img2png: out of memory\n");
        return 1;
    }

    for (int i = 0; i < nfiles; i++) {
        job_t *j = &jobs[i];
        strcpy_s(j->in_path, sizeof(j->in_path), inputs.items[i]);
        if (out_is_dir) {
            /* keep the relative structure when scanning a folder that is
             * also the input root */
            const char *src = inputs.items[i];
            const char *slash1 = strrchr(src, '/');
            const char *slash2 = strrchr(src, '\\');
            const char *sep = slash1 > slash2 ? slash1 : slash2;
            const char *base = sep ? sep + 1 : src;
            _snprintf(j->out_path, sizeof(j->out_path), "%s\\%s", out_dir, base);
            j->out_path[sizeof(j->out_path) - 1] = 0;
            char *dot = strrchr(j->out_path, '.');
            if (dot && dot > strrchr(j->out_path, '\\'))
                strcpy_s(dot, sizeof(j->out_path) - (size_t)(dot - j->out_path), ".png");
            else
                strcat_s(j->out_path, sizeof(j->out_path), ".png");
        } else if (single_out[0]) {
            strcpy_s(j->out_path, sizeof(j->out_path), single_out);
        } else {
            make_out_name(inputs.items[i], j->out_path, sizeof(j->out_path));
        }
        args[i].job = j;
        args[i].opts = &opts;
        args[i].keep_time = keep_time;
    }

    thread_pool_t *pool = pool_create(nthreads);
    if (!pool) {
        fprintf(stderr, "img2png: failed to create thread pool\n");
        return 1;
    }
    for (int i = 0; i < nfiles; i++)
        pool_submit(pool, job_run, &args[i]);

    /* report in input order as soon as each job finishes */
    int failures = 0;
    for (int i = 0; i < nfiles; i++) {
        while (!jobs[i].done)
            Sleep(5);
        job_t *j = &jobs[i];
        img2png_result_t *r = &j->result;
        if (r->ok) {
            const char *same = !strcmp(j->in_path, j->out_path);
            printf("[ ok ] %s (%s %d-bit %dx%d) -> %s  %llu -> %llu bytes, %.2fs\n",
                   j->in_path, img2png_color_name(r->out_color), r->out_depth,
                   r->out_w, r->out_h, same ? "(in place)" : j->out_path,
                   r->in_size, r->out_size, r->secs);
        } else {
            printf("[FAIL] %s: %s\n", j->in_path, r->err);
            failures++;
        }
        fflush(stdout);
    }
    pool_wait(pool);
    pool_destroy(pool);
    sl_free(&inputs);
    free(jobs);
    free(args);
    return failures ? 1 : 0;
}
