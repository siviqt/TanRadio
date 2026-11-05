#!/usr/bin/env python3
"""Reject private machine paths and credential material in distributable files.

Does not read Git history or generated build caches. Reports locations and
categories only, never matched contents. Public dependency credits, licenses,
and certificate-authority contacts are not personal development data.
"""
from pathlib import Path
import re
import sys

EXCLUDED = {'.git', 'build', 'managed_components', 'dist', '__pycache__',
            '.agents', '.aws', '.codex', '.vscode', '.idea'}
PATTERNS = {
    'private machine path': re.compile(rb'(?:/home/|/Users/|[A-Z]:\\Users\\)[A-Za-z0-9_.-]+[/\\]'),
    'private key material': re.compile(
        rb'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----\r?\n[A-Za-z0-9+/]{40,}'),
    'credential assignment': re.compile(
        rb'''(?im)(?:password|passwd|psk|api_key|access_token|secret_key)\s*[=:]\s*["'][^"'\r\n]+["']'''),
    'access token': re.compile(rb'(?:gh[pousr]_[A-Za-z0-9]{36,}|github_pat_[A-Za-z0-9_]{50,}|AKIA[A-Z0-9]{16})'),
    'Wi-Fi credential': re.compile(
        rb'(?m)^CONFIG_(?:EXAMPLE_)?(?:WIFI_SSID|WIFI_PASSWORD|WIFI_STA_SSID|WIFI_STA_PASSWORD)="[^"\r\n]+"'),
}


def check_tree(root):
    root = Path(root)
    findings = []
    for path in sorted(root.rglob('*')):
        relative = path.relative_to(root)
        if any(part in EXCLUDED for part in relative.parts):
            continue
        if path.is_symlink():
            findings.append(f'{relative}: symlink requires review')
            continue
        if not path.is_file():
            continue
        data = path.read_bytes()
        for category, pattern in PATTERNS.items():
            if pattern.search(data):
                findings.append(f'{relative}: {category}')
    return findings


if __name__ == '__main__':
    findings = check_tree(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parents[1])
    if findings:
        print('\n'.join(findings), file=sys.stderr)
        raise SystemExit(1)
    print('PASS: no private paths or credential material found in distributable files')
