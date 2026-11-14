#!/usr/bin/env python3
"""Prepare the asset directory; font goldens are now built in memory by font_reference."""
import argparse
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('research',type=Path)
p.parse_args()
(Path(__file__).resolve().parents[2]/'packages/ResidentEvil4/dualscreen/assets').mkdir(parents=True,exist_ok=True)
print('Font atlas and metrics are generated from player RomFS at runtime')
