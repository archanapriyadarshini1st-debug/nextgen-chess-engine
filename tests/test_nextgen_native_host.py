#!/usr/bin/env python3
"""Smoke-test the Windows Chrome Native Messaging host with a ping request."""
from __future__ import annotations

import json
import struct
import subprocess
import sys


def read_exact(stream, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = stream.read(size - len(data))
        if not chunk:
            raise RuntimeError("Native host closed stdout before replying.")
        data.extend(chunk)
    return bytes(data)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_nextgen_native_host.py path/to/NextGenNativeHost.exe")
    host = sys.argv[1]
    process = subprocess.Popen([host], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    assert process.stdin is not None and process.stdout is not None
    payload = json.dumps({"type": "ping"}).encode("utf-8")
    process.stdin.write(struct.pack("<I", len(payload)) + payload)
    process.stdin.flush()
    header = read_exact(process.stdout, 4)
    (length,) = struct.unpack("<I", header)
    response = json.loads(read_exact(process.stdout, length).decode("utf-8"))
    process.stdin.close()
    process.wait(timeout=10)
    if response.get("ok") is not True or "connected" not in response.get("message", ""):
        raise SystemExit("Native host returned an unexpected ping response: " + repr(response))
    print("Native Messaging protocol ping passed.")


if __name__ == "__main__":
    main()
