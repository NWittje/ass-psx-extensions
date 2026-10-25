#include "observation_internal.hpp"
namespace coordinator {
namespace w=aircraft_wire;
void ObservationController::Impl::aircraft_public(const LineResult& outcome,std::string_view line,std::int64_t received){
 std::lock_guard<std::mutex> lock(mutex);aircraft.public_session(model.latest().session(),received);
 ++aircraft_public_ordinal;
 if(outcome.selected&&!outcome.malformed){const auto i=static_cast<std::size_t>(outcome.source);if(i<psx::bridge::source_slots&&model.latest().available()[i])aircraft.public_observation(model.latest().records()[i].observation);}
 aircraft.public_stabilizer(line,received,aircraft_public_ordinal);
}
void ObservationController::Impl::drain_aircraft(){
 std::lock_guard<std::mutex> lock(mutex);auto now=aircraft_drain_clock();aircraft.public_session(model.latest().session(),now);
 if(view.bridge==State::connected&&view.bridge_peer_process_id&&aircraft_attachment!=view.bridge_attachments){aircraft_service.plugin(terrain_process_identity(view.bridge_peer_process_id));aircraft_attachment=view.bridge_attachments;}
 else if(view.bridge!=State::connected&&aircraft_attachment){aircraft_service.plugin({});aircraft_attachment=0;}
 bool reported_failure=false;AircraftEvent e;for(std::size_t n=0;n<32&&aircraft_service.pop(e);n++){
 const bool admitted=e.role==w::Role::exporter?aircraft.view().exporter_connected:aircraft.view().plugin_connected;
 if(e.kind==AircraftEventKind::frame){if(e.received_qpc>=aircraft_capture_start)aircraft_capture.record(1,e.role,e.frame.bytes.data(),e.frame.size,e.received_qpc);w::Header h;if(w::read_header(e.frame.bytes.data(),w::header_size,h)&&h.kind==w::Kind::hello)aircraft_capture_pending[e.role==w::Role::exporter?1:0]=true;}
 const auto admission=aircraft.admit(e);
 // An absent optional endpoint has no admitted trace to lose. Only retirement
 // of the current qualified connection makes a lost event incomplete evidence.
 const bool connected=e.role==w::Role::exporter?aircraft.view().exporter_connected:aircraft.view().plugin_connected;
 if(e.kind==AircraftEventKind::lost&&admitted&&!connected){const auto error=e.error?e.error:ERROR_BROKEN_PIPE;aircraft_capture.incomplete(error,now);
  if(diagnostics&&!reported_failure){diagnostics->producer(1,psx::diagnostics::Component::application).emit(psx::diagnostics::Code::disconnected,psx::diagnostics::Severity::warning,diagnostic_context(model.latest().session()),psx::diagnostics::ErrorDomain::win32,error,"Aircraft admitted peer lost: role,generation,receipt,controls,acks,overflows",{static_cast<std::uint64_t>(e.role),e.connection_generation,static_cast<std::uint64_t>(e.received_qpc),aircraft.view().controls,aircraft.view().acks,aircraft.view().overflows});reported_failure=true;}}
 if(admission==AircraftAdmission::fault){aircraft_capture.incomplete(ERROR_INVALID_DATA,now);aircraft_service.fault(e.role,ERROR_INVALID_DATA);
  if(diagnostics&&admitted&&!reported_failure){w::Header h;w::read_header(e.frame.bytes.data(),w::header_size,h);diagnostics->producer(1,psx::diagnostics::Component::application).emit(psx::diagnostics::Code::frame_rejected,psx::diagnostics::Severity::warning,diagnostic_context(model.latest().session()),psx::diagnostics::ErrorDomain::protocol,ERROR_INVALID_DATA,"Aircraft frame fault: role,generation,receipt,kind,size,sequence",{static_cast<std::uint64_t>(e.role),e.connection_generation,static_cast<std::uint64_t>(e.received_qpc),static_cast<std::uint64_t>(h.kind),e.frame.size,h.sequence});reported_failure=true;}}
 }
 // Service ingress can arrive after the loop-entry clock. Its original receipt
 // must not look future-dated and revoke an otherwise healthy group lease.
 now=aircraft_drain_clock();aircraft.tick(now);
 if(aircraft.view().overflows!=aircraft_overflows_seen){aircraft_overflows_seen=aircraft.view().overflows;aircraft_service.fault(w::Role::plugin,ERROR_BUFFER_OVERFLOW);aircraft_capture.incomplete(ERROR_BUFFER_OVERFLOW,now);}
 const auto dropped=aircraft_service.dropped();if(dropped!=aircraft_dropped_seen){aircraft_dropped_seen=dropped;aircraft_capture.incomplete(ERROR_BUFFER_OVERFLOW,now);}
 const bool capturing=aircraft_capture.status().active;
 if(capturing!=aircraft_capture_enabled){aircraft_capture_enabled=capturing;++aircraft_capture_generation;aircraft_capture_pending={true,true};if(capturing){for(std::size_t lane=0;lane<2;lane++)if(lane?aircraft.view().exporter_connected:aircraft.view().plugin_connected){AircraftDispatch seed;seed.kind=w::Kind::hello;seed.hello=lane?aircraft.view().exporter:aircraft.view().plugin;aircraft_capture.dispatch(lane?w::Role::exporter:w::Role::plugin,seed,now,5);}for(const auto& g:aircraft.view().groups)if(g.available){AircraftDispatch seed;seed.sample=g.sample;aircraft_capture.dispatch(w::Role::plugin,seed,now,5);}}}
 for(std::size_t lane=0;lane<2;lane++)if(aircraft_capture_pending[lane]&&(lane?aircraft.view().exporter_connected:aircraft.view().plugin_connected)){AircraftDispatch cap;cap.kind=w::Kind::capture;cap.capture.owner=aircraft.view().owner;cap.capture.generation=aircraft_capture_generation;cap.capture.enabled=capturing?1u:0u;const auto role=lane?w::Role::exporter:w::Role::plugin;aircraft_capture.dispatch(role,cap,now);if(aircraft_service.send(role,cap))aircraft_capture_pending[lane]=false;}
 AircraftDispatch d;for(std::size_t n=0;n<16&&aircraft.next(d);n++){aircraft_capture.dispatch(w::Role::plugin,d,now);if(!aircraft_service.send(w::Role::plugin,d)){aircraft_service.fault(w::Role::plugin,ERROR_BUFFER_OVERFLOW);break;}}
 view.aircraft=aircraft.view();
}
}
