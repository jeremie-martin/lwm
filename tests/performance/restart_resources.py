#!/usr/bin/env python3
"""Measure retained X-server clients across restart and exec recovery on a private Xvfb (requires libXRes)."""

import ctypes as c
import os
import json
import select
import subprocess
import tempfile
from pathlib import Path
from contextlib import ExitStack
from transition_counts import X, DISPLAY, WINDOW, INT, stop, ipc, wait

R = c.CDLL("libXRes.so.1")


class Client(c.Structure):
    _fields_ = [("base", WINDOW), ("mask", WINDOW)]


R.XResQueryClients.argtypes = [DISPLAY, c.POINTER(INT), c.POINTER(c.POINTER(Client))]
R.XResQueryClients.restype = INT
X.XFree.argtypes = [c.c_void_p]


def measure(binary):
    with tempfile.TemporaryDirectory() as temporary, ExitStack() as cleanup:
        directory = Path(temporary)
        logfile = directory / "wm.log"
        log = cleanup.enter_context(logfile.open("wb"))
        r, w = os.pipe()
        server = subprocess.Popen(
            [
                "Xvfb",
                "-displayfd",
                str(w),
                "-screen",
                "0",
                "800x600x24",
                "-nolisten",
                "tcp",
            ],
            pass_fds=[w],
            stdout=log,
            stderr=log,
        )
        cleanup.callback(stop, server)
        os.close(w)
        try:
            if not select.select([r], [], [], 5)[0]:
                raise TimeoutError("Xvfb")
            display_name = ":" + os.read(r, 32).decode().strip()
        finally:
            os.close(r)
        display = X.XOpenDisplay(display_name.encode())
        if not display:
            raise RuntimeError("Cannot open private X display")
        cleanup.callback(lambda: X.XCloseDisplay(display) if display else None)

        def clients():
            count = INT()
            data = c.POINTER(Client)()
            if not R.XResQueryClients(display, c.byref(count), c.byref(data)):
                raise RuntimeError("X Resource extension query failed")
            try:
                return count.value
            finally:
                X.XFree(data)

        config = directory / "config.toml"
        config.write_text("[workspaces]\ncount = 2\n")
        environment = dict(os.environ, DISPLAY=display_name, XDG_RUNTIME_DIR=temporary)
        environment.pop("LWM_SOCKET", None)
        wm = subprocess.Popen(
            [
                str(Path(binary).resolve()),
                "--config",
                str(config),
                "--log-target",
                "stderr",
                "--log-level",
                "off",
            ],
            env=environment,
            stdout=log,
            stderr=log,
        )
        cleanup.callback(stop, wm)
        path = directory / "lwm" / ("ipc-" + display_name.replace(":", "_") + ".sock")
        wait(lambda: ipc(path, "ping") == b"pong", wm, logfile)

        def changed(old):
            try:
                return json.loads(ipc(path, "state"))["instance"] != old
            except (ConnectionResetError, BrokenPipeError):
                return False

        before = clients()
        for index in range(20):
            old = json.loads(ipc(path, "state"))["instance"]
            ipc(path, "restart" if index % 2 else "exec /definitely/missing/lwm-binary")
            wait(lambda: changed(old), wm, logfile)
        # With no application/observer connection, the server may reset unless
        # the handoff preserves it. IPC uses no X connection of its own.
        ipc(path, "workspace switch 1")
        snapshot = json.loads(ipc(path, "state"))
        X.XCloseDisplay(display)
        display = None
        ipc(path, "restart")
        wait(lambda: changed(snapshot["instance"]), wm, logfile)
        if json.loads(ipc(path, "state"))["workspaces"] != snapshot["workspaces"]:
            raise AssertionError("Empty-display restart lost workspace state")
        display = X.XOpenDisplay(display_name.encode())
        if not display:
            raise RuntimeError("Cannot reopen private X display")
        return {
            "binary": binary,
            "restarts": 21,
            "clients_before": before,
            "clients_after": clients(),
        }


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    result = measure(str(args.binary.resolve(strict=True)))
    print(json.dumps(result))
    if args.check and result["clients_after"] != result["clients_before"]:
        raise AssertionError("Restart retained X-server clients: " + json.dumps(result))
