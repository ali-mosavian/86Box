"""End-to-end checks for the headless build.

    python3 selftest.py --binary BUILD/src/86Box-headless --roms ROMS --vms DIR

DIR holds qb-486, qb-p100 and qb-ppro200 (VM directories whose BIOS setup has
been saved, so boot does not stop at the CMOS prompt).
"""

from __future__ import annotations

import argparse
import re
import signal
import sys
import tempfile
import time
import unittest
from pathlib import Path

import hl86

HERE = Path(__file__).parent
EX = HERE / "examples"
args: argparse.Namespace


def run(vm: str, com: str) -> hl86.RunResult:
    return hl86.run_dos(args.binary, Path(args.vms) / vm, args.roms, EX / com)


def counts(out: str) -> dict[str, list[int]]:
    found: dict[str, list[int]] = {}
    for kind, op, n in re.findall(r"^(pit|tsc) (imul|shl)\s+(\d+)$", out, re.MULTILINE):
        found.setdefault(f"{kind} {op}", []).append(int(n))
    return found


class Headless(unittest.TestCase):
    def test_guest_output_and_exit_code(self) -> None:
        r = run("qb-486", "hello.com")
        self.assertEqual(r.code, 7)
        self.assertEqual(r.debug, "hello from the guest\n")

    def test_text_follows_crtc_start_address(self) -> None:
        """screen_text once read video memory from offset 0 and missed a scrolled screen."""
        vm = Path(args.vms) / "qb-486"
        com = EX / "crtcstart.com"
        orig = hl86.dos_get(vm / "c.img", "AUTOEXEC.BAT")
        with tempfile.TemporaryDirectory() as tmp:
            bat = Path(tmp) / "AUTOEXEC.BAT"
            bat.write_bytes(b"@ECHO OFF\r\nPATH C:\\DOS\r\nC:\\CRTCSTAR.COM\r\n")
            hl86.dos_put(vm / "c.img", com, "CRTCSTAR.COM")
            hl86.dos_put(vm / "c.img", bat, "AUTOEXEC.BAT")
            sock = Path(tmp) / "s"
            proc = hl86.launch(args.binary, vm, args.roms, sock, unthrottled=True)
            try:
                with hl86.Client(sock) as c:
                    text = c.wait_text("^TOPLINE", 60)
                    self.assertTrue(text.startswith("TOPLINE"))
                    c.quit()
            finally:
                proc.terminate()
                proc.wait(30)
                bat.write_bytes(orig)
                hl86.dos_put(vm / "c.img", bat, "AUTOEXEC.BAT")

    def test_imul_costs_several_times_shl(self) -> None:
        """The PIT count wraps every 54.9 ms; a long loop once read as 9 cycles per imul, not 32."""
        out = counts(run("qb-486", "imulshl.com").debug)
        imul, shl = out["pit imul"][-1], out["pit shl"][-1]
        cycles_per_tick = 66_666_666 / 1_193_182
        per_op = imul * cycles_per_tick / 64000
        self.assertGreater(per_op, 25)
        self.assertLess(per_op, 40)
        self.assertGreater(imul / shl, 3)

    def test_pit_agrees_with_tsc(self) -> None:
        out = counts(run("qb-p100", "imulshl.com").debug)
        for op in ("imul", "shl"):
            pit = out[f"pit {op}"][-1] * (100_000_000 / 1_193_182) / 64000
            tsc = out[f"tsc {op}"][-1] / 320000
            self.assertAlmostEqual(pit, tsc, delta=0.05 * tsc)

    def test_boot_skips_cmos_prompt(self) -> None:
        for vm in ("qb-486", "qb-p100", "qb-ppro200"):
            with self.subTest(vm=vm), tempfile.TemporaryDirectory() as tmp:
                sock = Path(tmp) / "s"
                proc = hl86.launch(
                    args.binary, Path(args.vms) / vm, args.roms, sock, unthrottled=True
                )
                try:
                    with hl86.Client(sock) as c:
                        text = c.wait_text(r"C:.>|Press F1", 90)
                        self.assertNotIn("Press F1", text)
                        c.quit()
                finally:
                    proc.terminate()
                    proc.wait(30)

    def test_sigterm_exits_cleanly(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            sock = Path(tmp) / "s"
            proc = hl86.launch(
                args.binary,
                Path(args.vms) / "qb-486",
                args.roms,
                sock,
                unthrottled=True,
            )
            with hl86.Client(sock) as c:
                c.wait_text(r"C:.>", 90)
            t0 = time.monotonic()
            proc.send_signal(signal.SIGTERM)
            self.assertEqual(proc.wait(10), 0)
            self.assertLess(time.monotonic() - t0, 5)
            self.assertFalse(sock.exists())


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--roms", required=True)
    ap.add_argument("--vms", required=True)
    args, rest = ap.parse_known_args()
    unittest.main(argv=[sys.argv[0], *rest])
