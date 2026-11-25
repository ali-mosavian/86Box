/*
 * Keyboard injection as XT (set 1) scancodes. Events wait in a queue and are
 * fed to the guest one per delay of *emulated* time so a fast sender cannot
 * overflow the keyboard controller.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <86box/86box.h>
#include <86box/keyboard.h>

#include "hl.h"

#define QUEUE_LEN 8192

#define SC_LCTRL  0x1d
#define SC_LSHIFT 0x2a
#define SC_LALT   0x38
#define SC_ENTER  0x1c
#define SC_EXT    0x100

typedef struct event_t {
    uint16_t scan;
    uint8_t  down;
} event_t;

static event_t  queue[QUEUE_LEN];
static unsigned head;
static unsigned tail;
static int      delay_ms = 10;
static uint64_t next_ns;

typedef struct named_t {
    const char *name;
    uint16_t    scan;
} named_t;

static const named_t named[] = {
    { "esc", 0x01 }, { "escape", 0x01 }, { "enter", SC_ENTER }, { "return", SC_ENTER },
    { "tab", 0x0f }, { "space", 0x39 }, { "backspace", 0x0e }, { "bksp", 0x0e },
    { "ctrl", SC_LCTRL }, { "control", SC_LCTRL }, { "shift", SC_LSHIFT },
    { "alt", SC_LALT }, { "capslock", 0x3a }, { "numlock", 0x45 }, { "scrolllock", 0x46 },
    { "f1", 0x3b }, { "f2", 0x3c }, { "f3", 0x3d }, { "f4", 0x3e }, { "f5", 0x3f },
    { "f6", 0x40 }, { "f7", 0x41 }, { "f8", 0x42 }, { "f9", 0x43 }, { "f10", 0x44 },
    { "f11", 0x57 }, { "f12", 0x58 },
    { "home", SC_EXT | 0x47 }, { "up", SC_EXT | 0x48 }, { "pageup", SC_EXT | 0x49 },
    { "pgup", SC_EXT | 0x49 }, { "left", SC_EXT | 0x4b }, { "right", SC_EXT | 0x4d },
    { "end", SC_EXT | 0x4f }, { "down", SC_EXT | 0x50 }, { "pagedown", SC_EXT | 0x51 },
    { "pgdn", SC_EXT | 0x51 }, { "insert", SC_EXT | 0x52 }, { "ins", SC_EXT | 0x52 },
    { "delete", SC_EXT | 0x53 }, { "del", SC_EXT | 0x53 },
    { "kp_enter", SC_EXT | SC_ENTER }, { "kp_plus", 0x4e }, { "kp_minus", 0x4a },
    { "kp_star", 0x37 },
};

/* US layout: unshifted and shifted characters of the main block, by scancode. */
static const char row_plain[]   = "\x1b" "1234567890-=\b\tqwertyuiop[]\n\0asdfghjkl;'`\0\\zxcvbnm,./";
static const char row_shifted[] = "\x1b" "!@#$%^&*()_+\b\tQWERTYUIOP{}\n\0ASDFGHJKL:\"~\0|ZXCVBNM<>?";

static int
queue_push(uint16_t scan, int down)
{
    const unsigned nt = (tail + 1) % QUEUE_LEN;

    if (nt == head)
        return -1;
    queue[tail] = (event_t) { scan, (uint8_t) down };
    tail        = nt;
    return 0;
}

static int
tap(uint16_t scan, int shift)
{
    if (shift && queue_push(SC_LSHIFT, 1))
        return -1;
    if (queue_push(scan, 1) || queue_push(scan, 0))
        return -1;
    if (shift && queue_push(SC_LSHIFT, 0))
        return -1;
    return 0;
}

/* Scancode (1-based index into the rows) of a character, or 0. */
static int
char_scan(char ch, int *shift)
{
    const char *p;

    if (ch == ' ') {
        *shift = 0;
        return 0x39;
    }
    if (ch == '\r' || ch == '\0')
        return 0;
    if ((p = memchr(row_plain, ch, sizeof(row_plain) - 1)) && ch != '\0') {
        *shift = 0;
        return (int) (p - row_plain) + 1;
    }
    if ((p = memchr(row_shifted, ch, sizeof(row_shifted) - 1)) && ch != '\0') {
        *shift = 1;
        return (int) (p - row_shifted) + 1;
    }
    return 0;
}

/* Queues the text as keystrokes; '\n' is Enter. Returns -1 on an untypable character or full queue. */
int
hl_keys_text(const char *text)
{
    for (const char *p = text; *p; p++) {
        int shift = 0;
        int scan  = char_scan(*p, &shift);

        if (!scan || tap((uint16_t) scan, shift))
            return -1;
    }
    return 0;
}

static int
lookup(const char *name)
{
    char one[2] = { 0 };

    if (strlen(name) == 1) {
        int shift = 0;
        one[0]    = (char) tolower((unsigned char) name[0]);
        return char_scan(one[0], &shift);
    }
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++)
        if (!strcasecmp(named[i].name, name))
            return named[i].scan;
    return 0;
}

/* A key name or a '+' chain such as "Ctrl+Alt+Del": modifiers go down in order, release in reverse. */
int
hl_keys_named(const char *spec)
{
    char     buf[128];
    uint16_t codes[8];
    int      n = 0;
    char    *tok, *save = NULL;

    if (strlen(spec) >= sizeof(buf))
        return -1;
    if (!strcmp(spec, "+")) {
        return tap(0x4e, 0);
    }
    strcpy(buf, spec);
    for (tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
        const int sc = lookup(tok);

        if (!sc || n == 8)
            return -1;
        codes[n++] = (uint16_t) sc;
    }
    if (!n)
        return -1;
    for (int i = 0; i < n; i++)
        if (queue_push(codes[i], 1))
            return -1;
    for (int i = n - 1; i >= 0; i--)
        if (queue_push(codes[i], 0))
            return -1;
    return 0;
}

void
hl_keys_set_delay(int ms)
{
    delay_ms = ms < 1 ? 1 : ms;
}

int
hl_keys_idle(void)
{
    return head == tail;
}

/* Called once per emulated frame. */
void
hl_keys_step(void)
{
    if (head == tail || hl_emu_ns() < next_ns)
        return;
    keyboard_input(queue[head].down, queue[head].scan);
    head    = (head + 1) % QUEUE_LEN;
    next_ns = hl_emu_ns() + (uint64_t) delay_ms * 1000000ull;
}
