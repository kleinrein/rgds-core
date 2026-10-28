#!/usr/bin/env python3
"""Sign an update feed with Ed25519 (tools/own_os_release.py builds the RG DS OS feed with it); uploads nothing."""
import base64
import json
from pathlib import Path
import subprocess
import tempfile

# macOS ships LibreSSL as `openssl`, which cannot sign Ed25519; prefer OpenSSL 3.
OPENSSL = next(
    (
        p
        for p in ('/opt/homebrew/opt/openssl@3/bin/openssl', '/usr/local/opt/openssl@3/bin/openssl') if Path(p).exists()
    ), 'openssl'
)


def sign(metadata, key, key_id, feed):
    """Write `feed`: the metadata as canonical JSON, Ed25519-signed with `key` (an envelope runtime/ota.py reads)."""
    signed = json.dumps(metadata, sort_keys=True, separators=(',', ':'), ensure_ascii=False).encode()
    with tempfile.TemporaryDirectory() as folder:
        folder = Path(folder)
        (folder / 'metadata').write_bytes(signed)
        subprocess.run(
            [
                OPENSSL, 'pkeyutl', '-sign', '-rawin', '-inkey',
                str(key), '-in',
                str(folder / 'metadata'), '-out',
                str(folder / 'signature')
            ],
            check=True,
            capture_output=True
        )
        signature = (folder / 'signature').read_bytes()
        public = subprocess.run(
            [OPENSSL, 'pkey', '-in', str(key), '-pubout', '-outform', 'DER'], check=True, capture_output=True
        ).stdout
    if len(signature) != 64 or public[:12] != bytes.fromhex('302a300506032b6570032100') or len(public) != 44:
        raise ValueError('Use an Ed25519 signing key.')
    envelope = dict(
        key_id=key_id, signed=base64.b64encode(signed).decode(), signature=base64.b64encode(signature).decode()
    )
    Path(feed).write_text(json.dumps(envelope, indent=2) + '\n')
    return public[-32:].hex()
