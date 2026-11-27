#!/usr/bin/env python3
"""Opt-in pure Missions received-list tests, no guest access or copied assets.
Required PLA_ROMFS, PLA_SAVE_READER. Optional PLA_SAVE, PLA_MISSION_F02/F05.
PLA_MISSION_PAGE_SANITIZE=1 builds the temporary CLI with ASan/UBSan.
"""
import importlib.util
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
romfs, reference = os.environ.get('PLA_ROMFS'), os.environ.get('PLA_SAVE_READER')
if not romfs or not reference:
    print('SKIP: set PLA_ROMFS and external PLA_SAVE_READER')
    sys.exit(77)
romfs = Path(romfs)
sys.path.insert(0, str(Path(reference).parent))
def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    sys.modules[name] = result
    spec.loader.exec_module(result)
    return result
reader = module('mission_page_save_reference', reference)
missions = module('mission_page_definition_reference', Path(reference).with_name('pla_missions.py'))
texts = module('mission_page_text_reference', Path(reference).with_name('pla_text.py'))
definitions = missions.load_defs(str(romfs), 'main') + missions.load_defs(str(romfs), 'sub')
assert len(definitions) == 149
assert len({q['hash'] for q in definitions}) == 149
EMPTY, NONE = 0xCBF29CE484222645, 0xC0EA47549AB5F3D9
message_tables = {}
for name in ('questlist_main', 'questlist_sub', 'mssn'):
    base = romfs/'bin/message/English/common'/name
    labels, values = texts.read_tbl(str(base)+'.tbl'), texts.read_dat(str(base)+'.dat')
    assert len(labels) == len(values)+1
    message_tables[name] = {value: values[i] for i, (_, value) in enumerate(labels) if i < len(values)}
def plain(value):
    if value is None or '[' in value:
        return ''
    return value.replace('\\n', '\n')
def text_hex(value):
    return value.encode().hex() or '-'
source = r'''
#include "pla_mission_text.h"
#include <iomanip>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
std::vector<uint8_t> Read(const std::filesystem::path &p) {
 std::ifstream f(p,std::ios::binary);assert(f);return{std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};
}
struct Table { std::vector<uint8_t> d,t; pla::MissionTextTable View()const{return{d,t};} };
Table Load(const std::filesystem::path&r,const char*n){return{Read(r/(std::string(n)+".dat")),Read(r/(std::string(n)+".tbl"))};}
void Hex(const std::string&s){if(s.empty()){std::cout<<'-';return;}for(unsigned char c:s)std::cout<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(c);std::cout<<std::dec;}
void W16(std::vector<uint8_t>&b,size_t at,unsigned n){b[at]=uint8_t(n);b[at+1]=uint8_t(n>>8);}
void W32(std::vector<uint8_t>&b,size_t at,unsigned n){W16(b,at,n);W16(b,at+2,n>>16);}
int main(int argc,char**argv){assert(argc==2);const std::filesystem::path r=argv[1];
 auto main=pla::ParseMissionDefinitions(Read(r/"quest_list_main.bin"),false);
 auto sub=pla::ParseMissionDefinitions(Read(r/"quest_list_sub.bin"),true);
 assert(main.valid&&sub.valid);
 const auto messages=r.parent_path().parent_path()/"message/English/common";
 auto mt=Load(messages,"questlist_main"),st=Load(messages,"questlist_sub"),ui=Load(messages,"mssn");
 pla::MissionTextTables tables{mt.View(),st.View()};
 pla::MissionPageSnapshot s;s.available=true;s.generation=71;
 for(const auto*defs:{&main,&sub})for(const auto&q:defs->quests)
   s.observations.push_back({q.request,q.state_hash,true,255});
 auto model=pla::BuildMissionPage(main,sub,s);assert(model.available&&model.rows.size()==149&&!model.tracked_row);
 auto refuses=[&](const auto&a,const auto&b,const auto&v){auto m=pla::BuildMissionPage(a,b,v);assert(!m.available&&m.rows.empty()&&!m.tracked_row&&m.generation==0);};
 auto bad=s;bad.available=false;refuses(main,sub,bad);
 bad=s;bad.observations.push_back(bad.observations[0]);refuses(main,sub,bad);
 bad=s;bad.observations[0].request=true;refuses(main,sub,bad);
 bad=s;bad.observations[0].state_hash=pla::MissionTrackedNone;refuses(main,sub,bad);
 bad=s;bad.observations[0].state_hash^=uint64_t(1)<<63;refuses(main,sub,bad);
 bad=s;bad.observations[0].state_hash=bad.observations[1].state_hash;refuses(main,sub,bad);
 auto malformed=main;malformed.valid=false;refuses(malformed,sub,s);
 malformed=main;malformed.quests[0].request=true;refuses(malformed,sub,s);
 malformed=main;malformed.quests[0].title_hash=0;refuses(malformed,sub,s);
 malformed=main;malformed.quests[0].steps.clear();refuses(malformed,sub,s);
 malformed=main;malformed.quests[0].steps.push_back(malformed.quests[0].steps.back());refuses(malformed,sub,s);
 malformed=main;malformed.quests[0].state_hash=sub.quests[0].state_hash;refuses(malformed,sub,s);
 bad=s;bad.observations.clear();auto missing=pla::BuildMissionPage(main,sub,bad);
 assert(missing.available&&missing.rows.empty()&&missing.unavailable[0]==27&&missing.unavailable[1]==122&&!missing.tracked_row);
 // A later generation does not mutate previously owned rows.
 bad=s;bad.generation=72;auto later=pla::BuildMissionPage(main,sub,bad);
 assert(model.rows[0].generation==71&&later.rows[0].generation==72);
 const auto row=model.rows[0];
 auto empty=[&](const auto&v,const auto&snapshot,const auto&t){const auto x=pla::MissionRowText(v,main,sub,snapshot,t);assert(x.title.empty()&&x.summary.empty()&&x.purpose.empty());};
 assert(!pla::MissionRowText(row,main,sub,s,tables).title.empty());
 auto wrong=row;wrong.request=!row.request;empty(wrong,s,tables);
 wrong=row;wrong.state_hash=model.rows[1].state_hash;empty(wrong,s,tables);
 wrong=row;wrong.raw=0;empty(wrong,s,tables);
 wrong=row;wrong.raw=1;empty(wrong,s,tables);
 wrong=row;wrong.raw=2;empty(wrong,s,tables);
 wrong=row;++wrong.ordinal;empty(wrong,s,tables);
 wrong=row;++wrong.generation;empty(wrong,s,tables);
 wrong=row;wrong.status=pla::MissionReceivedStatus::Active;empty(wrong,s,tables);
 wrong=row;wrong.title_hash=model.rows[1].title_hash;empty(wrong,s,tables);
 wrong=row;wrong.summary_hash^=1;empty(wrong,s,tables);
 wrong=row;wrong.purpose_hash=row.title_hash;empty(wrong,s,tables);
 bad=s;bad.generation++;empty(row,bad,tables);
 bad=s;bad.available=false;empty(row,bad,tables);
 empty(row,s,pla::MissionTextTables{st.View(),mt.View()});
 auto broken=mt;
 for(size_t n:{size_t(0),size_t(7),mt.t.size()-1}){broken=mt;broken.t.resize(n);empty(row,s,pla::MissionTextTables{broken.View(),st.View()});}
 broken=mt;broken.t[0]^=1;empty(row,s,pla::MissionTextTables{broken.View(),st.View()});
 broken=mt;W32(broken.t,4,8193);empty(row,s,pla::MissionTextTables{broken.View(),st.View()});
 broken=mt;W16(broken.t,16,0);empty(row,s,pla::MissionTextTables{broken.View(),st.View()});
 broken=mt;broken.t[18]='!';empty(row,s,pla::MissionTextTables{broken.View(),st.View()});
 broken=mt;broken.t[8]^=1;empty(row,s,pla::MissionTextTables{broken.View(),st.View()});
 broken=mt;const size_t second=18+(unsigned(mt.t[16])|(unsigned(mt.t[17])<<8));
 const size_t third=second+10+(unsigned(mt.t[second+8])|(unsigned(mt.t[second+9])<<8));
 const std::vector<uint8_t> duplicate(mt.t.begin()+8,mt.t.begin()+second);
 broken.t.erase(broken.t.begin()+second,broken.t.begin()+third);
 broken.t.insert(broken.t.begin()+second,duplicate.begin(),duplicate.end());empty(row,s,pla::MissionTextTables{broken.View(),st.View()});
 broken=mt;broken.t.back()='X';empty(row,s,pla::MissionTextTables{broken.View(),st.View()});
 broken=mt;broken.d[2]^=1;empty(row,s,pla::MissionTextTables{broken.View(),st.View()});
 broken=mt;broken.d.resize(15);empty(row,s,pla::MissionTextTables{broken.View(),st.View()});
 broken=mt;W32(broken.d,12,0xffffffff);empty(row,s,pla::MissionTextTables{broken.View(),st.View()});
 assert(pla::mission_text_detail::Lookup(mt.View(),pla::mission_text_detail::Hash("msg_questlist_main_max"),"msg_questlist_main_max").empty());
 assert(pla::mission_text_detail::Lookup(mt.View(),0xCBF29CE484222645ULL,"msg_questlist_main_max").empty());
 assert(pla::MissionUiLabelText(pla::MissionUiLabel(99),ui.View()).empty());
 for(unsigned i=0;i<7;++i){Hex(pla::MissionUiLabelText(pla::MissionUiLabel(i),ui.View()));std::cout<<'\n';assert(pla::MissionUiLabelText(pla::MissionUiLabel(i),mt.View()).empty());}
 unsigned cases;std::cin>>cases;
 for(unsigned c=0;c<cases;++c){unsigned available,n;std::cin>>available>>s.generation>>s.tracked_hash>>n;s.available=available;s.observations.clear();
  for(unsigned j=0;j<n;++j){unsigned request,readable,raw;uint64_t hash;std::cin>>request>>hash>>readable>>raw;assert(raw<=255);s.observations.push_back({bool(request),hash,bool(readable),uint8_t(raw)});}
  auto m=pla::BuildMissionPage(main,sub,s);
  std::cout<<m.available<<' '<<m.generation<<' '<<m.unavailable[0]<<' '<<m.unavailable[1]<<' '<<(m.tracked_row?int(*m.tracked_row):-1)<<' '<<m.rows.size()<<'\n';
  for(const auto&v:m.rows){std::cout<<v.request<<' '<<v.state_hash<<' '<<v.generation<<' '<<unsigned(v.raw)<<' '<<v.ordinal<<' '<<unsigned(v.status)<<' '<<v.title_hash<<' '<<v.summary_hash<<' '<<v.purpose_hash<<' ';
   const auto t=pla::MissionRowText(v,main,sub,s,tables);Hex(t.title);std::cout<<' ';Hex(t.summary);std::cout<<' ';Hex(t.purpose);std::cout<<'\n';}
 }
 assert(std::cin);
}
'''
# Independent Python lookup uses full hashes and original vectors, never legacy
# classify(), whose Open state is deliberately outside this model.
cases = []
def observation(q, raw, readable=True):
    return (int(q['kind'] == 'request'), q['hash'], int(readable), raw)
for raw in range(256):
    cases.append((True, 1000+raw, NONE, [observation(q, raw) for q in definitions]))
for key in ('PLA_SAVE', 'PLA_MISSION_F02', 'PLA_MISSION_F05'):
    path = os.environ.get(key)
    if not path:
        continue
    blocks, valid = reader.decrypt_save(Path(path).read_bytes())
    assert valid
    by_hash = {b.key: b for b in blocks}
    obs = []
    for q in definitions:
        b = by_hash.get(q['hash'] & 0xFFFFFFFF)
        readable = b is not None and len(b.data) == 1
        obs.append(observation(q, b.data[0] if readable else 0, readable))
    tracked = by_hash[0x717DDAA3].data
    assert len(tracked) == 8
    cases.append((True, 2000+len(cases), int.from_bytes(tracked, 'little'), obs))
# Tracked empty/foreign/hidden/active/completed, unreadable and sparse snapshots.
base = [observation(q, 0) for q in definitions]
for q in definitions:
    active = next((s[0] for s in q['steps'] if 1 < s[0] < 255), None)
    if active is None:
        continue
    obs = list(base); index = definitions.index(q); obs[index] = observation(q, active)
    for tracked in (NONE, 0x123456789ABCDEF0, q['hash'], definitions[(index+1)%149]['hash']):
        cases.append((True, 3000+len(cases), tracked, list(reversed(obs))))
    break
cases.extend([(True, 4000, NONE, []),
              (True, 4001, NONE, [observation(q, 255, False) for q in definitions]),
              (False, 4002, definitions[0]['hash'], [observation(q, 255) for q in definitions]),
              (True, 4003, definitions[0]['hash'], [observation(q, 255) for q in definitions])])
def expected(case):
    available, generation, tracked, obs = case
    if not available:
        return (0, 0, 0, 0, -1, 0), []
    by_identity = {(request, value): (readable, raw) for request, value, readable, raw in obs}
    rows, missing, tracked_row = [], [0, 0], -1
    for q in definitions:
        request = int(q['kind'] == 'request')
        readable, raw = by_identity.get((request, q['hash']), (False, 0))
        if not readable:
            missing[request] += 1
            continue
        if raw <= 1:
            continue
        candidates = [(i, s) for i, s in enumerate(q['steps']) if s[0] == raw]
        if not candidates:
            missing[request] += 1
            continue
        assert len(candidates) == 1
        ordinal, (_, summary, purpose) = candidates[0]
        rows.append((request, q['hash'], generation, raw, ordinal, int(raw == 255),
                     q['name_hash'], 0 if summary == EMPTY else summary,
                     0 if raw == 255 or purpose == EMPTY else purpose))
        if tracked != NONE and tracked == q['hash']:
            tracked_row = len(rows)-1
    return (1, generation, *missing, tracked_row, len(rows)), rows
with tempfile.TemporaryDirectory(prefix='pla-mission-page-') as temp:
    cpp, exe = Path(temp)/'test.cpp', Path(temp)/'test'
    cpp.write_text(source)
    compiler = shlex.split(os.environ.get('CXX', 'c++'))
    flags = ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror']
    if os.environ.get('PLA_MISSION_PAGE_SANITIZE') == '1':
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
    repo = Path(__file__).resolve().parents[2]
    subprocess.run(compiler+flags+['-I', str(repo/'native/modules'), str(cpp), '-o', str(exe)], check=True)
    lines = [str(len(cases))]
    for available, generation, tracked, obs in cases:
        lines.append(f'{int(available)} {generation} {tracked} {len(obs)}')
        lines.extend(' '.join(map(str, row)) for row in obs)
    output = iter(subprocess.run([str(exe), str(romfs/'bin/event/event_progress')],
        input='\n'.join(lines)+'\n', text=True, capture_output=True, check=True).stdout.splitlines())
    for suffix in ('01_01','01_02','02_02','03_01','05_01','06_01','25_01'):
        h = 0xCBF29CE484222645
        for c in ('msg_ui_mssn_'+suffix).encode():
            h = ((h ^ c) * 0x100000001B3) & ((1 << 64)-1)
        assert next(output) == text_hex(plain(message_tables['mssn'][h]))
    for case in cases:
        header, rows = expected(case)
        assert tuple(map(int, next(output).split())) == header
        for row in rows:
            parts = next(output).split()
            assert tuple(map(int, parts[:9])) == row
            table = message_tables['questlist_sub' if row[0] else 'questlist_main']
            assert parts[9:] == [text_hex(plain(table.get(h))) for h in row[6:9]], (row[:6], parts[9:])
    assert next(output, None) is None
print(f'PASS: all 149 definitions × 256 raw states; {len(cases)} cases, provided saves, native text/UI, malformed identity/table/definition and no-leak checks')
