'use strict';
globalThis.EnvironmentUI=(()=>{
 const el=id=>document.getElementById(id), ns='http://www.w3.org/2000/svg';
 let target='planet',data=null,ready=false,values={},cb=false,drag=null,windDrag=null,lastTap=null;
 let heightScale=AltitudeScale.create(45100,40,475);
 let cloudLeft=155,cloudWidth=500;
 let units='metric';
 const pending=new Map(),dirty=new Set();
 const wrap=d=>((d%360)+360)%360, pad=d=>String(Math.round(wrap(d))).padStart(3,'0');
 const lower=name=>cb?'cb_'+name:'lower_'+name;
 const octas=n=>`${n}/8`;
 function svg(tag,attrs,text){const node=document.createElementNS(ns,tag);for(const [k,v]of Object.entries(attrs))node.setAttribute(k,String(v));if(text!==undefined)node.textContent=text;return node;}
 function error(text){el('environment-result').textContent=text;}
 function protectedField(key){return dirty.has(key)||[...pending.values()].some(p=>p.fields.includes(key));}
 function invalid(node,text){node.setAttribute('aria-invalid','true');error(text);}
 function clear(node){node.removeAttribute('aria-invalid');}
 async function submit(path,body,fields=[]){
  if(!ready){error('PSX is not synchronized');return;}
  const savedTarget=target;
  try{const op=await api(path,{...body,request_id:requestId()});pending.set(op.id,{fields,target:savedTarget,path});error('Sending…');}
  catch(e){error(e.message);}
 }
 async function refreshOperations(){
  for(const [id,p] of [...pending])try{const op=await api('operations/'+id);if(op.state==='queued'||op.state==='sent')continue;
   pending.delete(id);
   error(globalThis.IOS.operationText(op));
  }catch(e){if(e.message.includes('Operation not retained'))pending.delete(id);error(e.message);}
 }
 function weather(edits){if(!Object.keys(edits).length)return;for(const [k,v]of Object.entries(edits)){values[k]=v;dirty.delete(k);}submit('weather',{target:'all',...edits},Object.keys(edits));drawAtmosphere();}
 function lowerEdits(name,value){return cb?{['lower_'+name]:value,['cb_'+name]:value}:{['lower_'+name]:value};}
 function inputValue(id,value,force=false){const node=el(id);if(force||(document.activeElement!==node&&!dirty.has(id)&&!dirty.has(node.dataset.weather)))node.value=String(value);}
 const shown=(key,value)=>WeatherUnits.display(key,value,units);
 function setUnits(choice){
  const next=choice==='imperial'?'imperial':'metric';if(next===units)return;units=next;
  el('environment-page').setAttribute('data-weather-units',units);
  const imperial=units==='imperial',symbol=imperial?'°F':'°C';
  for(const [key,min,max]of [['surface_temperature',-80,50],['inversion_temperature',-50,50],['qnh',914.4,1083.6]]){
   const node=el(key.replaceAll('_','-'));node.min=imperial?(key==='qnh'?'27.00':shown(key,min)):String(min);node.max=imperial?(key==='qnh'?'32.00':shown(key,max)):String(max);node.step=imperial?(key==='qnh'?'.01':'.1'):'1';
   node.setAttribute('aria-label',key==='qnh'?`QNH, ${imperial?'inches of mercury':'hectopascals'}`:`${key==='surface_temperature'?'Surface':'Inversion'} temperature, ${imperial?'Fahrenheit':'Celsius'}`);
   if(values[key]!==undefined)inputValue(node.id,shown(key,values[key]),true);
  }
  el('temperature-label').textContent=`Temp — ${symbol}`;el('qnh-label').textContent=`QNH — ${imperial?'inHg':'hPa'}`;el('inversion-label').textContent=`Inversion — ft / ${symbol}`;
  el('temperature-slider').min=shown('surface_temperature',-80);el('temperature-slider').max=shown('surface_temperature',50);el('temperature-slider').step=imperial?'1.8':'1';
  el('temperature-slider').setAttribute('aria-label',`Surface temperature, ${imperial?'Fahrenheit':'Celsius'}`);
  el('inversion-slider').setAttribute('aria-label',`Inversion temperature, ${imperial?'Fahrenheit':'Celsius'}`);el('inversion-slider').step=imperial?'1.8':'1';
  if(data)render({ready,environment:data});
 }
 const heightY=h=>heightScale.y(h);
 function render(s){
  ready=s.ready;data=s.environment||{};
  const actual=data[target];
  if(actual)for(const [key,v]of Object.entries(actual))if(!protectedField(key))values[key]=v;
  const visibility=data.visibility?.all;if(visibility&&Number.isFinite(visibility.commanded_m)&&!protectedField('visibility'))values.visibility=visibility.commanded_m;
  el('visibility-status').textContent=visibility?`PSX ${visibility.psx_m??'—'} m · X-Plane ${visibility.xplane_state||'unavailable'}${visibility.xplane_m==null?'':` · regional ${Math.round(visibility.xplane_m)} m`}${visibility.aircraft_m==null?'':` · aircraft ${Math.round(visibility.aircraft_m)} m`}`:'';
  if(!protectedField('cb_coverage')&&!dirty.has('cb-selector'))cb=(values.cb_coverage||0)>0;
  el('target-planet').setAttribute('aria-pressed',String(target==='planet'));el('target-nearest').setAttribute('aria-pressed',String(target==='zone1'));
  el('target-nearest').disabled=!ready||!data.nearest_available;el('target-nearest').title=data.nearest_notice||'';
  el('weather-station').textContent='All zones and Planet';
  document.querySelectorAll('#environment-page [data-weather],#environment-page input[type=range],#environment-page [data-weather-preset],#environment-page #cb-selector,#environment-page #inversion-enabled').forEach(n=>{n.disabled=!ready||!actual;});
  el('target-planet').disabled=false;
  for(const key of ['upper_top','upper_base','qnh','surface_temperature','visibility','precipitation','inversion_height','inversion_temperature'])if(values[key]!==undefined)inputValue(key.replaceAll('_','-'),shown(key,values[key]));
  for(const name of ['top','base'])if(values[lower(name)]!==undefined)inputValue('lower-'+name,values[lower(name)]);
  el('cb-selector').checked=cb;el('inversion-enabled').checked=!!values.inversion_enabled;
  for(const [id,key]of [['temperature-slider','surface_temperature']])if(!protectedField(key))inputValue(id,shown(key,values[key]||0));
  if(!protectedField('visibility'))inputValue('visibility-slider',VisibilityScale.position(values.visibility||0));
  const low=Math.max(-50,(values.surface_temperature||0)-10),high=Math.min(50,(values.surface_temperature||0)+20);
  el('inversion-slider').min=shown('inversion_temperature',low);el('inversion-slider').max=shown('inversion_temperature',high);inputValue('inversion-slider',shown('inversion_temperature',values.inversion_temperature||0));
  el('inversion-scale').textContent=`${shown('inversion_temperature',low)}…${shown('inversion_temperature',high)} ${units==='imperial'?'°F':'°C'}`;
  el('environment-notice').textContent=[data.startup_notice||'',!actual?'Weather settings unavailable':'',data.magnetic_variation===null?'Magnetic wind entry unavailable':''].filter(Boolean).join(' · ');
  renderWind();renderTime();drawAtmosphere();refreshOperations();
 }
 function renderTime(){
  const t=data.time;const hold=dirty.has('clock')||protectedField('clock');
  if(t){if(!hold){inputValue('local-hour',String(t.hour).padStart(2,'0'));inputValue('local-minute',String(t.minute).padStart(2,'0'));inputValue('time-slider',t.hour*60+t.minute);}
   const sign=t.offset<0?'−':'+';const off=Math.abs(t.offset);
   el('local-date').textContent=`${t.year}-${String(t.month).padStart(2,'0')}-${String(t.day).padStart(2,'0')} · ${t.zone} · UTC${sign}${String(Math.floor(off/3600)).padStart(2,'0')}:${String(Math.floor(off%3600/60)).padStart(2,'0')}`;
   el('utc-clock').textContent=new Date(t.utc_ms).toISOString().replace('T',' ').slice(0,19)+' UTC';
  }else{el('local-date').textContent=data.time_notice||'Local civil time unavailable';el('utc-clock').textContent='';}
  for(const id of ['local-hour','local-minute','time-slider'])el(id).disabled=!ready||!t;
  document.querySelectorAll('[data-time]').forEach(b=>b.disabled=!ready||!t||(b.dataset.time==='dawn'&&!t.dawn_available)||(b.dataset.time==='dusk'&&!t.dusk_available));
 }
 async function time(body){
  if(!ready)return;dirty.delete('clock');
  try{const op=await api('time',{...body,request_id:requestId()});pending.set(op.id,{fields:['clock'],target,path:'time'});error('Sending…');}
  catch(e){
   const match=e.message.match(/^Repeated local time\. Choose UTC offset seconds:(.*)$/);
   if(match){el('time-fold-buttons').replaceChildren(...match[1].trim().split(/\s+/).map(n=>{const b=document.createElement('button');const off=Number(n);b.textContent=`UTC ${off>=0?'+':''}${off/3600} h`;b.addEventListener('click',()=>{el('time-fold-dialog').close();time({...body,offset:off});});return b;}));el('time-fold-dialog').showModal();}
   else error(e.message);
  }
 }
 function clockCommit(){const hour=Number(el('local-hour').value),minute=Number(el('local-minute').value);if(el('local-hour').value===''||el('local-minute').value===''||!Number.isInteger(hour)||hour<0||hour>23||!Number.isInteger(minute)||minute<0||minute>59){error('Local time must be HH 00–23 and MM 00–59');return;}time({action:'clock',hour,minute});}
 function groupCommit(id,commit,key){const group=el(id);group.addEventListener('input',()=>dirty.add(key));group.addEventListener('keydown',e=>{if(e.key==='Enter'){e.preventDefault();commit();}});group.addEventListener('focusout',e=>{if(!group.contains(e.relatedTarget)&&dirty.has(key))commit();});}
 groupCommit('clock-group',clockCommit,'clock');groupCommit('wind-group',windCommit,'wind');
 el('time-slider').addEventListener('input',()=>{dirty.add('clock');const n=Number(el('time-slider').value);el('local-hour').value=String(Math.floor(n/60)).padStart(2,'0');el('local-minute').value=String(n%60).padStart(2,'0');});el('time-slider').addEventListener('change',clockCommit);
 document.querySelectorAll('[data-time]').forEach(b=>b.addEventListener('click',()=>time({action:b.dataset.time})));
 el('time-fold-cancel').addEventListener('click',()=>el('time-fold-dialog').close());
 el('target-planet').addEventListener('click',()=>{target='planet';values={};dirty.clear();});
 el('target-nearest').addEventListener('click',()=>submit('weather/nearest',{},['station']));
 document.querySelectorAll('[data-weather-preset]').forEach(b=>b.addEventListener('click',()=>{dirty.clear();submit('weather',{target:'all',preset:b.dataset.weatherPreset},Object.keys(values));}));
 function commitNumber(node,key){const displayed=Number(node.value);if(!dirty.has(key)&&displayed===Number(shown(key,values[key])))return;if(!node.value||!Number.isFinite(displayed)||displayed<Number(node.min)||displayed>Number(node.max)|| (key!=='qnh'&&!(units==='imperial'&&WeatherUnits.temperature(key))&&!Number.isInteger(displayed))){invalid(node,'Invalid '+key.replaceAll('_',' '));return;}
  const v=WeatherUnits.native(key,displayed,units);
  let edits={[key]:v};if(node.id==='lower-base'||node.id==='lower-top')edits=lowerEdits(node.id==='lower-base'?'base':'top',v);
  if(/^(lower|upper|cb)_(base|top)$/.test(key)){
   const [lo,hi]=heightLimits(key);
   if(v<lo||v>hi){invalid(node,'Cloud boundaries require at least 500 ft separation');return;}
  }
  clear(node);dirty.delete(key);node.value=shown(key,v);weather(edits);
 }
 document.querySelectorAll('[data-weather]').forEach(node=>{
  if(node.type==='range'||node.tagName==='SELECT') {node.addEventListener('input',()=>{dirty.add(node.dataset.weather);values[node.dataset.weather]=Number(node.value);drawAtmosphere();});node.addEventListener('change',()=>weather({[node.dataset.weather]:Number(node.value)}));}
  else{node.addEventListener('input',()=>dirty.add(node.dataset.weather));node.addEventListener('keydown',e=>{if(e.key==='Enter'){e.preventDefault();commitNumber(node,node.dataset.weather);}});node.addEventListener('change',()=>commitNumber(node,node.dataset.weather));}
 });
 for(const name of ['base','top']){const node=el('lower-'+name);node.addEventListener('input',()=>dirty.add(lower(name)));node.addEventListener('change',()=>commitNumber(node,lower(name)));node.addEventListener('keydown',e=>{if(e.key==='Enter'){e.preventDefault();commitNumber(node,lower(name));}});}
 el('cb-selector').addEventListener('change',()=>{cb=el('cb-selector').checked;const edits=cb?{cb_base:values.lower_base,cb_top:values.lower_top,cb_coverage:values.lower_coverage}:{cb_coverage:0};weather(edits);});
 el('inversion-enabled').addEventListener('change',()=>weather({inversion_enabled:el('inversion-enabled').checked?1:0}));
 for(const [id,key,numberId]of [['temperature-slider','surface_temperature','surface-temperature'],['inversion-slider','inversion_temperature','inversion-temperature']]){
  el(id).addEventListener('input',()=>{dirty.add(key);values[key]=WeatherUnits.native(key,Number(el(id).value),units);el(numberId).value=shown(key,values[key]);});el(id).addEventListener('change',()=>weather({[key]:WeatherUnits.native(key,Number(el(id).value),units)}));
 }
 el('visibility-slider').addEventListener('input',()=>{dirty.add('visibility');values.visibility=VisibilityScale.metres(Number(el('visibility-slider').value));el('visibility').value=String(values.visibility);});
 el('visibility-slider').addEventListener('change',()=>weather({visibility:VisibilityScale.metres(Number(el('visibility-slider').value))}));
 function point(event,root){const p=root.createSVGPoint();p.x=event.clientX;p.y=event.clientY;return p.matrixTransform(root.getScreenCTM().inverse());}
 function heightLimits(key){if(key==='inversion_height')return[500,3500];const base=key.endsWith('base'),prefix=key.split('_')[0];let low=prefix==='upper'?100:0,high=prefix==='upper'?45100:45000;
  if(base)high=Math.min(high,values[prefix+'_top']-500);else low=Math.max(low,values[prefix+'_base']+500);
  if(cb&&(prefix==='lower'||prefix==='cb')){const other=prefix==='cb'?'lower':'cb';if(base)high=Math.min(high,values[other+'_top']-500);else low=Math.max(low,values[other+'_base']+500);}
  if(key==='lower_top'||(cb&&key==='cb_top'))high=Math.min(high,values.upper_base-500);
  if(key==='upper_base')low=Math.max(low,values.lower_top+500,cb?values.cb_top+500:0);return[low,high];}
 function heightMove(key,value){const [lo,hi]=heightLimits(key);if(lo>hi){error('Cloud boundaries require at least 500 ft separation');return;}const v=Math.round(Math.min(hi,Math.max(lo,AltitudeScale.snap(value,0,45100))));values[key]=v;dirty.add(key);if(cb&&(key.startsWith('cb_')||key.startsWith('lower_'))){const other=key.startsWith('cb_')?key.replace('cb_','lower_'):key.replace('lower_','cb_');values[other]=v;dirty.add(other);}drawAtmosphere();}
 function heightCommit(key){const [lo,hi]=heightLimits(key);if(values[key]<lo||values[key]>hi){error('Cloud boundaries require at least 500 ft separation');return;}weather(cb&&(key.startsWith('cb_')||key.startsWith('lower_'))?lowerEdits(key.split('_')[1],values[key]):{[key]:values[key]});}
 function coverageMove(key,value){const v=Math.max(0,Math.min(8,Math.round(value)));values[key]=v;dirty.add(key);if(cb&&key==='cb_coverage'){values.lower_coverage=v;dirty.add('lower_coverage');}drawAtmosphere();}
 function coverageCommit(key){weather(cb&&key==='cb_coverage'?lowerEdits('coverage',values[key]):{[key]:values[key]});}
 function drawAtmosphere(){
  const root=el('atmosphere-svg'),width=root.clientWidth||720,height=root.clientHeight||520;
  heightScale=AltitudeScale.create(45100,height<400?20:40,height-45);
  // Match viewBox units to CSS pixels: wider screens add margins, never stretch the drawing.
  cloudWidth=Math.min(500,Math.max(100,width-200));cloudLeft=Math.max(155,(width-cloudWidth)/2);
  el('atmosphere').setAttribute('data-compact',String(width<840));
  const right=cloudLeft+cloudWidth,boxes=[],layers=['upper',cb?'cb':'lower'];
  root.setAttribute('viewBox',`0 0 ${width} ${height}`);root.replaceChildren();if(!Object.keys(values).length)return;
  root.append(svg('rect',{id:'cloud-section',x:cloudLeft,y:height<400?20:40,width:cloudWidth,height:height-(height<400?65:85),fill:'var(--ios-background)',stroke:'var(--ios-edge)'}));
  for(const layer of layers)for(const part of ['top','base']){const key=layer+'_'+part;boxes.push({key,box:el((layer==='upper'?'upper':'lower')+'-'+part+'-box'),y:heightY(values[key])});}
  boxes.sort((a,b)=>a.y-b.y);let previous=-20;
  for(const b of boxes){b.display=Math.max(b.y,previous+(height<400?50:68));previous=b.display;}
  const overflow=Math.max(0,previous-(height-75));for(const b of boxes){b.display-=overflow;b.box.style.top=`${b.display/height*100}%`;}
  for(const h of [0,1000,3000,5000,10000,20000,30000,45100]){const y=heightY(h);root.append(svg('line',{x1:cloudLeft,x2:right,y1:y,y2:y,stroke:'var(--ios-edge)','stroke-dasharray':'2 5'}),svg('text',{x:right+5,y:y+4,'font-size':11,fill:'var(--ios-text)'},h.toLocaleString()));}
  for(const layer of ['upper',cb?'cb':'lower']){
   const top=values[layer+'_top']||0,base=values[layer+'_base']||0,cov=values[layer+'_coverage']||0;
   const topY=heightY(top),baseY=heightY(base),layerHeight=Math.max(1,baseY-topY),edgeX=cloudLeft+cloudWidth*cov/8;
   root.append(svg('rect',{x:cloudLeft,y:topY,width:cloudWidth,height:layerHeight,fill:'var(--ios-surface)'}),svg('rect',{x:cloudLeft,y:topY,width:cloudWidth*cov/8,height:layerHeight,fill:'var(--ios-coverage)'}));
   const coverageKey=layer+'_coverage';const edge=svg('g',{id:'coverage-'+layer,role:'slider',tabindex:ready?0:-1,'aria-label':`${layer==='upper'?'Upper':'Lower'} cloud coverage, octas`,'aria-orientation':'horizontal','aria-valuemin':0,'aria-valuemax':8,'aria-valuenow':cov,'aria-valuetext':octas(cov),'aria-disabled':String(!ready)});
   edge.append(svg('rect',{x:edgeX-22,y:(topY+baseY)/2-Math.max(44,layerHeight)/2,width:44,height:Math.max(44,layerHeight),fill:'transparent','pointer-events':'all'}),svg('line',{x1:edgeX,x2:edgeX,y1:topY,y2:baseY,stroke:'var(--ios-selected)','stroke-width':3}));
   edge.addEventListener('pointerdown',e=>{if(!ready)return;drag={kind:'coverage',key:coverageKey,original:cov,id:e.pointerId};root.setPointerCapture(e.pointerId);e.preventDefault();});
   edge.addEventListener('keydown',e=>{if(!ready)return;const steps={ArrowRight:1,ArrowUp:1,ArrowLeft:-1,ArrowDown:-1};if(!(e.key in steps)&&e.key!=='Home'&&e.key!=='End')return;e.preventDefault();coverageMove(coverageKey,e.key==='Home'?0:e.key==='End'?8:cov+steps[e.key]);coverageCommit(coverageKey);});
   root.append(edge);
   const textX=cov>=5?(cloudLeft+edgeX)/2:(edgeX+right)/2;
   root.append(svg('text',{id:layer==='upper'?'upper-octas':'lower-octas',x:textX,y:layerHeight<22?topY-7:(topY+baseY)/2+5,'text-anchor':'middle','font-size':14,fill:'var(--ios-text)','pointer-events':'none'},octas(cov)));
   for(const part of ['top','base']){const key=layer+'_'+part,y=heightY(values[key]),b=boxes.find(b=>b.key===key),gripX=Math.min(cloudLeft-26,128);
    const line=svg('g',{id:'handle-'+key,role:'slider',tabindex:ready?0:-1,'aria-label':`${layer} cloud ${part}, feet`,'aria-orientation':'vertical','aria-disabled':String(!ready),'aria-valuemin':Math.min(values[key],heightLimits(key)[0]),'aria-valuemax':Math.max(values[key],heightLimits(key)[1]),'aria-valuenow':values[key]});
    line.append(svg('path',{d:`M 88 ${b.display} H ${gripX} L ${cloudLeft} ${y} H ${right}`,fill:'none',stroke:'var(--ios-edge)','stroke-width':3}));
    // Wide edge targets, leaving the coverage drag strip free. Separate grips remain usable on thin layers.
    for(const [x,w]of [[cloudLeft,edgeX-cloudLeft-22],[edgeX+22,right-edgeX-22]])if(w>0)line.append(svg('rect',{x,y:y-22,width:w,height:44,fill:'transparent','pointer-events':'all'}));
    line.append(svg('rect',{x:gripX-24,y:b.display-24,width:48,height:48,rx:6,fill:'var(--ios-surface)',stroke:'var(--ios-edge)','stroke-width':2,'pointer-events':'all'}),svg('path',{d:`M ${gripX-7} ${b.display-6} L ${gripX} ${b.display-12} L ${gripX+7} ${b.display-6} M ${gripX-7} ${b.display+6} L ${gripX} ${b.display+12} L ${gripX+7} ${b.display+6}`,fill:'none',stroke:'var(--ios-text)','stroke-width':2,'pointer-events':'none'}));
    line.addEventListener('pointerdown',e=>{if(!ready)return;drag={key,id:e.pointerId,offset:point(e,root).y-y};root.setPointerCapture(e.pointerId);e.preventDefault();});line.addEventListener('keydown',e=>{if(!ready||!['ArrowUp','ArrowDown'].includes(e.key))return;e.preventDefault();heightMove(key,AltitudeScale.next(values[key],e.key==='ArrowUp'?1:-1));heightCommit(key);});root.append(line);
    inputValue((layer==='upper'?'upper':'lower')+'-'+part,values[key]);
   }
  }
  const y=heightY(values.inversion_height||1500);el('inversion-box').style.top=`${Math.min(height-90,y)/height*100}%`;
  const gripX=right-30;
  const inv=svg('g',{id:'handle-inversion_height',role:'slider',tabindex:ready?0:-1,'aria-label':'Inversion height, feet','aria-orientation':'vertical','aria-disabled':String(!ready),'aria-valuemin':500,'aria-valuemax':3500,'aria-valuenow':values.inversion_height||1500});
  inv.append(svg('line',{x1:right-60,x2:width-110,y1:y,y2:y,stroke:'var(--ios-caution)','stroke-width':2,'stroke-dasharray':'6 4'}),svg('rect',{x:gripX-24,y:y-24,width:48,height:48,rx:6,fill:'var(--ios-surface)',stroke:'var(--ios-caution)','stroke-width':2,'pointer-events':'all'}),svg('path',{d:`M ${gripX-7} ${y-6} L ${gripX} ${y-12} L ${gripX+7} ${y-6} M ${gripX-7} ${y+6} L ${gripX} ${y+12} L ${gripX+7} ${y+6}`,fill:'none',stroke:'var(--ios-text)','stroke-width':2,'pointer-events':'none'}));
  inv.addEventListener('pointerdown',e=>{if(!ready)return;drag={key:'inversion_height',id:e.pointerId,offset:point(e,root).y-y};root.setPointerCapture(e.pointerId);e.preventDefault();});inv.addEventListener('keydown',e=>{if(ready&&(e.key==='ArrowUp'||e.key==='ArrowDown')){e.preventDefault();heightMove('inversion_height',AltitudeScale.next(values.inversion_height,e.key==='ArrowUp'?1:-1));heightCommit('inversion_height');}});root.append(inv);
  root.append(svg('line',{x1:0,x2:width,y1:height-1,y2:height-1,stroke:'var(--ios-edge)','stroke-width':2,'stroke-dasharray':'7 6'}));
 }
 el('atmosphere-svg').addEventListener('pointermove',e=>{if(!drag)return;const p=point(e,el('atmosphere-svg'));if(drag.kind==='coverage')coverageMove(drag.key,(p.x-cloudLeft)/cloudWidth*8);else heightMove(drag.key,heightScale.height(p.y-drag.offset));});
 const heightEnd=e=>{if(!drag)return;const d=drag;drag=null;if(d.kind==='coverage'){if(values[d.key]!==d.original)coverageCommit(d.key);}else heightCommit(d.key);};el('atmosphere-svg').addEventListener('pointerup',heightEnd);el('atmosphere-svg').addEventListener('pointercancel',()=>{drag=null;dirty.clear();});
 function windValue(){const first=Number(el('wind-first').value),second=el('wind-second').value===''?null:Number(el('wind-second').value),speed=Number(el('wind-speed').value),gust=Number(el('wind-gust').value);
  if(!el('wind-first').value||!Number.isFinite(first)||first<0||first>360||(second!==null&&(!Number.isFinite(second)||second<0||second>360))||!Number.isInteger(speed)||speed<0||speed>75||!Number.isInteger(gust)||gust<0||gust>75)throw Error('Invalid wind direction or speed');
  const delta=second===null?0:wrap(second-first+180)-180;if(speed+gust>75)throw Error('Wind plus gust must not exceed 75 kt');return{middle:wrap(first+delta/2),spread:Math.abs(delta),speed,gust:speed+gust};
 }
 function windCommit(){if(!Number.isFinite(data?.magnetic_variation)){error('PSX magnetic variation unavailable');return;}try{const w=windValue();dirty.delete('wind');weather({wind_direction:wrap(Math.round(w.middle+data.magnetic_variation)),wind_variation:Math.round(w.spread),wind_speed:w.speed,gust:w.gust});}catch(e){error(e.message);}}
 function windNativeCommit(w){if(!Number.isFinite(data?.magnetic_variation))return;dirty.delete('wind');weather({wind_direction:wrap(Math.round(w.middle+data.magnetic_variation)),wind_variation:Math.round(w.spread),wind_speed:Math.round(w.speed),gust:Math.round(Math.max(w.speed,w.gust))});}
 function renderWind(){
  if(!dirty.has('wind')&&!windDrag&&!['wind_direction','wind_variation','wind_speed','gust'].some(protectedField)){
   const m=wrap((values.wind_direction||0)-(data.magnetic_variation||0)),spread=values.wind_variation||0;
   inputValue('wind-first',pad(m-spread/2));inputValue('wind-second',spread?pad(m+spread/2):'');inputValue('wind-speed',values.wind_speed||0);inputValue('wind-gust',Math.max(0,(values.gust||0)-(values.wind_speed||0)));
  }
  for(const id of ['wind-first','wind-second','wind-speed','wind-gust'])el(id).disabled=!ready||!Number.isFinite(data.magnetic_variation);
  drawWind(windDrag?.w||{middle:wrap((values.wind_direction||0)-(data.magnetic_variation||0)),spread:values.wind_variation||0,speed:values.wind_speed||0,gust:values.gust||0});
 }
 const windCentre=180,windRim=146, strengthRadius=knots=>20+Math.max(0,Math.min(50,knots))*1.6;
 function windPoint(angle,r=windRim){const a=angle*Math.PI/180;return{x:windCentre+Math.sin(a)*r,y:windCentre-Math.cos(a)*r};}
 function drawWind(w){
  const root=el('wind-rose');root.replaceChildren(svg('circle',{cx:windCentre,cy:windCentre,r:windRim,fill:'var(--ios-surface)',stroke:'var(--ios-edge)'}));
  for(let knots=0;knots<=50;knots+=10){const r=strengthRadius(knots),p=windPoint(315,r);root.append(svg('circle',{'data-strength':knots,cx:windCentre,cy:windCentre,r,fill:'none',stroke:'var(--ios-edge)','stroke-opacity':.6}),svg('text',{x:p.x-4,y:p.y-3,'text-anchor':'end','font-size':10,fill:'var(--ios-text)'},String(knots)));}
  for(let a=0;a<360;a+=30){const p=windPoint(a),q=windPoint(a,139);root.append(svg('line',{x1:p.x,y1:p.y,x2:q.x,y2:q.y,stroke:'var(--ios-edge)'}));}
  for(const [a,label]of [[0,'N magnetic'],[90,'E'],[180,'S'],[270,'W']]){const p=windPoint(a,168);root.append(svg('text',{x:p.x,y:p.y+4,'text-anchor':'middle','font-size':a===0?12:14,fill:'var(--ios-text)'},label));}
  const tip=windPoint(w.middle);root.append(svg('line',{id:'wind-direction-line',x1:windCentre,y1:windCentre,x2:tip.x,y2:tip.y,stroke:'var(--ios-selected)','stroke-width':3,'stroke-dasharray':w.spread?'7 5':'none'}));
  if(w.spread){const first=windPoint(w.middle-w.spread/2),second=windPoint(w.middle+w.spread/2);root.append(svg('path',{d:`M ${first.x} ${first.y} A ${windRim} ${windRim} 0 ${w.spread>180?1:0} 1 ${second.x} ${second.y}`,fill:'none',stroke:'var(--ios-selected)','stroke-width':9,'stroke-dasharray':'2 3'}));}
  function knob(angle,mode,radius=windRim){const p=windPoint(angle,radius),strength=mode==='speed'||mode==='gust';const g=svg('g',{id:`wind-${mode}-grip`,role:'slider',tabindex:ready?0:-1,'aria-label':strength?`Wind ${mode}, knots`:(mode==='first'||mode==='second'?'Wind variation endpoint':'Wind direction, magnetic degrees'),'aria-valuemin':0,'aria-valuemax':strength?75:359,'aria-valuenow':strength?w[mode]:Math.round(wrap(angle)),transform:`translate(${p.x} ${p.y}) rotate(${angle})`});
   if(strength){const sweep=mode==='speed'?0:1;g.append(svg('path',{d:`M -23 0 A 23 23 0 0 ${sweep} 23 0 Z`,fill:'transparent','pointer-events':'all'}),svg('path',{d:`M -11.5 0 A 11.5 11.5 0 0 ${sweep} 11.5 0 Z`,fill:mode==='speed'?'var(--ios-selected)':'var(--ios-background)',stroke:'var(--ios-edge)','pointer-events':'none'}));}
   else g.append(svg('circle',{r:23,fill:'transparent','pointer-events':'all'}),svg('circle',{r:11.5,fill:'var(--ios-surface)',stroke:'var(--ios-edge)','pointer-events':'none'}));
   g.addEventListener('pointerdown',e=>{if(!ready||!Number.isFinite(data.magnetic_variation))return;windDrag={mode,id:e.pointerId,w:{...w},start:point(e,root),original:{...w},changed:false};root.setPointerCapture(e.pointerId);e.preventDefault();});
   g.addEventListener('keydown',e=>{if(!ready||!['ArrowLeft','ArrowRight','ArrowUp','ArrowDown'].includes(e.key))return;e.preventDefault();const step=e.key==='ArrowLeft'||e.key==='ArrowDown'?-1:1,changed={...w};if(strength)Object.assign(changed,WindControls.strength(w.speed,w.gust,mode,w[mode]+step,75));else if(mode==='first'||mode==='second'){const first=WindControls.direction(w.middle-w.spread/2+(mode==='first'?step*10:0)),second=WindControls.direction(w.middle+w.spread/2+(mode==='second'?step*10:0));const delta=Math.max(10,Math.min(350,wrap(second-first)));changed.middle=wrap(first+delta/2);changed.spread=delta;}else changed.middle=WindControls.direction(w.middle+step*10);windNativeCommit(changed);});root.append(g);
  }
  knob(w.middle,'speed',strengthRadius(w.speed));knob(w.middle,'gust',strengthRadius(w.gust));knob(w.middle,'direction');if(w.spread){knob(w.middle-w.spread/2,'first');knob(w.middle+w.spread/2,'second');}
  const centre=svg('circle',{cx:windCentre,cy:windCentre,r:12,fill:'transparent','pointer-events':'all','aria-label':'Double tap to remove variation'});centre.addEventListener('pointerup',()=>tap('centre',w));root.append(centre);
 }
 function tap(mode,w){const t=Date.now();if(lastTap&&lastTap.mode===mode&&t-lastTap.time<400){lastTap=null;windNativeCommit({...w,middle:WindControls.direction(w.middle),spread:mode==='centre'||w.spread?0:20});}else lastTap={mode,time:t};}
 el('wind-rose').addEventListener('pointermove',e=>{if(!windDrag)return;const root=el('wind-rose'),p=point(e,root),d=windDrag;const angle=WindControls.direction(Math.atan2(p.x-windCentre,windCentre-p.y)*180/Math.PI);if(Math.hypot(p.x-d.start.x,p.y-d.start.y)<4&&!d.changed)return;d.changed=true;dirty.add('wind');
  if(d.mode==='first'||d.mode==='second') {const other=WindControls.direction(d.original.middle+(d.mode==='first'?1:-1)*d.original.spread/2);let delta=d.mode==='first'?wrap(other-angle):wrap(angle-other);delta=Math.max(10,Math.min(350,delta));d.w.middle=wrap(d.mode==='first'?other-delta/2:other+delta/2);d.w.spread=delta;}
  else if(d.mode==='direction')d.w.middle=angle;
  else{const a=d.original.middle*Math.PI/180,along=(p.x-windCentre)*Math.sin(a)-(p.y-windCentre)*Math.cos(a);Object.assign(d.w,WindControls.strength(d.original.speed,d.original.gust,d.mode,(along-20)/1.6));}
  el('wind-first').value=pad(d.w.middle-d.w.spread/2);el('wind-second').value=d.w.spread?pad(d.w.middle+d.w.spread/2):'';el('wind-speed').value=d.w.speed;el('wind-gust').value=Math.max(0,d.w.gust-d.w.speed);drawWind(d.w);
 });
 el('wind-rose').addEventListener('pointerup',()=>{if(!windDrag)return;const d=windDrag;windDrag=null;if(d.changed)windNativeCommit(d.w);else tap(d.mode,d.w);});el('wind-rose').addEventListener('pointercancel',()=>{windDrag=null;dirty.delete('wind');});
 return{render,setUnits,windArc:(a,b)=>{const d=wrap(b-a+180)-180;return{middle:wrap(a+d/2),spread:Math.abs(d)};}};
})();
