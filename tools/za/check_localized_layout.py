# SPDX-FileCopyrightText: Copyright 2026 Sickops
# SPDX-License-Identifier: GPL-3.0-or-later
"""Audit fixed captions and navigation labels using the packaged MFNT metrics."""
import argparse,json,re,struct
from pathlib import Path
from ui_i18n import FALLBACKS

LANGUAGES = (1,2,3,4,5,7,8,9,10,11)
NAV_LABELS = ("Plus","Équipe","Pokédex","Boîte","Carte","Sac","Missions","Progression","Carnet")
FRENCH_ELISION = re.compile(r"\b(?:[cdjlmnst]|qu|jusqu|lorsqu|puisqu)\s*['’]\s+\w", re.IGNORECASE)


def line_count(text, width, measure):
    lines = 0
    for paragraph in text.split("\n"):
        current = ""
        for word in paragraph.split(" "):
            joined = f"{current} {word}" if current else word
            if measure(joined) <= width:
                current = joined
                continue
            if current:
                lines += 1
                current = ""
            for char in word:
                if current and measure(current + char) > width:
                    lines += 1
                    current = ""
                current += char
        lines += 1
    return lines


def main():
    p=argparse.ArgumentParser();p.add_argument('language_log',type=Path);a=p.parse_args()
    root=Path(__file__).resolve().parents[2];base=root/'packages/PokemonLegendsZA/dualscreen'
    m=json.loads((base/'manifest.json').read_text('utf-8'));specs=json.loads((root/'tools/za/ui_bindings.generated.json').read_text('utf-8'))
    font=(base/'font/za.mfnt').read_bytes();line_height=struct.unpack_from('<I',font,0x1c)[0];first=struct.unpack_from('<I',font,0x28)[0]
    coverage=set(json.loads((root/'tools/za/font_coverage.generated.json').read_text('utf-8')))
    game={}
    for line in a.language_log.read_text('utf-8').splitlines():
        if line.startswith('UI '):
            _,*parts=line.split('\t');lang=int(line.split('\t')[0][3:])
            if len(parts)==2:game[lang,parts[0]]=parts[1]
    errors=[];measured=0
    for page in m['pages']:
        for w in page['widgets']:
            key=w.get('bind_text');spec=specs.get(key)
            if spec and not spec[1]:
                sources=(spec[0],)
            elif key and key.startswith("za.nav.") and key.endswith(".name"):
                sources=NAV_LABELS
            else:
                continue
            scale=w.get('text_scale',5)
            if w.get('fit_text'):
                scale=w.get('text_min_scale',scale)
            def measure(value):
                return sum(struct.unpack_from('<H',font,first+(ord(c)-31)*14+12)[0]
                           for c in value)*scale*5/line_height
            for source in sources:
                for lang in LANGUAGES:
                    text=source if lang==3 else FALLBACKS.get(source,{}).get(lang) or game.get((lang,source))
                    place=f'{page["id"]} lang{lang}'
                    if text is None:
                        errors.append(f'{place}: missing translation for {source!r}')
                        continue
                    measured+=1
                    if (lang==3 and FRENCH_ELISION.search(text)) or re.search(r' {2,}',text):
                        errors.append(f'{place}: unexpected spacing in {text!r}')
                    missing={c for c in text if ord(c) not in coverage and c not in "\n\r\t"}
                    if missing:
                        errors.append(f'{place}: unsupported characters {sorted(missing)!r} in {text!r}')
                        continue
                    width=w.get('wrap_width',0)
                    limit=w.get('max_lines',0)
                    if width and limit and line_count(text,width,measure)>limit:
                        errors.append(f'{place}: {text!r} exceeds {limit} line(s) at {width}px')
                    if not width and 'x_bind' not in w:
                        anchor=w['rect'][0]
                        align=w.get('align','left')
                        room=anchor if align=='right' else m['canvas_w']-anchor
                        if align=='center':room=2*min(anchor,m['canvas_w']-anchor)
                        if any(measure(line)>room for line in text.splitlines()):
                            errors.append(f'{place}: {text!r} crosses the canvas edge')
    print(f'Translated caption checks: {measured}; issues: {len(errors)}')
    for error in errors:print(error)
    return bool(errors)
if __name__=='__main__':raise SystemExit(main())
