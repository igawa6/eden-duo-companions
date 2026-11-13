"""Inventory evidence ABI and production-plan regression checks."""
import hashlib
import struct
import tempfile
import unittest
from pathlib import Path

import build_helper_plan as production
import build_inventory_plan as inventory
from record_frame_probe import branch_target


class InventoryPlanTests(unittest.TestCase):
    def test_production_plan_matches_reviewed_r6(self):
        with tempfile.TemporaryDirectory() as work:
            plan=production.build_plan(Path(work))
        import json
        data=(json.dumps(plan,indent=2)+"\n").encode()
        self.assertEqual(hashlib.sha256(data).hexdigest(),
                         "c65d04f6bee1f20e32952f9b8220dc468d49fcc233717c198206d7ca276d89be")
        self.assertNotEqual(plan['build_id'],inventory.BUILD_ID)

    def test_runtime22_guest_instruction_compatibility(self):
        for build in [production.BUILD_ID,inventory.BUILD_ID]:
            with tempfile.TemporaryDirectory() as work:
                plan=production.build_plan(Path(work),build)
            code=bytes.fromhex(plan['guest_code']['bytes'])
            for (word,) in struct.iter_unpack('<I',code):
                self.assertNotEqual(word & 0xFF000000,0xD4000000)
                self.assertFalse(word & 0xFF000000 == 0xD5000000 and word != 0xD503201F)

    def test_evidence_sites_preserve_tail_and_player_call(self):
        with tempfile.TemporaryDirectory() as work:
            plan=inventory.build_plan(Path(work))
        ui,food=plan['patches']
        self.assertEqual(ui['expected'],'6fb4ff17')
        self.assertEqual(ui['replacement'],'00000014')
        word=struct.unpack('<I',bytes.fromhex(food['expected']))[0]
        self.assertEqual(branch_target(word,0xCD1A30,True),0xCF233C)
        self.assertEqual(food['replacement'],'00000094')
        code=bytes.fromhex(plan['guest_code']['bytes'])
        self.assertLess(len(code),65536)
        self.assertGreater(food['relocations'][0]['addend'],0)
        self.assertLess(food['relocations'][0]['addend'],len(code))
        self.assertEqual(inventory.SYMBOLS['carry_actor_vtable'],0x1CBB040)
        # Excluded areas have no relocations, so an unresolved map/HUD/warp
        # symbol cannot be quietly translated into a callable old address.
        for name in inventory.SYMBOLS:
            self.assertFalse(name.startswith(('hud_','map_','warp_','quest_')))
        self.assertEqual(inventory.SYMBOLS['native_hold_spawn'],0xBE2A40)
        self.assertEqual(inventory.SYMBOLS['native_info_int'],0x108EF5C)
        self.assertEqual(inventory.SYMBOLS['native_has_tag'],0x108E7A0)

    def test_ready_plan_adds_only_verified_hud_targets(self):
        with tempfile.TemporaryDirectory() as work:
            plan=inventory.build_plan(Path(work),ready=True)
        targets={r['addend'] for r in plan['guest_code']['relocations']
                 if r['target']=='main'}
        self.assertTrue({0x1CE8298,0x1D0E500,0x1CF4C20,0x1D0EEA0,
                         0x1D0E318,0x22C60} <= targets)
        self.assertFalse({production.SYMBOLS[k] for k in production.SYMBOLS
                          if k.startswith(('warp_','quest_'))} & targets)
        self.assertEqual(plan['build_id'],inventory.BUILD_ID)

    def test_quest_plan_uses_native_rebuild_and_integer_setter(self):
        with tempfile.TemporaryDirectory() as work:
            plan=inventory.build_plan(Path(work),quests=True)
        targets={r['addend'] for r in plan['guest_code']['relocations']
                 if r['target']=='main'}
        self.assertTrue({0x1E46720,0x1D3BBD8,0x1E17EF8,0x1CDD6C8,
                         0x1DA6E60,0xD8A150,0x6B6B3C} <= targets)
        self.assertNotIn(production.SYMBOLS['native_destination_set'],targets)
        self.assertNotIn(production.SYMBOLS['native_warp'],targets)

    def test_shortcut_plan_supplies_option_getter_bank_argument(self):
        with tempfile.TemporaryDirectory() as work:
            plan=inventory.build_plan(Path(work),shortcuts=True)
        code=bytes.fromhex(plan['guest_code']['bytes'])
        targets={r['addend'] for r in plan['guest_code']['relocations']
                 if r['target']=='main'}
        self.assertTrue({0x1E2D538,0x1E2D598,0x1CDB958,0xD6BFA0,
                         0xE278C0,0xD6696C,0xE25A18,0x1DA6928} <= targets)
        calls=[r for r in plan['guest_code']['relocations']
               if r['target']=='main' and r['addend']==0xD349D0
               and r['kind']=='aarch64_branch26']
        self.assertEqual(len(calls),1)
        at=calls[0]['offset']
        self.assertEqual(code[at-4:at].hex(),'e1031f2a')  # mov w1,wzr

    def test_map_plan_adds_independent_map_identities_and_edit_paths(self):
        with tempfile.TemporaryDirectory() as work:
            plan=inventory.build_plan(Path(work),maps=True)
        targets={r['addend'] for r in plan['guest_code']['relocations'] if r['target']=='main'}
        self.assertTrue({0x1E17BB8,0x1CE0748,0x1CDBFB0,0x1CDD340,0x1CDD438,
                         0xD8AC88,0xD8A034,0xD8226C,0xD81D04,0x16A1414} <= targets)
        self.assertNotIn(production.SYMBOLS['native_warp'],targets)
        self.assertNotIn(0x1CDD6E8,targets)  # adjacent static identity is not the live area

    def test_travel_getters_all_use_explicit_bank_zero(self):
        with tempfile.TemporaryDirectory() as work:
            plan=inventory.build_plan(Path(work),travel=True)
        code=bytes.fromhex(plan['guest_code']['bytes'])
        targets={r['addend'] for r in plan['guest_code']['relocations'] if r['target']=='main'}
        self.assertTrue({0x1E3A638,0x1D1FCE8,0x1E465B0,0x1DA6F48,0xE81A30,
                         0x1CDD530,0x15EEDEF,0x161794D} <= targets)
        calls=[r for r in plan['guest_code']['relocations'] if r['target']=='main'
               and r['addend']==0xD349D0 and r['kind']=='aarch64_branch26']
        self.assertEqual(len(calls),4)  # amiibo plus all three travel prohibitions
        for call in calls:
            at=call['offset']
            self.assertEqual(code[at-4:at].hex(),'e1031f2a')
        self.assertNotIn(production.SYMBOLS['native_warp'],targets)

    def test_production_build_selection_is_exact(self):
        with tempfile.TemporaryDirectory() as work:
            plan=production.build_plan(Path(work),inventory.BUILD_ID)
        self.assertEqual(plan['build_id'],inventory.BUILD_ID)
        self.assertEqual(plan['patches'][0]['offset'],0xE79530)
        self.assertEqual(plan['patches'][0]['expected'],'6fb4ff17')
        self.assertTrue(any(r['addend']==0xE81A30 and r['target']=='main'
                            for r in plan['guest_code']['relocations']))
        with tempfile.TemporaryDirectory() as work:
            for bad in [inventory.BUILD_ID[:16],inventory.BUILD_ID[:-1]+'1','',production.BUILD_ID.lower()]:
                with self.assertRaises(ValueError):production.build_plan(Path(work),bad)


if __name__=='__main__':
    unittest.main()
