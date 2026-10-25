'use strict';
globalThis.RepositionGraphic=(()=>{
 const input=document.getElementById('height'),marker=document.getElementById('height-marker'),handle=document.getElementById('height-drag'),root=document.getElementById('position-profile-svg');
 const ceiling=41000,minimum=200,scale=AltitudeScale.create(ceiling,25,350);
 let drag=null,changed=()=>{};
 const step=()=>100;
 const snap=h=>Math.max(minimum,Math.min(ceiling,Math.round(h/100)*100));
 const y=scale.y;
 const height=y=>snap(scale.height(y));
 function svg(name,attrs){const e=document.createElementNS('http://www.w3.org/2000/svg',name);for(const[k,v]of Object.entries(attrs))e.setAttribute(k,String(v));return e;}
 function update(h=Number(input.value)||5000){input.value=String(Math.round(h));input.step=String(step(h));handle.setAttribute('aria-valuenow',String(Math.round(h)));handle.setAttribute('aria-valuetext',`${Math.round(h)} feet above runway threshold`);marker.style.top=`${y(h)/400*100}%`;}
 function draft(h){update(snap(h));changed();}
 function point(e){const p=root.createSVGPoint();p.x=e.clientX;p.y=e.clientY;return p.matrixTransform(root.getScreenCTM().inverse());}
 // Draw in CSS pixels: runway markings share one uniform affine projection.
 // Button anchors and the glidepath use the same responsive coordinates.
 function draw(){
  const width=root.clientWidth||800,depth=root.clientHeight||400,unit=width/800;
  root.setAttribute('viewBox',`0 0 ${width} ${depth}`);root.replaceChildren();
  const threshold={x:.7*width,y:.875*depth};
  const project=(along,across)=>({x:threshold.x+unit*(along+.55*across),y:threshold.y+unit*(-.12*along+.48*across)});
  const path=points=>points.map((p,i)=>`${i?'L':'M'}${p.x} ${p.y}`).join(' ');
  const plane=(points,attrs)=>svg('path',{d:path(points.map(([a,b])=>project(a,b)))+'Z',...attrs});
  const surface=[[0,-24],[210,-24],[210,24],[0,24]],near=surface.map(([a,b])=>project(a,b));
  root.append(svg('path',{d:path([near[3],near[2],{x:near[2].x,y:near[2].y+7*unit},{x:near[3].x,y:near[3].y+7*unit}])+'Z',fill:'var(--ios-edge)'}),plane(surface,{fill:'var(--ios-background)',stroke:'var(--ios-text)','stroke-width':2}));
  for(const side of [-1,1])for(let i=0;i<3;++i){const across=side*(7+i*5);root.append(plane([[8,across-1.5],[30,across-1.5],[30,across+1.5],[8,across+1.5]],{fill:'var(--ios-text)'}));}
  for(let a=46;a<195;a+=42)root.append(plane([[a,-1.5],[a+22,-1.5],[a+22,1.5],[a,1.5]],{fill:'var(--ios-text)'}));
  const final6=root.parentElement?.querySelector('.profile-final6'),final3=root.parentElement?.querySelector('.profile-final3');
  const first={x:.165*width,y:.18*depth+(final6?.offsetHeight||48)+8},middle={x:.425*width,y:0};
  middle.y=first.y+(threshold.y-first.y)*(middle.x-first.x)/(threshold.x-first.x);
  if(final3)final3.style.top=`${middle.y-(final3.offsetHeight||48)-8}px`;
  const start={x:.05*width,y:first.y-(threshold.y-first.y)*(first.x-.05*width)/(threshold.x-first.x)};
  root.append(svg('path',{d:path([start,threshold]),fill:'none',stroke:'var(--ios-edge)','stroke-width':2}),svg('path',{d:path([{x:start.x,y:start.y-10},threshold])+path([{x:start.x,y:start.y+10},threshold]),fill:'none',stroke:'var(--ios-edge)','stroke-width':1}),svg('path',{d:`M${threshold.x} ${threshold.y}V${25/400*depth}`,stroke:'var(--ios-edge)','stroke-width':2,'stroke-dasharray':'3 6'}),svg('circle',{cx:threshold.x,cy:threshold.y,r:4,fill:'var(--ios-text)'}));
  for(const mark of [first,middle])root.append(svg('path',{d:`M${mark.x} ${mark.y}V${mark.y-8}`,stroke:'var(--ios-edge)','stroke-width':2}));
 }
 draw();if(typeof ResizeObserver!=='undefined')new ResizeObserver(draw).observe(root);
 handle.addEventListener('pointerdown',e=>{if(input.disabled)return;drag=e.pointerId;handle.setPointerCapture(e.pointerId);e.preventDefault();});
 handle.addEventListener('pointermove',e=>{if(drag===e.pointerId)draft(height(point(e).y/(root.clientHeight||400)*400));});
 handle.addEventListener('pointerup',()=>{drag=null;});handle.addEventListener('pointercancel',()=>{drag=null;});
 handle.addEventListener('keydown',e=>{if(input.disabled)return;if(!['ArrowUp','ArrowDown','ArrowLeft','ArrowRight','Home','End'].includes(e.key))return;e.preventDefault();const h=Number(input.value),up=e.key==='ArrowUp'||e.key==='ArrowRight';draft(e.key==='Home'?minimum:e.key==='End'?ceiling:h+(up?step(h):-step(h-1)));});
 input.addEventListener('change',()=>{const h=Number(input.value);if(!input.value||!Number.isFinite(h)||h<minimum||h>ceiling){input.setAttribute('aria-invalid','true');return;}input.removeAttribute('aria-invalid');draft(h);});
 update();return{update,onChange:f=>{changed=f;},snap,step,y,height};
})();
