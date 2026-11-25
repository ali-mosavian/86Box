/*
 * Headless frontend: shared declarations.
 *
 * One thread runs the emulator and the control socket, so commands never
 * race the CPU.
 */
#ifndef HL_H
#define HL_H

#include <stddef.h>
#include <stdint.h>

#define HL_EXIT_PORT  0x0501 /* guest writes a status byte here to end the run */
#define HL_DEBUG_PORT 0x00E9 /* Bochs debug port, collected for debug_out */

uint64_t hl_now_ns(void);
uint64_t hl_emu_ns(void);
void     hl_request_quit(int code);
int      hl_quit_requested(void);
int      hl_exit_code(void);
int      hl_paused(void);
void     hl_set_paused(int paused);
int      hl_unthrottled(void);

/* hl_ports.c: debug and exit ports, registered after every hard reset. */
void   hl_ports_register(void);
size_t hl_debug_take(char **buf);
size_t hl_debug_pending(void);

/* hl_video.c */
void  hl_video_init(void);
void  hl_video_close(void);
int   hl_screenshot(const char *path, int *w, int *h);
char *hl_screen_text(int *is_text, int *mode, int *cols, int *rows, int *cx, int *cy);

/* hl_keys.c */
int  hl_keys_text(const char *text);
int  hl_keys_named(const char *spec);
void hl_keys_set_delay(int ms);
void hl_keys_step(void);
int  hl_keys_idle(void);

/* hl_ctl.c */
int  hl_ctl_open(const char *path);
void hl_ctl_close(void);
void hl_ctl_poll(int timeout_ms);
void hl_ctl_announce_exit(int code);

#endif
