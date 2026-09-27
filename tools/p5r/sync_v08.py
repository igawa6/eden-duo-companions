#!/usr/bin/env python3
"""sync_v08.py <pkg dir> <version> <linux .so> [android .so] -- copy modules, update manifest +
package.json sha256 and version."""
import hashlib, json, shutil, sys
from pathlib import Path
pkg = Path(sys.argv[1]); version = sys.argv[2]; libs = {'linux-x86_64': sys.argv[3]}
if len(sys.argv) > 4: libs['android-arm64-v8a'] = sys.argv[4]
mp = pkg / 'dualscreen/manifest.json'; m = json.loads(mp.read_text())
for plat, src in libs.items():
    e = m['module']['libraries'][plat]; t = pkg / 'dualscreen' / e['path']
    t.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(src, t)
    e['sha256'] = hashlib.sha256(t.read_bytes()).hexdigest(); print(plat, e['sha256'])
mp.write_text(json.dumps(m, ensure_ascii=False, separators=(',', ':')) + '\n')
meta = json.loads((pkg / 'package.json').read_text()); meta['module'] = m['module']; meta['version'] = version
(pkg / 'package.json').write_text(json.dumps(meta, indent=1) + '\n')
