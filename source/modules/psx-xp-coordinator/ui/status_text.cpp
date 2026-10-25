#include "status_text.hpp"
#include "coordinator/observation_session.hpp"
#include "coordinator/environment_time.hpp"
#include <ctime>
#include <iomanip>
namespace coordinator::ui {
std::string traffic_text(const ObservationSnapshot& o){
 const auto& v=o.traffic;std::ostringstream t;const auto frequency=static_cast<std::uint64_t>(qpc_frequency());
 t<<"Connect observes native PSX traffic. General Follow also requests outside traffic; Stop hides it and keeps observation.\r\nTraffic source: "<<traffic_reason_name(v.reason)<<" / communication "<<(v.communication_healthy?"healthy":"unavailable")<<"\r\nSource age: ";
 if(v.source_age_qpc>=0)t<<v.source_age_qpc*1000.0/frequency<<" ms";else t<<"unavailable";
 t<<"\r\nSelected PID/birth: "<<v.selected.pid<<'/'<<v.selected.creation_filetime<<"  authenticated PID/birth: "<<v.process.pid<<'/'<<v.process.creation_filetime<<"\r\nProducer: "<<v.producer.high<<'/'<<v.producer.low<<"  connection/selection: "<<v.connection_id<<'/'<<v.selection_epoch<<"  capabilities: "<<v.capabilities;
 if(v.retained){const auto& h=v.source.header;t<<"\r\n"<<(v.source_available?"Current":"Retained (not current)")<<" native-active / pose-valid slots: "<<v.active_slots<<" / "<<v.pose_valid_slots<<" of 7\r\nNative sequence/lifecycle/frame: "<<v.sequence<<'/'<<v.lifecycle<<'/'<<h.frame<<"  mode: "<<h.mode<<"  full pause / motion freeze / loading: "<<bool(h.flags&2u)<<'/'<<bool(h.flags&4u)<<'/'<<bool(h.flags&8u)<<"\r\nOriginal observation/copy QPC: "<<h.observed<<'/'<<h.finished<<"  receipt/admission QPC: "<<v.received_qpc<<'/'<<v.admitted_qpc<<"\r\nAnchor revision/writer version: "<<h.anchor<<'/'<<h.writer_version<<"  slot generation changes: "<<v.generation_changes;
  if(h.metadata_valid)t<<"\r\nNative delta/rate (raw): "<<h.native_delta<<'/'<<h.rate;
 }else t<<"\r\nNative slot evidence unavailable.";
 if(v.status_frame.size){traffic_wire::Status s;if(traffic_wire::decode(v.status_frame.bytes.data()+16,v.status_frame.size-16,s))t<<"\r\nRetained worker statistics (independent of native source age): generated/receipt QPC "<<s.generated_qpc<<'/'<<v.status_received_qpc<<"\r\nDrained/sent/sequence skips: "<<s.drained<<'/'<<s.sent<<'/'<<s.sequence_skips<<"  aggregate/max worker work us: "<<s.work_ticks*1000000.0/frequency<<'/'<<s.max_work_ticks*1000000.0/frequency;}
 t<<"\r\nAdmission accepted/obsolete/fault: "<<v.accepted<<'/'<<v.obsolete<<'/'<<v.faults<<"  error: "<<v.error<<"\r\nTraffic Capture: "<<(o.traffic_evidence.active?"recording":o.traffic_evidence.incomplete?"incomplete":o.traffic_evidence.complete?"complete":"off")<<"  records: "<<o.traffic_evidence.records<<"  error: "<<o.traffic_evidence.error;
 const auto& visual=o.traffic_visual;const auto count=[](std::uint32_t mask){unsigned n{};for(unsigned i=0;i<7;++i)n+=(mask>>i)&1u;return n;};
 t<<"\r\nOutside traffic Follow: "<<(visual.following?"selected":"off")<<" / plugin communication: "<<(visual.communication_healthy?"healthy":"unavailable")<<" / "<<traffic_visual_reason_name(visual.reason)<<"\r\nOwnership: "<<(visual.owned?"owned":visual.pending||visual.following?"pending demand":"released/observing")<<"  model: "<<((visual.plugin.capabilities&visual_wire::loaded_model)?"loaded":(visual.plugin.capabilities&visual_wire::qualified_model)?"qualified, loading pending":"unavailable")<<"\r\nCurrent drawable / SDK-submitted / held / hidden: "<<count(visual.drawable_mask)<<" / "<<count(visual.submitted_mask)<<" / "<<count(visual.held_mask)<<" / "<<count(visual.hidden_mask)<<" of 7\r\nPresentation source age: ";
 if(visual.presentation_age_qpc>=0)t<<visual.presentation_age_qpc*1000.0/frequency<<" ms";else t<<"unavailable";
 t<<"\r\nPolicy: "<<(visual.policy.mode==psx::motion::ReconstructionMode::direct?"Direct":"Interpolated");if(visual.policy.mode!=psx::motion::ReconstructionMode::direct)t<<" / "<<visual.policy.delay_ms<<" ms";
 if(visual.report_retained)t<<"\r\n"<<(visual.report_current?"Current":"Retained (not current)")<<" SDK report: sequence "<<visual.report.sdk_sequence<<" / connection-lease "<<visual.report.connection<<'-'<<visual.report.lease<<" / resource-scenery-origin "<<visual.report.resource_generation<<'-'<<visual.report.scenery_generation<<'-'<<visual.report.origin_generation;
 t<<"\r\nFixed traffic model: Boeing 737-800, gear retracted, doors closed, neutral unobserved controls, navigation lights on and strobes flashing.\r\nOriginal source is stale at 1 s; heartbeat does not renew it. Transport expires at 5 s.\r\nSDK submission is not pixel readback; loaded visibility and complete overhead acceptance remain outstanding.";return t.str();
}
std::string readiness_text(const ObservationSnapshot& o){
 std::ostringstream text;const auto lane=[&](const char* name,const char* state,const ReadinessDetail& d,bool plugin){
  text<<name<<": "<<state<<" / "<<readiness_reason_name(d.reason)<<"\r\nCommunication: "<<(d.communication_healthy?"healthy":"unavailable")<<"  source age: ";
  if(d.observation_age_qpc>=0)text<<d.observation_age_qpc*1000.0/qpc_frequency()<<" ms";else text<<"unavailable";
  text<<"\r\nProducer PID/birth: "<<d.process.pid<<'/'<<d.process.creation_filetime<<"  instance: "<<d.producer.high<<'/'<<d.producer.low<<"  connection: "<<d.connection_id<<"  lifecycle: "<<d.lifecycle;
  text<<"\r\nLast known: ";if(d.last_known_available)text<<(plugin?xplane_state_name(static_cast<XPlaneState>(d.last_known_state)):psx_state_name(static_cast<PsxState>(d.last_known_state)))<<"  original source QPC: "<<d.last_known_qpc<<"  phase start QPC: "<<d.last_known_phase_qpc;else text<<"unavailable";
  if(d.observation_available){const auto& s=d.source;text<<"\r\nSource sequence: "<<s.sequence<<"  SDK/native QPC: "<<s.observed_qpc<<"  flags: "<<s.flags;
   if(plugin)text<<"\r\nEnable/aircraft/scenery/local-frame generations: "<<s.enable_generation<<'/'<<s.aircraft_generation<<'/'<<s.scenery_generation<<'/'<<s.local_frame_generation<<"\r\nPlacement command/attempt: "<<s.command_id<<'/'<<s.placement_attempt<<"  phase/returned/notification QPC: "<<s.phase_start_qpc<<'/'<<s.call_returned_qpc<<'/'<<s.notification_qpc<<"\r\nTarget source/selection/epoch/sequence: "<<s.target_source<<'/'<<s.selection_epoch<<'/'<<s.target_epoch<<'/'<<s.target_sequence;
  }text<<"\r\n\r\n";
 };
 lane("X-Plane",xplane_state_name(o.readiness.xplane),o.readiness.plugin,true);lane("PSX",psx_state_name(o.readiness.psx),o.readiness.exporter,false);
 text<<"Report only. PSX Crashed means simulated fatal state; pause/freeze can remain Running.\r\nSource stale after 1 s; missing status transport at 5 s. Loaded scenery appearance is not asserted.\r\nReadiness Capture: "<<(o.readiness_evidence.active?"recording":o.readiness_evidence.incomplete?"incomplete":o.readiness_evidence.complete?"complete":"off")<<" records="<<o.readiness_evidence.records<<" error="<<o.readiness_evidence.error;return text.str();
}
std::string environment_text(const ObservationSnapshot& s){
 constexpr const char* fields[]={"Clock rate","Latitude deg","Longitude deg","Aircraft MSL m","Weather datum MSL m","Upper coverage","Upper base MSL m","Upper top MSL m","Lower coverage","Lower base MSL m","Lower top MSL m","CB coverage","CB base MSL m","CB top MSL m","Wind from true deg","Wind m/s","OAT C","QNH Pa","Visibility m","Precipitation category","Pressure at aircraft Pa","Precipitation ratio","Configured temperature anchor MSL m","Pressure altitude m"};
 const auto& v=s.environment;std::ostringstream out;const auto frequency=static_cast<std::uint64_t>(coordinator::qpc_frequency());
 const auto time=environment_time_status(v,s.snapshot_qpc,frequency);
 const auto age=[&](std::int64_t t){if(t<=0||s.snapshot_qpc<t||!frequency)return std::string("unavailable");std::ostringstream value;value<<(s.snapshot_qpc-t)*1000.0/frequency;return value.str();};
 const auto calendar=[&](std::int64_t utc,bool available,bool full){std::ostringstream value;const auto c=environment_utc_calendar(utc);
  if(!available||!c.valid)return std::string("unavailable");value<<std::setfill('0');
  if(full)value<<std::setw(4)<<c.year<<'-'<<std::setw(2)<<c.month<<'-'<<std::setw(2)<<c.day<<' ';
  else value<<"day "<<c.day_index<<" (0-based), ";
  value<<std::setw(2)<<c.hour<<':'<<std::setw(2)<<c.minute<<':'<<std::setw(2)<<c.second<<'.'<<std::setw(3)<<c.millisecond;
  if(full)value<<" UTC";return value.str();};
 out<<"Environment follows general Align / Follow / Stop; Connect observes.\r\nEnvironment: "<<(v.following?"Follow selected":"Observe")<<"  exporter="<<v.exporter_connected<<" plugin="<<v.plugin_connected<<"\r\n"
  <<(time.source_current?"Current PSX UTC: ":"Retained PSX UTC (not current): ")<<calendar(v.source.utc_millis,time.utc_qualified,true)
  <<"\r\nSource age ms="<<age(v.source.sample_end_qpc)<<" seq="<<v.source.sequence<<" lifecycle="<<v.source.lifecycle<<" cloud configuration="<<v.source.configuration
  <<"\r\n"<<(time.source_current?"Qualified applied rate: ":"Retained applied rate (not current): ");
 if(time.rate_qualified)out<<v.source.values[0]<<"x";else out<<"unavailable";
 out<<"  Full PSX pause="<<time.paused<<"  Motion freeze="<<time.motion_frozen<<" (freeze alone does not stop time)\r\n"
  <<"Time owner="<<(v.groups[0].requested?v.owner:0)<<"  phase="<<environment_phase_name(time.phase)<<"  reason="<<environment_reason_name(time.reason)
  <<"\r\nCurrent time readback="<<time.readback_current<<"  readback age ms="<<age(v.groups[0].applied.readback_qpc)
  <<"\r\nDaylight / seasonal date mapping; X-Plane calendar-year fidelity unsupported.\r\n"
  <<"Current mask="<<v.source.valid_mask<<" configured diagnostic mask="<<v.source.configured_mask<<" configured UTC="
  <<calendar(v.source.configured_utc_millis,(v.source.flags&environment_wire::configured_utc_valid)!=0,true)<<"\r\n";
 constexpr const char* names[]={"Time","Weather"};
 out<<"Weather updates: initial Follow, manual PSX edits and loads only. Native cloud settling; no forced weather rebuild.\r\n"
    <<"Manual-edit source qualified="<<bool(v.source_change.qualified)<<" edit revision="<<v.source_change.revision
    <<" retained weather target="<<v.weather_target<<" target source seq="<<v.weather_target_sequence<<" captured age ms="<<age(v.weather_target_source_qpc)<<" (retained target, not a new observation)\r\n";
 for(std::size_t i=0;i<2;i++){const auto& g=v.groups[i];out<<"\r\n"<<names[i]<<": available / requested / pending / acknowledged ownership="<<g.available<<'/'<<g.requested<<'/'<<g.pending<<'/'<<g.owned<<" reason="<<environment_reason_name(g.reason)<<"\r\n";
  if(g.applied_retained||g.applied_available){const auto& a=g.applied;const bool current=i?g.applied_available&&g.owned&&v.following&&v.plugin_connected&&a.readback_qpc>0&&s.snapshot_qpc>=a.readback_qpc&&static_cast<std::uint64_t>(s.snapshot_qpc-a.readback_qpc)<frequency:time.readback_current;
   out<<(current?"Current qualified report":"Retained / unqualified report (not current)")<<" seq="<<a.source.sequence<<" accepted mask="<<a.accepted_mask<<" actual aircraft readback mask="<<a.actual_mask<<" SDK location mask="<<a.location_mask<<" approximation mask="<<a.approximation_mask<<" flags="<<a.flags<<" phase="<<environment_phase_name(a.phase)<<" reason="<<environment_reason_name(a.reason)<<"\r\nSource->application ms="<<(a.applied_qpc-a.source.sample_end_qpc)*1000.0/frequency<<" SDK getter duration ms="<<(a.location_end_qpc-a.location_begin_qpc)*1000.0/frequency<<" readback age ms="<<age(a.readback_qpc)<<" regeneration="<<a.regeneration_sequence<<" settling="<<bool(a.flags&environment_wire::clouds_settling)<<"\r\n";
   if(!i){const bool mapped=environment_time_report_qualified(a);out<<"Accepted X-Plane daylight / seasonal time: "<<calendar(a.accepted_utc_millis,mapped,false)
     <<"\r\nReadback X-Plane daylight / seasonal time: "<<calendar(a.actual_utc_millis,mapped,false)<<"\r\nQualified readback minus accepted error ms=";
    if(time.readback_current)out<<time.readback_error_millis;else out<<"unavailable";out<<"\r\n";
   }else {
    out<<"Cloud names Upper/Lower/CB apply to PSX source and accepted settings. Actual aircraft and SDK-location cloud columns retain independent native slots 0/1/2.\r\n";
    for(std::size_t n=1;n<environment_wire::field_capacity;n++)if((a.accepted_mask|a.actual_mask|a.location_mask)&(1u<<n)){out<<(n<std::size(fields)?fields[n]:"Unavailable field")<<" source=";if(a.source.valid_mask&(1u<<n))out<<a.source.values[n];else out<<"unavailable";out<<" accepted=";if(a.accepted_mask&(1u<<n))out<<a.accepted[n];else out<<"unavailable";out<<" actual=";if(a.actual_mask&(1u<<n))out<<a.actual[n];else out<<"unavailable";out<<" SDK location=";if(a.location_mask&(1u<<n))out<<a.location[n];else out<<"unavailable";out<<"\r\n";}
   }
  }else out<<"No current application/readback evidence.\r\n";}
 out<<"\r\nVisibility unlimited source="<<bool(v.source.flags&environment_wire::visibility_unlimited)<<" (destination unqualified)\r\nRejected="<<v.rejected<<" obsolete="<<v.obsolete<<" dispatched="<<v.dispatched<<" sidecar="<<(s.environment_evidence.active?"recording":s.environment_evidence.incomplete?"incomplete":s.environment_evidence.complete?"complete":"off")<<" records="<<s.environment_evidence.records<<" error="<<s.environment_evidence.error<<"\r\nTime advances only from fresh qualified clock rate; PSX pause freezes daylight without pausing X-Plane.\r\nLocal profiles/cloud opacity/calendar year may be approximate. Unlimited visibility is unqualified.\r\nLoaded appearance, cloud settling, regeneration frame cost and latency remain unmeasured.\r\n";return out.str();
}
std::string aircraft_text(const ObservationSnapshot& s){
    std::ostringstream text;const auto& v=s.aircraft;
    text<<"Aircraft state: "<<(v.following?"Follow":"Observe")<<"  plugin="<<v.plugin_connected<<" native source="<<v.exporter_connected<<"\r\nRejected inputs="<<v.rejected<<" including obsolete replies="<<v.obsolete<<"\r\n";
    constexpr const char* names[]={"Controls / stabilizer","Spoilers","LE / TE devices","Gear / doors","Reversers","Exterior lights"};
    for(std::size_t i=0;i<v.groups.size();i++){const auto& g=v.groups[i];text<<"\r\n"<<names[i]<<": ";if(!g.available)text<<"unavailable";else text<<(g.sample.source==aircraft_wire::Source::native?"native":"public receipts")<<" seq="<<g.sample.sequence<<" valid mask="<<g.sample.valid_mask<<" approximation mask="<<g.sample.approximation_mask;
        text<<"\r\nrequested / pending / acknowledged ownership="<<g.requested<<'/'<<g.pending<<'/'<<g.owned<<" reason="<<static_cast<unsigned>(g.reason);
        if(g.applied_available)text<<"\r\nApplied seq="<<g.applied.sequence<<" phase="<<static_cast<unsigned>(g.applied.phase)<<" readback mask="<<g.applied.readback_mask<<" approximation mask="<<g.applied.approximation_mask;
        else text<<"\r\nNo current applied-state evidence.";
        text<<"\r\n";
    }
    text<<"\r\nSidecar="<<(s.aircraft_evidence.active?"recording":s.aircraft_evidence.incomplete?"incomplete":s.aircraft_evidence.complete?"complete":"off")<<" records="<<s.aircraft_evidence.records<<" error="<<s.aircraft_evidence.error<<"\r\nPublic fallback keeps individual receipts; unchanged ECON state is held. Recovery after loss needs new records.\r\nAircraft synchronization FAILED in loaded use and is deferred; retained code is not loaded acceptance.\r\n";
    return text.str();
}
std::string terrain_text(const ObservationSnapshot& s) {
    const auto& v=s.terrain;std::ostringstream text;const auto age=[&](std::int64_t time){return s.snapshot_qpc>=time?(s.snapshot_qpc-time)*1000.0/coordinator::qpc_frequency():0;};
    text<<"TERRAIN SUPPORT - X-Plane surface / native PSX physics\r\nConnect observes. Follow acquires independent terrain input on both peers.\r\nStop, stale input or lease expiry holds the last accepted PSX plane; dynamics continue.\r\nOne reference plane with longitudinal and cross-slope; no new gear-force solver.\r\n\r\n"
        <<"Following="<<v.following<<" terrain epoch="<<v.terrain_epoch<<" owner="<<v.owner<<" reason="<<coordinator::terrain_reason_name(v.reason)<<"\r\n";
    for(const auto& item:{std::pair<const char*,const TerrainPeerView*>{"X-Plane provider",&v.plugin},{"PSX native support",&v.exporter}}){const auto& p=*item.second;
        text<<item.first<<": connected="<<p.connected<<" owned="<<p.owned<<" pending Ack="<<p.pending<<" PID="<<p.hello.pid<<" error="<<p.error<<"\r\n";}
    if(v.physical_available)text<<"Physical source: fresh="<<v.physical_fresh<<" epoch="<<v.reference.pose_epoch<<" frame="<<v.reference.frame_sequence<<" sequence="<<v.reference.sequence<<"\r\nNative realLat/lon="<<v.reference.latitude_deg<<", "<<v.reference.longitude_deg<<" altitude MSL m="<<v.reference.altitude_msl_m<<"\r\nHeading/pitch/bank-left rad="<<v.reference.heading_rad<<" / "<<v.reference.pitch_rad<<" / "<<v.reference.bank_left_rad<<"\r\nSample / receipt age ms="<<age(v.reference.sample_qpc)<<" / "<<age(v.reference.received_qpc)<<"\r\n";
    else text<<"Physical source unavailable; visual eye pose is not substituted.\r\n";
    if(v.surface_available)text<<"Last admitted surface: query="<<v.surface.query.query_id<<" height MSL m="<<v.surface.height_msl_m<<" normal ENU="<<v.surface.normal_east<<", "<<v.surface.normal_north<<", "<<v.surface.normal_up<<" age ms="<<age(v.surface.query_end_qpc)<<"\r\nProvider aircraft/scenery/local generations="<<v.surface.aircraft_generation<<'/'<<v.surface.scenery_generation<<'/'<<v.surface.local_frame_generation<<"\r\n";
    else text<<"No admitted surface in the current terrain epoch.\r\n";
    if(v.consumed_available)text<<"Actual consumed PSX support: "<<coordinator::terrain_phase_name(v.consumed.phase)<<" / "<<coordinator::terrain_reason_name(v.consumed.reason)<<" age ms="<<age(v.consumed.consumed_qpc)<<"\r\nHeight MSL m="<<v.consumed.effective_height_msl_m<<" pitch / bank-left rad="<<v.consumed.support_pitch_rad<<" / "<<v.consumed.support_bank_left_rad<<" query="<<v.consumed.query_id<<" applied="<<v.consumed.applied<<" rejected="<<v.consumed.rejected<<"\r\nActual strut ft="<<v.consumed.strut_ft<<" nose oscillation/compression rad="<<v.consumed.nose_osc_rad<<'/'<<v.consumed.nose_compress_rad<<" contact flags="<<v.consumed.contact_flags<<"\r\n";
    else text<<"No actual consumed-support report yet.\r\n";
    if(v.consumed_available){
        const auto latency=[&](const char* label,std::int64_t sample){text<<label;if(sample>0&&v.consumed.consumed_qpc>=sample)text<<(v.consumed.consumed_qpc-sample)*1000.0/coordinator::qpc_frequency();else text<<"unavailable";text<<"\r\n";};
        latency("Physical source sample -> consumption ms=",v.consumed.source_sample_qpc);
        latency("Terrain probe completion -> consumption ms=",v.consumed.surface_qpc);
    }
    constexpr const char* assemblies[]={"Nose","Body right","Body left","Wing right","Wing left"};
    for(std::size_t i=0;i<5;++i){text<<assemblies[i]<<" clearance: ";if(v.diagnostics_available&&(v.diagnostics.valid_mask&(1u<<i)))text<<v.diagnostics.assemblies[i].clearance_m<<" m (geometry diagnostic)";else text<<"unavailable - "<<coordinator::terrain_reason_name(v.diagnostics_available?v.diagnostics.assemblies[i].reason:psx::terrain::Reason::unsupported);text<<"\r\n";}
    text<<"\r\nSelected queries / admitted surfaces / rejected="<<v.queries<<'/'<<v.surfaces<<'/'<<v.rejected<<" controls / Acks="<<v.controls<<'/'<<v.acks<<" overflows="<<v.overflows<<"\r\nTerrain sidecar="<<(s.terrain_evidence.active?"recording":s.terrain_evidence.incomplete?"incomplete":s.terrain_evidence.complete?"complete":"off")<<" bytes="<<s.terrain_evidence.bytes<<" error="<<s.terrain_evidence.error<<"\r\nSettled targets 0.20 m / 0.25 degrees remain unmeasured with loaded simulators.\r\n";
    return text.str();
}
std::string describe(const coordinator::EndpointStatus& e, const char* name) {
    std::ostringstream s;
    s << name << ": " << coordinator::state_name(e.state) << "\r\n  Endpoint: " << (e.endpoint[0] ? e.endpoint.data() : "not configured")
      << "\r\n  Epoch " << e.epoch << " | RX " << e.bytes_in << " B | TX " << e.bytes_out
      << " B | Frames " << e.frames << " | Errors " << e.errors << " | Incomplete " << e.truncated << " | Last error " << e.last_error << "\r\n";
    return s.str();
}
void distribution_text(std::ostringstream& text,const char* label,const psx::bridge::Distribution& d) {
    const auto frequency=static_cast<double>(coordinator::qpc_frequency());
    text<<label<<" n="<<d.count<<" p50/p95/p99/max ms "<<d.p50*1000.0/frequency<<"/"<<d.p95*1000.0/frequency<<"/"<<d.p99*1000.0/frequency<<"/"<<d.maximum*1000.0/frequency<<"\r\n";
}
const char* freshness_name(psx::diagnostics::Freshness value){switch(value){case psx::diagnostics::Freshness::current:return "current";case psx::diagnostics::Freshness::delayed:return "delayed (informational)";default:return "unavailable";}}
std::string follow_availability(const coordinator::ObservationSnapshot& s) {
    if(s.bridge==coordinator::State::error)return "Local plugin bridge failed (Windows error "+std::to_string(s.bridge_last_error)+"). Another coordinator or harness may own it; check Events.";
    if(!s.motion.connected)return "Waiting for local plugin connection. Connect starts the bridge.";
    if(!s.motion.identity_available)return "Waiting for plugin/aircraft identity.";
    if(s.motion.transport_fault)return "Control queue fault; reconnect after checking Events.";
    if(s.motion.outstanding)return "Waiting for completion of motion request #"+std::to_string(s.motion.last_request.operation_id)+". Stop remains available.";
    if(s.exporter.peer&&s.complete_window.count&&s.snapshot_qpc>=s.exporter.last_sample_end_qpc&&s.snapshot_qpc-s.exporter.last_sample_end_qpc<coordinator::qpc_frequency()/10&&(s.exporter.ready||s.exporter.selection.source==psx::motion::PoseSource::exporter))return "Exporter pose and plugin identity available; Auto Follow can be requested independently of Main.";
    if(!(s.latest.session().flags&psx::bridge::synchronized))return "Waiting for synchronized PSX Main state.";
    if(!s.latest.available()[static_cast<std::size_t>(psx::aircraft::SourceRecord::boost)])return "Waiting for current Boost position.";
    if(!s.latest.available()[static_cast<std::size_t>(psx::aircraft::SourceRecord::qs121)])return "Waiting for current Main horizon pitch (Qs121) after the motion boundary.";
    if(s.motion.status_available && s.motion.status.reason!=psx::motion::Reason::none)return std::string("Sources available; plugin reports ")+coordinator::motion_reason_name(s.motion.status.reason)+". See Motion.";
    return "Sources and plugin identity available; Align and Follow PSX can be requested.";
}
std::string timing_text(const coordinator::ObservationSnapshot& s) {
    std::ostringstream text;distribution_text(text,"Decoder rolling <=512 samples",s.decode_duration);distribution_text(text,"Receive->callback rolling <=128",s.plugin.receive_to_callback);distribution_text(text,"Callback duration rolling <=128",s.plugin.callback_duration);distribution_text(text,"Frame interval rolling <=128",s.plugin.frame_interval);
    text<<"\r\nExact enable-lifetime maxima, load "<<s.plugin.plugin_load_id<<" / enable "<<s.plugin.enable_id<<"\r\n";
    const auto maximum=[&](const char* label,const psx::diagnostics::Maximum& m){text<<label<<" count="<<m.count<<" max="<<m.ticks*1000.0/coordinator::qpc_frequency()<<" ms at callback="<<m.callback_count<<" qpc="<<m.qpc<<" previous="<<m.previous_qpc<<"\r\n";};
    maximum("Receive->callback",s.plugin.receive_to_callback_max);maximum("Completed callback duration",s.plugin.callback_duration_max);maximum("Frame interval",s.plugin.frame_interval_max);
    text<<"\r\nHost QPC ticks: report received="<<s.plugin_received_qpc<<" generated="<<s.plugin.generated_qpc<<" callback="<<s.plugin.callback_qpc<<"\r\nDuration completed callback="<<s.plugin.duration_completed_callback<<"\r\nQuantiles describe rolling windows. These are not visual latency or whole-run percentiles.";return text.str();
}
void pause_text(std::ostringstream& text,const coordinator::ObservationSnapshot& s) {
    const auto source=coordinator::pause_source_state(s.latest);
    if(source.available) {
        text<<"PSX Main Qi129="<<unsigned(source.raw_value)<<" pause-all="<<source.paused_all<<" motion-freeze="<<source.motion_frozen
            <<" (ECON register; scheduler completion not confirmed)\r\n"
            <<"  Source session="<<source.stamp.session_epoch<<" config="<<source.stamp.configuration_epoch<<" sequence="<<source.stamp.sequence
            <<" received QPC="<<source.stamp.received_qpc<<" age="<<(s.snapshot_qpc-source.stamp.received_qpc)*1000.0/coordinator::qpc_frequency()<<" ms\r\n";
    } else text<<"PSX Main Qi129 unavailable; pause-all and motion-freeze unknown.\r\n";
    if(s.motion.status_available)text<<"X-Plane paused="<<bool(s.motion.status.flags&psx::motion::xp_paused)
        <<" (plugin report QPC="<<s.motion.status.generated_qpc<<" received="<<s.motion.status_received_qpc<<")\r\n";
    else text<<"X-Plane pause state unavailable; waiting for plugin motion report.\r\n";
}
const char* reconstruction_name(psx::motion::ReconstructionState state) {
    constexpr const char* names[]={"Unavailable","Direct","Bracketed interpolation","Holding oldest endpoint","Holding newest endpoint","No pitch at presentation time","Invalid timing"};
    const auto i=static_cast<unsigned>(state);return i<7?names[i]:"Unknown";
}
std::string audio_text(const coordinator::ObservationSnapshot& s) {
    namespace a=psx::audio;const auto& v=s.audio;std::ostringstream text;
    const auto age=[&](std::int64_t tick){return tick>0?(s.snapshot_qpc-tick)*1000.0/coordinator::qpc_frequency():0.0;};
    text<<"AIRCRAFT AUDIO - fixed pilot cockpit listening\r\nFollow starts motion and qualified sounds; Stop releases both. Mute retains PSX duplicate suppression.\r\nActual states only; engine/system values are not interpolated. Unavailable groups stay native PSX.\r\n\r\n"
        <<"Following="<<v.following<<" muted="<<v.effective_muted()<<" manual mute="<<v.muted<<" PSX Pause="<<(v.pause_available?(v.psx_paused?"on":"off"):"unknown")<<" exporter="<<v.peer<<" audio epoch="<<v.state.audio_epoch<<" reason="<<coordinator::audio_reason_name(v.reason)
        <<"\r\nAvailable groups="<<v.state.available_groups<<" suppression capability="<<v.state.suppression_capabilities<<" acknowledged="<<v.ownership.accepted_groups
        <<" lease operation="<<v.ownership.lease.operation<<" expiry QPC="<<v.ownership.lease.expires_qpc
        <<"\r\nNative enqueued/sent/dropped="<<v.native_enqueued<<'/'<<v.native_sent<<'/'<<v.native_dropped
        <<"\r\nSnapshots="<<v.received<<" rejected="<<v.rejected<<" transitions="<<v.transitions<<" queue resets="<<v.overflows<<" late acknowledgements="<<v.late_acks<<"\r\n";
    if(v.status_available)text<<"Plugin enable="<<v.plugin.plugin_generation<<" aircraft="<<v.plugin.aircraft_generation<<" ready/active/held/missing="<<v.plugin.ready_groups<<'/'<<v.plugin.active_groups<<'/'<<v.plugin.held_groups<<'/'<<v.plugin.missing_groups
        <<" phase="<<unsigned(v.plugin.phase)<<" reason="<<coordinator::audio_reason_name(v.plugin.reason)<<"\r\nReport receipt age="<<age(v.status_received)<<" ms; generated age="<<age(v.plugin.generated_qpc)
        <<" ms; applied sample age="<<age(v.plugin.applied_sample_qpc)<<" ms; applied sequence="<<v.plugin.applied_sequence<<" parameter updates="<<v.plugin.parameter_updates<<" saturations="<<v.plugin.saturations<<" dropped transitions="<<v.plugin.dropped_transitions<<"\r\n";
    for(std::size_t i=0;i<a::group_count;++i){text<<"\r\n"<<coordinator::audio_group_name(static_cast<a::Group>(i))<<": ";if(!(v.state.available_groups&(1u<<i))){text<<"unavailable";continue;}const auto& g=v.state.groups[i];
        text<<"family="<<unsigned(g.family)<<" variant="<<unsigned(g.variant)<<" seq="<<g.sequence<<" lifecycle/config="<<g.lifecycle_epoch<<'/'<<g.configuration_epoch<<" fields=0x"<<std::hex<<g.valid_fields<<std::dec
            <<" held="<<bool(g.flags&a::held)<<" coherent="<<bool(g.flags&a::coherent)<<" receipt age="<<age(g.received_qpc)<<" ms; producer age="<<age(g.sample_end_qpc)<<" ms\r\n";
        if(i==0){for(unsigned engine=0;engine<4;++engine){text<<"  Engine "<<engine+1;for(unsigned spool=0;spool<3;++spool){const auto field=engine*6+spool;text<<" N"<<spool+1<<'=';if(g.has(field))text<<g.values[field]/10.0f<<'%';else text<<"unavailable";}text<<"\r\n";}}
        else for(unsigned field=0;field<32;++field)if(g.has(field))text<<"  ["<<field<<"]="<<g.values[field];
    }return text.str();
}
std::string motion_text(const coordinator::ObservationSnapshot& s) {
    namespace m=psx::motion;const auto& view=s.motion;const auto& p=view.status;const auto& source=s.latest.session();std::ostringstream text;
    text<<"PSX MOTION - Auto exporter / public Main and Boost fallback\r\nConnect observes; the connected plugin keeps X-Plane unpaused. Align and Follow enables pose output.\r\nStop Motion releases our owned override. Disconnect/Exit holds an owned pose.\r\nSource preference, mode and delay latch on accepted Follow. Endpoint hold; no prediction. Terrain support has independent ownership.\r\nBank: raw PSX is left-wing-down positive; current presentation is right-wing-down positive (reference 2).\r\nPlacement may initialize/start X-Plane engines.\r\n\r\n";
    const auto& ex=s.exporter;const auto& complete=s.complete_window;
    text<<"Selected source="<<coordinator::pose_source_name(ex.selection.source)<<" epoch="<<ex.selection.selection_epoch<<" reason="<<coordinator::selection_reason_name(ex.selection.reason)
        <<"\r\nExporter Main-server PID="<<ex.process.pid<<" creation FILETIME="<<ex.process.creation_filetime<<" peer="<<ex.peer<<" ready="<<ex.ready<<" error="<<ex.error
        <<"\r\nProducer="<<ex.hello.producer.high<<':'<<ex.hello.producer.low<<" pose epoch="<<complete.domain.pose_epoch<<" reference="<<complete.domain.reference_revision<<" schema="<<ex.hello.schema_revision
        <<"\r\nRaw/invalid="<<ex.raw<<'/'<<ex.invalid<<" sequence/frame gaps="<<ex.sequence_gaps<<'/'<<ex.frame_gaps<<" producer queue drops="<<ex.health.queue_dropped<<" transport drops="<<ex.health.transport_dropped<<" coordinator handoff drops="<<ex.handoff_dropped
        <<"\r\nExporter history="<<complete.count<<" revision="<<complete.revision<<" floor="<<complete.sequence_floor<<" accepted/evicted="<<s.complete_counters.accepted<<'/'<<s.complete_counters.evicted;
    if(complete.count)text<<" coverage="<<(complete.samples[complete.count-1].sample_end_qpc-complete.samples[0].sample_end_qpc)*1000.0/coordinator::qpc_frequency()<<" ms age="<<(s.snapshot_qpc-ex.last_sample_end_qpc)*1000.0/coordinator::qpc_frequency()<<" ms copy interval="<<ex.last_sampling_width*1000.0/coordinator::qpc_frequency()<<" ms";
    text<<"\r\nFull windows selected/superseded="<<s.complete_windows_sent<<'/'<<s.complete_windows_superseded<<"\r\n\r\n";
    pause_text(text,s);text<<"Qi129 is informational; valid received PSX pose remains the motion authority.\r\nECON receipt age can increase while the register remains unchanged.\r\n\r\n";
    text<<"Motion source epoch="<<source.motion_epoch<<" floor="<<source.motion_sequence_floor<<" boundary="<<unsigned(source.motion_boundary)<<" PSX session="<<source.session_epoch<<"\r\n";
    const auto& w=s.motion_window;const auto& h=s.history_counters;
    text<<"Retained window="<<w.revision<<" Boost="<<w.boost_count<<" pitch events="<<w.pitch_count<<" older pitch anchor="<<w.has_pitch_anchor;
    if(w.boost_count)text<<" coverage="<<(w.boost[w.boost_count-1].stamp.received_qpc-w.boost[0].stamp.received_qpc)*1000.0/coordinator::qpc_frequency()<<" ms";
    text<<"\r\nHistory accepted Boost/pitch="<<h.decoded_boost<<'/'<<h.decoded_pitch<<" evicted="<<h.evicted_boost<<'/'<<h.evicted_pitch<<" equal receipt times="<<h.equal_time_boost<<" rejected="<<h.rejected<<" resets="<<h.resets
        <<"\r\nWindows published/selected to pipe/superseded="<<s.motion_windows_published<<'/'<<s.motion_windows_sent<<'/'<<s.motion_windows_superseded<<" (not network loss)\r\n";
    const auto& selected=s.selected_window;
    if(selected.available)text<<"Last selected for pipe (not acknowledged): connection="<<selected.connection_id<<" session revision="<<selected.session_revision<<" window="<<selected.window_revision<<" Boost count="<<selected.boost_count
        <<"\r\n  First/last Boost sequence="<<selected.first_sequence<<'/'<<selected.last_sequence<<" receipt QPC="<<selected.first_received_qpc<<'/'<<selected.last_received_qpc<<" coverage="<<(selected.last_received_qpc-selected.first_received_qpc)*1000.0/coordinator::qpc_frequency()<<" ms published QPC="<<selected.published_qpc<<"\r\n";
    else text<<"No motion window selected for pipe yet.\r\n";
    text<<"Control connection="<<view.connection_id<<" identity="<<(view.identity_available?"available":"unavailable")<<" pending="<<view.outstanding<<" queued="<<view.queued<<"\r\nLocal cancellations="<<view.cancelled<<" late acknowledgements="<<view.late_acks<<" overflow="<<view.control_overflows<<" transport fault="<<view.transport_fault<<"\r\n";
    text<<"Follow: "<<follow_availability(s)<<"\r\n";
    if(view.last_request.operation_id)text<<"Last request #"<<view.last_request.operation_id<<' '<<(view.last_request.action==m::Action::follow?"Align and Follow":"Stop Motion")<<"\r\n";
    if(view.last_ack.operation_id){const auto& ack=view.last_ack;const char* result=ack.result==m::AckResult::accepted?"accepted (pending completion)":ack.result==m::AckResult::applied?"applied":ack.result==m::AckResult::rejected?"rejected":"cancelled";text<<"Last acknowledgement #"<<ack.operation_id<<' '<<result<<" / "<<coordinator::motion_reason_name(ack.reason)<<(view.last_ack_local?" [local; remote result unknown]":" [plugin]")<<"\r\n";}
    if(!view.status_available){text<<"\r\nCurrent plugin motion report unavailable. No output state inferred.";return text.str();}
    const auto frequency=static_cast<double>(coordinator::qpc_frequency());const auto age=[&](std::int64_t tick){return tick>0?(s.snapshot_qpc-tick)*1000.0/frequency:0.0;};
    text<<"\r\n"<<coordinator::motion_phase_name(p.phase)<<" / "<<coordinator::motion_reason_name(p.reason)<<" | report receipt age "<<age(view.status_received_qpc)<<" ms\r\n";
    const auto& trace=p.reconstruction_trace;
    const auto& origin=p.presentation.provenance;
    text<<"Plugin selected="<<coordinator::pose_source_name(p.selected_source)<<" epoch="<<p.selection_epoch<<" applied="<<coordinator::pose_source_name(origin.kind)<<" applied epoch="<<origin.selection_epoch
        <<"\r\nApplied producer="<<origin.domain.producer.high<<':'<<origin.domain.producer.low<<" lifecycle="<<origin.domain.pose_epoch<<" left/right/frame="<<origin.left_sequence<<'/'<<origin.right_sequence<<'/'<<origin.frame_sequence
        <<"\r\nApplied copy QPC start/end/receipt="<<origin.sample_start_qpc<<'/'<<origin.sample_end_qpc<<'/'<<origin.received_qpc<<" handoff residual="<<p.handoff_position_m<<" m / "<<p.handoff_attitude_deg<<" deg\r\n";
    text<<"Effective mode="<<(p.reconstruction.mode==m::ReconstructionMode::direct?"Direct":"Interpolated")<<" delay="<<p.reconstruction.delay_ms<<" ms; "<<reconstruction_name(trace.state)
        <<"\r\nPresentation QPC="<<trace.presentation_qpc<<" target="<<trace.target_qpc<<" window="<<trace.window_revision<<" Source left/right seq="<<trace.left_sequence<<'/'<<trace.right_sequence<<" alpha="<<trace.alpha
        <<"\r\nFollow frames bracketed/oldest/newest/no-pitch="<<p.bracketed_count<<'/'<<p.oldest_hold_count<<'/'<<p.newest_hold_count<<'/'<<p.no_pitch_count<<" hold episodes="<<p.hold_transitions<<" history resets="<<p.history_reset_count<<"\r\n";
    text<<"Follow requested="<<bool(p.flags&m::follow_requested)<<" override owned="<<bool(p.flags&m::override_owned)<<" XP paused="<<bool(p.flags&m::xp_paused)<<" profile valid="<<bool(p.flags&m::profile_valid)<<"\r\n";
    text<<"Plugin load="<<p.plugin_load_id<<" enable="<<p.enable_id<<" aircraft="<<p.aircraft_generation<<" scenery="<<p.scenery_generation<<" reference="<<p.reference_revision<<"\r\nOperation="<<p.operation_id<<" applications="<<p.applied_count<<" discontinuity="<<p.discontinuity_generation<<" last applied QPC="<<p.applied_qpc<<"\r\n";
    if(p.flags&m::pose_valid){const auto& pose=p.presentation.geo;const auto& pair=p.presentation.source;
        if(origin.kind==m::PoseSource::public_main_boost)text<<"\r\nApplied public source pair: PSX session="<<pair.session_epoch<<" motion epoch="<<pair.motion_epoch<<" Boost generation="<<pair.boost_generation<<"\r\nBoost seq="<<pair.boost_sequence<<" receipt age="<<age(pair.boost_received_qpc)<<" ms; Main pitch seq="<<pair.pitch_sequence<<" receipt age="<<age(pair.pitch_received_qpc)<<" ms\r\nMain ECON silence may represent unchanged pitch; these are independent receipt times.\r\n";
        text<<"Flight-deck lat/lon deg="<<pose.latitude_deg<<'/'<<pose.longitude_deg<<" MSL m="<<pose.altitude_msl_m<<" true heading / pitch / bank deg="<<pose.heading_deg<<'/'<<pose.pitch_deg<<'/'<<pose.bank_deg<<"\r\n";
        const auto local=[&](const char* label,const m::LocalPose& v){text<<label<<" X/Y/Z m="<<v.origin.x<<'/'<<v.origin.y<<'/'<<v.origin.z<<" heading/pitch/bank deg="<<v.heading_deg<<'/'<<v.pitch_deg<<'/'<<v.bank_deg<<"\r\n";};local("Written local origin",p.written);if(p.flags&m::readback_valid){local("Readback local origin",p.readback);text<<"Readback residual position="<<p.position_error_m<<" m attitude="<<p.attitude_error_deg<<" deg\r\n";}else text<<"Readback unavailable\r\n";
    }else text<<"\r\nNo valid presentation pose reported.\r\n";
    text<<"\r\nRolling callback metrics; host QPC, not visual latency:\r\n";distribution_text(text,"Pre-model callback",p.pre_callback_duration);distribution_text(text,"Post-model callback",p.post_callback_duration);distribution_text(text,"Boost receive -> apply",p.boost_receive_to_apply);distribution_text(text,"Main pitch age at apply",p.pitch_age);distribution_text(text,"Exporter sample -> apply",p.exporter_sample_to_apply);distribution_text(text,"Exporter sampling copy width",p.sampling_width);distribution_text(text,"Motion frame interval",p.frame_interval);distribution_text(text,"Presentation output age",p.output_age);distribution_text(text,"Completed hold duration",p.hold_duration);text<<"Dropped controls="<<p.control_dropped<<" traces="<<p.trace_dropped;return text.str();
}
}
