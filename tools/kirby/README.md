# Kirby's Return to Dream Land Deluxe tools

`gen_manifest.py` writes `packages/KirbysReturnToDreamLandDeluxe/dualscreen/manifest.json` and three
plans that the module reads:

- `shatter.txt`: Grand Doomer's and Magolor's cuts, pieces and shards;
- `finale.txt`: the Master Crown finale;
- `soul.txt`: Magolor Soul's second phase.

```sh
python3 tools/kirby/gen_manifest.py         # the release manifest
python3 tools/kirby/gen_manifest.py --dev   # adds the research actions used with the desktop memory console
```

The script needs only the Python standard library and no game files. Running it reproduces the
committed files exactly.

Every piece of game art in the manifest is a `module:kirby:...` key. The module decodes each one
from the player's own romfs: zstd and LZ4 archives, SARC, BNTX, BFLYT, BFRES, BFFNT and MSBT. The
module is `01006B601380E000.cpp` and `kirby_*.cpp` under `native/modules`.
