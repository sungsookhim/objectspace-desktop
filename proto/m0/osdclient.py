#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
"""Test client for watchd, standing in for one user's osdd.

Connects to watchd's socket and reads commands on stdin:
  open NAME PATH   open PATH and keep the descriptor as NAME
  reg PATH         open PATH and register it
  regfd NAME       register a descriptor opened earlier with `open`
  regnofd          send a registration with no descriptor (must be refused)
  stall SECONDS    stop reading from watchd for a while (a slow service)
  quit

Prints one JSON object per line on stdout: command results, then every
line received from watchd, each with a timestamp "t" in milliseconds.
usage: osdclient.py SOCKET
"""
import json
import os
import socket
import sys
import threading
import time

lock = threading.Lock()
stall_until = [0.0]


def emit(obj):
    obj["t"] = int(time.time() * 1000)
    with lock:
        sys.stdout.write(json.dumps(obj) + "\n")
        sys.stdout.flush()


def reader(sock):
    while True:
        while time.time() < stall_until[0]:
            time.sleep(0.05)
        try:
            data = sock.recv(65536)
        except OSError:
            return
        if not data:
            emit({"closed": True})
            return
        for line in data.decode().splitlines():
            if line.strip():
                emit({"from": "watchd", **json.loads(line)})


def main():
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    sock.connect(sys.argv[1])
    threading.Thread(target=reader, args=(sock,), daemon=True).start()
    held = {}
    emit({"ready": True, "uid": os.getuid()})
    for line in sys.stdin:
        parts = line.split()
        if not parts:
            continue
        cmd = parts[0]
        try:
            if cmd == "open":
                held[parts[1]] = os.open(parts[2], os.O_RDONLY)
                emit({"cmd": line.strip(), "ok": True})
            elif cmd == "reg":
                fd = os.open(parts[1], os.O_RDONLY)
                socket.send_fds(sock, [b"REG"], [fd])
                os.close(fd)
                emit({"cmd": line.strip(), "sent": True})
            elif cmd == "regfd":
                socket.send_fds(sock, [b"REG"], [held[parts[1]]])
                emit({"cmd": line.strip(), "sent": True})
            elif cmd == "regnofd":
                sock.send(b"REG")
                emit({"cmd": line.strip(), "sent": True})
            elif cmd == "stall":
                stall_until[0] = time.time() + float(parts[1])
                emit({"cmd": line.strip(), "ok": True})
            elif cmd == "quit":
                break
        except OSError as ex:
            emit({"cmd": line.strip(), "ok": False, "error": ex.strerror})
    sock.close()


if __name__ == "__main__":
    main()
