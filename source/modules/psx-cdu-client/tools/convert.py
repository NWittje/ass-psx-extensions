from pathlib import Path
import re,subprocess,json,hashlib,argparse
root=Path(__file__).resolve().parents[3]
source=root/'shared/cdu/shaders/crt-royale';out=root/'shared/cdu/shaders/generated';out.mkdir(exist_ok=True)
parser=argparse.ArgumentParser();parser.add_argument('--tools-root',type=Path,required=True);parser.add_argument('--fxc',type=Path,default=Path('C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/fxc.exe'));parser.add_argument('--toolchain-manifest',type=Path,help='Explicit qualified replacement compiler identities; shader dependencies stay pinned');parser.add_argument('--output-dir',type=Path,help='Isolated recovery generation destination');args=parser.parse_args()
if args.output_dir:
 out=args.output_dir.resolve();out.mkdir(parents=True,exist_ok=True)
tools=args.tools_root
glsl=tools/'build-glslang/StandAlone/Release/glslang.exe';cross=tools/'build-cross/Release/spirv-cross.exe'
fxc=args.fxc
manifest=json.loads((source.parent/'manifest.json').read_text(encoding='utf-8'))
tool_identity=json.loads(args.toolchain_manifest.read_text(encoding='utf-8')) if args.toolchain_manifest else manifest
if tool_identity['glslang']!=manifest['glslang'] or tool_identity['spirv_cross']!=manifest['spirv_cross']:raise RuntimeError('Replacement toolchain source revision differs')
for tool in [glsl,cross,fxc]:
 expected=next(t['sha256'] for t in tool_identity['tools'] if Path(t['path']).name==tool.name)
 if hashlib.sha256(tool.read_bytes()).hexdigest()!=expected:raise RuntimeError('Pinned compiler hash differs: '+str(tool))
for item in manifest['files']:
 if item['file'].replace('\\','/').startswith('crt-royale/') and hashlib.sha256((source.parent/item['file']).read_bytes()).hexdigest()!=item['sha256']:raise RuntimeError('Pinned upstream dependency differs: '+item['file'])
def run(cmd):
 p=subprocess.run(list(map(str,cmd)),capture_output=True,text=True)
 if p.returncode:raise RuntimeError(' '.join(map(str,cmd))+'\n'+p.stdout+p.stderr)
 return p.stdout
preset=dict(re.findall(r'^(\w+)\s*=\s*"([^\"]*)"', (source/'crt/crt-royale.slangp').read_text(encoding='utf-8'),re.M))
def expand(p):
 return re.sub(r'^\s*#include\s+"([^"]+)".*$',lambda m:expand((p.parent/m[1]).resolve()),p.read_text(encoding='utf-8'),flags=re.M)
params={}
for p in source.rglob('*'):
 if p.suffix in ['.h','.slang']:
  for id,label,d,lo,hi,step in re.findall(r'#pragma parameter\s+(\w+)\s+"([^\"]*)"\s+([-\d.eE]+)\s+([-\d.eE]+)\s+([-\d.eE]+)\s+([-\d.eE]+)',p.read_text(encoding='utf-8')):
   params[id]=dict(id=id,label=label,default=float(d),minimum=float(lo),maximum=float(hi),step=float(step))
for id,val in {'geom_aspect_ratio_x':372/315,'geom_aspect_ratio_y':1,'geom_mode_runtime':0,'geom_overscan_x':1,'geom_overscan_y':1,'interlace_detect_toggle':0,'interlace_1080i':0,'border_size':0.0000001}.items():
 if id in params:params[id]['default']=val
# Pinned CDU progressive profile fixes interlace controls, leaving geometry editable.
for id in ['interlace_detect_toggle','interlace_1080i','interlace_bff','aa_subpixel_r_offset_x_runtime','aa_subpixel_r_offset_y_runtime']:
 if id in params:params[id]['fixed']=True
registry={'revision':'CRT-Royale CDU revision 1','parameters':list(params.values()),'passes':[],'lookups':[]}
for i in range(12):
 p=(source/'crt'/preset['shader'+str(i)]).resolve();flat=expand(p)
 info={'index':i,'source':str(p.relative_to(source)),'alias':preset.get('alias'+str(i),''),'stages':[]}
 for stage,ext,profile in [('vertex','vert','vs_5_0'),('fragment','frag','ps_5_0')]:
  current=None;lines=[]
  for line in flat.splitlines():
   m=re.match(r'\s*#pragma stage\s+(\w+)',line)
   if m:current=m[1];continue
   if current is None or current==stage:lines.append(line)
  text='\n'.join(lines)+'\n'
  text=re.sub(r'#pragma (parameter|format)[^\n]*','',text)
  # Move push constants to a normal std140 UBO. b0 remains ASS's established frame ABI.
  text=text.replace('layout(push_constant)','layout(std140, set=0, binding=1)')
  text=text.replace('set = 0, binding = 0','set = 0, binding = 1')
  # Distinct regular global/push CB slots. Vulkan source binding1 push -> binding2.
  text=text.replace('layout(std140, set=0, binding=1)','layout(std140, set=0, binding=2)')
  path=out/f'{i}.{ext}';path.write_text(text,encoding='utf-8')
  spv=out/f'{i}.{ext}.spv';run([glsl,'-V','-S',ext,'-o',spv,path])
  reflect=json.loads(run([cross,spv,'--reflect']));(out/f'{i}.{ext}.json').write_text(json.dumps(reflect,indent=2),encoding='utf-8',newline='\n')
  hlsl=run([cross,spv,'--hlsl','--shader-model','50'])
  if stage=='vertex':
   # Generated varying semantics preserved. Replace the input struct with SV_VertexID,
   # keeping upstream vertex calculations and UV origin (top-left).
   pattern=r'SPIRV_Cross_Output main\(SPIRV_Cross_Input stage_input\)\s*\{'
   fields=re.findall(r'\s+(float[234]?)\s+(\w+)\s*:\s*TEXCOORD(\d+)',re.search(r'struct SPIRV_Cross_Input\s*\{(.*?)\};',hlsl,re.S)[1])
   init='SPIRV_Cross_Input stage_input;float2 cdu_uv=float2((vertex_id<<1)&2,vertex_id&2);'
   for typ,name,loc in fields:
    init+=f'stage_input.{name}='+('float4(cdu_uv.x*2-1,1-cdu_uv.y*2,0,1);' if int(loc)==0 else 'cdu_uv;')
   hlsl,n=re.subn(pattern,'SPIRV_Cross_Output main(uint vertex_id : SV_VertexID)\n{\n'+init,hlsl)
   if n!=1:raise RuntimeError('Cannot adapt vertex signature')
  hp=out/f'{i}.{ext}.hlsl';hp.write_text(hlsl,encoding='utf-8')
  run([fxc,'/nologo','/O3','/T',profile,'/E','main','/Fo',out/f'{i}.{ext}.cso',hp])
  info['stages'].append({'stage':stage,'reflection':reflect})
 registry['passes'].append(info)
 print('converted',i,flush=True)
registry['preset']=preset
(out/'registry.json').write_text(json.dumps(registry,indent=2),encoding='utf-8',newline='\n')
print('Complete conversion',flush=True)
run([__import__('sys').executable,Path(__file__).parent/'generate-registry.py',*(['--output-dir',out] if args.output_dir else [])])
