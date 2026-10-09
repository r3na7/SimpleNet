"""Cancel only a harness PID; retain ownership of its process group for failed tests."""
import argparse
import ctypes
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


def members(group):
    found = set()
    for path in Path('/proc').glob('[0-9]*/stat'):
        try:
            fields = path.read_bytes().rsplit(b')', 1)[1].split()
            if int(fields[2]) == group: # state, ppid, pgrp
                found.add(int(path.parent.name))
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            pass
    return found


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--server', required=True)
    parser.add_argument('--client', required=True)
    parser.add_argument('--harness', required=True)
    parser.add_argument('--signal', choices=('INT', 'TERM'), required=True)
    args = parser.parse_args()
    # Adopt orphans in this test process so even the expected RED leaves no zombies.
    libc = ctypes.CDLL(None, use_errno=True)
    assert libc.prctl(36, 1, 0, 0, 0) == 0 # Linux PR_SET_CHILD_SUBREAPER
    process = subprocess.Popen([sys.executable, '-B', args.harness, '--server', args.server,
                                '--client', args.client, '--case', 'slow_ipv4'],
                               start_new_session=True, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
    group = process.pid
    selected = signal.SIGINT if args.signal == 'INT' else signal.SIGTERM
    descendants = set()
    try:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            descendants = members(group) - {process.pid}
            if len(descendants) >= 2:
                break
            assert process.poll() is None, 'harness exited before creating server and client'
            time.sleep(0.005)
        assert len(descendants) >= 2, 'server/client not observed'
        process.send_signal(selected) # Crucially not killpg: harness must own cleanup.
        result = process.wait(timeout=12)
        assert result != 0, 'cancellation must remain a non-successful test'
        deadline = time.monotonic() + 1
        while members(group) - {process.pid} and time.monotonic() < deadline:
            time.sleep(0.005)
        remaining = members(group) - {process.pid}
        assert not remaining, 'cancelled harness left descendants: '+str(sorted(remaining))
    finally:
        if members(group):
            try:
                os.killpg(group, signal.SIGKILL)
            except ProcessLookupError:
                pass
        process.wait(timeout=5)
        # Reap only members of this owned process group, never unrelated processes.
        while True:
            try:
                os.waitpid(-group, 0)
            except ChildProcessError:
                break
    print('Harness cancellation '+args.signal+': PASS')


if __name__ == '__main__':
    main()
