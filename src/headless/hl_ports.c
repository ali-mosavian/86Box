/*
 * Guest-visible I/O ports of the headless build.
 *
 *   0x00E9  Bochs debug port: every byte written is kept for debug_out and
 *           echoed to stderr.
 *   0x0501  Exit port: writing a byte ends the run with that exit status.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <86box/86box.h>
#include <86box/io.h>
#include <86box/plat_unused.h>

#include "hl.h"

#define DEBUG_CAP (16u << 20)

static char  *dbg_buf;
static size_t dbg_len;
static size_t dbg_cap;

static void
debug_write(UNUSED(uint16_t port), uint8_t val, UNUSED(void *priv))
{
    fputc(val, stderr);
    if (dbg_len >= DEBUG_CAP)
        return;
    if (dbg_len == dbg_cap) {
        dbg_cap = dbg_cap ? dbg_cap * 2 : 4096;
        dbg_buf = realloc(dbg_buf, dbg_cap);
    }
    dbg_buf[dbg_len++] = (char) val;
}

static void
exit_write(UNUSED(uint16_t port), uint8_t val, UNUSED(void *priv))
{
    hl_request_quit(val);
}

static void
exit_write_w(UNUSED(uint16_t port), uint16_t val, UNUSED(void *priv))
{
    hl_request_quit(val & 0xff);
}

static void
exit_write_l(UNUSED(uint16_t port), uint32_t val, UNUSED(void *priv))
{
    hl_request_quit(val & 0xff);
}

void
hl_ports_register(void)
{
    io_sethandler(HL_DEBUG_PORT, 1, NULL, NULL, NULL, debug_write, NULL, NULL, NULL);
    io_sethandler(HL_EXIT_PORT, 1, NULL, NULL, NULL, exit_write, exit_write_w, exit_write_l, NULL);
}

/* Hands the collected bytes to the caller (who frees them) and clears them. */
size_t
hl_debug_take(char **buf)
{
    const size_t n = dbg_len;

    *buf    = dbg_buf;
    dbg_buf = NULL;
    dbg_len = dbg_cap = 0;
    return n;
}

size_t
hl_debug_pending(void)
{
    return dbg_len;
}
