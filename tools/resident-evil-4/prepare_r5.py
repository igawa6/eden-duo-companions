#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build approved r5 materials without any mock game values. Run after prepare_map/weapon_art."""
import argparse, importlib.util, json, sys, struct
from pathlib import Path
from prepare_item_names import decode as decode_item_names
from case_layout import CAPACITIES, ART_WIDTH, ART_HEIGHT, NAME_Y, TOP, HEIGHT, layout
from PIL import Image, ImageDraw, ImageEnhance, ImageFilter
sys.dont_write_bytecode = True
p=argparse.ArgumentParser(description=__doc__);p.add_argument('research',type=Path);a=p.parse_args()
root=Path(__file__).resolve().parents[2];pkg=root/'packages/ResidentEvil4/dualscreen';out=pkg/'assets'
import ui_recipe
sys.path.insert(0,str(a.research/'assets/tools'))
import pack_fen
_native_textures=pack_fen.iter_textures
def recipe_textures(path,*args,**kw):
 for index,im in _native_textures(path,*args,**kw):
  yield index,ui_recipe.source(im,Path(path).name.split('.')[0],index)
pack_fen.iter_textures=recipe_textures
ui_recipe.install()
spec=importlib.util.spec_from_file_location('approved',a.research/'design/mockups/redesign-r5/render.py');art=importlib.util.module_from_spec(spec);spec.loader.exec_module(art)
m=json.loads((pkg/'manifest.json').read_text())
# Rebuild, rather than layer r5 over old a2 widgets.
m['name']='Resident Evil 4 companion'
m['min_runtime']=25
m['font']='romfs:bio4/font/common_p.fnt'
m['font_atlas']='module:re4/font/atlas/{p}'
m['font_page_h']=576
m.pop('_status',None)
m['flags'].pop('weapons.page',None);m['flags'].pop('slots.assignment',None)
# Idempotency: strip every persist_flags entry this section re-adds below (old singular name,
# per-bucket names, motion.reduced) before re-appending -- a plain append would duplicate on
# every regen run since m['persist_flags'] is read back from the manifest already on disk.
m['persist_flags']=[f for f in m['persist_flags']
                     if f!='slots.assignment' and not f.startswith('slots.assignment.') and f!='motion.reduced']
# PER-SAVE SLOT ISOLATION: one persisted flag per bucket (re4.save.bucket, module-computed
# hash of {chapter,save_cnt,terminal_no}), not one global flag -- see
# native/modules/010099A00BC1E000.cpp's SaveBucket(). SAVE_BUCKETS must match SaveBuckets
# there exactly.
SAVE_BUCKETS=4
for bucket in range(SAVE_BUCKETS):
 m['flags'][f'slots.assignment.{bucket}']=-1
 m['persist_flags'].append(f'slots.assignment.{bucket}')
m['flags']['motion.reduced']=0;m['persist_flags'].append('motion.reduced')
m['actions']={k:v for k,v in m['actions'].items() if not k.startswith(('assign_','picker_','clear_','swap_'))}
m['actions'].pop('weapons_more',None);m['actions'].pop('open_files',None)
m['actions']['ashley_toggle']['enabled_bind']='re4.ashley_command.available'
m['actions']['toggle_motion_reduced']=dict(kind='flag',flag='motion.reduced')
m['flags']['opt.map_spoilers']=0
if 'opt.map_spoilers' not in m['persist_flags']:m['persist_flags'].append('opt.map_spoilers')
m['actions']['toggle_map_spoilers']=dict(kind='flag',flag='opt.map_spoilers')
# Motion pass: a quick, restrained fade (STYLE-SHEET.md "Motion cues" -- the game's own
# loading transition is "a soft fade", never a slide/bounce). 150 ms for a deliberate
# tab/button tap, matching MOTION-AND-HAPTICS.md's "page 150-300 ms" guidance.
for key in ('open_field','open_case','open_key_items','open_settings'):
 if key in m['actions']:m['actions'][key].update(transition='fade',duration_ms=150,easing='ease_out')
# Motion/haptics pass (MOTION-AND-HAPTICS.md 2026-10-25). `tap` stays 'off' at the top level:
# every on_tap widget already carries its own per-widget override (enabled/disabled variant
# selected by the live opt.haptics flag, built at the bottom of this script) which replaces
# it regardless. `hold`/`drag`/`drop` cannot be overridden per-widget (package format: "Widget
# and action haptic overrides do not apply to holds or swipes") so -- same known limitation
# already shipped in Link's Awakening/MK8D -- they do not respect the live opt.haptics toggle;
# `refused` is likewise global but every refusing tap in this package already has its own
# per-widget override (the equip-unsafe tap below uses 'reject' instead of 'light'), so the
# global default stays 'off' and is never actually reached.
m['haptics'].update(hold='heavy',drag='light',drop='confirm',refused='off')
m['anim_hz']=60
NAV=('Field','Case','Key Items')
# optimize=True + max compress_level is still plain lossless PNG DEFLATE -- same
# pixels, smaller file (verified via a pixel diff against the unoptimized output across every
# generated asset). Centralised here since every save() in this
# file goes through this one helper.
MAP_IMAGES={'map-backing':'module:re4/map/backing','map-unmapped':'module:re4/map/unmapped','r5-map-frame.png':'module:re4/map/frame','r5-legend.png':'module:re4/map/legend',
            **{f'r5-{s}-label.png':f'module:re4/map/label/{s}' for s in ('village','castle','island')}}
def runtime_key(name):
 if name.startswith('key-art-'):return 'module:re4/icon/'+name[8:-4]
 if name.startswith('r5-case-name-'):return 'module:re4/case/name/'+name[13:-4]
 if name.startswith('r5-case-') and name[8:-4].isdigit():return 'module:re4/case/background/'+name[8:-4]
 if name.startswith('r5-digit-'):return 'module:re4/digit/'+name[9:-4]
 if name.startswith('r5-hp-'):return 'module:re4/ui/hp-'+name[6:-4]
 if name in MAP_IMAGES:return MAP_IMAGES[name]
 if name.startswith('r5-') and not name.startswith(('r5-badge-','r5-case-select-','r5-case-ammo-badge','r5-slot-edit-','r5-picker-highlight')):return 'module:re4/ui/'+name[3:-4]
 return None
background_images={}
def save(cv,name):
 key=runtime_key(name)
 if key:
  if name in ('r5-field.png','r5-field-unmapped.png'):
   cv=cv.copy();ImageDraw.Draw(cv).rectangle((6,114,986,951),fill=(0,0,0,0))
  if name not in MAP_IMAGES and not name.startswith('key-art-'):
   ui_recipe.outputs[key]=ui_recipe.ref(cv)
   background_images[key]=cv.copy()
 else:cv.save(out/name,optimize=True,compress_level=9)
def image(src,rect,**kw):
 # Both Field shells have identical pixels; share one host image allocation.
 key='module:re4/ui/field' if src=='r5-field-unmapped.png' else runtime_key(src) or MAP_IMAGES.get(src)
 if src in MAP_IMAGES:return dict(type='image',empty_src=key,src_bind='re4.asset.blank',src_hold=False,rect=rect,**kw)
 if key:return dict(type='image',src=key,src_hold=False,rect=rect,**kw)
 return dict(type='image',src='file:assets/'+src,rect=rect,**kw)
def case_images():
 # Keep the legacy transform's pivot and sampling phase (notably M at 90 degrees).
 # A virtual source window places a compact cell raster in that coordinate system.
 # The 1/8 px slack survives float conversion before the host truncates to integers.
 # It maps to exactly (-left,-top,1080,720), with no padding image allocation.
 ws=[]
 for w,h in case_shapes:
  pw,ph=w*120,h*120;left,top=(ART_WIDTH-pw)//2,(ART_HEIGHT-ph)//2
  key=f're4.case.shape.{w}x{h}'
  ws.append(dict(type='image',src_bind=key+'.{i}.art',rect=[0,0,ART_WIDTH,ART_HEIGHT],
                 src_rect=[(-left-.125)/pw,(-top-.125)/ph,
                           (ART_WIDTH-left+.125)/pw,(ART_HEIGHT-top+.125)/ph],
                 x_bind=key+'.{i}.x',y_bind=key+'.{i}.y',rotate_bind=key+'.{i}.rotation',
                 scale_bind='re4.case.scale',repeat=384,repeat_bind=key+'.count',src_hold=False))
 return ws
def label(txt,rect,**kw):
 base=dict(type='label',text=txt,rect=rect,text_scale=13,color='#FFE5D9CF',outline='#FF000000',outline_px=2);base.update(kw);return base
def button(rect,action,**kw):
 base=dict(type='button',rect=rect,text='',on_tap=action,bg='#00000000',color='#00000000',border=0,haptic='light')
 base.update(kw);return base
def nav():
 return [button(r,act) for r,act in [([0,960,367,120],'open_field'),([367,960,367,120],'open_case'),([734,960,366,120],'open_key_items'),([1100,960,140,120],'open_settings')]]
def bg(active):
 cv=art.backdrop();art.navbar3(cv,active,NAV);return cv
# B4: Mercenaries gets its own 2-tab nav ("Mercenaries | Case"), not the generic 3-tab strip
# (navbar3's own column math, re-derived here only for the invisible hit-rects).
MERC_NAV=('Mercenaries','Case')
_merc_cw=(1240-140)//2
def merc_nav():
 return [button([0,960,_merc_cw,120],'open_mercenaries'),button([_merc_cw,960,_merc_cw,120],'open_case'),button([1100,960,140,120],'open_settings')]
m['actions']['open_mercenaries']=dict(kind='page',page='mercenaries',transition='fade',duration_ms=150,easing='ease_out')
# Field shell and native frame are separate so map content cannot paint over chrome.
# TOP_BAR_H was 86 (ring+pesetas only). Round4 fix (20261103-visual-fixes.md): the
# Wait/Follow key moves into this bar's empty right side and needs a >=88px-tall tap
# target, so the bar grows to 96 -- for BOTH Ashley-present/absent states alike, since
# this constant isn't branched on Ashley anywhere below (the "map never jumps" rule from
# the original r5 spec stays satisfied, just at a new fixed height).
TOP_BAR_H=96
# Everything below the bar (map, legend, stage-label plate) shifts down by the bar's
# growth so it stays flush against the frame's new top edge, exactly as before; the ring
# + pesetas (inside the bar) only need half that, to stay vertically centred in a taller
# bar instead of hugging its old bottom edge.
DY=TOP_BAR_H-86
BAR_DY=DY//2
cv=bg('Field');cv.alpha_composite(art.plate(981,TOP_BAR_H),(6,6));art.label(cv,'PTAS',118,19+BAR_DY,.62,art.PARCH)
field_shell=cv.copy()
box=(6,TOP_BAR_H+18,987,952);w,h=981,952-(TOP_BAR_H+18)
# Map art is independently generated by the native map decoder. These coordinates
# preserve its existing label plate and widget geometry without baking map pixels.
PLATE_NATIVE=(178,33,643,128)
_ps=w/2012
PLATE_BOX=(6+round(PLATE_NATIVE[0]*_ps),box[1]+round(PLATE_NATIVE[1]*_ps),
           6+round(PLATE_NATIVE[2]*_ps),box[1]+round(PLATE_NATIVE[3]*_ps))
PLATE_W,PLATE_H=PLATE_BOX[2]-PLATE_BOX[0],PLATE_BOX[3]-PLATE_BOX[1]
cv=field_shell
for n in range(4):art.draw_slot(cv,(999,7+239*n,1230,234+239*n),None)
save(cv,'r5-field.png')
save(cv,'r5-field-unmapped.png')
assert ui_recipe.outputs['module:re4/ui/field']==ui_recipe.outputs['module:re4/ui/field-unmapped']
for n in range(15):
 g=Image.new('RGBA',(88,88));art.gauge(g,44,44,36,n/14,'',1,.01);save(g,f'r5-hp-{n}.png')
# HUD number atlas, not the body-text font. Right-aligned live digits overlay
# a rounded dark native badge; no unverified reserve/capacity value is invented.
for digit in range(10):save(art.tex('2a000009',digit).resize((23,34),Image.Resampling.LANCZOS),f'r5-digit-{digit}.png')
for digits in range(1,6):
 b=Image.new('RGBA',(123,50));ImageDraw.Draw(b).rounded_rectangle((123-(digits*19+24),0,122,49),10,fill=(16,16,14,235));save(b,f'r5-badge-{digits}.png')
slot=Image.new('RGBA',(231,227));art.draw_slot(slot,(0,0,231,227),('item',None,None,None),True);save(slot,'r5-equipped.png')
# Wait/Follow key: moved out of the floating map bubble into the top bar's top-right
# (owner round4 fix, 20261103-visual-fixes.md) -- same 3 asset names (native/modules/
# re4_slots.h picks the file by `command`, untouched here), now a compact native-plate
# button sized for the bar instead of a 250x190 map-corner bubble. Only published when
# native escort is present, including unmapped areas -- unchanged.
WF_W,WF_H=216,88  # >=88px tall target; width fits 'Follow' (the wider word) plus padding
WF_RECT=[987-14-WF_W,6+(TOP_BAR_H-WF_H)//2,WF_W,WF_H]  # right-aligned in the bar, 14px inset
for state in ('wait','follow','both'):
 b=Image.new('RGBA',(WF_W,WF_H));art.key(b,(0,0,WF_W,WF_H),r=18)
 if state=='both':
  # Fallback for an unread Ashley command (native command==0): both words still shown
  # (owner's stated fallback), but smaller and stacked -- the compact bar button has no
  # room for two lines at the single-word scale.
  small=art.LABEL_SCALE*0.62
  art.label(b,'Wait',WF_W//2,WF_H*0.28-art.cap(small)//2,small,art.BONE,'c')
  art.label(b,'Follow',WF_W//2,WF_H*0.72-art.cap(small)//2,small,art.BONE,'c')
 else:art.label(b,state.title(),WF_W//2,WF_H//2-art.cap(art.LABEL_SCALE)//2,art.LABEL_SCALE,art.BONE,'c')
 save(b,'r5-'+state+'.png')
# Accurate 2+2 row key/treasure material, no sample inventory/name.
cv=bg('Key Items');art.topstrip(cv,'Keys & Treasures',False)
for y,idx in ((66,127),(456,120)):
 cv.alpha_composite(art.baked('39000009',idx,42),(16,y))
 for n in range(8):
  x=10+(n%4)*307;sy=y+50+(n//4)*171;art.draw_slot(cv,(x,sy,x+295,sy+159),None)
cv.alpha_composite(art.plate(1220,86),(10,866));save(cv,'r5-key-items.png')
# Exact native itemTexNo mapping; retain each sprite's aspect ratio in its window.
keymeta=json.loads((a.research/'analysis/20261101-r5-key-metadata.json').read_text())['rows']
for row in keymeta:
 im=art.tex('39000009',row['sprite']);im=im.crop(im.getbbox());im.thumbnail((245,125),Image.Resampling.LANCZOS)
 canvas=Image.new('RGBA',(245,125));canvas.alpha_composite(im,((245-im.width)//2,(125-im.height)//2));save(canvas,f"key-art-{row['id']:02x}.png")
# Case shell and capacity-specific grids; no invented placements.
for cols,rows in CAPACITIES:
 cell,gx,gy=layout(cols,rows);gw,gh=cols*cell,rows*cell
 cv=bg('Case');art.topstrip(cv,'Attache Case',False)
 cv.alpha_composite(art.metal(1224,HEIGHT+24,(165,162,152),1),(8,TOP-12));d=ImageDraw.Draw(cv);d.rectangle((gx-8,gy-8,gx+gw+8,gy+gh+8),fill=(75,78,73,255));d.rectangle((gx,gy,gx+gw,gy+gh),fill=(31,31,27,255))
 for c in range(cols+1):d.line((gx+c*cell,gy,gx+c*cell,gy+gh),fill=(90,90,76,200))
 for r in range(rows+1):d.line((gx,gy+r*cell,gx+gw,gy+r*cell),fill=(90,90,76,200))
 cv.alpha_composite(art.plate(1220,64),(10,NAME_Y))
 save(cv,f'r5-case-{cols}.png')
# Native shape metadata determines the selected rectangle, including rotated pieces.
rows=json.loads((a.research/'analysis/20261101-local-readers/piece-table.json').read_text())['rows']
case_shapes=sorted({(v[1],v[2]) for v in rows})
shapes=sorted(set(case_shapes)|{(h,w) for w,h in case_shapes})
for cols,rows in CAPACITIES:
 cell,_,_=layout(cols,rows)
 for sw,sh in shapes:
  selection=Image.new('RGBA',(sw*cell,sh*cell),(107,108,58,150));ImageDraw.Draw(selection).rectangle((0,0,selection.width-1,selection.height-1),outline=(216,212,160,255),width=3);save(selection,f'r5-case-select-{cell}-{sw}x{sh}.png')
# B3: small cream ammo badge for the selected/equipped grid cell (case.png mockup's "12/15" in
# the cell corner). Reuses the selected cell's own x/y bind, no new reader.
badge=Image.new('RGBA',(104,42));ImageDraw.Draw(badge).rounded_rectangle((0,0,103,41),9,fill=(230,224,205,235),outline=(40,32,20,220),width=2);save(badge,'r5-case-ammo-badge.png')
# The existing font adapter loads ASCII. Preserve native non-ASCII glyphs in the two
# herb names as prebuilt native-atlas labels; all other names use fit_text.
name_rows=decode_item_names(Path(art.ROMFS,'etc/core.udas').read_bytes())
from font_reference import build as build_font
font_atlas,font_data=build_font(a.research)
font_atlas=ui_recipe.source(font_atlas,'0f000001',0)
# Match the packaged adapter's existing resize before glyph crops.
font_atlas._ui_recipe=ui_recipe.node('resize',[font_atlas._ui_recipe],[1536,576,1])
def name_glyph(ch):
 idx=struct.unpack_from('<H',font_data,ord(ch)*2)[0]
 assert idx!=65535,ch
 return struct.unpack_from('<HHHBBbBH',font_data,131072+idx*12)
name_ascent=max(name_glyph(ch)[4]+name_glyph(ch)[6] for ch in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789')
for row in name_rows:
 if row['name'].isascii():continue
 mask=Image.new('L',(1112,58));pen=0;ratio=55/name_ascent
 for ch in row['name']:
  cp,x,y,w,h,bearing,pad,advance=name_glyph(ch)
  if w and h:
   g=font_atlas.crop((x,y,x+w,y+h)).getchannel('A').resize((round(w*ratio),round(h*ratio)),Image.Resampling.BILINEAR)
   mask.paste(g,(round(pen+bearing*ratio),round(pad*ratio)))
  pen+=(advance//16)*ratio
 assert pen<=1112
 cv=Image.new('RGBA',mask.size,(0,0,0));cv.putalpha(mask.filter(ImageFilter.MaxFilter(5)))
 ink=Image.new('RGBA',mask.size,(229,217,207));ink.putalpha(mask);cv.alpha_composite(ink)
 save(cv,f"r5-case-name-{row['id']:03x}.png")
# Picker rows are live, scrollable owned equippables. No game-menu writes.
cv=Image.new('RGBA',(1240,1080),(0,0,0,150));cv.alpha_composite(art.plate(960,850),(140,70));art.text(cv,'Assign Slot',180,100,1,art.BONE);save(cv,'r5-picker.png')
# Keep the original atlas scale and advance, rather than a host-label approximation.
PICKER_NUMBER_X=180+art.text_img('Assign Slot ',1,art.BONE).width
for n in range(4):
 save(art.text_img(str(n+1),1,art.BONE),f'r5-picker-number-{n+1}.png')
 edit=Image.new('RGBA',(1240,1080));ImageDraw.Draw(edit).rectangle((999,7+239*n,1230,234+239*n),outline=(232,154,42,255),width=5);save(edit.crop((999,7+239*n,1231,235+239*n)),f'r5-slot-edit-{n}.png')
selection=Image.new('RGBA',(870,108),(107,108,58,150));ImageDraw.Draw(selection).rectangle((0,0,869,107),outline=(216,212,160,255),width=3);save(selection,'r5-picker-highlight.png')
for n in range(4):
 m['actions'][f'picker_{n}']=dict(kind='page',page=f'picker_{n}',transition='fade',duration_ms=150)
 for bucket in range(SAVE_BUCKETS):
  m['actions'][f'clear_{bucket}_{n}']=dict(kind='flag',flag=f'slots.assignment.{bucket}',value=f'$re4.assign.{n}.clear')
  # Swap uses a module-supplied table for the current drag source; no guest state is written.
  m['actions'][f'swap_{bucket}_{n}']=dict(kind='flag',flag=f'slots.assignment.{bucket}',value=f'$ui.swap.{n}',enabled_bind='re4.slots.ready')
  for k in range(30):m['actions'][f'assign_{bucket}_{n}_{k}']=dict(kind='flag',flag=f'slots.assignment.{bucket}',value=f'$re4.assign.{n}.{k}',enabled_bind=f're4.picker.{k}.available')
# One derived condition per bucket, shared by every bucket-gated widget below (assign/clear/
# swap buttons and the three stage labels have no per-bucket meaning of their own, so this is
# the only bucket-specific derived set needed).
m['derived']=[dict(name=f'd.bucket.{b}',expr=f"'re4.save.bucket' == {b}") for b in range(SAVE_BUCKETS)]
def fade(widget_id,bind,ms=180):
 # Reduced motion: the derived gate is permanently 1 once the flag is set, so the widget's
 # own appearance/disappearance edge never arms the transition -- it just snaps, matching
 # MOTION-AND-HAPTICS.md's "transitions -> none/fade, value animations off" and the shipped
 # Pokemon Sword `d.anim.*` precedent this mirrors.
 name=f'd.anim.{widget_id}'
 quoted="!'"+bind[1:]+"'" if bind.startswith('!') else "'"+bind+"'"
 m['derived'].append(dict(name=name,expr=f"'@flag:motion.reduced' || {quoted}"))
 return {'bind':name,'from':'fade','ms':ms,'easing':'ease_out'}
def field():
 # A transparent request submits the native-gated current room before neighbours.
 # It paints no pixels; pending/stage changes remain blank (never held).
 ws=[dict(type='image',src_bind='re4.map.current.art',rect=[0,0,1,1],color='#00FFFFFF',src_hold=False,need_bind='re4.map.available'),image('r5-field.png',[0,0,1240,1080]),image('r5-field-unmapped.png',[0,0,1240,1080],need_bind='!re4.map.available',anim=fade('field_unmapped','!re4.map.available'))]
 ws += [dict(type='map',id='village-map',rect=[110,196+DY,810,738],room_bind='re4.map.area',need_bind='re4.map.solo',area_label=False,pan_zoom=True,min_zoom=.5,max_zoom=5,marker_x_bind='re4.map.x',marker_y_bind='re4.map.y',marker_rotate_bind='re4.map.heading',marker_src='module:re4/mark/arrow',marker_size=[48,48],marker_anchor=[.5,.5],bg='#00000000',**{f'view_rect_{v}_bind':f're4.camera.{v}' for v in ('x0','y0','x1','y1')})]
 map_widget=ws[-1]
 map_widget['overlays']=m['map']['state_overlays']
 # Escort variant now reclaims the full map height: the shorter 526px rect used to carve
 # out room for the Wait/Follow bubble in this bottom-right corner; the bubble moved into
 # the top bar (below), so nothing needs protecting here any more.
 ws.append(dict(map_widget,id='village-map-escort',rect=[110,196+DY,810,738],need_bind='re4.map.escort'))
 ws += [image('r5-map-frame.png',[0,0,1240,1080],need_bind='re4.map.available',anim=fade('map_frame','re4.map.available'))]
 for stage in ('village','castle','island'):
  ws += [image(f'r5-{stage}-label.png',[PLATE_BOX[0],PLATE_BOX[1],PLATE_W,PLATE_H],need_bind=f're4.map.{stage}',anim=fade(f'{stage}_label',f're4.map.{stage}'))]
 ws += [dict(type='image',src_bind='re4.hp.art',rect=[10,5+BAR_DY,88,88],need_bind='re4.hp.available'),dict(type='value',bind='re4.money',rect=[240,24+BAR_DY,275,58],text_scale=13,color='#FFE5D9CF',need_bind='re4.money.available')]
 ws += [label('Reserve',[550,4,205,40],need_bind='re4.ammo.reserve.available',text_scale=8),label('',[550,30,205,64],bind_text='re4.ammo.reserve.text',need_bind='re4.ammo.reserve.available',text_scale=14)]
 ws += [image('r5-legend.png',[747,124+DY,220,64],need_bind='re4.treasure.show',anim=fade('legend','re4.treasure.show'))]
 # Wait/Follow: moved from a floating map bubble (bottom-right corner) to the top bar's
 # top-right, previously-empty side (owner round4 fix, 20261103-visual-fixes.md). Same
 # src_bind/action/need_bind (only visible when Ashley is present) -- rect only changes.
 ws += [dict(type='image',src_bind='re4.ashley.bubble',rect=WF_RECT,need_bind='re4.ashley.escort',input_block=True,anim=fade('ashley_bubble','re4.ashley.escort')),button([WF_RECT[0],max(0,WF_RECT[1]-12),WF_W,112],'ashley_toggle',need_bind='re4.ashley.escort')]
 for n in range(4):
  k=f're4.slot.{n}';x,y=999,7+239*n
  # TAP-TO-EQUIP (direct, no native case-open round trip; owner decision 2026-11-01).
  # re4.equip.safe mirrors the guest helper's own re-checked guard exactly (never UI-only);
  # it cannot read true yet (Leon's r_no_0/1/2 offset unlocated, ANSWERS.md Q1), so every
  # present slot shows the SAME dimmed, textless art already used for "owned but not in the
  # case" (no new art, per the owner's explicit instruction) until that gap closes.
  ws += [image('r5-equipped.png',[x,y,231,227],need_bind=k+'.equipped'),
         dict(type='image',src_bind=k+'.art',rect=[x+34,y+44,163,163],need_bind=f'ui.slot.{n}.safe'),
         dict(type='image',src_bind=k+'.missing_art',rect=[x+34,y+44,163,163],need_bind=f'ui.slot.{n}.unsafe'),
         dict(type='image',src_bind=k+'.badge',rect=[x+82,y+165,123,50],need_bind=f'ui.slot.{n}.ammo.safe'),
         *[dict(type='image',src_bind=k+f'.digit.{d}',rect=[x+82,y+177,23,34],x_bind=k+f'.digit.{d}.x',need_bind=f'ui.slot.{n}.ammo.safe') for d in range(5)],
         dict(type='image',src_bind=k+'.missing_art',rect=[x+34,y+44,163,163],need_bind=k+'.missing'),
         label('Not in',[x+32,y+87,167,48],need_bind=k+'.missing',text_scale=7),label('case',[x+42,y+135,147,48],need_bind=k+'.missing',text_scale=7)]
  m['derived'] += [dict(name=f'ui.slot.{n}.ammo.safe',expr=f"'{k}.ammo.available' && 're4.equip.safe'"),
                    dict(name=f'ui.slot.{n}.safe',expr=f"'{k}.present' && 're4.equip.safe'"),
                    dict(name=f'ui.slot.{n}.unsafe',expr=f"'{k}.present' && !'re4.equip.safe'")]
  for bucket in range(SAVE_BUCKETS):
   gate=f'd.bucket.{bucket}'
   for filled in (True,False):
    action='equip_unavailable' if filled else f'picker_{n}'
    base_gate=k+('.present' if filled else '.not_present')
    combined=f'ui.slot.{n}.{bucket}.{"present" if filled else "absent"}'
    m['derived'].append(dict(name=combined,expr=f"'{base_gate}' && '{gate}'"))
    ws += [button([x,y,231,227],action,need_bind=combined,
                  on_hold=f'picker_{n}',hold_ms=550,select_group='weapon_slots',payload=n,
                  draggable=True,accept_group='weapon_slots',drop_action=f'swap_{bucket}_{n}',
                  **({'haptic':'reject'} if filled else {}))]
 ws.insert(1,image('map-backing',[0,0,1240,1080]))
 ws.insert(3,image('map-unmapped',[0,0,1240,1080],need_bind='!re4.map.available',anim=fade('map_unmapped','!re4.map.available')))
 return ws+nav()
m['actions']['case_select']=dict(kind='module',action='case_select',argument='$payload',enabled_bind='re4.inventory.positions.available')
m['actions']['equip_unavailable']=dict(kind='module',action='equip_unavailable',argument='$payload')
for category in (1,2):
 m['actions'][f'key_next_{category}']=dict(kind='module',action=f'key_next_{category}')
 for n in range(8):m['actions'][f'key_select_{category}_{n}']=dict(kind='module',action=f'key_select_{category}',argument=n)
pages=[]
for old in m['pages']:
 id=old['id']
 if id=='files' or id.startswith('picker_'):continue
 if id in ('field','unavailable','hud_only'):ws=field()
 elif id=='key_items':
  ws=[image('r5-key-items.png',[0,0,1240,1080])]
  for category,y in ((1,116),(2,506)):
   for n in range(8):
    x=10+(n%4)*307;sy=y+(n//4)*171;k=f're4.keys.{category}.{n}'
    ws += [dict(type='image',src_bind=k+'.art',rect=[x+26,sy+15,245,125],need_bind=k+'.available'),label('',[x+220,sy+110,65,45],bind_text=k+'.count',text_scale=8),button([x,sy,295,159],f'key_select_{category}_{n}',need_bind=k+'.available')]
   cx,cw=(900,150) if category==1 else (1050,170)
   ws += [button([cx,848,cw,112],f'key_next_{category}',need_bind=f're4.keys.{category}.more'),label('Keys' if category==1 else 'Treasures',[cx,880,cw,60],need_bind=f're4.keys.{category}.more',text_scale=8,fit_text=True,text_min_scale=7,wrap_width=cw,max_lines=1)]
  ws += [label('',[50,884,800,60],bind_text='re4.keys.name')]+nav()
 elif id=='case':ws=[dict(type='image',src_bind='re4.case.art',rect=[0,0,1240,1080],need_bind='re4.case.capacity.available'),*case_images(),button([20,104,1200,768],'case_select',payload='$tap'),label('',[64,NAME_Y+5,1112,58],text_scale=11,bind_text='re4.case.name',need_bind='re4.case.name.text.available',fit_text=True,text_min_scale=11,wrap_width=1112,max_lines=1),dict(type='image',src_bind='re4.case.name.art',rect=[64,NAME_Y+5,1112,58],need_bind='re4.case.name.image.available')]+nav()
 elif id=='mercenaries':
  # B4: approved a2 direction (Time / Combo+meter / Score / Previous best) restyled to r5
  # materials. Data is Group C's to publish later -- every live widget stays hide-never-dash
  # gated on its own .available bind, same convention as the rest of this file.
  ws=[image('r5-mercenaries.png',[0,0,1240,1080]),
      label('',[60,150,1120,150],bind_text='re4.merc.time',need_bind='re4.merc.time.available',text_scale=22,align='center'),
      dict(type='value',bind='re4.merc.combo',rect=[150,436,220,110],text_scale=22,color='#FFE5D9CF',need_bind='re4.merc.combo.available'),
      dict(type='bar',bind='re4.merc.combo_window',max_const=10,rect=[470,470,680,70],color='#FFD8A03A',bg='#FF18160C',need_bind='re4.merc.combo_window.available'),
      dict(type='value',bind='re4.merc.combo_window',rect=[970,558,180,50],suffix=' s',text_scale=9,color='#FFB4AEA0',align='right',need_bind='re4.merc.combo_window.available'),
      dict(type='value',bind='re4.merc.score',rect=[46,742,520,150],text_scale=20,color='#FFE5D9CF',need_bind='re4.merc.score.available'),
      dict(type='value',bind='re4.merc.best',rect=[660,742,520,150],text_scale=20,color='#FFE5D9CF',need_bind='re4.merc.best.available')]+merc_nav()
 elif id=='standby':
  # States sheet: Title/MainMenu/SaveSelect/Options/Loading/GameOver/Extras all share this page
  # id -- differentiate per-state with need_bind, same native standby.png collage as before.
  ws=[image('r5-standby.png',[0,0,1240,1080],need_bind='!d.state.gameover'),
      image('r5-standby-gray.png',[0,0,1240,1080],need_bind='d.state.gameover'),
      image('r5-loading-band.png',[0,910,1240,170],need_bind='d.state.loading')]
 elif id=='unsupported':
  ws=[image('r5-standby.png',[0,0,1240,1080]),image('r5-unsupported-band.png',[0,910,1240,170])]
 elif id=='dimmed':
  # Owner round4 fix (20261103-visual-fixes.md): this id (states 6/14/17/31 -- menus and
  # Pause without the case open) fell through to the generic `else` branch below, which
  # kept the OLD manifest's own leftover widget verbatim -- a raw 'standby.png' (native
  # 1280x720) stretched with fill:stretch into [0,0,1240,960], squashing it hard
  # horizontally. Use the same properly aspect-fit r5-standby.png the title/pause-adjacent
  # 'standby'/'unsupported' pages already use instead; nav() restores the bottom tab row
  # this id has always needed (unchanged from the old fallback's own +nav()).
  ws=[image('r5-standby.png',[0,0,1240,1080])]+nav()
 elif id=='settings':
  ws=[image('r5-settings.png',[0,0,1240,1080])]
  for y,title,flag in ((100,'Hide Leon HUD on top','hide_leon'),(220,'Hide Ashley HUD on top','hide_ashley'),(340,'Hide ammo HUD on top','hide_ammo'),(460,'Haptics','haptics')):
   ws += [dict(type='button',text=title,rect=[24,y,1196,120],on_tap='toggle_'+flag,text_scale=11,bg='#00000000',border=0,color='#FFE5D9CF'),dict(type='value',bind='@flag:opt.'+flag,rect=[1030,y+25,190,70],text_scale=11,color='#FFE5D9CF',names=['Off','On'])]
  ws += [dict(type='button',text='Reduced motion',rect=[24,580,1196,120],on_tap='toggle_motion_reduced',text_scale=11,bg='#00000000',border=0,color='#FFE5D9CF'),
         dict(type='value',bind='@flag:motion.reduced',rect=[1030,605,190,70],text_scale=11,color='#FFE5D9CF',names=['Off','On'])]
  ws += [dict(type='button',text='Show treasures & medallions',rect=[24,700,1196,120],on_tap='toggle_map_spoilers',text_scale=11,bg='#00000000',border=0,color='#FFE5D9CF'),
         dict(type='value',bind='@flag:opt.map_spoilers',rect=[1030,725,190,70],text_scale=11,color='#FFE5D9CF',names=['Off','On']),
         label('Spoilers: without map or request note',[60,822,1000,40],text_scale=8),
         label('',[60,872,1000,45],bind_text='re4.hud_hide.status',text_scale=9)]+nav()
 else:
  ws=[w for w in old['widgets'] if not w.get('on_tap','').startswith('open_') and w.get('text') not in ('-','- / -') and w.get('src') not in ('file:assets/r5-neutral.png','file:assets/r5-nav.png','module:re4/ui/neutral','module:re4/ui/nav')]+nav()
 if id=='case':
  for cols,rows in CAPACITIES:
   cell,_,_=layout(cols,rows)
   ws[1:1]=[image(f'r5-case-select-{cell}-{sw}x{sh}.png',[0,0,sw*cell,sh*cell],x_bind='re4.case.selection.x',y_bind='re4.case.selection.y',need_bind=f'ui.case.shape.{cols}.{sw}x{sh}') for sw,sh in shapes]
  ws += [image('r5-case-ammo-badge.png',[0,0,104,42],x_bind='re4.case.badge.x',y_bind='re4.case.badge.y',need_bind='re4.case.ammo.available'),
         label('',[0,4,104,34],bind_text='re4.case.ammo',x_bind='re4.case.badge.x',y_bind='re4.case.badge.y',need_bind='re4.case.ammo.available',text_scale=8,color='#FF241A10',outline='#00000000',outline_px=0,align='center')]
 if id not in ('field','unavailable','hud_only','case','key_items','settings','mercenaries','standby','unsupported'):
  ws=[w for w in ws if not (w['type']=='image' and 'nav' in w.get('src',''))]
  ws.insert(0,image('r5-neutral.png',[0,0,1240,1080]))
  ws.append(image('r5-nav.png',[0,960,1240,120]))
 if id in ('case','key_items','settings'):
  # B1: "PTAS" small-caps suffix immediately after the digits (case.png/keyitems.png mockup),
  # mirroring the baked "PTAS" prefix Field already has. Value right-aligns to a fixed edge so
  # the suffix -- anchored right after that edge -- never drifts with digit count.
  ws += [dict(type='value',bind='re4.money',rect=[760,12,274,58],text_scale=13,color='#FFE5D9CF',need_bind='re4.money.available',align='right'),
         label('PTAS',[1034,34,160,36],need_bind='re4.money.available',text_scale=8)]
 pages.append(dict(id=id,title=id,widgets=ws))
neutral=bg('');save(neutral,'r5-neutral.png');save(neutral.crop((0,960,1240,1080)),'r5-nav.png')
cv=bg('');art.topstrip(cv,'Settings',False);cv.alpha_composite(art.plate(1228,886),(6,66));save(cv,'r5-settings.png')
# B4: Mercenaries background -- a2 direction restyled to r5 materials, dedicated 2-tab nav.
cv=art.backdrop();art.topstrip(cv,'Mercenaries',False)
cv.alpha_composite(art.plate(1220,270),(10,70));art.label(cv,'Time',46,96,.95,art.BONE)
cv.alpha_composite(art.plate(1220,270),(10,356));art.label(cv,'Combo',46,382,.95,art.BONE);art.label(cv,'x',96,452,1.1,art.BONE)
cv.alpha_composite(art.plate(1220,310),(10,642))
art.label(cv,'Score',46,668,.95,art.BONE);art.label(cv,'Previous best',660,668,.95,art.BONE)
ImageDraw.Draw(cv).line((620,682,620,932),fill=(90,84,72,200),width=3)
art.navbar3(cv,'Mercenaries',MERC_NAV);save(cv,'r5-mercenaries.png')
# States sheet -- Title/menus/Loading/GameOver(desaturated)/Unsupported, restyled to r5
# materials: native standby.png collage kept as the base (it is already genuine native art,
# per prepare_assets.py), bands use the same metal tone as the nav/topstrip bars elsewhere in
# this companion rather than a2's flat band.
def cover(im,tw,th):
 # Owner round4 fix (20261103-visual-fixes.md): standby.png is native 1280x720 (16:9);
 # a plain .resize((1240,1080)) squashes it to a 1.148 aspect, visibly distorting every
 # standby/pause/game-over/loading variant built from it (they all derive from this one
 # cover-cropped image below). Scale to COVER the target box, preserving aspect, then
 # centre-crop the overflow -- same no-distortion rule used for any other full-screen
 # native art in this companion.
 sw,sh=im.size;s=max(tw/sw,th/sh)
 im=im.resize((max(1,round(sw*s)),max(1,round(sh*s))),Image.Resampling.LANCZOS)
 x=(im.width-tw)//2;y=(im.height-th)//2
 return im.crop((x,y,x+tw,y+th))
standby=cover(art.tex('2d000009',1),1240,1080);save(standby,'r5-standby.png')
gray=ImageEnhance.Color(standby.convert('RGB')).enhance(0.0).convert('RGBA');save(gray,'r5-standby-gray.png')
def state_band(lines):
 b=Image.new('RGBA',(1240,170));b.alpha_composite(art.metal(1240,170,(0x30,0x2c,0x24),0.6))
 ImageDraw.Draw(b).rectangle((0,0,1240,4),fill=(0x88,0x88,0x78,255))
 art.label(b,lines[0],620,46,1.05,art.BONE,'c')
 if len(lines)>1:art.label(b,lines[1],620,108,.8,art.PARCH,'c')
 return b
save(state_band(['Loading']),'r5-loading-band.png')
save(state_band(['Unsupported game version','Resident Evil 4 1.0.2 required']),'r5-unsupported-band.png')
# re4_reader.h enum order (Title=1,...,Loading=5,...,GameOver=22,...,Pause=31): Select() already
# collapses Loading/GameOver into the same 'standby' page id as Title (C1), so the per-state
# look above is differentiated here, not by page choice.
m['derived'] += [dict(name='d.state.loading',expr="'re4.state' == 5"),dict(name='d.state.gameover',expr="'re4.state' == 22")]
for n in range(4):
 # The Field remains visible but cannot claim picker drags or pinch fallback.
 ws=field()
 for w in ws:
  for key in ('on_tap','on_hold','draggable','pan_zoom','select_group','accept_group','drop_action','payload'):w.pop(key,None)
 ws += [image(f'r5-slot-edit-{n}.png',[999,7+239*n,232,228]),image('r5-picker.png',[0,0,1240,1080]),image(f'r5-picker-number-{n+1}.png',[PICKER_NUMBER_X,86,art.text_img(str(n+1),1,art.BONE).width,64])]
 # rt23 hit-tests rects, not image alpha. Leave only the owned viewport unblocked.
 ws += [dict(type='rect',rect=r,bg='#00000000',color='#00000000',border=0,input_block=True)
        for r in ([0,0,1240,180],[0,180,170,580],[1070,180,170,580],[0,760,1240,320])]
 ws += [label('Display only',[720,98,330,65],fit_text=True,text_min_scale=7,wrap_width=330,max_lines=1)]
 scroll=dict(id='owned',rect=[170,180,900,580],content_h_bind='re4.picker.height',axes='y')
 for k in range(30):
  key=f're4.picker.{k}';y=180+k*120
  ws += [image('r5-picker-highlight.png',[175,y,870,108],need_bind=key+f'.selected.{n}',scroll='owned'),dict(type='image',src_bind=key+'.art',rect=[180,y+10,180,90],need_bind=key+'.available',scroll='owned'),label('',[390,y,540,120],bind_text=key+'.name',need_bind=key+'.available',scroll='owned',text_scale=11,fit_text=True,text_min_scale=11,wrap_width=540,max_lines=2,line_gap=3,text_center_h=106),label('',[960,y+20,70,70],bind_text=key+'.ammo',need_bind=key+'.ammo.available',scroll='owned')]
  for bucket in range(SAVE_BUCKETS):
   combined=f'ui.pick.{n}.{k}.{bucket}'
   m['derived'].append(dict(name=combined,expr=f"'{key}.available' && 'd.bucket.{bucket}'"))
   ws += [button([175,y,870,108],f'assign_{bucket}_{n}_{k}',need_bind=combined,scroll='owned')]
 # rt23 clips and offsets scroll widgets through repeat expansion, even for one row.
 for w in ws:
  if w.get('scroll')=='owned':w['repeat']=1
 ws += [label('Clear slot',[220,780,500,75])]
 for bucket in range(SAVE_BUCKETS):
  ws += [button([180,770,600,120],f'clear_{bucket}_{n}',need_bind=f'd.bucket.{bucket}')]
 ws += [label('Cancel',[855,780,200,75],fit_text=True,text_min_scale=7,wrap_width=200,max_lines=1),button([820,770,260,120],'open_field')]
 pages.append(dict(id=f'picker_{n}',title='Assign slot',widgets=ws,scrolls=[scroll]))
# Every touch obeys the haptics preference. Native menu changes snap; no idle motion.
m['derived'] += [dict(name=f'ui.case.shape.{cols}.{sw}x{sh}',expr=f"'re4.case.width' == {cols} && 're4.case.selection.shape' == {sw*16+sh}") for cols,rows in CAPACITIES for sw,sh in shapes]
for n in range(4):
    expr=':'.join(f"('@drag_payload' == {source}) ? 're4.swap.{n}.{source}' " for source in range(3))+f": 're4.swap.{n}.3'"
    m['derived'].append(dict(name=f'ui.swap.{n}',expr=expr))
for page in pages:
    result=[];seen=set()
    for w in page['widgets']:
        if 'on_tap' not in w:
            result.append(w);continue
        # Bucket-gated widgets (per-save slot isolation) intentionally share a rect/on_tap
        # with their sibling buckets but differ in drop_action/need_bind -- include those so
        # they are never mistaken for the same literal duplicate.
        ident=(tuple(w['rect']),w['on_tap'],w.get('scroll',''),w.get('drop_action',''),w.get('need_bind',''))
        if ident in seen:continue
        seen.add(ident)
        gate=w.get('need_bind','')
        if gate in ('@flag:opt.haptics','!@flag:opt.haptics'):gate=''
        for enabled in (True,False):
            key=f"ui.touch.{len(m['derived'])}"
            expr=("'@flag:opt.haptics'" if enabled else "!'@flag:opt.haptics'")
            if gate:
                expr += " && "+("!'"+gate[1:]+"'" if gate.startswith('!') else "'"+gate+"'")
            m['derived'].append(dict(name=key,expr=expr))
            # A widget's own pre-set haptic (e.g. the equip-unsafe tap's 'reject', a
            # deliberately distinct refusal feel) survives into the "enabled" variant
            # instead of always collapsing to the generic 'light' tap acknowledgment.
            item=dict(w,need_bind=key,haptic=w.get('haptic','light') if enabled else 'off');result.append(item)
    page['widgets']=result
# COLD-BOOT FIX (20261103-title-fix.md): mod_pages.cpp's DrivePageBinds treats a bind's
# first valid read as a baseline capture that never fires an edge ("This is what keeps a
# package's first page the one a user sees at the title screen even once the bind arms") --
# so whatever sits at pages[0] is exactly what a cold boot shows before any re4.state
# transition. It was 'unavailable' (built from field() widgets, PACKAGE_FORMAT.md has no
# separate initial-page key -- mod_runtime.h's `current_page{0}` is the only rule), so a
# cold boot rendered the Field layout even though re4.state/re4.page already read
# 1/"standby" correctly. Put 'standby' at index 0 with a plain SWAP, not a full reorder:
# every other page keeps its original index, so a mid-game manifest reload (ReloadManifest
# does not reset current_page, only re-arms page_bind_state cold -- same silent-baseline
# rule applies) cannot land a returning player on the wrong page by index shift. The real
# edge into gameplay (re4.state 1 -> e.g. 7) still fires normally once it happens, so
# Field/pause/game-over etc. are unaffected.
standby_idx=next(i for i,p in enumerate(pages) if p['id']=='standby')
pages[0],pages[standby_idx]=pages[standby_idx],pages[0]
m['pages']=pages
m['page_binds']=[b for b in m['page_binds'] if b.get('point',b.get('bind'))!='re4.slots.committed']
# Automatic page switch, not a player touch: no haptic, a soft fade (matches the game's own
# scene-transition feel, P5R precedent "fade 250 ms"), never a slide/grow/shrink.
m['page_binds'].append(dict(point='re4.slots.committed',equals=1,when_equal=dict(page='field',transition='fade',duration_ms=200,easing='ease_out')))
# B5: ready the a2 states-sheet's "Pause Items/Merchant: Case live" variant. re4.menu.case does
# not exist yet (Group C / C2); 'all'-style && on a missing point fails closed, so this bind
# never fires today -- confirmed live, pause still shows 'dimmed' (05-paused-dimmed.png).
# It activates automatically the moment Group C publishes the point, no further manifest edit.
m['derived'].append(dict(name='d.pause.case_live',expr="'re4.state' == 31 && 're4.menu.case'"))
m['page_binds']=[b for b in m['page_binds'] if b.get('point',b.get('bind'))!='d.pause.case_live']
m['page_binds'].append(dict(point='d.pause.case_live',equals=1,when_equal=dict(page='case',transition='fade',duration_ms=150,easing='ease_out')))
# re4.state 33 = ShootingRange (re4_reader.h, appended after Radio): the merchant's range
# session/round swaps the case for the range weapon, so it reuses the dimmed page.
m['page_binds']=[b for b in m['page_binds'] if not (b.get('point')=='re4.state' and b.get('equals')==33)]
last=max(i for i,b in enumerate(m['page_binds']) if b.get('point')=='re4.state')
m['page_binds'].insert(last+1,dict(point='re4.state',equals=33,when_equal=dict(page='dimmed',transition='none')))
for b in m['page_binds']:
 for key in ('when_equal','when_not_equal'):
  if b.get(key,{}).get('page')=='files':b[key]['page']='field'
  if key in b and 'page' in b[key] and 'transition' not in b[key]:
   b[key].update(transition='fade',duration_ms=200,easing='ease_out')
m['sprite_map']={f'file:assets/{p.name}':f'file:assets/{p.name}' for p in sorted(out.glob('weapon-??.png'))}
for pat in ('slot-art-*.png','slot-missing-*.png','key-art-*.png','r5-hp-*.png','r5-digit-*.png','r5-badge-*.png','r5-case-*.png','r5-wait.png','r5-follow.png','r5-both.png'):
 for p in sorted(out.glob(pat)):
  # Ignore obsolete fixed-cell highlights if a local asset tree was not cleared.
  if p.name.startswith('r5-case-select-') and p.name not in {f'r5-case-select-{layout(c,r)[0]}-{w}x{h}.png' for c,r in CAPACITIES for w,h in shapes}:continue
  m['sprite_map']['file:assets/'+p.name]='file:assets/'+p.name
for p in sorted(out.glob('case-art-??.png')):m['sprite_map']['file:assets/'+p.name]='file:assets/'+p.name
# Only fixed native materials are preloaded. Room/ownership visibility stays native-gated.
for page in pages:
 for widget in page['widgets']:
  if widget['type']=='image' and widget.get('src_bind'):
   widget['src_hold']=False
m['sprite_map']={k:v for k,v in m['sprite_map'].items() if not runtime_key(k.removeprefix('file:assets/')) and not k.startswith(('file:assets/weapon-','file:assets/slot-art-','file:assets/slot-missing-','file:assets/case-art-'))}
weapon_ids=[3,33,64,35,37,39,41,42,44,45,46,47,48,52,53,54,55,56,148,23,109,62,82,1,2,14,65,16,71,83]
# FIFO boot order: map materials (SW reuses the three native plates), then HUD,
# then the compact Case utility batches. Save-dependent art is requested below.
m['preload']=['module:re4/geometry/1/0','module:re4/map/frame','module:re4/mark/arrow',
              'module:re4/map/backing','module:re4/ui/field',
              *[f'module:re4/map/label/{stage}' for stage in ('village','castle','island')],
              'module:re4/map/unmapped','module:re4/mark/atlas',
              *[f'module:re4/ui/hp-{n}' for n in range(15)],
              'module:re4/ui/equipped',
              *[f'module:re4/digit/{n}' for n in range(10)],
              *[f'file:assets/r5-badge-{n}.png' for n in range(1,6)],
              *[f'module:re4/case/prewarm/{group}' for group in range(8)]]
# Native-gated, zero-pixel requests on every page, before its visible widgets.
# Stage/room/door state is read during title/loading as soon as native data exists.
# This is a bounded working set, never a sweep of every possible state/weapon.
def warm_image(bind,gate):
 return dict(type='image',src_bind=bind,need_bind=gate,rect=[0,0,1,1],
             color='#00FFFFFF',src_hold=False)
warm=[dict(type='image',src_bind='re4.geometry.art',rect=[0,0,1,1],color='#00FFFFFF',src_hold=False)]
warm += [dict(warm_image('re4.map.warm.{i}.art','re4.map.warm.ready'),repeat=256,repeat_bind='re4.map.warm.count')]
for n in range(4):
 warm += [warm_image(f're4.slot.{n}.art','re4.slots.ready'),
          warm_image(f're4.slot.{n}.missing_art','re4.slots.ready')]
# Optional-page work follows maps and assigned slots in the same render traversal.
for n in range(30):warm.append(warm_image(f're4.picker.{n}.art',f're4.picker.{n}.available'))
warm.append(dict(warm_image('re4.keys.warm.{i}.art','re4.slots.ready'),repeat=384,repeat_bind='re4.keys.warm.count'))
for key in ('picker',*[f'picker-number-{n}' for n in range(1,5)],'key-items','settings'):
 warm.append(dict(type='image',src_bind='re4.asset.blank',empty_src=f'module:re4/ui/{key}',need_bind='re4.slots.ready',
                  rect=[0,0,1,1],color='#00FFFFFF',src_hold=False))
for key in ('r5-picker-highlight.png',*[f'r5-slot-edit-{n}.png' for n in range(4)]):
 warm.append(dict(type='image',src_bind='re4.asset.blank',empty_src='file:assets/'+key,need_bind='re4.slots.ready',
                  rect=[0,0,1,1],color='#00FFFFFF',src_hold=False))
for page in pages:page['widgets']=warm+page['widgets']

import re
case_ids=map(int,re.findall(r'case (\d+):return', (root/'native/modules/re4_case_art.h').read_text()))
runtime_keys=set(ui_recipe.outputs)
runtime_keys.update('module:re4/icon/'+f"{row['id']:02x}" for row in keymeta)
runtime_keys.update('module:re4/case/'+f'{id:02x}/120' for id in case_ids)
for id in weapon_ids:
 runtime_keys.update((f'module:re4/weapon/{id:02x}',f'module:re4/slot/{id:02x}',f'module:re4/slot/{id:02x}/missing'))
m['sprite_map'].update({key:key for key in sorted(runtime_keys)})
# Native map chrome occupies only this content box in the logical canvas.
for page in pages:
 for widget in page['widgets']:
  if widget.get('empty_src') in ('module:re4/map/frame','module:re4/map/backing','module:re4/map/unmapped'):
   assert widget['rect']==[0,0,1240,1080]
   widget['rect']=[6,114,981,838]

# Gate all declared-font text until decode_font has delivered immutable metrics.
# The paged atlas keeps glyphs blank independently while page pixels are pending/failed.
font_gates={}
for page in pages:
 for widget in page['widgets']:
  if widget.get('text') or widget.get('bind_text') or widget['type']=='value':
   original=widget.get('need_bind','')
   if original:
    assert re.fullmatch(r'!?[A-Za-z0-9_.:@-]+',original),original
    if original not in font_gates:
     name=f'ui.font.{len(font_gates)}'
     expr=f"!'{original[1:]}'" if original.startswith('!') else f"'{original}'"
     font_gates[original]=name
     m['derived'].append(dict(name=name,expr=f"'re4.font.installed' && ({expr})"))
    widget['need_bind']=font_gates[original]
   else:widget['need_bind']='re4.font.installed'
# Field widgets are reused on seven pages; identical derived gates are shared.
unique={}
for entry in m['derived']:
 if entry['name'] in unique and unique[entry['name']]!=entry:
  raise ValueError(f"Conflicting derived gate: {entry['name']}")
 unique[entry['name']]=entry
m['derived']=list(unique.values())
import backgrounds
pieces,background_layouts=backgrounds.prepare(background_images,ui_recipe,CAPACITIES,layout)
backgrounds.rewrite(m,pages,background_layouts)
hp_layout=backgrounds.prepare_hp(background_images,ui_recipe)
backgrounds.rewrite_hp(m,pages,hp_layout)
m['sprite_map'].update({key:key for key in pieces})
(root/'tools/resident-evil-4/background_layout.json').write_text(json.dumps(dict(hp=hp_layout,layouts=background_layouts,sizes={k:list(v.size) for k,v in pieces.items()}),indent=2)+'\n')
(pkg/'manifest.json').write_text(json.dumps(m,indent=2)+'\n')
ui_recipe.emit(root/'native/modules/re4_ui_recipes.h')
from provenance import write_provenance
write_provenance(pkg)
print('Prepared r5 materials and live bindings')
