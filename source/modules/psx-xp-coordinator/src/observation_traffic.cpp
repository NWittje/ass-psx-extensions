#include "observation_internal.hpp"
namespace coordinator {
void ObservationController::Impl::queue_traffic_visual(TrafficVisualEventKind kind,psx::motion::ReconstructionSettings settings,bool enabled){
 if(kind==TrafficVisualEventKind::following)traffic_visual_wanted=enabled;
 if(traffic_visual_intent_size==traffic_visual_intents.size()){++traffic_visual_intent_overflows;traffic_visual_intent_head=traffic_visual_intent_size=0;traffic_visual_wanted=false;SetEvent(acquisition_ready.get());return;}
 TrafficVisualEvent e;e.kind=kind;e.received_qpc=qpc_now();e.policy=settings;e.enabled=enabled;traffic_visual_intents[(traffic_visual_intent_head+traffic_visual_intent_size)%traffic_visual_intents.size()]=e;++traffic_visual_intent_size;SetEvent(acquisition_ready.get());
}
void ObservationController::Impl::traffic_visual_input(const TrafficVisualEvent& e,std::int64_t now){
 const auto admission=traffic_visual.event(e,now);traffic_capture.visual_record(e,now,admission);if(admission==TrafficVisualAdmission::fault)traffic_visual_service.fault(ERROR_INVALID_DATA);
}
void ObservationController::Impl::traffic_visual_capture_intent(bool enabled,std::int64_t now){
 if(traffic_visual_capture_enabled==enabled)return;traffic_visual_capture_enabled=enabled;
 if(traffic_visual_capture_generation==UINT64_MAX){traffic_capture.incomplete(ERROR_ARITHMETIC_OVERFLOW,now);return;}
 TrafficVisualEvent e;e.kind=TrafficVisualEventKind::capture;e.enabled=enabled;e.capture_generation=++traffic_visual_capture_generation;e.received_qpc=now;traffic_visual_input(e,now);
}
void ObservationController::Impl::traffic_visual_capture_assertion(std::int64_t now,bool force){
 if(!traffic_capture.status().active)return;const auto& v=traffic_visual.view();
 const std::array<std::uint64_t,18> key{
  std::uint64_t(v.following)|(std::uint64_t(v.communication_healthy)<<1)|(std::uint64_t(v.requested)<<2)|(std::uint64_t(v.pending)<<3)|(std::uint64_t(v.owned)<<4)|(std::uint64_t(v.report_current)<<5)|(std::uint64_t(v.report_retained)<<6),
  std::uint64_t(v.phase)|(std::uint64_t(v.reason)<<32),v.selected.pid,v.selected.creation_filetime,v.process.pid,v.process.creation_filetime,
  v.plugin.instance.high,v.plugin.instance.low,v.plugin.capabilities,v.connection,v.lease,v.boundaries,v.overflows,v.error,
  std::uint64_t(v.policy.mode)|(std::uint64_t(v.policy.delay_ms)<<32),v.capture_generation,v.report.resource_generation,v.report.scenery_generation};
 if(force||!traffic_visual_assertion_qpc||key!=traffic_visual_assertion_key||now-traffic_visual_assertion_qpc>=qpc_frequency()/4){
  traffic_capture.visual_assertion(v,now);traffic_visual_assertion_qpc=now;traffic_visual_assertion_key=key;
 }
}
void ObservationController::Impl::drain_traffic_visual(std::int64_t now){
 if(view.bridge==State::connected&&view.bridge_peer_process_id&&traffic_visual_attachment!=view.bridge_attachments){
  TrafficVisualEvent e;e.kind=TrafficVisualEventKind::selection;e.process=terrain_process_identity(view.bridge_peer_process_id);e.received_qpc=now;traffic_visual_input(e,now);traffic_visual_service.associate(e.process);traffic_visual_attachment=view.bridge_attachments;
 }
 if(traffic_visual_intent_overflows_seen!=traffic_visual_intent_overflows){traffic_visual_intent_overflows_seen=traffic_visual_intent_overflows;traffic_capture.incomplete(ERROR_BUFFER_OVERFLOW,now);traffic_visual_service.fault(ERROR_BUFFER_OVERFLOW);TrafficVisualEvent e;e.kind=TrafficVisualEventKind::following;e.received_qpc=now;e.policy=traffic_visual.view().policy;traffic_visual_input(e,now);}
 for(unsigned budget=0;budget<32&&traffic_visual_intent_size;++budget){const auto e=traffic_visual_intents[traffic_visual_intent_head];traffic_visual_intent_head=(traffic_visual_intent_head+1)%traffic_visual_intents.size();--traffic_visual_intent_size;traffic_visual_input(e,qpc_now());}
 const auto& src=traffic.view();const std::array<std::uint64_t,12> key{src.producer.high,src.producer.low,src.connection_id,src.selection_epoch,src.lifecycle,src.sequence,static_cast<std::uint64_t>(src.received_qpc),static_cast<std::uint64_t>(src.admitted_qpc),src.communication_healthy,src.source_available,src.retained,static_cast<unsigned>(src.reason)};
 if(key!=traffic_visual_source_key){TrafficVisualEvent e;e.kind=TrafficVisualEventKind::source;e.received_qpc=qpc_now();e.source=visual_source(src);traffic_visual_input(e,e.received_qpc);traffic_visual_source_key=key;}
 TrafficVisualEvent e;for(unsigned budget=0;budget<128&&traffic_visual_service.pop(e);++budget)traffic_visual_input(e,qpc_now());
 now=qpc_now();traffic_visual_capture_intent(traffic_capture.status().active,now);now=qpc_now();traffic_visual.tick(now);
 // Replay's next(frame,time) repeats this tick at the same dispatch clock,
 // preserving renewal/Capture queue causality. Full views are status evidence:
 // record them at4Hz, on meaningful transitions, and at the final cutoff.
 traffic_visual_capture_assertion(now);
 visual_wire::Frame frame;for(unsigned budget=0;budget<20&&traffic_visual.next(frame,now);++budget){traffic_capture.visual_dispatch(frame,now);if(!traffic_visual_service.send(frame)){traffic_capture.incomplete(ERROR_WRITE_FAULT,now);traffic_visual_service.fault(ERROR_WRITE_FAULT);break;}}
 const auto dropped=traffic_visual_service.dropped();if(dropped!=traffic_visual_dropped_seen){traffic_visual_dropped_seen=dropped;traffic_capture.incomplete(ERROR_BUFFER_OVERFLOW,now);}
 view.traffic_visual=traffic_visual.view();
}
void ObservationController::Impl::associate_traffic(ProcessIdentity process,std::int64_t now){
 std::lock_guard<std::mutex> lock(mutex);TrafficEvent e;e.kind=TrafficEventKind::selection;e.process=process;e.received_qpc=now;
 const auto admission=traffic.event(e,now);traffic_capture.record(e,now,admission);traffic_service.associate(process);view.traffic=traffic.view();
}
void ObservationController::Impl::begin_traffic(std::int64_t now){std::lock_guard<std::mutex> lock(mutex);traffic.tick(now);traffic_visual.tick(now);traffic_capture.begin(now,traffic.checkpoint(),traffic_visual.checkpoint());traffic_visual_assertion_qpc=0;traffic_visual_assertion_key={};traffic_capture_revision=traffic.view().revision;traffic_visual_capture_intent(true,now);}
void ObservationController::Impl::finish_traffic_capture(std::int64_t now){std::lock_guard<std::mutex> lock(mutex);traffic.tick(now);traffic_visual.tick(now);traffic_capture.assertion(traffic.view(),now);traffic_visual_capture_assertion(now,true);traffic_capture.finish(now);traffic_visual_capture_intent(false,now);}
void ObservationController::Impl::end_traffic(std::int64_t now){traffic_service.request_stop();std::unique_lock<std::mutex> lock(mutex);TrafficEvent e;e.process=traffic.view().process;e.connection_id=traffic.view().connection_id;e.selection_epoch=traffic.view().selection_epoch;e.received_qpc=now;
 const auto admission=traffic.event(e,now);traffic_capture.record(e,now,admission);traffic.tick(now);traffic_visual.tick(now);view.traffic=traffic.view();traffic_capture.assertion(view.traffic,now);traffic_visual_capture_assertion(now,true);traffic_capture.finish(now);
 TrafficVisualEvent release;release.kind=TrafficVisualEventKind::following;release.policy=traffic_visual.view().policy;release.received_qpc=now;traffic_visual.event(release,now);traffic_visual_wanted=false;
 std::uint64_t release_request{};visual_wire::Frame frame;for(unsigned budget=0;budget<20&&traffic_visual.next(frame,now);++budget){visual_wire::Header h;visual_wire::Claim c;if(visual_wire::read_header(frame.bytes.data(),frame.size,h)&&h.kind==visual_wire::Kind::claim&&visual_wire::decode(frame.bytes.data()+16,h.payload_size,c)&&c.action==visual_wire::Action::release)release_request=c.request;traffic_visual_service.send(frame);}
 const auto deadline=GetTickCount64()+250;lock.unlock();bool completed=!release_request;
 while(!completed&&GetTickCount64()<deadline&&!traffic_visual_service.done()){
  TrafficVisualEvent sent;while(traffic_visual_service.pop(sent)){if(sent.kind==TrafficVisualEventKind::lost){completed=true;break;}visual_wire::Header h;visual_wire::Claim c;if(sent.kind==TrafficVisualEventKind::sent&&visual_wire::read_header(sent.frame.bytes.data(),sent.frame.size,h)&&h.kind==visual_wire::Kind::claim&&visual_wire::decode(sent.frame.bytes.data()+16,h.payload_size,c)&&c.request==release_request){completed=true;break;}}
  if(!completed)WaitForSingleObject(reinterpret_cast<HANDLE>(traffic_visual_service.ready_event()),5);
 }
 lock.lock();
 traffic_visual_service.request_stop();view.traffic_visual=traffic_visual.view();
}
void ObservationController::Impl::drain_traffic(){
 std::unique_lock<std::mutex> lock(mutex);TrafficEvent e;
 for(unsigned budget=0;budget<64&&traffic_service.pop(e);++budget){const auto now=qpc_now();const auto admission=traffic.event(e,now);traffic_capture.record(e,now,admission);if(admission==TrafficAdmission::fault)traffic_service.fault(ERROR_INVALID_DATA);}
 const auto now=qpc_now();traffic.tick(now);const auto state=traffic.view();
 const auto dropped=traffic_service.dropped();if(dropped!=traffic_dropped_seen){traffic_dropped_seen=dropped;traffic_capture.incomplete(ERROR_BUFFER_OVERFLOW,now);}
 if(traffic_capture.status().active&&state.revision!=traffic_capture_revision){traffic_capture.assertion(state,now);traffic_capture_revision=state.revision;}
 if(traffic_capture.status().active&&!capture.status().active){traffic_visual.tick(now);traffic_capture.assertion(state,now);traffic_visual_capture_assertion(now,true);traffic_capture.finish(now);}
 drain_traffic_visual(qpc_now());
 if(now-traffic_publish_qpc>=qpc_frequency()/4){view.traffic=state;traffic_publish_qpc=now;}
 // One fixed latest transition key; raw sequence/geometry never enters journals.
 const std::array<std::uint64_t,9> key{std::uint64_t(state.communication_healthy)|(std::uint64_t(state.source_available)<<1)|(std::uint64_t(state.retained)<<2),
  static_cast<unsigned>(state.reason),state.selected.pid,state.selected.creation_filetime,state.producer.high,state.producer.low,state.connection_id,state.selection_epoch,state.lifecycle};
 const bool journal=key!=traffic_journal_key&&now-traffic_journal_qpc>=qpc_frequency();if(journal){traffic_journal_key=key;traffic_journal_qpc=now;}lock.unlock();
 if(journal){io_event(psx::diagnostics::Code::metrics,psx::diagnostics::Component::application,psx::diagnostics::Severity::info,psx::diagnostics::ErrorDomain::none,0,
  "Traffic source state only: healthy/current/retained,reason,active,poseValid,accepted,rejected",{key[0],key[1],state.active_slots,state.pose_valid_slots,state.accepted,state.obsolete+state.faults});
  io_event(psx::diagnostics::Code::metrics,psx::diagnostics::Component::application,psx::diagnostics::Severity::info,psx::diagnostics::ErrorDomain::none,0,
  "Traffic scope: producerHigh,producerLow,connection,selection,lifecycle,ingressDrops",{state.producer.high,state.producer.low,state.connection_id,state.selection_epoch,state.lifecycle,dropped});}
}
}
