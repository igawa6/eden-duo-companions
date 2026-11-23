"""Dump UE 5.6 reflection (classes, structs, enums) of chosen script packages from a live game through ffr_probe.
usage: ue_sdk.py <run dir> <out.txt> [package prefix ...]   (default prefix /Script/mucha.)
Layout of this build: UStruct SuperStruct +0x40, ChildProperties +0x50, PropertiesSize +0x58; FField class +0x8, Next +0x18,
name +0x20; FProperty ElementSize +0x30, PropertyFlags +0x38, Offset +0x44; typed refs at +0x70 / +0x78; UEnum Names TArray +0x40."""
import struct
import sys

from ue_lib import Run


def main():
    run, out = sys.argv[1], sys.argv[2]
    prefixes = tuple(sys.argv[3:]) or ('/Script/mucha.',)
    R = Run(run)
    p = R.p
    ffc = {}

    def fclass(a):
        if a not in ffc:
            ffc[a] = R.fname(p.read(a, 8))
        return ffc[a]

    def oname(a):
        r = R.by_addr.get(a)
        return r[3].split('.')[-1] if r else f'?{a:#x}'

    def ptype(f, b):
        t = fclass(struct.unpack_from('<Q', b, 8)[0])
        q70, q78 = struct.unpack_from('<QQ', b, 0x70)
        if t in ('StructProperty', 'ObjectProperty', 'WeakObjectProperty', 'SoftObjectProperty', 'LazyObjectProperty', 'InterfaceProperty'):
            return f'{t[:-8]}<{oname(q70)}>'
        if t in ('ClassProperty', 'SoftClassProperty'):
            return f'{t[:-8]}<{oname(q78)}>'
        if t == 'ByteProperty':
            return f'Byte<{oname(q70)}>' if q70 else 'Byte'
        if t == 'EnumProperty':
            return f'Enum<{oname(q78)}>'
        if t == 'BoolProperty':
            fs, bo, bm, fm = b[0x70:0x74]
            return f'Bool(byte+{bo} mask {bm:#x})'
        if t == 'ArrayProperty':
            ib = p.read(q78, 0x80)
            return f'Array<{ptype(q78, ib)}>' if ib else 'Array<?>'
        if t == 'SetProperty':
            ib = p.read(q70, 0x80)
            return f'Set<{ptype(q70, ib)}>' if ib else 'Set<?>'
        if t == 'MapProperty':
            kb, vb = p.read(q70, 0x80), p.read(q78, 0x80)
            return f'Map<{ptype(q70, kb) if kb else "?"}, {ptype(q78, vb) if vb else "?"}>'
        return t[:-8] if t.endswith('Property') else t

    with open(out, 'w') as o:
        for r in R.rows:
            if not r[3].startswith(prefixes):
                continue
            if r[2] in ('Class', 'ScriptStruct', 'BlueprintGeneratedClass', 'WidgetBlueprintGeneratedClass'):
                h = p.read(r[1], 0x60)
                if not h:
                    continue
                sup, child = struct.unpack_from('<Q', h, 0x40)[0], struct.unpack_from('<Q', h, 0x50)[0]
                size = struct.unpack_from('<i', h, 0x58)[0]
                o.write(f'\n{r[2]} {r[3]} : {oname(sup) if sup else "-"}  size {size:#x}\n')
                f, n = child, 0
                while f and n < 2000:
                    b = p.read(f, 0x80)
                    if not b:
                        break
                    off, es = struct.unpack_from('<i', b, 0x44)[0], struct.unpack_from('<i', b, 0x30)[0]
                    o.write(f'  +{off:#06x} [{es:#x}] {ptype(f, b)} {R.fname(b, 0x20)}\n')
                    f = struct.unpack_from('<Q', b, 0x18)[0]
                    n += 1
            elif r[2] == 'Enum':
                h = p.read(r[1], 0x50)
                data, num = struct.unpack_from('<Qi', h, 0x40)
                if not data or not 0 < num < 5000:
                    continue
                e = p.read(data, num * 16) or b''
                vals = [f'{R.fname(e, 16 * i).split("::")[-1]}={struct.unpack_from("<q", e, 16 * i + 8)[0]}' for i in range(len(e) // 16)]
                o.write(f'\nEnum {r[3]}: {", ".join(vals)}\n')
    print('written', out)


if __name__ == '__main__':
    main()
