#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""All active map keys against the pre-port clean Python PNGs; skip without fixtures."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('binary',type=Path)
    p.add_argument('--report',type=Path)
    a=p.parse_args()
    rom=os.environ.get('RE4_TEST_ROMFS','')
    golden=Path(os.environ.get('RE4_TEST_GOLDENS',''))
    if not rom or not os.environ.get('RE4_TEST_GOLDENS') or not (Path(rom)/'bio4/ss/cmn/map_obj1.dat').is_file():
        print('SKIP: set RE4_TEST_ROMFS and RE4_TEST_GOLDENS');return 77
    from PIL import Image, ImageChops, ImageStat
    import re
    tables=(Path(__file__).resolve().parents[2]/'native/modules/re4_map_state_tables.h').read_text()
    room_section=tables.split('MapStateRooms[] = {',1)[1].split('};',1)[0]
    expected_keys=set()
    for stage,room,multi in re.findall(r'\{"([a-z0-9]+)-([a-f0-9]+)", \d+, \d+, (true|false)\}',room_section):
        prefix=f'module:re4/room/{stage}/{room}/'
        expected_keys.update(prefix+str(n) for n in (1,2,3,4))
        if multi=='true':expected_keys.update(prefix+'4/f'+str(n) for n in range(6))
    assert len(expected_keys)==852
    binary=str(a.binary.resolve());results={}
    def compare(key,expected,raw,width,height,ms=None):
        expected=expected.convert('RGBA')
        assert expected.size==(width,height), key+' dimensions differ'
        assert len(raw)==width*height*4,key+' pixel count differs'
        actual=Image.frombytes('RGBA',(width,height),raw)
        diff=ImageChops.difference(actual,expected)
        maximum=max(maximum for minimum,maximum in diff.getextrema())
        bands=diff.split()
        changed=ImageChops.lighter(ImageChops.lighter(bands[0],bands[1]),ImageChops.lighter(bands[2],bands[3]))
        histogram=changed.histogram()
        # Pillow's fixed-point alpha blend and this compositor can differ by one LSB.
        tolerance=1 if key in ('module:re4/map/legend','module:re4/map/unmapped') else 0
        row=dict(width=width,height=height,max_channel_error=maximum,
                 mean_channel_error=sum(ImageStat.Stat(diff).sum)/(width*height*4),
                 changed_pixels=sum(histogram[1:]),tolerance=tolerance,passed=maximum<=tolerance)
        if ms is not None:row['ms']=ms
        results[key]=row
    with tempfile.TemporaryDirectory(prefix='re4-map-golden-') as tmp:
        root=Path(tmp)
        run=subprocess.run([binary,'--map-images',tmp],check=True,capture_output=True,text=True)
        sweep=run.stdout
        for line in sweep.splitlines():
            if not line.startswith('map-room-'):continue
            name,width,height,ms=line.split();stage,room,state,*floor=name.removeprefix('map-room-').split('-')
            key=f'module:re4/room/{stage}/{room}/{state}'+('/'+floor[0] if floor else '')
            expected=golden/(name+'.png');assert expected.is_file(),str(expected)+' missing'
            raw=root/(name+'.rgba');compare(key,Image.open(expected),raw.read_bytes(),int(width),int(height),float(ms));raw.unlink()
        assert set(results)==expected_keys, 'Room/floor key set differs from generated state tables'
        samples=[('module:re4/mark/atlas','map-markers.png'),('module:re4/mark/arrow','leon-map-arrow.png'),
                 ('module:re4/map/frame','r5-map-frame.png'),('module:re4/map/legend','r5-legend.png')]
        samples += [('module:re4/map/backing','r5-field.png'),('module:re4/map/unmapped','r5-field-unmapped.png')]
        samples += [(f'module:re4/map/label/{s}',f'r5-{s}-label.png') for s in ('village','castle','island')]
        samples += [('module:re4/door/'+p.stem.removeprefix('map-door-').replace('-','/'),p.name) for p in sorted(golden.glob('map-door-*.png'))]
        # Every generated angle/state must exist; a partial golden directory cannot pass.
        section=tables.split('MapDoorDefs[] = {',1)[1].split('};',1)[0]
        angles={int(row.split(',')[2]) for row in re.findall(r'\{([^}]+)\}',section)}
        required={f'map-door-{angle}-{state}.png' for angle in angles for state in (1,2,3)}
        actual={png for key,png in samples if key.startswith('module:re4/door/')}
        assert actual==required, 'Incomplete door golden set'
        atlas=Image.open(golden/'map-markers.png').convert('RGBA')
        samples += [(f'module:re4/mark/{name}',atlas.crop((64*n,0,64*(n+1),64))) for n,name in enumerate(('star','medallion','save','goal'))]
        for key,png in samples:
            raw=root/'image.rgba'
            run=subprocess.run([binary,'--image',key,str(raw)],check=True,capture_output=True,text=True)
            width,height=map(int,run.stdout.splitlines()[-1].split())
            expected=Image.open(golden/png) if isinstance(png,str) else png
            ms=float(next(line.split()[1] for line in run.stdout.splitlines() if line.startswith('image_ms ')))
            if key in ('module:re4/map/backing','module:re4/map/unmapped'):
                cut=Image.new('RGBA',expected.size);cut.paste(expected.crop((6,114,987,952)),(6,114));expected=cut
            if key in ('module:re4/map/frame','module:re4/map/backing','module:re4/map/unmapped'):
                expected=expected.crop((6,114,987,952))
            compare(key,expected,raw.read_bytes(),width,height,ms)
        peak=int(next(l.split()[1] for l in sweep.splitlines() if l.startswith('peak_rss_kib ')))
        assert peak<=64*1024, f'Room sweep exceeds 64 MiB: {peak} KiB'
        assert all(r['width']<=4096 and r['height']<=4096 and r['width']*r['height']*4<=16*1024*1024 for r in results.values()), 'Host image cap exceeded'
        report=dict(keys=results,passed=sum(r['passed'] for r in results.values()),total=len(results),peak_rss_kib=peak)
        if a.report:a.report.write_text(json.dumps(report,indent=2)+'\n')
        failed={k:v for k,v in results.items() if not v['passed']}
        print(json.dumps(dict(total=len(results),passed=report['passed'],failed=failed),indent=2))
        assert not failed, 'map golden mismatch'
    return 0


if __name__=='__main__':raise SystemExit(main())
