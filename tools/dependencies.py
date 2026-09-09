"""Fetch pinned upstream static libraries, verifying every file before installation."""
from pathlib import Path
import argparse,hashlib,json,urllib.request

root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--from-local',type=Path,help='Use an existing OptiScaler source tree instead of downloading')
a=p.parse_args()
manifest=json.loads((root/'tools/dependencies.json').read_text())
for item in manifest['files']:
    rel=Path(item['path'])
    target=(root/'src'/rel).resolve()
    if not target.is_relative_to((root/'src').resolve()):raise ValueError('Invalid dependency path')
    if target.is_file() and hashlib.sha256(target.read_bytes()).hexdigest()==item['sha256']:
        print('Verified',rel);continue
    if a.from_local:
        data=(a.from_local/rel).read_bytes()
    else:
        url=f"https://raw.githubusercontent.com/{manifest['repository']}/{manifest['commit']}/{rel.as_posix()}"
        with urllib.request.urlopen(url,timeout=120) as response:data=response.read()
    if hashlib.sha256(data).hexdigest()!=item['sha256']:raise ValueError('SHA-256 mismatch: '+str(rel))
    target.parent.mkdir(parents=True,exist_ok=True)
    temporary=target.with_suffix(target.suffix+'.download')
    temporary.write_bytes(data)
    temporary.replace(target)
    print('Installed',rel)
