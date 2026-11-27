/*
 * 86Box headless frontend: no window, no audio, driven over a unix socket.
 *
 * The emulator and the control socket share this one thread, run by the core's
 * run control loop (runctl.h). The CPU runs in 1 ms slices of emulated time
 * (pc_run); guest clocks derive from emulated cycles only, so the host pace
 * never reaches the guest.
 */
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <86box/86box.h>
#include <86box/timer.h>
#include <86box/device.h>
#include <86box/plat_unused.h>
#include <86box/hdd.h>
#include <86box/keyboard.h>
#include <86box/nvr.h>
#include <86box/plat.h>
#include <86box/runctl.h>
#include <86box/video.h>

#include "cpu.h"
#include "hl.h"

extern int exit_event;

volatile int cpu_thread_run = 1;
int          usb_activity;

static volatile sig_atomic_t quit_flag;
static volatile int          exit_code;
static uint64_t              emu_ns;
static unsigned              frame;

uint64_t
hl_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

uint64_t
plat_get_ns(void)
{
    return hl_now_ns();
}

uint64_t
hl_emu_ns(void)
{
    return emu_ns;
}

/* Safe from inside the CPU loop: the run ends at the next frame boundary. */
void
hl_request_quit(int code)
{
    if (!quit_flag) {
        exit_code = code;
        quit_flag = 1;
    }
    cycles -= 99999999;
    runctl_request_stop();
}

int
hl_quit_requested(void)
{
    return quit_flag;
}

int
hl_exit_code(void)
{
    return exit_code;
}

int
hl_paused(void)
{
    return runctl_paused();
}

/* Resume clears every reason the user's Resume may: a guest's ACPI sleep wakes. */
void
hl_set_paused(int p)
{
    if (p)
        runctl_pause(RUN_PAUSE_USER);
    else
        runctl_resume((run_pause_t) RUN_PAUSE_USER_CLEARABLE);
}

int
hl_unthrottled(void)
{
    return runctl_mode() == RUN_MODE_FAST;
}

static void
on_signal(UNUSED(int sig))
{
    quit_flag = 1;
}

/* After every pass of the run loop: one quantum ran, or none (paused or early). */
static void
loop_pass(void)
{
    const uint64_t step_us = force_10ms ? 10000 : 1000;
    static uint64_t done_us;
    run_status_t    st;

    if (quit_flag)
        runctl_request_stop();

    runctl_status(&st);
    for (; done_us < st.emu_us; done_us += step_us) {
        emu_ns += step_us * 1000;
        hl_keys_step();
        if ((++frame % (force_10ms ? 100 : 1000)) == 0)
            pc_onesec();
    }

    if (!hl_unthrottled() || st.state != RUN_RUNNING || (frame % 4) == 0)
        hl_ctl_poll(0);
}

static void
usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s --vm DIR --roms DIR --socket PATH [--unthrottled] [--paused]\n"
            "  --vm DIR        VM directory holding 86box.cfg, nvr/ and disk images\n"
            "  --roms DIR      ROM set directory\n"
            "  --socket PATH   unix socket for the JSON control protocol\n"
            "  --unthrottled   run as fast as the host allows (guest time stays emulated)\n"
            "  --paused        start paused until a resume command\n",
            prog);
}

/* Closes everything the way the windowed frontends do on quit, so images and NVR are written. */
static void
shutdown_emulator(void)
{
    hdd_image_sync_all();
    nvr_save();
    hl_video_close();
    is_quit = 1;
    pc_close(NULL);
}

int
main(int argc, char **argv)
{
    static const struct option longopts[] = {
        { "vm", required_argument, NULL, 'v' },
        { "roms", required_argument, NULL, 'r' },
        { "socket", required_argument, NULL, 's' },
        { "unthrottled", no_argument, NULL, 'u' },
        { "paused", no_argument, NULL, 'p' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    const char *vm_dir = NULL, *roms = NULL, *sock = NULL;
    char       *core_argv[16];
    int         core_argc = 0;
    int         opt;
    int         unthrottled = 0, paused = 0;

    while ((opt = getopt_long(argc, argv, "h", longopts, NULL)) != -1) {
        switch (opt) {
            case 'v': vm_dir = optarg; break;
            case 'r': roms = optarg; break;
            case 's': sock = optarg; break;
            case 'u': unthrottled = 1; break;
            case 'p': paused = 1; break;
            default:
                usage(argv[0]);
                return opt == 'h' ? 0 : 2;
        }
    }
    if (!vm_dir || !roms || !sock) {
        usage(argv[0]);
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);

    core_argv[core_argc++] = argv[0];
    core_argv[core_argc++] = "-N";
    core_argv[core_argc++] = "-P";
    core_argv[core_argc++] = (char *) vm_dir;
    core_argv[core_argc++] = "-R";
    core_argv[core_argc++] = (char *) roms;
    core_argv[core_argc]   = NULL;

    if (pc_init(core_argc, core_argv) == 0)
        return 1;
    if (!pc_init_roms()) {
        fprintf(stderr, "no usable ROM images in %s\n", roms);
        return 6;
    }
    pc_init_modules();
    hl_video_init();
    if (hl_ctl_open(sock))
        return 1;

    pc_reset_hard_init();
    is_quit = 0;
    if (unthrottled)
        runctl_set_mode(RUN_MODE_FAST);
    if (paused)
        runctl_pause(RUN_PAUSE_USER);
    runctl_thread_starting();
    runctl_thread_run_loop(loop_pass);

    hl_ctl_announce_exit(exit_code);
    shutdown_emulator();
    hl_ctl_close();
    return exit_code;
}
