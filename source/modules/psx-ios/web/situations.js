'use strict';
(()=>{
 const $=id=>document.getElementById(id),page=$('situations-page');
 let visible=false,status=null,entries=[],selected=null,revision=null,loadingList=false,busy=false,pending=null,dialogOperation=null,overwriteSelection=null,cancelled=null;
 let deleteEntries=[],deleteSelected=new Set(),deleteConfirmed=false,deleteSubmitting=false,deleteInputs=[],deleteAnchor=-1,deleteRevision=null;
 const api=(path,body)=>globalThis.IOS.api('situations'+(path?'/'+path:''),body);
 function buttons(){
  const locked=busy||status?.command_busy||status?.situations?.busy||['queued','sent'].includes(status?.operation?.state);
  $('situation-save').disabled=!status?.ready||locked||!status?.situations?.save_available||!!selected?.error;
  $('situation-load').disabled=!status?.ready||locked||!status?.situations?.load_available||!selected||!!selected.error;
  $('situation-delete').disabled=!status?.ready||locked||!status?.situations?.delete_available||!entries.some(entry=>entry.hash);
  $('situation-delete').title=$('situation-delete').disabled?(status?.situations?.reason||'No situations available'):'';
  for(const action of ['save','load'])$('situation-'+action).title=$('situation-'+action).disabled?(selected?.error||status?.situations?.reason||'Select a situation'):'';
  $('situation-list').disabled=!!locked;
 }
 function clear(){selected=null;$('situation-list').selectedIndex=-1;$('situation-description').textContent='';buttons();}
 function select(){selected=entries[Number($('situation-list').value)]||null;$('situation-description').textContent=selected?.error||selected?.description||'';buttons();}
 async function list(){
  if(loadingList)return;loadingList=true;
  try{
   const data=await api('');const old=selected?.id;entries=data.entries||[];revision=data.revision;
   $('situation-list').replaceChildren(...entries.map((entry,index)=>{const option=document.createElement('option');option.value=String(index);option.textContent=entry.name;return option;}));
   const index=entries.findIndex(entry=>entry.id===old);$('situation-list').selectedIndex=index;selected=index>=0?entries[index]:null;
   $('situation-description').textContent=selected?.error||selected?.description||data.error||'';buttons();
  }catch(error){globalThis.IOS.message(error.message);}finally{loadingList=false;}
 }
 async function send(action,fields){
  busy=true;buttons();globalThis.IOS.message('');
  try{
   const op=await api(action,{...fields,request_id:globalThis.IOS.requestId(),expected_revision:status.revision});
   status.operation=op;pending={id:op.id,kind:'situation-'+action};
   if(action==='load')dialogOperation=op.id;
   return true;
  }catch(error){globalThis.IOS.message(error.message);return false;}
  finally{busy=false;buttons();}
 }
 $('situation-list').addEventListener('change',select);
 page.addEventListener('click',event=>{if(!event.target.closest('#situation-list,.situation-actions'))clear();});
 $('situation-save').addEventListener('click',()=>{
  if(selected){overwriteSelection={...selected};$('situation-overwrite-text').textContent=`Overwrite “${selected.name}” with the current situation?`;$('situation-overwrite-dialog').showModal();$('situation-overwrite-cancel').focus();}
  else{$('situation-name').value='';$('situation-new-description').value='';$('situation-save-dialog').showModal();$('situation-name').focus();}
 });
 $('situation-new-cancel').addEventListener('click',()=>$('situation-save-dialog').close());
 $('situation-new-save').addEventListener('click',async()=>{
  if(!$('situation-name').value.trim()){$('situation-name').focus();return;}
  $('situation-new-save').disabled=true;
  if(await send('save',{name:$('situation-name').value.trim(),description:$('situation-new-description').value}))$('situation-save-dialog').close();
  $('situation-new-save').disabled=false;
 });
 $('situation-overwrite-cancel').addEventListener('click',()=>$('situation-overwrite-dialog').close());
 $('situation-overwrite-save').addEventListener('click',async()=>{
  if(!overwriteSelection)return;$('situation-overwrite-save').disabled=true;
  if(await send('save',{id:overwriteSelection.id,hash:overwriteSelection.hash,overwrite:true}))$('situation-overwrite-dialog').close();
  $('situation-overwrite-save').disabled=false;
 });
 $('situation-load').addEventListener('click',()=>{if(selected)return send('load',{id:selected.id,hash:selected.hash});});
 function deleteFields(){
  const bits=Array(Math.ceil(deleteEntries.length/4)).fill(0);
  deleteEntries.forEach((entry,index)=>{if(deleteSelected.has(entry.id))bits[index>>2]|=1<<(index&3);});
  const hex=bits.map(value=>value.toString(16)).join('');
  return {catalogue_revision:deleteRevision,selection:(hex.match(/.{1,256}/g)||[]).map(bits=>({bits})),confirmed:true};
 }
 function deleteButtons(){
  const count=deleteSelected.size;
  $('situation-delete-confirm').disabled=deleteSubmitting||!count;
  $('situation-delete-confirm').textContent=deleteConfirmed?'Confirm':'Delete';
  $('situation-delete-cancel').disabled=deleteSubmitting;
  $('situation-delete-message').textContent=deleteConfirmed?`Delete ${count} selected ${count===1?'situation':'situations'}?`:'';
  for(const input of deleteInputs)input.disabled=deleteSubmitting||!deleteEntries[Number(input.value)].hash;
 }
 function closeDelete(){if(deleteSubmitting)return;$('situation-delete-dialog').close();deleteConfirmed=false;deleteSelected.clear();deleteInputs=[];deleteAnchor=-1;deleteRevision=null;}
 $('situation-delete').addEventListener('click',()=>{
  deleteEntries=entries.map(entry=>({...entry}));deleteSelected=new Set();deleteConfirmed=false;deleteSubmitting=false;deleteInputs=[];deleteAnchor=-1;deleteRevision=revision;
  $('situation-delete-list').replaceChildren(...deleteEntries.map((entry,index)=>{
   const label=document.createElement('label'),input=document.createElement('input'),text=document.createElement('span');input.type='checkbox';input.value=String(index);input.disabled=!entry.hash;deleteInputs.push(input);text.textContent=entry.name;
   if(!entry.hash)label.title=entry.error||'Situation identity unavailable';
   input.addEventListener('click',event=>{
    if(event.shiftKey&&deleteAnchor>=0){
     const checked=input.checked;
     for(let i=Math.min(deleteAnchor,index);i<=Math.max(deleteAnchor,index);++i){
      const box=deleteInputs[i];if(box.disabled)continue;box.checked=checked;
      if(checked)deleteSelected.add(deleteEntries[i].id);else deleteSelected.delete(deleteEntries[i].id);
     }
    }
    deleteAnchor=index;
   });
   input.addEventListener('change',()=>{if(input.checked)deleteSelected.add(entry.id);else deleteSelected.delete(entry.id);deleteConfirmed=false;deleteButtons();});label.append(input,text);return label;
  }));deleteButtons();$('situation-delete-dialog').showModal();$('situation-delete-cancel').focus();
 });
 $('situation-delete-cancel').addEventListener('click',closeDelete);
 $('situation-delete-dialog').addEventListener('cancel',event=>{event.preventDefault();closeDelete();});
 $('situation-delete-confirm').addEventListener('keydown',event=>{if(event.repeat)event.preventDefault();});
 $('situation-delete-confirm').addEventListener('click',async event=>{
  if(deleteSubmitting||!deleteSelected.size||event.detail>1)return;
  if(!deleteConfirmed){deleteConfirmed=true;deleteButtons();return;}
  deleteSubmitting=true;deleteButtons();await send('delete',deleteFields());deleteSubmitting=false;closeDelete();revision=null;if(visible)list();
 });
 async function cancel(){
  if(dialogOperation===null)return;
  try{await api('cancel',{operation:dialogOperation});cancelled=dialogOperation;$('situation-check-dialog').close();dialogOperation=null;}
  catch(error){globalThis.IOS.message(error.message);}
 }
 $('situation-check-cancel').addEventListener('click',cancel);
 $('situation-check-dialog').addEventListener('cancel',event=>{event.preventDefault();cancel();});
 function checklist(op){
  if(op?.result?.stage!=='checking'||op.id===cancelled||!['queued','sent'].includes(op.state)){
   if($('situation-check-dialog').open)$('situation-check-dialog').close();return;
  }
  dialogOperation=op.id;
  const rows=op.result.checklist||[],key=JSON.stringify(rows);
  if($('situation-checks').dataset.state!==key){
   $('situation-checks').dataset.state=key;
   $('situation-checks').replaceChildren(...rows.map(row=>{
    const item=document.createElement('li');item.className=row.match?'situation-match':'situation-mismatch';
    const mark=document.createElement('span');mark.className='situation-check-mark';mark.textContent=row.match?'✓':'✗';mark.setAttribute('aria-hidden','true');
    const label=document.createElement('span');label.textContent=`${row.label} — ${row.target}`;
    const state=document.createElement('span');state.className='sr-only';state.textContent=row.match?'matched':row.available?'adjust control':'current position unavailable';
    item.append(mark,label,state);if(!row.available)item.title='Current position unavailable';return item;
   }));
  }
  if(!$('situation-check-dialog').open){$('situation-check-dialog').showModal();$('situation-check-cancel').focus();}
 }
 function render(value){
  status=value;if(!value){buttons();if($('situation-check-dialog').open)$('situation-check-dialog').close();if(!deleteSubmitting)closeDelete();return;}
  if(!value.ready&&!deleteSubmitting)closeDelete();
  if(visible&&value.situations?.revision!==revision)list();
  const operation=value.situations?.operation||value.operation;
  if(operation?.kind==='situation-load'){checklist(operation);if(operation.state!=='sent'&&operation.state!=='queued'){pending=null;dialogOperation=null;}}
  else if(pending&&operation?.id===pending.id&&!['queued','sent'].includes(operation.state))pending=null;
  buttons();
 }
 globalThis.SituationsUI={render,setVisible(value){visible=value;if(value&&revision!==status?.situations?.revision)list();},disconnected(){render(null);}};
})();
