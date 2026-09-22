"""Direct mode starts and cleans up services without an SSH key or SSH calls."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

LAUNCHER = Path(__file__).resolve().parents[1] / 'demo/scripts/launch_pi.py'
spec = importlib.util.spec_from_file_location('launcher', LAUNCHER)
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)


class DirectModeTest(unittest.TestCase):
    def test_direct_without_key_never_starts_ssh_and_cleans_up(self):
        real_sleep = time.sleep
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'demo/pi/config').mkdir(parents=True)
            (root / 'demo/scripts').mkdir()
            script = root / 'demo/scripts/start_pi.sh'
            marker = root / 'service.txt'
            script.write_text('#!/bin/sh\n'
                              'echo "$$ $DEMO_PI_ADDRESS $DEMO_PI_PORT" > service.txt\n'
                              'exec sleep 60\n')
            script.chmod(0o755)

            def interrupt(_):
                deadline = time.monotonic() + 3
                while not marker.exists():
                    if time.monotonic() > deadline:
                        raise AssertionError('service did not start')
                    real_sleep(.02)
                raise KeyboardInterrupt

            output = io.StringIO()
            with patch.object(launcher, 'ROOT', root), \
                 patch('sys.argv', ['pDAL.sh', '--direct', '--key', str(root / 'missing-key')]), \
                 patch.object(launcher, 'health', return_value=True), \
                 patch.object(launcher.signal, 'signal'), \
                 patch.object(launcher.socket, 'socket') as socket, \
                 patch.object(launcher.subprocess, 'run', side_effect=AssertionError('SSH was invoked')), \
                 patch.object(launcher, 'time', SimpleNamespace(monotonic=time.monotonic, sleep=interrupt)), \
                 contextlib.redirect_stdout(output):
                socket.return_value.__enter__.return_value.connect_ex.return_value = 1
                self.assertEqual(launcher.main(), 0)
            pid, address, port = marker.read_text().split()
            self.assertEqual((address, port), ('0.0.0.0', '8090'))
            with self.assertRaises(ProcessLookupError):
                os.kill(int(pid), 0)
            self.assertIn('READY: direct access', output.getvalue())
            self.assertFalse((root / 'demo/pi/config/ssh-last-error.log').exists())


if __name__ == '__main__':
    unittest.main()
