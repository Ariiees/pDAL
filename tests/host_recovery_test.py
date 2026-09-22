"""Host stays usable through Pi outages and repeated process restarts."""
import json
from pathlib import Path
import signal
import socket
import subprocess
import threading
import time
import unittest
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = Path(__file__).resolve().parents[1]


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class Upstream(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.server.stall:
            time.sleep(1)
        body = json.dumps({'status': 'ready', 'decode_location': 'host'}).encode()
        try:
            self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *_):
        pass


class RecoveryTest(unittest.TestCase):
    def test_offline_start_timeout_recovery_and_restart(self):
        host_port, pi_port = free_port(), free_port()
        base = f'http://127.0.0.1:{host_port}'
        command = ['python3', str(ROOT / 'host/server.py'), '--address', '127.0.0.1',
                   '--port', str(host_port), '--pi-url', f'http://127.0.0.1:{pi_port}',
                   '--control-timeout', '0.2']
        upstream = None
        for stop_signal in (signal.SIGINT, signal.SIGTERM, signal.SIGINT):
            process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            try:
                deadline = time.monotonic() + 5
                while True:
                    try:
                        with urllib.request.urlopen(base + '/health', timeout=.3) as response:
                            self.assertEqual(response.status, 200)
                        break
                    except OSError:
                        self.assertIsNone(process.poll())
                        if time.monotonic() > deadline:
                            raise
                        time.sleep(.05)
                with urllib.request.urlopen(base + '/') as response:
                    self.assertIn('no-store', response.headers['Cache-Control'])
                if upstream is None:
                    with self.assertRaises(urllib.error.HTTPError) as error:
                        urllib.request.urlopen(base + '/api/health')
                    self.assertEqual(error.exception.code, 502)
                    upstream = ThreadingHTTPServer(('127.0.0.1', pi_port), Upstream)
                    upstream.stall = True
                    threading.Thread(target=upstream.serve_forever, daemon=True).start()
                    start = time.monotonic()
                    with self.assertRaises(urllib.error.HTTPError) as error:
                        urllib.request.urlopen(base + '/api/health')
                    self.assertEqual(error.exception.code, 502)
                    self.assertLess(time.monotonic() - start, .8)
                    upstream.stall = False
                with urllib.request.urlopen(base + '/api/health') as response:
                    self.assertEqual(json.load(response)['status'], 'ready')
            finally:
                process.send_signal(stop_signal)
                _, error = process.communicate(timeout=3)
                self.assertEqual(process.returncode, 0, error.decode())
                self.assertNotIn(b'Traceback', error)
        upstream.shutdown()
        upstream.server_close()


if __name__ == '__main__':
    unittest.main()
