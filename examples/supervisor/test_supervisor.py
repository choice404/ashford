"""Stdlib gate driver for the Service supervisor example.

It follows the intended operator story: a flaky service spends its crash
budget, a steady service is parked across a supervisor restart, and SIGUSR1
is an explicit clean stop rather than an inferred process crash.
"""

import os
from pathlib import Path
import queue
import signal
import subprocess
import sys
import tempfile
import threading
import time


ROOT = Path(__file__).resolve().parents[2]
SUPERVISOR = ROOT / "examples" / "supervisor" / "supervisor.py"
TIMEOUT = 10


class Gate:
    def __init__(self, process):
        self.process = process
        self.lines = []
        self.lines_queue = queue.Queue()
        self.reader = threading.Thread(target=self._read_stdout, daemon=True)
        self.reader.start()

    def _read_stdout(self):
        for line in self.process.stdout:
            self.lines_queue.put(line.rstrip("\n"))
        self.lines_queue.put(None)

    def expect(self, predicate, description):
        deadline = time.monotonic() + TIMEOUT
        while time.monotonic() < deadline:
            try:
                line = self.lines_queue.get(timeout=0.2)
            except queue.Empty:
                line = ""
            if line is None:
                stderr = self.process.stderr.read()
                raise AssertionError(
                    f"supervisor ended while waiting for {description}; "
                    f"stdout={self.lines!r}; stderr={stderr!r}"
                )
            if line:
                self.lines.append(line)
                if predicate(line):
                    return line
            if self.process.poll() is not None:
                stderr = self.process.stderr.read()
                raise AssertionError(
                    f"supervisor exited {self.process.returncode} while waiting "
                    f"for {description}; stdout={self.lines!r}; stderr={stderr!r}"
                )
        raise AssertionError(f"timed out waiting for {description}; stdout={self.lines!r}")

    def wait_ok(self):
        try:
            returncode = self.process.wait(timeout=TIMEOUT)
        except subprocess.TimeoutExpired as error:
            raise AssertionError("supervisor did not exit") from error
        if returncode != 0:
            raise AssertionError(
                f"supervisor exited {returncode}; stderr={self.process.stderr.read()!r}"
            )

    def close(self):
        self.reader.join(timeout=1)


def launch(dsn, piddir, services, resume=False):
    command = [sys.executable, str(SUPERVISOR), "--dsn", str(dsn),
               "--piddir", str(piddir), "--max-crashes", "2", "--poll", "0.05"]
    if resume:
        command.append("--resume")
    for spec in services:
        command.extend(["--service", spec])
    process = subprocess.Popen(command, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    return Gate(process)


def pid_from(path):
    words = Path(path).read_text(encoding="utf-8").split()
    if len(words) != 2:
        raise AssertionError(f"bad pidfile {path}: {words!r}")
    return int(words[0])


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def cleanup(gates, piddir):
    for gate in gates:
        if gate.process.poll() is None:
            gate.process.terminate()
            try:
                gate.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                gate.process.kill()
                gate.process.wait()
        gate.close()
    for path in Path(piddir).glob("*.pid"):
        try:
            os.killpg(pid_from(path), signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass


def main():
    gates = []
    with tempfile.TemporaryDirectory(prefix="ashford-supervisor-") as temp:
        root = Path(temp)
        dsn = root / "supervisor.sqlite"
        piddir = root / "pids"
        try:
            first = launch(
                dsn, piddir,
                ["steady=sleep 30", "flaky=sh -c 'sleep 0.4; exit 7'"],
            )
            gates.append(first)
            first.expect(lambda line: line.startswith("[supervisor] up steady pid "),
                         "steady up")
            first.expect(lambda line: line == "[supervisor] ready steady",
                         "steady ready")
            first.expect(lambda line: line.startswith("[supervisor] up flaky pid "),
                         "flaky first up")
            first.expect(lambda line: line == "[supervisor] ready flaky",
                         "flaky first ready")
            first.expect(lambda line: line == "[supervisor] crashed flaky code 7 crashes 1",
                         "flaky first crash")
            first.expect(lambda line: line.startswith("[supervisor] up flaky pid "),
                         "flaky second up")
            first.expect(lambda line: line == "[supervisor] ready flaky",
                         "flaky second ready")
            first.expect(lambda line: line == "[supervisor] crashed flaky code 7 crashes 2",
                         "flaky second crash")
            first.expect(lambda line: line == "[supervisor] gaveup flaky",
                         "flaky gave up")
            if any("crashed steady" in line for line in first.lines):
                raise AssertionError("steady crashed before parking")

            first.process.send_signal(signal.SIGTERM)
            first.expect(lambda line: line == "[supervisor] parked steady",
                         "steady parked")
            first.wait_ok()
            steady_pid = pid_from(piddir / "steady.pid")
            if not alive(steady_pid):
                raise AssertionError("steady child died when supervisor parked")

            second = launch(dsn, piddir, ["steady=sleep 30"], resume=True)
            gates.append(second)
            second.expect(lambda line: line == f"[supervisor] resumed steady pid {steady_pid}",
                          "steady resumed with its original pid")
            second.process.send_signal(signal.SIGUSR1)
            second.expect(lambda line: line == "[supervisor] stopped steady Fulfilled",
                          "operator clean stop")
            second.wait_ok()
        finally:
            cleanup(gates, piddir)
    print("[test_supervisor] ok", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr, flush=True)
        raise SystemExit(1)
