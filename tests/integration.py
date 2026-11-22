#!/usr/bin/env python3
"""Nested headless Sway + mock greetd integration test for rx-greeter.

This drives real Wayland input through the wlroots virtual-keyboard and
virtual-pointer protocols, captures grim screenshots, and verifies the greetd
conversation (create_session -> secret -> success -> start_session).
"""

import argparse
import getpass
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


class Failure(Exception):
    pass


def log(message):
    print(f"[integration] {message}", flush=True)


def wait_until(predicate, description, timeout=8.0, interval=0.05):
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


def rect_center(rect):
    return int(rect["x"] + rect["w"] / 2), int(rect["y"] + rect["h"] / 2)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--power", required=True)
    parser.add_argument("--wl-input", required=True)
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()

    repo = Path(__file__).resolve().parent.parent
    artifacts = repo / "build" / "test-artifacts" / time.strftime("%Y%m%d-%H%M%S")
    artifacts.mkdir(parents=True, exist_ok=True)
    log(f"artifacts: {artifacts}")

    theme_path = artifacts / "theme.conf"
    base_theme = (repo / "configs" / "theme.conf").read_text(encoding="utf-8")
    base_theme += (
        f"\npower_helper = {args.power}\n"
        "power_dry_run = true\n"
        "show_cursor = true\n"
        "fade_ms = 0\n"
    )
    theme_path.write_text(base_theme, encoding="utf-8")

    runtime = Path(tempfile.mkdtemp(prefix="rx-greeter-it-", dir="/tmp"))
    os.chmod(runtime, 0o700)
    status_path = artifacts / "status.json"
    sway_log = artifacts / "sway.log"
    mock_log = artifacts / "mock-greetd.jsonl"
    greetd_sock = runtime / "greetd.sock"
    sway_sock = runtime / "sway-ipc.sock"
    launcher = runtime / "launch.sh"

    launcher.write_text(
        "#!/bin/sh\n"
        f'"{args.binary}" --theme "{theme_path}" '
        f'--debug-status "{status_path}" --power-helper "{args.power}" '
        "--power-dry-run\n"
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
        "default_floating_border none\n"
        "hide_edge_borders both\n"
        "focus_follows_mouse no\n"
        'for_window [app_id="rx-greeter"] fullscreen enable\n'
        'for_window [app_id="rx-greeter"] focus\n'
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
            "WLR_HEADLESS_OUTPUTS": "1",
            "WLR_LOG": "error",
            "SWAYSOCK": str(sway_sock),
            "GREETD_SOCK": str(greetd_sock),
        }
    )
    env.pop("WAYLAND_DISPLAY", None)

    mock = subprocess.Popen(
        [
            sys.executable,
            str(repo / "tests" / "mock-greetd.py"),
            "--socket",
            str(greetd_sock),
            "--password",
            "correct horse",
            "--mode",
            "visible",
            "--log",
            str(mock_log),
        ],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    wait_until(lambda: greetd_sock.exists(), "mock greetd socket")

    sway_out = open(sway_log, "wb")
    sway = subprocess.Popen(
        ["sway", "-c", str(sway_conf)],
        env=env,
        stdout=sway_out,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )

    display_name = None
    input_proc = None
    try:
        wait_until(lambda: sway_sock.exists(), "sway IPC socket")
        def find_display():
            names = sorted(p.name for p in runtime.glob("wayland-*"))
            return names[0] if names else None

        display_name = wait_until(find_display, "Wayland display")

        def status():
            return read_json(status_path)

        wait_until(lambda: status_path.exists() and status().get("width") == 1280,
                   "greeter first frame")

        def start_input():
            process = subprocess.Popen(
                [
                    args.wl_input,
                    "--display",
                    display_name,
                    "--size",
                    "1280x720",
                    "--stdin",
                ],
                env=env,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                bufsize=1,
            )
            return process

        input_proc = start_input()
        time.sleep(0.5)

        def input_cmd(command):
            process = input_proc
            if process is None or process.poll() is not None:
                raise Failure("wl-input process exited")
            process.stdin.write(command + "\n")
            process.stdin.flush()
            line = process.stdout.readline()
            if line.strip() != "done":
                error = process.stderr.read() if process.stderr else ""
                raise Failure(f"wl-input command failed: {command!r} {error!r}")

        def screenshot(name):
            target = artifacts / name
            result = subprocess.run(
                ["grim", "-o", "HEADLESS-1", str(target)],
                env={**env, "WAYLAND_DISPLAY": display_name},
                capture_output=True,
                text=True,
                timeout=10,
            )
            if result.returncode != 0:
                raise Failure(f"grim failed: {result.stderr.strip()}")
            log(f"screenshot: {target}")
            return target

        def wait_log(predicate, description, timeout=8.0):
            def check():
                if not mock_log.exists():
                    return None
                for line in mock_log.read_text(encoding="utf-8").splitlines():
                    try:
                        record = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if predicate(record):
                        return record
                return None

            return wait_until(check, description, timeout=timeout)

        # --- initial state -------------------------------------------------
        state = status()
        current_user = getpass.getuser()
        if state["phase"] != "idle" or state["user"] != current_user:
            raise Failure(f"unexpected initial state: {state}")
        if state["session"] != "Sway":
            raise Failure(f"expected the local Sway session, got {state['session']}")
        if not state["background_loaded"]:
            raise Failure("wallpaper did not load")
        expected_exec = state["session_exec"]
        if not expected_exec:
            raise Failure("session has no Exec argv")
        screenshot("01-initial.png")

        # --- password editing ---------------------------------------------
        input_cmd("text abc")
        wait_until(lambda: status()["password_len"] == 3, "three password bytes")
        input_cmd("key BackSpace")
        wait_until(lambda: status()["password_len"] == 2, "backspace")
        screenshot("02-password-dots.png")

        # --- Escape clears the field, Tab moves focus ---------------------
        input_cmd("key Escape")
        wait_until(lambda: status()["password_len"] == 0, "Escape clear")
        input_cmd("key Tab")
        wait_until(lambda: status()["focus"] == "user", "Tab to user selector")
        input_cmd("key ISO_Left_Tab")
        wait_until(lambda: status()["focus"] == "password", "Shift+Tab back")

        # --- Caps Lock indicator ------------------------------------------
        input_cmd("key Caps_Lock")
        wait_until(lambda: status()["caps_lock"] is True, "Caps Lock on")
        screenshot("03-capslock.png")
        input_cmd("caps-lock off")
        wait_until(lambda: status()["caps_lock"] is False, "Caps Lock off")

        # --- user selector -------------------------------------------------
        input_cmd("key Up")
        wait_until(lambda: status()["focus"] == "user", "user selector focus")
        wait_until(lambda: status()["user_row"]["h"] > 30, "user selector expanded")
        screenshot("04-user-selector.png")
        user_row = status()["user_row"]
        input_cmd(
            f"click {int(user_row['x'] + user_row['w'] / 2)} "
            f"{int(user_row['y'] + user_row['h'] / 2)}"
        )
        wait_until(lambda: status()["focus"] == "password", "click user row returns")

        # --- session selector ---------------------------------------------
        input_cmd("key Down")
        wait_until(lambda: status()["focus"] == "session", "session selector focus")
        wait_until(lambda: status()["below_row"]["h"] > 30, "session selector expanded")
        screenshot("05-session-selector.png")
        before = status()["session_index"]
        below_right = status()["below_right"]
        input_cmd(
            f"click {int(below_right['x'] + below_right['w'] / 2)} "
            f"{int(below_right['y'] + below_right['h'] / 2)}"
        )
        wait_until(lambda: status()["session_index"] != before,
                   "click session right arrow")
        input_cmd("key Left")
        wait_until(lambda: status()["session_index"] == 0, "session back to Sway")

        # --- power selector and dry-run keys ------------------------------
        input_cmd("key Down")
        wait_until(lambda: status()["focus"] == "power", "power selector focus")
        wait_until(lambda: status()["below_row"]["h"] > 30, "power selector expanded")
        input_cmd("key Right")
        wait_until(lambda: status()["power_index"] == 1, "power selector right")
        screenshot("06-power-selector.png")

        input_cmd("key F10")
        wait_until(lambda: "suspend" in status()["power_status"], "F10 dry-run")
        screenshot("07-power-dryrun.png")
        input_cmd("key F11")
        wait_until(lambda: "poweroff" in status()["power_status"], "F11 dry-run")
        input_cmd("key F12")
        wait_until(lambda: "reboot" in status()["power_status"], "F12 dry-run")

        # Click outside closes the selector.
        input_cmd("click 50 50")
        wait_until(lambda: status()["focus"] == "password", "click outside")

        # --- authentication error, clear, retry ---------------------------
        input_cmd("text wrong password")
        input_cmd("key Return")
        wait_until(lambda: status()["error_active"] is True, "auth error feedback")
        state = status()
        if state["password_len"] != 0:
            raise Failure("password was not cleared on authentication error")
        if "Authentication" not in state["message"]:
            raise Failure(f"unexpected auth error message: {state['message']}")
        screenshot("08-auth-error.png")
        wait_until(lambda: status()["error_active"] is False, "error border reset")

        # --- visible prompt, success, and start_session -------------------
        input_cmd("text correct horse")
        input_cmd("key Return")
        wait_until(lambda: status()["phase"] == "input", "visible auth prompt")
        wait_until(lambda: status()["last_auth_type"] == "visible",
                   "visible message type")
        screenshot("09-visible-prompt.png")
        input_cmd("text 123456")
        input_cmd("key Return")

        start = wait_log(
            lambda record: record.get("type") == "start_session",
            "start_session request",
            timeout=10.0,
        )
        if start.get("cmd") != expected_exec:
            raise Failure(
                f"wrong start_session cmd: {start.get('cmd')} != {expected_exec}"
            )
        env_list = start.get("env") or []
        if "XDG_SESSION_TYPE=wayland" not in env_list:
            raise Failure(f"missing session environment: {env_list}")
        wait_until(lambda: status().get("phase") == "finished",
                   "greeter finished", timeout=8.0)
        sway.wait(timeout=8)
        log("session start accepted; greeter and nested Sway exited")

        log("all integration checks passed")
        return 0
    except Failure as exc:
        print(f"[integration] FAIL: {exc}", file=sys.stderr)
        if status_path.exists():
            print(json.dumps(read_json(status_path), indent=2), file=sys.stderr)
        if mock_log.exists():
            print(mock_log.read_text(encoding="utf-8"), file=sys.stderr)
        return 1
    finally:
        processes = [process for process in (sway, mock, input_proc) if process]
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
