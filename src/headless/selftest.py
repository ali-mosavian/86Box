"""End-to-end checks for the headless build.

    python3 selftest.py --binary BUILD/src/86Box-headless --roms ROMS --vms DIR

DIR holds qb-486, qb-p100 and qb-ppro200 (VM directories whose BIOS setup has
been saved, so boot does not stop at the CMOS prompt).
"""

from __future__ import annotations

import argparse
import contextlib
import json
import re
import signal
import sys
import tempfile
import time
import unittest
from collections.abc import Iterator
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

    @contextlib.contextmanager
    def hanging(self, com: str, vm_name: str = "qb-486") -> Iterator[hl86.Client]:
        """Boot the VM running one .COM that never exits; AUTOEXEC.BAT is restored afterwards."""
        vm = Path(args.vms) / vm_name
        name = com.upper()
        orig = hl86.dos_get(vm / "c.img", "AUTOEXEC.BAT")
        with tempfile.TemporaryDirectory() as tmp:
            bat = Path(tmp) / "AUTOEXEC.BAT"
            bat.write_bytes(f"@ECHO OFF\r\nPATH C:\\DOS\r\nC:\\{name}\r\n".encode())
            hl86.dos_put(vm / "c.img", EX / com, name)
            hl86.dos_put(vm / "c.img", bat, "AUTOEXEC.BAT")
            proc = hl86.launch(
                args.binary, vm, args.roms, Path(tmp) / "s", unthrottled=True
            )
            try:
                try:
                    with hl86.Client(Path(tmp) / "s") as c:
                        yield c
                finally:
                    proc.terminate()
                    proc.wait(30)
            finally:
                bat.write_bytes(orig)
                hl86.dos_put(vm / "c.img", bat, "AUTOEXEC.BAT")

    def test_text_follows_crtc_start_address(self) -> None:
        """screen_text once read video memory from offset 0 and missed a scrolled screen."""
        with self.hanging("crtcstart.com") as c:
            self.assertTrue(c.wait_text("^TOPLINE", 60).startswith("TOPLINE"))

    def spin_addresses(self, c: hl86.Client) -> tuple[int, int]:
        out = ""
        for _ in range(600):
            out += c.debug_out()
            if m := re.search(r"LOOP ([0-9A-F]{8}) DATA ([0-9A-F]{8})\n", out):
                return int(m[1], 16), int(m[2], 16)
            c.sleep(100)
        self.fail("spin.com never reported its addresses")

    def test_profile_lands_in_the_spin_loop(self) -> None:
        """The recorded PC must be CS base + EIP; EIP alone lands at 0x1xx, outside the loop."""
        with self.hanging("spin.com") as c, tempfile.TemporaryDirectory() as tmp:
            loop, _ = self.spin_addresses(c)
            c.profile_start(20)
            c.sleep(200)
            path = Path(tmp) / "p.json"
            r = c.profile_stop(path)
            data = json.loads(path.read_text())
            self.assertEqual(data["samples"], r["samples"])
            self.assertGreater(r["samples"], 8000)  # 200 ms / 20 us = 10000
            inside = sum(
                n for pc, n in data["pcs"].items() if loop <= int(pc, 16) < loop + 2
            )
            self.assertGreater(inside, 0.9 * data["samples"])

    def test_profile_charges_the_block_that_ran(self) -> None:
        """Samples were recorded at the block after the running one: a 16-imul block read as idle."""
        with self.hanging("blocks.com") as c, tempfile.TemporaryDirectory() as tmp:
            out = ""
            for _ in range(600):
                out += c.debug_out()
                if m := re.search(
                    r"SHORT ([0-9A-F]{8}) LONG ([0-9A-F]{8}) END ([0-9A-F]{8})\n", out
                ):
                    break
                c.sleep(100)
            else:
                self.fail("blocks.com never reported its addresses")
            short, long_, end = (int(g, 16) for g in m.groups())
            c.profile_start(20)
            c.sleep(200)
            path = Path(tmp) / "p.json"
            c.profile_stop(path)
            data = json.loads(path.read_text())
            in_long = sum(
                n for pc, n in data["pcs"].items() if long_ <= int(pc, 16) < end
            )
            self.assertGreater(in_long, 0.9 * data["samples"])
            self.assertGreater(long_, short)

    def test_mem_read_returns_guest_bytes(self) -> None:
        with self.hanging("spin.com") as c, tempfile.TemporaryDirectory() as tmp:
            _, data = self.spin_addresses(c)
            got = c.mem_read(data, 16, Path(tmp) / "m.bin")
            self.assertEqual(got, b"HL86-MEMREAD-OK\x00")

    def test_profiling_does_not_change_guest_timing(self) -> None:
        plain = counts(run("qb-p100", "imulshl.com").debug)
        r = hl86.run_dos(
            args.binary,
            Path(args.vms) / "qb-p100",
            args.roms,
            EX / "imulshl.com",
            profile_us=20,
        )
        self.assertEqual(counts(r.debug), plain)

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

    def bench(self, vm: str, com: str) -> tuple[float, float]:
        """Cycles per iteration of (A, B) from pmbench.asm; PIT ticks are scaled by the VM clock."""
        mhz = {"qb-486": 66_666_666, "qb-p100": 100_000_000, "qb-ppro200": 200_000_000}[
            vm
        ]
        m = re.search(r"(\w+) A=(\d+) B=(\d+) N=(\d+)\s*$", run(vm, com).debug)
        assert m
        unit, a, b, n = m.groups()
        f = mhz / 1_193_182 if unit == "pit" else 1
        return int(a) * f / int(n), int(b) * f / int(n)

    def test_p6_imul_chain_is_four_cycles_independent_is_one(self) -> None:
        """Every multiply had latency 1: a dependent chain read 1.0 per imul, not 4."""
        a, b = self.bench("qb-ppro200", "pmimul.com")
        self.assertAlmostEqual(a / 8, 4.0, delta=0.5)
        self.assertLess(b / 8, 1.5)

    def test_p6_partial_register_read_stalls_five_to_six_cycles(self) -> None:
        """Agner Fog: a partial register stall is 5-6 clocks. Modelled as 6.0-7.0 before the fix."""
        a, b = self.bench("qb-ppro200", "pmpartl.com")
        self.assertGreater((a - b) / 4, 5.0)
        self.assertLess((a - b) / 4, 6.5)

    def test_p6_store_forwarding_failure_costs_seven_to_eight_cycles(self) -> None:
        """Agner Fog: a failed store forward costs about 7-8 clocks; it read 4 (chained) or 14."""
        a, b = self.bench("qb-ppro200", "pmstfwd.com")
        self.assertGreater((a - b) / 4, 6.5)
        self.assertLess((a - b) / 4, 8.5)

    def test_p6_alternating_branch_is_predicted(self) -> None:
        """The P6 two-level predictor learns period 2; two-bit counters mispredicted half the time."""
        a, b = self.bench("qb-ppro200", "pmbranch.com")
        self.assertLess(a - b, 2)

    def test_p6_random_branch_mispredicts_half_the_time(self) -> None:
        """An xorshift bit is unpredictable: about half of a 15-cycle mispredict per iteration."""
        a, b = self.bench("qb-ppro200", "pmrand.com")
        self.assertGreater(a - b, 5)
        self.assertLess(a - b, 10)

    def test_p5_alternating_branch_still_costs_cycles(self) -> None:
        """The P5 has one two-bit counter per branch and no history."""
        a, b = self.bench("qb-p100", "pmbranch.com")
        self.assertGreater(a - b, 4)

    def test_agi_costs_one_cycle_on_486_and_p5(self) -> None:
        """An address register written by the previous instruction costs one extra cycle."""
        for vm in ("qb-486", "qb-p100"):
            a, b = self.bench(vm, "pmagi.com")
            with self.subTest(vm=vm):
                self.assertGreaterEqual((a - b) / 4, 0.4)

    def test_486_base_plus_index_costs_one_extra_cycle(self) -> None:
        """[esi+edi] loaded as fast as [esi]; the 486 charges one more cycle."""
        a, b = self.bench("qb-486", "pmbase.com")
        self.assertAlmostEqual((a - b) / 8, 1.0, delta=0.2)

    def test_486_taken_branch_costs_more_than_not_taken(self) -> None:
        """A taken jcc cost the same as a not-taken one; the 486 takes 3 cycles, not 1."""
        a, b = self.bench("qb-486", "pmbranch.com")
        self.assertGreater(a - b, 1.5)

    def test_fdiv_overlaps_independent_integer_adds(self) -> None:
        """fdiv plus 40 adds cost the sum: 197 cycles on a 486 (fdiv alone 77); max(fdiv, adds) is right."""
        for vm in ("qb-486", "qb-p100"):
            a, b = self.bench(vm, "pmfdadd.com")
            with self.subTest(vm=vm):
                self.assertAlmostEqual(a, b, delta=0.1 * b)

    def test_fdiv_after_fdiv_waits_for_the_divider(self) -> None:
        """The second fdiv did not wait on a Pentium (42 cycles for two divides, 41 for one)."""
        for vm in ("qb-486", "qb-p100"):
            a, b = self.bench(vm, "pmfdfd.com")
            with self.subTest(vm=vm):
                self.assertAlmostEqual(a / b, 2.0, delta=0.15)

    def test_486_add_immediate_costs_one_cycle(self) -> None:
        """add reg,imm used the memory-operand timing: 3 cycles, not 1."""
        a, b = self.bench("qb-486", "pmaddi.com")
        self.assertAlmostEqual((a - b) / 16, 0.0, delta=0.3)

    def test_ftst_after_an_infinity_load(self) -> None:
        """arm64 FTST compared against scratch - scratch: NaN after a load of +Inf, so 1.0 read unordered."""
        self.assertEqual(run("qb-486", "ftstinf.com").code, 0)

    def test_unrolled_jl_and_jge_loops_fall_through(self) -> None:
        """An unrolled JL/JGE loop left through its head: it ran 1001 times, not 100."""
        self.assertEqual(run("qb-486", "jlloop.com").code, 0)

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
