#!/usr/bin/env python3
"""Count WM-side X requests on an owned Xvfb server; no production instrumentation."""

import argparse
import ctypes as c
import json
import os
from pathlib import Path
import select
import struct
import subprocess
import tempfile
import time
from contextlib import ExitStack

X = c.CDLL("libX11.so.6")
DISPLAY, WINDOW, INT = c.c_void_p, c.c_ulong, c.c_int
for name, result, arguments in [
    ("XOpenDisplay", DISPLAY, [c.c_char_p]),
    ("XDefaultRootWindow", WINDOW, [DISPLAY]),
    ("XCreateSimpleWindow", WINDOW, [DISPLAY, WINDOW, INT, INT, c.c_uint, c.c_uint, c.c_uint, WINDOW, WINDOW]),
    ("XMapWindow", INT, [DISPLAY, WINDOW]),
    ("XSync", INT, [DISPLAY, INT]),
    ("XInternAtom", WINDOW, [DISPLAY, c.c_char_p, INT]),
    ("XChangeProperty", INT, [DISPLAY, WINDOW, WINDOW, WINDOW, INT, INT, c.c_void_p, INT]),
    ("XCloseDisplay", INT, [DISPLAY]),
    ("XSendEvent", INT, [DISPLAY, WINDOW, INT, c.c_long, c.c_void_p]),
    ("XGetGeometry", INT, [DISPLAY, WINDOW, c.POINTER(WINDOW), c.POINTER(INT), c.POINTER(INT),
                           c.POINTER(c.c_uint), c.POINTER(c.c_uint), c.POINTER(c.c_uint), c.POINTER(c.c_uint)]),
    ("XWarpPointer", INT, [DISPLAY, WINDOW, WINDOW, INT, INT, c.c_uint, c.c_uint, INT, INT]),
    ("XGetSelectionOwner", WINDOW, [DISPLAY, WINDOW]),
    ("XSelectInput", INT, [DISPLAY, WINDOW, c.c_long]),
    ("XFlush", INT, [DISPLAY]),
    ("XPending", INT, [DISPLAY]),
    ("XNextEvent", INT, [DISPLAY, c.c_void_p]),
    ("XFree", INT, [c.c_void_p]),
    ("XGetWindowProperty", INT, [DISPLAY, WINDOW, WINDOW, c.c_long, c.c_long, INT, WINDOW, c.POINTER(WINDOW),
                                 c.POINTER(INT), c.POINTER(c.c_ulong), c.POINTER(c.c_ulong), c.POINTER(c.c_void_p)]),
]:
    function = getattr(X, name)
    function.restype = result
    function.argtypes = arguments


class PointerEvent(c.Structure):
    _fields_ = [("type", INT), ("serial", c.c_ulong), ("send_event", INT),
                ("display", DISPLAY), ("window", WINDOW), ("root", WINDOW),
                ("subwindow", WINDOW), ("time", c.c_ulong), ("x", INT), ("y", INT),
                ("x_root", INT), ("y_root", INT), ("state", c.c_uint),
                ("button", c.c_uint), ("same_screen", INT)]


class ClientMessage(c.Structure):
    _fields_ = [("type", INT), ("serial", c.c_ulong), ("send_event", INT),
                ("display", DISPLAY), ("window", WINDOW), ("message_type", WINDOW),
                ("format", INT), ("data", c.c_long * 5)]


def pointer(display, root, kind, x, y, child=0):
    event = PointerEvent(type=kind, display=display, window=root, root=root,
                         subwindow=child, x=x, y=y, x_root=x, y_root=y,
                         state=64, button=0 if kind == 6 else 1, same_screen=1)
    mask = 64 if kind == 6 else (4 if kind == 4 else 8)
    X.XSendEvent(display, root, 0, mask, c.byref(event))


def position(display, window):
    root, x, y = WINDOW(), INT(), INT()
    width, height, border, depth = (c.c_uint() for _ in range(4))
    if not X.XGetGeometry(display, window, c.byref(root), c.byref(x), c.byref(y),
                          c.byref(width), c.byref(height), c.byref(border), c.byref(depth)):
        raise RuntimeError("Missing drag window")
    return x.value, y.value


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


class PropertyEvent(c.Structure):
    _fields_ = [("type", INT), ("serial", c.c_ulong), ("send_event", INT), ("display", DISPLAY),
                ("window", WINDOW), ("atom", WINDOW), ("time", c.c_ulong), ("state", INT)]


class Event(c.Union):
    _fields_ = [("type", INT), ("property", PropertyEvent), ("pad", c.c_long * 24)]


def text_property(display, window, property, delete=False):
    kind, size_format, count, after, data = WINDOW(), INT(), c.c_ulong(), c.c_ulong(), c.c_void_p()
    utf8 = X.XInternAtom(display, b"UTF8_STRING", 0)
    X.XGetWindowProperty(display, window, property, 0, 1 << 24, int(delete), utf8, c.byref(kind),
                         c.byref(size_format), c.byref(count), c.byref(after), c.byref(data))
    value = c.string_at(data, count.value) if data else b""
    X.XFree(data)
    return value


def published_state(display):
    """The WM's published state, read from the X server without involving the WM."""
    owner = X.XGetSelectionOwner(display, X.XInternAtom(display, b"WM_S0", 0))
    value = text_property(display, owner, X.XInternAtom(display, b"_LWM_STATE", 0)) if owner else b""
    return json.loads(value) if value else {"windows": {"windows": []}}


def ipc(display_name, command, timeout=3):
    """Send one command as lwmctl does, on a private connection; return the reply value."""
    display = X.XOpenDisplay(display_name.encode())
    if not display:
        raise ConnectionRefusedError("X display")
    try:
        atom = lambda name: X.XInternAtom(display, name.encode(), 0)
        owner = X.XGetSelectionOwner(display, atom("WM_S0"))
        if not owner:
            raise ConnectionRefusedError("lwm is not running")
        request, reply, utf8 = atom("_LWM_COMMAND"), atom("_LWM_REPLY"), atom("UTF8_STRING")
        window = X.XCreateSimpleWindow(display, X.XDefaultRootWindow(display), -1, -1, 1, 1, 0, 0, 0)
        X.XSelectInput(display, window, 1 << 22)  # PropertyChangeMask
        text = command.encode()
        X.XChangeProperty(display, window, request, utf8, 8, 0, text, len(text))
        message = ClientMessage(type=33, display=display, window=owner, message_type=request, format=32)
        message.data[0] = window
        X.XSendEvent(display, owner, 0, 0, c.byref(message))
        X.XFlush(display)
        deadline = time.monotonic() + timeout
        event = Event()
        while time.monotonic() < deadline:
            if not X.XPending(display):
                time.sleep(0.0002)
                continue
            X.XNextEvent(display, c.byref(event))
            if event.type != 28 or event.property.atom != reply or event.property.state != 0:
                continue
            value = text_property(display, window, reply, delete=True)
            if value != b"ok" and not value.startswith(b"ok "):
                raise RuntimeError((command, value))
            return value[3:]
        raise TimeoutError(command)
    finally:
        X.XCloseDisplay(display)


def wm_owner(display_name):
    """The WM_S0 owner window, which identifies one WM lifetime; 0 while none runs."""
    display = X.XOpenDisplay(display_name.encode())
    if not display:
        return 0
    try:
        return X.XGetSelectionOwner(display, X.XInternAtom(display, b"WM_S0", 0))
    finally:
        X.XCloseDisplay(display)


def wait(predicate, wm, log):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if wm.poll() is not None:
            raise RuntimeError(log.read_text())
        try:
            if predicate():
                return
        except (FileNotFoundError, ConnectionRefusedError):
            pass
        time.sleep(0.005)
    raise TimeoutError("WM did not complete the operation: " + log.read_text())


def measure(binary, library, scenario, operations):
    with tempfile.TemporaryDirectory(prefix="lwm-counts-") as temporary, ExitStack() as cleanup:
        directory = Path(temporary)
        read_fd, write_fd = os.pipe()
        server_log = cleanup.enter_context((directory / "server.log").open("wb"))
        server = subprocess.Popen(
            ["Xvfb", "-displayfd", str(write_fd), "-screen", "0", "1600x1000x24", "-nolisten", "tcp", "-noreset"],
            pass_fds=[write_fd], stdout=server_log, stderr=server_log,
        )
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
        X.XSync(display, 0)
        config = ('[workspaces]\nnames = ["1", "2"]\n[appearance]\npadding = 10\nborder_width = 1\n'
                  '[mousebinds]\n"super+1" = "drag_window"\n"super+3" = "resize_floating"\n')
        if scenario in ("sticky", "sticky_fullscreen"):
            for digits, enabled in [("02468", "true"), ("13579", "false")]:
                actions = f"sticky = {enabled}"
                if scenario == "sticky_fullscreen":
                    actions += f", fullscreen = {enabled}"
                config += f"[[rules]]\nmatch = {{ title = 'title-.*[{digits}]' }}\napply = {{ {actions} }}\n"
        config_path = directory / "config.toml"
        config_path.write_text(config)
        counts_path = directory / "counts"
        counts_path.write_bytes(bytes(48))
        log_path = directory / "wm.log"
        log = cleanup.enter_context(log_path.open("wb"))
        environment = dict(os.environ, DISPLAY=display_name, LD_PRELOAD=str(library),
                           LWM_TRANSITION_COUNTS=str(counts_path))
        atom = lambda name: X.XInternAtom(display, name.encode(), 0)
        if scenario == "dock_startup":
            for index in range(operations):
                window = X.XCreateSimpleWindow(display, root, 0, 0, 200, 20, 0, 0, 0)
                value = WINDOW(atom("_NET_WM_WINDOW_TYPE_DOCK"))
                X.XChangeProperty(display, window, atom("_NET_WM_WINDOW_TYPE"), atom("ATOM"),
                                  32, 0, c.byref(value), 1)
                values = (WINDOW * 4)(0, 0, 20 + index, 0)
                X.XChangeProperty(display, window, atom("_NET_WM_STRUT"), atom("CARDINAL"),
                                  32, 0, values, 4)
                X.XMapWindow(display, window)
            X.XSync(display, 0)
        wm = subprocess.Popen([str(binary), "--config", str(config_path), "--log-target", "stderr", "--log-level", "error"],
                              env=environment, stdout=log, stderr=log)
        cleanup.callback(stop, wm)
        wait(lambda: ipc(display_name, "ping") == b"pong", wm, log_path)
        if scenario == "dock_startup":
            # The successful ping follows startup completion, including adoption.
            counts = struct.unpack("=6Q", counts_path.read_bytes())
            return dict(scenario=scenario, operations=operations,
                        counts=dict(zip(("get_property", "geometry_configure", "visibility_barrier", "query_tree", "flush", "get_geometry"), counts)))
        window_type, dialog, atom_type = atom("_NET_WM_WINDOW_TYPE"), atom("_NET_WM_WINDOW_TYPE_DIALOG"), atom("ATOM")
        windows = []
        client_count = 200 if scenario == "workspace" else 10
        for index in range(client_count):
            window = X.XCreateSimpleWindow(display, root, 10 + index % 10 * 20, 10 + index % 10 * 20, 300, 200, 0, 0, 0)
            if not scenario.startswith("tiled_"):
                value = WINDOW(dialog)
                X.XChangeProperty(display, window, window_type, atom_type, 32, 0, c.byref(value), 1)
            X.XMapWindow(display, window)
            windows.append(window)
        X.XSync(display, 0)
        wait(lambda: len(json.loads(ipc(display_name, "window list"))["windows"]) == client_count, wm, log_path)
        target = windows[0]
        ipc(display_name, f"focus window={target}")
        name_atom, utf8 = atom("_NET_WM_NAME"), atom("UTF8_STRING")
        dragging = "drag" in scenario
        relocating = "relocation" in scenario
        desktop_atom = atom("_NET_WM_DESKTOP")
        if dragging:
            pointer(display, root, 4, 100, 100, target)
            marker = c.create_string_buffer(b"drag-started")
            X.XChangeProperty(display, target, name_atom, utf8, 8, 0, marker, 12)
            X.XSync(display, 0)
            wait(lambda: any(client["id"] == target and client["title"] == "drag-started"
                             for client in json.loads(ipc(display_name, "window list"))["windows"]), wm, log_path)
        time.sleep(0.05)
        start_position = position(display, target) if dragging else None
        ratio_position = position(display, windows[1]) if scenario == "tiled_ratio" else None
        before = struct.unpack("=6Q", counts_path.read_bytes())
        for index in range(operations):
            if scenario == "tiled_ratio":
                ipc(display_name, f"ratio set {0.45 if index % 2 == 0 else 0.55}")
                continue
            if scenario == "workspace":
                ipc(display_name, f"workspace switch {index % 2}")
                continue
            if relocating:
                event = ClientMessage(type=33, display=display, window=target,
                                      message_type=desktop_atom, format=32,
                                      data=(c.c_long * 5)((index + 1) % 2, 2, 0, 0, 0))
                X.XSendEvent(display, root, 0, (1 << 19) | (1 << 20), c.byref(event))
            if dragging:
                pointer(display, root, 6, 101 + index, 101 + index)
                if scenario.endswith("burst") and index != operations - 1:
                    continue
            title = f"title-{index}"
            value = c.create_string_buffer(title.encode())
            X.XChangeProperty(display, target, name_atom, utf8, 8, 0, value, len(title))
            X.XSync(display, 0)
            def settled():
                # Watching costs the WM nothing; a drag publishes on release,
                # so its progress is the window's own position.
                if dragging:
                    return position(display, target) == tuple(value + index + 1 for value in start_position)
                clients = published_state(display)["windows"]["windows"]
                for client in clients:
                    if client["id"] != target or client["title"] != title:
                        continue
                    if relocating:
                        return client["workspace"] == (index + 1) % 2
                    enabled = index % 2 == 0
                    return (scenario == "metadata" or dragging or client["sticky"] == enabled) and (
                        scenario != "sticky_fullscreen" or client["fullscreen"] == enabled
                    )
                return False
            wait(settled, wm, log_path)

        if scenario == "tiled_ratio" and position(display, windows[1]) == ratio_position:
            raise AssertionError("Ratio workload did not change layout")
        if relocating:
            workspaces = json.loads(ipc(display_name, "workspace list"))["monitors"][0]["workspaces"]
            expected = [client_count - operations % 2, operations % 2] if scenario.startswith("tiled_") else [0, 0]
            if [workspace["window_count"] for workspace in workspaces] != expected:
                raise AssertionError("Relocation lost or duplicated tiled membership")
        if dragging:
            expected = tuple(value + operations for value in start_position)
            if position(display, target) != expected:
                raise AssertionError(f"Drag did not reach its final position: {position(display, target)} != {expected}")
            # A title event after release establishes completion, including a tiled drop.
            pointer(display, root, 5, 100 + operations, 100 + operations)
            marker = c.create_string_buffer(b"drag-finished")
            X.XChangeProperty(display, target, name_atom, utf8, 8, 0, marker, 13)
            X.XSync(display, 0)
            wait(lambda: any(client["id"] == target and client["title"] == "drag-finished"
                             for client in json.loads(ipc(display_name, "window list"))["windows"]), wm, log_path)
        time.sleep(0.05)
        after = struct.unpack("=6Q", counts_path.read_bytes())
        names = ("get_property", "geometry_configure", "visibility_barrier", "query_tree", "flush", "get_geometry")
        return dict(scenario=scenario, operations=operations,
                    counts=dict(zip(names, (end - start for start, end in zip(before, after)))))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="Release lwm executable")
    parser.add_argument("--check", action="store_true", help="enforce transition request budgets")
    arguments = parser.parse_args()
    binary = arguments.binary.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="lwm-tracer-") as temporary:
        library = Path(temporary) / "xcb_counts.so"
        subprocess.run(["cc", "-shared", "-fPIC", "-O2", "-o", str(library),
                        str(Path(__file__).with_name("xcb_counts.c")), "-ldl"], check=True)
        scenarios = [(name, 200) for name in ("metadata", "sticky", "sticky_fullscreen", "workspace")]
        scenarios += [(name, 200) for name in ("floating_drag", "floating_drag_burst", "tiled_drag", "tiled_drag_burst")]
        scenarios += [(name, 200) for name in ("tiled_relocation", "floating_relocation", "tiled_ratio")]
        scenarios += [("dock_startup", count) for count in (10, 40)]
        for scenario, operations in scenarios:
            result = measure(binary, library, scenario, operations)
            print(json.dumps(result), flush=True)
            if arguments.check:
                counts = result["counts"]
                if scenario == "tiled_ratio":
                    if not operations <= counts["geometry_configure"] <= operations * 10:
                        raise AssertionError("Inactive or excessive ratio layout: " + json.dumps(result))
                    # Each IPC command costs exactly the read of its request property.
                    if counts["query_tree"] or counts["get_property"] != operations or counts["get_geometry"]:
                        raise AssertionError("Layout-only change reconciled unrelated state: " + json.dumps(result))
                    if counts["visibility_barrier"] > operations:
                        raise AssertionError("Repeated crossing barriers: " + json.dumps(result))
                    continue
                if scenario == "dock_startup":
                    if not operations <= counts["get_property"] <= 12 * operations + 100:
                        raise AssertionError("Repeated dock reads or inactive tracer: " + json.dumps(result))
                    if counts["get_geometry"] != 1:
                        raise AssertionError("Dock refresh must share one root geometry read: " + json.dumps(result))
                    continue
                if "drag" in scenario:
                    if not 1 <= counts["geometry_configure"] <= operations + 20:
                        raise AssertionError("Inactive or excessive drag geometry: " + json.dumps(result))
                    if counts["visibility_barrier"] > 2 or counts["query_tree"] > 2:
                        raise AssertionError("Per-motion reconciliation: " + json.dumps(result))
                    continue
                if "relocation" in scenario:
                    if counts["get_geometry"]:
                        raise AssertionError("Relocation must use known geometry: " + json.dumps(result))
                    geometry_budget = operations * (10 if scenario.startswith("tiled_") else 1)
                    if not operations <= counts["geometry_configure"] <= geometry_budget:
                        raise AssertionError("Inactive or excessive relocation geometry: " + json.dumps(result))
                operations_flush_budget = result["operations"] * 20
                # A workspace switch is an IPC command, which reads its request property.
                reads = result["operations"] * (1 if scenario == "metadata" else 3 if scenario == "workspace" else 2)
                if not result["operations"] - 1 <= counts["get_property"] <= reads:
                    raise AssertionError("Unexpected property reads or inactive tracer: " + json.dumps(result))
                limit = 0 if scenario == "metadata" else result["operations"]
                if counts["visibility_barrier"] > limit or counts["query_tree"] > limit:
                    raise AssertionError("Repeated reconciliation: " + json.dumps(result))
                if scenario in ("metadata", "sticky") and counts["geometry_configure"]:
                    raise AssertionError("Unnecessary geometry writes: " + json.dumps(result))
                if scenario == "workspace" and counts["flush"] > operations_flush_budget:
                    raise AssertionError("Notification-driven flush overhead: " + json.dumps(result))


if __name__ == "__main__":
    main()
