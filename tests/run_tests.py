from pathlib import Path
import subprocess,os,json,csv,time,sys
root=Path(__file__).resolve().parents[1]/'.build/tests'
root.mkdir(parents=True,exist_ok=True)
runtime=Path(os.environ.get('AMD_TEST_RUNTIME_DIR',str(Path(__file__).resolve().parents[1]/'.runtime')))
if '--baseline' in sys.argv:raise SystemExit('Historical baseline binaries are not included in this compact source tree.')
cases=[
 ('game-shape',0,[1,2048,1968,0,0,0,0,0,0,1,0,0,0]),
 ('scale100',1,[1,256,192,24,0,0,0,0,0,1,0,0,0]),
 ('scale75',1,[1,512,384,0,0,0,0,0,0,.75,0,0,0]),
 ('scale25-timeout',1,[1,128,128,0,0,0,1,1,0,.25,0,0,0]),
 ('scales-multipass-queues',1,[3,256,192,24,1,1,0,0,1,1,0,1,0]),
 ('post-return-signal',1,[1,256,192,24,1,0,0,0,0,.5,1,0,0]),
 ('display-motion',1,[1,256,128,32,1,0,0,0,0,.5,0,0,1]),
 ('sync-control',0,[1,256,192,24,0,0,0,0,0,1,0,0,0]),
]
cases += [
 ('delayed-observation',1,[1,256,192,24,0,0,0,0,0,1,0,0,0,0,1,0]),
 ('busy-reset-pulse',1,[1,256,192,24,0,0,0,0,0,1,0,0,0,0,2,0]),
 ('invalid-reset-pulse',0,[1,256,192,24,0,0,0,0,0,1,0,0,0,0,3,0]),
 ('last-job-timeout',1,[1,128,128,0,0,0,1,1,0,.25,0,0,0,0,0,1]),
 ('sync-default',None,[1,256,192,24,0,0,0,0,0,1,0,0,0]),
]
final = '--final' in sys.argv
if final: cases.append(('signed-timeout',1,[1,128,128,0,0,0,1,1,0,.25,0,0,0,1]))
run_name = sys.argv[sys.argv.index('--run')+1] if '--run' in sys.argv else ('tests-final' if final else 'tests')
if '--case' in sys.argv:
    cases = [c for c in cases if c[0] == sys.argv[sys.argv.index('--case')+1]]
results=[]
for name,async_,args in cases:
    folder=root/run_name/name;folder.mkdir(parents=True,exist_ok=True)
    assert not (folder/'console.txt').exists(), 'Use a fresh test-run directory to preserve evidence'
    for f in ['dlssnr_amd_pass1.dll','dlssnr_amd_pass2.dll','dlssnr_amd_pass3.dll','dlssnr_on_amd_weights.bin']:
        if not (folder/f).exists():os.link(runtime/f,folder/f)
    (folder/'amd_presr_perf.ini').write_text(f'[Performance]\nWaitForDx11Input=0\nTimerResolution1ms=1\nTiming=1\nDiagnosticStages=1\nAsyncSinglePass={async_}\n')
    if async_ is None:
        p=folder/'amd_presr_perf.ini';p.write_text(p.read_text().replace('AsyncSinglePass=None\n',''))
    if '--diagnostics-off' in sys.argv:
        p=folder/'amd_presr_perf.ini';p.write_text(p.read_text().replace('DiagnosticStages=1','DiagnosticStages=0'))
    if '--phased' in sys.argv:
        p=folder/'amd_presr_perf.ini'
        p.write_text('[Performance]\nTimerResolution1ms=1\nTiming=1\nDiagnosticStages=0\nAsyncSinglePass=0\nPhasedSubmission=1\n')
    started=time.monotonic()
    with (folder/'console.txt').open('w',encoding='utf-8') as log:
        exe = 'amd-smoke-baseline.exe' if '--baseline' in sys.argv else 'amd-smoke.exe'
        if '--cancel-at' in sys.argv: exe='amd-smoke-cancel.exe'
        env=os.environ.copy()
        if '--cancel-at' in sys.argv:env['AMD_TEST_CANCEL_AT']=sys.argv[sys.argv.index('--cancel-at')+1]
        if '--tail-sync' in sys.argv:env['AMD_TEST_TAIL_SYNC']='1'
        if '--tail-async' in sys.argv:env['AMD_TEST_TAIL_ASYNC']='1'
        if '--frames' in sys.argv:env['AMD_TEST_FRAMES']=sys.argv[sys.argv.index('--frames')+1]
        if '--capture-dependency' in sys.argv:env['AMD_TEST_CAPTURE_DEPENDENCY']='1'
        if '--game-shape' in sys.argv:env['AMD_TEST_GAME_SHAPE']='1'
        if '--recovery' in sys.argv:env['AMD_TEST_RECOVERY']='1'
        if '--prepared' in sys.argv:env['AMD_TEST_PREPARED']='1'
        try: code=subprocess.call([str(root/exe),str(folder),*map(str,args)],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=45)
        except subprocess.TimeoutExpired:code='timeout'
    rows=[]
    for p in folder.glob('amd_presr_timing_test*_*.csv'):
        rows+=list(csv.DictReader(p.open()))
    result=dict(case=name,exit=code,elapsed=round(time.monotonic()-started,2),rows=len(rows),args=args)
    results.append(result)
    print(json.dumps(result),flush=True)
    if code!=0:print((folder/'console.txt').read_text(encoding='utf-8',errors='replace')[-6000:],flush=True)
    (root/(run_name+'-results.json')).write_text(json.dumps(results,indent=2))
    if code=='timeout':break

