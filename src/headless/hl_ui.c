/* UI hooks the core calls; the headless build has no UI to update. */
#include <stdio.h>
#include <string.h>

#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/plat_unused.h>
#include <86box/ui.h>
#include <86box/version.h>

#include "hl.h"

int
ui_msgbox(int flags, char *message)
{
    return ui_msgbox_header(flags, NULL, message);
}

int
ui_msgbox_header(int flags, char *header, char *message)
{
    const char *kind = "info";

    if (flags & (MBX_ERROR | MBX_FATAL))
        kind = "error";
    else if (flags & MBX_WARNING)
        kind = "warning";
    fprintf(stderr, "[86box %s] %s%s%s\n", kind, header ? header : "", header ? ": " : "",
            message ? message : "");
    return 0;
}

/* No one to ask: name what is missing and refuse to start. */
int
ui_confirm_unsupported_hardware(const ui_unsupported_hardware_t *items, int count, UNUSED(int machine_missing))
{
    for (int i = 0; i < count; i++) {
        fprintf(stderr, "[86box error] ");
        fprintf(stderr, plat_get_string(items[i].kind), items[i].name);
        fprintf(stderr, "\n");
    }
    return count <= 0;
}

void
ui_notify(UNUSED(int what))
{
}

void
plat_ui_language(char *outbuf, UNUSED(int len))
{
    outbuf[0] = '\0';
}

void
ui_sb_update_icon_state(UNUSED(int tag), UNUSED(int state))
{
}

void
ui_sb_update_icon(UNUSED(int tag), UNUSED(int active))
{
}

void
ui_sb_update_icon_write(UNUSED(int tag), UNUSED(int active))
{
}

void
ui_sb_update_icon_wp(UNUSED(int tag), UNUSED(int state))
{
}

void
ui_sb_update_tip(UNUSED(int arg))
{
}

void
ui_sb_update_panes(void)
{
}

void
ui_sb_update_text(void)
{
}

void
ui_sb_set_text(UNUSED(char *wstr))
{
}

void
ui_sb_bugui(UNUSED(char *str))
{
}

void
ui_sb_set_ready(UNUSED(int ready))
{
}

void
ui_sb_mt32lcd(UNUSED(char *str))
{
}

void
ui_update_force_interpreter(void)
{
}

void
ui_init_monitor(UNUSED(int monitor_index))
{
}

void
ui_deinit_monitor(UNUSED(int monitor_index))
{
}

/* The I/O map is rebuilt on every hard reset, so our ports are re-added here. */
void
ui_hard_reset_completed(void)
{
    hl_ports_register();
    hl_prof_reset();
}

char *
ui_window_title(char *str)
{
    static char title[512] = EMU_NAME;

    if (str)
        strncpy(title, str, sizeof(title) - 1);
    return title;
}

void
plat_mouse_capture(UNUSED(int on))
{
}

void
plat_resize(UNUSED(int w), UNUSED(int h), UNUSED(int monitor_index))
{
}

void
plat_resize_request(UNUSED(int w), UNUSED(int h), UNUSED(int monitor_index))
{
}

int
plat_vidapi(UNUSED(const char *api))
{
    return 0;
}

void
startblit(void)
{
}

void
endblit(void)
{
}

/* Variables the core expects the frontend to own. */
int rctrl_is_lalt;
int update_icons;
int kbd_req_capture;
int hide_status_bar;
int hide_tool_bar;
int fixed_size_x = 640;
int fixed_size_y = 480;
int mouse_capture;

/* Called by the core on fatal conditions; ends the run like a guest exit. */
void
do_stop(void)
{
    hl_request_quit(1);
}
