"""Release x64 product and isolated tests; no game/launcher files are modified."""
from pathlib import Path
import argparse,os,subprocess,sys
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser()
p.add_argument('target',nargs='?',default='product',choices=['product','smoke','cancel','split','worker','continuity','fence'])
p.add_argument('--vs',type=Path,help='Visual Studio installation directory')
p.add_argument('--sdk',default='10.0.22621.0')
p.add_argument('--rebuild',action='store_true',help='Rebuild all product objects, including precompiled headers')
p.add_argument('--directx-headers',type=Path,default=root/'.deps/DirectX-Headers')
a=p.parse_args()
pf=Path(os.environ.get('ProgramFiles(x86)',r'C:\Program Files (x86)'))
vs=a.vs
if vs is None:
    vswhere=pf/'Microsoft Visual Studio/Installer/vswhere.exe'
    vs=Path(subprocess.check_output([str(vswhere),'-latest','-products','*','-requires','Microsoft.VisualStudio.Component.VC.Tools.x86.x64','-property','installationPath'],text=True).strip())
vc=max((vs/'VC/Tools/MSVC').iterdir(),key=lambda x:tuple(map(int,x.name.split('.'))))
sdk=pf/'Windows Kits/10';source=root/'src';amd=source/'OptiScaler/dlssnr/amd'
out=root/'.build';out.mkdir(exist_ok=True)
env={k.upper():v for k,v in os.environ.items()}
env['Path']=env.pop('PATH')
if a.target=='product':
    headers=a.directx_headers.resolve()/'include/directx'
    if not (headers/'d3dx12.h').is_file():p.error('DirectX-Headers missing; follow README.md or pass --directx-headers.')
    env['CL']='/I"'+str(headers)+'" '+env.get('CL','')
    env['MSBUILDDISABLENODEREUSE']='1'
    args=[str(vs/'MSBuild/Current/Bin/MSBuild.exe'),str(source/'OptiScaler.sln'),'/m:1','/nr:false','/t:Rebuild' if a.rebuild else '/t:Build','/p:Configuration=Release','/p:Platform=x64',f'/p:VCToolsVersion={vc.name}',f'/p:WindowsTargetPlatformVersion={a.sdk}','/p:PreBuildEventUseInBuild=false','/p:PostBuildEventUseInBuild=false','/v:minimal','/fl',f'/flp:logfile={out}/build.log;verbosity=normal']
    sys.exit(subprocess.call(args,cwd=source,env=env))
tests=root/'tests';bin_dir=out/'tests';bin_dir.mkdir(exist_ok=True)
obj=out/'obj';obj.mkdir(exist_ok=True)
sources=[tests/'smoke.cpp',amd/'AmdPreSr.cpp'];exe='amd-smoke.exe';defines=[]
names={'split':('split_test.cpp','split-test.exe'),'worker':('worker_completion_test.cpp','worker-completion-test.exe'),
       'continuity':('continuity_test.cpp','continuity-test.exe'),'fence':('fence_test.cpp','fence-test.exe')}
if a.target in names:
    cpp,exe=names[a.target];sources=[tests/cpp]
elif a.target=='cancel':
    original=(amd/'AmdPreSr.cpp').read_text(encoding='utf-8-sig');needle='if (split && !split->EndNative())'
    assert original.count(needle)==1
    fixture=out/'AmdPreSr-cancel-fixture.cpp'
    fixture.write_text('#include <cstdlib>\n'+original.replace(needle,
        'if (split && (!split->EndNative() || [] { static unsigned count=0; const char* at=std::getenv("AMD_TEST_CANCEL_AT"); return ++count==(at?std::strtoul(at,nullptr,10):1); }()))'),encoding='utf-8')
    sources=[tests/'smoke.cpp',fixture];exe='amd-smoke-cancel.exe';defines=['/DAMD_TEST_CANCEL']
incs=[amd,vc/'include']+[sdk/f'Include/{a.sdk}/{n}' for n in ['ucrt','shared','um','winrt']]
libs=[vc/'lib/x64']+[sdk/f'Lib/{a.sdk}/{n}/x64' for n in ['ucrt','um']]
args=[str(vc/'bin/Hostx64/x64/cl.exe'),'/nologo','/EHsc','/std:c++20','/MD','/O2',f'/Fo{obj}/',*defines,*[f'/I{x}' for x in incs],*[str(x) for x in sources],f'/Fe:{bin_dir/exe}','/link',*[f'/LIBPATH:{x}' for x in libs],'d3d12.lib','d3d11.lib','dxgi.lib','d3dcompiler.lib','bcrypt.lib','user32.lib']
sys.exit(subprocess.call(args,env=env,cwd=root))
