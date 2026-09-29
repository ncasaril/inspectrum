"""Small bounded debuggees: no large capture or desktop is involved."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


GUARD = Path(__file__).resolve().parents[1] / 'tools/gdb_memory_guard.py'


@unittest.skipUnless(shutil.which('c++'), 'C++ compiler required')
class MemoryTraceTests(unittest.TestCase):
    def test_request_logging_is_opt_in_and_filters_small_buffers(self):
        root = GUARD.parent.parent
        with tempfile.TemporaryDirectory() as directory:
            probe = str(Path(directory) / 'probe')
            subprocess.run(['c++', '-std=c++14', '-I', str(root / 'src'),
                            str(root / 'tests/memorytrace_probe.cpp'), '-o', probe], check=True)
            for value in ('0', '1'):
                result = subprocess.run([probe], env=dict(os.environ, INSPECTRUM_MEMORY_LOG=value),
                                        capture_output=True, text=True, check=True)
                if value == '0':
                    self.assertEqual(result.stderr, '')
                else:
                    self.assertIn('large source=', result.stderr)
                    self.assertIn('ProbeSource', result.stderr)
                    self.assertIn('start=123 count=262144 estimated_bytes=2097152', result.stderr)
                    self.assertNotIn('small source=', result.stderr)


@unittest.skipUnless(shutil.which('gdb') and Path('/sys/fs/cgroup/cgroup.controllers').exists(),
                     'GDB and Linux cgroup v2 required')
class MemoryGuardTests(unittest.TestCase):
    def run_guard(self, code, threshold):
        result = subprocess.run(
            ['gdb', '--nx', '--batch', '-x', str(GUARD), '--args', '/usr/bin/python3', '-c', code],
            env=dict(os.environ, INSPECTRUM_MEMORY_TRACE_MIB=str(threshold)),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertNotIn('Operation not permitted', result.stdout)
        self.assertNotIn('Traceback (most recent call last)', result.stdout)
        return result.stdout

    def test_threshold_stops_captures_and_kills(self):
        # An intentionally tiny threshold exercises capture without using GiBs.
        output = self.run_guard('import time; data=bytearray(8*1024*1024); time.sleep(5)', 1)
        self.assertIn('threshold reached', output)
        self.assertIn('CAPTURE BEGIN', output)
        self.assertIn('VmRSS:', output)
        self.assertIn('#0 ', output)
        self.assertIn('CAPTURE END; terminating debuggee', output)
        self.assertIn('killed', output)

    def test_normal_crash_captures(self):
        output = self.run_guard('import os,signal; os.kill(os.getpid(), signal.SIGSEGV)', 2**30)
        self.assertIn('SIGSEGV', output)
        self.assertIn('CAPTURE BEGIN: debugger stopped on signal', output)
        self.assertIn('#0 ', output)
        self.assertIn('CAPTURE END', output)

    def test_normal_exit(self):
        output = self.run_guard('pass', 2**30)
        self.assertIn('inferior exited; no live stack available', output)
        self.assertNotIn('CAPTURE BEGIN', output)

    def test_service_entry_refuses_uncontained_execution(self):
        result = subprocess.run(
            ['/usr/bin/python3', str(GUARD.with_name('debug_memory.py')),
             '--log', '/tmp/memory-guard-must-not-create.log', '/usr/bin/true', '/dev/null'],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('requires a separate system service', result.stdout)


if __name__ == '__main__':
    unittest.main()
