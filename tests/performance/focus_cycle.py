#!/usr/bin/env python3
"""Measure completed focus commands on a private Xvfb display (Release builds)."""
import argparse
from contextlib import ExitStack
import ctypes as c
import json
import os
from pathlib import Path
import select
import statistics
import subprocess
import tempfile
import time

from transition_counts import X, WINDOW, ipc, stop, wait


def measure(binary, count, operations, direction, transients):
    with tempfile.TemporaryDirectory(prefix="lwm-focus-") as temporary, ExitStack() as cleanup:
        directory = Path(temporary)
        read_fd, write_fd = os.pipe()
        log_path = directory / "wm.log"
        log = cleanup.enter_context(log_path.open("wb"))
        server = subprocess.Popen(
            ["Xvfb", "-displayfd", str(write_fd), "-screen", "0", "1600x1000x24", "-nolisten", "tcp", "-noreset"],
            pass_fds=[write_fd], stdout=log, stderr=log)
        cleanup.callback(stop, server)
        os.close(write_fd)
        try:
            if not select.select([read_fd], [], [], 5)[0]:
                raise TimeoutError("Xvfb startup")
            display_name = ":" + os.read(read_fd, 32).decode().strip()
        finally:
            os.close(read_fd)
        display = X.XOpenDisplay(display_name.encode())
        if not display:
            raise RuntimeError("Cannot open private X display")
        cleanup.callback(X.XCloseDisplay, display)
        root = X.XDefaultRootWindow(display)
        X.XWarpPointer(display, 0, root, 0, 0, 0, 0, 1599, 999)
        config = directory / "config.toml"
        config.write_text("[appearance]\npadding = 10\n")
        environment = dict(os.environ, DISPLAY=display_name, XDG_RUNTIME_DIR=temporary)
        environment.pop("LWM_SOCKET", None)
        wm = subprocess.Popen([str(binary), "--config", str(config), "--log-target", "stderr", "--log-level", "error"],
                              env=environment, stdout=log, stderr=log)
        cleanup.callback(stop, wm)
        path = directory / "lwm" / ("ipc-" + display_name.replace(":", "_") + ".sock")
        wait(lambda: ipc(path, "ping") == b"pong", wm, log_path)
        atom = lambda name: X.XInternAtom(display, name.encode(), 0)
        windows = []
        for _ in range(count):
            window = X.XCreateSimpleWindow(display, root, 20, 20, 300, 200, 0, 0, 0)
            value = WINDOW(atom("_NET_WM_WINDOW_TYPE_DIALOG"))
            X.XChangeProperty(display, window, atom("_NET_WM_WINDOW_TYPE"), atom("ATOM"), 32, 0, c.byref(value), 1)
            X.XMapWindow(display, window)
            windows.append(window)
        X.XSync(display, 0)
        wait(lambda: len(json.loads(ipc(path, "window list"))["windows"]) == count, wm, log_path)
        if transients != "none":
            for index, window in enumerate(windows):
                parent = windows[(index + 1) % count] if index + 1 < count or transients == "cycle" else 0
                value = WINDOW(parent)
                X.XChangeProperty(display, window, atom("WM_TRANSIENT_FOR"), atom("WINDOW"), 32, 0, c.byref(value), 1)
            marker = c.create_string_buffer(b"transient-setup-complete")
            X.XChangeProperty(display, windows[-1], atom("_NET_WM_NAME"), atom("UTF8_STRING"),
                              8, 0, marker, len(marker.value))
            X.XSync(display, 0)
            wait(lambda: any(w["id"] == windows[-1] and w["title"] == marker.value.decode()
                             for w in json.loads(ipc(path, "window list"))["windows"]), wm, log_path)
        windows = set(windows)
        def step():
            target = int(ipc(path, "focus " + direction))
            if target not in windows:
                raise AssertionError("Focus returned a non-client")
            return target
        for _ in range(count):
            step()
        durations, visited = [], set()
        for _ in range(operations):
            start = time.perf_counter_ns()
            target = step()
            durations.append((time.perf_counter_ns() - start) / 1000)
            visited.add(target)
        durations.sort()
        return dict(clients=count, operations=operations, direction=direction, transients=transients, distinct_targets=len(visited),
                    median_us=statistics.median(durations), p95_us=durations[int(.95 * (len(durations)-1))],
                    p99_us=durations[int(.99 * (len(durations)-1))])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--clients", type=int, nargs="+", default=[10, 100, 500])
    parser.add_argument("--transients", choices=["none", "chain", "cycle"], default="none")
    parser.add_argument("--direction", choices=["next", "prev"], default="prev")
    parser.add_argument("--operations", type=int, default=2000)
    args = parser.parse_args()
    if min(args.clients) < 1 or args.operations < 1:
        parser.error("clients and operations must be positive")
    for count in args.clients:
        print(json.dumps(measure(args.binary.resolve(strict=True), count, args.operations, args.direction, args.transients)), flush=True)
