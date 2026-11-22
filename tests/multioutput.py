#!/usr/bin/env python3
"""Nested headless test for multi-output layer surfaces and fractional scale."""

import argparse
import getpass
import json
import os
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path


class Failure(Exception):
    pass


def log(message):
    print(f"[multioutput] {message}", flush=True)


def wait_until(predicate, description, timeout=10.0, interval=0.05):
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        try:
            value = predicate()
            if value:
                return value
        except (OSError, ValueError, KeyError) as exc:
            last_error = exc
        time.sleep(interval)
    if last_error:
        raise Failure(f"timed out waiting for {description}: {last_error}")
    raise Failure(f"timed out waiting for {description}")


def read_json(path):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def write_test_png(path, width=16, height=16):
    def chunk(kind, data):
        body = kind + data
        return (
            struct.pack(">I", len(data))
            + body
            + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
        )

    rows = []
    for y in range(height):
        row = bytearray([0])
        for x in range(width):
            row.extend((40 + x * 8, 70 + y * 8, 90, 255))
        rows.append(bytes(row))
    raw = b"".join(rows)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--power", required=True)
    parser.add_argument("--wl-input", required=True)
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()

    repo = Path(__file__).resolve().parent.parent
    artifacts = repo / "build" / "test-artifacts" / (
        "multi-" + time.strftime("%Y%m%d-%H%M%S")
    )
    artifacts.mkdir(parents=True, exist_ok=True)
    log(f"artifacts: {artifacts}")

    runtime = Path(tempfile.mkdtemp(prefix="rx-greeter-multi-", dir="/tmp"))
    os.chmod(runtime, 0o700)
    status_path = artifacts / "status.json"
    sway_log = artifacts / "sway.log"
    sway_sock = runtime / "sway-ipc.sock"
    launcher = runtime / "launch.sh"

    sessions = artifacts / "wayland-sessions"
    sessions.mkdir()
    (sessions / "test.desktop").write_text(
        "[Desktop Entry]\nName=Test Session\nExec=/bin/true\nType=Application\n",
        encoding="utf-8",
    )
    wallpaper = artifacts / "wallpaper.png"
    write_test_png(wallpaper)
    theme = artifacts / "theme.conf"
    theme.write_text(
        "background_color = #121415\n"
        "surface_color = #1e2021\n"
        "surface_opacity = 0.8\n"
        "text_color = #e2e2e3\n"
        "primary_color = #a2cdde\n"
        "error_color = #ffb4ab\n"
        "outline_color = #8b9295\n"
        "font_family = monospace\n"
        "font_size = 16\n"
        "fade_ms = 0\n"
        "animation_ms = 0\n"
        "show_cursor = true\n",
        encoding="utf-8",
    )
    config = artifacts / "config.conf"
    config.write_text(
        f"theme_path = {theme}\n"
        f"background_image = {wallpaper}\n"
        f"default_user = {getpass.getuser()}\n"
        f"default_session = Test Session\n"
        f"session_dirs = {sessions}\n"
        f"power_helper = {args.power}\n"
        "power_dry_run = true\n"
        "output = HEADLESS-2\n",
        encoding="utf-8",
    )
    launcher.write_text(
        "#!/bin/sh\n"
        f'"{args.binary}" --config "{config}" --debug-status "{status_path}"\n'
        "status=$?\n"
        "swaymsg -t command exit >/dev/null 2>&1 || true\n"
        "exit $status\n",
        encoding="utf-8",
    )
    os.chmod(launcher, 0o700)

    sway_conf = runtime / "sway.conf"
    sway_conf.write_text(
        "xwayland disable\n"
        "default_border none\n"
        "output HEADLESS-1 scale 1.4\n"
        "output HEADLESS-2 scale 1.4\n"
        f"exec {launcher}\n",
        encoding="utf-8",
    )

    env = os.environ.copy()
    env.update(
        {
            "XDG_RUNTIME_DIR": str(runtime),
            "WLR_BACKENDS": "headless",
            "WLR_RENDERER": "pixman",
            "WLR_LIBINPUT_NO_DEVICES": "1",
            "WLR_HEADLESS_OUTPUTS": "2",
            "WLR_LOG": "error",
            "SWAYSOCK": str(sway_sock),
        }
    )
    env.pop("WAYLAND_DISPLAY", None)

    sway_out = open(sway_log, "wb")
    sway = subprocess.Popen(
        ["sway", "-c", str(sway_conf)],
        env=env,
        stdout=sway_out,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    input_proc = None
    try:
        wait_until(lambda: sway_sock.exists(), "sway IPC socket")

        def find_display():
            names = sorted(p.name for p in runtime.glob("wayland-*"))
            return names[0] if names else None

        display = wait_until(find_display, "Wayland display")

        def status():
            return read_json(status_path)

        wait_until(
            lambda: status_path.exists()
            and status().get("outputs") == 2
            and status().get("primary_output") == "HEADLESS-2"
            and status().get("output_scale_120", 0) >= 150
            and status().get("output_width", 0) > 0,
            "two fractional-scale outputs",
        )
        state = status()
        if state["output_scale_120"] not in (168, 169, 170):
            raise Failure(f"unexpected fractional scale: {state}")
        if state["output_width"] >= 1280:
            raise Failure(f"output width was not scaled logically: {state}")

        def grim(output, name):
            target = artifacts / name
            result = subprocess.run(
                ["grim", "-o", output, str(target)],
                env={**env, "WAYLAND_DISPLAY": display},
                capture_output=True,
                text=True,
                timeout=10,
            )
            if result.returncode != 0:
                raise Failure(f"grim {output} failed: {result.stderr.strip()}")
            log(f"screenshot: {target}")
            return target

        grim("HEADLESS-1", "output-1.png")
        grim("HEADLESS-2", "output-2.png")

        outputs = json.loads(
            subprocess.check_output(
                ["swaymsg", "-t", "get_outputs"], env=env, text=True
            )
        )
        rects = {item["name"]: item["rect"] for item in outputs}
        if "HEADLESS-1" not in rects or "HEADLESS-2" not in rects:
            raise Failure("Sway did not report both test outputs")
        total_w = max(rect["x"] + rect["width"] for rect in rects.values())
        total_h = max(rect["y"] + rect["height"] for rect in rects.values())

        input_proc = subprocess.Popen(
            [
                args.wl_input,
                "--display",
                display,
                "--size",
                f"{total_w}x{total_h}",
                "--stdin",
            ],
            env=env,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        time.sleep(0.5)

        def input_cmd(command):
            if input_proc.poll() is not None:
                raise Failure("wl-input exited")
            input_proc.stdin.write(command + "\n")
            input_proc.stdin.flush()
            line = input_proc.stdout.readline()
            if line.strip() != "done":
                raise Failure(f"wl-input failed: {command}")

        input_cmd("text abc")
        wait_until(lambda: status()["password_len"] == 3,
                   "keyboard input on primary output")

        # Click the user row on the non-primary output. This verifies that
        # pointer hit-testing uses the surface's logical dimensions.
        input_cmd("key Up")
        wait_until(lambda: status()["focus"] == "user", "user selector focus")
        wait_until(lambda: status()["user_row"]["h"] > 30,
                   "user selector expanded")
        row = status()["user_row"]
        rect = rects["HEADLESS-1"]
        click_x = int(rect["x"] + row["x"] + row["w"] / 2)
        click_y = int(rect["y"] + row["y"] + row["h"] / 2)
        input_cmd(f"click {click_x} {click_y}")
        wait_until(lambda: status()["focus"] == "password",
                   "click on secondary output")
        input_cmd("text d")
        wait_until(lambda: status()["password_len"] == 4,
                   "keyboard input after clicking secondary output")

        log("all multi-output and fractional-scale checks passed")
        return 0
    except Failure as exc:
        print(f"[multioutput] FAIL: {exc}", file=sys.stderr)
        if status_path.exists():
            print(json.dumps(read_json(status_path), indent=2), file=sys.stderr)
        return 1
    finally:
        processes = [process for process in (sway, input_proc) if process]
        for process in processes:
            if process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
        for process in processes:
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        sway_out.close()
        if not args.keep:
            shutil.rmtree(runtime, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
