#include <psx/sync/worker_io.hpp>
#include "observation_internal.hpp"
#include <sddl.h>
#include <algorithm>
#include <cstring>
#include <optional>

namespace coordinator {
namespace b=psx::bridge;
namespace d=psx::diagnostics;
namespace {
struct LocalFreeDeleter{void operator()(void* p)const noexcept{if(p)LocalFree(p);}};
using LocalPtr=std::unique_ptr<void,LocalFreeDeleter>;
std::wstring user_sid() {
    HANDLE raw=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw))throw_win32_error("OpenProcessToken");
    WinHandle token(raw);DWORD n=0;GetTokenInformation(token.get(),TokenUser,nullptr,0,&n);std::vector<unsigned char> bytes(n);
    if(!GetTokenInformation(token.get(),TokenUser,bytes.data(),n,&n))throw_win32_error("TokenUser");
    LPWSTR sid=nullptr;if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&sid))throw_win32_error("SID string");
    LocalPtr owner(sid);return sid;
}
struct Operation {
    WinHandle event{CreateEventW(nullptr,TRUE,FALSE,nullptr)};OVERLAPPED op{};bool pending{};
    Operation(){if(!event.valid())throw_win32_error("pipe event");op.hEvent=event.get();}
    void prepare(){ResetEvent(event.get());op={};op.hEvent=event.get();}
    void cancel(HANDLE pipe){if(pending){psx::sync::io_cancel(pipe,&op);DWORD bytes=0;psx::sync::io_result(pipe,&op,&bytes,TRUE);pending=false;}}
};
struct Peer {
    std::uint64_t id{},frequency{};bool hello{},failed{};
    std::int64_t last_received{},report_received{};b::PluginStatus plugin;bool new_plugin{};
    void* owner{};void (*batch_received)(void*,const b::DiagnosticBatch&,std::int64_t){};
    void (*plugin_received)(void*,const b::PluginStatus&,std::int64_t){};
    bool (*ack_received)(void*,const b::MotionAck&,std::int64_t){};
    void (*audio_received)(void*,const psx::audio::Status&,std::int64_t){};
    bool (*motion_received)(void*,const b::MotionStatus&,std::int64_t){};
    void (*view_support_received)(void*,bool){};
    void (*view_received)(void*,const b::ViewReply&){};
    void (*coverage_support)(void*,bool){};
    void (*coverage_received)(void*,const b::CoverageReply&){};
    static bool receive(void* context,const b::Header& h,const std::uint8_t* data,std::size_t size) {
        auto& p=*static_cast<Peer*>(context);
        if(!p.hello){b::Hello hello;if(h.kind!=b::Kind::hello || !b::decode(data,size,hello) || hello.role!=b::Role::plugin || hello.qpc_frequency!=p.frequency)return false;p.hello=true;p.view_support_received(p.owner,std::string_view(hello.build.data()).ends_with(";view1"));p.coverage_support(p.owner,std::string_view(hello.build.data()).find(";cov1;")!=std::string_view::npos);}
        else if(h.kind==b::Kind::plugin_status){if(!b::decode(data,size,p.plugin) || p.plugin.connection_id!=p.id)return false;p.report_received=qpc_now();p.plugin_received(p.owner,p.plugin,p.report_received);}
        else if(h.kind==b::Kind::diagnostic_events){b::DiagnosticBatch batch;if(!b::decode(data,size,batch) || batch.connection_id!=p.id)return false;p.batch_received(p.owner,batch,qpc_now());}
        else if(h.kind==b::Kind::coverage_reply){b::CoverageReply reply;if(!b::decode(data,size,reply)||reply.request.connection_id!=p.id)return false;p.coverage_received(p.owner,reply);}
        else if(h.kind==b::Kind::view_reply){b::ViewReply reply;if(!b::decode(data,size,reply)||reply.connection_id!=p.id)return false;p.view_received(p.owner,reply);}
        else if(h.kind==b::Kind::motion_ack){b::MotionAck ack;if(!b::decode(data,size,ack) || ack.connection_id!=p.id || !p.ack_received(p.owner,ack,qpc_now()))return false;}
        else if(h.kind==b::Kind::motion_status){b::MotionStatus status;if(!b::decode(data,size,status) || status.connection_id!=p.id || !p.motion_received(p.owner,status,qpc_now()))return false;}
        else if(h.kind==b::Kind::audio_status){psx::audio::Status status;if(b::decode(data,size,status)&&status.connection_id==p.id)p.audio_received(p.owner,status,qpc_now());}
        else return false;
        p.last_received=qpc_now();return true;
    }
};
}
std::wstring observation_pipe_name(){return L"\\\\.\\pipe\\psx-ios.observation.v1."+user_sid();}
std::uint64_t ObservationController::request_scenery_view(b::ViewAction action) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto& p=*impl_;const auto now=qpc_now();
    if(static_cast<unsigned>(action)>2||p.view.bridge!=State::connected||!p.view.scenery_view_available||
       !p.view.plugin_available||!(p.view.plugin.flags&b::enabled)||!p.view.plugin.enable_id||
       now<p.view.plugin_received_qpc||now-p.view.plugin_received_qpc>=qpc_frequency()||
       p.view_request_size==p.view_requests.size()||p.view_operation==UINT64_MAX)return 0;
    b::ViewRequest request{p.view.plugin.connection_id,++p.view_operation,p.view.plugin.plugin_load_id,p.view.plugin.enable_id,now,action};
    p.view_requests[(p.view_request_head+p.view_request_size)%p.view_requests.size()]=request;++p.view_request_size;
    SetEvent(p.published.get());return request.operation_id;
}
bool ObservationController::Impl::accept_motion_status(const b::MotionStatus& status,std::int64_t received) {
    std::lock_guard<std::mutex> lock(mutex);
    if(!motion.status(status,received))return false;
    SetEvent(acquisition_ready.get());
    const auto& observed=motion.snapshot();
    if(!observed.status_available||observed.status.generated_qpc!=status.generated_qpc||
       observed.status.plugin_load_id!=status.plugin_load_id||observed.status.enable_id!=status.enable_id)return true;
    const bool stop_pending=observed.last_request.operation_id&&observed.last_request.action==psx::motion::Action::stop;
    const bool follows=(status.flags&psx::motion::follow_requested)&&!stop_pending&&!follow_resume_blocked;
    terrain.following(follows,received);
    if(follows!=audio.view().following){AudioInput in;in.kind=AudioInputKind::follow;in.now=received;in.x=follows?1:0;apply_audio(in);}
    // A retained Follow can outlive this bridge or coordinator process. Adopt
    // its motion policy without issuing placement or changing independently
    // selected services. Connect remains observation-only.
    if(follows&&!exporter_policy_adopted&&!proposal_operation){
        committed_preference=status.source_preference;committed_settings=status.reconstruction;
        exporter.preference(status.source_preference,status.reconstruction);exporter_policy_adopted=true;
        traffic_ownship_policy_committed=true;queue_traffic_visual(TrafficVisualEventKind::policy,status.reconstruction);
        select_exporter(received);
    }
    return true;
}
void ObservationController::Impl::retire_plugin_bridge(std::uint32_t failure,bool protocol_failure) {
    std::lock_guard<std::mutex> lock(mutex);const auto now=qpc_now();
    motion.detach(now);terrain.following(false,now);
    // Transport retirement releases service ownership through the service-lost
    // events. It must not erase the operator's general Follow selection.
    SetEvent(acquisition_ready.get());AudioInput in;in.kind=AudioInputKind::detach;in.now=now;apply_audio(in);
    view.bridge=State::listening;view.plugin_available=false;coverage_supported=false;coverage_ready=false;view.scenery_view_available=view.scenery_view_reply_available=false;view.scenery_view={};view_request_head=view_request_size=0;view.bridge_peer_process_id=0;non_normal.select({});
    view.bridge_last_error=protocol_failure?ERROR_INVALID_DATA:failure;if(protocol_failure)++view.bridge_rejected;
}
void ObservationController::Impl::run_bridge() {
    const bool remote=psx::sync::enabled();
    const auto events=diagnostics?diagnostics->producer(2,d::Component::bridge):d::DiagnosticProducer{};
    WinHandle pipe;
    if(!remote){
        const auto sid=user_sid();const auto endpoint_name=test_bridge_name.empty()?L"\\\\.\\pipe\\psx-ios.observation.v1."+sid:test_bridge_name;
        const auto dacl=L"D:P(A;;GA;;;SY)(A;;GA;;;"+sid+L")";PSECURITY_DESCRIPTOR raw=nullptr;
        if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(dacl.c_str(),SDDL_REVISION_1,&raw,nullptr))throw_win32_error("pipe DACL");
        LocalPtr descriptor(raw);SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),descriptor.get(),FALSE};
        pipe.reset(CreateNamedPipeW(endpoint_name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,16384,16384,0,&security));
    }
    if(!remote&&!pipe.valid()){const auto error=GetLastError();events.emit(d::Code::io_failure,d::Severity::error,{},d::ErrorDomain::win32,error,"CreateNamedPipeW; PSX connection not started");std::lock_guard<std::mutex> lock(mutex);view.bridge=State::error;view.bridge_last_error=error;view.bridge_peer_process_id=0;SetEvent(stop);log.add(Level::error,"Local plugin pipe unavailable: another coordinator or harness may own it. PSX connection was not started.");return;}
    SetEvent(bridge_ready.get());
    Operation connection,read,write;
    while(WaitForSingleObject(stop,0)!=WAIT_OBJECT_0) {
        {std::lock_guard<std::mutex> lock(mutex);view.bridge=State::listening;view.plugin_available=false;coverage_supported=false;coverage_ready=false;view.scenery_view_available=view.scenery_view_reply_available=false;view.scenery_view={};view_request_head=view_request_size=0;view.bridge_peer_process_id=0;}
        std::optional<psx::sync::WorkerIo> network;
        if(remote){if(!psx::sync::runtime()->status().connected){WaitForSingleObject(stop,20);continue;}network.emplace(psx::sync::Family::observation);}
        connection.prepare();bool attached=remote||ConnectNamedPipe(pipe.get(),&connection.op)!=FALSE;
        if(!attached) {
            const auto error=GetLastError();if(error==ERROR_PIPE_CONNECTED)attached=true;
            else if(error==ERROR_IO_PENDING){connection.pending=true;HANDLE waits[]={stop,connection.event.get()};if(WaitForMultipleObjects(2,waits,FALSE,INFINITE)==WAIT_OBJECT_0+1){DWORD bytes=0;attached=psx::sync::io_result(pipe.get(),&connection.op,&bytes,FALSE)!=FALSE;connection.pending=false;}else{connection.cancel(pipe.get());break;}}
            else break;
        }
        if(!attached){DisconnectNamedPipe(pipe.get());continue;}
        static std::atomic<std::uint64_t> ids{static_cast<std::uint64_t>(qpc_now())};
        Peer peer;peer.id=++ids;peer.frequency=static_cast<std::uint64_t>(qpc_frequency());peer.last_received=qpc_now();
        ULONG peer_process{};if(remote)peer_process=network->status().peer.pid;else GetNamedPipeClientProcessId(pipe.get(),&peer_process);
        peer.owner=this;peer.batch_received=[](void* owner,const b::DiagnosticBatch& batch,std::int64_t received) {
            auto& self=*static_cast<Impl*>(owner);
            if(self.diagnostics)for(std::size_t i=0;i<batch.count;++i)self.diagnostics->emit(2,batch.events[i]);
            std::lock_guard<std::mutex> lock(self.mutex);
            if(self.diagnostic_size==self.diagnostic_queue.size()){self.view.diagnostic_handoff_lost+=batch.count;return;}
            self.diagnostic_queue[(self.diagnostic_head+self.diagnostic_size)%self.diagnostic_queue.size()]={batch,received};++self.diagnostic_size;SetEvent(self.acquisition_ready.get());
        };
        peer.plugin_received=[](void* owner,const b::PluginStatus& status,std::int64_t received){auto& self=*static_cast<Impl*>(owner);std::lock_guard<std::mutex> lock(self.mutex);if(self.view.plugin_available&&(self.view.plugin.plugin_load_id!=status.plugin_load_id||self.view.plugin.enable_id!=status.enable_id||self.view.plugin.aircraft_generation!=status.aircraft_generation)){self.terrain.provider_boundary(received);self.aircraft.provider_boundary(received);self.view.scenery_view_reply_available=false;self.view.scenery_view={};}self.view.plugin=status;self.view.plugin_available=true;self.view.plugin_received_qpc=received;self.motion.plugin(status,received);SetEvent(self.acquisition_ready.get());};
        peer.ack_received=[](void* owner,const b::MotionAck& ack,std::int64_t received){auto& self=*static_cast<Impl*>(owner);std::lock_guard<std::mutex> lock(self.mutex);if(!self.motion.acknowledge(ack,received))return false;self.settle_exporter(ack);SetEvent(self.acquisition_ready.get());const auto& request=self.motion.snapshot().last_request;if(!self.follow_resume_blocked&&ack.operation_id==request.operation_id&&request.action==psx::motion::Action::follow&&(ack.result==psx::motion::AckResult::accepted||ack.result==psx::motion::AckResult::applied)){self.terrain.following(true,received);AudioInput in;in.kind=AudioInputKind::follow;in.now=received;in.x=1;self.apply_audio(in);SetEvent(self.acquisition_ready.get());}return true;};
        peer.audio_received=[](void* owner,const psx::audio::Status& status,std::int64_t received){auto& self=*static_cast<Impl*>(owner);std::lock_guard<std::mutex> lock(self.mutex);if(!self.view.plugin_available||status.plugin_generation!=self.view.plugin.enable_id||status.aircraft_generation!=self.view.plugin.aircraft_generation)return;AudioInput in;in.kind=AudioInputKind::status;in.now=received;in.status=status;self.apply_audio(in);SetEvent(self.acquisition_ready.get());};
        peer.coverage_support=[](void* owner,bool available){auto& self=*static_cast<Impl*>(owner);std::lock_guard lock(self.mutex);self.coverage_supported=available;};
        peer.coverage_received=[](void* owner,const b::CoverageReply& reply){auto& self=*static_cast<Impl*>(owner);std::lock_guard lock(self.mutex);
            const auto& q=self.coverage_request;
            if(!self.coverage_pending||q.connection_id!=reply.request.connection_id||q.operation_id!=reply.request.operation_id||q.plugin_load_id!=reply.request.plugin_load_id||q.enable_id!=reply.request.enable_id||q.query!=reply.request.query||qpc_now()>=q.deadline_qpc)return;
            self.coverage_reply=reply;self.coverage_reply.request.deadline_qpc=q.deadline_qpc;self.coverage_ready=true;
        };
        peer.view_support_received=[](void* owner,bool available){auto& self=*static_cast<Impl*>(owner);std::lock_guard lock(self.mutex);self.view.scenery_view_available=available;};
        peer.view_received=[](void* owner,const b::ViewReply& reply){auto& self=*static_cast<Impl*>(owner);std::lock_guard lock(self.mutex);if(!self.view.plugin_available||reply.plugin_load_id!=self.view.plugin.plugin_load_id||reply.enable_id!=self.view.plugin.enable_id||reply.operation_id>self.view_operation||reply.operation_id<self.view.scenery_view.operation_id)return;self.view.scenery_view=reply;self.view.scenery_view_reply_available=true;};
        peer.motion_received=[](void* owner,const b::MotionStatus& status,std::int64_t received){return static_cast<Impl*>(owner)->accept_motion_status(status,received);};
        d::Context context;context.connection_id=peer.id;events.emit(d::Code::connected,d::Severity::info,context);
        b::Framer framer;b::Frame output;std::size_t offset=0;std::uint32_t sequence=0;bool hello_sent=false,initial_status=true;
        std::uint64_t peer_revision=0,reset_generation=0,sent_window=0,sent_window_session=0,sent_complete=0,peer_selection=0;bool initial_selection=true,prefer_window=true,prefer_audio=true;std::array<std::uint64_t,b::source_slots> sent{};std::size_t next_source=1;
        std::int64_t last_status=0,write_started=0;std::array<std::uint8_t,8192> input{};
        {std::lock_guard<std::mutex> lock(mutex);if(WaitForSingleObject(stop,0)==WAIT_OBJECT_0)break;control_head=control_size=0;selection_head=selection_size=0;exporter_policy_adopted=false;reset_generation=reset_peer;view.bridge=State::connected;view.bridge_last_error=0;view.bridge_peer_process_id=peer_process;++view.bridge_attachments;view.plugin={};coverage_supported=false;coverage_ready=false;view.scenery_view_available=view.scenery_view_reply_available=false;view.scenery_view={};view_request_head=view_request_size=0;bridge_seen={};bridge_window_seen=bridge_complete_seen=0;motion.attach(peer.id,qpc_now());SetEvent(acquisition_ready.get());AudioInput in;in.kind=AudioInputKind::attach;in.now=qpc_now();in.x=peer.id;apply_audio(in);}
        log.add(Level::info,"Observation plugin peer attached; validating Hello");
        bool healthy=true,stall_reported=false;DWORD failure=0;bool protocol_failure=false;
        while(healthy && WaitForSingleObject(stop,0)!=WAIT_OBJECT_0) {
            if(!psx::sync::worker_poll(pipe.get()))break;
            const auto now=qpc_now();
            if(write.pending && !stall_reported && now-write_started>qpc_frequency()/10){std::lock_guard<std::mutex> lock(mutex);++view.bridge_write_stalls;stall_reported=true;}
            if(now-peer.last_received>5*qpc_frequency() || (write.pending && now-write_started>5*qpc_frequency())){failure=ERROR_TIMEOUT;healthy=false;break;}
            {std::lock_guard<std::mutex> lock(mutex);if(reset_generation!=reset_peer || motion.snapshot().transport_fault){healthy=false;break;}}
            DWORD count=0;
            if(read.pending && WaitForSingleObject(read.event.get(),0)==WAIT_OBJECT_0) {
                const auto ok=psx::sync::io_result(pipe.get(),&read.op,&count,FALSE);failure=ok?0:GetLastError();read.pending=false;
                if(!ok || !count){healthy=false;break;}if(!framer.feed(input.data(),count,Peer::receive,&peer)){protocol_failure=true;healthy=false;break;}
            }
            if(!read.pending) {
                read.prepare();const auto ok=psx::sync::io_read(pipe.get(),input.data(),static_cast<DWORD>(input.size()),&count,&read.op);
                if(ok){if(!count){healthy=false;break;}if(!framer.feed(input.data(),count,Peer::receive,&peer)){protocol_failure=true;healthy=false;break;}}
                else {const auto error=GetLastError();if(error==ERROR_IO_PENDING)read.pending=true;else{failure=error;healthy=false;break;}}
            }
            if(write.pending && WaitForSingleObject(write.event.get(),0)==WAIT_OBJECT_0) {
                const auto ok=psx::sync::io_result(pipe.get(),&write.op,&count,FALSE);failure=ok?0:GetLastError();write.pending=false;
                if(!ok || !count){healthy=false;break;}offset+=count;
            }
            if(!write.pending && offset==output.size) {
                output.size=offset=0;
                if(!hello_sent) {
                    b::Hello hello;hello.role=b::Role::coordinator;hello.instance_id=peer.id;hello.qpc_frequency=peer.frequency;
                    constexpr char build[]="psx-xp-coordinator-" PSX_COORDINATOR_VERSION ";cov1";std::copy(std::begin(build),std::end(build),hello.build.begin());
                    healthy=b::encode(hello,++sequence,output);hello_sent=true;
                } else if(peer.hello) {
                    std::lock_guard<std::mutex> lock(mutex);
                    // Acquisition may publish while incoming pipe traffic is
                    // serviced. Stamp only after the protected snapshot is fixed;
                    // the loop's earlier `now` can precede its decode/receipt.
                    const auto publication=qpc_now();
                    if(initial_status || control_size || now-last_status>=qpc_frequency()) {
                        auto state=view.latest.session();
                        if(!initial_status && control_size){state=controls[control_head];control_head=(control_head+1)%controls.size();--control_size;}
                        if(initial_status){control_head=control_size=0;initial_status=false;}
                        state.connection_id=peer.id;state.published_qpc=publication;
                        if(state.revision!=peer_revision){sent={};bridge_seen={};peer_revision=state.revision;}
                        healthy=b::encode(state,++sequence,output);last_status=publication;
                    } else if(initial_selection||selection_size){
                        auto selected=view.exporter.selection;
                        if(!initial_selection&&selection_size){selected=selections[selection_head];selection_head=(selection_head+1)%selections.size();--selection_size;}
                        if(initial_selection){selection_head=selection_size=0;initial_selection=false;}
                        selected.connection_id=peer.id;healthy=b::encode(selected,++sequence,output);peer_selection=selected.selection_epoch;sent_window=sent_window_session=sent_complete=0;
                    } else if(peer_revision==view.latest.session().revision&&peer_selection==view.exporter.selection.selection_epoch) {
                        b::MotionRequest request;
                        if(motion.next_request(request))healthy=b::encode(request,++sequence,output);
                        else if(coverage_pending&&!coverage_sent&&qpc_now()<coverage_request.deadline_qpc){coverage_sent=true;healthy=b::encode(coverage_request,++sequence,output);}
                        else if(view_request_size){auto view_request=view_requests[view_request_head];view_request_head=(view_request_head+1)%view_requests.size();--view_request_size;healthy=b::encode(view_request,++sequence,output);}
                        else if(prefer_audio&&audio_frame(output,publication)){healthy=true;prefer_audio=false;++sequence;for(unsigned i=0;i<4;++i)output.bytes[12+i]=static_cast<std::uint8_t>(sequence>>(8*i));}
                        else {
                            prefer_audio=true;
                            // Required session/control records always win. Alternate
                            // replaceable window and systems work if both are busy.
                            b::MotionWindow window;window.connection_id=peer.id;window.session_revision=peer_revision;window.published_qpc=publication;window.samples=view.motion_window;window.selection_epoch=peer_selection;
                            auto source_selection=view.exporter.selection;source_selection.connection_id=peer.id;
                            b::CompletePoseWindow complete;complete.connection_id=peer.id;complete.selection_epoch=peer_selection;complete.published_qpc=publication;complete.samples=view.complete_window;
                            const bool pending_complete=sent_complete!=complete.samples.revision&&complete.samples.count&&b::matches(complete,source_selection);
                            auto state=view.latest.session();state.connection_id=peer.id;
                            const bool pending_window=(sent_window!=window.samples.revision || sent_window_session!=peer_revision) && b::matches(window,state,source_selection);
                            std::size_t source=0;
                            for(std::size_t step=0;step<b::source_slots-1;++step) {
                                const auto i=1+(next_source-1+step)%(b::source_slots-1);
                                if(view.latest.available()[i] && sent[i]!=view.latest.records()[i].observation.stamp.sequence){source=i;break;}
                            }
                            if(pending_complete&&(prefer_window||!source)){
                                healthy=b::encode(complete,++sequence,output);if(healthy){if(complete_model_revision==complete.samples.revision)model_to_publication.add(publication-complete_model_qpc);sent_complete=complete.samples.revision;bridge_complete_seen=sent_complete;++view.complete_windows_sent;prefer_window=false;}
                            } else if(pending_window && (prefer_window || !source)) {
                                healthy=b::encode(window,++sequence,output);
                                if(healthy){sent_window=window.samples.revision;sent_window_session=peer_revision;bridge_window_seen=sent_window;++view.motion_windows_sent;prefer_window=false;
                                    auto& selected=view.selected_window;selected={};selected.available=true;selected.connection_id=peer.id;selected.session_revision=peer_revision;selected.window_revision=window.samples.revision;selected.boost_count=window.samples.boost_count;selected.published_qpc=publication;
                                    if(selected.boost_count){const auto& first=window.samples.boost[0].stamp;const auto& last=window.samples.boost[selected.boost_count-1].stamp;selected.first_sequence=first.sequence;selected.last_sequence=last.sequence;selected.first_received_qpc=first.received_qpc;selected.last_received_qpc=last.received_qpc;}
                                }
                            } else if(source) {
                                auto value=view.latest.records()[source];value.connection_id=peer.id;value.session_revision=peer_revision;value.published_qpc=publication;
                                healthy=b::encode(value,++sequence,output);sent[source]=value.observation.stamp.sequence;bridge_seen[source]=sent[source];next_source=source+1==b::source_slots?1:source+1;prefer_window=true;
                            } else if(audio_frame(output,publication)){++sequence;for(unsigned i=0;i<4;++i)output.bytes[12+i]=static_cast<std::uint8_t>(sequence>>(8*i));}
                        }
                    }
                }
            }
            if(!healthy)break;
            if(!write.pending && offset<output.size) {
                write.prepare();write_started=qpc_now();const auto ok=psx::sync::io_write(pipe.get(),output.bytes.data()+offset,static_cast<DWORD>(output.size-offset),&count,&write.op);
                if(ok){if(!count){healthy=false;break;}offset+=count;}
                else {const auto error=GetLastError();if(error==ERROR_IO_PENDING)write.pending=true;else{failure=error;healthy=false;break;}}
            }
            // Independent events: incoming heartbeats remain serviced while an
            // outgoing observation is blocked by pipe backpressure.
            HANDLE waits[]={stop,read.event.get(),write.event.get(),published.get()};
            if(!read.pending)ResetEvent(read.event.get());if(!write.pending)ResetEvent(write.event.get());
            WaitForMultipleObjects(4,waits,FALSE,output.size && !write.pending?0:(remote?2:1000));
        }
        read.cancel(pipe.get());write.cancel(pipe.get());if(!remote)DisconnectNamedPipe(pipe.get());
        retire_plugin_bridge(failure,protocol_failure);
        if(WaitForSingleObject(stop,0)!=WAIT_OBJECT_0)events.emit(protocol_failure?d::Code::frame_rejected:d::Code::disconnected,protocol_failure?d::Severity::warning:d::Severity::info,context,protocol_failure?d::ErrorDomain::protocol:d::ErrorDomain::win32,protocol_failure?ERROR_INVALID_DATA:failure,"Plugin peer detached");
        if(WaitForSingleObject(stop,0)!=WAIT_OBJECT_0)log.add(Level::warning,"Observation peer detached or failed validation/timeout; source sockets continue independently");
    }
    {std::lock_guard<std::mutex> lock(mutex);view.bridge=State::stopped;view.plugin_available=false;coverage_supported=false;coverage_ready=false;view.scenery_view_available=view.scenery_view_reply_available=false;view.scenery_view={};view_request_head=view_request_size=0;}
}
}


