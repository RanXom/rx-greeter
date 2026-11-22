#!/usr/bin/env python3
"""Tiny greetd IPC mock for rx-greeter integration tests.

Wire format: native-endian uint32 payload length, then a UTF-8 JSON object.

Modes:
  success : correct password -> success
  error   : first response -> auth_error, then a retry can succeed
  visible : correct password -> visible "Verification code:" prompt,
            answer 123456 -> success
  hang    : never answer create_session; used to test Escape/cancel_session
"""

import argparse
import json
import os
import socket
import struct
import sys
import threading
import time


def send_message(conn, payload):
    data = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    conn.sendall(struct.pack("=I", len(data)) + data)


def recv_exact(conn, size):
    chunks = []
    remaining = size
    while remaining:
        chunk = conn.recv(remaining)
        if not chunk:
            return None
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def recv_message(conn):
    header = recv_exact(conn, 4)
    if header is None:
        return None
    (length,) = struct.unpack("=I", header)
    if length <= 0 or length > 1024 * 1024:
        raise ValueError("invalid length")
    payload = recv_exact(conn, length)
    if payload is None:
        return None
    return json.loads(payload.decode("utf-8"))


class Mock:
    def __init__(self, args):
        self.args = args
        self.lock = threading.Lock()

    def log(self, record):
        with self.lock:
            with open(self.args.log, "a", encoding="utf-8") as handle:
                handle.write(json.dumps(record) + "\n")
                handle.flush()

    def handle(self, conn, address):
        del address
        state = {
            "username": None,
            "stage": "new",
            "visible_done": False,
        }
        try:
            while True:
                request = recv_message(conn)
                if request is None:
                    break
                kind = request.get("type")
                if kind == "create_session":
                    state["username"] = request.get("username")
                    state["stage"] = "password"
                    self.log({"type": kind, "username": state["username"]})
                    if self.args.mode == "hang":
                        continue
                    time.sleep(self.args.delay_ms / 1000.0)
                    send_message(
                        conn,
                        {
                            "type": "auth_message",
                            "auth_message_type": "secret",
                            "auth_message": "Password:",
                        },
                    )
                elif kind == "post_auth_message_response":
                    response = request.get("response")
                    self.log(
                        {
                            "type": kind,
                            "response_len": len(response) if response else 0,
                        }
                    )
                    time.sleep(self.args.delay_ms / 1000.0)
                    if self.args.mode == "error" and state["stage"] == "password":
                        self.log({"event": "auth_error"})
                        send_message(
                            conn,
                            {
                                "type": "error",
                                "error_type": "auth_error",
                                "description": "Authentication failed",
                            },
                        )
                        break
                    if state["stage"] == "password":
                        if response != self.args.password:
                            self.log({"event": "wrong_password"})
                            send_message(
                                conn,
                                {
                                    "type": "error",
                                    "error_type": "auth_error",
                                    "description": "Authentication failed",
                                },
                            )
                            break
                        if self.args.mode == "visible" and not state["visible_done"]:
                            state["stage"] = "visible"
                            send_message(
                                conn,
                                {
                                    "type": "auth_message",
                                    "auth_message_type": "visible",
                                    "auth_message": "Verification code:",
                                },
                            )
                        else:
                            send_message(conn, {"type": "success"})
                    elif state["stage"] == "visible":
                        if response == "123456":
                            state["stage"] = "done"
                            send_message(conn, {"type": "success"})
                        else:
                            self.log({"event": "wrong_code"})
                            send_message(
                                conn,
                                {
                                    "type": "error",
                                    "error_type": "auth_error",
                                    "description": "Invalid verification code",
                                },
                            )
                            break
                    else:
                        send_message(conn, {"type": "success"})
                elif kind == "start_session":
                    self.log(
                        {
                            "type": kind,
                            "cmd": request.get("cmd"),
                            "env": request.get("env"),
                        }
                    )
                    send_message(conn, {"type": "success"})
                    # Give the greeter time to exit and greetd to start the
                    # session. The integration test checks the log.
                    time.sleep(0.2)
                    break
                elif kind == "cancel_session":
                    self.log({"type": kind})
                    send_message(conn, {"type": "success"})
                    break
                else:
                    self.log({"type": "unknown", "request": request})
                    send_message(
                        conn,
                        {
                            "type": "error",
                            "error_type": "error",
                            "description": "unknown request",
                        },
                    )
                    break
        except (ConnectionError, OSError, ValueError) as exc:
            self.log({"event": "connection_error", "error": str(exc)})
        finally:
            conn.close()

    def run(self):
        try:
            os.unlink(self.args.socket)
        except FileNotFoundError:
            pass
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(self.args.socket)
        os.chmod(self.args.socket, 0o600)
        listener.listen(16)
        self.log({"event": "listening", "socket": self.args.socket})
        try:
            while True:
                conn, address = listener.accept()
                thread = threading.Thread(
                    target=self.handle, args=(conn, address), daemon=True
                )
                thread.start()
        finally:
            listener.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", required=True)
    parser.add_argument("--password", default="correct horse")
    parser.add_argument("--mode", choices=["success", "error", "visible", "hang"],
                        default="success")
    parser.add_argument("--log", required=True)
    parser.add_argument("--delay-ms", type=int, default=0)
    args = parser.parse_args()
    try:
        Mock(args).run()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
