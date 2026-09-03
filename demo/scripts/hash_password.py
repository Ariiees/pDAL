#!/usr/bin/env python3
"""Print a PBKDF2-HMAC-SHA256 hash entry for demo/pi/config/roles.auth.json.

Usage:
  demo/scripts/hash_password.py --role incident_investigator --password 's3cret'
  # then paste the printed "role": "pbkdf2_sha256$..." line into roles.auth.json
"""

from __future__ import annotations

import argparse
import base64
import getpass
import hashlib
import os
import sys


def _b64url(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode("ascii")


def hash_password(password: str, *, iterations: int = 200_000) -> str:
    salt = os.urandom(16)
    derived = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, iterations)
    return f"pbkdf2_sha256${iterations}${_b64url(salt)}${_b64url(derived)}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--role", required=True)
    parser.add_argument("--password", help="prompted if omitted")
    parser.add_argument("--iterations", type=int, default=200_000)
    args = parser.parse_args()

    password = args.password or getpass.getpass("password: ")
    if not password:
        print("password must not be empty", file=sys.stderr)
        return 1
    entry = hash_password(password, iterations=args.iterations)
    print(f'  "{args.role}": "{entry}"')
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
