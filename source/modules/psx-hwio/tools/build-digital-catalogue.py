"""Explicit source catalogue upgrade from the audited register, never runtime fallback."""
from pathlib import Path
import argparse
import re
import hashlib
import json
import xml.etree.ElementTree as E

ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--variables',type=Path,required=True)
parser.add_argument('--shutter-source',type=Path,required=True)
parser.add_argument('--shutter-sha256',required=True)
parser.add_argument('--catalogue-output',type=Path,required=True)
parser.add_argument('--evidence-output-dir',type=Path,required=True)
args=parser.parse_args()
for supplied in (args.shutter_source,args.catalogue_output,args.evidence_output_dir):
    if not supplied.is_absolute() or supplied.drive.upper()!='P:':
        parser.error('Recovered analysis and generated artifacts require absolute P: paths')
shutter_source=args.shutter_source
if hashlib.sha256(shutter_source.read_bytes()).hexdigest()!=args.shutter_sha256.lower():
    parser.error('Shutter evidence does not match the explicitly qualified SHA-256')
if hashlib.sha256(args.variables.read_bytes()).hexdigest()!='57f9f27041ac791f79466650a447c98245ed774f6308d24a0b2d418c0ad30dc8':
    parser.error('Variables.txt does not match the audited PSX10.195 source')
args.evidence_output_dir.mkdir(parents=True,exist_ok=True)
path=ROOT/'config/catalogue.xml'
root=E.parse(path).getroot()
draft=E.parse(ROOT/'docs/evidence/digital-outputs-20261003/catalogue-draft.xml').getroot()
variables=args.variables.read_text(encoding='utf-8-sig')
ranges={r:(int(lo),int(hi)) for r,lo,hi in re.findall(r'(Q[shi]\d+)="[^"]+"; Mode=[^;]+; Min=(-?\d+); Max=(-?\d+);',variables)}
chapters={int(c.get('number')):c for c in root.findall('chapter')}
predicates={}
for action in root.findall('.//action'):
    if action.findall('when'):
        predicates.setdefault(action.get('record'),[(p.get('record'),p.get('values')) for p in action.findall('when')])

def operand(branch,record,test,mask=0,value=0,field=0,fields=1):
    low,high=ranges[record] if record.startswith(('Qh','Qi')) else (0,2000)
    E.SubElement(branch,'operand',dict(record=record,test=test,mask=str(mask),value=str(value),
        field=str(field),fields=str(fields),min=str(low),max=str(high)))

# Individually identified white legends, panel supplies and asserted captions.
shutters={}
for prefix,caption,count,field in [('Eec','NORM',4,6),('HydEng','ON',4,6),('NaiPush','ON',4,10)]:
    for n in range(1,count+1):shutters[prefix+str(n)]=(field,caption)
for name in ['FltDeckFan','TrimAir','RecircUpr','RecircLwr','HighFlow','Gasper','Humid',
             'AutoStart','JettNozzL','JettNozzR','FuelXfer','YawDamperUpr','YawDamperLwr']:
    shutters[name]=(6,'ON')
shutters.update({'FireArmFwd':(6,'ARMED'),'FireArmAft':(6,'ARMED'),'FireArmMain':(6,'ARMED'),
                 'IgnitCon':(10,'ON'),'WaiPush':(10,'ON')})

count=qualified=0
for chapter in draft.findall('chapter'):
    ata=int(chapter.get('number'));parent=chapters[ata]
    # Re-running replaces only generated groups in the source default.
    for old in list(parent):
        if old.tag=='group' and old.get('id','').startswith('digital-outputs.'+str(ata)):parent.remove(old)
    group=E.SubElement(parent,'group',dict(id='digital-outputs.'+str(ata),name='Digital outputs',section='controls'))
    for candidate in chapter.findall('candidate'):
        a=candidate.attrib;record=a['records'].split(',')[0];formula=a['decoder'];name=a['name']
        is_shutter=a['evidence']=='review-shutter' and a['id'][4:-8].lower() in {n.lower() for n in shutters}
        available=a['evidence'] in ['documented','documented-supply'] or is_shutter
        kind='shutter' if '.shutter' in a['id'] else 'lighting' if a['evidence']=='documented-supply' else 'actuator' if (a['id'].endswith('.unlock') or 'solenoid' in formula or 'actual' in formula or a['id'].startswith(('out.stickshaker','out.spdbrk','out.aircon'))) else 'lamp'
        out=E.SubElement(group,'digital-output',dict(id=a['id'],name=name,kind=kind,qualified='false',
            reference='PSX 10.195 installed developer contracts and power tables; Operations Manual p111-113; HWIO digital output audit 2026-10-03',
            unavailable=a['note'] or a['evidence']))
        if a['id'].startswith(('out.mastwarncp.','out.mastwarnfo.')):
            out.set('reference','PSX 10.195 BIGMOM and indicator-power contracts; installed MasterWarningSwitches.drawIt/drawThem and MasterDimAndTest.sendQsMdtVoltages; HWIO master-lamp qualification 2026-10-03')
        count+=1
        if not available:continue
        branch=E.SubElement(out,'branch')
        if is_shutter:
            key=next(n for n in shutters if n.lower()==a['id'][4:-8].lower());field,caption=shutters[key]
            out.set('name',name.replace('Shutter illumination candidate: ','')+' '+caption+' illuminated legend')
            operand(branch,record,'any',1);operand(branch,'Qs324','positive',field=field,fields=22)
        elif 'each MCP' in formula or 'bulb2-good' in formula:
            out.remove(branch)
            for fail,power in [(2,8),(4,11)]:
                b=E.SubElement(out,'branch');operand(b,record,'any',8200);operand(b,record,'clear',fail)
                operand(b,'Qs323','positive',field=power,fields=16)
        elif 'mic==' in formula:
            i=int(re.search(r'mic==(\d+)',formula)[1]);power=int(re.search(r'power-bit(\d+)',formula)[1])
            recv=a['records'].split(',')[1]
            operand(branch,record,'equal',value=i);operand(branch,'Qi175','any',power)
            b=E.SubElement(out,'branch');operand(b,recv,'any',8192);operand(b,'Qi175','any',power)
        else:
            if match:=re.search(r'contact-any:(\d+)\|8192',formula):operand(branch,record,'any',int(match[1])+8192)
            elif match:=re.search(r'contact-bit(\d+)',formula):operand(branch,record,'any',int(match[1]))
            elif match:=re.search(r'power(?:ed)?-bit(\d+)',formula):operand(branch,record,'any',int(match[1]))
            elif formula.startswith('value==1'):operand(branch,record,'equal',value=1)
            elif match:=re.match(r'bit(\d+)',formula):operand(branch,record,'any',int(match[1]))
            if match:=re.search(r'bulbs-not-all-failed:(\d+)',formula):operand(branch,record,'not-all',int(match[1]))
            if match:=re.search(r'NOT failed-bit(\d+)',formula):operand(branch,record,'clear',int(match[1]))
            if match:=re.search(r'illuminated-bit(\d+)',formula):operand(branch,record,'any',int(match[1]))
            if match:=re.search(r'Qi175 power-bit(\d+)',formula):operand(branch,'Qi175','any',int(match[1]))
            if match:=re.search(r'(Qs32[34])\[(\d+)\]>0',formula):
                operand(branch,match[1],'positive',field=int(match[2]),fields=16 if match[1]=='Qs323' else 22)
        if not list(branch) and len(out.findall('branch'))==1:
            raise RuntimeError('Unsupported qualified decoder '+a['id']+' '+formula)
        # The bulb branch case removed the original branch; the real branches remain.
        for p,values in predicates.get(record,[]):E.SubElement(out,'when',dict(record=p,values=values))
        if a['applicability']!='all' and not predicates.get(record) and not is_shutter:
            # No configuration guesses for squib layouts, variant controls or doors.
            for b in list(out):out.remove(b)
            out.set('unavailable','Aircraft applicability predicate needs qualification: '+a['applicability'])
            continue
        out.set('qualified','true');out.set('unavailable','');qualified+=1
# Apply the reviewed producer/renderer contracts after the original qualification register.
# This is a source build tool; runtime never creates or repairs catalogue definitions.
supplement=json.loads((ROOT/'docs/evidence/output-contracts-20261004/contracts.json').read_text(encoding='utf-8'))
for contract in supplement['contracts']:
    existing=next((n for n in root.findall('.//digital-output') if n.get('id')==contract['id']),None)
    if existing is not None:
        old_parent=next(g for g in root.findall('.//group') if existing in list(g))
        old_parent.remove(existing)
        ata=int(next(c.get('number') for c in root.findall('chapter') if old_parent in list(c)))
    else:ata=contract['chapter']
    parent=chapters[ata]
    group_id='digital-outputs.'+str(ata)+('.exterior' if contract.get('group')=='Exterior lighting' else '')
    group=next((g for g in parent.findall('group') if g.get('id')==group_id),None)
    if group is None:group=E.SubElement(parent,'group',dict(id=group_id,name=contract.get('group','Digital outputs'),section='controls'))
    attributes={key:str(contract[key]).lower() if isinstance(contract[key],bool) else str(contract[key]) for key in ['id','name','kind','qualified','reference','unavailable']}
    if contract.get('restricted'):attributes['restricted']=contract['restricted']
    out=E.SubElement(group,'digital-output',attributes)
    for branch in contract['branches']:
        b=E.SubElement(out,'branch')
        for term in branch:E.SubElement(b,'operand',term)
    for term in contract.get('requirements',[]):E.SubElement(out,'require',term)
    for predicate in contract['applicability']:E.SubElement(out,'when',predicate)
    if contract['id'] not in {c.get('id') for c in draft.findall('.//candidate')}:
        chapter=next(c for c in draft.findall('chapter') if int(c.get('number'))==ata)
        E.SubElement(chapter,'candidate',dict(id=contract['id'],name=contract['name'],records=','.join(sorted({term['record'] for b in contract['branches'] for term in b})),decoder='Audited typed contract in output-contracts-20261004/contracts.json',applicability='audited',evidence='audited-installed-contract',note=contract['proof']))
for chapter in root.findall('chapter'):
    for group in chapter.findall('group'):
        if group.get('id','').startswith('digital-outputs.'):
            group[:]=sorted(list(group),key=lambda n:(n.get('name','').casefold(),n.get('id','')))
            if group.get('id')=='digital-outputs.33':group.set('name','Cockpit lighting')
count=len(root.findall('.//digital-output'));qualified=sum(n.get('qualified')=='true' for n in root.findall('.//digital-output'))
root.set('version','3');root.set('revision','PSX-10.195-HWIO-1.11.3')
E.indent(root,space='  ');E.ElementTree(root).write(args.catalogue_output,encoding='utf-8',xml_declaration=True)
print(f'{count} digital output definitions, {qualified} qualified, {count-qualified} unavailable')
definitions={out.get('id'):out for out in root.findall('.//digital-output')}
for candidate in draft.findall('.//candidate'):
    out=definitions[candidate.get('id')]
    candidate.set('implementation-qualified',out.get('qualified'))
    candidate.set('implementation-reason',out.get('unavailable'))
    contract=next((c for c in supplement['contracts'] if c['id']==candidate.get('id')),None)
    if contract:
        candidate.set('disposition',contract['disposition']);candidate.set('qualification-proof',contract['proof'])
draft.set('implementation','HWIO 1.11.3 schema 3; source-contract qualification only')
draft.set('status','audited');draft.set('audit-updated','2026-10-04')
E.indent(draft,space='  ')
E.ElementTree(draft).write(args.evidence_output_dir/'catalogue-draft.xml',encoding='utf-8',xml_declaration=True)
register=dict(version='1.11.3',candidate_definitions=count,qualified_definitions=qualified,
    unavailable_definitions=count-qualified,physical_measurement=False,
    shutter_contract='BigSwitch.drawWord color1 skips bulb/contact checks at offsets0-26, uses panel lettering at285-318, exposes lettering only at pressed depth at400-443. BigSwitch.runDelta reads pressed bit1 at55-70. Installed panel power table supplies gT38 field6 and gR8098c field10; manual p112 distinguishes illumination from daylight visibility.',
    static_shutter_evidence=dict(path=str(shutter_source),sha256=hashlib.sha256(shutter_source.read_bytes()).hexdigest()),
    definitions=[dict(id=key,qualified=out.get('qualified')=='true',kind=out.get('kind'),reason=out.get('unavailable'),
        branches=[[node.attrib for node in branch] for branch in out.findall('branch')],requirements=[n.attrib for n in out.findall('require')],restricted=out.get('restricted',''),applicability=[node.attrib for node in out.findall('when')]) for key,out in definitions.items()])
(args.evidence_output_dir/'qualification.json').write_text(json.dumps(register,indent=2)+'\n',encoding='utf-8')
