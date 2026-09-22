"""Exercise real Unix process ownership, including a killed supervisor."""
import importlib.util
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

LAUNCHER = Path(__file__).resolve().parents[1] / 'demo/scripts/launch_pi.py'
spec = importlib.util.spec_from_file_location('launcher', LAUNCHER)
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)


def alive(pid):
    try:
        return Path(f'/proc/{pid}/stat').read_text().split()[2] != 'Z'
    except FileNotFoundError:
        return False


def wait_until(predicate):
    deadline = time.monotonic() + 3
    while not predicate():
        if time.monotonic() > deadline:
            raise AssertionError('process did not reach expected state')
        time.sleep(.02)


class CleanupTest(unittest.TestCase):
    def test_kills_child_after_group_leader_exits(self):
        with tempfile.TemporaryDirectory() as temp:
            marker = Path(temp) / 'pid'
            script = '''import os, signal, time
pid = os.fork()
if pid:
    time.sleep(.2)
    os._exit(0)
signal.signal(signal.SIGTERM, signal.SIG_IGN)
open(%r, 'w').write(str(os.getpid()))
time.sleep(60)
''' % str(marker)
            process = subprocess.Popen([sys.executable, '-c', script], start_new_session=True)
            child = None
            try:
                wait_until(marker.exists)
                child = int(marker.read_text())
                process.wait(timeout=2)
                launcher.stop_service(process, timeout=.1)
                wait_until(lambda: not alive(child))
            finally:
                if child and alive(child):
                    os.kill(child, signal.SIGKILL)

    def test_child_exits_if_supervisor_is_killed(self):
        with tempfile.TemporaryDirectory() as temp:
            marker = Path(temp) / 'pid'
            script = '''import importlib.util, subprocess, sys, time
spec = importlib.util.spec_from_file_location('launcher', %r)
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)
child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'],
    start_new_session=True, preexec_fn=launcher.child_setup)
open(%r, 'w').write(str(child.pid))
time.sleep(60)
''' % (str(LAUNCHER), str(marker))
            parent = subprocess.Popen([sys.executable, '-c', script])
            child = None
            try:
                wait_until(marker.exists)
                child = int(marker.read_text())
                parent.kill()
                parent.wait(timeout=2)
                wait_until(lambda: not alive(child))
            finally:
                if parent.poll() is None:
                    parent.kill()
                    parent.wait()
                if child and alive(child):
                    os.kill(child, signal.SIGKILL)


if __name__ == '__main__':
    unittest.main()
