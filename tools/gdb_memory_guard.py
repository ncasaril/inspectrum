"""Loaded by GDB, not standalone. Capture BEFORE an uncatchable OOM SIGKILL."""
import os
from pathlib import Path
import signal
import threading
import time

import gdb


def cgroup_directory():
    for line in Path('/proc/self/cgroup').read_text().splitlines():
        if line.startswith('0::'):
            return Path('/sys/fs/cgroup') / line[3:].lstrip('/')
    raise RuntimeError('cgroup v2 is required')


threshold = int(os.environ.get('INSPECTRUM_MEMORY_TRACE_MIB', '3072')) * 1024**2
if threshold <= 0:
    raise ValueError('memory threshold must be positive')
group = cgroup_directory()
done = threading.Event()
trigger = []
monitor_started = False


def log(message):
    print('[memory-guard] ' + message, flush=True)


def monitor(pid):
    """No GDB API calls here: those must remain on the debugger thread."""
    last_report = 0
    try:
        while not done.wait(.05):
            status = Path(f'/proc/{pid}/status').read_text()
            rss = next(int(line.split()[1])*1024 for line in status.splitlines()
                       if line.startswith('VmRSS:'))
            charged = int((group / 'memory.current').read_text())
            now = time.monotonic()
            if now-last_report >= 1:
                log(f'monotonic={now:.3f} pid={pid} rss={rss} cgroup_bytes={charged}')
                last_report = now
            if max(rss, charged) >= threshold:
                trigger.append(f'threshold reached: rss={rss} cgroup_bytes={charged} limit={threshold}')
                log(trigger[0])
                os.kill(pid, signal.SIGSTOP)
                return
    except (OSError, ValueError, StopIteration) as error:
        if not done.is_set():
            log(f'monitor unavailable: {error}; stopping inferior fail-closed')
            trigger.append('monitor failure')
            try:
                os.kill(pid, signal.SIGSTOP)
            except ProcessLookupError:
                pass


def on_continue(event):
    global monitor_started
    if not monitor_started:
        pid = gdb.selected_inferior().pid
        if pid:
            monitor_started = True
            threading.Thread(target=monitor, args=(pid,), daemon=True).start()


gdb.execute('set pagination off')
gdb.execute('set confirm off')
gdb.execute('set debuginfod enabled off')
gdb.execute('handle SIGSTOP stop print nopass')
gdb.events.cont.connect(on_continue)
log(f'armed threshold_bytes={threshold} cgroup={group}')
try:
    gdb.execute('run')
    done.set()
    inferior = gdb.selected_inferior()
    if inferior.pid:
        log('CAPTURE BEGIN: ' + (trigger[0] if trigger else 'debugger stopped on signal'))
        for name in ('status', 'smaps_rollup'):
            try:
                log(f'/proc/{inferior.pid}/{name}\n' + Path(f'/proc/{inferior.pid}/{name}').read_text())
            except OSError as error:
                log(str(error))
        for command in ('info threads', 'thread apply all bt 24', 'info sharedlibrary'):
            try:
                gdb.execute(command)
            except gdb.error as error:
                log(str(error))
        log('CAPTURE END; terminating debuggee (no automatic restart)')
    else:
        log('inferior exited; no live stack available')
finally:
    done.set()
    gdb.events.cont.disconnect(on_continue)
    if gdb.selected_inferior().pid:
        gdb.execute('kill')
