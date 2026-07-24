"""A small process supervisor whose Ashford contract is its state machine.

Each Service instance describes one run. Signed means the process is starting,
Partial means it is running, Fulfilled is a clean exit, and Broken is a
crash. The partial surface is therefore the useful diagnosis, not an extra
status flag. Parking persists that diagnosis before this host exits, so a
later host can resume the same running service instead of guessing at it.
"""

import argparse
import os
from pathlib import Path
import signal
import sqlite3
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "target" / "ashc-out"
sys.path.insert(0, str(ROOT / "interop" / "python"))

from ashford import AshError, Err, Ok, Runtime


class Run:
    """The host-only facts which deliberately do not belong in the contract."""

    def __init__(self, name, command, contract, run_id):
        self.name = name
        self.command = command
        self.contract = contract
        self.run_id = run_id
        self.pid = None
        self.process = None


class Supervisor:
    """Owns one runtime and the live process records for its invocation."""

    def __init__(self, runtime, dsn, piddir, max_crashes, poll):
        self.runtime = runtime
        self.dsn = str(dsn)
        self.piddir = Path(piddir)
        self.max_crashes = max_crashes
        self.poll = poll
        self.running = {}
        self.starting = {}
        self._next_run = time.time_ns()
        self.park_requested = False
        self.stop_requested = False

    def pidfile(self, name):
        return self.piddir / f"{name}.pid"

    def issue_run_id(self):
        self._next_run += 1
        return self._next_run

    def write_pidfile(self, run):
        self.piddir.mkdir(parents=True, exist_ok=True)
        self.pidfile(run.name).write_text(f"{run.pid} {run.run_id}\n",
                                          encoding="utf-8")

    def read_pidfile(self, name):
        try:
            words = self.pidfile(name).read_text(encoding="utf-8").split()
            if len(words) != 2:
                raise ValueError("expected PID RUNID")
            return int(words[0]), int(words[1])
        except (OSError, ValueError):
            return None

    @staticmethod
    def alive(pid):
        if pid is None or pid <= 0:
            return False
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        return True

    def bind_start(self, contract, _args):
        """Spawn only when the contract crosses its host-bound start pledge."""
        name = contract.vow("name")
        run = self.starting.get(name)
        if run is None:
            return Err(-1)
        try:
            process = subprocess.Popen(
                ["/bin/sh", "-c", contract.vow("cmd")],
                start_new_session=True,
            )
        except OSError:
            return Err(-1)
        run.pid = process.pid
        run.process = process
        self.write_pidfile(run)
        return Ok(process.pid)

    def bind_ready(self, contract, _args):
        """Health is intentionally boring here: a live pid is first pass."""
        run = self.starting.get(contract.vow("name"))
        if run is not None and self.alive(run.pid):
            return Ok(True)
        return Err(-1)

    @staticmethod
    def result_value(result):
        if isinstance(result, Ok):
            return result.value
        if isinstance(result, Err):
            return result.value
        return result

    def finish(self, run, code, clean):
        # If Service gains a leading svc fallback vow parameter, change only
        # this fulfillment call and crashes() below.
        return run.contract.fulfill_sync("finish", run.run_id, code, clean)

    def crashes(self, run):
        # finish(false) has already made the run instance Broken. Count from a
        # short-lived observer over the shared Runs table instead. See the
        # fallback note in finish().
        observer = self.runtime.sign(
            "Service", vows={"name": run.name, "cmd": run.command,
                             "dsn": self.dsn}
        )
        try:
            return int(self.result_value(observer.fulfill_sync("crashes")))
        finally:
            # The observer signed only to read; breaking it keeps the table
            # free of instances nobody is standing behind.
            try:
                observer.break_()
            except AshError:
                pass

    def consume_park(self, name):
        """resume reads the row; DELETE makes the successful claim one-shot."""
        with sqlite3.connect(self.dsn) as db:
            db.execute("DELETE FROM ash_park WHERE pkey = ?", (name.encode(),))

    def sign_fresh(self, name, command):
        contract = self.runtime.sign(
            "Service", vows={"name": name, "cmd": command, "dsn": self.dsn}
        )
        run = Run(name, command, contract, self.issue_run_id())
        self.starting[name] = run
        try:
            started = contract.fulfill_sync("start")
            if not isinstance(started, Ok) or run.pid is None:
                raise RuntimeError(f"start returned {started!r}")
            print(f"[supervisor] up {name} pid {run.pid} run {run.run_id}",
                  flush=True)
            ready = contract.fulfill_sync("ready")
            if not isinstance(ready, Ok) or ready.value is not True:
                raise RuntimeError(f"ready returned {ready!r}")
            print(f"[supervisor] ready {name}", flush=True)
        except Exception:
            if run.process is not None and run.process.poll() is None:
                run.process.terminate()
            raise
        finally:
            self.starting.pop(name, None)
        self.running[name] = run
        return run

    def resume_or_start(self, name, command, resume):
        if not resume:
            return self.sign_fresh(name, command)
        try:
            contract = self.runtime.resume(self.dsn, name)
        except AshError:
            return self.sign_fresh(name, command)

        self.consume_park(name)
        saved = self.read_pidfile(name)
        if saved is not None and self.alive(saved[0]):
            run = Run(name, command, contract, saved[1])
            run.pid = saved[0]
            self.running[name] = run
            print(f"[supervisor] resumed {name} pid {run.pid}", flush=True)
            return run

        run_id = saved[1] if saved is not None else self.issue_run_id()
        run = Run(name, command, contract, run_id)
        count = self.record_crash(run, -1)
        return self.restart_or_giveup(name, command, run, count)

    def exit_code(self, run):
        """Popen.poll performs waitpid(WNOHANG) for children this host owns."""
        if run.process is not None:
            return run.process.poll()
        return -1

    def record_crash(self, run, code):
        self.finish(run, code, False)
        count = self.crashes(run)
        print(f"[supervisor] crashed {run.name} code {code} crashes {count}",
              flush=True)
        return count

    def restart_or_giveup(self, name, command, run, count=None):
        if count is None:
            count = self.crashes(run)
        if count < self.max_crashes:
            return self.sign_fresh(name, command)
        print(f"[supervisor] gaveup {name}", flush=True)
        return None

    def handle_dead(self, name, run):
        self.running.pop(name, None)
        code = self.exit_code(run)
        if code is None:
            return
        if code == 0:
            self.finish(run, 0, True)
            print(f"[supervisor] stopped {name} "
                  f"{run.contract.state_name().title()}", flush=True)
            try:
                self.pidfile(name).unlink()
            except FileNotFoundError:
                pass
            return
        count = self.record_crash(run, code)
        self.restart_or_giveup(name, run.command, run, count)

    def poll_once(self):
        for name, run in list(self.running.items()):
            # A child that has exited but has not been reaped is a zombie;
            # kill(pid, 0) still says it exists. Popen.poll is the required
            # waitpid(WNOHANG) path for children this host owns.
            exited = (run.process is not None and run.process.poll() is not None)
            if exited or (run.process is None and not self.alive(run.pid)):
                self.handle_dead(name, run)

    def park_all(self):
        for name, run in list(self.running.items()):
            run.contract.park(self.dsn, name)
            print(f"[supervisor] parked {name}", flush=True)
        self.running.clear()

    def stop_all(self):
        for name, run in list(self.running.items()):
            if self.alive(run.pid):
                try:
                    os.kill(run.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
            if run.process is not None:
                try:
                    run.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    run.process.kill()
                    run.process.wait()
            else:
                deadline = time.monotonic() + 5
                while self.alive(run.pid) and time.monotonic() < deadline:
                    time.sleep(min(self.poll, 0.05))
            self.finish(run, 0, True)
            print(f"[supervisor] stopped {name} {run.contract.state_name().title()}",
                  flush=True)
            try:
                self.pidfile(name).unlink()
            except FileNotFoundError:
                pass
        self.running.clear()

    def loop(self):
        while self.running:
            if self.stop_requested:
                self.stop_all()
                return
            if self.park_requested:
                self.park_all()
                return
            self.poll_once()
            if self.running:
                time.sleep(self.poll)


def service_spec(text):
    name, marker, command = text.partition("=")
    if not marker or not name or not command or "/" in name:
        raise argparse.ArgumentTypeError("service must be NAME=CMD with a simple NAME")
    return name, command


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dsn", required=True, help="SQLite database path")
    parser.add_argument("--piddir", required=True, help="directory for PID RUNID files")
    parser.add_argument("--service", action="append", type=service_spec,
                        required=True, metavar="NAME=CMD")
    parser.add_argument("--max-crashes", type=int, default=2)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--poll", type=float, default=0.2)
    args = parser.parse_args(argv)
    if args.max_crashes < 0:
        parser.error("--max-crashes must be non-negative")
    if args.poll <= 0:
        parser.error("--poll must be positive")
    names = [name for name, _command in args.service]
    if len(names) != len(set(names)):
        parser.error("each --service NAME must be unique")
    return args


def main(argv=None):
    args = parse_args(argv)
    with Runtime(OUT / "libashrt.so") as runtime:
        runtime.load(OUT / "libservice.ash.so")
        supervisor = Supervisor(runtime, args.dsn, args.piddir,
                                args.max_crashes, args.poll)
        runtime.bind("Service.start", supervisor.bind_start)
        runtime.bind("Service.ready", supervisor.bind_ready)
        runtime.freeze()

        def request_park(_signum, _frame):
            supervisor.park_requested = True

        def request_stop(_signum, _frame):
            supervisor.stop_requested = True

        signal.signal(signal.SIGTERM, request_park)
        signal.signal(signal.SIGINT, request_park)
        signal.signal(signal.SIGUSR1, request_stop)
        for name, command in args.service:
            supervisor.resume_or_start(name, command, args.resume)
        supervisor.loop()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AshError, OSError, RuntimeError, sqlite3.Error) as error:
        print(f"[supervisor] error {error}", file=sys.stderr, flush=True)
        raise SystemExit(1)
