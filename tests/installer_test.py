"""Windows integration checks in fresh disposable directories; never installs into the supplied dependency directory."""
from pathlib import Path
import argparse, ctypes, hashlib, json, os, shutil, subprocess, tempfile

p = argparse.ArgumentParser()
p.add_argument('--package', required=True, type=Path)
p.add_argument('--dependencies', required=True, type=Path, help='Read-only directory containing the matched dependencies')
p.add_argument('--output', required=True, type=Path)
a = p.parse_args()
package = a.package.resolve()
root = a.output.resolve()
root.mkdir(parents=True, exist_ok=True)
run = Path(tempfile.mkdtemp(prefix='installer-check-', dir=root))
manifest = json.loads((package / 'package-manifest.json').read_text())
names = ['winmm.dll', 'OptiScaler.ini', 'amd_presr_perf.ini']
original = {n: ('old fixture ' + n).encode() for n in names}
def fixture(name, old=True, skip=None):
    target = run / name
    target.mkdir()
    (target / 'ffxiv_dx11.exe').write_bytes(b'fixture only; not an executable')
    for dep in manifest['dependencies']:
        dst = target / dep['name']
        dst.parent.mkdir(parents=True, exist_ok=True)
        if dep['name'] == skip: continue
        src = a.dependencies / dep['name']
        try: os.link(src, dst)
        except OSError: shutil.copy2(src, dst)
    if old:
        for name, data in original.items(): (target / name).write_bytes(data)
    return target
def execute(target, expect=0, check=False, pkg=package):
    args = ['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(pkg / 'Install.ps1'), '-GameDirectory', str(target)]
    args += ['-CheckOnly'] if check else ['-Yes']
    r = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
    assert r.returncode == expect, (target.name, r.returncode, r.stdout.decode(errors='replace'))
    (run / (target.name + '-console.txt')).write_bytes(r.stdout)
def unchanged(target):
    assert all((target / n).read_bytes() == data for n, data in original.items())
def installed(target):
    for f in manifest['files']:
        assert hashlib.sha256((target / f['name']).read_bytes()).hexdigest() == f['sha256']

t = fixture('check-only'); execute(t, check=True); unchanged(t)
assert not list(t.glob('OptiScaler-backup-*')); print('PASS check-only makes no installation changes')
t = fixture('upgrade 中文 [目录] & spaces'); execute(t); installed(t)
backup, = t.glob('OptiScaler-backup-*')
assert all((backup / n).read_bytes() == data for n, data in original.items())
print('PASS upgrade, Unicode/special paths, exact backups and installed hashes')
t = fixture('first-install', old=False); execute(t); installed(t)
backup, = t.glob('OptiScaler-backup-*')
assert not any(json.loads((backup / 'original-files.json').read_text(encoding='utf-8-sig')).values())
print('PASS first installation records originally absent files')
t = fixture('missing-exe'); (t / 'ffxiv_dx11.exe').unlink(); execute(t, expect=1); unchanged(t)
print('PASS missing game rejected without replacing files')
t = fixture('missing-runtime', skip='dlssnr_amd_pass2.dll'); execute(t, expect=1); unchanged(t)
print('PASS missing runtime rejected without replacing files')
t = fixture('wrong-runtime', skip='dlssnr_amd_pass1.dll')
# This file was deliberately not hardlinked: never write through links to real dependencies.
(t / 'dlssnr_amd_pass1.dll').write_bytes(b'wrong runtime')
execute(t, expect=1); unchanged(t); print('PASS wrong runtime hash rejected')
bad = run / 'tampered-package'; shutil.copytree(package, bad)
with (bad / 'payload/OptiScaler.ini').open('ab') as f: f.write(b'\nmodified')
t = fixture('tampered-payload'); execute(t, expect=1, pkg=bad); unchanged(t)
print('PASS modified package rejected before writing')
t = fixture('locked-second-file')
k = ctypes.WinDLL('kernel32', use_last_error=True)
k.CreateFileW.restype = ctypes.c_void_p
k.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
k.CloseHandle.argtypes = [ctypes.c_void_p]
h = k.CreateFileW(str(t / 'OptiScaler.ini'), 0x80000000, 1, None, 3, 0x80, None)
assert h != ctypes.c_void_p(-1).value
try: execute(t, expect=1)
finally: k.CloseHandle(h)
unchanged(t)
backup, = t.glob('OptiScaler-backup-*')
assert not (backup / 'new-files/winmm.dll').exists(), 'First replacement must have occurred before rollback'
print('PASS second replacement failure restores the first replaced DLL')
t = fixture('running-game')
fake = run / 'ffxiv_dx11.exe'
shutil.copy2(Path(os.environ['SystemRoot']) / 'System32/cmd.exe', fake)
process = subprocess.Popen([str(fake), '/c', 'set /p test_wait='], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
try:
    assert process.poll() is None
    execute(t, expect=1); unchanged(t)
finally:
    process.terminate(); process.wait(timeout=10)
print('PASS running-game process blocks installation')
print('ALL INSTALLER CHECKS PASSED; evidence:', run)
