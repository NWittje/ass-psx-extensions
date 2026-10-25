"""Generate our manifest/header and optional PSX aircraft entry from local assets.

No bank/audio extraction and no edits to the source ACF/SND/banks. By default
outputs stay under this module. --install-entry explicitly creates the two
additional aircraft files; parent deployment owns invoking that option.
"""
import argparse, hashlib, json, pathlib, re

MODULE = pathlib.Path(__file__).resolve().parents[1]
SOURCE_HASH = 'b9edc9c710ad6ef31e2514c411955d8d7bb2a3f1f30a9e2b4f19a7309f17a29a'
GROUPS = dict(engines=0, gear=1, flaps=2, apu=3, packs=4, hydraulic=5, fuel=6, airflow=7)

def identity(path):
    data = path.read_bytes()
    return dict(path=str(path), bytes=len(data), sha256=hashlib.sha256(data).hexdigest())

def generate(aircraft, inspection, output, install=False):
    original = aircraft / '747-400.acf'
    data = original.read_bytes()
    if hashlib.sha256(data).hexdigest() != SOURCE_HASH:
        raise ValueError('Sparky source ACF identity changed')
    # Description only; every geometry/system/asset property is preserved.
    adapted, count = re.subn(rb'(?m)^(P acf/_name[^\r\n]*)', rb'\1 PSX', data)
    if count != 1:
        raise ValueError('Expected one ACF name')
    source_snd = aircraft / 'fmod' / '747-400.snd'
    snd_text=source_snd.read_text()
    if re.search(r'(?m)^\s*REF_POINT_ACF\b',snd_text):
        raise ValueError('SND authoring reference changed; review event geometry')
    spaces=re.findall(r'BEGIN_SOUND_SPACE.*?END_SOUND_SPACE',snd_text,re.S)
    blocks = re.findall(r'BEGIN_SOUND_ATTACHMENT(.*?)END_SOUND_ATTACHMENT', snd_text, re.S)
    bank_data = [json.loads(line) for line in inspection.read_text().splitlines() if line.strip()]
    event_details_path=MODULE/'docs/evidence/sparky-fmod-events-20261002.json'
    event_details={e['path']:e for e in json.loads(event_details_path.read_text())['events']}
    events = {e['event'].lower(): (b, e) for b in bank_data for e in b['examples']}
    specs, omitted = [], []
    fuel_slots = {}
    for tank in range(1,5):
        for aft,word in enumerate(('fwd','aft')):
            fuel_slots[f'main{tank}_tank/main_pump_{word}_on']=(tank-1)*2+aft
    for tank,base in ((2,8),(3,10)):
        for aft,word in enumerate(('fwd','aft')):
            fuel_slots[f'main{tank}_tank/ovrd_jett_pump_{word}_on']=base+aft
    for side,slot in (('L',12),('R',13)):
        fuel_slots[f'center_tank/ovrd_jett_pump_{side}_on']=slot
    for side,slot in (('L',14),('R',15)):
        fuel_slots[f'stab_tank/xfr_jett_pump_{side}_on']=slot
    def add(path, group, index, trigger, xyz, family=0, evidence=None):
        b, e = events[path.lower()]
        runtime=event_details[e['event']]
        specs.append(dict(path=e['event'], bank=b['bank'], group=group, index=index,
                          trigger=trigger, position=xyz, family=family,
                          guid_bytes=runtime['guid_bytes'],oneshot=runtime['oneshot'],is3d=runtime['is3d'],
                          parameters=[dict(name=n, metadata=b['parameters'][n]) for n in e['params']],
                          source_attachment=evidence))
    for block in blocks:
        match = re.search(r'EVENT_NAME\s+(\S+)', block)
        if not match:
            continue
        path = 'event:' + match[1]
        pos = re.search(r'VEH_XYZ\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)', block)
        xyz = list(map(float, pos.groups())) if pos else [0.,0.,0.]
        index = re.search(r'\[(\d+)\]', block)
        index = int(index[1]) if index else 0
        metadata = dict(trigger_lines=[l.strip() for l in block.splitlines() if 'COND' in l], position=xyz)
        family = re.search(r'/Engines/(GE|PW|RR)/', path)
        if family:
            f = dict(GE=1,PW=2,RR=3)[family[1]]
            kind = 'engine_run' if re.search(r'/engine[1-4]$', path) else ('engine_crank' if re.search(r'/start1_[1-4]$',path)
                else ('engine_shutdown' if path.endswith('/shutdown') else 'engine_ignite'))
            add(path,'engines',index,kind,xyz,f,metadata)
        elif '/Engines/Blast/' in path:
            add(path,'engines',index,'reverse',xyz,evidence=metadata)
        elif '/Ambience/gear/gear_wind' in path:
            # ACF: nose,bodyR,bodyL,wingR,wingL; PSX: nose,wingL,wingR,bodyL,bodyR.
            if index < 5: add(path,'gear',[0,4,3,2,1][index],'gear',xyz,evidence=metadata)
        elif '/Ambience/HYD/hyd' in path:
            add(path,'hydraulic',int(path[-1])-1,'hydraulic',xyz,evidence=metadata)
        elif path.endswith('/pack_ext/pack_ext'):
            add(path,'packs',index,'pack',xyz,evidence=metadata)
        elif '/Cabin/Passenger/packs' in path:
            # Installed L/C attachments use pack0/pack2. Duplicate LOFF block
            # represents the second stop condition, not a second sound source.
            if path.endswith('LOFF') and any(e['path']==path for e in specs):
                continue
            add(path,'packs',index,'pack_stop' if path.endswith('OFF') else 'pack',xyz,evidence=metadata)
        elif path.endswith('/APU/apu') or path.endswith('/APU/apu_start') or path.endswith('/APU/apu_stop'):
            trigger = 'apu_run' if path.endswith('/apu') else ('apu_start' if path.endswith('_start') else 'apu_stop')
            add(path,'apu',0,trigger,xyz,evidence=metadata)
        elif path.endswith('/Ambience/in_wind') or path.endswith('/Ambience/windX'):
            add(path,'airflow',0,'wind',xyz,evidence=metadata)
        elif '/Ambience/fuel' in path:
            source = re.search(r'laminar/B747/fuel/(\S+)\s+==',block)
            if not source or source[1] not in fuel_slots:
                raise ValueError('Unknown physical fuel attachment: '+block)
            add(path,'fuel',fuel_slots[source[1]],'fuel_stop' if path.endswith('_off') else 'boolean',xyz,evidence=metadata)
        elif '/Ambience/Flaps/' in path:
            # Generated separately below from verified source grouping. Preserve original evidence.
            omitted.append(dict(path=path,reason='See explicit source-group mapping',attachment=metadata))
    # Reuse installed wing attachments without manufacturing six separate LE positions.
    for group_indices, stem, xyz in [([6,7],'flaps',[-7,.8,3]),([8,9],'flaps',[7,.8,3]),
                                     ([0,1,2],'slats',[-16,1,0]),([3,4,5],'slats',[-16,1,0])]:
        field_mask=sum(1 << (i+10) for i in group_indices)
        for suffix,trigger in [('', 'flap_move'),('_stop','flap_stop')]:
            add('event:/Boeing/747/-400/Ambience/Flaps/'+stem+suffix,'flaps',field_mask,trigger,xyz,
                evidence='Actual inTransit OR for the named wing group; no position or command-derived edge')
    omitted.append(dict(group='fuel',fields=[16,17,18],reason='PSX AuxL/AuxR/Scavenge have no qualified installed acoustic attachment; no invented placement'))
    for path in ('snapshot:/EngineOption','snapshot:/lower_deck','snapshot:/all'):
        add(path,'engines',0,'snapshot',[0.,0.,0.],evidence='Owned snapshot; no shared listener/master writes')
    specs.sort(key=lambda e: (0 if e['family']==3 else 1, e['group'], e['path'], e['index']))
    names = sorted(set(e['bank'] for e in specs) | {'Master Bank.bank','Master Bank.strings.bank'})
    snd = ('A\n1000\nACF_SOUNDS\n# PSX-owned instances; native attachments intentionally absent.\n' +
           ''.join('REQUIRES_BANK '+n+'\n' for n in names if not n.startswith('Master Bank')) +
           'DISABLE_LEGACY_ALERT_SOUNDS\n'+''.join(block+'\n' for block in spaces)).encode()
    result = dict(source_acf=identity(original), source_snd=identity(source_snd),
                  metadata=identity(inspection), event_metadata=identity(event_details_path), banks=[identity(aircraft/'fmod'/n) for n in names],
                  psx_acf_sha256=hashlib.sha256(adapted).hexdigest(), psx_snd_sha256=hashlib.sha256(snd).hexdigest(),
                  sound_spaces=spaces, events=specs, omissions=omitted,
                  policy='Actual state only; absent fields unavailable; bank parameter metadata revalidated at runtime')
    output.mkdir(parents=True,exist_ok=True)
    (output/'sparky-audio-manifest.json').write_text(json.dumps(result,indent=2)+'\n')
    lines=['// Generated by scripts/prepare-sparky-audio.py; no vendor audio/source bundled.',
           'inline constexpr char sparky_psx_sha256[]="'+result['psx_acf_sha256'].upper()+'";',
           'inline constexpr EventSpec sparky_events[]{']
    for e in specs:
        coords=','.join(format(x,'.9g') for x in e['position'])
        lines.append('{'+json.dumps(e['path'])+','+json.dumps(e['bank'])+',audio::Group::'+e['group']+
                     ',audio::Family('+str(e['family'])+'),'+str(e['index'])+',Trigger::'+e['trigger']+',{'+coords+'},{'+
                     ','.join(map(str,e['guid_bytes']))+'},'+str(e['oneshot']).lower()+','+str(e['is3d']).lower()+'},')
    lines.append('};')
    lines.append('inline constexpr AssetSpec sparky_assets[]{')
    for b in result['banks']:
        lines.append('{'+json.dumps(pathlib.Path(b['path']).name)+','+str(b['bytes'])+',"'+b['sha256'].upper()+'"},')
    lines.append('{"747-400-PSX.snd",'+str(len(snd))+',"'+result['psx_snd_sha256'].upper()+'"},')
    lines.append('};')
    (output/'sparky_audio_manifest.inc').write_text('\n'.join(lines)+'\n')
    if install:
        for target, content in [(aircraft/'747-400-PSX.acf',adapted),(aircraft/'fmod'/'747-400-PSX.snd',snd)]:
            if target.exists() and target.read_bytes()!=content:
                raise ValueError('Refusing to overwrite a different existing adapter: '+str(target))
        (aircraft/'747-400-PSX.acf').write_bytes(adapted)
        (aircraft/'fmod'/'747-400-PSX.snd').write_bytes(snd)
    return result

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--aircraft',type=pathlib.Path,default=pathlib.Path('D:/X-Plane12/Aircraft/747-400'))
    p.add_argument('--inspection',type=pathlib.Path,default=MODULE.parents[1]/'docs/evidence/sparky-fmod-parameters-20261002.jsonl')
    p.add_argument('--output',type=pathlib.Path,default=MODULE/'assets');p.add_argument('--install-entry',action='store_true')
    a=p.parse_args();r=generate(a.aircraft,a.inspection,a.output,a.install_entry)
    print(json.dumps(dict(events=len(r['events']),banks=len(r['banks']),acf_sha256=r['psx_acf_sha256'],installed=a.install_entry)))
