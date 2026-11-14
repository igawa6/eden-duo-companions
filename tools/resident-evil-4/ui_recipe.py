#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Record native-art composition recipes; constants contain only code-drawn masks.

Installed before importing the approved renderer. Texture leaves are RomFS reads;
all pixel-changing Pillow operations used by that renderer are recorded explicitly.
"""
import json
import os
from pathlib import Path
import zlib
from PIL import Image, ImageDraw, ImageChops

nodes=[]
_node_cache={}
outputs={}
sizes={}
active=False
original={}

def node(op,inputs,args):
    key=(op,tuple(inputs),tuple(args))
    if key in _node_cache:return _node_cache[key]
    nodes.append((op,inputs,args));_node_cache[key]=len(nodes)-1;return len(nodes)-1

def tag(im,n):
    global active
    im._ui_recipe=n
    sizes[n]=im.size
    capture=os.environ.get('RE4_RECIPE_CAPTURE')
    if capture and im.width*im.height<=500000:
        previous=active;active=False
        try:im.save(Path(capture)/(str(n)+'.png'))
        finally:active=previous
    return im

def ref(im):
    if hasattr(im,'_ui_recipe'):return im._ui_recipe
    # Only images created independently of any native texture reach this leaf.
    mode=im.mode
    if mode not in ('L','RGB','RGBA'):raise ValueError(mode)
    n=node('constant',[],[im.width,im.height,len(mode),zlib.compress(im.tobytes(),9).hex()])
    sizes[n]=im.size
    return n

def region(n, box):
    """Push an exact crop through page paints, leaving resampling coordinates intact."""
    op,ins,args=nodes[n]
    x,y,r,b=box
    if box==(0,0,*sizes[n]):return n
    if op=='copy':return region(ins[0],box)
    if op=='crop':return region(ins[0],(x+args[0],y+args[1],r+args[0],b+args[1]))
    if op=='constant':
        w,h,c,data=args
        image=Image.frombytes({1:'L',3:'RGB',4:'RGBA'}[c],(w,h),zlib.decompress(bytes.fromhex(data)))
        # Constants are code-drawn; native pixels never enter this branch.
        return ref(image.crop(box))
    pointwise={'convert','getchannel','putalpha','point','composite','blend',
               'multiply','invert','lighter','subtract','add','merge'}
    if op in pointwise:
        result=node(op,[region(i,box) for i in ins],args)
    elif op in ('paste','paint','alpha_composite'):
        sx,sy=(*args,0,0)[:2];sw,sh=sizes[ins[1]]
        a,c=max(x,sx),min(r,sx+sw);d,e=max(y,sy),min(b,sy+sh)
        base=region(ins[0],box)
        if a>=c or d>=e:return base
        cut=(a-sx,d-sy,c-sx,e-sy)
        result=node(op,[base,*[region(i,cut) for i in ins[1:]]],[a-x,d-y])
    else:
        result=node('crop',[n],list(box))
    sizes[result]=(r-x,b-y)
    return result

def source(im,pack,index):
    # Offline provenance control: changing every native RGB value must not change
    # serialized recipes or their code-drawn constant leaves.
    if os.environ.get('RE4_RECIPE_PROBE'):
        alpha=im.convert('RGBA').getchannel('A')
        im=Image.new('RGBA',im.size,(17,93,201));im.putalpha(alpha)
    return tag(im,node('texture',[],[int(pack,16),index]))

def install():
    global active
    active=True
    cls=Image.Image
    original_new=cls._new
    def checked_new(im,core):
        if active and hasattr(im,'_ui_recipe'):
            raise ValueError('Unrecorded native-image operation')
        return original_new(im,core)
    cls._new=checked_new
    def wrap(name,fn):
        old=getattr(cls,name);original[name]=old
        def call(im,*args,**kw):
            global active
            if not active:return old(im,*args,**kw)
            return fn(old,im,args,kw)
        setattr(cls,name,call)
    def unary(name,params):
        def run(old,im,args,kw):
            global active
            inp=ref(im) if hasattr(im,'_ui_recipe') else None
            active=False
            try:out=old(im,*args,**kw)
            finally:active=True
            if inp is not None:tag(out,node(name,[inp],params(im,args,kw)))
            return out
        wrap(name,run)
    unary('copy',lambda im,a,k:[])
    unary('crop',lambda im,a,k:list(a[0]))
    unary('resize',lambda im,a,k:[*a[0],int(a[1] if len(a)>1 else k.get('resample',Image.Resampling.BICUBIC))])
    unary('convert',lambda im,a,k:[len(a[0])])
    unary('getchannel',lambda im,a,k:[im.getbands().index(a[0]) if isinstance(a[0],str) else a[0]])
    unary('filter',lambda im,a,k:[a[0].name, getattr(a[0],'size',0),getattr(a[0],'radius',0)])
    unary('transform',lambda im,a,k:[*a[0],int(a[1]),*a[2],int(a[3] if len(a)>3 else k.get('resample',0))])
    def point(old,im,args,kw):
        inp=ref(im) if hasattr(im,'_ui_recipe') else None
        lut=args[0];lut=[lut(i) for i in range(256)] if callable(lut) else list(lut)
        global active
        active=False
        try:out=old(im,lut,**kw)
        finally:active=True
        if inp is not None:tag(out,node('point',[inp],lut))
        return out
    wrap('point',point)
    def split(old,im,args,kw):
        global active
        active=False
        try:out=old(im,*args,**kw)
        finally:active=True
        if hasattr(im,'_ui_recipe'):
            for i,part in enumerate(out):tag(part,node('getchannel',[ref(im)],[i]))
        return out
    wrap('split',split)
    def mutate(name):
        def run(old,im,args,kw):
            global active
            dependent=hasattr(im,'_ui_recipe') or any(hasattr(a,'_ui_recipe') for a in args if isinstance(a,cls))
            if dependent:
                inputs=[ref(im)];par=[]
                for arg in args:
                    if isinstance(arg,cls):inputs.append(ref(arg))
                    elif isinstance(arg,(list,tuple)):par.extend(arg)
                    elif isinstance(arg,int):par.append(arg)
                    elif arg is not None:raise ValueError((name,arg))
                if kw:raise ValueError((name,kw))
            active=False
            try:out=old(im,*args,**kw)
            finally:active=True
            if dependent:tag(im,node(name,inputs,par))
            return out
        wrap(name,run)
    for name in ('alpha_composite','paste','putalpha'):mutate(name)
    # Pillow thumbnail mutates via resize and a final __dict__ update. Explicitly record it.
    oldthumb=cls.thumbnail
    def thumbnail(im,size,resample=Image.Resampling.BICUBIC,reducing_gap=2.0):
        global active
        inp=ref(im) if hasattr(im,'_ui_recipe') else None
        active=False
        try:oldthumb(im,size,resample,reducing_gap)
        finally:active=True
        if inp is not None:tag(im,node('resize',[inp],[im.width,im.height,int(resample)]))
    cls.thumbnail=thumbnail
    def function(module,name,op):
        old=getattr(module,name)
        def call(*args,**kw):
            global active
            ims=[a for a in args if isinstance(a,cls)]
            if name=='merge':ims=list(args[1])
            dependent=any(hasattr(a,'_ui_recipe') for a in ims)
            inputs=[ref(a) for a in ims] if dependent else []
            active=False
            try:out=old(*args,**kw)
            finally:active=True
            if dependent:
                par=[len(args[0])] if name=='merge' else [args[-1]] if name=='blend' else []
                tag(out,node(op,inputs,par))
            return out
        setattr(module,name,call)
    for name in ('merge','blend','composite'):function(Image,name,name)
    for name in ('multiply','invert','lighter','subtract','add'):function(ImageChops,name,name)
    # ImageDraw replaces destination pixels, rather than alpha-compositing them.
    drawcls=ImageDraw.ImageDraw
    for name in ('rectangle','rounded_rectangle','line','ellipse','arc','pieslice','polygon','regular_polygon'):
        old=getattr(drawcls,name)
        def draw(self,*args,_old=old,**kw):
            global active
            im=self._image
            if not active or not hasattr(im,'_ui_recipe'):return _old(self,*args,**kw)
            inp=ref(im)
            active=False
            try:
                layer=Image.new(im.mode,im.size);d=ImageDraw.Draw(layer);_old(d,*args,**kw)
                mask=Image.new('L',im.size);md=ImageDraw.Draw(mask)
                # The approved renderer supplies drawing colours by keyword.
                mk=dict(kw)
                for key in ('fill','outline'):
                    if key in mk and mk[key] is not None:mk[key]=255
                _old(md,*args,**mk)
                result=_old(self,*args,**kw)
            finally:active=True
            box=mask.getbbox()
            if box:tag(im,node('paint',[inp,ref(layer.crop(box)),ref(mask.crop(box))],list(box[:2])))
            return result
        setattr(drawcls,name,draw)


def emit(path):
    # Only retain reachable nodes; texture pixels are never serialized.
    used=set()
    def visit(n):
        if n in used:return
        used.add(n)
        for i in nodes[n][1]:visit(i)
    for n in outputs.values():visit(n)
    order=sorted(used);remap={n:i for i,n in enumerate(order)}
    lines=['// SPDX-License-Identifier: GPL-3.0-or-later','// Generated composition instructions. Constant leaves are code-drawn only.','#pragma once','namespace re4::assets {']
    flat_inputs=[];flat_args=[];definitions=[];arg_offsets={}
    for n in order:
        op,ins,args=nodes[n];data=''
        if op=='constant':*args,data=args
        if op=='filter':
            kind,size,radius=args
            if kind not in ('MaxFilter','Max'):raise ValueError(('unsupported dynamic filter',args))
            op='maxfilter';args=[size]
        inputs=[remap[i] for i in ins]
        signature=tuple(args)
        if signature not in arg_offsets:
            arg_offsets[signature]=len(flat_args);flat_args.extend(args)
        definitions.append('{'+json.dumps(op)+', {UiInputs+'+str(len(flat_inputs))+','+str(len(inputs))+'}, {UiArgs+'+str(arg_offsets[signature])+','+str(len(args))+'},'+json.dumps(data)+'},')
        flat_inputs.extend(inputs)
    lines+=['inline constexpr unsigned UiInputs[] = {'+','.join(map(str,flat_inputs))+'};',
            'inline constexpr double UiArgs[] = {'+','.join(str(v).lower() for v in flat_args)+'};',
            'inline constexpr UiNode UiNodes[] = {']
    lines+=definitions
    lines+=['};','inline const std::pair<const char*,unsigned> UiOutputs[] = {']
    lines += ['{'+json.dumps(key)+','+str(remap[n])+'},' for key,n in sorted(outputs.items())]
    large=[str(remap[n]) for n in sorted(set(outputs.values())) if sizes[n][0]*sizes[n][1]*4>=2*1024*1024]
    lines+=['};',f'inline constexpr std::array<unsigned,{len(large)}> UiLargeOutputs = {{'+','.join(large)+'};','}']
    capture=os.environ.get('RE4_RECIPE_CAPTURE')
    if capture:(Path(capture)/'nodes.json').write_text(json.dumps(dict(nodes=nodes,remap=remap,outputs=outputs)))
    path.write_text('\n'.join(lines)+'\n')
