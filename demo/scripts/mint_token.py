#!/usr/bin/env python3
"""Mint an HS256 bearer token accepted by pDAL's BearerTokenAuthenticator.

The token is a compact JWS (JWT) with the claims pDAL verifies: `iss`, `aud`,
`exp`, `sub`, and the identity claims `role` / `org`. Use it for local demos and
tests only; a production deployment issues tokens from a real identity provider.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import hmac
import json
import sys
import time
from pathlib import Path


def _b64url(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode("ascii")


def mint(
    secret: str,
    *,
    issuer: str,
    audience: str,
    subject: str,
    role: str,
    org: str = "",
    ttl_seconds: int = 900,
    now: int | None = None,
) -> str:
    """Return a signed HS256 token string."""
    if len(secret) < 16:
        raise ValueError("secret must be at least 16 bytes")
    issued = int(time.time()) if now is None else now
    header = {"alg": "HS256", "typ": "JWT"}
    payload: dict[str, object] = {
        "iss": issuer,
        "aud": audience,
        "sub": subject,
        "role": role,
        "iat": issued,
        "exp": issued + ttl_seconds,
    }
    if org:
        payload["org"] = org
    signing_input = (
        _b64url(json.dumps(header, separators=(",", ":")).encode())
        + "."
        + _b64url(json.dumps(payload, separators=(",", ":")).encode())
    )
    signature = hmac.new(
        secret.encode(), signing_input.encode(), hashlib.sha256
    ).digest()
    return signing_input + "." + _b64url(signature)


def _resolve_secret(args: argparse.Namespace) -> str:
    if args.secret is not None:
        return args.secret
    if args.secret_file is not None:
        return Path(args.secret_file).read_text(encoding="utf-8").strip()
    raise SystemExit("provide --secret or --secret-file")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--secret")
    parser.add_argument("--secret-file")
    parser.add_argument("--issuer", default="pdal-local-issuer")
    parser.add_argument("--audience", default="pdal")
    parser.add_argument("--subject", required=True)
    parser.add_argument("--role", required=True)
    parser.add_argument("--org", default="")
    parser.add_argument("--ttl", type=int, default=900)
    args = parser.parse_args()

    token = mint(
        _resolve_secret(args),
        issuer=args.issuer,
        audience=args.audience,
        subject=args.subject,
        role=args.role,
        org=args.org,
        ttl_seconds=args.ttl,
    )
    sys.stdout.write(token + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
