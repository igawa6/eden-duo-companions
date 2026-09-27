#!/usr/bin/env python3
"""set_min_runtime.py <pkg dir> -- write "min_runtime" (p5style.MIN_RUNTIME) into package.json and
dualscreen/manifest.json. Runtimes >= 11 show a built-in "update Eden" page instead of loading a
package whose min_runtime is newer than they are; older runtimes ignore the key."""
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
os.environ.setdefault('ASSET_MODE', 'png')
from p5style import MIN_RUNTIME  # noqa: E402

pkg = Path(sys.argv[1])
value = int(sys.argv[2]) if len(sys.argv) > 2 else MIN_RUNTIME  # override: gating tests only
mp = pkg / 'dualscreen/manifest.json'
m = json.loads(mp.read_text())
m['min_runtime'] = value
mp.write_text(json.dumps(m, ensure_ascii=False, separators=(',', ':')) + '\n')
pp = pkg / 'package.json'
meta = json.loads(pp.read_text())
meta['min_runtime'] = value
pp.write_text(json.dumps(meta, indent=1) + '\n')
print('min_runtime', value)
