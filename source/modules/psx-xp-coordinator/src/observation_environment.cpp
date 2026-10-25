#include "observation_internal.hpp"
#include "coordinator/environment_time.hpp"
namespace coordinator {
namespace w=environment_wire;
void ObservationController::Impl::drain_environment(){
 std::lock_guard<std::mutex> lock(mutex);auto now=qpc_now();
 VisibilityIpcEvent ipc;for(std::size_t n=0;n<16&&non_normal.pop_visibility(ipc);++n){
  if(ipc.kind==VisibilityIpcEvent::Kind::open)environment.visibility_ios(ipc.epoch,ipc.session,true);
  else if(ipc.kind==VisibilityIpcEvent::Kind::lost)environment.visibility_ios(ipc.epoch,ipc.session,false);
  else{const auto result=environment.visibility_request(ipc.request,qpc_now());if(!non_normal.visibility_result(ipc.epoch,result))environment.visibility_ios(ipc.epoch,ipc.session,false);}
 }

 if(view.bridge==State::connected&&view.bridge_peer_process_id&&environment_attachment!=view.bridge_attachments){environment_service.plugin(terrain_process_identity(view.bridge_peer_process_id));environment_attachment=view.bridge_attachments;}
 else if(view.bridge!=State::connected&&environment_attachment){environment_service.plugin({});environment_attachment=0;}
 if(environment_capture.status().active&&!capture.status().active)environment_capture.finish(now);
 EnvironmentEvent e;for(std::size_t n=0;n<32&&environment_service.pop(e);n++){
  const bool admitted=e.role==w::Role::exporter?environment.view().exporter_connected:environment.view().plugin_connected;
  if(e.kind==EnvironmentEventKind::frame){if(e.received_qpc>=environment_capture_start)environment_capture.record(1,e.role,e.frame.bytes.data(),e.frame.size,e.received_qpc);w::Header h;presentation_wire::Header h2;if((w::read_header(e.frame.bytes.data(),w::header_size,h)&&h.kind==w::Kind::hello)||(presentation_wire::read_header(e.frame.bytes.data(),w::header_size,h2)&&h2.kind==presentation_wire::Kind::hello))environment_capture_pending[e.role==w::Role::exporter?1:0]=true;}
 const auto admission_begin=qpc_now();const auto admission=environment.admit(e);const auto admission_end=qpc_now();environment_trace.record(environment.diagnostic_input(),static_cast<unsigned>(admission));
 if(environment_capture.status().active&&environment.diagnostic_input().kind==w::Kind::applied&&(!environment_admission_trace_qpc||admission_end-environment_admission_trace_qpc>=qpc_frequency()/4)){
  environment_admission_trace_qpc=admission_end;
  // Acquisition already owns mutex here. io_event would take it recursively.
  if(diagnostics){psx::diagnostics::Event event;event.component=psx::diagnostics::Component::application;event.code=psx::diagnostics::Code::message;
   event.severity=psx::diagnostics::Severity::info;event.context=diagnostic_context(model.latest().session());event.context.connection_id=view.bridge==State::connected?view.plugin.connection_id:0;
   event.values={static_cast<std::uint64_t>(admission_begin),static_cast<std::uint64_t>(admission_end),static_cast<std::uint64_t>(e.received_qpc),environment.view().source.sequence,environment.view().weather_target,static_cast<std::uint64_t>(admission)};
   psx::diagnostics::text(event.text,"environment_report_admission_v1");diagnostics->emit_stamped(1,event);
   if(psx::diagnostics::valid(event)){psx::bridge::DiagnosticBatch batch;batch.connection_id=model.latest().session().connection_id;batch.count=1;batch.events[0]=event;capture.diagnostics(batch,event.qpc);}
  }
 }
 // Retry failures before a qualified Hello describe optional source absence, not
 // lost evidence. The reducer also ignores stale connection-generation losses.
 const bool connected=e.role==w::Role::exporter?environment.view().exporter_connected:environment.view().plugin_connected;
 if(e.kind==EnvironmentEventKind::lost&&admitted&&!connected)environment_capture.incomplete(e.error?e.error:ERROR_BROKEN_PIPE,now);
 if(admission==EnvironmentAdmission::fault){environment_capture.incomplete(ERROR_INVALID_DATA,now);environment_service.fault(e.role,ERROR_INVALID_DATA);}
 }
 now=qpc_now(); // Freshness uses a clock sampled after all newly popped ingress.
 environment.tick(now);
 if(environment.view().overflows!=environment_overflows_seen){environment_overflows_seen=environment.view().overflows;environment_service.fault(w::Role::plugin,ERROR_BUFFER_OVERFLOW);environment_capture.incomplete(ERROR_BUFFER_OVERFLOW,now);}
 const auto dropped=environment_service.dropped();if(dropped!=environment_dropped_seen){environment_dropped_seen=dropped;environment_capture.incomplete(ERROR_BUFFER_OVERFLOW,now);}
 const bool capturing=environment_capture.status().active;
 if(capturing!=environment_capture_enabled){environment_capture_enabled=capturing;++environment_capture_generation;environment_capture_pending={true,true};if(capturing){for(std::size_t lane=0;lane<2;lane++)if(lane?environment.view().exporter_connected:environment.view().plugin_connected){EnvironmentDispatch seed;seed.kind=w::Kind::hello;seed.hello=lane?environment.view().exporter:environment.view().plugin;seed.version2=lane?environment.view().exporter_v2:environment.view().plugin_v2;seed.capabilities=lane?environment.view().exporter_capabilities:environment.view().plugin_capabilities;environment_capture.dispatch(lane?w::Role::exporter:w::Role::plugin,seed,now,5);}if(environment.view().source_available){EnvironmentDispatch seed;seed.sample=environment.view().source;seed.version2=environment.view().exporter_v2;seed.context=environment.view().source_context;seed.context.source_sequence=seed.sample.sequence;seed.change=environment.view().source_change;seed.local_visibility=environment.view().local_visibility;environment_capture.dispatch(seed.version2?w::Role::exporter:w::Role::plugin,seed,now,5);}if(environment.weather_target().identity){EnvironmentDispatch seed;seed.version2=true;seed.capabilities=presentation_wire::visibility_presentation|presentation_wire::manual_weather;seed.sample=environment.view().source;seed.context=environment.view().source_context;seed.change=environment.view().source_change;seed.local_visibility=environment.view().local_visibility;seed.visibility=environment.view().visibility_policy;seed.weather=environment.weather_target();environment_capture.dispatch(w::Role::plugin,seed,now,5);}for(const auto& g:environment.view().groups)if(g.applied_available){EnvironmentDispatch seed;seed.kind=w::Kind::applied;seed.applied=g.applied;if((g.applied.groups&2)&&environment.view().applied_visibility_available){seed.version2=true;seed.visibility=environment.view().applied_visibility_policy;seed.weather_report={g.applied,seed.visibility,g.weather_target,g.current_lifecycle,g.current_sequence,g.current_source_end_qpc,g.weather_observed_qpc};}environment_capture.dispatch(w::Role::plugin,seed,now,5);}}}
 for(std::size_t lane=0;lane<2;lane++)if(environment_capture_pending[lane]&&(lane?environment.view().exporter_connected:environment.view().plugin_connected)){EnvironmentDispatch cap;cap.kind=w::Kind::capture;cap.capture.owner=environment.view().owner;cap.capture.generation=environment_capture_generation;cap.capture.enabled=capturing?1u:0u;const auto role=lane?w::Role::exporter:w::Role::plugin;cap.version2=lane?environment.view().exporter_v2:environment.view().plugin_v2;environment_capture.dispatch(role,cap,now);if(environment_service.send(role,cap))environment_capture_pending[lane]=false;}
 EnvironmentDispatch d;for(std::size_t n=0;n<16&&environment.next(d);n++){environment_capture.dispatch(w::Role::plugin,d,now);if(!environment_service.send(w::Role::plugin,d)){environment_service.fault(w::Role::plugin,ERROR_BUFFER_OVERFLOW);environment_capture.incomplete(ERROR_BUFFER_OVERFLOW,now);break;}}
 view.environment=environment.view();
 const auto& v=view.environment;const auto& g=v.groups[0];const auto t=environment_time_status(v,now,static_cast<std::uint64_t>(qpc_frequency()));
 std::uint64_t rate{};std::memcpy(&rate,&v.source.values[0],sizeof(rate));
 const auto bits=std::uint64_t(t.source_current)|(std::uint64_t(t.utc_qualified)<<1)|(std::uint64_t(t.rate_qualified)<<2)|
  (std::uint64_t(t.paused)<<3)|(std::uint64_t(t.motion_frozen)<<4)|(std::uint64_t(g.requested)<<5)|(std::uint64_t(g.owned)<<6)|
  (std::uint64_t(g.pending)<<7)|(std::uint64_t(t.report_current)<<8)|(std::uint64_t(t.readback_current)<<9)|(std::uint64_t(t.report_retained)<<10);
 environment_time_transitions.record({bits,std::uint64_t(t.phase)|(std::uint64_t(t.reason)<<32),v.owner,v.source.producer.high,v.source.producer.low,
  v.source.lifecycle,v.plugin.instance.high,v.plugin.instance.low,g.applied.provider_generation,t.rate_qualified?rate:0},now);
}
void ObservationController::Impl::environment_metrics(const EnvironmentView& v,std::int64_t now){
 std::array<EnvironmentDiagnosticRecord,8> records{};std::array<unsigned,8> slots{};std::size_t count{};std::uint64_t received{},coalesced{},pending{};EnvironmentTimeTransition time_transition;bool time_changed{};
 {std::lock_guard<std::mutex> lock(mutex);if(!environment_trace.due(now,static_cast<std::uint64_t>(qpc_frequency())))return;
  while(count<records.size()&&environment_trace.next(records[count],slots[count]))++count;
  received=environment_trace.received();coalesced=environment_trace.coalesced();pending=environment_trace.pending();time_changed=environment_time_transitions.next(time_transition);}
 const auto emit=[&](const char* text,std::array<std::uint64_t,6> values){io_event(psx::diagnostics::Code::metrics,psx::diagnostics::Component::application,psx::diagnostics::Severity::info,psx::diagnostics::ErrorDomain::none,0,text,values);};
 emit("Environment state: follow/peers/fresh,groups,currentMask,configuredMask,flags,dispatched",{std::uint64_t(v.following)|(std::uint64_t(v.exporter_connected)<<1)|(std::uint64_t(v.plugin_connected)<<2)|(std::uint64_t(v.source_available)<<3),std::uint64_t(v.exporter.groups)|(std::uint64_t(v.plugin.groups)<<32),v.source.valid_mask,v.source.configured_mask,v.source.flags,v.dispatched});
 emit("Environment domain: producerHigh,producerLow,owner,lifecycle,cloudCfg,sequence",{v.source.producer.high,v.source.producer.low,v.owner,v.source.lifecycle,v.source.configuration,v.source.sequence});
 emit("Environment clocks: startQPC,endQPC,receiptQPC,frame,writer,acks",{static_cast<std::uint64_t>(v.source.sample_start_qpc),static_cast<std::uint64_t>(v.source.sample_end_qpc),static_cast<std::uint64_t>(v.source.received_qpc),v.source.frame_sequence,v.source.writer_version,v.acks});
 for(std::size_t i=0;i<2;++i){const auto& g=v.groups[i];emit("Environment group: group,stateBits,reason,baselineFloor,renewedQPC,expiresQPC",{1ull<<i,std::uint64_t(g.available)|(std::uint64_t(g.requested)<<1)|(std::uint64_t(g.pending)<<2)|(std::uint64_t(g.owned)<<3)|(std::uint64_t(g.blocked)<<4)|(std::uint64_t(g.applied_available)<<5),static_cast<std::uint64_t>(g.reason),static_cast<std::uint64_t>(g.baseline_floor),static_cast<std::uint64_t>(g.renewed_qpc),static_cast<std::uint64_t>(g.expires_qpc)});}
 for(std::size_t i=0;i<count;++i){auto scope=records[i].input.scope;const bool ack=records[i].input.kind==w::Kind::ack;
  if(ack){scope[4]=records[i].observations;scope[5]=records[i].coalesced;}
  emit(ack?"Environment Ack: receiptQPC,operation,group/action/phase/reason/admission,accepted,life,cfg":"Environment Applied: receiptQPC,sourceSeq,group/phase/reason/admission,accepted,actual,SDK",records[i].input.head);
  emit(ack?"Environment Ack scope: producerHigh,producerLow,owner,duration,observations,coalesced":"Environment Applied scope: life,cfg,sourceEndQPC,appliedQPC,owner,providerGeneration",scope);
 }
 emit("Environment metadata: observedGroupRecords,coalesced,pendingSlots,emittedSlots,budget,capacity",{received,coalesced,pending,count,8,12});
 if(time_changed){emit("Environment time transition: originalQPC,stateBits,phase/reason,qualifiedRateBits,transitions,coalesced",time_transition.head);
  emit("Environment time scope: owner,sourceProducerHigh,sourceProducerLow,lifecycle,providerHigh,providerLow",time_transition.scope);}
}
}
