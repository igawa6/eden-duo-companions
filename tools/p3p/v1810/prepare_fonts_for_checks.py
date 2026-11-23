#!/usr/bin/env python3
"""Obtain and authenticate the exact public Inter fonts used for P3P UI art.

The font binaries are downloaded from rsms/inter v4.1 into a temporary local
directory. They are NOT committed, added to a mod, or uploaded as CI artifacts.
Source: https://github.com/rsms/inter/releases/tag/v4.1 (SIL OFL-1.1).
The two output OTF SHA-256s are pinned to the local original-design environment.
"""
from __future__ import annotations

import argparse
import hashlib
import io
from pathlib import Path
from urllib.request import Request, urlopen
from zipfile import ZipFile

URL = 'https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip'
FONTS = {
    'Inter-BlackItalic.otf':
        '54302c38cf020111ea06deaa137ba0c17b703d0f8acf0a5cd37f054c09f11c51',
    'InterDisplay-BoldItalic.otf':
        '095f2bd9b95275826bd361f80ce40b3619b4cb586c7bc4f64d5210cedb29fd89',
}


def prepare_fonts(output: Path, archive_path: Path | None = None) -> None:
    if archive_path:
        data = archive_path.read_bytes()
    else:
        with urlopen(Request(URL, headers={'User-Agent': 'P3P-source-art-verification/1.0'}),
                     timeout=120) as response:
            data = response.read()
    output.mkdir(parents=True, exist_ok=True)
    with ZipFile(io.BytesIO(data)) as archive:
        for name, expected in FONTS.items():
            matches = [key for key in archive.namelist()
                       if key.endswith('/otf/' + name)]
            if len(matches) != 1:
                raise ValueError(f'Missing or ambiguous Inter 4.1 OTF: {name}')
            original = archive.read(matches[0])
            actual = hashlib.sha256(original).hexdigest()
            if actual != expected:
                raise ValueError(f'Font bytes changed: {name}; SHA-256={actual}')
            (output / name).write_bytes(original)
    print(f'Pinned official Inter 4.1 fonts verified: {len(FONTS)} OTFs')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--archive', type=Path, help='offline original Inter-4.1.zip')
    args = parser.parse_args()
    prepare_fonts(args.output, args.archive)


if __name__ == '__main__':
    main()
