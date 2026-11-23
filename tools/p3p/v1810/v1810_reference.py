#!/usr/bin/env python3
"""Read and validate the *exact* v1.8.10 FINAL user-tested UI manifest.

Canonical source reference only. The 62 rendered custom UI assets and two
native .so modules are not embedded here and are not licensed by this file.

Usage:
    python v1810_reference.py --output /tmp/v1.8.10-manifest.json
    python v1810_reference.py --archive /path/to/1.8.10-FINAL.dsmod.zip

This does not modify the normal generated manifest or native reader; it is an
immutable design reference for a proper, independently reproducible source port.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import lzma
import zipfile
from pathlib import Path

REFERENCE_PATH = Path(__file__).with_name('canonical_manifest_v1.8.10.lzma.b85')
REFERENCE_SHA256 = 'd9079a3852e24a24c07608ccfcb32b0b4c9c1283476650204e2a72cb30c9e379'
ARCHIVE_SHA256 = '70faf618f1fd4777c78d5204688b5427e80eeb0e57c2d16ec30e9b02ed3e4513'
TITLE_ID = '0100DCD01525A000'


def manifest_bytes():
    compact = ''.join(REFERENCE_PATH.read_text(encoding='ascii').split())
    content = lzma.decompress(base64.b85decode(compact.encode('ascii')))
    actual_sha = hashlib.sha256(content).hexdigest()
    if actual_sha != REFERENCE_SHA256:
        raise ValueError(f'Canonical manifest mismatch: {actual_sha}')
    return content


def verify_manifest(data):
    if data['title_id'] != TITLE_ID:
        raise ValueError('Unexpected game title')
    if (data['canvas_w'], data['canvas_h']) != (1240, 1080):
        raise ValueError('Unexpected canvas geometry')
    assert len(data['pages']) == 12
    assert len(data['module_outputs']) == 2798
    assert len(data['actions']) == 61
    assert len(data.get('preload', [])) > 0
    assert len(data.get('derived', [])) > 0
    assert len(data.get('composites', [])) > 0
    return True


def verify_archive(path):
    path = Path(path)
    raw = path.read_bytes()
    actual = hashlib.sha256(raw).hexdigest()
    if actual != ARCHIVE_SHA256:
        raise ValueError(f'Not the audited v1.8.10 FINAL archive: {actual}')
    with zipfile.ZipFile(path) as zf:
        if zf.testzip():
            raise ValueError('ZIP CRC failure')
        if zf.read('dualscreen/manifest.json') != manifest_bytes():
            raise ValueError('Archive manifest is not the exact reference')
        data = json.loads(manifest_bytes())
        references = set()
        def walk(node):
            if isinstance(node, dict):
                for v in node.values(): walk(v)
            elif isinstance(node, list):
                for v in node: walk(v)
            elif isinstance(node, str) and node.startswith('file:'):
                references.add('dualscreen/' + node[5:])
        walk(data)
        missing = sorted(ref for ref in references if ref not in zf.namelist())
        if missing: raise ValueError(f'Missing referenced assets: {missing}')
        images = [x for x in zf.namelist() if x.endswith('.png')]
        if len(images) != 62: raise ValueError('Unexpected PNG inventory')
    return {'archive_sha256': actual, 'asset_refs': len(references), 'pngs': 62}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, help='write exact canonical JSON bytes')
    parser.add_argument('--archive', type=Path, help='verify the original FINAL ZIP')
    args = parser.parse_args()
    data = manifest_bytes()
    verify_manifest(json.loads(data))
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(data)
        print(f'Wrote canonical v1.8.10 manifest: {args.output}')
    if args.archive:
        print(json.dumps(verify_archive(args.archive), indent=2))
    if not args.output and not args.archive:
        print('Verified canonical v1.8.10 manifest:', REFERENCE_SHA256)

if __name__ == '__main__':
    main()
