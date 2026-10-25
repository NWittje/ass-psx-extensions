#include "observation_internal.hpp"
namespace coordinator {
void ObservationController::Impl::drain_readiness(){
 std::unique_lock<std::mutex> lock(mutex);auto now=qpc_now();
 if(view.bridge==State::connected&&view.bridge_peer_process_id&&readiness_attachment!=view.bridge_attachments){
  const auto peer=terrain_process_identity(view.bridge_peer_process_id);readiness_service.plugin(peer);ReadinessEvent e;e.kind=ReadinessEventKind::selection;e.process=peer;e.received_qpc=now;
  const auto a=readiness.event(e);readiness_capture.record(e,now,a);readiness_attachment=view.bridge_attachments;
 }
 // Selected status observations survive unrelated Main/bridge/pose failures.
 ReadinessEvent e;for(std::size_t n=0;n<64&&readiness_service.pop(e);++n){now=qpc_now();const auto a=readiness.event(e);readiness_capture.record(e,now,a);if(a==ReadinessAdmission::fault)readiness_service.fault(e.role,ERROR_INVALID_DATA);}
 now=qpc_now();readiness.tick(now);const auto state=readiness.view();
 const bool changed=view.readiness.revision!=state.revision;
 if(changed||now-readiness_assertion_qpc>=qpc_frequency()/4){readiness_capture.assertion(state,now);readiness_assertion_qpc=now;}
 const auto dropped=readiness_service.dropped();if(dropped!=readiness_dropped_seen){readiness_dropped_seen=dropped;readiness_capture.incomplete(ERROR_BUFFER_OVERFLOW,now);}
 if(readiness_capture.status().active&&!capture.status().active)readiness_capture.finish(now);
 if(changed||now-readiness_publish_qpc>=qpc_frequency()/4){view.readiness=state;readiness_publish_qpc=now;}
 const bool journal=state.revision!=readiness_journal_revision;readiness_journal_revision=state.revision;lock.unlock();
 if(journal){
  io_event(psx::diagnostics::Code::metrics,psx::diagnostics::Component::application,psx::diagnostics::Severity::info,psx::diagnostics::ErrorDomain::none,0,"Readiness report only: XPlaneState,PsxState,pluginReason,PSXReason,healthyBits,revision",
   {static_cast<unsigned>(state.xplane),static_cast<unsigned>(state.psx),static_cast<unsigned>(state.plugin.reason),static_cast<unsigned>(state.exporter.reason),std::uint64_t(state.plugin.communication_healthy)|(std::uint64_t(state.exporter.communication_healthy)<<1),state.revision});
 }
}
void ObservationController::Impl::begin_readiness(std::int64_t now){readiness.tick(now);readiness_capture.begin(now,readiness.checkpoint());readiness_assertion_qpc=now;}
void ObservationController::Impl::end_readiness(std::int64_t now){readiness_service.request_stop();std::lock_guard<std::mutex> lock(mutex);for(const auto role:{status_wire::Role::plugin,status_wire::Role::exporter}){ReadinessEvent e;e.role=role;e.received_qpc=now;const auto a=readiness.event(e);readiness_capture.record(e,now,a);}readiness.tick(now);view.readiness=readiness.view();readiness_capture.assertion(view.readiness,now);readiness_capture.finish(now);}
}
