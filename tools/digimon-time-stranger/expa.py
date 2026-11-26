# SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
# SPDX-License-Identifier: GPL-3.0-or-later
import struct
SZ={2:(4,4),3:(2,2),4:(1,1),5:(4,4),6:(8,8),7:(8,8),8:(8,8),10:(4,4)}
def layout(types,z10=False):
    off=0;cols=[];bit=None
    for t in types:
        if t==9:
            if bit is None or bit>=32:
                off=(off+3)//4*4;base=off;off+=4;bit=0
            cols.append((t,base,bit));bit+=1;continue
        bit=None
        al,sz=SZ.get(t,(4,4))
        if t==10 and z10: al,sz=1,0
        off=(off+al-1)//al*al;cols.append((t,off,None));off+=sz
    return cols
def parse(b):
    assert b[:4]==b'EXPA'
    nt=struct.unpack_from('<I',b,4)[0];p=8;tables=[]
    for _ in range(nt):
        nl=struct.unpack_from('<I',b,p)[0];p+=4;name=b[p:p+nl].split(b'\0')[0].decode('utf8','replace');p+=nl
        sc=struct.unpack_from('<I',b,p)[0];p+=4;types=list(struct.unpack_from('<%dI'%sc,b,p));p+=4*sc
        esize,ec=struct.unpack_from('<II',b,p);p+=8
        p=(p+7)//8*8;stride=(esize+7)//8*8
        tables.append(dict(name=name,types=types,esize=esize,count=ec,start=p,stride=stride));p+=stride*ec
    ci=b.find(b'CHNK',p-16 if p>=16 else 0)
    patches={}
    if ci>=0:
        n=struct.unpack_from('<I',b,ci+4)[0];q=ci+8
        for _ in range(n):
            off,sz=struct.unpack_from('<II',b,q);q+=8
            patches[off]=b[q:q+sz].split(b'\0')[0].decode('utf8','replace');q+=sz
    out={}
    for t in tables:
        cols=layout(t['types'])
        def end(c): 
            ty,o,bit=c[-1];return o+(0 if ty==10 else SZ.get(ty,(4,4))[1]) if ty!=9 else o+4
        if cols and end(cols)>t['esize']: cols=layout(t['types'],True)
        rows=[]
        for r in range(t['count']):
            base=t['start']+r*t['stride'];row=[]
            for ty,o,bit in cols:
                a=base+o
                if ty in (6,7,8): row.append(patches.get(a))
                elif ty==9: row.append((struct.unpack_from('<I',b,a)[0]>>bit)&1)
                elif ty==5: row.append(round(struct.unpack_from('<f',b,a)[0],3))
                elif ty==3: row.append(struct.unpack_from('<h',b,a)[0])
                elif ty==4: row.append(b[a])
                elif ty==10: row.append(struct.unpack_from('<i',b,a)[0] if not (cols and cols is not None and False) else None)
                else: row.append(struct.unpack_from('<i',b,a)[0])
            rows.append(row)
        out[t['name']]=rows
    return out
