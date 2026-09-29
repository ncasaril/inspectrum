#!/usr/bin/env python3
"""End-to-end MCP tests against the actual Qt application, with synthetic IQ.
Copyright (C) 2026 inspectrum contributors; GPL-3.0-or-later.
"""
import json
import math
import os
import queue
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "ai"))
from inspectrum_mcp import Client

BINARY = sys.argv.pop(1)


class BridgeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="inspectrum-integration-")
        folder = Path(cls.temp.name)
        cls.endpoint = folder / "endpoint.json"
        capture = folder / "tone.cf32"
        with capture.open("wb") as f:
            for i in range(131072):
                f.write(struct.pack("ff", math.cos(2*math.pi*.125*i), math.sin(2*math.pi*.125*i)))
        env = os.environ.copy()
        env["QT_QPA_PLATFORM"] = "offscreen"
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        for var in ("XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME", "XDG_RUNTIME_DIR"):
            path = folder / var
            path.mkdir(mode=0o700)
            env[var] = str(path)
        cls.log = (folder / "application.log").open("w+")
        cls.app = subprocess.Popen([BINARY, "--ai-endpoint", str(cls.endpoint), "-r", "48000", str(capture)],
                                   env=env, stdout=cls.log, stderr=cls.log)
        for _ in range(200):
            if cls.endpoint.exists():
                break
            if cls.app.poll() is not None:
                cls.log.seek(0)
                raise RuntimeError(cls.log.read())
            time.sleep(.05)
        cls.client = Client(str(cls.endpoint))
        time.sleep(.3)  # initial Qt layout settles before revision assertions

    @classmethod
    def tearDownClass(cls):
        cls.client.close()
        cls.app.terminate()
        cls.app.wait(timeout=5)
        cls.log.close()
        cls.temp.cleanup()

    def state(self):
        return self.client.tool("get_state")["structuredContent"]

    def context(self):
        s = self.state()
        return {k: s[k] for k in ("capture_id", "revision")}

    def test_state_tools_and_image(self):
        specs = self.client.request("tools/list")["tools"]
        self.assertIn("propose_annotations", [s["name"] for s in specs])
        state = self.state()
        self.assertEqual(state["sample_count"], 131072)
        self.assertEqual(state["sample_rate"], 48000)
        image = self.client.tool("get_spectrogram", self.context())
        self.assertFalse(image["isError"], image)
        self.assertEqual(image["content"][1]["mimeType"], "image/png")
        self.assertTrue(image["content"][1]["data"].startswith("iVBOR"))

    def test_measure_and_energy(self):
        for analyzer in ("measure", "energy", "fsk"):
            args = dict(self.context(), analyzer=analyzer, sample_start=0, sample_count=65536)
            started = self.client.tool("start_analysis", args)
            self.assertFalse(started["isError"], started)
            job = started["structuredContent"]
            answer = self.client.tool("get_analysis", {"job_id": job["job_id"], "wait": True})["structuredContent"]
            self.assertEqual(answer["status"], "completed", answer)
            self.assertFalse(answer["stale"])
            if analyzer == "measure":
                self.assertAlmostEqual(answer["result"]["peak_offset_hz"], 6000, places=4)
                self.assertAlmostEqual(answer["result"]["mean_power_dbfs"], 0, places=5)
            else:
                self.assertIsInstance(answer["result"]["annotations"], list)

    def test_stale_and_preview_only(self):
        old = self.context()
        focus = self.client.tool("focus_region", dict(old, sample_start=8192, sample_count=4096))
        self.assertFalse(focus["isError"], focus)
        annotation = {"sample_start": 8192, "sample_count": 4096, "freq_low_hz": 5000,
                      "freq_high_hz": 7000, "label": "Tone", "evidence": "Measured peak 6000 Hz"}
        self.assertTrue(self.client.tool("propose_annotations", dict(old, annotations=[annotation]))["isError"])
        self.assertFalse(self.client.tool("propose_annotations", dict(self.context(), annotations=[annotation]))["isError"])
        self.assertTrue(self.client.tool("apply_annotations", self.context())["isError"])
        self.assertEqual(self.state()["annotation_count"], 0)
        annotation["sample_count"] = 100000000
        self.assertTrue(self.client.tool("propose_annotations", dict(self.context(), annotations=[annotation]))["isError"])

    def test_authentication(self):
        config = json.loads(self.endpoint.read_text())
        with socket.create_connection(("127.0.0.1", config["port"]), timeout=2) as sock:
            sock.sendall(b'{"token":"wrong"}\n')
            self.assertEqual(sock.recv(100), b"")
        if os.name != "nt":
            self.assertEqual(self.endpoint.stat().st_mode & 0o077, 0)

    def test_stdio_adapter(self):
        process = subprocess.Popen([sys.executable, str(ROOT/"tools/ai/inspectrum_mcp.py"), "--endpoint", str(self.endpoint)],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            process.stdin.write(json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params":
                {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "test", "version": "1"}}})+"\n")
            process.stdin.flush()
            messages = queue.Queue()
            threading.Thread(target=lambda: messages.put(process.stdout.readline()), daemon=True).start()
            answer = json.loads(messages.get(timeout=5))
            self.assertEqual(answer["result"]["serverInfo"]["name"], "inspectrum")
        finally:
            process.stdin.close()
            process.wait(timeout=5)
            self.assertEqual(process.returncode, 0, process.stderr.read())
            process.stdout.close()
            process.stderr.close()


if __name__ == "__main__":
    unittest.main()
