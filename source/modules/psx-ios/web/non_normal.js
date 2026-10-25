'use strict';
(() => {
 const el=id=>document.getElementById(id), draft=new Map(), selected=new Set(), systems=new Map(), buttons=new Map();
 const modes={engine:'Fail',gear:'Locked up',display:'Blackout',control:'Fail',stabilizer:'Runaway',cdu:'Blackout'};
 let snapshot=null, catalogue=[], catalogueSelected=null, catalogueIds=null, catalogueLoading=false, catalogueAttempted=false, cataloguePaintKey=null, searchGeneration=0, armEntries=[], active=false, shear=null, ash=false, traffic=null, runway=null, runwayChoice=null, actorNotice='';
 const catalogueChoices=new Map(),quickFailureIds=new Set([209,210,211,212,213,214,215,216,254,255,256,257,144,145,146,147,148,311,310,313,312,178,179]);
 const armFields={altitude:'failure-arm-altitude',ias:'failure-arm-ias',minutes:'failure-arm-minutes'};
 const applied=new Map(), key=e=>e.source==='psx'?'psx:'+e.failure_id:e.source==='cdu'?'cdu:'+e.unit:'hwio:'+e.control;
 const groups=[
  {id:'engine',label:'Engines',items:['1','2','3','4'],modes:['Fail','Fire','Surge']},
  {id:'control',label:'Controls',items:['Aileron','Elevator','Rudder'],modes:['Fail']},
  {id:'stabilizer',label:'Stabilizer',items:['Stabilizer'],modes:['Runaway','Stuck']},
  {id:'gear',label:'Gear',items:['NG','MLG L','MLG R','BLG L','BLG R'],modes:['Locked up','Locked down']},
  {id:'display',label:'Displays',items:['CPT PFD','CPT ND','FO PFD','FO ND','Upper','Lower'],modes:['Blackout']},
  {id:'cdu',label:'CDUs',items:['L','C','R'],modes:['Blackout']}
 ];
 const api=(path,body)=>globalThis.IOS.api(path,body);
 function notice(text){el('failure-selection-note').textContent=text;}
 function pressed(b,on){globalThis.IOS.pressed(b,on);}
 function error(text){el('event-error-text').textContent=text;el('event-error-dialog').showModal();}
 function entry(group,index) {
  if(group==='engine')return {source:'psx',failure_id:(modes.engine==='Fail'?213:modes.engine==='Fire'?254:209)+index,severe:modes.engine!=='Fire'};
  if(group==='gear')return {source:'psx',failure_id:144+index,severe:modes.gear==='Locked up'};
  if(group==='display')return {source:'psx',failure_id:[311,310,313,312,178,179][index],severe:true};
  if(group==='control')return {source:'hwio',control:['aileron','elevator','rudder'][index]};
  if(group==='cdu')return {source:'cdu',unit:['L','C','R'][index]};
  return {source:'hwio',control:'trim'};
 }
 function chosen(){return [...new Map([...selected].map(id=>{const s=systems.get(id);return entry(s.group,s.index);}).concat([...catalogueChoices.values()]).map(e=>[key(e),e])).values()];}
 function supported(s) {
  if(s.group==='stabilizer'&&modes.stabilizer==='Stuck')return 'Stabilizer Stuck unsupported';
  if(s.group==='cdu')return snapshot?.non_normal?.cdu?.available?'':snapshot?.non_normal?.cdu?.reason||'CDU blackout service unavailable';
  if(s.group==='control'||s.group==='stabilizer')return snapshot?.non_normal?.hwio?.available?'':snapshot?.non_normal?.hwio?.reason||'HWIO unavailable';
  return '';
 }
 for(const group of groups) {
  const row=document.createElement('div');row.className='failure-group';
  const label=document.createElement('strong');label.textContent=group.label;
  const list=document.createElement('div');list.className='failure-system-buttons';
  group.items.forEach((name,index)=>{
   const id=group.id+index,s={group:group.id,index,name},b=document.createElement('button');systems.set(id,s);buttons.set(id,b);
   b.textContent=name;b.dataset.system=id;b.setAttribute('aria-pressed','false');
   b.addEventListener('click',()=>{selected.has(id)?selected.delete(id):selected.add(id);paint();});list.append(b);
  });
  const choices=document.createElement('div');choices.className='failure-mode-buttons';
  group.modes.forEach(mode=>{
   const b=document.createElement('button');b.textContent=mode;b.dataset.family=group.id;b.dataset.mode=mode;
   if(group.id==='stabilizer'&&mode==='Stuck'){b.disabled=true;b.title='Stabilizer Stuck unsupported';}
   b.addEventListener('click',()=>{modes[group.id]=mode;paint();});choices.append(b);
  });row.append(label,list,choices);el('failure-groups').append(row);
 }
 function nativeStates(){return new Map((snapshot?.non_normal?.states||[]).map(s=>[s.id,s]));}
 function systemState(s,states) {
  if(s.group==='engine')return [209+s.index,213+s.index,254+s.index].map(id=>states.get(id)?.state).find(v=>v==='active')||
   [209+s.index,213+s.index,254+s.index].map(id=>states.get(id)?.state).find(v=>v==='armed');
  if(s.group==='control'||s.group==='stabilizer'){
   const h=snapshot?.non_normal?.hwio,axis=s.group==='control'?[2,1,4][s.index]:0,trim=s.group==='stabilizer';
   if((axis&&(h?.axis_mask&axis))||(trim&&h?.trim_direction))return 'active';
   return h?.arms?.some(a=>(axis&&(a.axis_mask&axis))||(trim&&a.trim))?'armed':'';
  }
  if(s.group==='cdu'){const c=snapshot?.non_normal?.cdu;return c?.available&&c.bound&&(c.mask&[1,4,2][s.index])?'active':'';}
  return states.get(entry(s.group,s.index).failure_id)?.state;
 }
 function styleEvent(b,chosen,id){pressed(b,chosen);b.classList.toggle('event-draft',chosen);b.classList.toggle('event-applied',!chosen&&(applied.get(id)||0)>Date.now());}
 function paint() {
  const ready=!!snapshot?.ready,states=nativeStates(),h=snapshot?.non_normal?.hwio;
  el('failure-hwio-availability').textContent=ready&&!h?.available?'Controls / stabilizer: '+(h?.reason||'HWIO unavailable'):'';
  globalThis.AircraftDamage?.update(snapshot,draft.values());
  for(const [id,b]of buttons){const s=systems.get(id),why=supported(s);b.disabled=active||!ready||!!why;b.title=why;
   const st=systemState(s,states),staged=[...draft.values()].some(e=>key(e)===key(entry(s.group,s.index))||(s.group==='engine'&&e.source==='psx'&&[209+s.index,213+s.index,254+s.index].includes(e.failure_id)));
   pressed(b,selected.has(id));b.classList.toggle('staged-now',staged);b.classList.toggle('active-failure',st==='active'&&!staged);b.classList.toggle('armed',st==='armed'&&!staged);
   b.setAttribute('aria-label',s.name+(staged?' staged Now':st==='active'?(s.group==='cdu'?' blackout requested':' failed'):st==='armed'?' armed':'')+(why?' — '+why:''));
   if(s.group==='cdu'&&st==='active')b.title='Text blackout requested';
  }
  document.querySelectorAll('[data-family]').forEach(b=>pressed(b,modes[b.dataset.family]===b.dataset.mode));
  const usable=chosen().length&&[...selected].every(id=>!supported(systems.get(id)));
  const hasCdu=chosen().some(e=>e.source==='cdu');
  el('failure-now').disabled=!ready||active||!usable;el('failure-arm').disabled=!ready||active||!usable||hasCdu;
  el('failure-arm').title=hasCdu?'CDU blackout supports Now and Clear only':'';
  el('failure-clear-selected').disabled=!ready||active||!selected.size&&!catalogueChoices.size;el('failure-clear-all').disabled=active||!snapshot?.enabled;
  el('failure-execute-now').disabled=active||!ready||!draft.size;el('failure-draft-count').textContent=draft.size?'('+draft.size+')':'';
  el('crash-reset').disabled=active||!ready||snapshot?.command_busy;
  document.querySelectorAll('[data-shear]').forEach(b=>{styleEvent(b,shear===b.dataset.shear,'shear:'+b.dataset.shear);b.disabled=!ready||active;});
  styleEvent(el('vash-select'),ash,'ash');el('vash-select').disabled=!ready||active;
  el('weather-events-execute').disabled=!ready||active||(shear===null&&!ash);el('shear-off').disabled=!ready||active;el('vash-off').disabled=!ready||active;
  const w=snapshot?.non_normal?.weather_events;
  el('vash-description').textContent='(entry 10nm ahead,\ncenter 210nm ahead,\nabove FL'+(w?.ash_limit_fl===null||w?.ash_limit_fl===undefined?' unavailable':String(w.ash_limit_fl).padStart(3,'0'))+')';
  el('vash-description').title=w?.readout_notice||'';
  document.querySelectorAll('[data-tcas]').forEach(b=>{styleEvent(b,traffic===b.dataset.tcas,'traffic:'+b.dataset.tcas);b.disabled=!ready||active;});
  const actor=snapshot?.non_normal?.actor;
  const trafficState=el('traffic-events-state');if(!trafficState.textContent||trafficState.textContent===actorNotice){actorNotice=actor?.available?'':actor?.reason||'Coordinator runway aircraft unavailable';trafficState.textContent=actorNotice;}
  el('runway-actor-select').disabled=!ready||active||!actor?.available;el('runway-actor-select').title=actor?.available?'':actor?.reason||'Coordinator runway aircraft unavailable';
  styleEvent(el('runway-actor-select'),!!runway,'actor');
  el('traffic-events-execute').disabled=!ready||active||(!traffic&&!runway);
  el('runway-actor-remove').disabled=active||(!ready&&!actor?.available);
  el('failure-arm-send').disabled=active;el('failure-catalogue-variant').disabled=active||!ready||!catalogueSelected;renderCatalogue();
  if(h&&!h.available&&selected.size&&[...selected].some(id=>systems.get(id).group==='control'))notice(h.reason||'HWIO unavailable');
 }
 async function operation(path,body={}) {
  if(!snapshot)throw Error('PSX status unavailable');
  const receipt=await api('non-normal/'+path,{...body,request_id:globalThis.IOS.requestId(),expected_revision:snapshot.revision});
  const until=Date.now()+30000;
  while(Date.now()<until){const op=await api('operations/'+receipt.id);
   if(['observed','failed','superseded','indeterminate'].includes(op.state))return op;
   await new Promise(resolve=>setTimeout(resolve,200));
  }throw Error('Operation response unavailable');
 }
 async function action(callback,where='failure-selection-note') {
  if(active)return;active=true;paint();
  try{await callback();}catch(e){el(where).textContent=e.message;}finally{active=false;paint();}
 }
 function fullEntry(e,criterion='immediate',value=''){return {...e,criterion,value};}
 function stage(entries){if(new Set([...draft.keys(),...entries.map(key)]).size>32)throw Error('Select up to 32 failures');
  for(const e of entries)draft.set(key(e),e);notice('Staged — press Execute.');paint();}
 function operationText(op){
  if(!op||!['failure-batch','failure-set','failure-clear','failure-reset-all','tcas','crash-reset','runway-place','runway-remove','runway-candidates'].includes(op.kind))return null;
  const r=op.result||{};
  if(op.kind==='failure-reset-all'){
   const done=[],missing=[];
   (r.hwio_clear?.state==='accepted'?done:missing).push('HWIO');
   (r.cdu_clear?.state==='accepted'?done:missing).push('CDU');
   (r.native_action_returned?done:missing).push('PSX');
   return (done.length?done.join(' / ')+' reset sent':'Reset unavailable')+(missing.length&&done.length?' · '+missing.join(' / ')+' unavailable':'')+
    (r.configured_readback==='different'?' · PSX readback differs':r.configured_readback==='unavailable'?' · PSX readback unavailable':'')+
    (op.state!=='observed'?' · '+(r.native_error||r.native?.reason||op.detail):'');
  }
  if(op.state==='observed'&&r.configured_readback!=='different'&&r.configured_readback!=='unavailable'&&!r.hwio_error&&!r.cdu_error){
   if(op.kind==='tcas')return 'TCAS settings updated';
   if(op.kind==='crash-reset')return 'Crash reset sent';
   if(op.kind==='runway-place')return 'Runway aircraft requested';
   if(op.kind==='runway-remove')return 'Runway aircraft removal requested';
   if(op.kind==='runway-candidates')return 'Select a runway';
   return r.items?.length&&r.items.every(i=>i.source==='cdu')?'CDU text blackout requested':'Failure settings updated';
  }
  return op.detail+(r.hwio_error?' · '+r.hwio_error:'')+(r.cdu_error?' · '+r.cdu_error:'');
 }
 function showOperation(op,where){el(where).textContent=operationText(op)||globalThis.IOS.operationText(op);}
 function confirmed(op){return op.state==='observed'&&op.result?.configured_readback!=='different'&&op.result?.configured_readback!=='unavailable';}
 async function sendBatch(entries,actionName='set'){
  if(actionName==='clear')entries=entries.map(e=>e.source==='psx'?{source:e.source,failure_id:e.failure_id}:e.source==='cdu'?{source:e.source,unit:e.unit}:{source:e.source,control:e.control});
  const op=await operation('failures/batch',{action:actionName,entries});showOperation(op,'failure-selection-note');return op;
 }
 el('failure-now').addEventListener('click',()=>{try{stage(chosen().map(e=>fullEntry(e)));}catch(e){notice(e.message);}});
 el('failure-execute-now').addEventListener('click',()=>action(async()=>{
  const entries=[...draft.values()],op=await sendBatch(entries);
  for(const e of entries){const item=op.result?.items?.find(i=>i.source===e.source&&(e.source==='psx'?i.failure_id===e.failure_id:e.source==='cdu'?i.unit===e.unit:i.control===e.control));
   if(item&&(['matching','activated'].includes(item.configured_readback)||item.state==='accepted'))draft.delete(key(e));
  }
 }));
 function armOptions(){
  const mixed=armEntries.some(e=>e.source==='hwio'),h=snapshot?.non_normal?.hwio;
  for(const [criterion,id]of Object.entries(armFields)){
   const field=el(id);field.disabled=(criterion==='minutes'&&mixed)||(mixed&&criterion==='altitude'&&!h?.altitude_available)||(mixed&&criterion==='ias'&&!h?.ias_available);
   field.title=field.disabled?(criterion==='minutes'?'Timed arming unavailable for controls and stabilizer':criterion==='altitude'?'Corrected altitude unavailable':'IAS unavailable'):'';
  }
 }
 for(const id of Object.values(armFields)){
  const select=()=>{if(el(id).disabled)return;for(const other of Object.values(armFields))if(other!==id)el(other).value='';};
  el(id).addEventListener('focus',select);el(id).addEventListener('input',select);
 }
 el('failure-arm').addEventListener('click',()=>{
  if(catalogueChoices.size&&!selected.size&&el('failure-catalogue-criterion').value){action(async()=>{const c=el('failure-catalogue-criterion').value,v=el('failure-catalogue-value').value;if(v==='')throw Error('Enter the trigger value');await sendBatch(chosen().map(e=>fullEntry(e,c,c==='minutes'?Number(v):v)));});return;}
  armEntries=chosen();armOptions();for(const id of Object.values(armFields))el(id).value='';el('failure-arm-dialog').showModal();});
 el('failure-arm-send').addEventListener('click',()=>action(async()=>{
  const entered=Object.entries(armFields).filter(([,id])=>!el(id).disabled&&el(id).value!=='');
  if(entered.length!==1)throw Error('Enter one arming value');
  const [c,id]=entered[0],v=Number(el(id).value);
  if(!Number.isFinite(v))throw Error('Enter a valid arming value');
  const op=await sendBatch(armEntries.map(e=>fullEntry(e,c,v)));if(op.state==='observed')el('failure-arm-dialog').close();
 }));
 el('failure-arm-cancel').addEventListener('click',()=>el('failure-arm-dialog').close());
 el('failure-clear-selected').addEventListener('click',()=>action(async()=>{
  const entries=[...selected].flatMap(id=>{const s=systems.get(id);return s.group==='engine'?[209,213,254].map(base=>({source:'psx',failure_id:base+s.index})):[entry(s.group,s.index)];}).concat([...catalogueChoices.values()]);for(const e of entries)draft.delete(key(e));
  await sendBatch(entries,'clear');
 }));
 el('failure-clear-all').addEventListener('click',()=>action(async()=>{
  draft.clear();const op=await operation('failures/reset-all');showOperation(op,'failure-selection-note');
 }));
 function renderCatalogue(){
  const states=nativeStates(),ready=!!snapshot?.ready;
  const items=catalogue.filter(i=>catalogueIds===null||catalogueIds.has(i.id)||(!quickFailureIds.has(i.id)&&ready&&['armed','active'].includes(states.get(i.id)?.state)));
  const pinned=i=>!quickFailureIds.has(i.id)&&ready&&['armed','active'].includes(states.get(i.id)?.state);
  items.sort((a,b)=>Number(pinned(b))-Number(pinned(a))||a.id-b.id);
  const signature=JSON.stringify(items.map(i=>[i.id,ready?states.get(i.id)?.state:'unavailable',draft.has('psx:'+i.id),catalogueChoices.has(i.id),active]));
  if(signature===cataloguePaintKey)return;cataloguePaintKey=signature;
  el('failure-catalogue-list').replaceChildren(...items.map(i=>{
   const b=document.createElement('button'),state=ready?states.get(i.id)?.state:'unavailable',staged=draft.has('psx:'+i.id);
   b.dataset.failureId=String(i.id);b.textContent=i.name;b.title=i.id+' · '+i.category;
   b.disabled=active||!ready;b.classList.toggle('active-failure',state==='active');b.classList.toggle('staged-now',state!=='active'&&staged);b.classList.toggle('armed',state==='armed'&&!staged);
   b.setAttribute('aria-label',i.name+(state==='active'?' active':staged?' staged Now':state==='armed'?' armed':''));pressed(b,catalogueChoices.has(i.id));
   b.addEventListener('click',()=>{
    if(catalogueChoices.has(i.id)){catalogueChoices.delete(i.id);if(catalogueSelected?.id===i.id)catalogueSelected=null;}
    else {catalogueSelected=i;catalogueChoices.set(i.id,{source:'psx',failure_id:i.id,severe:false});
     el('failure-catalogue-variant').replaceChildren(...[['false',i.non_severe_variant],['true',i.severe_variant]].map(([value,text])=>{const o=document.createElement('option');o.value=value;o.textContent=text;return o;}));el('failure-catalogue-variant').value='false';}
    paint();
   });return b;
  }));
 }
 async function loadCatalogue(){
  if(catalogue.length||catalogueLoading||catalogueAttempted)return;catalogueLoading=true;catalogueAttempted=true;
  try{catalogue=(await api('non-normal/failures')).failures;renderCatalogue();}catch(e){notice(e.message);}finally{catalogueLoading=false;}
 }
 el('failure-catalogue-variant').addEventListener('change',()=>{if(catalogueSelected&&catalogueChoices.has(catalogueSelected.id))catalogueChoices.get(catalogueSelected.id).severe=el('failure-catalogue-variant').value==='true';});
 el('failure-catalogue-search').addEventListener('input',async()=>{
  const generation=++searchGeneration,text=el('failure-catalogue-search').value.trim();
  if(!text){catalogueIds=null;renderCatalogue();return;}
  try{const result=await api('non-normal/failures/search',{text});if(generation!==searchGeneration)return;catalogueIds=new Set(result.ids);renderCatalogue();}
  catch(e){notice(e.message);}
 });
 for(const b of document.querySelectorAll('[data-shear]'))b.addEventListener('click',()=>{shear=shear===b.dataset.shear?null:b.dataset.shear;paint();});
 el('vash-select').addEventListener('click',()=>{ash=!ash;paint();});
 for(const id of ['shear-top','shear-top-slider'])el(id).addEventListener('input',()=>{el(id==='shear-top'?'shear-top-slider':'shear-top').value=el(id).value;});
 async function sendWeather(body){
  const op=await operation('weather-events',body);showOperation(op,'weather-events-state');return op;
 }
 el('weather-events-execute').addEventListener('click',()=>action(async()=>{
  if(shear&&!snapshot?.non_normal?.weather_events?.cloud_compatible){shear=null;error('no compatible cloud layer for windshear');return;}
  const selectedShear=shear,selectedAsh=ash,body={};if(shear){body.shear=shear;body.shear_top_ft=Number(el('shear-top').value);}if(ash)body.ash=true;
  const op=await sendWeather(body);
  if(op.detail.includes('no compatible cloud layer for windshear')){shear=null;error('no compatible cloud layer for windshear');return;}
  if(confirmed(op)){if(selectedShear){applied.set('shear:'+selectedShear,Date.now()+15000);shear=null;}if(selectedAsh){applied.set('ash',Date.now()+15000);ash=false;}}
 },'weather-events-state'));
 el('shear-off').addEventListener('click',()=>action(async()=>{const op=await sendWeather({shear:'off',shear_top_ft:Number(el('shear-top').value)});if(confirmed(op)){shear=null;for(const k of [...applied.keys()])if(k.startsWith('shear:'))applied.delete(k);}},'weather-events-state'));
 el('vash-off').addEventListener('click',()=>action(async()=>{const op=await sendWeather({ash:false});if(confirmed(op)){ash=false;applied.delete('ash');}},'weather-events-state'));
 for(const b of document.querySelectorAll('[data-tcas]'))b.addEventListener('click',()=>{traffic=traffic===b.dataset.tcas?null:b.dataset.tcas;paint();});
 el('runway-actor-select').addEventListener('click',()=>action(async()=>{
  const op=await operation('runway-aircraft/candidates');showOperation(op,'traffic-events-state');
  if(op.state!=='observed')return;const r=op.result;runwayChoice=null;
  el('runway-actor-options').replaceChildren(...(r.candidates||[]).map(c=>{
   const b=document.createElement('button');b.textContent=c.airport+' / '+c.runway+' · '+c.distance_nm.toFixed(1)+' NM'+(c.preselected?' · FMC arrival':'');
   const choice={airport:c.airport,runway:c.runway,database_revision:r.database_revision};
   if(c.preselected)runwayChoice=choice;
   pressed(b,c.preselected);b.addEventListener('click',()=>{runwayChoice=choice;for(const other of el('runway-actor-options').children)pressed(other,other===b);el('runway-actor-confirm').disabled=false;});return b;
  }));
  el('runway-actor-notice').textContent=r.candidates?.length?'Select a runway.':'No runway candidate available; usable track or FMC arrival required.';
  el('runway-actor-confirm').disabled=!runwayChoice;el('runway-actor-dialog').showModal();
 },'traffic-events-state'));
 el('runway-actor-confirm').addEventListener('click',()=>{runway=runwayChoice;el('runway-actor-dialog').close();paint();});
 el('runway-actor-cancel').addEventListener('click',()=>el('runway-actor-dialog').close());
 el('traffic-events-execute').addEventListener('click',()=>action(async()=>{
  if(traffic){const t=traffic,preset=el('traffic-ignore-ra').checked?{crossing_descent:'descent_ignoring_ra',same_altitude:'same_ignoring_ra',crossing_climb:'climb_ignoring_ra'}[t]:t;
   const op=await operation('tcas',{preset});showOperation(op,'traffic-events-state');if(confirmed(op)){applied.set('traffic:'+t,Date.now()+15000);traffic=null;}}
  if(runway){const op=await operation('runway-aircraft/place',{...runway,actor_revision:snapshot.non_normal.actor.revision});showOperation(op,'traffic-events-state');
   if(op.state==='observed'&&op.result?.state==='sdk_submitted'){applied.set('actor',Date.now()+15000);runway=null;}}
 },'traffic-events-state'));
 el('runway-actor-remove').addEventListener('click',()=>action(async()=>{
  const outcomes=[];
  if(snapshot?.ready){const op=await operation('tcas',{preset:'off'});outcomes.push(operationText(op));if(confirmed(op)){traffic=null;for(const k of [...applied.keys()])if(k.startsWith('traffic:'))applied.delete(k);}}
  if(snapshot?.non_normal?.actor?.available){const op=await operation('runway-aircraft/remove',{actor_revision:snapshot.non_normal.actor.revision});outcomes.push(operationText(op));if(op.state==='observed'){runway=null;applied.delete('actor');}}
  el('traffic-events-state').textContent=outcomes.join(' · ');
 },'traffic-events-state'));
 el('crash-reset').addEventListener('click',()=>action(async()=>{const op=await operation('reset-after-crash');globalThis.IOS.message(operationText(op)||globalThis.IOS.operationText(op));}));
 el('event-error-close').addEventListener('click',()=>el('event-error-dialog').close());
 function clearDrafts(){draft.clear();selected.clear();catalogueChoices.clear();catalogueSelected=null;cataloguePaintKey=null;shear=null;ash=false;traffic=null;runway=null;runwayChoice=null;applied.clear();for(const id of ['failure-arm-dialog','runway-actor-dialog'])if(el(id).open)el(id).close();}
 globalThis.FailureEvents={update(s){
  if(snapshot?.ready&&!s.ready)clearDrafts();if(runway&&runway.database_revision!==s.database?.revision){runway=null;el('traffic-events-state').textContent='Database changed. Select the runway again.';}
  snapshot=s;paint();if(s.ready)loadCatalogue();
 },disconnected(){clearDrafts();snapshot=null;catalogueAttempted=false;paint();},operationText};
 paint();
})();
