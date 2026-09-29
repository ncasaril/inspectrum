#!/usr/bin/env python3
"""Service entry point; refuses to run without the expected hard memory limits."""
import argparse
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log', required=True)
    parser.add_argument('binary')
    parser.add_argument('capture')
    args = parser.parse_args()
    relative = next(line[3:] for line in Path('/proc/self/cgroup').read_text().splitlines()
                    if line.startswith('0::'))
    group = Path('/sys/fs/cgroup') / relative.lstrip('/')
    if not relative.startswith('/system.slice/'):
        raise RuntimeError('Run with debug-memory.sh: requires a separate system service')
    maximum = (group / 'memory.max').read_text().strip()
    if maximum == 'max' or not 3*1024**3 < int(maximum) <= 4*1024**3:
        raise RuntimeError('Expected hard RAM limit >3 GiB and <=4 GiB')
    if int((group / 'memory.swap.max').read_text()) != 0:
        raise RuntimeError('Expected zero swap allowance')
    if int((group / 'memory.oom.group').read_text()) != 1:
        raise RuntimeError('Expected OOMPolicy=kill')
    env = dict(os.environ, INSPECTRUM_MEMORY_LOG='1', INSPECTRUM_MEMORY_TRACE_MIB='3072')
    guard = Path(__file__).resolve().with_name('gdb_memory_guard.py')
    # Exclusive/private log, survives process exit; includes inferior stderr too.
    fd = os.open(args.log, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    print(f'Diagnostic log: {args.log}', flush=True)
    with os.fdopen(fd, 'w') as output:
        output.write(f'binary={args.binary}\ncapture={args.capture}\ncgroup={group}\n')
        output.flush()
        result = subprocess.run(['/usr/bin/gdb', '--nx', '--batch', '-x', str(guard),
                                 '--args', args.binary, args.capture],
                                env=env, stdout=output, stderr=subprocess.STDOUT)
    print(f'Debug run finished (gdb status {result.returncode}); log: {args.log}', flush=True)
    return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
