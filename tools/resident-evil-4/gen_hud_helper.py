#!/usr/bin/env python3
"""Generate RE4 1.0.2's pinned, companion-side format-2 guest helper.

Single hook (main+0x1EA7C, the native reticle-update tail branch, run every frame regardless
of menu/gameplay state) now drives a small combined trampoline that calls TWO independently
compiled C helpers in sequence: re4_hud_tick (ammo/HUD-hide, native/guest/re4_hud_tick.c) with
the mailbox base, then re4_equip_tick (direct tap-to-equip, native/guest/re4_equip_tick.c,
owner decision 2026-11-01) with mailbox base + RE4_EQUIP_MAILBOX_OFFSET -- "appended to that
same tick" per re4_equip_tick.c's own comment. The two C sources never call each other and
share no state; only the trampoline sequences them, and only because both run from the one
hook this build has. x0/x1 (main/mailbox) are caller-saved across a `bl`, so they are reloaded
before the second call.
"""
import argparse,hashlib,json,re,struct,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
BUILD='82C2A04A21D3E0B850F273D4149C9003'+'0'*32
ELF_SHA='7dd2ff716d167eb3bb1c829836880ec13d8a1fc36807d49870c2461f6a7576ff'
NSO_SHA='636f574c081594422a3d7eb00d94bbdcc4e7e63201fbb8ace834b9ea1eadb2fc'
HOOK=0x1EA7C
EQUIP_MAILBOX_OFFSET=256  # must match native/guest/re4_equip_abi.h's RE4_EQUIP_MAILBOX_OFFSET
TRAMP_LEN=60  # 15 words; body1 (re4_hud_tick) starts exactly here

def _check_equip_mailbox_offset():
    """The header is the single source of truth; this constant is a duplicate kept only
    because the trampoline's #EQUIP_MAILBOX_OFFSET<<10 literal (below) has to be a Python
    int at generation time. Catch any future edit to one side without the other instead of
    silently baking a stale offset into the trampoline.
    """
    header=(ROOT/'native/guest/re4_equip_abi.h').read_text()
    m=re.search(r'#define\s+RE4_EQUIP_MAILBOX_OFFSET\s+(\d+)u?\b',header)
    if not m or int(m.group(1))!=EQUIP_MAILBOX_OFFSET:
        raise ValueError('EQUIP_MAILBOX_OFFSET drifted from re4_equip_abi.h')

def _compile_body(src,entry):
    """Compile one freestanding guest .c file in isolation and extract its raw .text, with the
    same invariants the original single-helper script enforced: entry symbol at offset 0 (this
    file links each body on its own, unlike the trampoline, so 0 -- not 48 -- is the natural,
    equally-valid base; only the *relative* distance from entry to any internal branch like the
    static sane() helper matters, and that is unaffected by the chosen base) and zero leftover
    ELF relocations (every native call target is computed at runtime from the `main` parameter,
    never a compile-time symbol, so a real relocation surviving here means something unexpected
    crept in).
    """
    with tempfile.TemporaryDirectory(prefix='re4-helper-') as tmp:
        tmp=Path(tmp);obj=tmp/'helper.o';linked=tmp/'helper.elf';binary=tmp/'helper.bin';script=tmp/'helper.ld'
        script.write_text('SECTIONS { . = 0; .text : { *(.text.entry) *(.text*) *(.rodata*) . = ALIGN(4); } /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) } }\n')
        subprocess.run(['clang','--target=aarch64-none-elf','-O2','-ffreestanding','-fno-builtin','-fno-jump-tables','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-mgeneral-regs-only','-Wall','-Wextra','-Werror','-c',str(src),'-o',str(obj)],check=True)
        subprocess.run(['ld.lld','-T',str(script),f'--entry={entry}',str(obj),'-o',str(linked)],check=True)
        names=subprocess.check_output(['llvm-nm','--defined-only',str(linked)],text=True)
        if not any(len(p:=line.split())>=3 and p[-1]==entry and int(p[0],16)==0 for line in names.splitlines()):raise ValueError(f'{entry} entry moved')
        if 'There are no relocations' not in subprocess.check_output(['llvm-readelf','-r',str(linked)],text=True):raise ValueError(f'unresolved {entry} relocation')
        subprocess.run(['llvm-objcopy','-O','binary','--only-section=.text',str(linked),str(binary)],check=True)
        body=binary.read_bytes()
    if not body or len(body)%4:raise ValueError(f'{entry} size/alignment')
    return body

def _bl(from_off,to_off):
    delta=to_off-from_off
    if delta%4:raise ValueError('bl target misaligned')
    imm26=(delta//4)&0x3FFFFFF
    return 0x94000000|imm26

def generate(elf,nso):
    _check_equip_mailbox_offset()
    if hashlib.sha256(elf).hexdigest()!=ELF_SHA or hashlib.sha256(nso).hexdigest()!=NSO_SHA:raise ValueError('unsupported executable identity')
    if nso[:4]!=b'NSO0' or nso[0x40:0x60].hex().upper()!=BUILD:raise ValueError('unsupported build')
    phoff=struct.unpack_from('<Q',elf,0x20)[0];ents,num=struct.unpack_from('<HH',elf,0x36)
    expected=None
    for i in range(num):
        kind,flags,off,va,pa,filesz,memsz,align=struct.unpack_from('<IIQQQQQQ',elf,phoff+i*ents)
        if kind==1 and va<=0x800000000+HOOK<va+filesz:
            at=off+0x800000000+HOOK-va;expected=elf[at:at+4]
    if expected!=bytes.fromhex('91060014'):raise ValueError('native tail-branch pin mismatch')
    body1=_compile_body(ROOT/'native/guest/re4_hud_tick.c','re4_hud_tick')
    body2=_compile_body(ROOT/'native/guest/re4_equip_tick.c','re4_equip_tick')
    if len(body1)+len(body2)>8192:raise ValueError('helper size/alignment')
    t1=TRAMP_LEN
    t2=TRAMP_LEN+len(body1)
    if not 0<=EQUIP_MAILBOX_OFFSET<=0xFFF:raise ValueError('equip mailbox offset exceeds imm12')
    words=[
        0xA9BF7BFD,            # 0  stp x29, x30, [sp, #-16]!
        0x910003FD,            # 4  mov x29, sp
        0x90000000,            # 8  adrp x0, main        (reloc)
        0x91000000,            # 12 add x0, x0, :lo12:main (reloc)
        0x90000001,            # 16 adrp x1, mailbox     (reloc)
        0x91000021,            # 20 add x1, x1, :lo12:mailbox (reloc)
        _bl(24,t1),            # 24 bl re4_hud_tick
        0x90000000,            # 28 adrp x0, main        (reloc)
        0x91000000,            # 32 add x0, x0, :lo12:main (reloc)
        0x90000001,            # 36 adrp x1, mailbox     (reloc)
        0x91000021,            # 40 add x1, x1, :lo12:mailbox (reloc)
        0x91000021|(EQUIP_MAILBOX_OFFSET<<10), # 44 add x1, x1, #EQUIP_MAILBOX_OFFSET (literal, verified against llvm-mc: 256 -> 0x91040021)
        _bl(48,t2),            # 48 bl re4_equip_tick
        0xA8C17BFD,            # 52 ldp x29, x30, [sp], #16
        0xD65F03C0,            # 56 ret
    ]
    assert len(words)*4==TRAMP_LEN
    rel=[]
    for off,kind,target in [
        (8,'aarch64_adrp','main'),(12,'aarch64_add_lo12','main'),
        (16,'aarch64_adrp','mailbox'),(20,'aarch64_add_lo12','mailbox'),
        (28,'aarch64_adrp','main'),(32,'aarch64_add_lo12','main'),
        (36,'aarch64_adrp','mailbox'),(40,'aarch64_add_lo12','mailbox'),
    ]:rel.append(dict(offset=off,kind=kind,target=target,addend=0))
    segments=[dict(location=struct.unpack_from('<I',nso,o+4)[0],size=struct.unpack_from('<I',nso,o+8)[0]) for o in (0x10,0x20,0x30)]
    code=struct.pack(f'<{len(words)}I',*words)+body1+body2
    return dict(format=2,title_id='010099A00BC1E000',build_id=BUILD,module='main',layout=dict(segments=segments,bss_size=struct.unpack_from('<I',nso,0x3c)[0]),mailbox_size=4096,mailbox_epoch_offset=4,patches=[dict(offset=HOOK,expected=expected.hex(),replacement='00000014',relocations=[dict(offset=0,kind='aarch64_branch26',target='code',addend=0)])],payloads=[],guest_code=dict(bytes=code.hex(),relocations=rel))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--elf',type=Path,required=True);p.add_argument('--nso',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    plan=generate(a.elf.read_bytes(),a.nso.read_bytes());a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(plan,indent=2)+'\n');print('Generated',len(plan['guest_code']['bytes'])//2,'bytes:',a.output)
