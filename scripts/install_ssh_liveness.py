#!/usr/bin/env python3
"""Run with sudo on the demo host; bound stale tunnels for the demo user."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--user', default='yuxw')
args = parser.parse_args()
if not args.user.replace('_', '').replace('-', '').isalnum():
    parser.error('invalid username')
path = Path('/etc/ssh/sshd_config.d/60-pdal-liveness.conf')
original = path.read_bytes() if path.exists() else None
path.write_text(
    '# Release pDAL reverse ports when a client disappears across 5G/NAT.\n'
    f'Match User {args.user}\n'
    '    ClientAliveInterval 10\n'
    '    ClientAliveCountMax 3\n'
    'Match all\n'
)
try:
    subprocess.run(['/usr/sbin/sshd', '-t'], check=True)
    output = subprocess.check_output([
        '/usr/sbin/sshd', '-T', '-C', f'user={args.user},host=localhost,addr=127.0.0.1'
    ], text=True)
    assert 'clientaliveinterval 10\n' in output
    assert 'clientalivecountmax 3\n' in output
    subprocess.run(['systemctl', 'reload', 'ssh'], check=True)
except BaseException:
    if original is None:
        path.unlink()
    else:
        path.write_bytes(original)
    raise
print(f'Installed {path}; applies to new {args.user} connections. Existing sessions remain open.')
