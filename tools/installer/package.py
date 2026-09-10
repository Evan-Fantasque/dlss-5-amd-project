"""Assemble the replacement installer without copying private runtime/model files."""
from pathlib import Path
import argparse, hashlib, json, shutil, zipfile, subprocess, sys

root = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser()
p.add_argument('--output', type=Path, default=root / '.build/installer/OptiScaler-AMD-test13-installer')
p.add_argument('--dll', type=Path, default=root / 'src/x64/Release/OptiScaler.dll')
a = p.parse_args()
subprocess.run([sys.executable, str(root / 'tools/localization.py'), '--check', '--binary', str(a.dll)], check=True)
out = a.output.resolve()
if out.exists() and any(out.iterdir()):
    p.error('Choose a new empty output directory to avoid packaging stale files.')
(out / 'payload').mkdir(parents=True, exist_ok=True)
shutil.copy2(a.dll, out / 'payload/winmm.dll')
for name in ('OptiScaler.ini', 'amd_presr_perf.ini'):
    shutil.copy2(root / 'config/ffxiv-release' / name, out / 'payload' / name)
for name in ('Install.ps1', 'INSTALL.bat', 'README.zh-CN.md'):
    shutil.copy2(root / 'tools/installer' / name, out / name)
shutil.copy2(root / 'LICENSE', out / 'LICENSE')
files = [dict(name=f.name, sha256=hashlib.sha256(f.read_bytes()).hexdigest()) for f in sorted((out / 'payload').iterdir())]
manifest = dict(version='test13', files=files,
                dependencies=json.loads((root / 'tools/installer/dependencies.json').read_text()))
(out / 'package-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
archive = out.with_suffix('.zip')
with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for file in sorted(out.rglob('*')):
        if file.is_file(): z.write(file, file.relative_to(out))
with zipfile.ZipFile(archive) as z:
    assert z.testzip() is None
    for f in files:
        assert hashlib.sha256(z.read('payload/' + f['name'])).hexdigest() == f['sha256']
print(archive)
