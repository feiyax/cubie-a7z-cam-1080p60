#!/usr/bin/env python3
"""Fetch the pinned BSP; verify immutable headers or the exact Git baseline."""
import argparse, hashlib, json, os, pathlib, subprocess, urllib.request
ROOT = pathlib.Path(__file__).resolve().parents[1]
def sha(path): return hashlib.file_digest(path.open('rb'), 'sha256').hexdigest()
def fetch(spec, dest):
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists() and sha(dest) == spec['sha256']: return dest
    part = dest.with_suffix(dest.suffix + '.part')
    try:
        with urllib.request.urlopen(spec['url'], timeout=90) as source, part.open('wb') as output:
            import shutil
            shutil.copyfileobj(source, output)
        if sha(part) != spec['sha256']: raise RuntimeError('SHA256 mismatch: ' + spec['url'])
        part.replace(dest)
    finally:
        part.unlink(missing_ok=True)
    return dest
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cache', type=pathlib.Path, default=pathlib.Path(os.environ.get('WORK_DIR', ROOT/'.work'))/'downloads')
    p.add_argument('--bsp', action='store_true'); p.add_argument('--baseline-ref')
    a=p.parse_args()
    for item in json.loads((ROOT/'upstream/manifest.json').read_text())['files']:
        if item['baseline_only']:
            if not a.baseline_ref: continue
            data=subprocess.check_output(['git','show',a.baseline_ref+':'+item['path']],cwd=ROOT)
            actual=hashlib.sha256(data).hexdigest()
        else: actual=sha(ROOT/item['path'])
        if actual != item['sha256']: raise SystemExit('Upstream mismatch: '+item['path'])
    if a.bsp:
        spec=json.loads((ROOT/'sources.json').read_text())['bsp']
        print(fetch(spec,a.cache/spec['file']))
    else: print('Upstream hash checks passed')
if __name__ == '__main__': main()
