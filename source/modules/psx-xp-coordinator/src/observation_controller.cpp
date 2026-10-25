#include <winsock2.h>
#include <ws2tcpip.h>
#include <psx/sync/runtime.hpp>
#include "observation_internal.hpp"
#include "coordinator/protocol.hpp"
#include <algorithm>
#include <array>
#include <cstring>

namespace coordinator {
namespace b=psx::bridge;
namespace d=psx::diagnostics;
namespace {
struct Winsock {int error;Winsock(){WSADATA data{};error=WSAStartup(MAKEWORD(2,2),&data);}~Winsock(){if(!error)WSACleanup();}};
struct SocketState {
    WinHandle event{make_stop_event()};
    SOCKET socket{INVALID_SOCKET};bool connected{},connecting_done{},write_blocked{},close_pending{};DWORD close_error{};std::uint64_t next{},deadline{};unsigned failures{};
    std::unique_ptr<LineFramer> framer{std::make_unique<LineFramer>()};
    std::string output;std::size_t sent{};bool subscribed{};
    ProcessAssociationRetry association;
    std::uint32_t association_error{};
    ~SocketState(){close();}
    void close(){if(socket!=INVALID_SOCKET)closesocket(socket);socket=INVALID_SOCKET;ResetEvent(event.get());connected=connecting_done=write_blocked=close_pending=false;close_error=0;output.clear();sent=0;subscribed=false;association={};association_error=0;*framer=LineFramer{};}
};
constexpr char demands[]="demand=Qs479\ndemand=Qs480\ndemand=Qs481\ndemand=Qs482\ndemand=Qs483\n";
std::size_t graceful(SocketState& channel) {
    if(!channel.connected)return 0;
    std::string output;
    if(channel.sent && channel.sent<channel.output.size() && channel.output[channel.sent-1]!='\n') {
        const auto end=channel.output.find('\n',channel.sent);output=channel.output.substr(channel.sent,end-channel.sent+1);
    }
    output+="exit\n";std::size_t sent=0;const auto deadline=GetTickCount64()+250;
    while(sent<output.size() && GetTickCount64()<deadline) {
        fd_set writable;FD_ZERO(&writable);FD_SET(channel.socket,&writable);timeval timeout{0,10000};
        if(select(0,nullptr,&writable,nullptr,&timeout)>0) {
            const int n=send(channel.socket,output.data()+sent,static_cast<int>(output.size()-sent),0);
            if(n>0)sent+=static_cast<std::size_t>(n);else if(WSAGetLastError()!=WSAEWOULDBLOCK)break;
        }
    }
    shutdown(channel.socket,SD_SEND);
    // Closing a socket with unread Boost bytes may reset TCP and discard the
    // queued exit before the server processes it. Drain only within the same
    // bounded shutdown deadline; shutdown never waits for simulator restart.
    std::array<char,8192> discard{};
    while(GetTickCount64()<deadline) {
        fd_set readable;FD_ZERO(&readable);FD_SET(channel.socket,&readable);timeval timeout{0,10000};
        if(select(0,&readable,nullptr,nullptr,&timeout)>0) {
            const int n=recv(channel.socket,discard.data(),static_cast<int>(discard.size()),0);
            if(n==0 || (n<0 && WSAGetLastError()!=WSAEWOULDBLOCK))break;
        }
    }
    return sent;
}
}
ObservationController::ObservationController(EventLog& log,d::DiagnosticsHub* hub):impl_(std::make_unique<Impl>(log,hub)) {}
ObservationController::~ObservationController(){disconnect();stop_non_normal();join();join_non_normal();}
void ObservationController::Impl::finish_capture_evidence(std::int64_t now) noexcept {
    const auto prepared=capture_evidence_prepared.load(std::memory_order_acquire);
    if(prepared&1)terrain_capture.finish(now);
    if(prepared&2)aircraft_capture.finish(now);
    if(prepared&4)environment_capture.finish(now);
    if(prepared&8)readiness_capture.finish(now);
    if(prepared&16)traffic_capture.finish(now);
}
CaptureEvidenceCompletion ObservationController::Impl::capture_evidence_completion(void* context) noexcept {
    auto& p=*static_cast<Impl*>(context);CaptureEvidenceCompletion result;
    const auto prepared=p.capture_evidence_prepared.load(std::memory_order_acquire);
    result.complete=prepared==31;
    if(!result.complete)result.error=ERROR_NOT_READY;
    try {
        const auto inspect=[&](unsigned bit,const auto& evidence){if(!(prepared&bit))return;
            if(!evidence.done()){result.done=false;return;}const auto status=evidence.status();
            if(!status.complete||status.incomplete||status.error){result.complete=false;if(!result.error)result.error=status.error?status.error:ERROR_INVALID_DATA;}};
        inspect(1,p.terrain_capture);inspect(2,p.aircraft_capture);inspect(4,p.environment_capture);inspect(8,p.readiness_capture);inspect(16,p.traffic_capture);
    }catch(...){result.complete=false;result.error=ERROR_UNHANDLED_EXCEPTION;}
    return result;
}
bool ObservationController::connect(ObservationEndpoint endpoint) {
    IN_ADDR address{};
    if(impl_->io_thread.joinable() || impl_->bridge_thread.joinable() || !endpoint.main_port || !endpoint.boost_port ||
       endpoint.main_port==endpoint.boost_port || InetPtonA(AF_INET,endpoint.ipv4.c_str(),&address)!=1)return false;
    impl_->endpoint=std::move(endpoint);ResetEvent(impl_->acquisition_ready.get());ResetEvent(impl_->stop);ResetEvent(impl_->bridge_ready.get());impl_->io_done=false;impl_->bridge_done=false;
    if(!impl_->test_bridge_name.empty()){impl_->terrain_service.fixture_names(impl_->test_bridge_name+L".terrain.plugin",impl_->test_bridge_name+L".terrain.exporter");impl_->aircraft_service.fixture_names(impl_->test_bridge_name+L".aircraft.plugin",impl_->test_bridge_name+L".aircraft.exporter");impl_->environment_service.fixture_names(impl_->test_bridge_name+L".environment.plugin",impl_->test_bridge_name+L".environment.exporter");impl_->readiness_service.fixture_names(impl_->test_bridge_name+L".status.plugin",impl_->test_bridge_name+L".status.exporter");impl_->traffic_service.fixture_name(impl_->test_bridge_name+L".traffic.exporter");impl_->traffic_visual_service.fixture_name(impl_->test_bridge_name+L".traffic.plugin");}
    {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->view.bridge_last_error=0;impl_->view.bridge_peer_process_id=0;impl_->traffic_visual.configure(static_cast<std::uint64_t>(qpc_frequency()),impl_->traffic_visual_owner_id);impl_->traffic_visual_attachment=impl_->traffic_visual_dropped_seen=0;impl_->traffic_visual_source_key={};impl_->traffic_visual_wanted=impl_->traffic_ownship_policy_committed=impl_->traffic_visual_capture_enabled=false;impl_->traffic_visual_intent_head=impl_->traffic_visual_intent_size=0;impl_->traffic_visual_intent_overflows=impl_->traffic_visual_intent_overflows_seen=0;impl_->traffic_visual_capture_generation=1;impl_->view.traffic_visual=impl_->traffic_visual.view();impl_->traffic_visual_service.associate({});}
    try {
        impl_->bridge_thread=std::thread([p=impl_.get()]{try{p->run_bridge();}catch(const std::system_error& error){if(p->diagnostics)p->diagnostics->producer(2,d::Component::bridge).emit(d::Code::io_failure,d::Severity::error,{},d::ErrorDomain::win32,static_cast<std::uint32_t>(error.code().value()),error.what());std::lock_guard<std::mutex> lock(p->mutex);p->view.bridge=State::error;p->view.bridge_last_error=static_cast<std::uint32_t>(error.code().value());SetEvent(p->stop);}catch(...){if(p->diagnostics)p->diagnostics->producer(2,d::Component::bridge).emit(d::Code::io_failure,d::Severity::error,{},d::ErrorDomain::application,ERROR_UNHANDLED_EXCEPTION,"Bridge worker exception");std::lock_guard<std::mutex> lock(p->mutex);p->view.bridge=State::error;p->view.bridge_last_error=ERROR_UNHANDLED_EXCEPTION;SetEvent(p->stop);}p->bridge_done=true;});
        impl_->io_thread=std::thread([p=impl_.get()]{try{HANDLE ready[]={p->stop,p->bridge_ready.get()};if(WaitForMultipleObjects(2,ready,FALSE,INFINITE)==WAIT_OBJECT_0+1)p->run_io();}catch(...){p->io_event(d::Code::io_failure,d::Component::main,d::Severity::error,d::ErrorDomain::application,ERROR_UNHANDLED_EXCEPTION,"Acquisition worker exception");p->log.add(Level::error,"Observation I/O stopped after internal error");SetEvent(p->stop);p->finish_capture_evidence(qpc_now());p->capture.stop();p->capture.join();}p->finish_capture_evidence(qpc_now());p->capture.stop();p->capture.join();p->end_readiness(qpc_now());p->readiness_service.join();p->readiness_capture.join();p->end_traffic(qpc_now());p->traffic_service.join();p->traffic_visual_service.join();p->traffic_capture.join();{std::lock_guard<std::mutex> lock(p->mutex);p->capture_evidence_started=false;}p->io_done=true;});
    }catch(...){SetEvent(impl_->stop);impl_->io_done=true;if(!impl_->bridge_thread.joinable())impl_->bridge_done=true;join();return false;}
    return true;
}
void ObservationController::disconnect() noexcept {{std::lock_guard<std::mutex> lock(impl_->mutex);impl_->follow_resume_blocked=true;impl_->motion.detach(qpc_now());impl_->traffic_visual_wanted=false;impl_->terrain.following(false,qpc_now());impl_->aircraft.following(false,qpc_now());impl_->environment.following(false,qpc_now());impl_->environment.associate_visibility({});AudioInput in;in.kind=AudioInputKind::detach;in.now=qpc_now();impl_->apply_audio(in);}SetEvent(impl_->stop);impl_->non_normal.associate_psx({});impl_->non_normal.select({});}
bool ObservationController::done() const noexcept{return impl_->io_done && impl_->bridge_done && impl_->readiness_service.done() && impl_->traffic_service.done() && impl_->traffic_visual_service.done() && impl_->capture.done() && impl_->terrain_capture.done() && impl_->aircraft_capture.done() && impl_->environment_capture.done() && impl_->readiness_capture.done() && impl_->traffic_capture.done();}
void ObservationController::join(){
    impl_->readiness_service.request_stop();impl_->traffic_service.request_stop();impl_->traffic_visual_service.request_stop();
    // Acquisition owns normal service teardown. Join it before touching those
    // std::threads here; simultaneous joins have undefined behavior on an early
    // Disconnect or fixture failure while acquisition is still unwinding.
    if(impl_->io_thread.joinable())impl_->io_thread.join();
    if(impl_->bridge_thread.joinable())impl_->bridge_thread.join();
    impl_->readiness_service.join();impl_->readiness_capture.finish(qpc_now());impl_->readiness_capture.join();
    impl_->traffic_service.join();impl_->traffic_capture.finish(qpc_now());impl_->traffic_capture.join();
    impl_->traffic_visual_service.join();
    impl_->terrain_service.stop();impl_->terrain_capture.finish(qpc_now());impl_->terrain_capture.join();
    impl_->aircraft_service.stop();impl_->aircraft_capture.finish(qpc_now());impl_->aircraft_capture.join();
    impl_->environment_service.stop();impl_->environment_capture.finish(qpc_now());impl_->environment_capture.join();
}
ObservationSnapshot ObservationController::snapshot() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);auto snapshot=impl_->view;snapshot.motion=impl_->motion.snapshot();snapshot.audio=impl_->audio.view();snapshot.terrain=impl_->terrain.view();snapshot.terrain_evidence=impl_->terrain_capture.status();snapshot.aircraft=impl_->aircraft.view();snapshot.aircraft_evidence=impl_->aircraft_capture.status();snapshot.environment=impl_->environment.view();snapshot.environment_evidence=impl_->environment_capture.status();snapshot.readiness_evidence=impl_->readiness_capture.status();snapshot.traffic_evidence=impl_->traffic_capture.status();snapshot.snapshot_qpc=qpc_now();
    auto& t=snapshot.traffic;const auto frequency=static_cast<std::uint64_t>(qpc_frequency());t.source_age_qpc=t.retained&&snapshot.snapshot_qpc>=t.observed_qpc?snapshot.snapshot_qpc-t.observed_qpc:-1;
    auto& visual=snapshot.traffic_visual;visual.following=impl_->traffic_visual_wanted;visual.presentation_age_qpc=visual.report_retained&&visual.report.source_observed_qpc>0&&snapshot.snapshot_qpc>=visual.report.source_observed_qpc?snapshot.snapshot_qpc-visual.report.source_observed_qpc:-1;
    if(!visual.following||WaitForSingleObject(impl_->stop,0)==WAIT_OBJECT_0||visual.presentation_age_qpc<0||static_cast<std::uint64_t>(visual.presentation_age_qpc)>=frequency||snapshot.snapshot_qpc<visual.heartbeat_qpc||static_cast<std::uint64_t>(snapshot.snapshot_qpc-visual.heartbeat_qpc)>=frequency*5){visual.owned=visual.report_current=false;visual.drawable_mask=visual.submitted_mask=visual.held_mask=visual.hidden_mask=0;}
    if(WaitForSingleObject(impl_->stop,0)==WAIT_OBJECT_0){visual.following=visual.communication_healthy=visual.requested=visual.pending=visual.owned=visual.report_current=false;visual.phase=visual_wire::Phase::released;visual.reason=visual_wire::Reason::disconnected;}
    if(WaitForSingleObject(impl_->stop,0)==WAIT_OBJECT_0){t.communication_healthy=t.source_available=t.retained=false;t.reason=traffic_wire::Reason::disconnected;t.source={};t.active_slots=t.pose_valid_slots=0;t.source_age_qpc=-1;}
    else if(t.communication_healthy&&snapshot.snapshot_qpc>=t.heartbeat_qpc&&static_cast<std::uint64_t>(snapshot.snapshot_qpc-t.heartbeat_qpc)>=frequency*5){t.communication_healthy=t.source_available=false;t.reason=traffic_wire::Reason::timeout;}
    else if(t.communication_healthy&&t.retained&&(t.source_age_qpc<0||static_cast<std::uint64_t>(t.source_age_qpc)>=frequency)&&(t.reason==traffic_wire::Reason::none||t.reason==traffic_wire::Reason::loading||t.reason==traffic_wire::Reason::stale)){t.source_available=false;t.reason=traffic_wire::Reason::stale;}
    snapshot.capture.finalizing=snapshot.capture.finalizing||(!impl_->capture.done()||!impl_->terrain_capture.done()||!impl_->aircraft_capture.done()||!impl_->environment_capture.done()||!impl_->readiness_capture.done()||!impl_->traffic_capture.done())&&!snapshot.capture.active;
    return snapshot;
}
bool ObservationController::start_capture(std::wstring path) {
    {std::lock_guard<std::mutex> lock(impl_->mutex);
        if(impl_->io_done||WaitForSingleObject(impl_->stop,0)==WAIT_OBJECT_0||!impl_->capture.done()||!impl_->terrain_capture.done()||!impl_->aircraft_capture.done()||!impl_->environment_capture.done()||!impl_->readiness_capture.done()||!impl_->traffic_capture.done()||impl_->capture_preparing||impl_->capture_evidence_started||!impl_->capture_path.empty()||path.empty())return false;
        impl_->capture_preparing=true;
    }
    // Allocation/thread startup is deliberately outside the acquisition mutex.
    bool prepared=false;std::uint32_t sidecars{};try{prepared=impl_->capture.prepare(path);}catch(...){prepared=false;}
    const auto prepare_sidecar=[&](unsigned bit,auto&& operation,const char* message){if(!prepared)return;try{if(operation()){sidecars|=bit;return;}}catch(...){}impl_->log.add(Level::warning,message);};
    const auto frequency=static_cast<std::uint64_t>(qpc_frequency());
    prepare_sidecar(1,[&]{return impl_->terrain_capture.prepare(path+L".terrain.pxtcap",frequency);},"Terrain sidecar preparation failed; aggregate Capture evidence will be incomplete");
    prepare_sidecar(2,[&]{return impl_->aircraft_capture.prepare(path+L".aircraft",frequency);},"Aircraft sidecar preparation failed; aggregate Capture evidence will be incomplete");
    prepare_sidecar(4,[&]{return impl_->environment_capture.prepare(path+L".environment",frequency);},"Environment sidecar preparation failed; aggregate Capture evidence will be incomplete");
    prepare_sidecar(8,[&]{return impl_->readiness_capture.prepare(path+L".readiness",frequency);},"Readiness sidecar preparation failed; aggregate Capture evidence will be incomplete");
    prepare_sidecar(16,[&]{return impl_->traffic_capture.prepare(path+L".traffic",frequency,true);},"Traffic sidecar preparation failed; aggregate Capture evidence will be incomplete");
    std::lock_guard<std::mutex> lock(impl_->mutex);impl_->capture_preparing=false;
    impl_->capture_evidence_prepared=sidecars;
    if(prepared)prepared=impl_->capture.evidence_completion(&Impl::capture_evidence_completion,impl_.get());
    if(!prepared||impl_->io_done||WaitForSingleObject(impl_->stop,0)==WAIT_OBJECT_0){impl_->capture.stop(CaptureStopReason::setup);impl_->finish_capture_evidence(qpc_now());return false;}
    impl_->capture_path=std::move(path);SetEvent(impl_->acquisition_ready.get());return true;
}
void ObservationController::stop_capture() noexcept {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->capture_stop=true;SetEvent(impl_->acquisition_ready.get());}
std::uint64_t ObservationController::request_motion(psx::motion::Action action,psx::motion::ReconstructionSettings settings,psx::motion::SourcePreference preference) {
    std::lock_guard<std::mutex> lock(impl_->mutex);const auto now=qpc_now();
    if(!psx::motion::valid(settings)||static_cast<unsigned>(preference)>1)return 0;
    if(!impl_->io_done&&WaitForSingleObject(impl_->stop,0)!=WAIT_OBJECT_0&&(action==psx::motion::Action::follow||action==psx::motion::Action::stop))impl_->queue_traffic_visual(TrafficVisualEventKind::following,impl_->traffic_ownship_policy_committed?impl_->committed_settings:settings,action==psx::motion::Action::follow);
    if(action==psx::motion::Action::follow&&impl_->proposal_operation)return 0;
    if(!impl_->io_done&&WaitForSingleObject(impl_->stop,0)!=WAIT_OBJECT_0&&(action==psx::motion::Action::follow||action==psx::motion::Action::stop)){impl_->follow_resume_blocked=action==psx::motion::Action::stop;impl_->environment.following(action==psx::motion::Action::follow,now);impl_->aircraft.following(action==psx::motion::Action::follow,now);}
    const auto old=impl_->exporter;auto candidate=old;
    if(action==psx::motion::Action::follow){candidate.preference(preference,settings);candidate.select(now,(impl_->view.latest.session().flags&b::synchronized)!=0);}
    const auto id=impl_->motion.request(action,impl_->view.latest,now,settings,preference,&candidate.state().selection,candidate.fresh(now));
    if(id&&action==psx::motion::Action::follow){impl_->exporter.preference(preference,settings);impl_->select_exporter(now);impl_->proposal_operation=id;impl_->exporter_policy_adopted=true;}
    if(id&&action==psx::motion::Action::stop){impl_->terrain.following(false,now);impl_->aircraft.following(false,now);AudioInput in;in.kind=AudioInputKind::follow;in.now=now;impl_->apply_audio(in);}
    if(impl_->motion.snapshot().transport_fault)++impl_->reset_peer;SetEvent(impl_->published.get());SetEvent(impl_->acquisition_ready.get());return id;
}
bool ObservationController::resume_automatic_follow_services(){
    std::lock_guard<std::mutex> lock(impl_->mutex);const auto now=qpc_now(),frequency=qpc_frequency();const auto& motion=impl_->motion.snapshot();
    if(impl_->io_done||WaitForSingleObject(impl_->stop,0)==WAIT_OBJECT_0||impl_->follow_resume_blocked||
       !motion.connected||!motion.identity_available||motion.transport_fault||!motion.status_available||
       !(motion.status.flags&psx::motion::follow_requested)||now<motion.status.generated_qpc||now-motion.status.generated_qpc>=frequency||
       now<motion.status_received_qpc||now-motion.status_received_qpc>=frequency)return false;
    impl_->environment.following(true,now);impl_->aircraft.following(true,now);
    if(!impl_->traffic_visual_wanted)impl_->queue_traffic_visual(TrafficVisualEventKind::following,impl_->committed_settings,true);
    SetEvent(impl_->acquisition_ready.get());return true;
}
bool ObservationController::follow_environment(bool enabled){std::lock_guard<std::mutex> lock(impl_->mutex);if(impl_->io_done||WaitForSingleObject(impl_->stop,0)==WAIT_OBJECT_0)return false;impl_->environment.following(enabled,qpc_now());SetEvent(impl_->acquisition_ready.get());return true;}
void ObservationController::Impl::exporter_record(const ExportEvidence& value){
    if(exporter_evidence_size==exporter_evidence.size()){++exporter_evidence_lost;return;}
    exporter_evidence[(exporter_evidence_head+exporter_evidence_size)%exporter_evidence.size()]=value;++exporter_evidence_size;
}
void ObservationController::Impl::select_exporter(std::int64_t now){
    ExportEvidence record;record.input=true;record.time=now;record.public_ready=(view.latest.session().flags&b::synchronized)!=0&&view.motion_window.boost_count!=0;
    record.preference=exporter.state().preference;record.settings=exporter.state().settings;record.changed=exporter.select(now,record.public_ready);record.selection=exporter.state().selection;exporter_record(record);
    if(record.changed){if(selection_size==selections.size()){selection_head=selection_size=0;++reset_peer;++view.bridge_control_overflows;}
        selections[(selection_head+selection_size)%selections.size()]=record.selection;++selection_size;}
    const auto& window=exporter.history().window();
    if(window.revision!=view.complete_window.revision&&view.bridge==State::connected&&view.complete_window.count&&bridge_complete_seen!=view.complete_window.revision)++view.complete_windows_superseded;
    view.exporter=exporter.state();view.complete_window=window;view.complete_counters=exporter.history().counters();SetEvent(published.get());
}
void ObservationController::Impl::settle_exporter(const b::MotionAck& ack){
    if(!proposal_operation||ack.operation_id!=proposal_operation)return;
    if(ack.result==psx::motion::AckResult::accepted){committed_preference=exporter.state().preference;committed_settings=exporter.state().settings;proposal_operation=0;traffic_ownship_policy_committed=true;queue_traffic_visual(TrafficVisualEventKind::policy,committed_settings);}
    else if(ack.result==psx::motion::AckResult::rejected||ack.result==psx::motion::AckResult::cancelled){exporter.preference(committed_preference,committed_settings);proposal_operation=0;select_exporter(qpc_now());}
}
void ObservationController::Impl::capture_exporter_evidence(){
    capture.network();
    capture_audio_evidence();
    // Called with facade mutex. UI/bridge can enqueue, but only this acquisition
    // owner writes capture records. Ordering is the same as model mutation.
    while(exporter_evidence_size){const auto& r=exporter_evidence[exporter_evidence_head];
        if(r.input){capture.exporter_input(r.public_ready,r.preference,r.settings,r.time);if(r.changed)capture.selection(r.selection,r.time);}
        else capture.exporter_event(r.event,r.configuration);
        exporter_evidence_head=(exporter_evidence_head+1)%exporter_evidence.size();--exporter_evidence_size;
    }
    if(exporter_evidence_lost!=exporter_evidence_lost_seen||exporter_service.dropped()!=exporter_dropped_seen){capture.stop(exporter_service.dropped()!=exporter_dropped_seen?CaptureStopReason::exporter_handoff:CaptureStopReason::exporter_evidence);exporter_evidence_lost_seen=exporter_evidence_lost;exporter_dropped_seen=exporter_service.dropped();exporter.handoff_dropped(exporter_dropped_seen+exporter_evidence_lost_seen);}
    if(exporter.history().window().revision!=captured_complete_revision){capture.complete_window(exporter,qpc_now());captured_complete_revision=exporter.history().window().revision;}
}
void ObservationController::Impl::drain_exporter(){
    std::lock_guard<std::mutex> lock(mutex);ExporterEvent event;
    for(std::size_t n=0;n<128&&exporter_service.pop(event);++n){
        const auto model_time=qpc_now();
        if(event.kind==ExporterEventKind::association||event.kind==ExporterEventKind::hello){timing_sample_qpc=0;timing_frame=timing_sequence=timing_epoch=0;receipt_to_model={};model_to_publication={};producer_interval={};contiguous_source_gap={};lifecycle_interval={};}
        if(event.kind==ExporterEventKind::snapshot){
            terrain_capture.record(5,psx::terrain::Role::exporter,event.frame.bytes.data(),event.frame.size,event.received_qpc);
            receipt_to_model.add(model_time-event.received_qpc);
            psx::exporter::Snapshot raw;
            if(event.frame.size>=psx::exporter::header_size&&psx::exporter::decode(event.frame.bytes.data()+psx::exporter::header_size,event.frame.size-psx::exporter::header_size,raw)){
                if(timing_sample_qpc&&raw.sample_end_qpc>=timing_sample_qpc){
                    const auto gap=raw.sample_end_qpc-timing_sample_qpc;producer_interval.add(gap);
                    if(raw.pose_epoch!=timing_epoch){++timing_lifecycle_changes;lifecycle_interval.add(gap);}
                    if(raw.frame_sequence>timing_frame+1)timing_frame_gaps+=raw.frame_sequence-timing_frame-1;
                    if(raw.sequence>timing_sequence+1)timing_sequence_gaps+=raw.sequence-timing_sequence-1;
                    if(raw.frame_sequence==timing_frame+1&&raw.sequence==timing_sequence+1&&raw.pose_epoch==timing_epoch&&gap>=qpc_frequency()*psx::motion::exporter_timeout_ms/1000)contiguous_source_gap.add(gap);
                }
                timing_sample_qpc=raw.sample_end_qpc;timing_frame=raw.frame_sequence;timing_sequence=raw.sequence;timing_epoch=raw.pose_epoch;
            }
        }
        ExportEvidence record;record.event=event;record.configuration=model.latest().session().configuration_epoch;exporter_record(record);
        const auto revision=exporter.history().window().revision;
        if(exporter.event(event,record.configuration))audio_exporter(event);
        if(exporter.history().window().revision!=revision){complete_model_qpc=qpc_now();complete_model_revision=exporter.history().window().revision;}
    }
    if(audio_dropped_seen!=exporter_service.audio_dropped()){audio_dropped_seen=exporter_service.audio_dropped();capture.stop(CaptureStopReason::audio_handoff);AudioInput in;in.kind=AudioInputKind::overflow;in.now=qpc_now();apply_audio(in);}
    audio_tick(qpc_now());select_exporter(qpc_now());capture_exporter_evidence();
}
void ObservationController::record_diagnostic(const d::Event& event) noexcept {
    if(!d::valid(event))return;std::lock_guard<std::mutex> lock(impl_->mutex);if(impl_->io_done)return;
    if(impl_->diagnostic_size==impl_->diagnostic_queue.size()){++impl_->view.diagnostic_handoff_lost;return;}
    auto& envelope=impl_->diagnostic_queue[(impl_->diagnostic_head+impl_->diagnostic_size)%impl_->diagnostic_queue.size()];
    envelope={};envelope.batch.connection_id=1;envelope.batch.count=1;envelope.batch.events[0]=event;envelope.received=qpc_now();++impl_->diagnostic_size;SetEvent(impl_->acquisition_ready.get());
}
void ObservationController::Impl::publish() {
    // Capture this complete acquisition window before handing off its replaceable
    // snapshot. Original raw lines have already been recorded individually.
    const auto& history=model.history();const auto& window=history.window();
    std::lock_guard<std::mutex> lock(mutex);
    capture_exporter_evidence();
    if(window.revision!=captured_window_revision || model.latest().session().revision!=captured_window_session_revision) {
        capture.window(model,qpc_now(),exporter.state().selection.selection_epoch);captured_window_revision=window.revision;captured_window_session_revision=model.latest().session().revision;
    }
    const auto& state=model.latest().session();
    if(view.latest.session().revision!=state.revision) {
        if(control_size==controls.size()) {control_head=control_size=0;++reset_peer;++view.bridge_control_overflows;}
        controls[(control_head+control_size)%controls.size()]=state;++control_size;
    }
    for(std::size_t i=1;i<b::source_slots;++i) if(view.bridge==State::connected && view.latest.available()[i] && model.latest().available()[i] &&
        view.latest.records()[i].observation.stamp.sequence!=model.latest().records()[i].observation.stamp.sequence &&
        bridge_seen[i]!=view.latest.records()[i].observation.stamp.sequence)++view.bridge_coalesced;
    if(window.revision!=view.motion_window.revision) {
        if(view.bridge==State::connected && view.motion_window.revision && bridge_window_seen!=view.motion_window.revision)++view.motion_windows_superseded;
        ++view.motion_windows_published;view.motion_window=window;
    }
    view.history_counters=history.counters();view.latest=model.latest();
    SetEvent(published.get());
}
void ObservationController::Impl::publish_observation(){publish();}
void ObservationController::Impl::io_event(d::Code code,d::Component component,d::Severity severity,d::ErrorDomain domain,std::uint32_t error,std::string_view message,std::array<std::uint64_t,6> values,const d::Context* supplied) {
    if(diagnostics){d::Event event;event.component=component;event.code=code;event.severity=severity;event.context=diagnostic_context(model.latest().session());
        {std::lock_guard<std::mutex> lock(mutex);event.context.connection_id=view.bridge==State::connected?view.plugin.connection_id:0;}
        if(supplied)event.context=*supplied;
        event.error_domain=domain;event.error=error;event.values=values;d::text(event.text,message);
        diagnostics->emit_stamped(1,event);if(d::valid(event)){b::DiagnosticBatch batch;batch.connection_id=model.latest().session().connection_id;batch.count=1;batch.events[0]=event;capture.diagnostics(batch,event.qpc);}}
}
void ObservationController::Impl::observe_pause_state() {
    const auto current=pause_source_state(model.latest());
    if(pause_source_changed(last_pause,current)) {
        {std::lock_guard<std::mutex> lock(mutex);
            AudioInput input;input.kind=AudioInputKind::pause;input.now=qpc_now();input.x=current.available?1:0;input.y=current.available?current.raw_value:0;
            apply_audio(input);
        }
        // On invalidation retain the last observed source provenance, but mark
        // availability=0 explicitly. Zero unavailable values never mean unpaused.
        const auto& source=current.available?current:last_pause;
        auto context=diagnostic_context(model.latest().session());context.session_epoch=source.stamp.session_epoch;context.configuration_epoch=source.stamp.configuration_epoch;
        {std::lock_guard<std::mutex> lock(mutex);context.connection_id=view.bridge==State::connected?view.plugin.connection_id:0;}
        const auto text=std::string(current.available?"PSX Qi129=":"PSX Qi129 unavailable; last value=")+std::to_string(source.raw_value)+
            " pause_all="+std::to_string(source.paused_all)+" motion_frozen="+std::to_string(source.motion_frozen);
        io_event(d::Code::configuration,d::Component::decoder,d::Severity::info,d::ErrorDomain::none,0,text,
            {current.available?1ull:0ull,source.raw_value,source.paused_all?1ull:0ull,source.motion_frozen?1ull:0ull,source.stamp.sequence,static_cast<std::uint64_t>(source.stamp.received_qpc)},&context);
        log.add(Level::info,text);
    }
    last_pause=current; // latest receipt retained even for unchanged ECON repeats
}
void ObservationController::Impl::reject(const LineResult& result,SourceStream stream,std::int64_t time) {
    if(!result.selected || (!result.malformed&&!result.bootstrap_error))return;
    auto& bucket=rejections[static_cast<std::size_t>(result.source)*16+result.rejection_reason];
    const auto context=diagnostic_context(model.latest().session());
    if(bucket.count && (bucket.context.session_epoch!=context.session_epoch || bucket.context.configuration_epoch!=context.configuration_epoch || bucket.context.boost_generation!=context.boost_generation))flush_rejections();
    if(!bucket.count){bucket.context=context;{std::lock_guard<std::mutex> lock(mutex);bucket.context.connection_id=view.bridge==State::connected?view.plugin.connection_id:0;}bucket.first=static_cast<std::uint64_t>(time);bucket.stream=stream;}
    ++bucket.count;bucket.last=static_cast<std::uint64_t>(time);
}
void ObservationController::Impl::flush_rejections() {
    for(std::size_t i=0;i<rejections.size();++i){auto& bucket=rejections[i];if(!bucket.count)continue;
        io_event(d::Code::observation_rejected,d::Component::decoder,d::Severity::warning,d::ErrorDomain::decoder,static_cast<std::uint32_t>(i%16),"Rejected selected records; values=source,count,firstQPC,lastQPC,stream",{i/16,bucket.count,bucket.first,bucket.last,static_cast<std::uint64_t>(bucket.stream)},&bucket.context);bucket.count=0;}
}
void ObservationController::Impl::motion_boundary(const LineResult& result,std::int64_t time) {
    capture.boundary(model,result,time);const auto& s=model.latest().session();
    io_event(d::Code::motion_boundary,d::Component::decoder,d::Severity::info,d::ErrorDomain::none,0,"Motion history invalidated; detector detail in optional capture",{s.motion_epoch,s.motion_sequence_floor,static_cast<std::uint64_t>(s.motion_boundary),model.sequence()});
}
void ObservationController::Impl::drain_motion() {
    for(std::size_t i=0;i<MotionControl::record_capacity;++i){MotionRecord record;{std::lock_guard<std::mutex> lock(mutex);if(!motion.next_record(record))break;}
        capture.motion(record);d::Context context{};d::Code code=d::Code::motion_state;d::Severity severity=d::Severity::info;std::array<std::uint64_t,6> values{};
        const auto* payload=record.frame.bytes.data()+b::header_size;const auto size=record.frame.size-b::header_size;
        if(record.kind==MotionRecordKind::request){b::MotionRequest r;b::decode(payload,size,r);context.connection_id=r.connection_id;context.session_epoch=r.session_epoch;values={r.operation_id,static_cast<std::uint64_t>(r.action),r.plugin_load_id,r.enable_id,r.aircraft_generation,r.motion_epoch};code=d::Code::motion_requested;}
        else if(record.kind==MotionRecordKind::ack){b::MotionAck r;b::decode(payload,size,r);if(record.local){std::lock_guard<std::mutex> lock(mutex);settle_exporter(r);}context.connection_id=r.connection_id;values={r.operation_id,static_cast<std::uint64_t>(r.result),static_cast<std::uint64_t>(r.reason),r.plugin_load_id,r.enable_id,record.local?1ull:0ull};if(r.result==psx::motion::AckResult::rejected || r.result==psx::motion::AckResult::cancelled){code=d::Code::motion_rejected;severity=d::Severity::warning;}}
        else {b::MotionStatus r;b::decode(payload,size,r);context.connection_id=r.connection_id;context.session_epoch=r.presentation.source.session_epoch;values={r.operation_id,static_cast<std::uint64_t>(r.phase),static_cast<std::uint64_t>(r.reason),r.flags,r.discontinuity_generation,r.applied_count};}
        io_event(code,d::Component::bridge,severity,d::ErrorDomain::none,0,record.local?"Local cancellation; remote outcome not established":"Motion control or observed state transition",values,&context);
    }
    std::uint64_t overflows;{std::lock_guard<std::mutex> lock(mutex);overflows=motion.snapshot().control_overflows;}
    if(overflows!=motion_overflows_reported){io_event(d::Code::motion_rejected,d::Component::bridge,d::Severity::error,d::ErrorDomain::application,ERROR_BUFFER_OVERFLOW,"Required motion control/evidence overflow; connection faulted; active capture incomplete",{overflows,overflows-motion_overflows_reported});motion_overflows_reported=overflows;capture.stop(CaptureStopReason::motion_evidence);}
}
void ObservationController::Impl::run_io() {
    readiness.configure(static_cast<std::uint64_t>(qpc_frequency()));readiness_attachment=readiness_dropped_seen=readiness_journal_revision=0;readiness_assertion_qpc=readiness_publish_qpc=0;readiness_service.plugin({});readiness_service.associate({});readiness_service.start(static_cast<std::uint64_t>(qpc_frequency()));
    traffic.configure(static_cast<std::uint64_t>(qpc_frequency()));traffic_dropped_seen=traffic_capture_revision=0;traffic_publish_qpc=traffic_journal_qpc=0;traffic_journal_key={};traffic_service.associate({});traffic_service.start(static_cast<std::uint64_t>(qpc_frequency()));
    traffic_visual_service.start(static_cast<std::uint64_t>(qpc_frequency()),traffic_visual_owner_id);
    model.clock_frequency(static_cast<std::uint64_t>(qpc_frequency()));
    {std::lock_guard<std::mutex> lock(mutex);exporter.frequency(static_cast<std::uint64_t>(qpc_frequency()));}
    Winsock wsa;if(wsa.error){io_event(d::Code::io_failure,d::Component::main,d::Severity::error,d::ErrorDomain::winsock,wsa.error,"WSAStartup");log.add(Level::error,"Observation Winsock initialization failed");SetEvent(stop);return;}
    terrain_service.plugin({});terrain_service.start(static_cast<std::uint64_t>(qpc_frequency()),exporter_service.owner());aircraft_service.plugin({});aircraft_service.start(static_cast<std::uint64_t>(qpc_frequency()),exporter_service.owner());environment_service.plugin({});environment_service.start(static_cast<std::uint64_t>(qpc_frequency()),exporter_service.owner());
    transition(SessionEvent::wanted_on,qpc_now());log.add(Level::info,"Observation connect enabled; Main/Boost retries and local bridge active");
    io_event(d::Code::connect_requested,d::Component::main);
    std::array<SocketState,2> channels;
    const auto status=[&](std::size_t i,auto update){std::lock_guard<std::mutex> lock(mutex);update(i?view.boost:view.main);};
    const auto lost=[&](std::size_t i,DWORD error,d::ErrorDomain domain=d::ErrorDomain::winsock){
        auto& c=channels[i];if(!c.framer->complete())status(i,[](auto& s){++s.truncated;});
        if(c.connected)transition(i?SessionEvent::boost_down:SessionEvent::main_down,qpc_now());if(!i){associate_traffic({},qpc_now());std::lock_guard<std::mutex> lock(mutex);environment.associate_visibility({});}c.close();
        const auto delay=psx::sync::enabled()?1u:1u<<(std::min)(c.failures++,3u);c.next=GetTickCount64()+delay*1000;
        status(i,[&](auto& s){s.state=State::disconnected;s.last_error=error;++s.errors;s.retry_seconds=delay;});
        log.add(Level::warning,i?"Boost unavailable; independent retry scheduled":"Main unavailable; source state invalidated and retry scheduled");
        io_event(d::Code::retry,i?d::Component::boost:d::Component::main,d::Severity::warning,domain,error,"Retry scheduled",{delay});
    };
    std::int64_t last_diagnostics=0;
    const auto metrics_interval=qpc_frequency()*(diagnostics?diagnostics->options().config.metrics_ms:1000)/1000;
    while(WaitForSingleObject(stop,0)!=WAIT_OBJECT_0) {
        std::wstring start_path;bool stop_recording=false;
        {std::lock_guard<std::mutex> lock(mutex);start_path.swap(capture_path);stop_recording=capture_stop;capture_stop=false;}
        // Bridge diagnostic handoff is bounded; acquisition remains the sole capture producer.
        for(std::size_t n=0;n<diagnostic_queue.size();++n) {
            DiagnosticEnvelope event;
            {std::lock_guard<std::mutex> lock(mutex);if(!diagnostic_size)break;event=diagnostic_queue[diagnostic_head];diagnostic_head=(diagnostic_head+1)%diagnostic_queue.size();--diagnostic_size;}
            capture.diagnostics(event.batch,event.received);
        }
        drain_exporter();drain_terrain();drain_aircraft();drain_environment();drain_readiness();drain_traffic();drain_motion();
        {std::lock_guard<std::mutex> lock(mutex);if(capture_evidence_started&&!capture.status().active){
            // A primary file failure/queue limit must also retire its sidecars.
            // finish() only posts a bounded state change; all file work stays on their owners.
            finish_capture_evidence(qpc_now());capture_evidence_started=false;
        }}
        if(stop_recording){
            finish_traffic_capture(qpc_now());finish_capture_evidence(qpc_now());
            flush_rejections();
            {std::lock_guard<std::mutex> lock(mutex);capture_exporter_evidence();if(capture.status().active)capture.finish(model,qpc_now(),&exporter,&audio);else capture.stop();capture_evidence_started=false;}
            const auto c=capture.status();
            if(c.cutoff.acknowledged)io_event(d::Code::capture_cutoff,d::Component::capture,d::Severity::info,d::ErrorDomain::none,0,"Acquisition cutoff acknowledged",{c.capture_id,static_cast<std::uint64_t>(c.cutoff.owner_qpc),c.cutoff.ingestion_sequence});
            else io_event(d::Code::capture_incomplete,d::Component::capture,d::Severity::info,d::ErrorDomain::none,0,"Capture cancelled before recording or already incomplete",{c.capture_id});
            start_path.clear();
        }
        if(!start_path.empty()) {
            {std::lock_guard<std::mutex> lock(mutex);capture_exporter_evidence();if(!capture.start(start_path,model,static_cast<std::uint64_t>(qpc_frequency()),qpc_now(),&exporter,&audio)){capture.stop(CaptureStopReason::setup);log.add(Level::error,"Capture start rejected; prepared request cancelled or invalid");}}
            io_event(d::Code::capture_start,d::Component::capture,d::Severity::info,d::ErrorDomain::none,0,"Capture requested",{capture.status().capture_id});
            if(capture.status().active){
                const auto prepared=capture_evidence_prepared.load(std::memory_order_acquire);
                if(prepared&1)terrain_capture.begin(qpc_now());
                if(prepared&2){aircraft_capture_start=qpc_now();aircraft_capture.begin(aircraft_capture_start);}
                if(prepared&4){environment_capture_start=qpc_now();environment_capture.begin(environment_capture_start);}
                if(prepared&8)begin_readiness(qpc_now());
                if(prepared&16)begin_traffic(qpc_now());
                std::lock_guard<std::mutex> lock(mutex);capture_evidence_started=true;
            }else {finish_capture_evidence(qpc_now());finish_traffic_capture(qpc_now());}
        }
        const auto now=GetTickCount64();
        for(std::size_t i=0;i<2;++i) {
            auto& c=channels[i];if(c.socket!=INVALID_SOCKET || now<c.next)continue;
            c.socket=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(c.socket==INVALID_SOCKET){lost(i,WSAGetLastError());continue;}
            if(WSAEventSelect(c.socket,c.event.get(),FD_CONNECT|FD_READ|FD_WRITE|FD_CLOSE)){const auto code=WSAGetLastError();lost(i,code);continue;}BOOL no_delay=TRUE;if(setsockopt(c.socket,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&no_delay),sizeof(no_delay))){const auto code=WSAGetLastError();lost(i,code);continue;}
            sockaddr_in remote{};remote.sin_family=AF_INET;remote.sin_port=htons(i?endpoint.boost_port:endpoint.main_port);InetPtonA(AF_INET,endpoint.ipv4.c_str(),&remote.sin_addr);
            status(i,[](auto& s){s.state=State::connecting;++s.attempts;s.retry_seconds=0;});c.deadline=now+5000;
            const auto result=::connect(c.socket,reinterpret_cast<sockaddr*>(&remote),sizeof(remote));
            const auto code=result==SOCKET_ERROR?WSAGetLastError():0;if(code && code!=WSAEWOULDBLOCK){lost(i,code);continue;}c.connecting_done=result==0;
        }
        // Ready inputs all share this wait. The 20 ms deadline services source
        // freshness/audio leases; it is not an ingress polling dependency.
        DWORD wait_ms=20;
        for(const auto& c:channels)if(c.connecting_done||(c.connected&&!c.write_blocked&&c.sent<c.output.size()))wait_ms=0;
        HANDLE waits[]={stop,acquisition_ready.get(),reinterpret_cast<HANDLE>(exporter_service.ready_event()),reinterpret_cast<HANDLE>(terrain_service.ready_event()),reinterpret_cast<HANDLE>(aircraft_service.ready_event()),reinterpret_cast<HANDLE>(environment_service.ready_event()),reinterpret_cast<HANDLE>(non_normal.visibility_ready_event()),reinterpret_cast<HANDLE>(readiness_service.ready_event()),reinterpret_cast<HANDLE>(traffic_service.ready_event()),reinterpret_cast<HANDLE>(traffic_visual_service.ready_event()),channels[0].event.get(),channels[1].event.get()};
        const auto waited=WaitForMultipleObjects(static_cast<DWORD>(std::size(waits)),waits,FALSE,wait_ms);
        if(waited==WAIT_OBJECT_0)break;
        if(waited==WAIT_FAILED)throw_win32_error("Acquisition wait");
        // The exporter wake is serviced before any bounded public socket batch.
        drain_exporter();drain_terrain();drain_aircraft();drain_environment();drain_readiness();drain_traffic();drain_non_normal();
        for(std::size_t i=0;i<2;++i) {
            auto& c=channels[i];if(c.socket==INVALID_SOCKET)continue;
            WSANETWORKEVENTS events{};
            if(WSAEnumNetworkEvents(c.socket,c.event.get(),&events)==SOCKET_ERROR){lost(i,WSAGetLastError());continue;}
            if(events.lNetworkEvents&FD_CONNECT){if(events.iErrorCode[FD_CONNECT_BIT]){lost(i,events.iErrorCode[FD_CONNECT_BIT]);continue;}if(!c.connected)c.connecting_done=true;}
            if(events.lNetworkEvents&FD_WRITE){if(events.iErrorCode[FD_WRITE_BIT]){lost(i,events.iErrorCode[FD_WRITE_BIT]);continue;}c.write_blocked=false;}
            if(events.lNetworkEvents&FD_READ&&events.iErrorCode[FD_READ_BIT]){lost(i,events.iErrorCode[FD_READ_BIT]);continue;}
            if(events.lNetworkEvents&FD_CLOSE){c.close_pending=true;c.close_error=events.iErrorCode[FD_CLOSE_BIT];}
            if(!c.connected && !c.connecting_done && GetTickCount64()>=c.deadline){lost(i,WSAETIMEDOUT);continue;}
            if(!c.connected && c.connecting_done) {
                c.connecting_done=false;
                c.connected=true;transition(i?SessionEvent::boost_up:SessionEvent::main_up,qpc_now());status(i,[](auto& s){s.state=State::connected;s.last_error=0;});
                log.add(Level::info,i?"Boost socket connected; waiting for usable record":"Main socket connected; waiting for bootstrap (version is informational)");
                io_event(d::Code::connected,i?d::Component::boost:d::Component::main);
            }
            if(!c.connected)continue;
            if(!i && !c.close_pending && c.association.due(GetTickCount64())) {
                ProcessIdentity owner;std::uint32_t error{};
                if(process_lookup(static_cast<std::uintptr_t>(c.socket),owner,error)) {
                    exporter_service.associate(owner,static_cast<std::uint64_t>(qpc_frequency()));terrain_service.associate(owner);aircraft_service.associate(owner);environment_service.associate(owner);
                    {std::lock_guard<std::mutex> lock(mutex);environment.associate_visibility(owner);}
                    readiness_service.associate(owner);associate_traffic(owner,qpc_now());
                    c.association.accepted=true;
                    if(c.association.step) log.add(Level::info,"Optional PSX services associated after retry; Main connection preserved");
                } else {
                    if(!c.association.step || error!=c.association_error)
                        io_event(d::Code::io_failure,d::Component::main,d::Severity::warning,d::ErrorDomain::win32,error,"Optional PSX process association unavailable; bounded retry, public sockets continue");
                    c.association_error=error;c.association.failed(GetTickCount64());
                }
            }
            if(!i && !c.subscribed && (model.latest().session().flags&b::synchronized)) {c.output=demands;c.sent=0;c.subscribed=true;c.failures=0;}
            if(c.sent<c.output.size() && !c.write_blocked) {
                const int n=send(c.socket,c.output.data()+c.sent,static_cast<int>(c.output.size()-c.sent),0);
                if(n>0){c.sent+=static_cast<std::size_t>(n);status(i,[&](auto& s){s.bytes_out+=n;});}
                else {const auto code=n<0?WSAGetLastError():0;if(n==0 || code!=WSAEWOULDBLOCK){lost(i,code);continue;}c.write_blocked=true;}
            }
            if(!(events.lNetworkEvents&FD_READ)&&!c.close_pending)continue;
            // At most 64 KiB per channel per turn, including a flood of tiny lines.
            for(int batch=0;batch<8 && c.socket!=INVALID_SOCKET;++batch) {
                std::array<char,8192> bytes{};const int n=recv(c.socket,bytes.data(),static_cast<int>(bytes.size()),0);
                if(n==0){lost(i,c.close_error);break;}
                if(n<0){const auto code=WSAGetLastError();if(code!=WSAEWOULDBLOCK)lost(i,code);else if(c.close_pending)lost(i,c.close_error);break;}
                const auto received=qpc_now();status(i,[&](auto& s){s.bytes_in+=n;});bool remote_exit=false;
                const bool framed=c.framer->feed(bytes.data(),static_cast<std::size_t>(n),[&](std::string_view line){
                    const auto previous_revision=model.latest().session().revision;
                    const auto previous_session=model.latest().session().session_epoch;
                    const auto previous_version=model.latest().session().psx_version;
                    const auto outcome=model.line(i?SourceStream::boost:SourceStream::main,line,received,qpc_now(),qpc_now);if(!i)aircraft_public(outcome,line,received);
                    if(!i&&previous_session&&model.latest().session().session_epoch!=previous_session){std::lock_guard<std::mutex> lock(mutex);environment.invalidate_visibility(outcome.decoded_qpc);}
                    if(previous_version!=model.latest().session().psx_version)log.add(Level::info,std::string("PSX version: ")+model.latest().session().psx_version.data()+" (informational; observation continues)");
                    capture.line(i?SourceStream::boost:SourceStream::main,line,received,outcome.decoded_qpc);
                    if(outcome.motion_boundary)motion_boundary(outcome,outcome.decoded_qpc);
                    if(model.latest().session().revision!=previous_revision ||
                       (outcome.source==psx::aircraft::SourceRecord::qi129 && !outcome.malformed))observe_pause_state();
                    reject(outcome,i?SourceStream::boost:SourceStream::main,received);
                    remote_exit|=outcome.remote_exit;
                    {std::lock_guard<std::mutex> lock(mutex);++(i?view.boost:view.main).lines;view.selected+=outcome.selected?1:0;view.malformed+=outcome.malformed?1:0;view.unsupported+=outcome.selected?0:1;view.bootstrap_errors+=outcome.bootstrap_error?1:0;}
                    if(outcome.selected && !outcome.malformed){decode_samples[decode_index++%decode_samples.size()]=static_cast<std::uint64_t>(outcome.decoded_qpc-received);decode_count=(std::min)(decode_count+1,decode_samples.size());}
                    // Every semantic transition enters the control queue before
                    // any replaceable latest-record publication.
                    if(model.latest().session().revision!=previous_revision){publish();io_event(d::Code::configuration,d::Component::decoder,d::Severity::info,d::ErrorDomain::none,0,"Source lifecycle changed",{model.latest().session().flags});}
                });
                publish_observation();
                if(!framed || remote_exit){lost(i,framed?0:ERROR_INVALID_DATA,framed?d::ErrorDomain::winsock:d::ErrorDomain::protocol);break;}
                if(i && (model.latest().session().flags&b::boost_available))c.failures=0;
                if(WaitForSingleObject(reinterpret_cast<HANDLE>(exporter_service.ready_event()),0)==WAIT_OBJECT_0)drain_exporter();if(WaitForSingleObject(reinterpret_cast<HANDLE>(aircraft_service.ready_event()),0)==WAIT_OBJECT_0)drain_aircraft();if(WaitForSingleObject(reinterpret_cast<HANDLE>(environment_service.ready_event()),0)==WAIT_OBJECT_0)drain_environment();if(WaitForSingleObject(reinterpret_cast<HANDLE>(readiness_service.ready_event()),0)==WAIT_OBJECT_0)drain_readiness(); // bounded socket bursts cannot starve independent ingress
            }
            if(c.socket!=INVALID_SOCKET&&!i&&!c.subscribed&&(model.latest().session().flags&b::synchronized)){c.output=demands;c.sent=0;c.subscribed=true;c.failures=0;}
        }
        const auto tick=qpc_now();
        if(tick-last_diagnostics>=metrics_interval) {
            flush_rejections();
            auto sorted=decode_samples;std::sort(sorted.begin(),sorted.begin()+decode_count);
            b::Distribution distribution;if(decode_count){distribution.count=decode_count;distribution.minimum=sorted[0];distribution.maximum=sorted[decode_count-1];distribution.p50=sorted[(decode_count*50+99)/100-1];distribution.p95=sorted[(decode_count*95+99)/100-1];distribution.p99=sorted[(decode_count*99+99)/100-1];}
            b::PluginStatus plugin;bool available;MotionSnapshot motion_view;
            {std::lock_guard<std::mutex> lock(mutex);view.decode_duration=distribution;plugin=view.plugin;available=view.plugin_available;motion_view=motion.snapshot();}
            if(available)capture.plugin(plugin,tick);if(motion_view.status_available)capture.motion_status(motion_view.status,motion_view.status_received_qpc);last_diagnostics=tick;
            const auto snapshot=[&]{std::lock_guard<std::mutex> lock(mutex);return view;}();
            io_event(d::Code::metrics,d::Component::decoder,d::Severity::info,d::ErrorDomain::none,0,"Acquisition totals",{snapshot.selected,snapshot.malformed,snapshot.bootstrap_errors,snapshot.main.bytes_in,snapshot.boost.bytes_in,snapshot.diagnostic_handoff_lost});
            // Exact rolling-window nearest-rank quantiles, QPC ticks, newest 512 samples.
            std::array<std::array<std::uint64_t,6>,5> timing;
            {std::lock_guard<std::mutex> lock(mutex);const auto f=static_cast<std::uint64_t>(qpc_frequency());timing={receipt_to_model.summary(f),model_to_publication.summary(f),producer_interval.summary(f),contiguous_source_gap.summary(f),lifecycle_interval.summary(f)};}
            constexpr const char* labels[]={"exporter receipt-to-model","exporter model-to-publication newest selected","exporter producer interval","exporter contiguous source gap >=100ms","exporter lifecycle interval"};
            const auto identity=std::string(" pid=")+std::to_string(snapshot.exporter.process.pid)+" selection="+std::to_string(snapshot.exporter.selection.selection_epoch);
            for(std::size_t i=0;i<timing.size();++i)io_event(d::Code::metrics,d::Component::decoder,d::Severity::info,d::ErrorDomain::none,0,std::string(labels[i])+identity,timing[i]);
            io_event(d::Code::metrics,d::Component::decoder,d::Severity::info,d::ErrorDomain::none,0,"Exporter attribution: lifecycle,frameGaps,sequenceGaps,healthRejected,queueDrop,transportDrop",{timing_lifecycle_changes,timing_frame_gaps,timing_sequence_gaps,snapshot.exporter.health.rejected,snapshot.exporter.health.queue_dropped,snapshot.exporter.health.transport_dropped});
            const auto& audio_view=snapshot.audio;
            io_event(d::Code::metrics,d::Component::audio,d::Severity::info,d::ErrorDomain::none,0,"Audio epoch,groups,received,rejected,edges,overflows",{audio_view.state.audio_epoch,audio_view.state.available_groups,audio_view.received,audio_view.rejected,audio_view.transitions,audio_view.overflows});
            io_event(d::Code::metrics,d::Component::audio,d::Severity::info,d::ErrorDomain::none,0,"Audio ready,active,held,missing,appliedSequence,appliedSampleQPC",{audio_view.plugin.ready_groups,audio_view.plugin.active_groups,audio_view.plugin.held_groups,audio_view.plugin.missing_groups,audio_view.plugin.applied_sequence,static_cast<std::uint64_t>(audio_view.plugin.applied_sample_qpc)});
            const auto& terrain_view=snapshot.terrain;
            if(diagnostics){const auto terrain_journal=diagnostics->producer(1,d::Component::terrain);
                terrain_journal.emit(d::Code::metrics,d::Severity::info,{},d::ErrorDomain::none,0,"Terrain epoch,queries,surfaces,rejected,controls,acks",{terrain_view.terrain_epoch,terrain_view.queries,terrain_view.surfaces,terrain_view.rejected,terrain_view.controls,terrain_view.acks});
                terrain_journal.emit(d::Code::metrics,d::Severity::info,{},d::ErrorDomain::none,0,"Terrain ownership,phase,reason,consumedFrame,applied,transportDrops",{std::uint64_t(terrain_view.plugin.owned)|(std::uint64_t(terrain_view.exporter.owned)<<1),static_cast<std::uint64_t>(terrain_view.phase),static_cast<std::uint64_t>(terrain_view.reason),terrain_view.consumed.frame_sequence,terrain_view.consumed.applied,terrain_service.dropped()});}
            const auto& env=snapshot.environment;io_event(d::Code::metrics,d::Component::application,d::Severity::info,d::ErrorDomain::none,0,"Environment source,owned,pending,rejected,obsolete,transportDrops",{env.source.sequence,std::uint64_t(env.groups[0].owned)|(std::uint64_t(env.groups[1].owned)<<1),std::uint64_t(env.groups[0].pending)|(std::uint64_t(env.groups[1].pending)<<1),env.rejected,env.obsolete,environment_service.dropped()});
            environment_metrics(env,tick);
            const auto& history=model.history().counters();
            io_event(d::Code::metrics,d::Component::decoder,d::Severity::info,d::ErrorDomain::none,0,"Motion history: decodedBoost,pitch,evictedBoost,pitch,equalTimeBoost,rejected",{history.decoded_boost,history.decoded_pitch,history.evicted_boost,history.evicted_pitch,history.equal_time_boost,history.rejected});
            const auto& window=model.history().window();const auto coverage=window.boost_count?static_cast<std::uint64_t>(window.boost[window.boost_count-1].stamp.received_qpc-window.boost[0].stamp.received_qpc):0;
            io_event(d::Code::metrics,d::Component::decoder,d::Severity::info,d::ErrorDomain::none,0,"Motion windows: published,selectedToPipe,superseded,coverageQPC,resets,revision",{snapshot.motion_windows_published,snapshot.motion_windows_sent,snapshot.motion_windows_superseded,coverage,history.resets,window.revision});
        }
        {std::lock_guard<std::mutex> lock(mutex);view.capture=capture.status();}
    }
    end_readiness(qpc_now());readiness_service.join();readiness_capture.join();
    for(std::size_t i=0;i<2;++i){const auto sent=graceful(channels[i]);if(channels[i].connected)transition(i?SessionEvent::boost_down:SessionEvent::main_down,qpc_now());channels[i].close();status(i,[&](auto& s){s.state=State::stopped;s.retry_seconds=0;s.bytes_out+=sent;});}
    {std::lock_guard<std::mutex> lock(mutex);terrain.following(false,qpc_now());}drain_terrain();terrain_service.stop();drain_terrain();terrain_capture.finish(qpc_now());terrain_capture.join();{std::lock_guard<std::mutex> lock(mutex);aircraft.following(false,qpc_now());}drain_aircraft();aircraft_service.stop();drain_aircraft();aircraft_capture.finish(qpc_now());aircraft_capture.join();
    {std::lock_guard<std::mutex> lock(mutex);environment.following(false,qpc_now());}drain_environment();environment_service.stop();drain_environment();environment_capture.finish(qpc_now());environment_capture.join();
    exporter_service.stop();drain_exporter();
    finish_traffic_capture(qpc_now());finish_capture_evidence(qpc_now());
    transition(SessionEvent::wanted_off,qpc_now());{std::lock_guard<std::mutex> lock(mutex);motion.detach(qpc_now());}drain_motion();flush_rejections();{std::lock_guard<std::mutex> lock(mutex);capture_exporter_evidence();capture.finish(model,qpc_now(),&exporter,&audio);}capture.stop();capture.join();
    {std::lock_guard<std::mutex> lock(mutex);view.capture=capture.status();}
    log.add(Level::info,"Observation disconnected; graceful exit attempted on connected PSX channels");
    io_event(d::Code::shutdown_result,d::Component::main,d::Severity::info,d::ErrorDomain::none,0,"Acquisition stopped after bounded graceful exits");
}
}
