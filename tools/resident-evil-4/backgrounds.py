#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Lossless rectangular sharing for the approved canvas materials.

Keep native pixels in runtime recipes. Only geometry and image identities enter
layout metadata; even a flat native colour is a one-pixel module image.
"""

FRAME_BOXES=((0,0,981,70),(0,70,69,733),(916,70,981,733),(0,733,981,838))

def partition(box, cuts):
    result=[box]
    for cut in cuts:
        next_boxes=[]
        for x0,y0,x1,y1 in result:
            a,b,c,d=max(x0,cut[0]),max(y0,cut[1]),min(x1,cut[2]),min(y1,cut[3])
            if a>=c or b>=d:
                next_boxes.append((x0,y0,x1,y1));continue
            next_boxes.extend(v for v in ((x0,y0,x1,b),(x0,d,x1,y1),(x0,b,a,d),(c,b,x1,d),(a,b,c,d)) if v[0]<v[2] and v[1]<v[3])
        result=next_boxes
    return result

def prepare(images, recipe, capacities, layout):
    pieces={};layouts={};intern={}
    # Main route first; unvisited capacities and gray/Mercenaries never enlarge
    # an image used by that route.
    keys=['ui/field','ui/picker','ui/key-items','ui/settings','case/background/15',
          'ui/standby','ui/nav','ui/neutral','ui/standby-gray','ui/mercenaries',
          *[f'case/background/{c}' for c,r in capacities if c!=15]]
    for suffix in keys:
        source='module:re4/'+suffix
        im=images[source];cuts=[]
        if suffix=='ui/nav':
            cuts.extend((x,0,x+w,120) for x,w in ((0,366),(366,366),(732,368),(1100,140)))
            cuts.extend(((0,0,1100,32),(0,90,1100,120)))
        elif im.size==(1240,1080) and suffix not in ('ui/standby','ui/standby-gray','ui/picker'):
            # Labels/bullets vary; the upper/lower bands and gear do not.
            cuts.extend((x,960,x+w,1080) for x,w in ((0,366),(366,366),(732,368),(1100,140)))
            cuts.extend(((0,960,1100,992),(0,1050,1100,1080)))
            if suffix in ('ui/settings','ui/key-items') or suffix.startswith('case/background/'):
                cuts.append((600,0,1240,66))
        if suffix=='ui/field':
            cuts.append((6,114,987,952))
            cuts.extend((1022,38+239*n,1205,216+239*n) for n in range(4))
        if suffix=='ui/key-items':
            cuts.extend((39+307*n,y+32,274+307*n,y+148) for y in (116,287,506,677) for n in range(4))
        if suffix=='ui/picker':cuts.append((140,70,1100,920))
        if suffix.startswith('case/background/'):
            cols=int(suffix.rsplit('/',1)[1]);rows=dict(capacities)[cols]
            cell,gx,gy=layout(cols,rows)
            # Exact ImageDraw grid spans: lines replace RGBA, including alpha.
            for y in range(rows):
                for x in range(cols):
                    cuts.append((gx+x*cell+1,gy+y*cell+1,gx+(x+1)*cell,gy+(y+1)*cell))
            cuts.append((gx,gy,gx+cols*cell+1,gy+rows*cell+1))
        entries=[]
        for box in partition((0,0,*im.size),cuts):
            crop=im.crop(box)
            if not crop.getchannel('A').getbbox():continue
            # Flat spans retain the native/code recipe, never serialize colours.
            solid=all(a==b for a,b in crop.getextrema())
            sample=crop.crop((0,0,1,1)) if solid else crop
            sig=(source if suffix in ('ui/standby','ui/standby-gray','ui/mercenaries') else '',sample.size,sample.tobytes())
            if sig not in intern:
                key=source if suffix in ('ui/standby','ui/standby-gray','ui/mercenaries') and box==(0,0,1240,1080) else f'module:re4/bg/{len(intern)}'
                intern[sig]=key;pieces[key]=sample
                x,y,r,b=box
                recipe.outputs[key]=recipe.region(recipe.ref(im),(x,y,x+1,y+1) if solid else box)
            key=intern[sig];x,y,r,b=box
            entries.append(dict(src=key,rect=[x,y,r-x,b-y]))
        layouts[source]=entries
    return pieces,layouts

def rewrite(manifest, pages, layouts):
    def expand(widget, key):
        # Warm widgets still request compact sources with zero alpha at 1x1.
        warm=widget.get('color')=='#00FFFFFF'
        entries=layouts[key]
        if not warm:
            assert widget['rect']==([0,960,1240,120] if key.endswith('/nav') else [0,0,1240,1080]),widget
        result=[]
        for entry in entries:
            w={k:v for k,v in widget.items() if k not in ('src','empty_src','src_bind','rect')}
            rect=entry['rect'].copy()
            if not warm and key=='module:re4/ui/nav':rect[1]+=960
            w.update(type='image',src=entry['src'],rect=[0,0,1,1] if warm else rect,src_hold=False)
            result.append(w)
        return result
    for page in pages:
        widgets=[]
        for w in page['widgets']:
            key=w.get('src',w.get('empty_src'))
            # This opaque standby covers neutral in full. Retain nav above it.
            if page['id']=='dimmed' and key=='module:re4/ui/neutral':continue
            if key in layouts:widgets.extend(expand(w,key))
            elif w.get('src_bind')=='re4.case.art':
                for cols in (10,11,12,15):
                    gate=f'ui.case.background.{cols}'
                    manifest['derived'].append(dict(name=gate,expr=f"'re4.case.capacity.available' && 're4.case.width' == {cols}"))
                    widgets.extend(expand(dict(w,need_bind=gate),f'module:re4/case/background/{cols}'))
            else:widgets.append(w)
        page['widgets']=widgets
    preload=[]
    for key in manifest['preload']:
        if key=='module:re4/map/frame':preload.extend(f'module:re4/map/frame/{n}' for n in range(4))
        else:preload.extend(e['src'] for e in layouts[key]) if key in layouts else preload.append(key)
    manifest['preload']=list(dict.fromkeys(preload))
    for page in pages:
        widgets=[]
        for w in page['widgets']:
            if w.get('empty_src')=='module:re4/map/frame':
                for n,(x,y,r,b) in enumerate(FRAME_BOXES):
                    widgets.append(dict(w,empty_src=f'module:re4/map/frame/{n}',rect=[6+x,114+y,r-x,b-y]))
            else:widgets.append(w)
        page['widgets']=widgets
    # Do not submit duplicate fixed warming keys or keys already in boot preload.
    for page in pages:
        seen=set();widgets=[]
        for w in page['widgets']:
            if w.get('color')=='#00FFFFFF' and w.get('src'):
                signature=(w['src'],w.get('need_bind'))
                if w['src'] in manifest['preload'] or signature in seen:continue
                seen.add(signature)
            widgets.append(w)
        page['widgets']=widgets


def prepare_hp(images,recipe):
    """One immutable frame plus the fourteen newly lit, opaque ring wedges."""
    from PIL import ImageChops
    base=images['module:re4/ui/hp-0']
    result={'base':dict(src='module:re4/hp/base',rect=[0,0,*base.size])}
    recipe.outputs[result['base']['src']]=recipe.ref(base)
    for level in range(1,15):
        current=images[f'module:re4/ui/hp-{level}']
        previous=images[f'module:re4/ui/hp-{level-1}']
        # Ring changes are code-drawn colours; disable the recipe recorder while
        # deriving the binary geometry mask, then record native-preserving crops.
        recipe.active=False
        try:
            diff=ImageChops.difference(current,previous)
            channels=diff.split();mask=channels[0]
            for channel in channels[1:]:mask=ImageChops.lighter(mask,channel)
            mask=mask.point(lambda value:255 if value else 0)
            box=mask.getbbox();mask=mask.crop(box)
            assert all(alpha==255 for alpha,changed in zip(current.crop(box).getchannel('A').getdata(),mask.getdata()) if changed)
        finally:recipe.active=True
        wedge=current.crop(box);wedge.putalpha(mask)
        key=f'module:re4/hp/wedge/{level}'
        recipe.outputs[key]=recipe.ref(wedge)
        x,y,r,b=box;result[str(level)]=dict(src=key,rect=[x,y,r-x,b-y])
    return result


def rewrite_hp(manifest,pages,hp):
    for level in range(1,15):
        manifest['derived'].append(dict(name=f'ui.hp.segment.{level}',expr=f"'re4.hp.available' && 're4.hp.level' >= {level}"))
    for page in pages:
        widgets=[]
        for w in page['widgets']:
            if w.get('src_bind')!='re4.hp.art':widgets.append(w);continue
            for level,entry in hp.items():
                x,y,width,height=entry['rect']
                widgets.append(dict(type='image',src=entry['src'],rect=[w['rect'][0]+x,w['rect'][1]+y,width,height],
                                    need_bind='re4.hp.available' if level=='base' else f'ui.hp.segment.{level}',src_hold=False))
        page['widgets']=widgets
    manifest['preload']=[key for key in manifest['preload'] if not key.startswith('module:re4/ui/hp-')]
    at=manifest['preload'].index('module:re4/ui/equipped')
    manifest['preload'][at:at]=[entry['src'] for entry in hp.values()]
    manifest['sprite_map'].update({entry['src']:entry['src'] for entry in hp.values()})
