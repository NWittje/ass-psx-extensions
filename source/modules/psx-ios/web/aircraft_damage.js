'use strict';
(() => {
 function create(root,summary,availability){
 if(!root)return;
 const ns='http://www.w3.org/2000/svg';
 const engineBases=[160,167,180,184,192,209,213,217,221,225,229,233,237,245,254,258,262,266,566];
 const definitions=[
  ...[1,2,3,4].map(n=>({id:'engine-'+n,label:'Engine '+n,native:engineBases.map(b=>b+n-1)})),
  ...['nose','wing-left','wing-right','body-left','body-right'].map((name,i)=>({
   id:'gear-'+name,label:['Nose gear','Left wing gear','Right wing gear','Left body gear','Right body gear'][i],native:[144+i]})),
  {id:'aileron',label:'Aileron',native:[137],axis:2,control:'aileron'},
  {id:'elevator',label:'Elevator',native:[],axis:1,control:'elevator'},
  {id:'rudder',label:'Rudder',native:[],axis:4,control:'rudder'},
  {id:'stabilizer',label:'Stabilizer',native:[155,156],trim:true,control:'trim'}
 ];
 const catalogue=new Map(JSON.parse(root.querySelector('metadata').textContent).map(f=>[f.id,f]));
 const regions=new Map(definitions.map(d=>[d.id,{...d,nodes:[],state:null,detail:null}]));
 for(const node of root.querySelectorAll('[data-damage-region]')){
  const region=regions.get(node.getAttribute('data-damage-region'));
  if(!region)throw Error('Unknown aircraft damage region');
  const title=node.querySelector('title')||node.appendChild(document.createElementNS(ns,'title'));
  region.nodes.push({node,title});
 }
 const known=new Set(['off','armed','active']);
 let lastSummary='';
 function nativeDescription(id,setting){
  const info=catalogue.get(id),name=info?.name||'Qs'+id;
  const variant=setting.severe?info?.severe:info?.non_severe;
  const criterion=setting.state==='armed'?' Â· '+setting.criterion+' '+setting.value:'';
  return name+(variant?' Â· '+variant:'')+criterion;
 }
 function stateOf(region,snapshot,states,drafts){
  if(!snapshot?.ready)return {state:'unavailable',unavailable:true,detail:region.label+' â€” status unavailable'};
  let active=false,armed=false,staged=false,unavailable=false;
  const facts=[];
  for(const id of region.native){
   const setting=states.get(id);
   if(!setting||!known.has(setting.state)){unavailable=true;continue;}
   if(setting.state==='active'||setting.state==='armed'){
    active ||= setting.state==='active';armed ||= setting.state==='armed';
    facts.push(nativeDescription(id,setting)+' â€” '+(setting.state==='active'?'active':'armed'));
   }
  }
  if(region.control){
   const h=snapshot.non_normal?.hwio;
   if(!h?.available)unavailable=true;
   else{
    const running=region.trim?!!h.trim_direction:!!(h.axis_mask&region.axis);
    const arms=(h.arms||[]).filter(a=>region.trim?a.trim:(a.axis_mask&region.axis));
    if(running){active=true;facts.push('HWIO '+region.control+' â€” active command state');}
    for(const a of arms){armed=true;facts.push('HWIO '+region.control+' â€” armed'+(a.criterion?' Â· '+a.criterion:'')+(a.value!==undefined?' '+a.value:''));}
   }
  }
  for(const draft of drafts){
   if((draft.source==='psx'&&region.native.includes(draft.failure_id))||
      (draft.source==='hwio'&&region.control===draft.control)){
    staged=true;
    facts.push((draft.source==='psx'?nativeDescription(draft.failure_id,{...draft,state:'active'}):'HWIO '+region.control)+' â€” staged Now');
   }
  }
  // A known active fault wins even if another source or associated record is unavailable.
  const state=active?'active':staged?'staged':armed?'armed':unavailable?'unavailable':'clear';
  const label={active:'active',staged:'staged Now',armed:'armed',unavailable:'status unavailable',clear:'clear'}[state];
  return {state,unavailable,detail:region.label+' â€” '+label+(facts.length?': '+facts.join('; '):'')+
   (unavailable&&state!=='unavailable'?' Â· some status unavailable':'')};
 }
 function update(snapshot,draftValues=[]){
  const states=new Map((snapshot?.non_normal?.states||[]).map(s=>[s.id,s]));
  const drafts=[...draftValues],details=[];
  let unavailable=0;
  for(const region of regions.values()){
   const next=stateOf(region,snapshot,states,drafts);
   if(next.state!==region.state){
    for(const {node} of region.nodes)node.setAttribute('data-damage-state',next.state);
    region.state=next.state;
   }
   if(next.detail!==region.detail){
    for(const {title} of region.nodes)title.textContent=next.detail;
    region.detail=next.detail;
   }
   if(['active','armed','staged'].includes(next.state))details.push(next.detail);
   if(next.unavailable)++unavailable;
  }
  const text=!snapshot?.ready?'Failure status unavailable.':
   (details.length?details.join('. '):unavailable?'Failure status partly unavailable.':'Mapped components clear.')+
   (details.length&&unavailable?' '+unavailable+' component statuses unavailable.':'');
  const note=!snapshot?.ready?'Status unavailable':unavailable?'Some status unavailable':'';
  if(availability.textContent!==note)availability.textContent=note;
  if(text!==lastSummary){summary.textContent=text;root.setAttribute('aria-label','Boeing 747-400 top and front views. '+text);lastSummary=text;}
 }
 update(null);
 return {update};
 }
 globalThis.AircraftDamage=create(document.getElementById('aircraft-damage'),document.getElementById('aircraft-damage-summary'),document.getElementById('aircraft-damage-availability'));
 globalThis.createAircraftDamage=create;
})();
