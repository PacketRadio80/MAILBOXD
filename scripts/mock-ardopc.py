#!/usr/bin/env python3
"""
Minimal ARDOPC TCP mock for MailboxD plugin host-TCP local tests.
Control port N, data port N+1. CRC-16 poly 0x8810 (ARDOP host family).
"""
from __future__ import annotations

import socket
import struct
import sys
import threading
import time

INT_POLY = 0x8810


def crc16(data: bytes) -> int:
    reg = 0xFFFF
    for byte in data:
        mask = 0x80
        for _ in range(8):
            bit = byte & mask
            mask >>= 1
            if reg & 0x8000:
                if bit:
                    reg = (1 + (reg << 1)) & 0xFFFF
                else:
                    reg = (reg << 1) & 0xFFFF
                reg ^= INT_POLY
            else:
                if bit:
                    reg = (1 + (reg << 1)) & 0xFFFF
                else:
                    reg = (reg << 1) & 0xFFFF
    return reg


def frame_cmd(text: str) -> bytes:
    body = (text + "\r").encode("ascii")
    c = crc16(body)
    return body + struct.pack(">H", c)


def parse_frames(buf: bytearray) -> list[bytes]:
    out: list[bytes] = []
    while True:
        if b"\r" not in buf:
            break
        cr = buf.index(0x0D)
        need = cr + 1 + 2
        if len(buf) < need:
            break
        frame = bytes(buf[:need])
        del buf[:need]
        if crc16(frame[:-2]) == struct.unpack(">H", frame[-2:])[0]:
            out.append(frame[:-2])
    return out


def serve_ctrl(port: int, log: list[str]) -> None:
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(1)
    srv.settimeout(8.0)
    try:
        conn, _ = srv.accept()
    except socket.timeout:
        srv.close()
        return
    conn.settimeout(2.0)
    conn.sendall(frame_cmd("VERSION crdopc_1.0.0"))
    buf = bytearray()
    try:
        while True:
            try:
                chunk = conn.recv(4096)
            except TimeoutError:
                break
            if not chunk:
                break
            buf.extend(chunk)
            for frame in parse_frames(buf):
                cmd = frame.decode("ascii", errors="replace").strip()
                log.append(f"ctrl<= {cmd}")
                if cmd.startswith("INITIALIZE"):
                    conn.sendall(frame_cmd("RDY"))
                elif cmd == "RDY":
                    pass
                else:
                    conn.sendall(frame_cmd("RDY"))
    finally:
        conn.close()
        srv.close()


def serve_data(port: int, log: list[str]) -> None:
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(1)
    srv.settimeout(8.0)
    try:
        conn, _ = srv.accept()
    except socket.timeout:
        srv.close()
        return
    conn.settimeout(1.0)
    try:
        while True:
            try:
                chunk = conn.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                break
            log.append(f"data<= {len(chunk)} bytes")
    finally:
        conn.close()
        srv.close()


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18515
    log: list[str] = []
    t1 = threading.Thread(target=serve_ctrl, args=(port, log), daemon=True)
    t2 = threading.Thread(target=serve_data, args=(port + 1, log), daemon=True)
    t1.start()
    t2.start()
    time.sleep(6)
    for line in log:
        print(line)
    return 0 if any("INITIALIZE" in x for x in log) else 1


if __name__ == "__main__":
    sys.exit(main())
