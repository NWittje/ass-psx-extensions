'use strict';
globalThis.WeightServiceUI=(()=>{
 const el=id=>document.getElementById(id), pending=new Map(),dirty=new Set();
 let state={},ready=false,units=globalThis.IOS?.units?.()==='imperial'?'imperial':'metric',revision=0,editingSlider=null,checking=false;
 const imperial=()=>units==='imperial',factor=()=>imperial()?1:.45359237;
 const label=()=>imperial()?'lb':'kg';
 const fields=['zfw','fuel','gw_cg'];
 const observed=key=>key==='gw_cg'?state.gw_cg:state[key+'_lb']==null?null:Math.round(state[key+'_lb']*factor());
 function note(text){el('weight-service-result').textContent=text;}
 function held(key){return dirty.has(key)||editingSlider===key||[...pending.values()].some(v=>v===key);}
 function show(key,force=false){const n=observed(key);if(!force&&held(key))return;el('ws-'+key).value=n==null?'':String(n);if(n!=null)el('ws-'+key+'-slider').value=String(n);}
 function revert(key){dirty.delete(key);show(key,true);el('ws-'+key).removeAttribute('aria-invalid');}
 function bounds(key){return key==='gw_cg'?[8.5,33]:key==='zfw'?[Math.ceil((state.zfw_min_lb??352739)*factor()),Math.floor(639340*factor())]:[0,state.fuel_max_lb==null?null:Math.floor(state.fuel_max_lb*factor())];}
 async function send(path,body,key){
  if(!ready)return;
  try{const receipt=await IOS.api('weight-service/'+path,{...body,expected_revision:revision,request_id:IOS.requestId()});pending.set(receipt.id,key);note('Sending…');}
  catch(e){note(e.message);if(fields.includes(key))revert(key);}
 }
 function commit(key){
  const input=el('ws-'+key),text=input.value.trim(),n=Number(text),[lo,hi]=bounds(key);
  const valid=text!==''&&Number.isFinite(n)&&hi!==null&&n>=lo&&n<=hi&&(key==='gw_cg'?Math.abs(n*10-Math.round(n*10))<1e-6:Number.isInteger(n));
  if(!valid){revert(key);return;}
  dirty.delete(key);input.removeAttribute('aria-invalid');
  if(n===observed(key))return;
  send('set',{field:key,value:n,units:imperial()?'lb':'kg'},key);
 }
 for(const key of fields){
  const input=el('ws-'+key),slider=el('ws-'+key+'-slider');
  input.addEventListener('input',()=>dirty.add(key));
  input.addEventListener('keydown',e=>{if(e.key==='Enter'){e.preventDefault();commit(key);}else if(e.key==='Escape')revert(key);});
  input.addEventListener('blur',()=>{if(dirty.has(key))revert(key);});
  slider.addEventListener('input',()=>{editingSlider=key;input.value=slider.value;dirty.add(key);});
  slider.addEventListener('change',()=>{editingSlider=null;commit(key);});
 }
 const controls=[['ground-power','ground_power'],['ground-air','ground_air'],['fifth-engine','fifth_engine']];
 for(const [id,key]of controls)el('ws-'+id).addEventListener('click',()=>send('set',{field:key,value:state[key]?0:1,units:'lb'},key));
 for(const button of document.querySelectorAll('[data-ws-engine]'))button.addEventListener('click',()=>send('set',{field:'engine_model',value:Number(button.dataset.wsEngine),units:'lb'},'engine_model'));
 for(const button of document.querySelectorAll('[data-ws-action]'))button.addEventListener('click',()=>send('action',{action:button.dataset.wsAction},button.dataset.wsAction));
 async function refresh(){if(checking)return;checking=true;try{for(const [id,key]of [...pending])try{const op=await IOS.api('operations/'+id);if(!pending.has(id)||op.state==='queued'||op.state==='sent')continue;pending.delete(id);note(op.result?.configured_readback==='different'?'PSX returned a different value':op.state==='observed'?(key==='quickstart'?'Engine start requested':key==='fuel'?'Fuel updated':'Done'):op.detail||'Result unavailable');if(fields.includes(key))show(key);}catch(e){pending.delete(id);note(e.message);}}finally{checking=false;}}
 function render(s){
  ready=!!s.ready;revision=s.revision;state=s.weight_service||{};
  if(!ready){pending.clear();dirty.clear();editingSlider=null;state={};}
  for(const key of fields){const [lo,hi]=bounds(key),input=el('ws-'+key),slider=el('ws-'+key+'-slider');input.min=slider.min=String(lo);input.max=slider.max=String(hi??0);input.step=key==='gw_cg'?'.1':'1';slider.step=key==='gw_cg'?'.1':'100';
   const enabled=ready&&observed(key)!==null&&(key!=='fuel'||state.native?.fuel);input.disabled=slider.disabled=!enabled;slider.title=key==='fuel'&&!enabled?(state.native?.reason||'Fuel service unavailable'):'';show(key);}
  el('ws-zfw-label').textContent='ZFW — '+label();el('ws-fuel-label').textContent='Fuel — '+label();
  el('ws-zfw-cg').textContent=state.zfw_cg==null?'ZFW-CG: —':`ZFW-CG: ${state.zfw_cg.toFixed(1)}% MAC`;
  for(const [id,key]of controls){const b=el('ws-'+id);b.disabled=!ready||state[key]==null||(key==='fifth_engine'&&state.engine_model===0);IOS.pressed(b,!!state[key]);b.title=key==='fifth_engine'&&state.engine_model===0?'Unavailable with CF6':'';}
  for(const b of document.querySelectorAll('[data-ws-engine]')){b.disabled=!ready||state.engine_model==null;IOS.pressed(b,state.engine_model===Number(b.dataset.wsEngine));}
  for(const b of document.querySelectorAll('[data-ws-action]')){b.disabled=!ready||!state.native?.[b.dataset.wsAction];b.title=b.disabled?(state.native?.reason||'Service unavailable'):'';}
  refresh();
 }
 function setUnits(choice){const next=choice==='imperial'?'imperial':'metric';if(next===units)return;units=next;dirty.clear();editingSlider=null;for(const key of fields)show(key,true);}
 return {render,setUnits};
})();
