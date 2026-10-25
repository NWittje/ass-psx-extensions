'use strict';
(() => {
 const dialog=document.getElementById('terrain-dialog'),proceed=document.getElementById('terrain-continue'),abort=document.getElementById('terrain-abort');
 let pending=null,online=false,sending=false,choice=null,request='',dismissed=false,abortQueued=false,ignoredCloses=0;
 const waiting=op=>op?.state==='awaiting-confirmation'&&op.terrain?.verdict==='missing';
 function close(){if(dialog.open){++ignoredCloses;dialog.close();}}
 function buttons(){proceed.disabled=!online||sending||choice===false;abort.disabled=sending||choice===true;}
 async function decide(value){
  if(!pending||sending||(choice!==null&&choice!==value))return;
  choice=value;request ||= globalThis.IOS.requestId();
  if(!online){if(!value){dismissed=true;abortQueued=true;close();}buttons();return;}
  sending=true;buttons();const op=pending;
  try{
   const result=await globalThis.IOS.api('reposition/terrain-confirm',{operation_id:op.id,target_id:op.terrain.target_id,request_id:request,continue:value});
   if(pending?.id!==op.id)return;
   if(!waiting(result)||result.terrain?.decision){pending=null;close();}
  }catch(error){globalThis.IOS.message(error.message);}
  finally{sending=false;buttons();}
 }
 function render(status){
  online=!!status?.ready;
  if(!status){buttons();return;}
  const op=status.reposition_preflight;
  if(!waiting(op)){pending=null;choice=null;request='';abortQueued=false;dismissed=false;close();return;}
  if(pending?.id!==op.id){pending=op;choice=null;request='';dismissed=false;abortQueued=false;}else pending=op;
  if(op.terrain.decision){close();buttons();return;}
  if(abortQueued&&online&&!sending){abortQueued=false;void decide(false);return;}
  if(!dismissed&&!dialog.open){dialog.showModal();abort.focus();}
  buttons();
 }
 proceed.addEventListener('click',()=>decide(true));abort.addEventListener('click',()=>decide(false));
 dialog.addEventListener('cancel',event=>{event.preventDefault();void decide(false);});
 dialog.addEventListener('close',()=>{if(ignoredCloses){--ignoredCloses;return;}if(pending&&choice===null&&!pending.terrain?.decision)void decide(false);});
 globalThis.TerrainPreflightUI={render};
})();
