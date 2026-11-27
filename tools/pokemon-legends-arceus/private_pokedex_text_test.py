#!/usr/bin/env python3
"""Opt-in native Pokedex text/schema fixtures. No guest access or imported assets.
PLA_ROMFS, PLA_SAVE_READER; optional PLA_SAVE / PLA_POKEDEX_F02/F05/PAYLOAD,
PLA_POKEDEX_TEXT_SANITIZE=1. Builds an isolated temporary read-only CLI.
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
romfs, reference = os.environ.get('PLA_ROMFS'), os.environ.get('PLA_SAVE_READER')
if not romfs or not reference:
    print('SKIP: set PLA_ROMFS and external PLA_SAVE_READER')
    sys.exit(77)
romfs = Path(romfs)
def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    sys.modules[name] = result
    spec.loader.exec_module(result)
    return result
save_reader = module('text_save_reference', reference)
text_reader = module('pokedex_text_reference', Path(reference).with_name('pla_text.py'))
metadata = module('text_metadata_reference', Path(reference).with_name('pla_metadata.py'))
def hash_label(label):
    value = 0xCBF29CE484222645
    for char in label.encode('ascii'):
        value = ((value ^ char) * 0x100000001B3) & ((1 << 64)-1)
    return value
def table(name):
    root = romfs / 'bin/message/English' / name
    labels, data = text_reader.read_tbl(str(root)+'.tbl'), text_reader.read_dat(str(root)+'.dat')
    assert len(labels) == len(data)+1
    assert all(hash_label(label) == value for label, value in labels)
    return {label: (row, value, data[row] if row < len(data) else None)
            for row, (label, value) in enumerate(labels)}
tables = {name: table(name) for name in ('common/zukan_comment_A', 'common/zukan_comment_B',
    'common/pokedex', 'common/zkn_type', 'common/wazaname', 'common/typename',
    'script/place_name', 'common/place_name_indirect')}
def plain(value):
    if value is None or any(token in value for token in ('[VAR ', '[~ ', '[WAIT ', '\\r', '\\c', '\\u')):
        return ''
    return value.replace('\\n', '\n')
def lookup(name, label):
    return plain(tables[name].get(label, (0, 0, None))[2])
personal = metadata.Flat(romfs / 'bin/pml/personal/personal_data_total.perbin')
art = [(personal.scalar(row, 0, '<H'), personal.scalar(row, 1, '<H'))
       for row in personal.tables(personal.root, 0)
       if personal.scalar(row, 2, '<B') and personal.scalar(row, 49, '<H')]
assert len(art) == len(set(art)) == 318
native_tasks = metadata.Flat(romfs / 'bin/appli/pokedex/res_table/pokedex_research_task_table.bin')
tasks = {}
for row in native_tasks.tables(native_tasks.root, 0):
    fields = [native_tasks.scalar(row, i, '<I') for i in range(6)]
    tasks.setdefault(fields[0], []).append(fields)
# Exact native hash map from retained binary trace, not arithmetic on DAT rows.
labels = ['msg_ui_pokedex_21_'+x for x in ('01','04','11','02','13','12','07','08','09','10')]
labels += [None]
labels += ['msg_ui_pokedex_21_'+x for x in ('28','31','15','03','14','16','17','18','19','25','26','27','30','29')]
assert len(labels) == 25
source = r'''
#include "pla_pokedex_text.h"
#include "pla_pokedex_page.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
std::vector<uint8_t> Read(const std::filesystem::path &p) {
 std::ifstream f(p,std::ios::binary);assert(f);return{std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};
}
struct Table {
 std::vector<uint8_t>d,t;
 pla::PokedexTextTable View()const{return{d,t};}
};
Table Load(const std::filesystem::path&r,const char*n){return{Read(r/(std::string(n)+".dat")),Read(r/(std::string(n)+".tbl"))};}
void Hex(const std::string&s){for(unsigned char c:s)std::cout<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(c);std::cout<<std::dec;}
void W16(std::vector<uint8_t>&b,size_t at,unsigned n){b[at]=uint8_t(n);b[at+1]=uint8_t(n>>8);}
void W32(std::vector<uint8_t>&b,size_t at,unsigned n){W16(b,at,n);W16(b,at+2,n>>16);}
Table Make(std::vector<std::vector<unsigned>>texts,std::vector<std::string>labels,const char*sentinel){
 Table o;const size_t section=16;size_t at=section+4+texts.size()*8;o.d.resize(at);
 W16(o.d,0,1);W16(o.d,2,texts.size());W32(o.d,12,section);
 for(unsigned row=0;row<texts.size();++row){auto units=texts[row];units.push_back(0);W32(o.d,section+4+row*8,at-section);W16(o.d,section+8+row*8,units.size());uint16_t key=uint16_t(0x7c89+row*0x2983);
  for(unsigned u:units){o.d.resize(at+2);W16(o.d,at,u^key);at+=2;key=uint16_t((key<<3)|(key>>13));}}
 labels.push_back(sentinel);o.t.resize(8);W32(o.t,0,0x42544841);W32(o.t,4,labels.size());
 for(const auto&label:labels){const auto at=o.t.size();o.t.resize(at+10+label.size()+1);const auto hash=pla::PokedexLabelHash(label);W32(o.t,at,uint32_t(hash));W32(o.t,at+4,uint32_t(hash>>32));W16(o.t,at+8,label.size()+1);std::copy(label.begin(),label.end(),o.t.begin()+at+10);}
 return o;
}
int main(int argc,char**argv){assert(argc==3);const std::filesystem::path r=argv[1];const auto m=r/"bin/message/English";
 const auto full=Load(m,"common/zukan_comment_A"),ongoing=Load(m,"common/zukan_comment_B"),ui=Load(m,"common/pokedex"),category=Load(m,"common/zkn_type"),moves=Load(m,"common/wazaname"),types=Load(m,"common/typename"),places=Load(m,"script/place_name"),indirect=Load(m,"common/place_name_indirect");
 const pla::PokedexEntryMessages entries{full.View(),ongoing.View(),ui.View()};
 const pla::PokedexTaskTextTables task_tables{ui.View(),moves.View(),types.View()};
 pla::PokedexEntry e;e.valid=e.revealed=e.ever_updated=true;e.species=25;e.raw_research_rate=100;
 pla::PokedexFormState f;f.valid=f.revealed=true;f.species=25;f.form=0;
 assert(!pla::PokedexEntryComment(e,f,entries).empty());
 auto hidden=e;hidden.revealed=false;assert(pla::PokedexEntryComment(hidden,f,entries).empty());
 auto wrong=f;wrong.species=26;assert(pla::PokedexEntryComment(e,wrong,entries).empty());
 wrong=f;wrong.revealed=false;assert(pla::PokedexEntryComment(e,wrong,entries).empty());
 assert(pla::PokedexAreaName(6,ui.View(),places.View()).empty());
 assert(pla::PokedexAreaName(1,ui.View(),indirect.View()).empty());
 assert(pla::PokedexUiLabelText(pla::PokedexUiLabel(99),ui.View()).empty());
 assert(pla::PokedexMessageLabel(full.View(),"msg_zukan_comment_A_max","msg_zukan_comment_A_max").empty());
 assert(pla::PokedexMessageLabel(full.View(),"ZKN_COMMENT_A_025_000","msg_zukan_comment_B_max").empty());
 assert(!pla::PokedexLabelRow({},"a","max").has_value());
 for(size_t n:{size_t(0),size_t(7),full.t.size()-1})assert(pla::PokedexMessageLabel({full.d,std::span(full.t).first(n)},"ZKN_COMMENT_A_025_000","msg_zukan_comment_A_max").empty());
 auto bad=full;bad.t[0]^=1;assert(pla::PokedexMessageLabel(bad.View(),"ZKN_COMMENT_A_025_000","msg_zukan_comment_A_max").empty());
 bad=full;W32(bad.t,4,8193);assert(pla::PokedexMessageLabel(bad.View(),"ZKN_COMMENT_A_025_000","msg_zukan_comment_A_max").empty());
 bad=full;W16(bad.t,16,0);assert(pla::PokedexMessageLabel(bad.View(),"ZKN_COMMENT_A_025_000","msg_zukan_comment_A_max").empty());
 bad=full;bad.t[18]='!';assert(pla::PokedexMessageLabel(bad.View(),"ZKN_COMMENT_A_025_000","msg_zukan_comment_A_max").empty());
 bad=full;bad.d[2]^=1;assert(pla::PokedexMessageLabel(bad.View(),"ZKN_COMMENT_A_025_000","msg_zukan_comment_A_max").empty());
 bad=full;W32(bad.d,12,0xffffffff);assert(pla::PokedexMessageLabel(bad.View(),"ZKN_COMMENT_A_025_000","msg_zukan_comment_A_max").empty());
 auto duplicate=Make({{'A'},{'B'}},{"same","same"},"max");assert(pla::PokedexMessageLabel(duplicate.View(),"same","max").empty());
 auto false_sentinel=Make({{'A'},{'B'}},{"max","wanted"},"max");assert(pla::PokedexMessageLabel(false_sentinel.View(),"wanted","max").empty());
 auto token=Make({{'X',16,2,0x0107,0,'Y'}},{"msg_ui_pokedex_21_04"},"msg_pokedex_max");
 assert(pla::PokedexMessageLabel(token.View(),"msg_ui_pokedex_21_04","msg_pokedex_max",true).empty());
 assert(pla::PokedexTaskWordMessage(token.View(),"msg_ui_pokedex_21_04",0x0107,"Ember")=="XEmberY");
 assert(pla::PokedexTaskWordMessage(token.View(),"msg_ui_pokedex_21_04",0x0103,"Fire").empty());
 assert(pla::PokedexTaskWordMessage(token.View(),"msg_ui_pokedex_21_04",0x0107,"\x10").empty());
 for(auto units:std::vector<std::vector<unsigned>>{{'X',16,2,0x0108,0},{'X',16,2,0x0107,1},{'X',16,3,0x0107,0,0},{'X',16,2,0x0107,0,16,2,0x0107,0},{'X'},{0xd800}}){auto u=Make({units},{"msg_ui_pokedex_21_04"},"msg_pokedex_max");assert(pla::PokedexTaskWordMessage(u.View(),"msg_ui_pokedex_21_04",0x0107,"Ember").empty());}
 auto unsupported=Make({{16,2,0xbdff,1}},{"ZKN_COMMENT_A_025_000"},"msg_zukan_comment_A_max");assert(pla::PokedexEntryComment(e,f,{unsupported.View(),ongoing.View(),ui.View()}).empty());
 pla::PokedexNativeTask task;task.species=25;task.native_type=0;
 assert(!pla::PokedexTaskLabel(e,task,task_tables).empty());
 task.species=26;assert(pla::PokedexTaskLabel(e,task,task_tables).empty());task.species=25;
 hidden=e;hidden.ever_updated=false;assert(pla::PokedexTaskLabel(hidden,task,task_tables).empty());
 for(unsigned t=25;t<29;++t){task.native_type=t;assert(pla::PokedexTaskLabel(e,task,task_tables).empty());}
 task.native_type=10;task.time_of_day=5;assert(pla::PokedexTaskLabel(e,task,task_tables).empty());
 uint32_t seed=0xabc194;
 for(unsigned i=0;i<256;++i){seed=seed*1664525+1013904223;bad=full;auto&b=i%2?bad.t:bad.d;b[seed%b.size()]^=uint8_t(seed>>24);(void)pla::PokedexEntryComment(e,f,{bad.View(),ongoing.View(),ui.View()});}
 const auto native_tasks=pla::ReadPokedexResearchTable(Read(r/"bin/appli/pokedex/res_table/pokedex_research_task_table.bin"));assert(native_tasks.valid);
 pla::PokedexSnapshot snapshot;pla::PokedexIndex index;
 if(std::string(argv[2])!="-"){snapshot=pla::ParsePokedexSnapshot(Read(argv[2]));assert(snapshot.valid);index=pla::ReadPokedexIndex(Read(r/"bin/pml/personal/personal_data_total.perbin"),Read(r/"bin/appli/pokedex/res_table/pokedex_form_storage_index_table.bin"));assert(index.valid);}
 if(snapshot.valid){
  pla::PokedexPageSnapshot page;page.data=snapshot;page.index=index;page.tasks=native_tasks;page.generation=7;
  assert(pla::ReadPokedexHeaderCounts(page,0).valid);
  for(unsigned area=1;area<=6;++area)assert(!pla::ReadPokedexHeaderCounts(page,area).valid);
  for(unsigned id=1;id<=905;++id)for(const auto&mapping:index.species[id].forms){
   page.species=id;page.form=mapping.form;page.area=0;page.tab=0;
   const auto entry=page.Entry();const auto state=page.Form();
   assert(page.Valid()==(entry.valid&&entry.revealed&&state.valid&&state.revealed));
   page.tab=3;assert(!page.Valid());page.tab=0;
   page.area=6;assert(!page.Valid());
   for(unsigned area=1;area<=5;++area){page.area=area;assert(!page.Valid());
    page.area_available[area]=true;
    assert(page.Valid()==(entry.valid&&entry.revealed&&state.valid&&state.revealed&&index.species[id].local[area-1]));
    page.area_available[area]=false;}
  }
  page.data={};assert(!page.Valid());assert(!pla::ReadPokedexHeaderCounts(page,0).valid);
 }
 char command;unsigned species,form,mode;
 while(std::cin>>command>>species>>form>>mode){e={};e.valid=e.revealed=e.ever_updated=true;e.species=species;f={};f.valid=f.revealed=true;f.species=species;f.form=form;
  if(command=='E'||command=='C'){e.raw_research_rate=mode==0?100:99;e.has_report=mode==1;e.obtained_count=mode==2?1:0;e.ever_updated=mode!=4;Hex(command=='E'?pla::PokedexEntryComment(e,f,entries):pla::PokedexCategoryName(e,f,category.View()));}
  else if(command=='A')Hex(pla::PokedexAreaName(species,ui.View(),places.View()));
  else if(command=='U')Hex(pla::PokedexUiLabelText(pla::PokedexUiLabel(species),ui.View()));
  else if(command=='T')Hex(pla::PokedexTaskLabel(e,native_tasks.species[species][form],task_tables));
  else if(command=='L'){e=pla::ReadPokedexEntry(snapshot,index,species);f=pla::ReadPokedexForm(snapshot,index,species,form);Hex(pla::PokedexEntryComment(e,f,entries));}
  else if(command=='Q'){e=pla::ReadPokedexEntry(snapshot,index,species);Hex(pla::PokedexTaskLabel(e,native_tasks.species[species][form],task_tables));}
  std::cout<<'\n';
 }
}
'''
def comment(species, form, mode):
    if mode < 2:
        kind = 'A' if mode == 0 else 'B'
        return lookup('common/zukan_comment_'+kind, f'ZKN_COMMENT_{kind}_{species:03}_{form:03}')
    return lookup('common/pokedex', f'msg_ui_pokedex_16_{2 if mode == 2 else 1 if mode == 3 else 0:02}')
def task_label(fields):
    species, kind, _, move, move_type, time = fields
    if kind > 24:
        return ''
    label = f'msg_ui_pokedex_21_{20+time:02}' if kind == 10 and time <= 4 else labels[kind]
    if label is None:
        return ''
    raw = tables['common/pokedex'][label][2]
    if kind == 1:
        word = lookup('common/wazaname', f'WAZANAME_{move:03}')
        if not word:
            return ''
        raw = raw.replace('[VAR 0107(0000)]', word)
    elif kind == 2:
        word = lookup('common/typename', f'TYPENAME_{move_type:03}')
        if not word:
            return ''
        raw = raw.replace('[VAR 0103(0000)]', word)
    return plain(raw)
commands, expected = [], []
for species, form in art:
    for mode in range(5):
        commands.append(f'E {species} {form} {mode}')
        expected.append(comment(species, form, mode).encode().hex())
    commands.append(f'C {species} {form} 0')
    label = f'ZKN_TYPE_{species:03}' + (f'_{form:03}' if form else '')
    expected.append(lookup('common/zkn_type', label).encode().hex())
for area in range(6):
    commands.append(f'A {area} 0 0')
    expected.append((lookup('common/pokedex', 'msg_ui_pokedex_04_02') if area == 0 else lookup('script/place_name', f'PLACENAME_AREA{area:02}')).encode().hex())
for i, label in enumerate(('02_01','02_04','15_04','15_02','15_03')):
    commands.append(f'U {i} 0 0')
    expected.append(lookup('common/pokedex', 'msg_ui_pokedex_'+label).encode().hex())
for species, rows in tasks.items():
    for slot, fields in enumerate(rows):
        commands.append(f'T {species} {slot} 0')
        expected.append(task_label(fields).encode().hex())
with tempfile.TemporaryDirectory(prefix='pla-pokedex-text-') as temporary:
    temporary = Path(temporary)
    (temporary/'test.cpp').write_text(source)
    include = Path(__file__).resolve().parents[2]/'native/modules'
    flags = ['-std=c++20','-O2','-Wall','-Wextra']
    if os.environ.get('PLA_POKEDEX_TEXT_SANITIZE'):
        flags += ['-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer']
    subprocess.run([*shlex.split(os.environ.get('CXX','c++')),*flags,'-I',str(include),str(temporary/'test.cpp'),'-o',str(temporary/'test')],check=True)
    actual = subprocess.check_output([str(temporary/'test'),str(romfs),'-'],input='\n'.join(commands)+'\n',text=True).splitlines()
    assert len(actual)==len(expected),(len(actual),len(expected))
    for command,got,want in zip(commands,actual,expected):
        assert got==want,(command,got,want)
    print('Native text: all318art forms A/B/generic/exact-category labels;1611tasks;UI/area hashes;bounds/sentinels/controls pass')
    primary = Path(reference).parent/'rawsrc/PokedexConstants8a.cs'
    constants = primary.read_text()
    def array(name):
        part = constants.split(name+' =>',1)[1].split('[',1)[1].split(']',1)[0]
        return [int(n,0) for n in re.findall(r'0x[0-9a-fA-F]+|\d+',part)]
    mapped = {}
    for packed,storage in zip(array('FormStorageIndexIds'),array('FormStorageIndexValues')):
        species,form = packed & 2047, packed >> 11
        if species:
            mapped.setdefault(species,[]).append((form,storage))
    payloads=[]
    for name,key in [('F14','PLA_SAVE'),('F02','PLA_POKEDEX_F02'),('F05','PLA_POKEDEX_F05')]:
        if os.environ.get(key):
            blocks,valid=save_reader.decrypt_save(Path(os.environ[key]).read_bytes());assert valid
            payloads.append((name,next(b.data for b in blocks if b.key==0x02168706)))
    if os.environ.get('PLA_POKEDEX_PAYLOAD'):
        payloads.append(('PAYLOAD',Path(os.environ['PLA_POKEDEX_PAYLOAD']).read_bytes()))
    for name,data in payloads:
        assert len(data)==124000
        (temporary/'dex.bin').write_bytes(data)
        request,wanted=[],[]
        for species,forms in mapped.items():
            at=0x70+species*0x58
            flags,_,rate,obtained=struct.unpack_from('<IIHH',data,at)
            seen={form:bool(data[0x151a8+storage*0x18+4] or data[0x151a8+storage*0x18+5]) for form,storage in forms}
            revealed=any(seen.values()) or obtained>0
            mode=0 if rate>=100 else 1 if flags&2 else 2 if obtained else 3 if flags&1 else 4
            for form,_ in forms:
                request.append(f'L {species} {form} 0')
                wanted.append(comment(species,form,mode).encode().hex() if revealed and seen[form] else '')
            for slot,fields in enumerate(tasks[species]):
                request.append(f'Q {species} {slot} 0')
                wanted.append(task_label(fields).encode().hex() if revealed and flags&1 else '')
        actual=subprocess.check_output([str(temporary/'test'),str(romfs),str(temporary/'dex.bin')],input='\n'.join(request)+'\n',text=True).splitlines()
        assert len(actual)==len(wanted)
        for command,got,want in zip(request,actual,wanted):assert got==want,(name,command,got,want)
        print(name,'actual snapshot species/form/task reveal projections match independent reference; no freshness assertion')
