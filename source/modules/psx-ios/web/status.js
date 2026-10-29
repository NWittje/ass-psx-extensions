'use strict';
(() => {
 const $=id=>document.getElementById(id),ns='http://www.w3.org/2000/svg',page=$('status-page');if(!page)return;
 const make=(name,attrs={},text)=>{const n=document.createElementNS(ns,name);for(const [k,v]of Object.entries(attrs))n.setAttribute(k,v);if(text!==undefined)n.textContent=text;return n;};
 const drawing=$('status-controls'),marks={},bars={};
 drawing.append(make('rect',{x:85,y:20,width:108,height:108,class:'frame'}),make('path',{d:'M139 24V124M89 74H189',class:'frame','stroke-dasharray':'3 4'}));
 marks.xy=make('circle',{cx:139,cy:74,r:4,class:'input-mark'});drawing.append(marks.xy,make('text',{x:139,y:144,'text-anchor':'middle'},'Aileron / Elevator'));
 drawing.append(make('rect',{x:85,y:158,width:108,height:15,class:'frame'}));marks.rudder=make('rect',{x:135,y:159,width:8,height:13,class:'input-mark'});drawing.append(marks.rudder,make('text',{x:139,y:190,'text-anchor':'middle'},'Rudder'));
 drawing.append(make('circle',{cx:38,cy:86,r:26,class:'frame'}));marks.tiller=make('path',{d:'M38 86V62',class:'input-mark'});drawing.append(marks.tiller,make('text',{x:38,y:131,'text-anchor':'middle'},'Tiller'));
 for(const [key,count,x,label]of [['engine',4,220,''],['spoilers',11,335,'Spoiler'],['flaps',4,373,'Flaps']]){
  bars[key]=[];const span=key==='flaps'?32:24,width=key==='engine'?18:span/count;
  for(let i=0;i<count;++i){const bx=x+i*(key==='engine'?25:width);if(key==='engine'||i===0)drawing.append(make('rect',{x:bx,y:20,width:key==='engine'?18:span,height:150,class:'frame'}));const bar=make('rect',{x:bx,y:key==='flaps'?20:170,width:width,height:0,class:key==='engine'?'engine-bar':'surface-bar'});drawing.append(bar);bars[key].push(bar);if(key==='engine')drawing.append(make('text',{x:bx+9,y:12,'text-anchor':'middle'},String(i+1)));}
  drawing.append(make('text',{x:key==='engine'?265:x+span/2,y:190,'text-anchor':'middle'},label));
 }
 // Nominal flap units at native TE coordinates; UP and1 share zero TE extension.
 for(const [index,label]of ['UP/1','5','10','20','25','30'].entries()){
  const y=Math.max(29,Math.min(162,20+150*index/5));
  drawing.append(make('text',{x:389,y,'text-anchor':'middle','dominant-baseline':'middle',class:'flap-unit-label','data-flap-unit':label},label));
 }
 const engineText=make('text',{x:265,y:207,'text-anchor':'middle'});drawing.append(engineText);
 const damage=globalThis.createAircraftDamage?.($('status-aircraft-damage'),$('status-aircraft-damage-summary'),$('status-aircraft-damage-availability'));
 let visible=false,units='metric',selection=null,fastTimer,summaryTimer,fastGeneration=0,summaryGeneration=0,traceFloor=0,lastRunway="",selectionChanged=false,retainedTrace=[],retainedRunway=null,traceGeneration=null;
 function barValues(key,values,max){bars[key].forEach((bar,i)=>{const v=values?.[i];bar.classList.toggle('missing',!Number.isFinite(v));const raw=v*2250,fraction=key==='flaps'?(raw<=250?raw/1250:.2+(raw-250)/2500):v/max;const height=Number.isFinite(v)?150*Math.max(0,Math.min(1,fraction)):0;bar.setAttribute('y',key==='flaps'?20:170-height);bar.setAttribute('height',height);bar.setAttribute('aria-label',key+' '+(i+1)+': '+(Number.isFinite(v)?v.toFixed(2):'unavailable'));});}
 function controls(s){const ready=!!s?.ready,a=ready?s.axes:null;marks.xy.classList.toggle('missing',!a);marks.xy.setAttribute('cx',139+(a?.[1]||0)/999*50);marks.xy.setAttribute('cy',74-(a?.[0]||0)/999*50);marks.rudder.setAttribute('x',135+(a?.[2]||0)/999*50);marks.rudder.classList.toggle('missing',!a);marks.tiller.setAttribute('transform','rotate('+((ready?s.tiller:0)||0)/999*70+' 38 86)');marks.tiller.classList.toggle('missing',!ready||s.tiller==null);const e=ready?s.engine_model:null,n=e===0?s.n1:e===1||e===2?s.epr:null;barValues('engine',n,e===0?110:2);barValues('spoilers',ready?s.spoilers:null,1);barValues('flaps',ready?s.flaps:null,1);engineText.textContent=n?(e===0?'N1 %':'EPR')+' · '+n.map(v=>e===0?v.toFixed(1):v.toFixed(2)).join(' / '):'Engine output unavailable';$('status-source').textContent=ready?(s.fast_available?'':s.source||'PSX TCP'):'';}
 const approach=$('status-approach');let plotPoints=[],plotRunway=null;
 function plot(points,runway){
  plotPoints=points||[];plotRunway=runway;approach.replaceChildren();
  const width=Math.max(320,approach.clientWidth||500),height=Math.max(220,approach.clientHeight||280);
  approach.setAttribute('viewBox',`0 0 ${width} ${height}`);
  const add=(name,attrs,text,parent=approach)=>{const n=make(name,attrs,text);parent.append(n);return n;};
  const label=(x,y,text,attrs={},parent=approach)=>add('text',{x,y,...attrs},text,parent);
  const left=58,threshold=width-34,span=threshold-left,X=nm=>threshold-nm*span/15;
  const angle=runway?.angle_deg||3,tch=runway?.threshold_height_ft||0,rad=Math.PI/180;
  const pathHeight=(nm,degrees)=>tch+nm*6076.11548556*Math.tan(degrees*rad);
  // Shared distance axis; feet remain threshold-relative. Bounds are display references.
  const ceiling=Math.max(6000,Math.ceil(pathHeight(15,angle+.7)/1000)*1000);
  const gsTop=26,gsBottom=height*.40,G=ft=>gsBottom-ft/ceiling*(gsBottom-gsTop);
  const locCentre=height*.76,locHalf=height*.145,L=ft=>locCentre+ft/4500*locHalf;
  const lateral=nm=>nm*6076.11548556*Math.tan(2.5*rad);
  const gs=`M${X(15)} ${G(pathHeight(15,angle+.7))}L${X(0)} ${G(tch)}L${X(15)} ${G(pathHeight(15,angle-.7))}L${X(13)} ${G(pathHeight(13,angle))}Z`;
  const loc=`M${X(15)} ${L(-lateral(15))}L${X(0)} ${L(0)}L${X(15)} ${L(lateral(15))}L${X(13)} ${L(0)}Z`;
  add('title',{},'Glideslope and localizer approach traces');
  add('desc',{},'15 NM to the runway threshold. Dashed reference paths; glideslope bounds plus/minus 0.7 degrees and localizer bounds plus/minus 2.5 degrees. Light shading below the glidepath and on the aircraft-right side of the localizer. Magenta dots show the recorded aircraft path.');
  const defs=add('defs');
  for(const [id,d]of [['status-gs-arrow-clip',gs],['status-loc-arrow-clip',loc]])add('path',{d},undefined,add('clipPath',{id},undefined,defs));
  for(const nm of [10,5,3])add('line',{x1:X(nm),x2:X(nm),y1:gsTop,y2:L(4500),class:'approach-range','data-distance-nm':nm});
  const vertical=add('g',{id:'status-vertical'}),horizontal=add('g',{id:'status-horizontal'});
  add('path',{d:`M${left} ${gsTop}V${gsBottom}H${threshold}`,class:'approach-axis'},undefined,vertical);
  add('path',{d:`M${left} ${L(-4500)}V${L(4500)}`,class:'approach-axis'},undefined,horizontal);
  for(const ft of [ceiling,ceiling/2,0])label(left-7,G(ft)+3,String(ft),{'text-anchor':'end'},vertical);
  for(const [ft,text]of [[-4000,'4000 L'],[0,'0'],[4000,'4000 R']])label(left-7,L(ft)+3,text,{'text-anchor':'end'},horizontal);
  add('path',{d:`M${X(15)} ${G(pathHeight(15,angle))}L${X(0)} ${G(tch)}V${gsBottom}H${left}Z`,class:'approach-shade','clip-path':'url(#status-gs-arrow-clip)'},undefined,vertical);
  // Inbound travel points right on the page, so aircraft-right is the lower half.
  add('rect',{x:left,y:L(0),width:span,height:locHalf,class:'approach-shade','clip-path':'url(#status-loc-arrow-clip)'},undefined,horizontal);
  add('path',{d:gs,class:'approach-arrow','data-lower-deg':angle-.7,'data-upper-deg':angle+.7},undefined,vertical);
  add('path',{d:loc,class:'approach-arrow','data-lower-deg':-2.5,'data-upper-deg':2.5},undefined,horizontal);
  add('line',{x1:X(15),y1:G(pathHeight(15,angle)),x2:X(0),y2:G(tch),class:'approach-nominal'},undefined,vertical);
  add('line',{x1:X(15),y1:L(0),x2:X(0)+24,y2:L(0),class:'approach-nominal'},undefined,horizontal);
  add('rect',{x:threshold,y:gsBottom,width:24,height:3,class:'approach-runway'},undefined,vertical);
  add('rect',{x:threshold,y:L(0)-6,width:24,height:12,class:'approach-runway'},undefined,horizontal);
  label(left,12,'Glideslope · ft',{class:'approach-heading'},vertical);
  label(width-3,12,angle.toFixed(1)+'° ±0.7°',{'text-anchor':'end'},vertical);
  label(left,height*.59,'Localizer · ft',{class:'approach-heading'},horizontal);
  label(width-3,height*.59,'±2.5°',{'text-anchor':'end'},horizontal);
  label(X(11)+4,G(pathHeight(11,angle+.7))-7,(angle+.7).toFixed(1)+'°',{},vertical);
  label(X(11)+4,G(pathHeight(11,angle-.7))+13,(angle-.7).toFixed(1)+'°',{},vertical);
  label((left+threshold)/2,height*.49,runway?.id||'Runway unavailable',{id:'status-runway-id',class:'approach-runway-id','text-anchor':'middle'});
  label((left+threshold)/2,height*.49+12,runway?runway.reference+' · '+angle.toFixed(1)+'°':'',{id:'status-glide-reference',class:'approach-source','text-anchor':'middle'});
  for(const nm of [15,10,5,3,0])label(X(nm),height-5,String(nm),{'text-anchor':'middle','data-distance-label':nm});
  label(width-2,height-5,'NM',{'text-anchor':'end'});
  for(const p of plotPoints){const x=X(p[0]);if(x<left||x>threshold)continue;
   for(const [group,value,Y,min,max]of [[vertical,p[2],G,0,ceiling],[horizontal,p[1],L,-4500,4500]])if(Number.isFinite(value)&&value>=min&&value<=max)add('circle',{cx:x,cy:Y(value),r:1.2,class:'trace-dot'},undefined,group);
  }
 }
 if(typeof ResizeObserver!=='undefined')new ResizeObserver(()=>plot(plotPoints,plotRunway)).observe(approach);
 function summary(s){const p=s?.ready?s.performance:null;let gw=p?.gross_weight_kg;$('status-gw').textContent='GW: '+(Number.isFinite(gw)?Math.round(units==='imperial'?gw/0.45359237:gw).toLocaleString('en-US')+' '+(units==='imperial'?'lb':'kg'):'—');['v1','vr','v2'].forEach((k,i)=>$('status-'+k).textContent=s?.ready?(s.takeoff?.[i]??'—'):'—');for(const [key,field]of [['flaps-up','flaps_up'],['vref25','ref25'],['vref30','ref30']])$('status-'+key).textContent=p?.[field]??'—';if(s?.ready&&traceGeneration!==s.trace_generation){retainedTrace=[];retainedRunway=null;traceFloor=0;}traceGeneration=s?.trace_generation;const r=s?.ready?(s.runway||retainedRunway):null;let points=s?.ready?s.trace||[]:[];if((selectionChanged&&!r?.fmc)||(lastRunway&&r?.id&&lastRunway!==r.id)){traceFloor=Math.max(0,...points.map(p=>p[3]))+1;retainedTrace=[];retainedRunway=null;}selectionChanged=false;lastRunway=r?.id||'';points=points.filter(p=>p[3]>=traceFloor);if(points.length){retainedTrace=points;retainedRunway=r;}else if(s?.ready&&r?.id===retainedRunway?.id)points=retainedTrace;plot(points,r);$('status-metar-airport').textContent='METAR'+(s?.ready&&s.metar_airport?' · '+s.metar_airport:'');$('status-metar-text').textContent=s?.ready&&s.metar?s.metar:'Unavailable';}
 async function pollFast(g){if(!visible||g!==fastGeneration)return;const start=performance.now();try{const s=await globalThis.IOSStatusAPI('status/controls');if(g===fastGeneration&&visible)controls(s);}catch{if(g===fastGeneration)controls(null);}finally{if(visible&&g===fastGeneration)fastTimer=setTimeout(()=>pollFast(g),Math.max(0,1000/15-(performance.now()-start)));}}
 function xplane(s){
  $('status-xplane-state').textContent=['Ready','Loading','Unknown','Unavailable'].includes(s?.state)?s.state:'Unavailable';
  $('status-xplane-phase').textContent=s?.phase||'Unavailable';$('status-xplane-progress').textContent=s?.progress||'Progress unavailable';
  const value=Number.isFinite(s?.cpu_percent)?Math.max(0,Math.min(100,s.cpu_percent)):null,meter=$('status-xplane-cpu');
  $('status-xplane-cpu-value').textContent=value===null?'—':Math.round(value)+'%';
  meter.style.setProperty('--cpu-percent',(value??0)+'%');meter.classList.toggle('missing',value===null);
  meter.setAttribute('role',value===null?'img':'meter');
  if(value===null){meter.removeAttribute('aria-valuenow');meter.removeAttribute('aria-valuemin');meter.removeAttribute('aria-valuemax');meter.setAttribute('aria-valuetext','Unavailable');}else{meter.setAttribute('aria-valuemin',0);meter.setAttribute('aria-valuemax',100);meter.setAttribute('aria-valuenow',value);meter.setAttribute('aria-valuetext',Math.round(value)+' percent');}
  $('status-xplane-io').textContent=Number.isFinite(s?.io_mb_s)?s.io_mb_s.toFixed(1)+' MB/s':'—';
  const rows=$('status-xplane-logs').children,lines=Array.isArray(s?.logs)?s.logs.slice(-5):[];
  for(let i=0;i<rows.length;++i)rows[i].textContent=typeof lines[i]==='string'?lines[i]:'';
 }
 async function pollSummary(g){if(!visible||g!==summaryGeneration)return;const start=performance.now();try{const results=await Promise.allSettled([globalThis.IOSStatusAPI('status/summary',selection||undefined),globalThis.IOSStatusAPI('status/xplane')]);if(g===summaryGeneration&&visible){summary(results[0].status==='fulfilled'?results[0].value:null);xplane(results[1].status==='fulfilled'?results[1].value:null);}}finally{if(visible&&g===summaryGeneration)summaryTimer=setTimeout(()=>pollSummary(g),Math.max(0,1000-(performance.now()-start)));}}
 globalThis.StatusUI={setVisible(on){if(visible===on)return;visible=on;++fastGeneration;++summaryGeneration;clearTimeout(fastTimer);clearTimeout(summaryTimer);if(on){pollFast(fastGeneration);pollSummary(summaryGeneration);}},setUnits(v){units=v;},selection(a,r){const next=a&&r?{airport:a,runway:r}:null;if(JSON.stringify(next)!==JSON.stringify(selection))selectionChanged=true;selection=next;},render(s){damage?.update(s,[]);if(!s?.ready){traceFloor=0;lastRunway='';selectionChanged=false;retainedTrace=[];retainedRunway=null;traceGeneration=null;controls(null);summary(null);}}};
 controls(null);summary(null);
})();
