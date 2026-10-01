#ifndef IMG2PNG_PNGERR_H
#define IMG2PNG_PNGERR_H

#include <stdlib.h>
#include <setjmp.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <png.h>

/* Context shared by the custom libpng error/warning callbacks. */
typedef struct {
    jmp_buf jb;
    char msg[256];
} png_err_t;

static void pngerr_error(png_structp png, png_const_charp msg)
{
    png_err_t *e = (png_err_t *)png_get_error_ptr(png);
    if (e) {
        snprintf(e->msg, sizeof(e->msg), "%s", msg);
        longjmp(e->jb, 1);
    }
    abort();
}

static void pngerr_warn(png_structp png, png_const_charp msg)
{
    (void)png;
    (void)msg;  /* warnings are ignored */
}

#endif
