/*
 * Frame capture and text-screen reading.
 *
 * The blit callback copies each frame into memory; nothing is drawn.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <png.h>

#include <86box/86box.h>
#include <86box/timer.h>
#include <86box/mem.h>
#include <86box/device.h>
#include <86box/video.h>
#include <86box/vid_svga.h>

#include "hl.h"
#include "hl_cp437.h"

#define MAX_DIM 2048

static pthread_mutex_t frame_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t       *frame;
static int             frame_w;
static int             frame_h;

static void
blit(int x, int y, int w, int h, int monitor_index)
{
    if (monitor_index == 0 && buffer32 && x >= 0 && y >= 0 && w > 0 && h > 0
        && w <= MAX_DIM && h <= MAX_DIM) {
        pthread_mutex_lock(&frame_lock);
        for (int row = 0; row < h; row++)
            memcpy(&frame[row * MAX_DIM], &buffer32->line[y + row][x], (size_t) w * sizeof(uint32_t));
        frame_w = w;
        frame_h = h;
        pthread_mutex_unlock(&frame_lock);
    }
    video_blit_complete_monitor(monitor_index);
}

void
hl_video_init(void)
{
    frame = calloc((size_t) MAX_DIM * MAX_DIM, sizeof(uint32_t));
    video_setblit(blit);
}

void
hl_video_close(void)
{
    video_setblit(NULL);
}

static int
write_png(const char *path, const uint32_t *px, int w, int h)
{
    FILE       *fp  = fopen(path, "wb");
    png_structp png = NULL;
    png_infop   inf = NULL;
    png_bytep   row = NULL;

    if (!fp)
        return -1;
    png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    inf = png ? png_create_info_struct(png) : NULL;
    if (!inf || setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &inf);
        free(row);
        fclose(fp);
        return -1;
    }
    row = malloc((size_t) w * 3);
    png_init_io(png, fp);
    png_set_IHDR(png, inf, w, h, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    png_write_info(png, inf);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint32_t p = px[(size_t) y * MAX_DIM + x];
            row[x * 3]       = (p >> 16) & 0xff;
            row[x * 3 + 1]   = (p >> 8) & 0xff;
            row[x * 3 + 2]   = p & 0xff;
        }
        png_write_row(png, row);
    }
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &inf);
    free(row);
    return fclose(fp);
}

static int
write_ppm(const char *path, const uint32_t *px, int w, int h)
{
    FILE *fp = fopen(path, "wb");

    if (!fp)
        return -1;
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint32_t p = px[(size_t) y * MAX_DIM + x];
            const uint8_t  c[3] = { (p >> 16) & 0xff, (p >> 8) & 0xff, p & 0xff };
            fwrite(c, 1, 3, fp);
        }
    }
    return fclose(fp);
}

/* Writes the last frame; ".ppm" selects PPM, anything else PNG. Returns 0 on success, -2 if no frame yet. */
int
hl_screenshot(const char *path, int *w, int *h)
{
    int         rc;
    const char *dot = strrchr(path, '.');

    pthread_mutex_lock(&frame_lock);
    if (frame_w == 0) {
        pthread_mutex_unlock(&frame_lock);
        return -2;
    }
    *w = frame_w;
    *h = frame_h;
    rc = (dot && !strcmp(dot, ".ppm")) ? write_ppm(path, frame, frame_w, frame_h)
                                       : write_png(path, frame, frame_w, frame_h);
    pthread_mutex_unlock(&frame_lock);
    return rc;
}

static size_t
put_utf8(char *out, uint16_t cp)
{
    if (cp < 0x80) {
        out[0] = (char) cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char) (0xc0 | (cp >> 6));
        out[1] = (char) (0x80 | (cp & 0x3f));
        return 2;
    }
    out[0] = (char) (0xe0 | (cp >> 12));
    out[1] = (char) (0x80 | ((cp >> 6) & 0x3f));
    out[2] = (char) (0x80 | (cp & 0x3f));
    return 3;
}

static uint8_t
peek(uint32_t addr)
{
    return mem_readb_phys(addr);
}

/*
 * Reads the text screen from video memory using the BIOS data area for the
 * geometry. Returns a malloc'd UTF-8 string with one line per row (trailing
 * blanks trimmed), or NULL when the mode is not text. The VGA read latches
 * are restored so the guest cannot notice.
 */
char *
hl_screen_text(int *is_text, int *mode, int *cols, int *rows, int *cx, int *cy)
{
    svga_t  *svga    = svga_get_pri();
    latch_t  saved   = { 0 };
    const int bmode  = peek(0x449);
    const int ncols  = peek(0x44a) | (peek(0x44b) << 8);
    int       nrows  = peek(0x484) + 1;
    const int page   = peek(0x462);
    /* The CRTC start address is what the card shows; the BIOS copy can lag behind it. */
    const int start  = svga ? ((svga->crtc[0x0c] << 8) | svga->crtc[0x0d]) * 2 : peek(0x44e) | (peek(0x44f) << 8);
    const int cpos   = peek(0x450 + page * 2) | (peek(0x451 + page * 2) << 8);
    const int text   = (bmode <= 3 || bmode == 7) && !(svga && (svga->attrregs[0x10] & 1));
    uint32_t  base   = (bmode == 7) ? 0xb0000 : 0xb8000;
    char     *out;
    size_t    n = 0;

    *mode = bmode;
    *cols = ncols;
    *is_text = text;
    if (nrows < 2 || nrows > 100)
        nrows = 25;
    *rows = nrows;
    *cx = cpos % (ncols ? ncols : 80);
    *cy = cpos / (ncols ? ncols : 80);
    if (!text || ncols <= 0 || ncols > 255)
        return NULL;

    if (svga)
        saved = svga->latch;
    base += (uint32_t) start;
    out = malloc((size_t) nrows * (ncols * 3 + 1) + 1);
    for (int r = 0; r < nrows; r++) {
        const size_t line = n;
        size_t       keep = n;

        for (int c = 0; c < ncols; c++) {
            const uint8_t ch = peek(base + (uint32_t) (r * ncols + c) * 2);

            n += put_utf8(out + n, hl_cp437[ch]);
            if (ch != ' ' && ch != 0 && hl_cp437[ch] != ' ')
                keep = n;
        }
        n = keep > line ? keep : line;
        out[n++] = '\n';
    }
    out[n] = '\0';
    if (svga)
        svga->latch = saved;
    return out;
}
