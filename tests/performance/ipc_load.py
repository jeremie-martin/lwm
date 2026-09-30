#!/usr/bin/env python3
"""Measure independent IPC callers against an owned WM and Xvfb (Linux)."""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import select
import socket
import statistics
import subprocess
import tempfile
import time
from contextlib import ExitStack

def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def request(path):
    start = time.perf_counter_ns()
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(3)
        peer.connect(str(path))
        try:
            peer.sendall(b"ping\n")
            result = b""
            while chunk := peer.recv(1024):
                result += chunk
        except OSError as error:
            result = ("socket error: " + type(error).__name__).encode()
    return result.decode().strip(), (time.perf_counter_ns() - start) / 1000


def measure(binary, stalled, requests):
    with tempfile.TemporaryDirectory(prefix="lwm-ipc-load-") as temporary, ExitStack() as cleanup:
        directory = Path(temporary)
        read_fd, write_fd = os.pipe()
        log = cleanup.enter_context((directory / "server.log").open("wb"))
        server = subprocess.Popen(["Xvfb", "-displayfd", str(write_fd), "-screen", "0", "800x600x24", "-nolisten", "tcp", "-noreset"],
                                  pass_fds=[write_fd], stdout=log, stderr=log)
        cleanup.callback(stop, server)
        os.close(write_fd)
        try:
            if not select.select([read_fd], [], [], 5)[0]:
                raise TimeoutError("Xvfb startup")
            display = ":" + os.read(read_fd, 32).decode().strip()
        finally:
            os.close(read_fd)
        config = directory / "config.toml"
        config.write_text("[workspaces]\ncount = 2\n")
        environment = dict(os.environ, DISPLAY=display, XDG_RUNTIME_DIR=temporary)
        environment.pop("LWM_SOCKET", None)
        environment["LWM_LOG_SOCKET"] = str(directory / "unused-journal")
        wm = subprocess.Popen([str(binary), "--config", str(config), "--log-level", "error"],
                              env=environment, stdout=log, stderr=log)
        cleanup.callback(stop, wm)
        path = directory / "lwm" / ("ipc-" + display.replace(":", "_") + ".sock")
        deadline = time.monotonic() + 5
        while True:
            try:
                if request(path)[0] == "ok pong":
                    break
            except (FileNotFoundError, ConnectionRefusedError):
                pass
            if wm.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("WM startup failed")
            time.sleep(0.005)
        if stalled:
            peer = cleanup.enter_context(socket.socket(socket.AF_UNIX))
            peer.connect(str(path))
            peer.sendall(b"pi")
            time.sleep(0.02)  # Let the WM accept this incomplete request first.
        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as workers:
            results = list(workers.map(lambda _: request(path), range(requests)))
        successes = [latency for response, latency in results if response == "ok pong"]
        errors = {}
        for response, _ in results:
            if response != "ok pong":
                errors[response] = errors.get(response, 0) + 1
        return {"stalled_request": stalled, "requests": requests, "successes": len(successes), "errors": errors,
                "median_success_us": statistics.median(successes) if successes else None,
                "p95_success_us": sorted(successes)[int((len(successes) - 1) * .95)] if successes else None}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    for stalled in (False, True):
        result = measure(args.binary.resolve(strict=True), stalled, 512)
        print(json.dumps(result), flush=True)
        if args.check and result["successes"] != result["requests"]:
            raise AssertionError("Independent callers failed: " + json.dumps(result))


if __name__ == "__main__":
    main()
