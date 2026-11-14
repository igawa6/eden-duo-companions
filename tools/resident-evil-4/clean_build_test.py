#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""CI check: build committed RE4 sources from git archive, without generators/game files."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdk',type=Path,required=True)
    p.add_argument('--cache',type=Path)
    p.add_argument('--fmt',type=Path)
    p.add_argument('--json',type=Path)
    args=p.parse_args()
    extra=[]
    for value, name in ((args.cache,'EDEN_CPM_CACHE'),(args.fmt,'EDEN_FMT_INCLUDE_DIR'),(args.json,'EDEN_NLOHMANN_JSON_INCLUDE_DIR')):
        if value:extra.append('-D'+name+'='+str(value.resolve()))
    root=Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix='re4-clean-build-') as tmp:
        scratch=Path(tmp)
        archive=scratch/'source.tar'
        subprocess.run(['git','archive','HEAD','-o',str(archive)],cwd=root,check=True)
        tree=scratch/'source';tree.mkdir()
        subprocess.run(['tar','-xf',str(archive),'-C',str(tree)],check=True)
        assert not (tree/'native/modules/re4_map_geometry.h').exists()
        assert not (tree/'native/modules/re4_item_names.h').exists()
        subprocess.run(['cmake','-S',str(tree/'native/modules'),'-B',str(scratch/'build'),
            '-G','Ninja','-DCMAKE_BUILD_TYPE=Release','-DBUILD_TESTING=OFF',
            '-DEDEN_SOURCE_ROOT='+str(args.sdk.resolve()),*extra],check=True)
        subprocess.run(['cmake','--build',str(scratch/'build'),'--target',
            'dsmod-resident-evil-4','-j','2'],check=True)
    print('Clean archive builds with no generated headers or RomFS')


if __name__=='__main__':
    main()
