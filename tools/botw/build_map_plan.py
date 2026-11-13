#!/usr/bin/env python3
"""Build the isolated 1.9.0 inventory + Ready HUD + quest + shortcuts + Map evidence plan."""
import argparse
import json
import tempfile
from pathlib import Path
from build_inventory_plan import build_plan


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    with tempfile.TemporaryDirectory() as work:
        plan=build_plan(Path(work),maps=True)
    with args.output.open('x') as output:
        output.write(json.dumps(plan,indent=2)+'\n')
    print(f"{args.output}: inventory + Ready + quest + shortcut + Map evidence only")


if __name__=='__main__':
    main()
