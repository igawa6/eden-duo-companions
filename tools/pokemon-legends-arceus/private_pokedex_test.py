#!/usr/bin/env python3
"""Opt-in decoded Pokedex snapshot/schema comparison. Never connects to a guest.
Required: PLA_ROMFS, PLA_SAVE, PLA_SAVE_READER. Optional PLA_POKEDEX_F02/F05
or PLA_POKEDEX_PAYLOAD (exact decoded124000B, no save/freshness equivalence implied).
PLA_POKEDEX_SANITIZE=1 enables ASan/UBSan for the isolated temporary CLI.
"""
import importlib.util
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
romfs, save, reference = (os.environ.get(n) for n in ('PLA_ROMFS', 'PLA_SAVE', 'PLA_SAVE_READER'))
if not romfs or not save or not reference:
    print('SKIP: set PLA_ROMFS, PLA_SAVE and external PLA_SAVE_READER')
    sys.exit(77)
romfs = Path(romfs)
def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    out = importlib.util.module_from_spec(spec)
    sys.modules[name] = out
    spec.loader.exec_module(out)
    return out
reader = module('dex_save_reference', reference)
metadata = module('dex_metadata_reference', Path(reference).with_name('pla_metadata.py'))
texts = module('dex_text_reference', Path(reference).with_name('pla_text.py'))
primary = Path(reference).parent / 'rawsrc/PokedexConstants8a.cs'
constants = primary.read_text()
def array(name):
    text = constants.split(name + ' =>', 1)[1].split('[', 1)[1].split(']', 1)[0]
    return [int(n, 0) for n in re.findall(r'0x[0-9a-fA-F]+|\d+', text)]
primary_map = dict(zip(array('FormStorageIndexIds'), array('FormStorageIndexValues')))
index_data = (romfs / 'bin/appli/pokedex/res_table/pokedex_form_storage_index_table.bin').read_bytes()
assert struct.unpack_from('<H', index_data)[0] == 318
forms = {}
for species, form, storage in struct.iter_unpack('<HHH', index_data[2:]):
    assert primary_map[species | form << 11] == storage
    if species:
        forms.setdefault(species, []).append((form, storage))
assert len(primary_map) == 318
personal = metadata.Flat(romfs / 'bin/pml/personal/personal_data_total.perbin')
catalog = {}
for table in personal.tables(personal.root, 0):
    species = personal.scalar(table, 0, '<H')
    dex = personal.scalar(table, 49, '<H')
    if not dex:
        continue
    locals_ = [personal.scalar(table, i, '<I') for i in range(51, 56)]
    old = catalog.setdefault(species, [dex, [0]*5])
    assert old[0] == dex
    old[1] = [a or b for a, b in zip(old[1], locals_)]
assert len(catalog) == 242
assert sorted(v[0] for v in catalog.values()) == list(range(1, 243))
names = texts.read_dat(str(romfs / 'bin/message/English/common/monsname.dat'))
# Exact cached primary FBS fields, independent Python Flat reader.
native_tasks = metadata.Flat(romfs / 'bin/appli/pokedex/res_table/pokedex_research_task_table.bin')
tasks = {}
for row in native_tasks.tables(native_tasks.root, 0):
    scalars = [native_tasks.scalar(row, i, '<Q' if i in (6,7,8) else '<B' if i == 16 else '<I') for i in range(17)]
    species = scalars[0]
    thresholds = [v for v in scalars[9:14] if v]
    record = [*scalars[:9], scalars[14], scalars[15], scalars[16], len(thresholds), *thresholds]
    tasks.setdefault(species, []).append(record)
assert sum(map(len, tasks.values())) == 1611
source = r'''
#include "pla_pokedex.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
std::vector<uint8_t> Read(const std::filesystem::path &p) {
 std::ifstream f(p,std::ios::binary);assert(f);
 return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};
}
void Hex(const std::string &s) {
 for(unsigned char c:s)std::cout<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(c);
 std::cout<<std::dec;
}
int main(int argc,char **argv) {
 assert(argc==3);const std::filesystem::path r=argv[1];
 const auto personal=Read(r/"bin/pml/personal/personal_data_total.perbin");
 const auto forms=Read(r/"bin/appli/pokedex/res_table/pokedex_form_storage_index_table.bin");
 const auto taskbytes=Read(r/"bin/appli/pokedex/res_table/pokedex_research_task_table.bin");
 const auto index=pla::ReadPokedexIndex(personal,forms);assert(index.valid);
 const auto table=pla::ReadPokedexResearchTable(taskbytes);assert(table.valid);
 const auto names=Read(r/"bin/message/English/common/monsname.dat");
 const auto bytes=Read(argv[2]);auto snapshot=pla::ParsePokedexSnapshot(bytes);assert(snapshot.valid);
 for(size_t n:{size_t(0),size_t(7),bytes.size()-1})assert(!pla::ParsePokedexSnapshot(std::span(bytes).first(n)).valid);
 auto bad=bytes;bad.push_back(0);assert(!pla::ParsePokedexSnapshot(bad).valid);
 bad=bytes;bad[15]=0x80;assert(!pla::ParsePokedexSnapshot(bad).valid);
 bad=bytes;bad[3]=0x80;assert(!pla::ParsePokedexSnapshot(bad).valid);
 assert(!pla::ReadPokedexEntry(snapshot,index,0).valid);
 assert(!pla::ReadPokedexEntry(snapshot,index,906).valid);
 assert(!pla::ReadPokedexEntry(snapshot,index,1).valid);
 assert(!pla::ReadPokedexSummary(snapshot,index,6).valid);
 assert(!pla::ReadPokedexForm(snapshot,index,25,28).valid);
 assert(!pla::ReadPokedexIndex(personal,std::span(forms).first(forms.size()-1)).valid);
 bad=forms;bad[0]^=1;assert(!pla::ReadPokedexIndex(personal,bad).valid);
 bad=forms;bad[6]=0xff;bad[7]=0xff;assert(!pla::ReadPokedexIndex(personal,bad).valid);
 bad=personal;std::fill_n(bad.begin(),4,0xff);assert(!pla::ReadPokedexIndex(bad,forms).valid);
 bad=taskbytes;std::fill_n(bad.begin(),4,0xff);assert(!pla::ReadPokedexResearchTable(bad).valid);
 assert(!pla::ReadPokedexResearchTable(std::span(taskbytes).first(taskbytes.size()/2)).valid);
 // Even forged update/report/perfect flags and counters cannot reveal a locked name.
 std::vector<uint8_t> locked(pla::PokedexDataSize);
 const size_t selected=0x70+25*0x58;
 locked[selected]=0x7f;locked[selected+8]=100;locked[selected+0x10]=200;
 auto hidden=pla::ParsePokedexSnapshot(locked);assert(hidden.valid);
 const auto no=pla::ReadPokedexEntry(hidden,index,25);
 assert(no.valid && !no.revealed && !no.species && !no.obtained_count && !no.raw_research_rate &&
        !no.has_report && !no.perfect && !no.reported_progress);
 for(auto v:no.counters)assert(!v);
 assert(pla::PokedexSpeciesName(hidden,index,25,names).empty());
 assert(pla::PokedexRevealedTasks(hidden,index,table,25).empty());
 assert(!pla::ReadPokedexEntry({},index,25).valid);
 assert(pla::PokedexSpeciesName({},index,25,names).empty());
 // A species sighting of one form cannot authorize the other form's portrait.
 const auto &vulpix=index.species[37].forms;assert(vulpix.size()==2);
 locked.assign(pla::PokedexDataSize,0);locked[0x151a8+vulpix[0].storage_index*0x18+4]=1;
 locked[0x70+37*0x58+0x50]=uint8_t(vulpix[1].form);
 const auto oneform=pla::ParsePokedexSnapshot(locked);
 const auto ve=pla::ReadPokedexEntry(oneform,index,37);
 assert(ve.valid && ve.revealed && !ve.selected_form_revealed);
 assert(!pla::ReadPokedexForm(oneform,index,37,vulpix[1].form).revealed);
 assert(pla::PokedexEntryTextPermission(ve)==pla::PokedexEntryTextKind::Unavailable);
 const auto permitted_form=pla::ReadPokedexForm(oneform,index,37,vulpix[0].form);
 assert(pla::PokedexEntryTextPermission(ve,permitted_form)==pla::PokedexEntryTextKind::Generic);
 assert(pla::PokedexGenericCommentVariant(ve,permitted_form)==0);
 auto foreign_form=permitted_form;foreign_form.species=25;
 assert(pla::PokedexEntryTextPermission(ve,foreign_form)==pla::PokedexEntryTextKind::Unavailable);
 assert(!pla::PokedexGenericCommentVariant(ve,foreign_form).has_value());
 // Primary derived task format and required-task correction, kept separate from native enum.
 std::array<uint8_t,40> task{};task[0x20]=5;
 for(unsigned j=0;j<5;++j)task[0x21+j]=uint8_t(1u<<j);
 auto definition=pla::ReadPokedexTaskDefinition(task);assert(definition.valid);
 auto entry=no;entry.revealed=true;entry.species=37;entry.selected_form_revealed=true;entry.obtained_count=20;entry.raw_research_rate=120;
 auto progress=pla::ReadPokedexTaskProgress(entry,definition,0);
 assert(progress.valid && progress.current_available && progress.current==20 &&
        progress.reported_thresholds==0 && progress.achieved_thresholds==5 && progress.unreported==5);
 entry.reported_progress=7;progress=pla::ReadPokedexTaskProgress(entry,definition,0);
 assert(progress.valid && progress.reported_thresholds==6 && progress.achieved_thresholds==6 && !progress.unreported);
 entry.reported_progress=6;entry.obtained_count=1;
 progress=pla::ReadPokedexTaskProgress(entry,definition,0);
 assert(progress.valid && progress.achieved_thresholds==5 && progress.unreported==0);
 entry.reported_progress=0;definition.required_for_completion=true;
 assert(pla::PokedexReportedResearchRate(entry,std::span(&definition,1))==99);
 entry.reported_progress=2;
 assert(pla::PokedexReportedResearchRate(entry,std::span(&definition,1))==120);
 assert(!pla::PokedexReportedResearchRate(entry,{}).has_value());
 assert(!pla::ReadPokedexTaskProgress(no,definition,0).valid);
 assert(!pla::ReadPokedexTaskProgress(entry,definition,10).valid);
 assert(pla::PokedexEntryTextPermission(no)==pla::PokedexEntryTextKind::Unavailable);
 entry.raw_research_rate=99;entry.has_report=false;
 assert(pla::PokedexEntryTextPermission(entry)==pla::PokedexEntryTextKind::Generic);
 assert(pla::PokedexGenericCommentVariant(entry,permitted_form)==2);
 entry.obtained_count=0;entry.ever_updated=true;
 assert(pla::PokedexGenericCommentVariant(entry,permitted_form)==1);
 entry.ever_updated=false;
 assert(pla::PokedexGenericCommentVariant(entry,permitted_form)==0);
 assert(!pla::PokedexGenericCommentVariant(entry,{}).has_value());
 entry.has_report=true;
 assert(pla::PokedexEntryTextPermission(entry)==pla::PokedexEntryTextKind::Ongoing);
 assert(!pla::PokedexGenericCommentVariant(entry,permitted_form).has_value());
 entry.raw_research_rate=100;
 assert(pla::PokedexEntryTextPermission(entry)==pla::PokedexEntryTextKind::Full);
 entry.has_report=false;
 assert(pla::PokedexEntryTextPermission(entry)==pla::PokedexEntryTextKind::Full);
 for(unsigned type=25;type<=27;++type) {
  definition.type=type;progress=pla::ReadPokedexTaskProgress(entry,definition,0);
  assert(progress.valid && !progress.current_available);
 }
 task[0x20]=6;assert(!pla::ReadPokedexTaskDefinition(task).valid);
 task[0x20]=5;task[0x22]=1;assert(!pla::ReadPokedexTaskDefinition(task).valid);
 uint32_t seed=0x218706;
 for(unsigned i=0;i<512;++i) {
  seed=seed*1664525+1013904223;bad=i%2 ? taskbytes : forms;bad[seed%bad.size()]^=uint8_t(seed>>24);
  if(i%2)(void)pla::ReadPokedexResearchTable(bad);else(void)pla::ReadPokedexIndex(personal,bad);
 }
 char command;unsigned species,form;
 while(std::cin>>command>>species>>form) {
  if(command=='E') {
   const auto e=pla::ReadPokedexEntry(snapshot,index,species);assert(e.valid);
   std::cout<<e.revealed<<' '<<e.seen_in_wild<<' '<<e.obtained<<' '<<e.ever_updated<<' '<<e.has_report<<' '
    <<e.perfect<<' '<<e.species<<' '<<e.selected_form_revealed<<' '<<e.selected_form<<' '<<e.obtained_count<<' '
    <<e.raw_research_rate<<' '<<e.reported_progress;
   for(auto value:e.counters)std::cout<<' '<<value;
   std::cout<<' ';Hex(pla::PokedexSpeciesName(snapshot,index,species,names));
  } else if(command=='F') {
   const auto f=pla::ReadPokedexForm(snapshot,index,species,form);assert(f.valid);
   if(f.revealed)assert(f.species==species && f.form==form);else assert(!f.species && !f.form);
   std::cout<<f.revealed<<' '<<unsigned(f.seen_flags)<<' '<<unsigned(f.obtained_flags)<<' '<<unsigned(f.caught_flags);
  } else if(command=='S') {
   const auto s=pla::ReadPokedexSummary(snapshot,index,species);assert(s.valid);
   std::cout<<s.total<<' '<<s.revealed<<' '<<s.seen_in_wild<<' '<<s.obtained<<' '<<s.reported<<' '<<s.perfect;
   for(auto value:s.raw_level_counts)std::cout<<' '<<value;
  } else if(command=='V') {
   const auto p=pla::ReadPokedexNativeTaskProgress(snapshot,index,table,species,form);
   std::cout<<p.valid<<' '<<p.current_available<<' '<<p.current<<' '<<p.reported_thresholds<<' '
    <<p.achieved_thresholds<<' '<<p.unreported;
  } else if(command=='R') {
   const auto s=pla::ReadPokedexResearchSummary(snapshot,index,table,species);assert(s.valid);
   std::cout<<s.total<<' '<<s.completed;
   for(auto n:s.level_counts)std::cout<<' '<<n;
  } else if(command=='I') {
   const auto &s=index.species[species];std::cout<<s.hisui;
   for(auto local:s.local)std::cout<<' '<<local;
   for(const auto &f:s.forms)std::cout<<' '<<f.form<<' '<<f.storage_index;
  } else if(command=='T') {
   const auto &rows=table.species[species];std::cout<<rows.size();
   for(const auto &t:rows) {
    std::cout<<' '<<t.species<<' '<<t.native_type<<' '<<t.qualifier<<' '<<t.move<<' '<<t.move_type<<' '<<t.time_of_day;
    for(auto hash:t.hashes)std::cout<<' '<<hash;
    std::cout<<' '<<t.points_single<<' '<<t.points_bonus<<' '<<t.required_for_completion<<' '<<t.thresholds.size();
    for(auto threshold:t.thresholds)std::cout<<' '<<threshold;
   }
  } else if(command=='Q') {
   unsigned type,idx,slot;std::cin>>type>>idx>>slot;
   std::array<uint8_t,40> b{};b[0]=uint8_t(type);b[0x27]=uint8_t(idx);b[0x20]=5;
   for(unsigned j=0;j<5;++j)b[0x21+j]=uint8_t(1u<<j);
   const auto d=pla::ReadPokedexTaskDefinition(b);assert(d.valid);
   const auto e=pla::ReadPokedexEntry(snapshot,index,species);
   const auto p=pla::ReadPokedexTaskProgress(e,d,slot);
   std::cout<<p.valid<<' '<<p.current_available<<' '<<p.current<<' '<<p.reported_thresholds<<' '
    <<p.achieved_thresholds<<' '<<p.unreported;
  }
  std::cout<<'\n';
 }
}
'''
commands, static_expected = [], []
for species, (dex, locals_) in sorted(catalog.items()):
    commands.append(f'I {species} 0')
    static_expected.append(' '.join(map(str, [dex, *locals_, *(v for pair in forms[species] for v in pair)])))
for species in sorted(tasks):
    commands.append(f'T {species} 0')
    static_expected.append(' '.join(map(str, [len(tasks[species]), *(v for row in tasks[species] for v in row)])))
save_paths = [('F14', Path(save), False)]
for name in ('F02', 'F05'):
    path = os.environ.get('PLA_POKEDEX_' + name)
    if path:
        save_paths.append((name, Path(path), False))
if os.environ.get('PLA_POKEDEX_PAYLOAD'):
    save_paths.append(('PAYLOAD', Path(os.environ['PLA_POKEDEX_PAYLOAD']), True))
with tempfile.TemporaryDirectory(prefix='pla-pokedex-') as temporary:
    temporary = Path(temporary)
    (temporary / 'test.cpp').write_text(source)
    flags = ['-std=c++20', '-O2', '-Wall', '-Wextra']
    if os.environ.get('PLA_POKEDEX_SANITIZE'):
        flags += ['-O1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    include = Path(__file__).resolve().parents[2] / 'native/modules'
    subprocess.run([*shlex.split(os.environ.get('CXX', 'c++')), *flags, '-I', str(include), str(temporary / 'test.cpp'), '-o', str(temporary / 'test')], check=True)
    for label, path, payload in save_paths:
        if payload:
            data = path.read_bytes()
        else:
            blocks, valid = reader.decrypt_save(path.read_bytes())
            assert valid
            data = next(b.data for b in blocks if b.key == 0x02168706)
        assert len(data) == 124000
        (temporary / 'dex.bin').write_bytes(data)
        expected, request = list(static_expected), list(commands)
        entries = {}
        for species in sorted(catalog):
            at = 0x70 + 0x58 * species
            flags_, progress, rate, obtained = struct.unpack_from('<IIHH', data, at)
            states = {}
            for form, storage in forms[species]:
                seen, got, caught = struct.unpack_from('<BBB', data, 0x151a8 + 0x18 * storage + 4)
                states[form] = (seen, got, caught)
                request.append(f'F {species} {form}')
                expected.append(' '.join(map(str, [int(bool(seen or got)), seen, got, caught])))
            seen = any(x[0] for x in states.values())
            revealed = seen or any(x[1] for x in states.values()) or obtained > 0
            selected = data[at + 0x50]
            selected_revealed = bool(selected in states and any(states[selected][:2]))
            counters = list(struct.unpack_from('<29H', data, at + 0x10))
            values = ([1, int(seen), int(obtained > 0), int(bool(flags_ & 1)), int(bool(flags_ & 2)),
                       int(bool(flags_ & 4)), species, int(selected_revealed), selected if selected_revealed else 0,
                       obtained, rate, progress, *counters] if revealed else [0]*41)
            name = names[species].encode().hex() if revealed else ''
            request.append(f'E {species} 0')
            expected.append(' '.join(map(str, values)) + (' ' + name if name else ''))
            entries[species] = dict(revealed=bool(revealed), seen=seen, obtained=obtained > 0,
                                    reported=bool(flags_ & 2) if revealed else False,
                                    perfect=bool(flags_ & 4) if revealed else False, rate=rate if revealed else 0,
                                    count=obtained, progress=progress, counters=counters)
        for area in range(6):
            selected = [e for species, e in entries.items() if not area or catalog[species][1][area-1]]
            buckets = [sum(min(e['rate']//10, 10) == i for e in selected) for i in range(11)]
            request.append(f'S {area} 0')
            expected.append(' '.join(map(str, [len(selected), sum(e['revealed'] for e in selected),
                sum(e['seen'] for e in selected), sum(e['obtained'] for e in selected),
                sum(e['reported'] for e in selected), sum(e['perfect'] for e in selected), *buckets])))
        # Real native table order and independent per-type occurrence ordinals.
        corrected_rates = {}
        for species, records in tasks.items():
            e = entries[species]
            move_index = type_index = 0
            required_done = True
            for slot, record in enumerate(records):
                kind = record[1]
                idx = move_index if kind == 1 else type_index if kind == 2 else -1
                move_index += kind == 1
                type_index += kind == 2
                request.append(f'V {species} {slot}')
                if not e['revealed']:
                    expected.append('0 0 0 0 0 0')
                    continue
                reported = max((e['progress'] >> (3*slot)) & 7, 1)-1
                required_done &= not record[11] or reported >= 1
                available = kind <= 24
                current = e['count'] if kind == 0 else e['counters'][idx] if kind == 1 else e['counters'][4+idx] if kind == 2 else e['counters'][kind+4] if available else 0
                achieved = max(sum(current >= threshold for threshold in record[13:]), reported) if available else 0
                expected.append(' '.join(map(str, [1, int(available), current, reported, achieved, achieved-reported if available else 0])))
            corrected_rates[species] = 99 if e['rate'] >= 100 and not required_done else e['rate']
        for area in range(6):
            species_ids = [s for s in catalog if not area or catalog[s][1][area-1]]
            request.append(f'R {area} 0')
            rates = [corrected_rates[s] for s in species_ids]
            expected.append(' '.join(map(str, [len(rates), sum(r >= 100 for r in rates),
                *[sum(min(r//10,10) == n for r in rates) for n in range(11)]])))
        # All directly documented counter kinds and slots, independent raw offsets.
        species = next(s for s, e in entries.items() if e['revealed'] and (e['progress'] & 7) <= 6)
        reported = max(entries[species]['progress'] & 7, 1) - 1
        for kind in range(28):
            for idx in range(4 if kind == 1 else 3 if kind == 2 else 1):
                request.append(f'Q {species} 0 {kind} {idx} 0')
                current = entries[species]['count'] if kind == 0 else entries[species]['counters'][idx] if kind == 1 else entries[species]['counters'][4+idx] if kind == 2 else entries[species]['counters'][kind+4] if kind <= 24 else 0
                available = kind <= 24
                achieved = max(sum(current >= threshold for threshold in (1,2,4,8,16)), reported) if available else 0
                expected.append(' '.join(map(str, [1, int(available), current, reported, achieved, achieved-reported if available else 0])))
        actual = subprocess.check_output([str(temporary / 'test'), str(romfs), str(temporary / 'dex.bin')], input='\n'.join(request)+'\n', text=True).splitlines()
        assert len(actual) == len(expected), (label, len(actual), len(expected))
        for command, got, want in zip(request, actual, expected):
            assert got.strip() == want, (label, command, got, want)
        # Independent established save-summary counts remain separate from seen flags.
        if not payload:
            summary = reader.summarize(str(path))['zukan']
            assert sum(e['obtained'] for e in entries.values()) == summary['species_ids_with_num_obtained_gt0 (1..980)']
        print(label, 'decoded124000B;242species;317forms;1611rawtasks;bounds/no-leak pass;',
              'revealed', sum(e['revealed'] for e in entries.values()),
              'obtained', sum(e['obtained'] for e in entries.values()),
              'reported', sum(e['reported'] for e in entries.values()))
