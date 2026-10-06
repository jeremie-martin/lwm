#!/usr/bin/env python3
"""Measure actual WM latency/CPU and blocked-stderr behavior on a private Xvfb.

Optionally compare a pre-Quill LWM binary. This measures the complete implementation,
including changed call sites and delivery policy, not isolated library performance.
"""
import argparse
from contextlib import ExitStack
import json
import hashlib
import os
from pathlib import Path
import random
import select
import socket
import statistics
import subprocess
import tempfile
import time

import transition_counts as harness


def request(path, command, timeout=1):
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(timeout)
        peer.connect(str(path))
        peer.sendall(command.encode() + b"\n")
        reply = b""
        while chunk := peer.recv(65536):
            reply += chunk
        if reply == b"ok\n":
            return b""
        if not reply.startswith(b"ok "):
            raise RuntimeError(reply)
        return reply[3:].strip()


def measure(binary, level, blocked=False, affinity=None, switches=400, target_name="stderr"):
    with tempfile.TemporaryDirectory(prefix="lwm-log-perf-") as temporary, ExitStack() as cleanup:
        directory = Path(temporary)
        server_log = cleanup.enter_context((directory / "server.log").open("wb"))
        read_fd, write_fd = os.pipe()
        server = subprocess.Popen(["Xvfb", "-displayfd", str(write_fd), "-screen", "0", "1280x800x24",
                                   "-nolisten", "tcp", "-noreset"], pass_fds=[write_fd], stdout=server_log, stderr=server_log)
        cleanup.callback(harness.stop, server)
        if affinity:
            os.sched_setaffinity(server.pid, {affinity[2]})
        os.close(write_fd)
        try:
            if not select.select([read_fd], [], [], 5)[0]:
                raise TimeoutError("Xvfb startup")
            display = ":" + os.read(read_fd, 32).decode().strip()
        finally:
            os.close(read_fd)
        config = directory / "config.toml"
        config.write_text("[workspaces]\ncount=2\n")
        environment = dict(os.environ, DISPLAY=display, XDG_RUNTIME_DIR=temporary)
        environment.pop("LWM_SOCKET", None)
        if blocked:
            reader, writer = os.pipe()
            cleanup.callback(os.close, reader)
            cleanup.callback(os.close, writer)
            target = writer
        else:
            target = cleanup.enter_context(open(os.devnull, "wb"))
        target_args = ["--log-target", "stderr" if blocked else target_name]
        wm = subprocess.Popen([str(binary), "--config", str(config), "--log-level", level,
                               "--log-color", "never", *target_args],
                              env=environment, stdout=server_log, stderr=target)
        cleanup.callback(harness.stop, wm)
        if affinity:
            os.sched_setaffinity(wm.pid, {affinity[0]})
        path = directory / "lwm" / ("ipc-" + display.replace(":", "_") + ".sock")
        deadline = time.monotonic() + 5
        while True:
            try:
                request(path, "ping")
                break
            except OSError:
                if wm.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("WM startup failed")
                time.sleep(.005)
        if affinity:
            for task in Path(f"/proc/{wm.pid}/task").iterdir():
                if int(task.name) != wm.pid:
                    os.sched_setaffinity(int(task.name), {affinity[1]})
        x = harness.X
        connection = x.XOpenDisplay(display.encode())
        if not connection:
            raise RuntimeError("XOpenDisplay failed")
        cleanup.callback(x.XCloseDisplay, connection)
        root = x.XDefaultRootWindow(connection)
        x.XWarpPointer(connection, 0, root, 0, 0, 0, 0, 1279, 799)
        for _ in range(1 if blocked else 40):
            window = x.XCreateSimpleWindow(connection, root, 10, 10, 200, 100, 0, 0, 0)
            x.XMapWindow(connection, window)
        x.XSync(connection, 0)
        deadline = time.monotonic() + 5
        while len(json.loads(request(path, "window list"))["windows"]) != (1 if blocked else 40):
            if time.monotonic() >= deadline:
                raise TimeoutError("window management")
            time.sleep(.005)
        row = {"binary": str(binary), "level": level, "blocked": blocked, "target": "stderr" if blocked else target_name, "affinity": affinity, "sha256": hashlib.sha256(binary.read_bytes()).hexdigest()}
        if blocked:
            before = harness.wm_owner(display)
            os.set_blocking(writer, False)
            try:
                while True:
                    os.write(writer, b"x" * 4096)
            except BlockingIOError:
                pass
            os.set_blocking(writer, True)
            # An INFO reload outcome guarantees a write after the pipe is full.
            try:
                request(path, "reload-config", .3)
            except TimeoutError:
                pass  # A historical synchronous logger can stall here too.
            time.sleep(.2)
            samples = []
            for command in ["workspace switch 1", "ping", "restart"]:
                start = time.monotonic()
                if command == "restart":
                    restart_started = start
                try:
                    request(path, command, .3)
                    samples.append({"command": command, "timeout": False, "ms": (time.monotonic() - start) * 1000})
                except TimeoutError:
                    samples.append({"command": command, "timeout": True})
            row["commands"] = samples
            row["restart_completed_while_blocked"] = False
            deadline = time.monotonic() + .3
            while time.monotonic() < deadline:
                try:
                    if harness.wm_owner(display) not in (0, before) and request(path, "ping", max(.001, deadline - time.monotonic())) == b"pong":
                        row["restart_completed_while_blocked"] = True
                        row["restart_completion_ms"] = (time.monotonic() - restart_started) * 1000
                        break
                except (OSError, RuntimeError):
                    pass
                time.sleep(.001)
            os.set_blocking(reader, False)
            resumed = time.monotonic()
            deadline = resumed + 3
            while time.monotonic() < deadline:
                try:
                    while os.read(reader, 65536):
                        pass
                except BlockingIOError:
                    pass
                try:
                    if harness.wm_owner(display) not in (0, before) and request(path, "ping", .02) == b"pong":
                        row["restart_after_reader_resumes_ms"] = (time.monotonic() - resumed) * 1000
                        break
                except (OSError, RuntimeError):
                    pass
                time.sleep(.001)
            else:
                raise TimeoutError("restart did not recover after stderr reader resumed")
            wm.terminate()
            deadline = time.monotonic() + 1
            while wm.poll() is None and time.monotonic() < deadline:
                try:
                    os.read(reader, 65536)
                except BlockingIOError:
                    time.sleep(.001)
            return row

        def cpu():
            return sum(int(v) for v in Path(f"/proc/{wm.pid}/stat").read_text().split()[13:15]) / os.sysconf("SC_CLK_TCK")

        for i in range(20):
            request(path, f"workspace switch {i % 2}")
        cpu_start = cpu()
        elapsed_start = time.monotonic()
        latencies = []
        for i in range(switches):
            start = time.perf_counter_ns()
            request(path, f"workspace switch {i % 2}")
            latencies.append((time.perf_counter_ns() - start) / 1000)
        row.update(clients=40, switches=switches, wall_ms=(time.monotonic() - elapsed_start) * 1000,
                   wm_cpu_ms=(cpu() - cpu_start) * 1000, p50_us=statistics.median(latencies),
                   p99_us=sorted(latencies)[max(0, (99 * switches + 99) // 100 - 1)])
        time.sleep(.1)  # finish pending output before measuring idle worker cost
        cpu_start = cpu()
        elapsed_start = time.monotonic()
        time.sleep(2)
        row["idle_cpu_percent_one_core"] = 100 * (cpu() - cpu_start) / (time.monotonic() - elapsed_start)
        row["logging"] = json.loads(request(path, "log status"))
        return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--target", choices=["stderr", "journal"], default="stderr",
                        help="ordinary-workload destination; journal writes to the system journal")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--switches", type=int, default=400)
    parser.add_argument("--affinity", type=int, nargs=4, metavar=("WM", "WORKER", "XSERVER", "DRIVER"),
                        help="pin the four roles to Linux CPU IDs, preferably separate physical cores")
    parser.add_argument("--include-off", action="store_true", help="also measure logging disabled")
    args = parser.parse_args()
    if args.repeats < 1 or args.switches < 1:
        parser.error("--repeats and --switches must be positive")
    if args.affinity:
        os.sched_setaffinity(0, {args.affinity[3]})
    binaries = [args.binary.resolve()]
    if args.baseline:
        binaries.append(args.baseline.resolve())
    for repeat in range(args.repeats):
        cases = [(binary, level) for binary in binaries for level in ["info", "trace"]]
        if args.include_off:
            cases.append((args.binary.resolve(), "off"))
        random.Random(repeat).shuffle(cases)
        for binary, level in cases:
            print(json.dumps(dict(measure(binary, level, affinity=args.affinity, switches=args.switches, target_name=args.target), repeat=repeat)), flush=True)
    for binary in binaries:
        print(json.dumps(measure(binary, "info", blocked=True, affinity=args.affinity)), flush=True)


if __name__ == "__main__":
    main()
