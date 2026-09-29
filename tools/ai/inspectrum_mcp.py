#!/usr/bin/env python3
"""Local stdio MCP adapter for a running inspectrum instance (Python stdlib only).

Copyright (C) 2026 inspectrum contributors; GPL-3.0-or-later.
"""
import argparse
import glob
import json
import os
import queue
import socket
import sys
import tempfile
import threading

MAX_LINE = 8 * 1024 * 1024


def connect(endpoint, timeout=30):
    if endpoint == "auto":
        candidates = []
        for path in glob.glob(os.path.join(tempfile.gettempdir(), "inspectrum-ai-*", "endpoint.json")):
            try:
                if hasattr(os, "getuid") and os.stat(path).st_uid != os.getuid():
                    continue
                probe, stream = connect(path, timeout=.3)
                probe.shutdown(socket.SHUT_RDWR)
                stream.close()
                probe.close()
                candidates.append(path)
            except (OSError, ValueError, KeyError):
                continue
        if len(candidates) != 1:
            raise ValueError("Expected one active inspectrum AI session; found %d. Open Tools > AI review, or use --endpoint with its explicit path." % len(candidates))
        endpoint = candidates[0]
    with open(endpoint, encoding="utf-8") as f:
        config = json.load(f)
    if config.get("host") != "127.0.0.1":
        raise ValueError("The inspectrum endpoint must be on loopback")
    sock = socket.create_connection(("127.0.0.1", int(config["port"])), timeout=timeout)
    stream = sock.makefile("rb")
    sock.sendall(json.dumps({"token": config["token"]}).encode() + b"\n")
    if json.loads(stream.readline(MAX_LINE)).get("authenticated") is not True:
        sock.close()
        raise ValueError("Inspectrum authentication failed")
    sock.settimeout(None)
    return sock, stream


class Client:
    """Synchronous MCP client for local automation and integration tests."""
    def __init__(self, endpoint):
        self.sock, self.stream = connect(endpoint)
        self.lock = threading.Lock()
        self.counter = 0
        self.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                     "clientInfo": {"name": "inspectrum-assistant", "version": "0.1.0"}})
        self.sock.sendall(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')

    def request(self, method, params=None):
        with self.lock:
            self.counter += 1
            request_id = self.counter
            self.sock.sendall(json.dumps({"jsonrpc": "2.0", "id": request_id,
                              "method": method, "params": params or {}}).encode() + b"\n")
            while True:
                line = self.stream.readline(MAX_LINE + 1)
                if not line or len(line) > MAX_LINE:
                    raise ConnectionError("Inspectrum disconnected or returned an oversized message")
                message = json.loads(line)
                if message.get("id") != request_id:
                    continue
                if "error" in message:
                    raise RuntimeError(message["error"].get("message", "MCP error"))
                return message["result"]

    def tool(self, name, arguments=None):
        return self.request("tools/call", {"name": name, "arguments": arguments or {}})

    def close(self):
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.stream.close()
        self.sock.close()


def read_stdin(put):
    """Raw descriptor reads avoid a daemon holding Python's stdin buffer at exit."""
    pending = b""
    while True:
        chunk = os.read(sys.stdin.fileno(), 65536)
        if not chunk:
            put("eof", None)
            return
        pending += chunk
        if len(pending) > 1024 * 1024:
            put("fatal", "Input message exceeds 1 MiB")
            return
        while b"\n" in pending:
            line, pending = pending.split(b"\n", 1)
            put("input", line.decode("utf-8"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", default="auto", help="Private endpoint.json, or auto when exactly one instance is running")
    args = parser.parse_args()
    sock, stream = connect(args.endpoint)

    events = queue.Queue(maxsize=128)
    def output():
        try:
            while True:
                line = stream.readline(MAX_LINE + 1)
                if not line or len(line) > MAX_LINE:
                    break
                events.put(("output", line))
        except (OSError, BrokenPipeError):
            pass
        finally:
            events.put(("eof", None))

    reader = threading.Thread(target=output, daemon=True)
    reader.start()
    threading.Thread(target=read_stdin, args=(lambda kind, data: events.put((kind, data)),), daemon=True).start()
    try:
        while True:
            kind, data = events.get()
            if kind == "eof": break
            if kind == "fatal": raise ValueError(data)
            if kind == "input": sock.sendall(data.encode("utf-8") + b"\n")
            if kind == "output":
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
    finally:
        try:
            sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        reader.join(timeout=2)
        stream.close()
        sock.close()


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError) as error:
        print("inspectrum MCP: " + str(error), file=sys.stderr)
        sys.exit(1)
