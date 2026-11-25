/*
 * Control socket: one JSON object per line in, one per line out.
 * Slow commands (keys, wait_text, sleep) hold their client's reply until done;
 * a client has at most one request in flight.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cJSON.h>

#include <86box/86box.h>
#include <86box/timer.h>
#include <86box/device.h>
#include <86box/machine.h>
#include <86box/video.h>

#include "cpu.h"
#include "hl.h"

#define MAX_CLIENTS 16
#define LINE_MAX_LEN (1 << 20)
#define WAIT_POLL_NS 20000000ull /* re-read the screen at most every 20 ms of host time */

typedef enum wait_kind_t {
    WAIT_NONE,
    WAIT_KEYS,
    WAIT_TEXT,
    WAIT_EMU,
} wait_kind_t;

typedef struct client_t {
    int         fd;
    char       *in;
    size_t      in_len;
    wait_kind_t wait;
    regex_t     re;
    uint64_t    deadline_ns; /* host time for WAIT_TEXT */
    uint64_t    until_emu;   /* emulated time for WAIT_EMU */
    uint64_t    next_check;
} client_t;

static int      listen_fd = -1;
static char     sock_path[1024];
static client_t clients[MAX_CLIENTS];

static void
send_line(int fd, const char *s)
{
    const size_t n = strlen(s);
    size_t       off = 0;

    while (off < n) {
        const ssize_t w = write(fd, s + off, n - off);

        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return;
        off += (size_t) w;
    }
    (void) !write(fd, "\n", 1);
}

static void
reply(int fd, cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);

    if (s) {
        send_line(fd, s);
        cJSON_free(s);
    }
    cJSON_Delete(obj);
}

static cJSON *
ok_obj(void)
{
    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "ok", 1);
    return o;
}

static void
reply_error(int fd, const char *msg)
{
    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "ok", 0);
    cJSON_AddStringToObject(o, "error", msg);
    reply(fd, o);
}

static const char *
str_arg(const cJSON *req, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *) req, key);

    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static double
num_arg(const cJSON *req, const char *key, double def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *) req, key);

    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static void
cmd_status(int fd)
{
    cJSON *o = ok_obj();
    char   cpu[256];
    int    is_text, mode, cols, rows, cx, cy;
    char  *txt;

    snprintf(cpu, sizeof(cpu), "%s %s", cpu_f->name, cpu_s->name);
    cJSON_AddStringToObject(o, "state", hl_paused() ? "paused" : "running");
    cJSON_AddStringToObject(o, "machine", machine_getname(machine));
    cJSON_AddStringToObject(o, "machine_id", machine_get_internal_name());
    cJSON_AddStringToObject(o, "cpu", cpu);
    cJSON_AddNumberToObject(o, "cpu_hz", cpu_s->rspeed);
    cJSON_AddNumberToObject(o, "emu_ms", (double) (hl_emu_ns() / 1000000ull));
    cJSON_AddNumberToObject(o, "tsc", (double) tsc);
    cJSON_AddBoolToObject(o, "unthrottled", hl_unthrottled());
    cJSON_AddNumberToObject(o, "cr0", cr0);
    cJSON_AddBoolToObject(o, "cache_int", cpu_cache_int_enabled);
    cJSON_AddBoolToObject(o, "cache_ext", cpu_cache_ext_enabled);
    txt = hl_screen_text(&is_text, &mode, &cols, &rows, &cx, &cy);
    free(txt);
    cJSON_AddNumberToObject(o, "video_mode", mode);
    cJSON_AddBoolToObject(o, "text_mode", is_text);
    reply(fd, o);
}

static void
cmd_screen_text(int fd)
{
    cJSON *o = ok_obj();
    int    is_text, mode, cols, rows, cx, cy;
    char  *txt = hl_screen_text(&is_text, &mode, &cols, &rows, &cx, &cy);

    cJSON_AddBoolToObject(o, "text_mode", is_text);
    cJSON_AddNumberToObject(o, "video_mode", mode);
    if (txt) {
        cJSON_AddNumberToObject(o, "cols", cols);
        cJSON_AddNumberToObject(o, "rows", rows);
        cJSON_AddNumberToObject(o, "cursor_x", cx);
        cJSON_AddNumberToObject(o, "cursor_y", cy);
        cJSON_AddStringToObject(o, "text", txt);
        free(txt);
    }
    reply(fd, o);
}

static void
cmd_screenshot(int fd, const cJSON *req)
{
    const char *path = str_arg(req, "path");
    int         w = 0, h = 0, rc;

    if (!path) {
        reply_error(fd, "path required");
        return;
    }
    rc = hl_screenshot(path, &w, &h);
    if (rc == -2) {
        reply_error(fd, "no frame yet");
    } else if (rc) {
        reply_error(fd, "cannot write file");
    } else {
        cJSON *o = ok_obj();

        cJSON_AddStringToObject(o, "path", path);
        cJSON_AddNumberToObject(o, "width", w);
        cJSON_AddNumberToObject(o, "height", h);
        reply(fd, o);
    }
}

/* Returns 0 if the request queued something to wait for. */
static int
cmd_keys(client_t *c, const cJSON *req)
{
    const char  *text = str_arg(req, "text");
    const cJSON *keys = cJSON_GetObjectItemCaseSensitive((cJSON *) req, "keys");
    const cJSON *k;

    if (cJSON_GetObjectItemCaseSensitive((cJSON *) req, "delay_ms"))
        hl_keys_set_delay((int) num_arg(req, "delay_ms", 10));
    if (text && hl_keys_text(text)) {
        reply_error(c->fd, "untypable character or key queue full");
        return -1;
    }
    if (cJSON_IsArray(keys)) {
        cJSON_ArrayForEach(k, keys)
        {
            if (!cJSON_IsString(k) || hl_keys_named(k->valuestring)) {
                reply_error(c->fd, "unknown key");
                return -1;
            }
        }
    } else if (cJSON_IsString(keys) && hl_keys_named(keys->valuestring)) {
        reply_error(c->fd, "unknown key");
        return -1;
    }
    c->wait = WAIT_KEYS;
    return 0;
}

static int
cmd_wait_text(client_t *c, const cJSON *req)
{
    const char *pat = str_arg(req, "pattern");

    if (!pat || regcomp(&c->re, pat, REG_EXTENDED | REG_NEWLINE | REG_NOSUB)) {
        reply_error(c->fd, "bad or missing pattern (POSIX extended regex)");
        return -1;
    }
    c->deadline_ns = hl_now_ns() + (uint64_t) (num_arg(req, "timeout", 30.0) * 1e9);
    c->next_check  = 0;
    c->wait        = WAIT_TEXT;
    return 0;
}

/* Bytes above 0x7f become the Latin-1 code point of the same value. */
static char *
latin1_to_utf8(const char *raw, size_t n)
{
    char  *out = malloc(n * 2 + 1);
    size_t o   = 0;

    for (size_t i = 0; i < n; i++) {
        const uint8_t b = (uint8_t) raw[i];

        if (b < 0x80) {
            out[o++] = (char) b;
        } else {
            out[o++] = (char) (0xc0 | (b >> 6));
            out[o++] = (char) (0x80 | (b & 0x3f));
        }
    }
    out[o] = '\0';
    return out;
}

static void
cmd_debug_out(int fd)
{
    char  *raw;
    size_t n   = hl_debug_take(&raw);
    char  *out = latin1_to_utf8(raw, n);
    cJSON *obj = ok_obj();

    cJSON_AddStringToObject(obj, "data", out);
    cJSON_AddNumberToObject(obj, "bytes", (double) n);
    free(raw);
    free(out);
    reply(fd, obj);
}

static void
client_close(client_t *c)
{
    if (c->wait == WAIT_TEXT)
        regfree(&c->re);
    close(c->fd);
    free(c->in);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

static void
handle_line(client_t *c, const char *line)
{
    cJSON      *req = cJSON_Parse(line);
    const char *cmd;

    if (!req) {
        reply_error(c->fd, "invalid JSON");
        return;
    }
    cmd = str_arg(req, "cmd");
    if (!cmd) {
        reply_error(c->fd, "cmd required");
    } else if (!strcmp(cmd, "status")) {
        cmd_status(c->fd);
    } else if (!strcmp(cmd, "screen_text")) {
        cmd_screen_text(c->fd);
    } else if (!strcmp(cmd, "screenshot")) {
        cmd_screenshot(c->fd, req);
    } else if (!strcmp(cmd, "keys")) {
        cmd_keys(c, req);
    } else if (!strcmp(cmd, "wait_text")) {
        cmd_wait_text(c, req);
    } else if (!strcmp(cmd, "sleep")) {
        c->until_emu = hl_emu_ns() + (uint64_t) (num_arg(req, "ms", 0) * 1e6);
        c->wait      = WAIT_EMU;
    } else if (!strcmp(cmd, "pause")) {
        hl_set_paused(1);
        reply(c->fd, ok_obj());
    } else if (!strcmp(cmd, "resume")) {
        hl_set_paused(0);
        reply(c->fd, ok_obj());
    } else if (!strcmp(cmd, "reset")) {
        pc_reset_hard();
        reply(c->fd, ok_obj());
    } else if (!strcmp(cmd, "debug_out")) {
        cmd_debug_out(c->fd);
    } else if (!strcmp(cmd, "profile_start")) {
        hl_prof_start((uint32_t) num_arg(req, "interval_us", 20));
        reply(c->fd, ok_obj());
    } else if (!strcmp(cmd, "profile_stop")) {
        const char *path = str_arg(req, "path");
        uint64_t    n;
        uint32_t    distinct, iv;

        if (!path || !hl_prof_active()) {
            reply_error(c->fd, path ? "profiler not running" : "path required");
        } else if (hl_prof_stop(path, &n, &distinct, &iv)) {
            reply_error(c->fd, "cannot write file");
        } else {
            cJSON *o = ok_obj();

            cJSON_AddNumberToObject(o, "interval_us", iv);
            cJSON_AddNumberToObject(o, "samples", (double) n);
            cJSON_AddNumberToObject(o, "distinct", distinct);
            reply(c->fd, o);
        }
    } else if (!strcmp(cmd, "mem_read")) {
        const char *path = str_arg(req, "path");
        double      addr = num_arg(req, "addr", -1), len = num_arg(req, "len", -1);

        if (!path || addr < 0 || addr > 4294967295.0 || len < 0 || len > (16 << 20)) {
            reply_error(c->fd, "need path, addr (0..4G) and len (0..16M)");
        } else {
            uint8_t *buf = hl_mem_read((uint32_t) addr, (uint32_t) len);
            FILE    *fp  = buf ? fopen(path, "wb") : NULL;

            if (!buf) {
                reply_error(c->fd, "unmapped address");
            } else if (!fp || fwrite(buf, 1, (size_t) len, fp) != (size_t) len) {
                reply_error(c->fd, "cannot write file");
                if (fp)
                    fclose(fp);
            } else {
                cJSON *o = ok_obj();

                fclose(fp);
                cJSON_AddNumberToObject(o, "len", len);
                reply(c->fd, o);
            }
            free(buf);
        }
    } else if (!strcmp(cmd, "quit")) {
        reply(c->fd, ok_obj());
        hl_request_quit((int) num_arg(req, "code", 0));
    } else {
        reply_error(c->fd, "unknown cmd");
    }
    cJSON_Delete(req);
}

/* Runs every complete line the client has sent, stopping when one has to wait. */
static void
client_run_lines(client_t *c)
{
    char *nl;

    while (c->wait == WAIT_NONE && c->in && (nl = memchr(c->in, '\n', c->in_len))) {
        const size_t len = (size_t) (nl - c->in);
        char        *line = strndup(c->in, len);

        memmove(c->in, nl + 1, c->in_len - len - 1);
        c->in_len -= len + 1;
        if (len)
            handle_line(c, line);
        free(line);
    }
}

static void
client_read(client_t *c)
{
    char    chunk[4096];
    ssize_t n = read(c->fd, chunk, sizeof(chunk));

    if (n <= 0) {
        client_close(c);
        return;
    }
    if (c->in_len + (size_t) n > LINE_MAX_LEN) {
        client_close(c);
        return;
    }
    c->in = realloc(c->in, c->in_len + (size_t) n);
    memcpy(c->in + c->in_len, chunk, (size_t) n);
    c->in_len += (size_t) n;
    client_run_lines(c);
}

static void
client_check_wait(client_t *c, uint64_t now)
{
    switch (c->wait) {
        case WAIT_KEYS:
            if (hl_keys_idle()) {
                c->wait = WAIT_NONE;
                reply(c->fd, ok_obj());
            }
            break;
        case WAIT_EMU:
            if (hl_emu_ns() >= c->until_emu) {
                c->wait = WAIT_NONE;
                reply(c->fd, ok_obj());
            }
            break;
        case WAIT_TEXT:
            if (now < c->next_check)
                break;
            c->next_check = now + WAIT_POLL_NS;
            {
                int    is_text, mode, cols, rows, cx, cy;
                char  *txt     = hl_screen_text(&is_text, &mode, &cols, &rows, &cx, &cy);
                int    matched = txt && !regexec(&c->re, txt, 0, NULL, 0);

                if (matched || now >= c->deadline_ns) {
                    cJSON *o = matched ? ok_obj() : cJSON_CreateObject();

                    if (!matched) {
                        cJSON_AddBoolToObject(o, "ok", 0);
                        cJSON_AddStringToObject(o, "error", "timeout");
                    }
                    cJSON_AddBoolToObject(o, "matched", matched);
                    if (txt)
                        cJSON_AddStringToObject(o, "text", txt);
                    regfree(&c->re);
                    c->wait = WAIT_NONE;
                    reply(c->fd, o);
                }
                free(txt);
            }
            break;
        default:
            break;
    }
    if (c->wait == WAIT_NONE)
        client_run_lines(c);
}

int
hl_ctl_open(const char *path)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };

    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "socket path too long: %s\n", path);
        return -1;
    }
    for (int i = 0; i < MAX_CLIENTS; i++)
        clients[i].fd = -1;
    strcpy(addr.sun_path, path);
    strcpy(sock_path, path);
    unlink(path);
    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0 || bind(listen_fd, (struct sockaddr *) &addr, sizeof(addr)) || listen(listen_fd, 8)) {
        perror("control socket");
        return -1;
    }
    fcntl(listen_fd, F_SETFL, O_NONBLOCK);
    return 0;
}

void
hl_ctl_announce_exit(int code)
{
    /* Debug bytes still unread would otherwise be lost with the process. */
    char  *raw;
    size_t n   = hl_debug_take(&raw);
    char  *out = latin1_to_utf8(raw, n);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0) {
            cJSON *o = cJSON_CreateObject();

            cJSON_AddStringToObject(o, "event", "exit");
            cJSON_AddNumberToObject(o, "code", code);
            cJSON_AddNumberToObject(o, "emu_ms", (double) (hl_emu_ns() / 1000000ull));
            cJSON_AddStringToObject(o, "data", out);
            reply(clients[i].fd, o);
        }
    }
    free(raw);
    free(out);
}

void
hl_ctl_close(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0)
            client_close(&clients[i]);
    if (listen_fd >= 0)
        close(listen_fd);
    if (sock_path[0])
        unlink(sock_path);
    listen_fd = -1;
}

/* Waits up to timeout_ms for socket activity, serves it, and re-checks pending waits. */
void
hl_ctl_poll(int timeout_ms)
{
    struct pollfd pfd[MAX_CLIENTS + 1];
    int           map[MAX_CLIENTS + 1];
    int           n = 0;
    uint64_t      now;

    if (listen_fd < 0)
        return;
    pfd[n]   = (struct pollfd) { listen_fd, POLLIN, 0 };
    map[n++] = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0) {
            pfd[n]   = (struct pollfd) { clients[i].fd, POLLIN, 0 };
            map[n++] = i;
        }
    }
    if (poll(pfd, (nfds_t) n, timeout_ms) > 0) {
        for (int k = 0; k < n; k++) {
            if (!(pfd[k].revents & (POLLIN | POLLHUP)))
                continue;
            if (map[k] < 0) {
                const int fd = accept(listen_fd, NULL, NULL);

                if (fd < 0)
                    continue;
                int placed = 0;
                for (int i = 0; i < MAX_CLIENTS && !placed; i++) {
                    if (clients[i].fd < 0) {
                        clients[i].fd = fd;
                        placed        = 1;
#ifdef SO_NOSIGPIPE
                        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &(int) { 1 }, sizeof(int));
#endif
                    }
                }
                if (!placed)
                    close(fd);
            } else {
                client_read(&clients[map[k]]);
            }
        }
    }
    now = hl_now_ns();
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0)
            client_check_wait(&clients[i], now);
}
