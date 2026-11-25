/*
 * 86Box headless frontend: no window, no audio, driven over a unix socket.
 *
 * The emulator and the control socket share this one thread. The CPU runs in
 * 1 ms slices of emulated time (pc_run); guest clocks derive from emulated
 * cycles only, so the host pace never reaches the guest.
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
#include <86box/video.h>

#include "cpu.h"
#include "hl.h"

#define MAX_BEHIND_NS 50000000ull /* give up catching up after 50 ms */

extern int exit_event;

static volatile sig_atomic_t quit_flag;
static volatile int          exit_code;
static int                   paused;
static int                   unthrottled;
static uint64_t              emu_ns;

uint64_t
hl_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
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
    return paused;
}

void
hl_set_paused(int p)
{
    paused = p;
}

int
hl_unthrottled(void)
{
    return unthrottled;
}

static void
on_signal(UNUSED(int sig))
{
    quit_flag = 1;
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
    uint64_t    next, step_ns;
    unsigned    frame = 0;

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
    dopause = 0;
    step_ns = force_10ms ? 10000000ull : 1000000ull;
    next    = hl_now_ns();

    while (!quit_flag) {
        const uint64_t now = hl_now_ns();

        if (paused) {
            hl_ctl_poll(10);
            next = hl_now_ns();
            continue;
        }
        if (!unthrottled && now < next) {
            hl_ctl_poll((int) ((next - now + 999999ull) / 1000000ull));
            continue;
        }

        pc_run();
        emu_ns += step_ns;
        hl_keys_step();
        next += step_ns;
        if (!unthrottled && hl_now_ns() > next + MAX_BEHIND_NS)
            next = hl_now_ns();

        if ((++frame % (force_10ms ? 100 : 1000)) == 0)
            pc_onesec();
        if (nvr_dosave && (frame % (force_10ms ? 200 : 2000)) == 0) {
            nvr_save();
            nvr_dosave = 0;
        }
        if (unthrottled ? (frame % 4 == 0) : 0)
            hl_ctl_poll(0);
        else if (!unthrottled)
            hl_ctl_poll(0);
    }

    hl_ctl_announce_exit(exit_code);
    shutdown_emulator();
    hl_ctl_close();
    return exit_code;
}
