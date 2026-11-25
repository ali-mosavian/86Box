# 86Box headless

A windowless 86Box with no audio or MIDI, driven over a unix socket. CPU timing
models and the timer system are stock 86Box; guest clocks (PIT, RDTSC) come
from emulated cycles, so a DOS program that times itself sees emulated-CPU
speed.

## Build

```
cmake -S . -B build -G Ninja -DHEADLESS=ON -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_MACOSX_BUNDLE=OFF -DDISCORD=OFF -DVNC=OFF
ninja -C build
```

Binary: `build/src/86Box-headless`. Needs freetype, libpng, libslirp,
libsndfile and fluidsynth; no SDL, OpenAL, RtMidi or Qt.

## Run

```
86Box-headless --vm DIR --roms DIR --socket PATH [--unthrottled] [--paused]
```

- `--vm DIR`: VM directory with `86box.cfg`, `nvr/` and disk images.
- `--roms DIR`: ROM set.
- `--socket PATH`: unix socket (macOS limits the path to 103 bytes).
- `--unthrottled`: run as fast as the host allows. Default is real time.
  Guest-measured results are bit-identical in both modes (checked with
  `examples/imulshl.com`, RDTSC and PIT counts).
- `--paused`: start paused.

SIGTERM, SIGINT and the `quit` command shut down cleanly: disk images are
synced and closed, the NVR and BIOS flash are written, the socket is removed.
A global `86box_global.cfg` is written next to the binary.

## Protocol

One JSON object per line, one reply per request, `{"ok": true, ...}` or
`{"ok": false, "error": "..."}`. A client has one request in flight.

| `cmd` | arguments | reply |
|---|---|---|
| `status` | | `state` (running/paused), `machine`, `cpu`, `cpu_hz`, `emu_ms`, `tsc`, `unthrottled`, `video_mode`, `text_mode`, `cr0`, `cache_int`, `cache_ext` |
| `screen_text` | | `text_mode`, `video_mode`; in text mode also `cols`, `rows`, `cursor_x`, `cursor_y`, `text` (UTF-8 from CP437, one line per row) |
| `screenshot` | `path` (`.ppm` for PPM, else PNG) | `path`, `width`, `height` |
| `keys` | `text` (typed, `\n` is Enter), `keys` (names or chains: `Enter`, `Esc`, `F1`, `Del`, `Up`, `Ctrl+Alt+Del`), `delay_ms` (emulated, default 10) | after the last key is delivered |
| `wait_text` | `pattern` (POSIX extended regex, line-wise), `timeout` (host seconds, default 30) | `matched`, `text`; error `timeout` otherwise |
| `sleep` | `ms` (emulated) | after that much emulated time |
| `pause`, `resume` | | |
| `reset` | | hard reset |
| `debug_out` | | `data`, `bytes`: what the guest wrote to port 0xE9 since the last call; also echoed to stderr |
| `profile_start` | `interval_us` (emulated, default 20) | start sampling the guest PC |
| `profile_stop` | `path` | writes `{"interval_us", "samples", "pcs": {"<hex linear pc>": count}}` to `path`; replies with `interval_us`, `samples`, `distinct` |
| `mem_read` | `addr` (linear), `len` (up to 16 MB), `path` | writes the raw bytes to `path` (paging walked if on; error on an unmapped page) |
| `quit` | `code` (default 0) | then exits |

When the run ends, every connected client gets
`{"event": "exit", "code": N, "emu_ms": ..., "data": "<unread 0xE9 bytes>"}`.

Samples are the linear PC (CS base + EIP) and come from a timer in 86Box's
timer system, so guest timing is unchanged (`selftest.py` checks that
`imulshl.com` counts are identical with sampling on). With the dynarec the
sample is the PC at the next block boundary; set `cpu_use_dynarec = 0` for
exact instruction PCs. A hard reset keeps sampling. Wall-clock cost: none
measurable at 20 us, about 2x at 5 us.

## Guest ports

| port | use |
|---|---|
| `0xE9` | Bochs debug port: bytes are kept for `debug_out` and echoed to stderr |
| `0x0501` | exit port: write a byte (`out dx, al`) to end the run; the byte is the process exit code |
| `0x0502` | profile window: 1 restarts the counts, 0 stops counting, so the host's delay in stopping is not sampled |

`examples/hlexit.asm` is `HLEXIT n` and `examples/hlprof.asm` `HLPROF n` for batch files. `examples/hello.asm` is
the smallest user of both ports.

## Python client

`hl86.py` (Python 3.12+, stdlib only):

```python
import hl86

proc = hl86.launch("build/src/86Box-headless", vm, roms, "/tmp/vm.sock")
with hl86.Client("/tmp/vm.sock") as c:
    c.wait_text(r"C:.>")
    c.keys("dir", keys=["Enter"])
    print(c.screen_text())
    c.quit()
```

Profile with `c.profile_start(20)` ... `c.profile_stop(path)`; read memory with `c.mem_read(addr, n, path)`.

Run a DOS program and collect its port 0xE9 output (needs mtools; the program
is copied to the image root, AUTOEXEC.BAT is swapped for the run and restored):

```
python3 hl86.py run --binary build/src/86Box-headless --vm VM --roms ROMS examples/imulshl.com
```

The first boot of a VM stops at the BIOS CMOS prompt. Answer it once (`Del`,
IDE auto-detect on the 486 board, then `F10`, `y`, `Enter`) so the NVR holds
valid settings.

## Timing notes

- Under `--unthrottled`, `emu_ms` is the emulated clock, not wall time.
- The PIT counter is 16 bits and wraps every 54.9 ms; time shorter loops.
- On the 486 and Pentium models a 0x66 operand-size prefix (32-bit code in
  real mode) costs extra cycles; protected-mode 32-bit code does not pay it.
- The dynarec timing models are calibrated by `examples/pmbench.asm`
  (flat protected-mode micro-benchmarks, `-DTEST=1..10`, run by `selftest.py`).
  P6: multiplier latency 4, partial-register (5-6 clocks) and partial-flags
  stalls, store-forward failure (7-8), 66h prefix stalls, 16-byte fetch blocks,
  and a two-level branch predictor (512-entry BTB, 4-bit local history, 15-cycle
  mispredict). Pentium: 256 two-bit counters, 4-cycle mispredict. 486: base+index
  addressing and taken jcc cost extra cycles. On the 486 and Pentium an x87
  instruction charges only its non-concurrent clocks and leaves the FPU busy;
  `codegen_fpu_sync` carries that across blocks and the next x87 instruction
  waits. The interpreter (`cpu_use_dynarec=0`) is unchanged.

## Checks

```
python3 selftest.py --binary build/src/86Box-headless --roms ROMS --vms DIR
```

`DIR` holds `qb-486`, `qb-p100` and `qb-ppro200`.
