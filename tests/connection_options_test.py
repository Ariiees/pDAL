"""The default tunnel remains unchanged; explicit Ethernet options override it."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('viewer', ROOT / 'host/server.py')
viewer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(viewer)


class ConnectionOptionsTest(unittest.TestCase):
    def test_no_options_uses_current_tunnel(self):
        with patch.dict(os.environ, {}, clear=True):
            self.assertEqual(viewer.parse_args([]).pi_url, 'http://127.0.0.1:18090')

    def test_environment_default_and_explicit_overrides(self):
        with patch.dict(os.environ, {'PI_URL': 'http://127.0.0.1:28090'}):
            self.assertEqual(viewer.parse_args([]).pi_url, 'http://127.0.0.1:28090')
            self.assertEqual(viewer.parse_args(['--pi-ip', '192.168.50.36']).pi_url,
                             'http://192.168.50.36:8090')
            self.assertEqual(viewer.parse_args(['--pi-url', 'http://example:9000']).pi_url,
                             'http://example:9000')

    def test_port_and_ipv6(self):
        self.assertEqual(viewer.parse_args(['--pi-ip=192.168.50.36', '--pi-port=9000']).pi_url,
                         'http://192.168.50.36:9000')
        self.assertEqual(viewer.parse_args(['--pi-ip', '::1']).pi_url, 'http://[::1]:8090')

    def test_invalid_or_ambiguous_options_are_rejected(self):
        for args in [
            ['--pi-ip', 'bad-ip'], ['--pi-ip'], ['--pi-port', '8090'],
            ['--pi-ip', '192.168.50.36', '--pi-port', '0'],
            ['--pi-ip', '192.168.50.36', '--pi-port', '65536'],
            ['--pi-ip', '192.168.50.36', '--pi-url', 'http://localhost'],
        ]:
            with self.subTest(args=args), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    viewer.parse_args(args)
                self.assertEqual(error.exception.code, 2)

    def test_shell_launcher_exposes_options_from_any_directory(self):
        result = subprocess.run([str(ROOT / 'scripts/start_host.sh'), '--help'],
                                cwd='/tmp', text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('--pi-ip', result.stdout)
        self.assertIn('--pi-url', result.stdout)


if __name__ == '__main__':
    unittest.main()
