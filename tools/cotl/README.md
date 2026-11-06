# Cult of the Lamb tools

Scripts for the Cult of the Lamb companion (`01002E7016C46000`). The module is
`native/modules/01002E7016C46000.cpp` with the `cotl_*` files next to it. It reads the game, and
follower commands run in a small guest helper, `native/helper/cotl/action_tick.c`.

## Compatibility and validation

- Game versions: **1.5.27 and 1.5.28**. Both have main build
  `316A4D4881A9F1E077A07262E917917C`, and their code and the data files the companion reads are
  byte-identical, so one set of offsets serves both. Any other build is refused.
- Tested on an **AYN Thor** with **Eden Duo 1.3.2.2**, and headless with `eden-cli`.
- Declares **runtime 22** as its minimum (`content_h_bind` in the inventory scroll region).
- English text only, and only player 1 in co-op.

## Files

| File | What it does |
|---|---|
| `gen_manifest.py` | Writes `packages/CultOfTheLamb/dualscreen/manifest.json`. Never edit the manifest by hand. |
| `gen_enums.py` | `cotl_enums_1_5_27.inc` from Il2CppDumper's `dump.cs`. |
| `gen_atlas_bundles.sh`, `bundlescan.cpp` | `cotl_atlas_bundles_1_5_27.inc`: SpriteAtlas name to Addressables bundle. Needs `COTL_ROMFS`. |
| `menu_lists.py` | `cotl_inventory_menu_1_5_27.inc` from `InventoryMenu..ctor`. |
| `room_icons.py` | `cotl_room_icons_1_5_27.inc`: room prefab to `MapIcon` sprite. |
| `spine_skins.py`, `tarot_regions.py` | `cotl_tarot_regions_1_5_27.inc`: tarot card to its region in `WeaponCard.atlas`. |
| `spine_portraits.py` | `cotl_follower_parts_1_5_27.inc`: the parts of a follower picture from `Follower.skel` and `Follower.atlas`. |
| `gen_action_helper.py` | Compiles the guest helper and writes its format-2 load plan under `packages/CultOfTheLamb/dualscreen/guest/`. Pass the game's `main` with `--verify-image`; it stops if the hook bytes do not match. Needs the Android NDK (`COTL_DEPS`). |
| `gen_ui_art.py` | The button frames and icons in `packages/CultOfTheLamb/dualscreen/ui/`, drawn from patterns in code. |

The generated tables hold only code identifiers and file names. The fuller reverse-engineering
notes, in German, are in
[AlterDigga/Cult-of-the-lamb-companion](https://github.com/AlterDigga/Cult-of-the-lamb-companion/tree/main/docs).

## Tests

```bash
ctest --test-dir <build-directory> -R '^dsmod-cotl-module$' --output-on-failure
```

Set `COTL_ROMFS` to your own merged romfs to run the tests that read real game files as well.

```bash
PYTHONPATH=<unicorn> python3 tools/cotl/test_action_helper.py   # guest helper under Unicorn
python3 tools/cotl/test_gen_ui_art.py
```

`test_action_helper.py` runs the helper against stand-ins for the game functions. Set `COTL_MAIN`
to your own `main` to check the hook against it as well.

## Notes

- **Ready signal.** `UIMainMenuController.Instance` is set only at the title screen, and
  `SaveAndLoad.Loaded` stays set after quitting, so the title check comes first.
- **I2 terms.** Texts come from `I2Languages` in `resources.assets`, using the same term formats as
  the game code, for example `Traits/{0}` and `UpgradeSystem/{0}/Name`.
- **Problems and quests.** Thresholds and states follow the game's follower code. Quest text follows
  `ObjectivesData.get_Text` of each subclass.
- **Inventory icons.** `Inventory Icon Mapping` gives the sprite. Most sprites are in atlases in
  Addressables bundles. Others have their own texture in `resources.assets`, as ASTC 12x12 in Tegra
  block layout, where one GOB per block is still swizzled, so `DecodeSwitchSprite` handles them.
- **Spine images.** HUD hearts and tarot fronts exist only as Spine skins (`heart.atlas`,
  `WeaponCard.atlas`) and are composed from their regions.
- **Game font.** TextMesh Pro `LaptureRegular`, converted from SDF to a coverage atlas, with heart
  icons appended as extra glyphs.
- **Dungeon map.** Follows `MiniMapIcon.SetIcons` and `SetSelfToQuestionMark`, and shows only what
  the game's map would show.
- **Equipment cards.** Stats follow `PlayerWeapon.GetDamage` and
  `PlayerSpells.GetCurseDamageMultiplier`. Co-op tarot cards and
  `IsCorruptedPositiveEffectNegated` are not replicated. `EquipmentManager` loads its weapon and
  curse tables when an item is equipped, so stats appear only once that has happened.
- **Follower commands.** The guest helper hooks `PlayerFarming.Update`. The module writes one
  request at a time into a 4 KiB mailbox; the helper checks it against the game state (base only,
  no menu or dialogue, the follower free and the command on offer), walks the Lamb over and runs
  the command through the game's own command handler, then reports the result. The module offers
  the same command list as the game's command wheel and greys out the rest with the game's reason.
- **Follower pictures.** Composed from the `Follower` Spine skins with each follower's outfit and
  colours, like the game's follower portraits.
