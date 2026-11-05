#!/usr/bin/env python3
"""Exercise the export privacy gate without disclosing sample credentials."""
from pathlib import Path
import importlib.util
import tempfile

TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('check_privacy', TOOLS / 'check_privacy.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)

with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    sample = root / 'sample'
    examples = [
        ('private machine path', b'/home' + b'/developer/project'),
        ('private machine path', b'C:' + b'\\Users\\developer\\project'),
        ('private key material', b'-----BEGIN PRIVATE KEY-----\n' + b'A' * 60),
        ('credential assignment', b'password' + b'="sample credential"'),
        ('access token', b'ghp_' + b'A' * 36),
        ('Wi-Fi credential', b'CONFIG_WIFI_SSID' + b'="private network"'),
    ]
    for category, data in examples:
        sample.write_bytes(data)
        findings = audit.check_tree(root)
        assert f'sample: {category}' in findings, category
        assert all('sample credential' not in finding for finding in findings)
    sample.write_bytes(b'-----BEGIN PRIVATE KEY-----\x00')
    assert not audit.check_tree(root), 'TLS parser label is not a private key'
    sample.write_text('Public upstream contact: info@example.org\n')
    (root / 'build').mkdir()
    (root / 'build/cache').write_bytes(examples[0][1])
    assert not audit.check_tree(root), 'Generated build caches must be excluded'
    (root / 'link').symlink_to(sample)
    assert audit.check_tree(root) == ['link: symlink requires review']

print('PASS: privacy gate catches paths, keys, credentials and tokens; preserves public contacts')
