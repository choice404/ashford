"""Native gate for the supervisor's read-only gRPC observer."""

import os
from pathlib import Path
import queue
import re
import shlex
import signal
import subprocess
import sys
import tempfile
import threading
import time


ROOT = Path(__file__).resolve().parents[2]
SUPERVISOR = ROOT / "examples" / "supervisor" / "supervisor.py"
WATCH = ROOT / "examples" / "supervisor" / "watch.py"
PROTO = ROOT / "examples" / "supervisor" / "observer.proto"
GRPC_GEN = ROOT / "target" / "grpc-gen"
PORT = 50259
TIMEOUT = 10


class Gate:
    def __init__(self, process):
        self.process = process
        self.lines = []
        self.queue = queue.Queue()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.process.stdout:
            self.queue.put(line.rstrip("\n"))
        self.queue.put(None)

    def expect(self, predicate, description):
        deadline = time.monotonic() + TIMEOUT
        while time.monotonic() < deadline:
            try:
                line = self.queue.get(timeout=0.2)
            except queue.Empty:
                line = ""
            if line is None:
                raise AssertionError(
                    f"supervisor ended waiting for {description}; "
                    f"stdout={self.lines!r}; stderr={self.process.stderr.read()!r}"
                )
            if line:
                self.lines.append(line)
                if predicate(line):
                    return line
            if self.process.poll() is not None:
                raise AssertionError(
                    f"supervisor exited {self.process.returncode} waiting for "
                    f"{description}; stderr={self.process.stderr.read()!r}"
                )
        raise AssertionError(f"timed out waiting for {description}: {self.lines!r}")

    def wait_ok(self):
        try:
            code = self.process.wait(timeout=TIMEOUT)
        except subprocess.TimeoutExpired as error:
            raise AssertionError("supervisor did not exit") from error
        if code != 0:
            raise AssertionError(
                f"supervisor exited {code}: {self.process.stderr.read()!r}"
            )

    def close(self):
        self.reader.join(timeout=1)


def grpc_ready():
    try:
        import grpc  # noqa: F401
        import grpc_tools  # noqa: F401
    except ImportError:
        print("[test_watch] grpcio or grpc_tools not found, skipping", flush=True)
        return False
    return True


def generate_stubs():
    GRPC_GEN.mkdir(parents=True, exist_ok=True)
    command = [sys.executable, "-m", "grpc_tools.protoc", "-I",
               str(PROTO.parent), f"--python_out={GRPC_GEN}",
               f"--grpc_python_out={GRPC_GEN}", str(PROTO)]
    subprocess.run(command, check=True)


def launch(dsn, piddir, marker):
    marker_text = shlex.quote(str(marker))
    flaky = (f'flaky=sh -c "test -f {marker_text} || {{ touch {marker_text}; '
             'sleep 0.3; exit 7; }; exec sleep 30"')
    command = [sys.executable, str(SUPERVISOR), "--dsn", str(dsn),
               "--piddir", str(piddir), "--max-crashes", "2", "--poll",
               "0.05", "--grpc", str(PORT), "--service", "steady=sleep 30",
               "--service", flaky]
    return Gate(subprocess.Popen(command, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True))


def pid_from(path):
    words = Path(path).read_text(encoding="utf-8").split()
    if len(words) != 2:
        raise AssertionError(f"bad pidfile {path}: {words!r}")
    return int(words[0])


def watch(*args):
    return subprocess.run([sys.executable, str(WATCH), "--port", str(PORT),
                           *args], text=True, capture_output=True, timeout=TIMEOUT)


def cleanup(gate, piddir):
    if gate is not None and gate.process.poll() is None:
        gate.process.terminate()
        try:
            gate.process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            gate.process.kill()
            gate.process.wait()
    if gate is not None:
        gate.close()
    for path in Path(piddir).glob("*.pid"):
        try:
            os.killpg(pid_from(path), signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass


def main():
    if not grpc_ready():
        return 0
    gate = None
    with tempfile.TemporaryDirectory(prefix="ashford-watch-") as temp:
        root = Path(temp)
        dsn = root / "supervisor.sqlite"
        piddir = root / "pids"
        marker = root / "flaky-ran"
        try:
            generate_stubs()
            gate = launch(dsn, piddir, marker)
            gate.expect(lambda line: line == "[supervisor] ready steady",
                        "steady ready")
            gate.expect(lambda line: line == "[supervisor] ready flaky",
                        "flaky first ready")
            gate.expect(lambda line: line == f"[supervisor] observing {PORT}",
                        "observer serving")
            gate.expect(
                lambda line: line == "[supervisor] crashed flaky code 7 crashes 1",
                "flaky first crash",
            )
            gate.expect(lambda line: line == "[supervisor] ready flaky",
                        "flaky second ready")

            steady_pid = pid_from(piddir / "steady.pid")
            listed = watch("list")
            if listed.returncode != 0:
                raise AssertionError(f"watch list failed: {listed.stderr!r}")
            rows = listed.stdout.splitlines()
            if not any(re.fullmatch(rf"steady Partial pid {steady_pid} run \d+ crashes 0",
                                    row) for row in rows):
                raise AssertionError(f"steady list row missing: {rows!r}")
            if not any(re.fullmatch(r"flaky Partial pid \d+ run \d+ crashes 1", row)
                       for row in rows):
                raise AssertionError(f"flaky list row missing: {rows!r}")

            detail = watch("get", "steady")
            if detail.returncode != 0:
                raise AssertionError(f"watch get failed: {detail.stderr!r}")
            lines = detail.stdout.splitlines()
            if (not re.fullmatch(rf"steady Partial pid {steady_pid} run \d+ crashes 0",
                                 lines[0] if lines else "") or
                    lines[1:] != ["fulfilled: start,ready",
                                  "pending: finish,crashes", "broken:"]):
                raise AssertionError(f"steady detail mismatch: {lines!r}")

            missing = watch("get", "nosuch")
            if missing.returncode != 3 or "unknown service nosuch" not in missing.stderr:
                raise AssertionError(f"unknown service mismatch: {missing!r}")

            gate.process.send_signal(signal.SIGUSR1)
            gate.expect(lambda line: line == "[supervisor] stopped steady Fulfilled",
                        "steady stopped")
            gate.expect(lambda line: line == "[supervisor] stopped flaky Fulfilled",
                        "flaky stopped")
            gate.wait_ok()
        finally:
            cleanup(gate, piddir)
    print("[test_watch] ok", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr, flush=True)
        raise SystemExit(1)
