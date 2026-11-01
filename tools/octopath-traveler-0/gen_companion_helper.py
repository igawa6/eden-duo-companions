#!/usr/bin/env python3
"""Build the pinned game-thread UI/stat/healing helper, or its heartbeat probe."""
import argparse
import hashlib
import importlib.util
import json
import subprocess
import tempfile
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('ot0_probe',Path(__file__).with_name('gen_heal_probe.py'))
probe=importlib.util.module_from_spec(spec);spec.loader.exec_module(probe)

def compile_body():
    with tempfile.TemporaryDirectory(prefix='ot0-companion-helper-') as tmp:
        tmp=Path(tmp);obj=tmp/'helper.o';elf=tmp/'helper.elf';binary=tmp/'helper.bin'
        script=tmp/'helper.ld'
        script.write_text('SECTIONS { . = 80; .text : { *(.text.entry) *(.text*) *(.rodata*) . = ALIGN(4); } /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) } }\n')
        subprocess.run(['clang','--target=aarch64-none-elf','-O2','-ffreestanding','-fno-builtin',
            '-fno-jump-tables','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables',
            '-Wall','-Wextra','-Werror','-c',str(ROOT/'native/guest/ot0_companion_tick.c'),'-o',str(obj)],check=True)
        heal=tmp/'heal.o'
        subprocess.run(['clang','--target=aarch64-none-elf','-O2','-ffreestanding','-fno-builtin',
            '-fno-jump-tables','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables',
            '-Wall','-Wextra','-Werror','-c',str(ROOT/'native/guest/ot0_heal_query.c'),'-o',str(heal)],check=True)
        subprocess.run(['ld.lld','-T',str(script),'--entry=ot0_companion_tick',str(obj),str(heal),'-o',str(elf)],check=True)
        symbols=subprocess.check_output(['llvm-nm','--defined-only',str(elf)],text=True)
        if not any(line.split()[-1]=='ot0_companion_tick' and int(line.split()[0],16)==80 for line in symbols.splitlines() if len(line.split())>=3):raise ValueError('helper entry moved')
        reloc=subprocess.check_output(['llvm-readelf','-r',str(elf)],text=True)
        if 'There are no relocations' not in reloc:raise ValueError('unresolved helper relocation')
        subprocess.run(['llvm-objcopy','-O','binary','--only-section=.text',str(elf),str(binary)],check=True)
        data=binary.read_bytes()
        if not data or len(data)%4 or len(data)>32768:raise ValueError('helper code bounds')
        return data

def generate(image,heartbeat_only=False):
    if hashlib.sha256(image).hexdigest()!=probe.IMAGE_SHA256 or image[probe.HOOK:probe.HOOK+4].hex()!='eb210094':
        raise ValueError('unsupported executable/hook pin')
    plan=probe.generate(None if heartbeat_only else compile_body())
    # The running application is the base program, not its update title ID.
    plan['title_id']='01005270232F2000'
    plan['mailbox_size']=4096
    return plan

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--verify-image',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--heartbeat-only',action='store_true')
    a=p.parse_args();plan=generate(a.verify_image.read_bytes(),a.heartbeat_only)
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(json.dumps(plan,indent=2)+'\n')
    print('Wrote',len(plan['guest_code']['bytes'])//2,'bytes; heartbeat only:',a.heartbeat_only)

if __name__=='__main__':main()
