#!/usr/bin/env python3
"""One-time conversion of the tested P3P redesign to readable source and package.

Reads the historical, SHA-pinned source reference already present on the fork,
writes ordinary UTF-8 JSON as the SINGLE editable UI definition, and replaces
the obsolete 1.0-only manifest generator with an explicit 1.1 source copier.
Run once, then keep gen_manifest.py and layout_v1_1.json in source control.
"""
from __future__ import annotations
import hashlib
import json
from pathlib import Path
import sys

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
sys.path.insert(0,str(HERE/"v1810"))
from v1810_reference import manifest_bytes

BASE=HERE/"layout_v1_1.json"
GEN=HERE/"gen_manifest.py"

GENERATOR='''#!/usr/bin/env python3
"""Regenerate the Persona 3 Portable 1.1.0 second-screen layout.

The editable source of truth is tools/p3p/layout_v1_1.json (plain UTF-8 JSON).
This replaces the obsolete 1.0 UI generator entirely; invoking this command
cannot restore the old layout. Native libraries are injected by the standard
source package builder at packaging time.
"""
from __future__ import annotations
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
SOURCE=ROOT/"tools/p3p/layout_v1_1.json"
PACKAGE=ROOT/"packages/Persona3Portable/dualscreen/manifest.json"


def build() -> dict:
    ui=json.loads(SOURCE.read_text(encoding="utf-8"))
    if ui.get("title_id")!="0100DCD01525A000":
        raise ValueError("Wrong title ID in P3P 1.1.0 layout")
    if ui.get("format")!=1 or ui.get("requires_module") is not True:
        raise ValueError("Invalid companion source schema")
    if (ui.get("canvas_w"),ui.get("canvas_h"))!=(1240,1080):
        raise ValueError("Unexpected P3P canvas dimensions")
    if len(ui.get("pages",[]))!=12 or len(ui.get("actions",{}))!=61:
        raise ValueError("P3P UI pages/actions incomplete")
    if len(ui.get("module_outputs",[]))!=2798:
        raise ValueError("P3P reader output schema mismatch")
    libs=ui.get("module",{}).get("libraries")
    if libs!={}:
        raise ValueError("Source must not pin old prebuilt native modules")
    return ui


def main() -> None:
    manifest=build()
    PACKAGE.parent.mkdir(parents=True,exist_ok=True)
    PACKAGE.write_text(json.dumps(manifest,indent=2,ensure_ascii=False)+"\\n",encoding="utf-8")
    print(f"Regenerated 12-page P3P v1.1 layout: {PACKAGE}")


if __name__=="__main__":
    main()
'''

def main():
    original=manifest_bytes()
    ui=json.loads(original)
    assert ui["title_id"]=="0100DCD01525A000"
    assert len(ui["pages"])==12 and len(ui["actions"])==61
    assert len(ui["module_outputs"])==2798
    # Do not pin the two binaries from the old user-tested community release.
    ui["module"]["libraries"]={}
    BASE.write_text(json.dumps(ui,indent=2,ensure_ascii=False)+"\n",encoding="utf-8")
    GEN.write_text(GENERATOR,encoding="utf-8")
    import subprocess
    subprocess.run([sys.executable,str(GEN)],check=True)
    actual=json.loads((ROOT/"packages/Persona3Portable/dualscreen/manifest.json").read_text())
    assert actual==ui
    print("Created readable single-source 1.1 layout, retired old code generator")
    print("Original reference manifest SHA:",hashlib.sha256(original).hexdigest())


if __name__=="__main__":
    main()
