#!/usr/bin/env python3
"""Opt-in recipe schema/model reference check. No guest access or bundled game data.

PLA_ROMFS and PLA_SAVE_READER are required. PLA_SAVE / PLA_RECIPE_F02 / PLA_RECIPE_F05
are optional saved fixtures, never current-state evidence. ASan/UBSan enabled.
"""
import collections
import importlib.util
import os
from pathlib import Path
import shlex
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
romfs, reference = os.environ.get('PLA_ROMFS'), os.environ.get('PLA_SAVE_READER')
if not romfs or not reference:
    print('SKIP: set PLA_ROMFS and external PLA_SAVE_READER')
    sys.exit(77)
sys.path.insert(0, str(Path(reference).parent))
from pla_metadata import Flat
spec = importlib.util.spec_from_file_location('recipe_save_reference', reference)
reader = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = reader
spec.loader.exec_module(reader)
path = Path(romfs)/'bin/appli/table/making/makelist.bin'
flat = Flat(path)
expected = [[flat.scalar(t, i, '<B' if i == 0 else '<I') for i in range(12)]
            for t in flat.tables(flat.root, 0)]
assert len(expected) == 43
source = r'''
#include "pla_recipes.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
std::vector<uint8_t> Read(const char *p) {
 std::ifstream f(p,std::ios::binary);assert(f);
 return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};
}
int main(int argc,char**argv) {
 assert(argc==2||argc==5);const auto bytes=Read(argv[1]);
 const auto defs=pla::ParseRecipeDefinitions(bytes);assert(defs.valid&&defs.rows.size()==43);
 std::vector<uint8_t> pouch(2700);assert(pla::ParseRecipeMaterials(pouch,20).available);
 pouch[80]=1;pouch[82]=1;assert(!pla::ParseRecipeMaterials(pouch,20).available);
 assert(pla::ParseRecipeMaterials(std::span(pouch).first(80),20).available);
 assert(!pla::ParseRecipeMaterials(pouch,0).available);
 assert(!pla::ParseLearnedRecipes(std::span(pouch).first(279)).available);
 std::vector<uint8_t> learned(280);learned[0]=1;learned[2]=2;assert(!pla::ParseLearnedRecipes(learned).available);

 for(const auto&r:defs.rows) {
  std::cout<<"D "<<r.id<<' '<<r.recipe_item<<' '<<r.result_item<<' '<<r.result_quantity;
  for(unsigned k=0;k<4;++k)std::cout<<' '<<(k<r.materials.size()?r.materials[k].id:0)
                                      <<' '<<(k<r.materials.size()?r.materials[k].count:0);
  std::cout<<'\n';
 }
 for(size_t n=0;n<bytes.size();++n)assert(!pla::ParseRecipeDefinitions(std::span(bytes).first(n)).valid);
 auto bad=bytes;std::fill_n(bad.begin(),4,255);assert(!pla::ParseRecipeDefinitions(bad).valid);
 pla::FlatMetadata f(bytes);const auto tables=f.Tables(f.Read(0,4),0,70);assert(f.Valid());
 auto replace=[&](unsigned row,unsigned field,unsigned value,unsigned size=4) {
  auto copy=bytes;const auto at=f.Field(tables[row],field);assert(at);
  for(unsigned n=0;n<size;++n)copy[at+n]=uint8_t(value>>(8*n));
  assert(!pla::ParseRecipeDefinitions(copy).valid);
 };
 replace(0,3,0);replace(0,3,2);replace(0,2,4096);replace(0,5,0);
 replace(1,0,defs.rows[0].id,1);replace(1,1,defs.rows[0].recipe_item);
 replace(1,2,defs.rows[0].result_item);replace(0,6,defs.rows[0].materials[0].id);

 pla::RecipePageSnapshot s;s.generation=1;s.learned.available=true;
 assert(pla::ProjectRecipes(defs,s).available&&pla::ProjectRecipes(defs,s).rows.empty());
 const auto item=defs.rows.front().recipe_item;s.learned.items={{item,1}};
 auto m=pla::ProjectRecipes(defs,s);assert(m.available&&m.rows.size()==1);
 s.dock_only=true;assert(!pla::ProjectRecipes(defs,s).available);
 s.dock_available=true;assert(pla::ProjectRecipes(defs,s).rows.empty());
 s.dock_items={defs.rows.front().result_item};assert(pla::ProjectRecipes(defs,s).rows.size()==1);
 s.dock_items.push_back(s.dock_items.front());assert(!pla::ProjectRecipes(defs,s).available);
 s.dock_items.clear();s.dock_only=s.dock_available=false;
 s.satchel_capacity=19;assert(!pla::ProjectRecipes(defs,s).available);
 s.satchel_capacity=20;s.materials.available=true;
 auto capacity=pla::ProjectRecipes(defs,s);assert(capacity.capacity==20&&capacity.used_slots==0);
 s.materials.available=false;assert(!pla::ProjectRecipes(defs,s).capacity);s.satchel_capacity=0;

 assert(!m.rows[0].material_batches&&std::all_of(m.rows[0].have.begin(),m.rows[0].have.end(),
     [](auto n){return !n.has_value();}));
 s.materials.available=true;m=pla::ProjectRecipes(defs,s);
 assert(m.available&&m.rows[0].material_batches==0);
 s.materials.items={{4096,1}};assert(!pla::ProjectRecipes(defs,s).available);s.materials.items.clear();
 s.learned.items={{item,2}};assert(!pla::ProjectRecipes(defs,s).available);
 s.learned.items={{item,1},{item,1}};assert(!pla::ProjectRecipes(defs,s).available);
 s.learned.items={{4095,1}};assert(!pla::ProjectRecipes(defs,s).available);
 s.learned.items={{item,1}};s.generation=0;assert(!pla::ProjectRecipes(defs,s).available);
 s.generation=1;s.learned.available=false;assert(!pla::ProjectRecipes(defs,s).available);
 if(argc==5) {
  s.learned=pla::ParseLearnedRecipes(Read(argv[2]));
  const auto upgrades=Read(argv[4]);assert(upgrades.size()==4);unsigned value=0;
  for(unsigned n=0;n<4;++n)value|=unsigned(upgrades[n])<<(8*n);
  s.satchel_capacity=20+std::min(655u,value);
  s.materials=pla::ParseRecipeMaterials(Read(argv[3]),s.satchel_capacity);
  m=pla::ProjectRecipes(defs,s);assert(m.available&&m.generation==1);
  if(m.capacity)std::cout<<"C "<<*m.used_slots<<' '<<*m.capacity<<'\n';
  for(const auto&r:m.rows) {
   std::cout<<"R "<<r.definition.id<<' '<<(r.material_batches?int(*r.material_batches):-1);
   for(auto n:r.have){std::cout<<' '<<(n?int(*n):-1);}std::cout<<'\n';
  }
 }
}
'''
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='pla-recipes-') as temp:
    temp = Path(temp)
    cpp, cli = temp/'check.cpp', temp/'check'
    cpp.write_text(source)
    command = [*shlex.split(os.environ.get('CXX', 'c++')), '-std=c++20', '-O1', '-g',
               '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
               '-fno-omit-frame-pointer', '-I'+str(root/'native/modules'), str(cpp), '-o', str(cli)]
    subprocess.run(command, check=True)
    runs = [([str(cli), str(path)], [], None)]
    for name in ('PLA_SAVE', 'PLA_RECIPE_F02', 'PLA_RECIPE_F05'):
        if not os.environ.get(name):
            continue
        blocks, valid = reader.decrypt_save(Path(os.environ[name]).read_bytes())
        assert valid
        recipe = reader.find_block(blocks, 0xF5D9F4A5).data
        bag = reader.find_block(blocks, 0x9FE2790A).data
        learned = {i for i, n in struct.iter_unpack('<HH', recipe) if i and n == 1}
        upgrades = reader.find_block(blocks, 0x75CE2CF6).data
        limit = min(675, 20+struct.unpack('<I', upgrades)[0])
        trusted_materials = not any(bag[limit*4:])
        counts = collections.Counter()
        for i, n in struct.iter_unpack('<HH', bag[:limit*4]):
            if i:
                counts[i] += n
        rows = []
        for row in expected:
            if row[1] not in learned:
                continue
            ingredients = [(row[i], row[i+1]) for i in range(4, 12, 2) if row[i]]
            rows.append([row[0], min(counts[i]//n for i, n in ingredients) if trusted_materials else -1,
                         *[counts[i] if trusted_materials else -1 for i, n in ingredients]])
        rp, bp = temp/(name+'-recipes.bin'), temp/(name+'-satchel.bin')
        rp.write_bytes(recipe)
        bp.write_bytes(bag)
        upgrades = reader.find_block(blocks, 0x75CE2CF6).data
        up = temp/(name+'-upgrades.bin')
        up.write_bytes(upgrades)
        expected_capacity = ([sum(bool(i) for i, n in struct.iter_unpack('<HH', bag[:limit*4])), limit]
                             if trusted_materials else None)
        runs.append(([str(cli), str(path), str(rp), str(bp), str(up)], rows, expected_capacity))
    for command, rows, expected_capacity in runs:
        output = subprocess.check_output(command, text=True)
        definitions, projected, capacity = [], [], None
        for line in output.splitlines():
            kind, *values = line.split()
            values = list(map(int, values))
            if kind == 'C':
                capacity = values
            else:
                (definitions if kind == 'D' else projected).append(values)
        assert definitions == expected
        assert projected == rows
        assert capacity == expected_capacity
        print(f'Recipe reference: 43 definitions, {len(rows)} learned rows, malformed/unknown checks pass')
print('Recipe schema/model: ASan/UBSan passed; offline inputs only')
