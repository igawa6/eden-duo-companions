# SPDX-License-Identifier: GPL-3.0-or-later
"""Compact, world-placed room state images for the existing map overlay API."""
import math
import struct
import json
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter, ImageChops

def map_material(pixels, colour=(255,255,255,255), ambient=(46,28,11), diffuse=.83):
    """Effective lit map material, calibrated against native map interiors.

    mapModelLight enables light mask 4; col0 is a model colour, not the
    final framebuffer colour. Retain the texture and floor alpha separately.
    The diffuse gain and warm ambient term approximate that lit composite.
    """
    import numpy as np
    source=np.asarray(pixels,dtype='float64')
    out=source.copy()
    out[...,:3]=np.minimum(255,source[...,:3]*np.asarray(colour[:3])/255*diffuse+ambient)
    out[...,3]=source[...,3]*colour[3]/255
    out[out[...,3]==0]=0
    return np.rint(out).astype('uint8')


def door_material(colours_blob):
    """doorModelDisp uses animated col, whereas rooms use unanimated col0.

    Mover 2 interpolates col0/col1. Bake the captured HaveKey phase (30%
    col0) into this static atlas; Open/Locked have no second endpoint.
    """
    colours=id_colours(colours_blob)
    result=[]
    for unit in (16,17,18):
        at=next(8+i*0xa8 for i in range(colours_blob[5]) if colours_blob[8+i*0xa8+4]==unit)
        end=tuple(colours_blob[at+0x5c:at+0x60])
        colour=colours[unit]
        if any(end):colour=tuple(round(.3*a+.7*b) for a,b in zip(colour,end))
        result.append(tuple(map_material([[[255,255,252,255]]],colour,(53,32,11))[0,0,:3]))
    return result

def id_colours(blob):
    """Switch ID_DATA_V2: 0xA8-byte rows, colour bytes retain the GC +0x58 slot."""
    if blob[:4] != b'2.00' or len(blob) < 8+blob[5]*0xa8:
        raise ValueError('unsupported map colour units')
    return {blob[8+i*0xa8+4]:tuple(blob[8+i*0xa8+0x58:8+i*0xa8+0x5c])
            for i in range(blob[5]) if blob[8+i*0xa8+4] != 255}


AREA_FILES = ['map_obj1.dat'] + [f'map_obj{s}{c}.dat' for s, letters in
              ((2,'abc'),(3,'abcdef'),(4,'abcdefg'),(5,'abcdefgh')) for c in letters]


def area_files(include_stage4=False):
    # Keep native area numbers stable when the development-only stage is omitted.
    return [(area, name) for area, name in enumerate(AREA_FILES, 1)
            if include_stage4 or not name.startswith('map_obj4')]


def part_positions(blob):
    """getPartsPtr indexes the parts chain, independently of ModelDataHead.partsNo."""
    if len(blob)<0x98 or struct.unpack_from('<I',blob,0x5c)[0]!=0x20030818:
        raise ValueError('unsupported native mark model')
    head=struct.unpack_from('<Q',blob)[0]
    count=blob[0x29]
    if head<0x98 or head+count*16>len(blob):
        raise ValueError('native parts out of bounds')
    result=[]
    for i in range(count):
        _,parent,_,_,x,y,z=struct.unpack_from('<BBBBfff',blob,head+i*16)
        if parent!=255 or not all(math.isfinite(v) and abs(v)<2000000 for v in (x,y,z)):
            raise ValueError('unsupported native mark hierarchy/position')
        result.append((x,y,z))
    return result


def check_mark_bounds(mark, area, key):
    for value,lo,hi in zip((mark['x'],mark['y']),area['min'],area['max']):
        if not math.isfinite(value) or not lo<value<hi:
            raise ValueError(f'{key}: native mark out of bounds')


def prepare_marks(package, cmn_dir, decoder, art, areas, overlays, rooms, common_buf, common_entries, include_stage4=False, golden_dir=None):
    """Return native tables with generated render identities; never infer key state from art."""
    tables = json.loads(Path(__file__).with_name('map_legend_tables.json').read_text())
    if tables['elf_sha256'] != '7dd2ff716d167eb3bb1c829836880ec13d8a1fc36807d49870c2461f6a7576ff':
        raise ValueError('unrecognized map-state tables')
    door_colours = door_material(decoder.slice_entry(common_buf, common_entries[1]))
    model = decoder.slice_entry(common_buf, common_entries[12-4])
    vertices, triangles = model_triangles(model)
    doors, marks = [], []
    shadow=min((rgb[:3] for rgb in art.tex('3a000009',2).getdata() if rgb[3]==255),key=sum) if golden_dir else None
    for area_no, fname in area_files(include_stage4):
        buf, entries = decoder.load_area(cmn_dir/fname)
        anchors = (part_positions(decoder.slice_entry(buf, entries[0]))
                   if tables['doors'][str(area_no)] else [])
        for n, (part, angle, flag_type, flag, item) in enumerate(tables['doors'][str(area_no)]):
            if part < 0:
                continue  # native hidden attachment model, not a door to display
            if part >= len(anchors):
                raise ValueError(f'{fname}: door part {part} out of bounds')
            angle_radians=math.radians(angle)
            c,s=math.cos(angle_radians),math.sin(angle_radians)
            box,_=room_images([(x*c+z*s,y,-x*s+z*c) for x,y,z in vertices],[],.15,[(0,0,0)])
            if golden_dir:
                _, variants = door_images(vertices, triangles, angle, door_colours)
                for state, image in enumerate(variants, 1):
                    glyph=image.convert('RGBA')
                    outline=Image.new('RGBA',glyph.size,shadow+(255,))
                    outline.putalpha(glyph.getchannel('A').filter(ImageFilter.MaxFilter(5)))
                    outline.alpha_composite(glyph)
                    palette_image(outline).save(golden_dir/f'map-door-{angle}-{state}.png',optimize=True,compress_level=9)
            x, _, z = anchors[part]
            key = f'{area_no}.{n}'
            overlays.append(dict(src=f'module:re4/door/{angle}/1',
                src_bind=f're4.map.door.{key}.art',show_bind=f're4.map.door.{key}.show',
                x0=x+box[0],y0=-z+box[1],x1=x+box[2],y1=-z+box[3]))
            doors.append(dict(area=area_no,index=n,angle=angle,type=flag_type,flag=flag,item=item,
                              x=x,y=-z,box=[x+box[0],-z+box[1],x+box[2],-z+box[3]]))
        for kind, no, mask in (('save',10,tables['typewriter_mask']),('goal',6,tables['goal_model_mask'])):
            if not (mask>>area_no)&1:
                continue
            points = part_positions(decoder.slice_entry(buf, entries[no-4]))
            if kind == 'goal':
                count = len(tables['goals'][str(area_no)])
                if len(points) < count:
                    raise ValueError(f'{fname}: goal flag/model count mismatch')
                points = points[:count]  # extra model parts have no native selector
            for n, (x, _, z) in enumerate(points):
                marks.append(dict(area=area_no,index=n,kind=kind,x=x,y=-z))
    # Refit bounds around actual native marks without moving any room or player point.
    # The initial camera bounds are retained separately for Separate Ways.
    camera_bounds = {key:[*area['min'],*area['max']] for key,area in areas.items()}
    room_areas = {}
    for key, room in rooms:
        candidates = [int(a) for a, rows in tables['rooms'].items() if any(r[0]==room for r in rows)]
        area_no = (1 if key.startswith('village-') else min(a for a in candidates if a>=2))
        room_areas[key] = area_no
    for key, area in areas.items():
        area['icons'] = list(area['icons'])
        stage = key.split('-')[0]
        active_areas = ({6,7} if key in ('island-304','island-306') else {room_areas[key]})
        selected = [mark for mark in marks if mark['area'] in active_areas]
        boxes = [d['box'] for d in doors if d['area'] in active_areas]
        x0,y0=area['min'];x1,y1=area['max']
        for mark in selected:
            x0=min(x0,mark['x']-120);x1=max(x1,mark['x']+120)
            y0=min(y0,mark['y']-120);y1=max(y1,mark['y']+120)
        for a,b,c,d in boxes:
            x0=min(x0,a-120);y0=min(y0,b-120);x1=max(x1,c+120);y1=max(y1,d+120)
        area['min'],area['max']=[x0,y0],[x1,y1]
        for mark in selected:
            check_mark_bounds(mark,area,key)
            kind=mark['kind']
            area['icons'].append(dict(k=kind+'_location',i=kind,x=mark['x'],y=mark['y'],
                size=80 if kind=='save' else 152,collectible=False,
                show_bind=f're4.map.{kind}.{mark["area"]}.{mark["index"]}.show'))
        for mark in area['icons']:
            check_mark_bounds(mark,area,key)
        for door in doors:
            if door['area'] in active_areas:
                check_mark_bounds(door,area,key)
    tables.update(draw_doors=doors,draw_marks=marks,room_areas=room_areas,camera_bounds=camera_bounds)
    return tables


def native_goal_sprite(art):
    """Native ss_map goal group: core sphere/glow textures plus the map's rim."""
    import numpy as np
    canvas=Image.new('RGBA',(64,64))
    def layer(pack,index,size,colour,offset=(0,0)):
        image=art.tex(pack,index).resize((size,size),Image.Resampling.LANCZOS)
        pixels=np.asarray(image).copy()
        pixels[:,:,:3]=(pixels[:,:,:3].astype('uint16')*np.array(colour,dtype='uint16')//255).astype('uint8')
        result=Image.new('RGBA',(64,64));result.alpha_composite(Image.fromarray(pixels),
            ((64-size)//2+offset[0],(64-size)//2+offset[1]))
        return result
    canvas.alpha_composite(layer('3a000009',21,24,(255,255,255)))
    canvas.alpha_composite(layer('07000000',250,14,(95,0,0)))
    # Convert the native additive glow passes to RGBA over the native map brown.
    background=np.asarray([66,43,21],dtype='float32')/255
    for glow_layer in (layer('07000000',350,21,(255,0,0),(-1,-1)),
                       layer('07000000',355,64,(255,70,70))):
        glow=np.asarray(glow_layer).astype('float32')/255
        base=np.asarray(canvas).astype('float32')/255
        target=np.minimum(1,background*(1-base[:,:,3:4])+base[:,:,:3]*base[:,:,3:4]+glow[:,:,:3]*glow[:,:,3:4])
        alpha=np.maximum(base[:,:,3:4],np.maximum(0,(target-background)/(1-background)).max(axis=2,keepdims=True))
        rgb=np.divide(target-background*(1-alpha),alpha,out=np.zeros_like(target),where=alpha>0)
        canvas=Image.fromarray(np.clip(np.concatenate((rgb,alpha),axis=2)*255,0,255).astype('uint8'))
    # Static equivalent of the captured red effect phase; geometry and alpha stay native.
    pixels=np.asarray(canvas).copy()
    pixels[:,:,:3]=np.rint(pixels[:,:,:3]*np.asarray([1,.9,.69])).astype('uint8')
    return Image.fromarray(pixels)


def write_tables(path, tables, rooms, multi_rooms, include_stage4=False):
    active = lambda area: include_stage4 or not 11 <= int(area) <= 17
    lines = ['// SPDX-License-Identifier: GPL-3.0-or-later', '#pragma once',
             '#include <cstdint>', '#include <string_view>', 'namespace re4 {',
             'struct MapStateRoom { std::string_view key; uint16_t room; uint8_t area; bool multi; };',
             'inline constexpr MapStateRoom MapStateRooms[] = {']
    lines += [f'    {{"{key}", {room}, {tables["room_areas"][key]}, {str(key in multi_rooms).lower()}}},' for key,room in rooms if active(tables['room_areas'][key])]
    lines += ['};', 'struct MapRoomRule { uint8_t area; uint16_t room; uint8_t hide,open,sealed; };',
              'inline constexpr MapRoomRule MapRoomRules[] = {']
    for area, rows in tables['rooms'].items():
        if not active(area):continue
        lines += ['    {'+', '.join(map(str,(int(area),*row)))+'},' for row in rows]
    lines += ['};','struct MapSaveRoom { uint16_t room,index; };',
              'inline constexpr MapSaveRoom MapSaveRooms[] = {']
    lines += [f'    {{{r["room"]}, {r["record"]}}},' for r in tables['save_records'] if include_stage4 or r['room'] >> 8 != 4]
    lines += ['};',f'inline constexpr unsigned MapSaveCount={len(tables["save_records"])};',
              'struct MapDoorDef { uint8_t area,index,angle,type,flag,item; };',
              'inline constexpr MapDoorDef MapDoorDefs[] = {']
    lines += ['    {'+', '.join(str(d[k]) for k in ('area','index','angle','type','flag','item'))+'},'
              for d in tables['draw_doors'] if active(d['area'])]
    lines += ['};','struct MapMarkDef { uint8_t area,index; bool save; };',
              'inline constexpr MapMarkDef MapMarkDefs[] = {']
    lines += [f'    {{{m["area"]}, {m["index"]}, {str(m["kind"]=="save").lower()}}},'
              for m in tables['draw_marks'] if active(m['area'])]
    lines += ['};','struct MapGoalFlag { uint8_t area,index,flag; };',
              'inline constexpr MapGoalFlag MapGoalFlags[] = {']
    for area, flags in tables['goals'].items():
        if not active(area):continue
        lines += [f'    {{{area}, {n}, {flag}}},' for n, flag in enumerate(flags)]
    lines += ['};','}']
    path.write_text('\n'.join(lines)+'\n')


def model_triangles(blob, material=None):
    """Native Switch BIN expanded primitives (same format as native case models)."""
    if struct.unpack_from('<I', blob, 0x5c)[0] != 0x20030818:
        raise ValueError('unsupported door model version')
    count = struct.unpack_from('<H', blob, 0x58)[0]
    at = struct.unpack_from('<Q', blob, 0x48)[0]
    vertices = [struct.unpack_from('<3f', blob, at+i*12) for i in range(count)]
    off = struct.unpack_from('<Q', blob, 0x30)[0]
    triangles, base = [], 0
    for _ in range(struct.unpack_from('<H', blob, 0x2a)[0]):
        if material is not None and blob[off+12]!=material:
            raise ValueError('unsupported native model material')
        size = struct.unpack_from('<I', blob, off+24)[0]
        at = off+32
        runs = struct.unpack_from('<I', blob, at)[0]
        at += 4
        for _ in range(runs):
            mode, n = struct.unpack_from('<HH', blob, at)
            at += 4
            ids = list(range(base, base+n))
            base += n
            if mode == 6:
                triangles += [tuple(ids[i:i+3]) for i in range(n-2)]
            elif mode in (4, 5):
                triangles += [tuple(ids[i:i+3]) for i in range(0,n,3)]
            elif mode == 8:
                triangles += [t for i in range(0,n,4)
                              for t in ((ids[i],ids[i+1],ids[i+2]),(ids[i],ids[i+2],ids[i+3]))]
            else:
                raise ValueError(f'unsupported door primitive {mode}')
        off += 32+size
    if base != count or any(len(t)!=3 or max(t)>=count for t in triangles):
        raise ValueError('door vertex count mismatch')
    return vertices, triangles


def native_room_model(blob, scale=1):
    vertices, triangles = model_triangles(blob,0)
    if blob[0x29]!=1:
        raise ValueError('unsupported room model hierarchy')
    head=struct.unpack_from('<Q',blob)[0]
    _,parent,_,_,tx,ty,tz=struct.unpack_from('<BBBBfff',blob,head)
    if parent!=255:
        raise ValueError('unsupported room parent')
    uv_at=struct.unpack_from('<Q',blob,0x18)[0]
    uv=[struct.unpack_from('<2f',blob,uv_at+i*8) for i in range(len(vertices))]
    if not all(math.isfinite(c) for row in vertices+uv for c in row):
        raise ValueError('nonfinite native model coordinates')
    return dict(vertices=[(scale*(x+tx),scale*(y+ty),scale*(z+tz)) for x,y,z in vertices],
                triangles=triangles,uv=uv,floor=int(scale*ty/100+.5))


def palette_image(image):
    """Use a palette only when it represents every RGBA pixel exactly."""
    import numpy as np
    pixels=np.asarray(image)
    colours,indexes=np.unique(pixels.reshape(-1,4),axis=0,return_inverse=True)
    if len(colours)>256:return image
    out=Image.frombytes('P',image.size,indexes.astype('uint8').tobytes())
    out.putpalette(colours[:,:3].flatten().tolist()+[0]*(768-3*len(colours)))
    out.info['transparency']=bytes(colours[:,3])
    return out


def room_model_images(models, pixels_per_unit, texture, colours):
    """Rasterize the native visual BINs and their own map material, not collision masks."""
    import numpy as np
    all_vertices=[v for m in models for v in m['vertices']]
    box,_=room_images(all_vertices,[],pixels_per_unit,[(0,0,0)])
    x0,y0,x1,y1=box
    w,h=round((x1-x0)*pixels_per_unit),round((y1-y0)*pixels_per_unit)
    tex=np.asarray(texture.convert('RGBA'));th,tw=tex.shape[:2]
    layers=[]
    for model in models:
        pos=np.asarray(model['vertices']);uv=np.asarray(model['uv'])
        xy=np.column_stack(((pos[:,0]-x0)*pixels_per_unit,(pos[:,2]+y1)*pixels_per_unit))
        canvas=np.zeros((h,w,4),dtype=np.uint8);depth=np.full((h,w),-np.inf)
        for ids in model['triangles']:
            a,c,d=xy[list(ids)]
            den=(c[1]-d[1])*(a[0]-d[0])+(d[0]-c[0])*(a[1]-d[1])
            if abs(den)<1e-9:continue
            l=max(0,math.floor(min(a[0],c[0],d[0])));r=min(w,math.ceil(max(a[0],c[0],d[0])))
            t=max(0,math.floor(min(a[1],c[1],d[1])));b=min(h,math.ceil(max(a[1],c[1],d[1])))
            if l>=r or t>=b:continue
            yy,xx=np.mgrid[t:b,l:r];xx=xx+.5;yy=yy+.5
            wa=((c[1]-d[1])*(xx-d[0])+(d[0]-c[0])*(yy-d[1]))/den
            wb=((d[1]-a[1])*(xx-d[0])+(a[0]-d[0])*(yy-d[1]))/den
            wc=1-wa-wb
            height=wa*pos[ids[0],1]+wb*pos[ids[1],1]+wc*pos[ids[2],1]
            mask=(wa>=0)&(wb>=0)&(wc>=0)&(height>=depth[t:b,l:r])
            u=wa*uv[ids[0],0]+wb*uv[ids[1],0]+wc*uv[ids[2],0]
            v=wa*uv[ids[0],1]+wb*uv[ids[1],1]+wc*uv[ids[2],1]
            pixels=tex[np.clip((v*th).astype(int),0,th-1),np.clip((u*tw).astype(int),0,tw-1)]
            canvas[t:b,l:r][mask]=pixels[mask];depth[t:b,l:r][mask]=height[mask]
        layers.append((model['floor'],canvas))
    states={str(n):[colours[n] for _ in layers] for n in (1,2,3)}
    states['4']=[colours[0] if len(layers)==1 or floor==0 else colours[5+floor] for floor,_ in layers]
    if len(layers)>1:
        for current in range(6):
            states[f'4-f{current}']=[colours[0] if floor==current else colours[5+floor] for floor,_ in layers]
    result={}
    for state,tints in states.items():
        image=Image.new('RGBA',(w,h))
        for (_,pixels),colour in sorted(zip(layers,tints),key=lambda pair:pair[0][0]):
            rgba=map_material(pixels,colour)
            image.alpha_composite(Image.fromarray(rgba))
        result[state]=palette_image(image)
    return box,result,len(layers)>1


def door_images(vertices, triangles, degrees, colours):
    """Native common door mesh, rotated as doorModelInit rotates it."""
    angle = math.radians(degrees)
    c, s = math.cos(angle), math.sin(angle)
    vertices = [(x*c+z*s,y,-x*s+z*c) for x,y,z in vertices]
    box, _ = room_images(vertices, triangles, .15, [(0,0,0)])
    x0, y0, x1, y1 = box
    w, h = round((x1-x0)*.15), round((y1-y0)*.15)
    mask = Image.new('P', (w,h))
    draw = ImageDraw.Draw(mask)
    for tri in triangles:
        draw.polygon([((vertices[i][0]-x0)*.15,(y1+vertices[i][2])*.15) for i in tri],fill=1)
    images = []
    for colour in colours:
        image = mask.copy()
        image.putpalette([0,0,0,*colour]+[0]*762)
        image.info['transparency']=0
        images.append(image)
    return box, images


def room_images(vertices, polys, pixels_per_unit, colours):
    """Keep the placed hit mesh untouched; crop each mask to its own world box."""
    points = [(v[0], -v[2]) for v in vertices]
    if (not points or not all(math.isfinite(c) for p in points for c in p) or
            not math.isfinite(pixels_per_unit) or pixels_per_unit <= 0):
        raise ValueError('invalid room mask')
    pad = 4 / pixels_per_unit
    x0, y0 = (min(p[k] for p in points) - pad for k in (0, 1))
    x1, y1 = (max(p[k] for p in points) + pad for k in (0, 1))
    w, h = math.ceil((x1-x0)*pixels_per_unit), math.ceil((y1-y0)*pixels_per_unit)
    # Pixel edges and the overlay's world edges must describe exactly the same box.
    x1, y0 = x0+w/pixels_per_unit, y1-h/pixels_per_unit
    mask = Image.new('L', (w, h))
    draw = ImageDraw.Draw(mask)
    for poly in polys:
        if any(i < 0 or i >= len(points) for i in poly[:3]):
            raise ValueError('room triangle out of bounds')
        draw.polygon([((points[i][0]-x0)*pixels_per_unit,
                       (y1-points[i][1])*pixels_per_unit) for i in poly[:3]], fill=1)
    edge = ImageChops.subtract(mask.filter(ImageFilter.MaxFilter(5)), mask)
    # Index 0 is transparent; 1 is native fill; 2 is the existing pale wall.
    indexed = Image.frombytes('P', mask.size,
                             bytes(2 if e else m for m, e in zip(mask.tobytes(), edge.tobytes())))
    images = []
    for colour in colours:
        image = indexed.copy()
        wall = tuple(w*c//b for w,c,b in zip((216,184,148),colour,(157,120,89)))
        image.putpalette([0,0,0, *colour, *wall] + [0]*759)
        image.info['transparency'] = bytes((0,225,255))
        images.append(image)
    return (x0, y0, x1, y1), images
