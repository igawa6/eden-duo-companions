#!/usr/bin/env python3
"""prune_derived.py <dualscreen dir>: drop derived values nothing reads (perf pass 2026-09-25).

The runtime evaluates every derived value every tick (twice while any derived value reads an
'@' interaction source) and publishes each as an int AND a float. A derived value is live when
its name is referenced by any manifest string outside the derived definitions themselves
(pages, page_binds, actions, enforce, composites, map, ...; gates split on '+', '|', ',' and a
leading '!' or '$' ("$name" payloads); '{i}' templates match any index) or by a live derived value. Everything else is
removed. module_outputs / _about are declarations, not references.
"""
import json, re, sys
from pathlib import Path

def main(dual):
    mp = Path(dual) / 'manifest.json'
    m = json.loads(mp.read_text())
    derived = m.get('derived', [])
    names = [d['name'] for d in derived]
    lit, pats = set(), []

    def add1(part):
        # '$name' (a widget payload / action operand read from a published value) and '{$name}'
        # (an expanded action name) reference the value too; 1.0.1 missed '$' and pruned the
        # calendar's ui.cal.key.* payloads, so a calendar cell tap carried no date.
        for inner in re.findall(r'\{\$([^{}]+)\}', part):
            add1(inner)
        part = part.strip().lstrip('!').lstrip('$')
        if not part:
            return
        if '{' in part or '%' in part:
            p = re.escape(part)
            p = re.sub(r'\\\{i(?:[+-]\d+)?\\\}', r'\\d+', p)
            p = re.sub(r'%d', r'\\d+', p)
            pats.append(re.compile(p))
        else:
            lit.add(part)

    def add(s):
        add1(s)  # whole string (derived names may contain '+')
        for part in re.split(r'[+|,]', s):
            add1(part)

    def walk(o, skip_name=False):
        if isinstance(o, dict):
            for k, v in o.items():
                if k in ('module_outputs', '_about') or (skip_name and k == 'name'):
                    continue
                if isinstance(k, str) and '.' in k:
                    add(k)
                walk(v)
        elif isinstance(o, list):
            for v in o:
                walk(v)
        elif isinstance(o, str):
            add(o)

    for k, v in m.items():
        if k != 'derived':
            walk(v)
    roots = set(lit)
    root_pats = list(pats)
    deps = {}
    for d in derived:
        lit.clear(); pats.clear()
        walk(d, True)
        deps[d['name']] = set(lit)
    live = {n for n in names if n in roots or any(p.fullmatch(n) for p in root_pats)}
    todo = list(live)
    while todo:
        n = todo.pop()
        for dep in deps.get(n, ()):
            if dep in deps and dep not in live:
                live.add(dep)
                todo.append(dep)
    kept = [d for d in derived if d['name'] in live]
    m['derived'] = kept
    mp.write_text(json.dumps(m, ensure_ascii=False, separators=(',', ':')) + '\n')
    print(f'prune_derived: {len(derived)} -> {len(kept)} derived values ({len(derived) - len(kept)} unread)')

if __name__ == '__main__':
    main(sys.argv[1])
