#!/usr/bin/env python3
"""Opt-in ROMFS/schema comparison; temporary standalone CLI avoids shared test sources.
PLA_ROMFS, PLA_SAVE_READER (sibling pla_metadata.py), optional PLA_METADATA_ELF.
No emulator, guest writes, imported artwork, or generated data enters the repository.
"""
import importlib.util
import os
import re
from pathlib import Path
import shlex
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
romfs = os.environ.get('PLA_ROMFS')
reference = os.environ.get('PLA_METADATA_READER')
if not reference and os.environ.get('PLA_SAVE_READER'):
    reference = str(Path(os.environ['PLA_SAVE_READER']).with_name('pla_metadata.py'))
if not romfs or not reference or not Path(reference).is_file():
    print('SKIP: set PLA_ROMFS and external PLA_METADATA_READER (or PLA_SAVE_READER sibling)')
    sys.exit(77)
romfs = Path(romfs)
spec = importlib.util.spec_from_file_location('metadata_reference', reference)
reader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reader)
text_spec = importlib.util.spec_from_file_location('text_reference', Path(reference).with_name('pla_text.py'))
texts = importlib.util.module_from_spec(text_spec)
text_spec.loader.exec_module(texts)
include = Path(__file__).resolve().parents[2] / 'native/modules'
source = r'''
#include "pla_pokemon_metadata.h"
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <vector>
std::vector<uint8_t> Read(const std::filesystem::path &p) {
 std::ifstream f(p,std::ios::binary); assert(f);
 return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};
}
void Hex(const std::string &s) {
 for (unsigned char c:s) std::cout << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
 std::cout << std::dec;
}
int main(int argc,char **argv) {
 assert(argc==3); const std::filesystem::path r=argv[1];
 auto personal=Read(r/"bin/pml/personal/personal_data_total.perbin");
 for (size_t n:{size_t(0),size_t(7),personal.size()/2})
  assert(!pla::ReadPokemonPersonal(std::span(personal).first(n),468,0).valid);
 auto corrupt=personal;std::fill_n(corrupt.begin(),4,0xff);
 assert(!pla::ReadPokemonPersonal(corrupt,468,0).valid);
 assert(!pla::ReadPokemonPersonal(personal,0,0).valid);
 assert(!pla::ReadPokemonPersonal(personal,906,0).valid);
 assert(!pla::ReadPokemonPersonal(personal,468,256).valid);
 assert(!pla::ReadPokemonPersonal(personal,468,255).valid);
 // A second row claiming the selected species/form must not be guessed away.
 pla::FlatMetadata pm(personal);
 const auto tables=pm.Tables(pm.Read(0,4),0,2048);
 corrupt=personal;
 bool duplicate=false;
 for (const auto table:tables) {
  if (pm.Scalar(table,0,2)==1 && pm.Scalar(table,1,2)==0) {
   const auto at=pm.Field(table,0);assert(at);
   corrupt[at]=uint8_t(468);corrupt[at+1]=uint8_t(468>>8);duplicate=true;break;
  }
 }
 assert(duplicate && !pla::ReadPokemonPersonal(corrupt,468,0).valid);
 auto learnset_bytes=Read(r/"bin/pml/waza_oboe/waza_oboe_total.wazaoboe");
 const auto togekiss=pla::ReadPokemonLearnset(learnset_bytes,468,0);assert(togekiss.valid);
 for (size_t n:{size_t(0),size_t(7),learnset_bytes.size()/2})
  assert(!pla::ReadPokemonLearnset(std::span(learnset_bytes).first(n),468,0).valid);
 corrupt=learnset_bytes;std::fill_n(corrupt.begin(),4,0xff);
 assert(!pla::ReadPokemonLearnset(corrupt,468,0).valid);
 assert(!pla::ReadPokemonLearnset(learnset_bytes,0,0).valid);
 assert(!pla::ReadPokemonLearnset(learnset_bytes,906,0).valid);
 assert(!pla::ReadPokemonLearnset(learnset_bytes,468,28).valid);
 pla::FlatMetadata lm(learnset_bytes);
 const auto learn_tables=lm.Tables(lm.Read(0,4),0,2048);
 corrupt=learnset_bytes;duplicate=false;
 for (const auto table:learn_tables) {
  if (lm.Scalar(table,0,2)==1 && lm.Scalar(table,1,2)==0) {
   const auto at=lm.Field(table,0);assert(at);
   corrupt[at]=uint8_t(468);corrupt[at+1]=uint8_t(468>>8);duplicate=true;break;
  }
 }
 assert(duplicate && !pla::ReadPokemonLearnset(corrupt,468,0).valid);
 for (const auto table:learn_tables) {
  if (lm.Scalar(table,0,2)!=468 || lm.Scalar(table,1,2)!=0) continue;
  const auto entries=lm.Tables(table,3,64);assert(!entries.empty());
  const auto mastery=lm.Field(entries.front(),1);assert(mastery);
  corrupt=learnset_bytes;corrupt[mastery]=101;corrupt[mastery+1]=0;
  assert(!pla::ReadPokemonLearnset(corrupt,468,0).valid);
  const auto field=lm.Field(table,3),vector=field+lm.Read(field,4);
  corrupt=learnset_bytes;std::fill_n(corrupt.begin()+vector,4,0xff);
  assert(!pla::ReadPokemonLearnset(corrupt,468,0).valid);break;
 }
 const std::array<uint8_t,8> none{};
 assert(pla::PokemonMoveMastery(togekiss,none,19,314,0)==false);
 assert(pla::PokemonMoveMastery(togekiss,none,20,314,0)==true);
 assert(pla::PokemonMoveMastery(togekiss,none,1,314,314)==true);
 assert(pla::PokemonMoveMastery(togekiss,none,1,94,0)==false);
 auto shop_record=none;shop_record[56>>3]|=1u<<(56&7);
 assert(pla::PokemonMoveMastery(togekiss,shop_record,1,94,0)==true);
 assert(!pla::PokemonMoveMastery({},none,20,314,0).has_value());
 assert(!pla::PokemonMoveMastery(togekiss,none,0,314,0).has_value());
 assert(!pla::PokemonMoveMastery(togekiss,none,101,314,0).has_value());
 assert(!pla::PokemonMoveMastery(togekiss,none,20,0,0).has_value());
 assert(!pla::PokemonMoveMastery(togekiss,none,20,314,851).has_value());
 assert(!pla::PokemonMoveMastery(togekiss,std::array<uint8_t,7>{},20,314,0).has_value());
 const pla::PokemonLearnset repeated{true,{{314,1,50},{314,1,20}}};
 assert(pla::PokemonMoveMastery(repeated,none,30,314,0)==true);
 assert(pla::PokemonMoveMastery(repeated,none,10,314,0)==false);
 assert(pla::PokemonMoveMastery({true,{{314,1,0}}},none,1,314,0)==true);
 auto ember=Read(r/"bin/pml/waza/waza0052.wazabin");
 for (const auto field:{1u,2u,4u,6u,7u,68u,80u}) {
  pla::FlatMetadata wm(ember);const auto at=wm.Field(wm.Read(0,4),field);assert(at);
  corrupt=ember;corrupt[at]=field==7 ? 0 : 255;
  assert(!pla::ReadPokemonMove(corrupt,52).valid);
 }
 // Sanitizer runs also exercise deterministically damaged tables and uoffsets.
 uint32_t seed=0x6819;
 for (unsigned iteration=0;iteration<512;++iteration) {
  seed=seed*1664525+1013904223;corrupt=iteration%3==0 ? personal : iteration%3==1 ? ember : learnset_bytes;
  corrupt[seed%corrupt.size()]^=uint8_t(seed>>24);
  if (iteration%3==0) (void)pla::ReadPokemonPersonal(corrupt,468,0);
  else if (iteration%3==1) (void)pla::ReadPokemonMove(corrupt,52);
  else (void)pla::ReadPokemonLearnset(corrupt,468,0);
 }
 for (unsigned id:{0,51,851}) assert(!pla::ReadPokemonMove(ember,id).valid);
 for (size_t n:{size_t(0),size_t(7)}) assert(!pla::ReadPokemonMove(std::span(ember).first(n),52).valid);
 corrupt=ember;std::fill_n(corrupt.begin(),4,0xff);
 assert(!pla::ReadPokemonMove(corrupt,52).valid);
 auto chart=std::string(argv[2])=="-" ? std::vector<uint8_t>{} : Read(argv[2]);
 assert(!pla::ValidPokemonTypeChart({}));
 if (!chart.empty()) {
  assert(pla::ValidPokemonTypeChart(chart));
  assert(!pla::ValidPokemonTypeChart(std::span(chart).first(323)));
  for (size_t at=0;at<chart.size();++at) {corrupt=chart;corrupt[at]^=1;assert(!pla::ValidPokemonTypeChart(corrupt));}
  assert(!pla::PokemonTypeMatchups(chart,18,0).valid);
  assert(!pla::PokemonTypeMatchups(chart,0,18).valid);
 }
 assert(!pla::PokemonMoveMastered(std::array<uint8_t,7>{},94).has_value());
 assert(!pla::PokemonMoveMastered(std::array<uint8_t,8>{},0).has_value());
 assert(!pla::PokemonMoveMastered(std::array<uint8_t,8>{},851).has_value());
 char kind;unsigned a,b;
 while (std::cin>>kind>>a>>b) {
  if (kind=='P') {
   const auto p=pla::ReadPokemonPersonal(personal,a,b);assert(p.valid);
   std::cout<<p.present<<' '<<p.type1<<' '<<p.type2<<' '<<p.hisui_dex;
   for(auto n:p.base_stats)std::cout<<' '<<n;
  } else if (kind=='L') {
   const auto l=pla::ReadPokemonLearnset(learnset_bytes,a,b);assert(l.valid);
   std::cout<<l.moves.size();
   for(const auto &v:l.moves)std::cout<<' '<<v.move<<' '<<v.level<<' '<<v.mastery_level;
  } else if (kind=='Q') {
   unsigned level,move,alpha,bit;std::cin>>level>>move>>alpha>>bit;
   std::array<uint8_t,8> record{};if(bit<64)record[bit>>3]=uint8_t(1u<<(bit&7));
   const auto result=pla::PokemonMoveMastery(pla::ReadPokemonLearnset(learnset_bytes,a,b),record,level,move,alpha);
   std::cout<<(result.has_value() ? int(*result) : -1);
  } else if (kind=='M') {
   char name[64];std::snprintf(name,sizeof(name),"waza%04u.wazabin",a);
   const auto m=pla::ReadPokemonMove(Read(r/"bin/pml/waza"/name),a);assert(m.valid);
   std::cout<<m.usable<<' '<<m.type<<' '<<m.category<<' '<<m.power<<' '<<m.accuracy<<' '<<m.pp<<' '
    <<m.can_style<<' '<<m.agile_power<<' '<<m.strong_power<<' '<<m.strong_accuracy;
   for(auto n:m.speed_user)std::cout<<' '<<n;
   for(auto n:m.speed_target)std::cout<<' '<<n;
  } else if (kind=='R') {
   std::array<uint8_t,8> record{};if(b<64)record[b>>3]=uint8_t(1u<<(b&7));
   const auto mastered=pla::PokemonMoveMastered(record,a);
   std::cout<<(mastered.has_value() ? int(*mastered) : -1);
  } else if (kind=='C') {
   const auto m=pla::PokemonTypeMatchups(chart,a,b);assert(m.valid);
   for(const auto &v:m.types)std::cout<<v.numerator<<' '<<v.denominator<<' ';
  } else {
   const char *names[]={"typename","seikaku","wazaname","wazainfo"};assert(a<4);
   const auto dat=Read(r/"bin/message/English/common"/(std::string(names[a])+".dat"));
   Hex(a==0 ? pla::PokemonTypeName(dat,b):a==1 ? pla::PokemonNatureName(dat,b):
       a==2 ? pla::PokemonMoveName(dat,b):pla::PokemonMoveDescription(dat,b));
  }
  std::cout<<'\n';
 }
}
'''
expected, commands = [], []
for (species, form), p in reader.personal(romfs).items():
    if not species:
        continue
    commands.append(f'P {species} {form}')
    expected.append(' '.join(str(int(x)) for x in [p['present'], *p['types'], p['hisui_dex'], *p['stats']]))
personal_count = len(commands)
for ident in range(1, 851):
    f = reader.Flat(romfs / f'bin/pml/waza/waza{ident:04d}.wazabin')
    assert f.scalar(f.root, 0, '<i') == ident
    fields = [1, 2, 4, 5, 6, 7, 68, 74, 79, 80, 69, 70, 71, 72, 73]
    values = [f.scalar(f.root, i, '<i' if 69 <= i <= 73 else '<B') for i in fields]
    commands.append(f'M {ident} 0')
    expected.append(' '.join(map(str, values)))
# English tables are the currently configured companion locale, not translated fallbacks.
for table, (name, ids) in enumerate([
    ('typename', range(19)), ('seikaku', range(26)),
    ('wazaname', [0, 52, 94, 161, 314, 585, 841, 851]),
    ('wazainfo', [52, 94, 161, 314, 585, 841, 851]),
]):
    values = texts.read_dat(str(romfs / f'bin/message/English/common/{name}.dat'))
    for ident in ids:
        commands.append(f'T {table} {ident}')
        valid = ident < (18 if table == 0 else 25 if table == 1 else 851) and (table < 2 or ident > 0)
        value = values[ident].replace('\\n', '\n') if valid else ''
        expected.append(value.encode().hex())
# Resolve the pinned primary enum names against actual native move-name IDs;
# never compare the implementation's numeric61list to a mirrored test list.
primary = Path(os.environ.get('PLA_METADATA_PKHEX_PERSONAL', str(Path(reference).parent / 'rawsrc/PersonalInfo8LA.cs')))
source_personal = primary.read_text()
permit = source_personal.split('private static ReadOnlySpan<ushort> MoveShopMoves =>', 1)[1].split('];', 1)[0]
labels = re.findall(r'\(int\)Move\.([A-Za-z0-9]+)', permit)
move_names = texts.read_dat(str(romfs / 'bin/message/English/common/wazaname.dat'))
normalize = lambda value: re.sub('[^A-Za-z0-9]', '', value)
permit_ids = []
for label in labels:
    matches = [i for i, name in enumerate(move_names) if normalize(name) == label]
    assert len(matches) == 1, (label, matches)
    permit_ids.append(matches[0])
assert len(permit_ids) == len(set(permit_ids)) == 61
for ident in range(1, 851):
    if ident in permit_ids:
        index = permit_ids.index(ident)
        for bit in (index, (index + 1) % 64, 64):
            commands.append(f'R {ident} {bit}')
            expected.append(str(int(bit == index)))
    else:
        commands.append(f'R {ident} 64')
        expected.append('-1')
learnset = reader.Flat(romfs / 'bin/pml/waza_oboe/waza_oboe_total.wazaoboe')
learnsets = {}
for table in learnset.tables(learnset.root, 0):
    species, form = [learnset.scalar(table, i, '<H') for i in (0, 1)]
    if not species:
        continue
    rows = [[learnset.scalar(entry, i, '<H') for i in range(3)] for entry in learnset.tables(table, 3)]
    learnsets[species, form] = rows
    commands.append(f'L {species} {form}')
    numbers = [len(rows)]
    for level, mastery, move in rows:
        numbers.extend((move, level, mastery))
    expected.append(' '.join(map(str, numbers)))
# Representative party species and native move IDs exercise the native threshold/shop/AlphaMove rule.
f14 = {(468,0):[314,585,94,161], (571,1):[247,841,53,399],
       (503,1):[401,453,400,224], (59,1):[394,157,442,242],
       (901,0):[838,163,9,8], (903,0):[409,398,332,206]}
for (species, form), moves in f14.items():
    for move in moves:
        for level in (1, 19, 20, 54, 100):
            for alpha, bit in ((0,64), (move,64), (0,permit_ids.index(move) if move in permit_ids else 64)):
                commands.append(f'Q {species} {form} {level} {move} {alpha} {bit}')
                mastered = any(m == move and level >= master for _, master, m in learnsets[species, form])
                mastered |= move in permit_ids and bit == permit_ids.index(move)
                mastered |= alpha == move
                expected.append(str(int(mastered)))
with tempfile.TemporaryDirectory(prefix='pla-pokemon-metadata-') as scratch:
    scratch = Path(scratch)
    (scratch / 'test.cpp').write_text(source)
    compiler = shlex.split(os.environ.get('CXX', 'c++'))
    flags = ['-std=c++20', '-O2', '-Wall', '-Wextra']
    if os.environ.get('PLA_METADATA_SANITIZE'):
        flags += ['-O1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run([*compiler, *flags, '-I', str(include), str(scratch / 'test.cpp'), '-o', str(scratch / 'test')], check=True)
    chart_file = '-'
    elf = os.environ.get('PLA_METADATA_ELF')
    if elf:
        data = Path(elf).read_bytes()
        assert data[:6] == b'\x7fELF\x02\x01'
        phoff = struct.unpack_from('<Q', data, 32)[0]
        phsize, phcount = struct.unpack_from('<HH', data, 54)
        chart = None
        for i in range(phcount):
            kind, _, offset, address, _, filesz, _, _ = struct.unpack_from('<IIQQQQQQ', data, phoff + i * phsize)
            target = 0x800000000 + 0x3984908
            if kind == 1 and address <= target and target + 324 <= address + filesz:
                chart = data[offset + target-address:offset + target-address + 324]
        assert chart is not None
        (scratch / 'chart.bin').write_bytes(chart)
        chart_file = str(scratch / 'chart.bin')
        products = {0: (0, 1), 4: (2, 5), 8: (1, 2), 16: (1, 1), 32: (2, 1), 64: (5, 2)}
        for type1 in range(18):
            for type2 in range(18):
                commands.append(f'C {type1} {type2}')
                numbers = []
                for attack in range(18):
                    product = chart[18 * attack + type1] * (4 if type1 == type2 else chart[18 * attack + type2])
                    numbers.extend(products[product])
                expected.append(' '.join(map(str, numbers)))
    actual = subprocess.check_output([str(scratch / 'test'), str(romfs), chart_file], input='\n'.join(commands)+'\n', text=True).splitlines()
    assert len(actual) == len(expected), (len(actual), len(expected))
    for command, got, want in zip(commands, actual, expected):
        assert got.strip() == want, (command, got, want)
print(f'Learnset mastery: {len(learnsets)} native species/forms + threshold/shop/AlphaMove rule checked')
print('Shop-only mastery API: all61primary ordered indices / all850move IDs checked; nonshop returns unknown')
print(f'Private Pokemon metadata: {personal_count} personal forms, 850 moves, native names/descriptions and bounds match')
print('Native type chart: all 324 defender pairs / 5832 matchups + every single-byte mutation checked' if elf else 'Native type chart skipped: set PLA_METADATA_ELF to the retained exact carrier')
