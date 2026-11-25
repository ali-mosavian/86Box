"""Client for the 86Box headless control socket.

Import it (``from hl86 import Client, launch``) or run it as a CLI
(``python3 hl86.py --socket PATH status``).
"""

from __future__ import annotations

import argparse
import json
import socket
import subprocess
import sys
import time
from pathlib import Path
from types import TracebackType
from typing import Any, Self


class Error(Exception):
    """The emulator refused a request or the connection ended."""


class Client:
    """One connection to a running ``86Box-headless``."""

    def __init__(self, path: str | Path, timeout: float | None = 120.0) -> None:
        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._sock.settimeout(timeout)
        self._sock.connect(str(path))
        self._buf = b""

    def __enter__(self) -> Self:
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        self.close()

    def close(self) -> None:
        self._sock.close()

    def _readline(self) -> dict[str, Any]:
        while b"\n" not in self._buf:
            chunk = self._sock.recv(65536)
            if not chunk:
                raise Error("connection closed")
            self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        return json.loads(line)

    def call(self, cmd: str, **args: Any) -> dict[str, Any]:
        """Send one request and return its reply; raises ``Error`` on ``ok: false``."""
        self._sock.sendall(json.dumps({"cmd": cmd, **args}).encode() + b"\n")
        while True:
            reply = self._readline()
            if "event" in reply:  # exit notice: the run is over
                if reply.get("event") == "exit":
                    raise Error(f"emulator exited with code {reply.get('code')}")
                continue
            if not reply.get("ok"):
                raise Error(reply.get("error", "request failed"))
            return reply

    def status(self) -> dict[str, Any]:
        return self.call("status")

    def screen_text(self) -> str:
        """The text screen, or an empty string when the video mode is not text."""
        return str(self.call("screen_text").get("text", ""))

    def screenshot(self, path: str | Path) -> dict[str, Any]:
        return self.call("screenshot", path=str(Path(path).resolve()))

    def keys(
        self,
        text: str = "",
        keys: list[str] | None = None,
        delay_ms: int | None = None,
    ) -> None:
        """Type ``text``, then press ``keys`` (names such as ``Enter`` or ``Ctrl+Alt+Del``)."""
        args: dict[str, Any] = {"text": text, "keys": keys or []}
        if delay_ms is not None:
            args["delay_ms"] = delay_ms
        self.call("keys", **args)

    def wait_text(self, pattern: str, timeout: float = 30.0) -> str:
        """Block until the screen matches the POSIX extended regex; returns the screen."""
        return str(self.call("wait_text", pattern=pattern, timeout=timeout)["text"])

    def sleep(self, ms: float) -> None:
        """Let ``ms`` of emulated time pass."""
        self.call("sleep", ms=ms)

    def pause(self) -> None:
        self.call("pause")

    def resume(self) -> None:
        self.call("resume")

    def reset(self) -> None:
        self.call("reset")

    def debug_out(self) -> str:
        """Bytes the guest wrote to port 0xE9 since the previous call."""
        return str(self.call("debug_out")["data"])

    def quit(self, code: int = 0) -> None:
        try:
            self.call("quit", code=code)
        except Error:
            pass


def launch(
    binary: str | Path,
    vm: str | Path,
    roms: str | Path,
    socket_path: str | Path,
    *,
    unthrottled: bool = False,
    log: str | Path | None = None,
    wait: float = 30.0,
) -> subprocess.Popen[bytes]:
    """Start the emulator and return once its socket accepts connections."""
    cmd = [str(binary), "--vm", str(vm), "--roms", str(roms), "--socket", str(socket_path)]
    if unthrottled:
        cmd.append("--unthrottled")
    err = open(log, "wb") if log else subprocess.DEVNULL  # noqa: SIM115
    proc = subprocess.Popen(cmd, stderr=err)
    deadline = time.monotonic() + wait
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise Error(f"emulator exited early with code {proc.returncode}")
        try:
            Client(socket_path, timeout=1.0).close()
            return proc
        except OSError:
            time.sleep(0.05)
    proc.kill()
    raise Error("control socket did not appear")


def _main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--socket", required=True, help="control socket path")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status")
    sub.add_parser("text")
    sub.add_parser("pause")
    sub.add_parser("resume")
    sub.add_parser("reset")
    sub.add_parser("debug")
    p = sub.add_parser("screenshot")
    p.add_argument("path")
    p = sub.add_parser("keys", help="type TEXT, then press each --key")
    p.add_argument("text", nargs="?", default="")
    p.add_argument("--key", action="append", default=[])
    p = sub.add_parser("wait")
    p.add_argument("pattern")
    p.add_argument("--timeout", type=float, default=30.0)
    p = sub.add_parser("sleep")
    p.add_argument("ms", type=float)
    p = sub.add_parser("quit")
    p.add_argument("--code", type=int, default=0)
    args = ap.parse_args()

    try:
        with Client(args.socket) as c:
            match args.cmd:
                case "status":
                    print(json.dumps(c.status(), indent=2))
                case "text":
                    print(c.screen_text())
                case "screenshot":
                    print(json.dumps(c.screenshot(args.path)))
                case "keys":
                    c.keys(args.text, args.key)
                case "wait":
                    print(c.wait_text(args.pattern, args.timeout))
                case "sleep":
                    c.sleep(args.ms)
                case "debug":
                    sys.stdout.write(c.debug_out())
                case "quit":
                    c.quit(args.code)
                case cmd:
                    c.call(cmd)
    except (Error, OSError) as e:
        print(f"hl86: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(_main())
