"""Add audited cockpit input definitions; preserve existing XML bytes/definitions.

This is a delivery/editor tool, never a runtime fallback. PSX 10.195 contracts
come from installed Variables/Network Documentation, panel constructors and
the Aerowinx manual. Boeing FCOM establishes physical control type independently.
"""
import os, tempfile
import argparse
import copy
import hashlib
import json
import re
from pathlib import Path
import xml.etree.ElementTree as ET

BASE = Path(__file__).resolve().parents[1]
REFERENCE = "PSX 10.195 Variables/Network Documentation; installed panel contract audit; Aerowinx Operations Manual; Boeing 747-400 FCOM"
ROTARIES = {29,31,32,33,50,52,53,54,76,*range(116,135),137,*range(154,162),184,185,186,187,192,193,213,214,215,235,236,263,265,274,292,301,319,320,321,322,358,420,421}
BRIGHTNESS = {
    7:"Overhead panel brightness",8:"Dome brightness",9:"Glareshield panel brightness",10:"Glareshield flood brightness",11:"Aisle panel brightness",12:"Aisle flood brightness",
    139:"Upper EICAS brightness",140:"Lower EICAS inner brightness",141:"Lower EICAS outer brightness",404:"CDU L brightness",405:"CDU C brightness",406:"CDU R brightness",
}
for seat,first in (("CPT",84),("FO",95)):
    for offset,name in enumerate(("PVD","Panel","Flood","Outboard DU","Weather radar","ND","Map")):
        BRIGHTNESS[first+offset]=f"{seat} {name} brightness"

def definitions(root):
    groups = {}
    def group(chapter,identity,name):
        key=(chapter,identity)
        if key not in groups: groups[key]=ET.Element("group",id=identity,name=name,section="controls")
        return groups[key]
    def analogue(chapter,identity,name,record,lo,hi,unit="",field=None,fields=None,detents=None,when=()):
        g=group(chapter,f"analogue.{chapter}","Cockpit analogue controls")
        attrs=dict(id=identity,name=name,record=record,layout="fields" if field is not None else "integer",semantics="absolute",min=str(lo),max=str(hi),pressed=str(hi),released=str(lo),writable="true",unit=unit,reference=REFERENCE)
        if field is not None: attrs.update(field=str(field),fields=str(fields))
        if detents: attrs["detents"]=",".join(map(str,detents))
        if record in {f"Qh{n}" for n in range(302,309)}: attrs.update({"baseline-min":"-4000","baseline-max":"4000"})
        a=ET.SubElement(g,"analogue-input",attrs)
        for w in when: a.append(copy.deepcopy(w))
        return a
    for chapter in root.findall("chapter"):
        ata=int(chapter.attrib["number"])
        for existing in chapter.findall("group"):
            actions=existing.findall("action")
            turns=[a for a in actions if a.attrib["operation"]=="step"]
            if len(turns)==2 and sorted(int(a.attrib["value"]) for a in turns)==[-1,1]:
                down=next(a for a in turns if int(a.attrib["value"])<0);up=next(a for a in turns if int(a.attrib["value"])>0)
                ET.SubElement(group(ata,f"encoder.{ata}","Native rotary encoders"),"encoder",id="encoder."+existing.attrib["id"],name=existing.attrib["name"],decrease=down.attrib["id"],increase=up.attrib["id"],reference=REFERENCE)
            if not actions: continue
            record=actions[0].attrib["record"]
            is_rotary=record.startswith("Qh") and int(record[2:]) in ROTARIES
            # ACP outer rotary selectors; TCAS altitude reporting/transponder/mode
            # are rotary. TCAS altitude-range field 2 is a rocker, so excluded.
            if record in ("Qs113","Qs114","Qs115") and int(actions[0].attrib.get("field","0"))>=13: is_rotary=True
            if record=="Qs117" and actions[0].attrib.get("field") in ("0","1","3"): is_rotary=True
            if not is_rotary: continue
            stable=[a for a in actions if a.attrib["operation"]=="position"]
            if record in ("Qh154","Qh156"): stable=[a for a in stable if a.attrib["value"]!="0"] # spring RESET
            if len(stable)<2: continue
            values=sorted(set(int(a.attrib["value"]) for a in stable))
            first=stable[0];ident="analogue."+existing.attrib["id"]
            name=existing.attrib["name"]+(" (HOLD/RUN; RESET uses Digital inputs)" if record in ("Qh154","Qh156") else " (OFF/ON; START uses Digital inputs)" if record=="Qh193" else "")
            analogue(ata,ident,name,record,min(values),max(values),"detent",int(first.attrib["field"]) if "field" in first.attrib else None,int(first.attrib["fields"]) if "fields" in first.attrib else None,values,first.findall("when"))
    for number,name in BRIGHTNESS.items(): analogue(33,f"analogue.qh{number}",name,f"Qh{number}",0,4713,"rad ×1000")
    volumes=("VHF L","VHF C","VHF R","FLT interphone","CAB interphone","PA","HF L","HF R","SAT L","SAT R","Speaker","VOR/ADF","Approach")
    for seat,record in (("L","Qs113"),("C","Qs114"),("R","Qs115")):
        for field,name in enumerate(volumes): analogue(23,f"analogue.{record.lower()}.field{field}",f"Audio {seat} {name} volume",record,0,99,"volume",field,16)
    for number,name in ((302,"Passenger"),(303,"Flight deck"),(304,"Main deck forward"),(305,"Main deck aft"),(306,"Lower lobe forward"),(307,"Lower lobe aft"),(308,"Cargo")):
        when=[]
        if number!=303: when=[ET.Element("when",record="Qi0",values="1,3" if number in (304,305,306,307) else "0,2,4")]
        analogue(21,f"analogue.qh{number}",name+" temperature C–W",f"Qh{number}",-2400,2400,"rad ×1000; automatic sector",when=when)
    analogue(21,"analogue.qh297","Landing altitude knob","Qh297",0,62830,"rad ×1000; ten turns")
    analogue(28,"analogue.qh273","Jettison fuel-to-remain knob","Qh273",0,62830,"rad ×1000; ten turns")
    for number in (3,4,5): analogue(34,f"analogue.qh{number}",f"Standby airspeed bug {number-2} absolute angle",f"Qh{number}",0,6283,"rad ×1000")
    for seat,tilt,gain in (("CPT",0,1),("FO",2,3)):
        analogue(34,f"analogue.qs105.field{tilt}",seat+" weather radar tilt","Qs105",0,4713,"rad ×1000",tilt,4)
        analogue(34,f"analogue.qs105.field{gain}",seat+" weather radar gain","Qs105",-6,3,"detent",gain,4,list(range(-6,4)))
    return groups

def merge(file,report=None):
    text=file.read_text(encoding="utf-8-sig");root=ET.fromstring(text);original=ET.fromstring(text)
    additions=definitions(root);existing={e.attrib["id"] for tag in ("encoder","analogue-input") for e in root.iter(tag)};added=0
    for chapter in root.findall("chapter"):
        number=int(chapter.attrib["number"]);payload=[]
        for (ata,identity),g in additions.items():
            if ata!=number: continue
            g[:]=[e for e in g if e.attrib["id"] not in existing]
            if not len(g): continue
            for e in g: existing.add(e.attrib["id"])
            added+=len(g);ET.indent(g,space="  ",level=2);payload.append("    "+ET.tostring(g,encoding="unicode"))
        if payload:
            pattern=rf'(<chapter\b[^>]*\bnumber="{number}"[^>]*>[\s\S]*?)(\s*</chapter>)'
            text,count=re.subn(pattern,lambda m:m.group(1)+"\n"+"\n".join(payload)+m.group(2),text,count=1)
            assert count==1
    text=re.sub(r'(<catalogue\s+version=")[1234](")',r'\g<1>4\2',text,count=1)
    if added: text=text.replace(root.attrib["revision"],root.attrib["revision"]+" + cockpit 1.12.0",1)
    result=ET.fromstring(text)
    for tag in ("action","target","observation","digital-output"):
        assert [ET.tostring(e) for e in original.iter(tag)]==[ET.tostring(e) for e in result.iter(tag)],f"Existing {tag} changed"
    if added:
        with tempfile.NamedTemporaryFile(mode="w",encoding="utf-8",newline="\n",dir=file.parent,delete=False) as stream:
            stream.write(text);temporary=stream.name
        os.replace(temporary,file)
    evidence={"added":added,"encoders":len(list(result.iter("encoder"))),"analogue":len(list(result.iter("analogue-input"))),"preserved_existing_tags":["action","target","observation","digital-output"],"definitions":[{"type":e.tag,**e.attrib,"when":[w.attrib for w in e.findall("when")]} for tag in ("encoder","analogue-input") for e in result.iter(tag)],"exclusions":{"flight_control_axes":"Remain in original flight-axis pane","spring_commands":"APU START, clock RESET, temperature MAN/ALTN and spring warm/cool remain digital","EFIS_bearings":"ADF/OFF/VOR lever contacts remain digital","TCAS_altitude_range":"Above/NORM/Below rocker remains digital","door":"Relative spring command remains digital","SmokeEvac":"Cockpit non-rotary mechanism, not a potentiometer/selector","brightness_encoder":"No native turn command; analogue only"}}
    if report: report.parent.mkdir(parents=True,exist_ok=True);report.write_text(json.dumps(evidence,indent=2,ensure_ascii=False)+"\n",encoding="utf-8")
    print(json.dumps({k:v for k,v in evidence.items() if k in ("added","encoders","analogue")}))

if __name__=="__main__":
    parser=argparse.ArgumentParser();parser.add_argument("file",type=Path);parser.add_argument("--report",type=Path);args=parser.parse_args();merge(args.file,args.report)
