"""Run sweep-spm-maps.py over many maps in parallel game windows (default 10), one chunk per window, then merge.
Usage: python tools\\sweep-parallel.py --out autotest\\pal-fr [--disc Disc_PAL\\DATA] [--windows 10] [--seconds 3]
       [--maps a,b,c] [--baseline autotest\\map-sweep-20261016-210137\\results.json] (env SPM_SWEEP_* passes through)
Writes <out>\\results.json and REPORT.md over all chunks; with --baseline, lists maps whose status changed."""
import argparse, json, os, subprocess, sys
from collections import Counter
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument('--out', type=Path, required=True)
ap.add_argument('--disc', type=Path)
ap.add_argument('--windows', type=int, default=10)
ap.add_argument('--seconds', type=int, default=3)
ap.add_argument('--maps')
ap.add_argument('--baseline', type=Path)
args = ap.parse_args()
root = Path(__file__).resolve().parents[1]
disc = (args.disc or root / 'Disc/DATA').resolve()
maps = args.maps.split(',') if args.maps else sorted(p.stem for p in (disc / 'files/map').glob('*.bin'))
n = max(1, min(args.windows, len(maps)))
chunks = [maps[i::n] for i in range(n)]  # interleaved: each window gets maps from every area
args.out.mkdir(parents=True, exist_ok=True)
procs = []
for i, chunk in enumerate(chunks):
    cmd = [sys.executable, str(root / 'tools/sweep-spm-maps.py'), '--disc', str(disc), '--maps', ','.join(chunk),
           '--seconds', str(args.seconds), '--out', str(args.out / f'chunk-{i:02}')]
    procs.append(subprocess.Popen(cmd, cwd=root, stdout=open(args.out / f'chunk-{i:02}.log', 'w'), stderr=subprocess.STDOUT))
print(f'{len(maps)} maps in {n} windows -> {args.out}', flush=True)
for p in procs:
    p.wait()
results, by_map = [], {}
for i in range(n):
    f = args.out / f'chunk-{i:02}' / 'results.json'
    if f.exists():
        for r in json.load(open(f))['results']:
            results.append(r)
            by_map[r['map']] = r
missing = [m for m in maps if m not in by_map]
counts = Counter(r['status'] for r in results)
lines = [f'# Parallel sweep: {len(results)}/{len(maps)} maps, {dict(counts)}' + (f', not reached: {missing}' if missing else ''), '',
         '| Map | Result | Detail |', '|---|---|---|']
for m in maps:
    r = by_map.get(m)
    if r:
        detail = r.get('detail', '') + ('; ' + ' / '.join(r['problems'][:2]) if r.get('problems') else '') + ('; RENDER: ' + ', '.join(r['render']) if r.get('render') else '')
        lines.append(f'| {m} | {r["status"]} | {detail.replace("|", "/")} |')
    else:
        lines.append(f'| {m} | not reached | |')
json.dump(dict(requested=len(maps), attempted=len(results), counts=counts, results=results), open(args.out / 'results.json', 'w'), indent=1)
if args.baseline:
    base = {r['map']: r for r in json.load(open(args.baseline))['results']}
    changed = [(m, base[m]['status'], by_map[m]['status']) for m in maps if m in base and m in by_map and base[m]['status'] != by_map[m]['status']]
    lines += ['', f'## vs baseline {args.baseline}: {len(changed)} status changes'] + [f'- {m}: {a} -> {b}' for m, a, b in changed]
(args.out / 'REPORT.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
print('\n'.join(lines[:1] + lines[-(len(changed) + 2) if args.baseline else 0:]), flush=True)
