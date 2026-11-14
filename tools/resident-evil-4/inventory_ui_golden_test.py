#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare every inventory/UI module key with clean pre-port Python assets."""
import argparse,json,os,re,subprocess,tempfile
from pathlib import Path

def keys(golden):
    result={}
    for p in sorted(golden.glob('*.png')):
        n=p.name
        if n.startswith('case-art-'):key='case/'+n[9:-4]+'/120'
        elif n.startswith('weapon-'):key='weapon/'+n[7:-4]
        elif n.startswith('slot-art-'):key='slot/'+n[9:-4]
        elif n.startswith('slot-missing-'):key='slot/'+n[13:-4]+'/missing'
        elif n.startswith('key-art-'):key='icon/'+n[8:-4]
        elif n.startswith('r5-case-name-'):key='case/name/'+n[13:-4]
        elif re.fullmatch(r'r5-case-\d+\.png',n):key='case/background/'+n[8:-4]
        elif n.startswith('r5-digit-'):key='digit/'+n[9:-4]
        elif n.startswith('r5-hp-'):key='ui/hp-'+n[6:-4]
        elif n in {'r5-equipped.png','r5-field.png','r5-field-unmapped.png','r5-key-items.png','r5-loading-band.png','r5-mercenaries.png','r5-nav.png','r5-neutral.png','r5-settings.png','r5-standby.png','r5-standby-gray.png','r5-unsupported-band.png','r5-wait.png','r5-follow.png','r5-both.png'}:key='ui/'+n[3:-4]
        else:continue
        result['module:re4/'+key]=p
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('binary',type=Path);p.add_argument('--report',type=Path,default=os.environ.get('RE4_TEST_INVENTORY_REPORT'));a=p.parse_args()
    if not os.environ.get('RE4_TEST_ROMFS') or not os.environ.get('RE4_TEST_GOLDENS'):
        print('SKIP: RE4_TEST_ROMFS and RE4_TEST_GOLDENS required');return 77
    from PIL import Image,ImageChops,ImageStat,ImageDraw
    golden=Path(os.environ['RE4_TEST_GOLDENS']);samples=keys(golden)
    assert len(samples)==311,(len(samples),'incomplete clean reference')
    picker=os.environ.get('RE4_TEST_PICKER_GOLDEN')
    if not picker or not Path(picker).is_file():
        print('SKIP: RE4_TEST_PICKER_GOLDEN required for shared picker composition');return 77
    samples['module:re4/ui/picker']=Path(picker)
    results={}
    with tempfile.TemporaryDirectory(prefix='re4-inventory-ui-golden-') as tmp:
        for key,path in samples.items():
            raw=Path(tmp)/'image.rgba'
            run=subprocess.run([str(a.binary.resolve()),'--image',key,str(raw)],capture_output=True,text=True)
            if run.returncode:
                results[key]=dict(passed=False,error=run.stderr[-500:]);continue
            w,h=map(int,run.stdout.splitlines()[-1].split());expected=Image.open(path).convert('RGBA')
            if key in ('module:re4/ui/field','module:re4/ui/field-unmapped'):
                ImageDraw.Draw(expected).rectangle((6,114,986,951),fill=(0,0,0,0))
            original=expected
            if key.startswith('module:re4/case/') and key.endswith('/120'):
                table=(Path(__file__).resolve().parents[2]/'native/modules/re4_case_art.h').read_text()
                item=int(key.split('/')[-2],16);item=0 if item==160 else item
                cells=re.search(r'case '+str(item)+r':return \{(\d+),(\d+)\}',table)
                assert cells and (w,h)==tuple(int(v)*120 for v in cells.groups()),key
                x=(expected.width-w)//2;y=(expected.height-h)//2
                expected=expected.crop((x,y,x+w,y+h))
                restored=Image.new('RGBA',original.size);restored.paste(expected,(x,y))
                assert ImageChops.difference(restored,original).getbbox(alpha_only=False) is None,(key,'nonempty crop margin')
            elif key.startswith(('module:re4/slot/','module:re4/weapon/')):
                assert (w,h)==((163,163) if '/slot/' in key else (180,90)),key
                # Host DrawImageRegion uses floor(dest * source / rect), not Pillow NEAREST.
                expected=Image.frombytes('RGBA',(w,h),b''.join(bytes(original.getpixel((x*original.width//w,y*original.height//h))) for y in range(h) for x in range(w)))
            assert expected.size==(w,h),(key,expected.size,(w,h))
            actual=Image.frombytes('RGBA',(w,h),raw.read_bytes());diff=ImageChops.difference(expected,actual)
            maximum=max(v for _,v in diff.getextrema());mean=sum(ImageStat.Stat(diff).sum)/(w*h*4)
            ms=float(next(line.split()[1] for line in run.stdout.splitlines() if line.startswith('image_ms ')))
            # Nested Pillow fixed-point composition can round one channel by one LSB.
            tolerance=1 if '/ui/' in key or '/background/' in key or '/name/' in key else 0
            results[key]=dict(width=w,height=h,max_channel_error=maximum,mean_channel_error=mean,tolerance=tolerance,ms=ms,passed=maximum<=tolerance)
        # Recompose the shared panel with its native-atlas numeral at manifest coordinates.
        manifest=json.loads((Path(__file__).resolve().parents[2]/'packages/ResidentEvil4/dualscreen/manifest.json').read_text())
        for n in range(4):
            page=next(p for p in manifest['pages'] if p['id']==f'picker_{n}')
            widget=next(w for w in page['widgets'] if w.get('src')==f'module:re4/ui/picker-number-{n+1}')
            layers=[]
            for key in ('module:re4/ui/picker',widget['src']):
                raw=Path(tmp)/'title.rgba'
                run=subprocess.run([str(a.binary.resolve()),'--image',key,str(raw)],capture_output=True,text=True,check=True)
                w,h=map(int,run.stdout.splitlines()[-1].split())
                layers.append(Image.frombytes('RGBA',(w,h),raw.read_bytes()))
            layers[0].alpha_composite(layers[1],tuple(widget['rect'][:2]))
            box=(140,70,1100,160)
            expected=Image.open(golden/f'r5-picker-{n}.png').convert('RGBA').crop(box)
            diff=ImageChops.difference(layers[0].crop(box),expected)
            maximum=max(v for _,v in diff.getextrema())
            results[f'picker-title-{n+1}']=dict(max_channel_error=maximum,tolerance=1,passed=maximum<=1)
        from background_golden_test import check as check_backgrounds
        results.update(check_backgrounds(a.binary,Path(tmp)))
        report=dict(total=len(results),passed=sum(r['passed'] for r in results.values()),keys=results)
        if a.report:a.report.write_text(json.dumps(report,indent=2)+'\n')
        failures={k:v for k,v in results.items() if not v['passed']}
        print(json.dumps(dict(total=len(results),passed=report['passed'],failures=failures),indent=2))
        assert not failures
    return 0
if __name__=='__main__':raise SystemExit(main())
