#include <psx/sync/worker_io.hpp>
#include "psx_plugin/terrain_service.hpp"
#include "psx_plugin/terrain_source_clock.hpp"
#include "psx_plugin/observation_client.hpp"
#include "psx_plugin/triple_buffer.hpp"
#include <Windows.h>
#include <sddl.h>
#include <atomic>
#include <algorithm>
#include <thread>

namespace psx::xp {
namespace t=terrain;
namespace {
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    Handle()=default;explicit Handle(HANDLE h):value(h){}
    ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
    bool valid() const noexcept {return value&&value!=INVALID_HANDLE_VALUE;}
};
struct Operation {
    Handle event{CreateEventW(nullptr,TRUE,FALSE,nullptr)};OVERLAPPED op{};bool pending{};
    void prepare() noexcept {ResetEvent(event.value);op={};op.hEvent=event.value;}
    void cancel(HANDLE pipe) noexcept {if(pending){psx::sync::io_cancel(pipe,&op);DWORD ignored{};psx::sync::io_result(pipe,&op,&ignored,TRUE);pending=false;}}
};
std::wstring sid_string() {
    Handle token;token.value=nullptr;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value))return {};
    alignas(TOKEN_USER) std::array<std::byte,sizeof(TOKEN_USER)+SECURITY_MAX_SID_SIZE> data{};DWORD bytes{};
    if(!GetTokenInformation(token.value,TokenUser,data.data(),static_cast<DWORD>(data.size()),&bytes))return {};
    LPWSTR text{};if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,&text))return {};
    const std::wstring result(text);LocalFree(text);return result;
}
std::uint64_t creation_time() noexcept {
    FILETIME created{},exit{},kernel{},user{};if(!GetProcessTimes(GetCurrentProcess(),&created,&exit,&kernel,&user))return 0;
    return (static_cast<std::uint64_t>(created.dwHighDateTime)<<32)|created.dwLowDateTime;
}
}
std::wstring terrain_endpoint() {
    const auto sid=sid_string();if(sid.empty())return {};
    const std::string prefix=t::plugin_pipe_prefix;return std::wstring(prefix.begin(),prefix.end())+sid;
}
struct TerrainService::Impl {
    Handle stop_event{CreateEventW(nullptr,TRUE,FALSE,nullptr)};std::thread worker;
    TripleBuffer<TerrainInput> incoming;TripleBuffer<TerrainResult> results;
    TripleBuffer<TerrainHost> hosts;TripleBuffer<TerrainServiceStatus> statuses;
    TripleBuffer<TerrainCaptureResult> capture_results;
    std::atomic<std::uint64_t> host_generation{};
    TerrainInput latest{};TerrainServiceStatus status{};TerrainHost host{};
    TerrainResult latest_result{};bool result_pending{},diagnostics_pending{};
    std::array<t::ProbeDetail,7> probe_details{};std::array<bool,7> probe_pending{};
    t::RenderDetail render_detail{};bool render_pending{};
    std::array<t::Ack,32> acks{};std::size_t ack_head{},ack_count{};
    t::Hello hello{};std::wstring endpoint;
    std::uint64_t revision{},next_connection{},last_operation{},last_query{},last_source_sequence{},last_frame_sequence{},lease_generation{};
    std::int64_t last_sample{},last_received{},last_io{},next_summary{},capture_start_qpc{};
    std::uint32_t peer_pid{},incoming_sequence{},outgoing_sequence{};
    std::uint64_t owner{};bool subscribed{};t::Framer framer;
    t::Query output_query{};std::int64_t output_qpc{};std::uint64_t output_lease{};
    std::uint64_t output_capture{};bool output_is_capture{};
    std::uint64_t result_revision{};diagnostics::DiagnosticProducer events;TerrainServiceStatus reported{};
    ~Impl(){SetEvent(stop_event.value);if(worker.joinable())worker.join();}
    void publish() noexcept {latest.revision=++revision;incoming.writing()=latest;if(incoming.publish())++status.coalesced;report();}
    void report() noexcept {
        status.active=latest.active;status.subscribed=subscribed;statuses.writing()=status;statuses.publish();
        diagnostics::Context context;context.plugin_load_id=hello.instance.high;context.enable_id=hello.instance.low;
        context.connection_id=status.connection;context.aircraft_generation=host.aircraft_generation;context.scenery_generation=host.scenery_generation;
        if(status.running!=reported.running||status.connected!=reported.connected||status.subscribed!=reported.subscribed||status.active!=reported.active||status.reason!=reported.reason||status.error!=reported.error) {
            events.emit(diagnostics::Code::terrain_state,status.error?diagnostics::Severity::error:diagnostics::Severity::info,context,
                status.error?diagnostics::ErrorDomain::win32:diagnostics::ErrorDomain::none,status.error,"Terrain provider state",
                {status.running,status.connected,status.subscribed,status.active,static_cast<std::uint64_t>(status.reason),host.generation});
            reported=status;
        }
        const auto now=observation_qpc();if(now>=next_summary) {
            next_summary=now+static_cast<std::int64_t>(hello.qpc_frequency);
            events.emit(diagnostics::Code::terrain_metrics,diagnostics::Severity::info,context,diagnostics::ErrorDomain::none,0,"Terrain provider totals and QPC maxima",
                {status.queries,status.surfaces,status.rejected,status.coalesced,status.query_ticks_max,status.source_age_ticks_max});
        }
    }
    void clear_details() noexcept {probe_pending={};render_pending=false;}
    void clear_query() noexcept {latest.have_query=false;result_pending=diagnostics_pending=false;clear_details();result_revision=0;last_query=last_source_sequence=last_frame_sequence=0;last_sample=last_received=0;}
    bool ack(const t::Control& control,t::Reason reason,t::Phase phase) noexcept {
        if(ack_count==acks.size()){status.reason=t::Reason::overflow;++status.rejected;return false;}
        acks[(ack_head+ack_count)%acks.size()]={control,phase,reason,last_query};++ack_count;return true;
    }
    bool matches(const t::Query& query) const noexcept {
        const auto& c=latest.control;
        return subscribed&&latest.active&&query.owner==owner&&query.owner==c.owner&&query.terrain_epoch==c.terrain_epoch&&
            query.reference.producer==c.producer&&query.reference.pose_epoch==c.pose_epoch;
    }
    bool fresh_output(std::int64_t now) const noexcept {
        const auto& r=output_query.reference;
        return terrain_source_clock::fresh_output(r,now,output_qpc,psx::sync::freshness_now(now),hello.qpc_frequency);
    }
    bool output_valid(std::int64_t now) const noexcept {
        if(output_lease!=latest.lease_generation||!matches(output_query))return false;
        return output_is_capture?latest.capture_active&&output_capture==latest.capture_generation&&output_qpc>=capture_start_qpc:fresh_output(now);
    }
    static bool frame(void* context,const t::Header& header,const std::uint8_t* data,std::size_t bytes) noexcept {
        auto& self=*static_cast<Impl*>(context);
        if(!header.sequence||header.sequence<=self.incoming_sequence)return false;
        self.incoming_sequence=header.sequence;const auto now=observation_qpc();self.last_io=now;
        if(header.kind==t::Kind::subscribe) {
            t::Subscribe subscribe;if(self.subscribed||!t::decode(data,bytes,subscribe)||subscribe.instance!=self.hello.instance||
                subscribe.pid!=self.peer_pid||subscribe.qpc_frequency!=self.hello.qpc_frequency)return false;
            self.owner=subscribe.owner;self.subscribed=true;self.status.reason=t::Reason::none;self.publish();return true;
        }
        if(!self.subscribed)return false;
        if(header.kind==t::Kind::capture) {
            t::Capture capture;if(!t::decode(data,bytes,capture)||capture.owner!=self.owner||capture.generation<self.latest.capture_generation)return false;
            if(capture.generation==self.latest.capture_generation&&capture.enabled&& !self.latest.capture_active)return false;
            if(capture.enabled&&(capture.generation!=self.latest.capture_generation||!self.latest.capture_active))self.capture_start_qpc=now;
            self.clear_details();self.latest.capture_generation=capture.generation;self.latest.capture_active=capture.enabled!=0;self.publish();return true;
        }
        if(header.kind==t::Kind::control) {
            t::Control control;if(!t::decode(data,bytes,control)||control.owner!=self.owner)return false;
            ++self.status.controls;
            if(control.operation<=self.last_operation){++self.status.rejected;return self.ack(control,t::Reason::epoch,t::Phase::unavailable);}
            self.last_operation=control.operation;
            const bool domain=control.producer==self.latest.control.producer&&control.terrain_epoch==self.latest.control.terrain_epoch&&control.pose_epoch==self.latest.control.pose_epoch;
            if(control.action==t::Action::acquire&&control.producer==self.latest.control.producer&&
                (control.terrain_epoch<self.latest.control.terrain_epoch||control.pose_epoch<self.latest.control.pose_epoch)) {
                ++self.status.rejected;return self.ack(control,t::Reason::epoch,t::Phase::unavailable);
            }
            if(control.action==t::Action::renew&&(!self.latest.active||!domain||now>=self.latest.lease_expires_qpc)) {
                ++self.status.rejected;return self.ack(control,t::Reason::owner,t::Phase::unavailable);
            }
            if(control.action==t::Action::release&&self.latest.active&&!domain){++self.status.rejected;return self.ack(control,t::Reason::epoch,t::Phase::unavailable);}
            if(control.action!=t::Action::renew){self.clear_query();self.latest.lease_generation=++self.lease_generation;}
            self.latest.control=control;self.latest.active=control.action!=t::Action::release;
            self.latest.lease_expires_qpc=self.latest.active?now+static_cast<std::int64_t>(self.hello.qpc_frequency*t::lease_ms/1000):0;
            self.status.reason=self.latest.active?t::Reason::none:t::Reason::stopped;self.publish();
            return self.ack(control,t::Reason::none,self.latest.active?t::Phase::active:t::Phase::held);
        }
        if(header.kind==t::Kind::query) {
            t::Query query;if(!t::decode(data,bytes,query))return false;++self.status.queries;
            if(!self.matches(query)||now>=self.latest.lease_expires_qpc){++self.status.rejected;self.status.reason=t::Reason::owner;self.report();return true;}
            const auto& r=query.reference;
            if(query.query_id<=self.last_query||r.sequence<self.last_source_sequence||r.frame_sequence<self.last_frame_sequence||
                r.sample_qpc<self.last_sample||r.received_qpc<self.last_received){++self.status.rejected;return false;}
            // A held source identity is immutable, including native reference and original clocks.
            if(self.latest.have_query&&r.sequence==self.latest.query.reference.sequence) {
                t::Query old=self.latest.query;old.query_id=query.query_id;t::Frame a,b;
                if(!t::encode(old,1,a)||!t::encode(query,1,b)||a.size!=b.size||a.bytes!=b.bytes)return false;
            }
            self.last_query=query.query_id;self.last_source_sequence=r.sequence;self.last_frame_sequence=r.frame_sequence;
            self.last_sample=r.sample_qpc;self.last_received=r.received_qpc;
            self.latest.query=query;self.latest.have_query=true;self.publish();return true;
        }
        return false;
    }
    void poll_owner() noexcept {
        if(hosts.consume()) {
            const auto previous=host;host=hosts.reading();
            if(host.generation!=previous.generation) {
                result_pending=diagnostics_pending=false;
                clear_details();
                status.reason=host.loading?t::Reason::loading:
                    host.scenery_generation!=previous.scenery_generation||host.aircraft_generation!=previous.aircraft_generation?t::Reason::scenery:t::Reason::local_frame;
                if(subscribed&&latest.active)ack(latest.control,status.reason,t::Phase::unavailable);
            }
        }
        if(results.consume()) {
            const auto& value=results.reading();
            if(value.connection==latest.connection&&value.host_generation==host_generation.load(std::memory_order_acquire)&&
                latest.active&&value.lease_generation==latest.lease_generation&&value.revision>result_revision&&matches(value.surface.query)&&value.surface.query.query_id<=last_query&&
                (value.reason!=t::Reason::none||t::qualified_footprint(value.surface))) {
                result_revision=value.revision;
                latest_result=value;result_pending=diagnostics_pending=true;status.reason=value.reason;
                for(std::size_t i=0;i<5;++i){const auto& detail=value.details[i];
                    if(latest.capture_active&&detail.capture_generation==latest.capture_generation&&detail.assembly_index==i&&detail.count&&detail.attempts[0].query_start_qpc>=capture_start_qpc){probe_details[i]=detail;probe_pending[i]=true;}}
                const auto& s=value.surface;if(s.query_end_qpc>=s.query_start_qpc)status.query_ticks_max=(std::max)(status.query_ticks_max,static_cast<std::uint64_t>(s.query_end_qpc-s.query_start_qpc));
                if(s.query_end_qpc>=s.query.reference.sample_qpc)status.source_age_ticks_max=(std::max)(status.source_age_ticks_max,static_cast<std::uint64_t>(s.query_end_qpc-s.query.reference.sample_qpc));
                if(value.reason!=t::Reason::none)ack(latest.control,value.reason,t::Phase::unavailable);
            } else {++status.rejected;status.reason=t::Reason::epoch;}
        }
        if(capture_results.consume()) {
            const auto& c=capture_results.reading();
            if(c.connection==latest.connection&&c.host_generation==host_generation.load(std::memory_order_acquire)&&c.lease_generation==latest.lease_generation&&latest.capture_active) {
                if(c.have_render&&c.render.capture_generation==latest.capture_generation&&c.render.sample_qpc>=capture_start_qpc&&matches(c.render.query)){render_detail=c.render;render_pending=true;}
                if(c.have_probe&&c.probe.capture_generation==latest.capture_generation&&c.probe.count&&c.probe.attempts[0].query_start_qpc>=capture_start_qpc&&matches(c.probe.query)) {
                    const auto index=4+static_cast<std::size_t>(c.probe.purpose);if(index>=5&&index<probe_details.size()&&c.probe.assembly_index==t::no_assembly){probe_details[index]=c.probe;probe_pending[index]=true;}
                }
            }
        }
        const auto now=observation_qpc();
        if(latest.active&&now>=latest.lease_expires_qpc) {
            latest.active=false;clear_query();status.reason=t::Reason::expired;ack(latest.control,t::Reason::expired,t::Phase::held);publish();
        }
        report();
    }
    bool next_frame(t::Frame& output,bool& surface_frame,std::uint64_t& generation) noexcept {
        surface_frame=false;generation=0;output_is_capture=false;
        if(ack_count){const auto value=acks[ack_head];ack_head=(ack_head+1)%acks.size();--ack_count;return t::encode(value,++outgoing_sequence,output);}
        if(result_pending) {
            result_pending=false;
            if(latest_result.host_generation!=host_generation.load(std::memory_order_acquire))return false;
            surface_frame=true;generation=latest_result.host_generation;++status.surfaces;
            output_query=latest_result.surface.query;output_qpc=latest_result.surface.query_end_qpc;output_lease=latest_result.lease_generation;
            return t::encode(latest_result.surface,++outgoing_sequence,output);
        }
        if(diagnostics_pending) {
            diagnostics_pending=false;if(latest_result.host_generation!=host_generation.load(std::memory_order_acquire))return false;
            t::Diagnostics d;const auto& s=latest_result.surface;d.query=s.query;d.provider=s.provider;
            d.aircraft_generation=s.aircraft_generation;d.scenery_generation=s.scenery_generation;d.local_frame_generation=s.local_frame_generation;
            // Geometry/state qualification absent: no diagnostic probes or invented heights/timestamps.
            surface_frame=true;generation=latest_result.host_generation;
            output_query=s.query;output_qpc=s.query_end_qpc;output_lease=latest_result.lease_generation;return t::encode(d,++outgoing_sequence,output);
        }
        for(std::size_t i=0;i<probe_pending.size();++i)if(probe_pending[i]) {
            probe_pending[i]=false;const auto& d=probe_details[i];if(!latest.capture_active||d.capture_generation!=latest.capture_generation)continue;
            surface_frame=true;generation=host.generation;output_query=d.query;output_lease=latest.lease_generation;output_capture=d.capture_generation;output_is_capture=true;
            output_qpc=d.attempts[0].query_start_qpc;
            return t::encode(d,++outgoing_sequence,output);
        }
        if(render_pending) {
            render_pending=false;if(!latest.capture_active||render_detail.capture_generation!=latest.capture_generation)return false;
            surface_frame=true;generation=host.generation;output_query=render_detail.query;output_lease=latest.lease_generation;output_capture=render_detail.capture_generation;output_is_capture=true;
            output_qpc=render_detail.sample_qpc;
            return t::encode(render_detail,++outgoing_sequence,output);
        }
        return false;
    }
    void connection(HANDLE pipe) noexcept {
        Operation read,write;std::array<std::uint8_t,4096> buffer{};t::Frame outgoing;
        bool frame_ready=false,surface_frame=false;std::uint64_t write_generation{};std::size_t offset{};std::int64_t write_started{};
        if(!read.event.valid()||!write.event.valid())return;
        framer.reset();subscribed=false;owner=0;last_operation=incoming_sequence=outgoing_sequence=0;ack_head=ack_count=0;clear_query();
        latest={};capture_start_qpc=0;latest.connection=++next_connection;status.connection=latest.connection;status.connected=true;last_io=observation_qpc();publish();
        frame_ready=t::encode(hello,++outgoing_sequence,outgoing);
        while(WaitForSingleObject(stop_event.value,0)==WAIT_TIMEOUT) {
            if(!psx::sync::worker_poll(pipe))break;
            poll_owner();
            if(ack_count==acks.size()){status.reason=t::Reason::overflow;break;}
            const auto now=observation_qpc();
            if(write.pending&&now-write_started>static_cast<std::int64_t>(hello.qpc_frequency*2))break;
            if(surface_frame&&frame_ready&&(write_generation!=host_generation.load(std::memory_order_acquire)||
                !output_valid(now))) {
                if(write.pending||offset)break;frame_ready=false;surface_frame=false;
            }
            if(!subscribed&&now-last_io>static_cast<std::int64_t>(hello.qpc_frequency*2))break;
            bool failed=false;std::size_t serviced{};
            for(unsigned batch=0;batch<8&&serviced<16384;++batch) {
                DWORD count{};
                if(read.pending) {
                    if(WaitForSingleObject(read.event.value,0)!=WAIT_OBJECT_0)break;
                    if(!psx::sync::io_result(pipe,&read.op,&count,FALSE)){failed=true;break;}read.pending=false;
                } else {
                    read.prepare();
                    if(!psx::sync::io_read(pipe,buffer.data(),static_cast<DWORD>(buffer.size()),&count,&read.op)) {
                        if(GetLastError()!=ERROR_IO_PENDING){failed=true;break;}read.pending=true;break;
                    }
                }
                if(!count||!framer.feed(buffer.data(),count,frame,this)){failed=true;break;}serviced+=count;
            }
            if(failed)break;
            if(write.pending&&WaitForSingleObject(write.event.value,0)==WAIT_OBJECT_0) {
                DWORD sent{};if(!psx::sync::io_result(pipe,&write.op,&sent,FALSE)||!sent)break;
                write.pending=false;offset+=sent;if(offset==outgoing.size){frame_ready=false;offset=0;}
            }
            // No unbounded write loop: at most eight already-completing frames per service turn.
            for(unsigned batch=0;batch<8&&!write.pending;++batch) {
                if(!frame_ready){frame_ready=next_frame(outgoing,surface_frame,write_generation);offset=0;if(!frame_ready)break;}
                if(surface_frame&&(write_generation!=host_generation.load(std::memory_order_acquire)||!output_valid(observation_qpc()))) {
                    if(offset){failed=true;break;}frame_ready=false;surface_frame=false;continue;
                }
                write.prepare();DWORD sent{};write_started=observation_qpc();
                if(!psx::sync::io_write(pipe,outgoing.bytes.data()+offset,static_cast<DWORD>(outgoing.size-offset),&sent,&write.op)) {
                    if(GetLastError()!=ERROR_IO_PENDING){failed=true;break;}write.pending=true;break;
                }
                if(!sent){failed=true;break;}offset+=sent;if(offset==outgoing.size){frame_ready=false;offset=0;}
            }
            if(failed)break;
            HANDLE waits[]={stop_event.value,read.event.value,write.event.value};
            WaitForMultipleObjects(3,waits,FALSE,5);
            if(!read.pending)ResetEvent(read.event.value);if(!write.pending)ResetEvent(write.event.value);
        }
        read.cancel(pipe);write.cancel(pipe);latest.active=false;latest.have_query=false;subscribed=false;status.connected=false;
        if(status.reason!=t::Reason::overflow)status.reason=t::Reason::disconnected;result_pending=diagnostics_pending=false;publish();
    }
    void run() noexcept {while(WaitForSingleObject(stop_event.value,0)==WAIT_TIMEOUT){if(psx::sync::enabled()){status.running=true;psx::sync::WorkerIo io(psx::sync::Family::terrain);peer_pid=io.status().peer.pid;connection(nullptr);}else run_local();WaitForSingleObject(stop_event.value,20);}}
    void run_local() noexcept {
        if(endpoint.empty())return;
        try {
            const auto sid=sid_string();const auto acl=L"D:P(A;;GA;;;SY)(A;;GA;;;"+sid+L")";
            PSECURITY_DESCRIPTOR raw{};
            if(sid.empty()||!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(),SDDL_REVISION_1,&raw,nullptr)){status.error=GetLastError();report();return;}
            struct Descriptor {PSECURITY_DESCRIPTOR value;~Descriptor(){LocalFree(value);}} descriptor{raw};
            SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),raw,FALSE};
            Handle pipe(CreateNamedPipeW(endpoint.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,&security));
            if(!pipe.valid()){status.error=GetLastError();report();return;}
            status.running=true;status.reason=t::Reason::none;report();Operation connect;
            if(!connect.event.valid()){status.error=GetLastError();status.running=false;report();return;}
            while(psx::sync::local_io_selected()&&WaitForSingleObject(stop_event.value,0)==WAIT_TIMEOUT) {
                connect.prepare();bool attached=ConnectNamedPipe(pipe.value,&connect.op)!=FALSE;
                if(!attached) {
                    const auto error=GetLastError();
                    if(error==ERROR_PIPE_CONNECTED)attached=true;
                    else if(error==ERROR_IO_PENDING) {
                        connect.pending=true;HANDLE waits[]={stop_event.value,connect.event.value};
                        if(psx::sync::wait_local_connection(stop_event.value,waits[1])==WAIT_OBJECT_0+1) {
                            DWORD bytes{};attached=psx::sync::io_result(pipe.value,&connect.op,&bytes,FALSE)!=FALSE;connect.pending=false;
                        } else {connect.cancel(pipe.value);break;}
                    } else break;
                }
                ULONG pid{};
                if(attached&&GetNamedPipeClientProcessId(pipe.value,&pid)&&pid){peer_pid=pid;connection(pipe.value);}
                DisconnectNamedPipe(pipe.value);
            }
        } catch(...) {status.error=ERROR_NOT_ENOUGH_MEMORY;}
        status.running=false;status.connected=false;status.active=false;status.reason=t::Reason::stopped;report();
    }
};
TerrainService::TerrainService()=default;
TerrainService::~TerrainService(){stop();}
bool TerrainService::start(const TerrainOptions& options,motion::InstanceId provider) {
    stop();if(!motion::valid(provider))return false;
    auto next=std::make_unique<Impl>();next->endpoint=options.endpoint.empty()?terrain_endpoint():options.endpoint;
    if(!next->stop_event.valid()||(next->endpoint.empty()?!psx::sync::runtime():next->endpoint.rfind(L"\\\\.\\pipe\\",0)!=0))return false;
    next->hello={t::Role::plugin,provider,GetCurrentProcessId(),t::surface_query|t::probe_details|t::render_details|t::footprint_surface,creation_time(),observation_qpc_frequency()};
    if(!next->hello.process_creation_filetime)return false;
    if(options.diagnostics)next->events=options.diagnostics->producer(3,diagnostics::Component::terrain);
    auto* raw=next.get();raw->worker=std::thread([raw]{raw->run();});impl_=std::move(next);return true;
}
void TerrainService::stop() noexcept {impl_.reset();empty_={};}
const TerrainInput& TerrainService::input() noexcept {if(!impl_)return empty_;impl_->incoming.consume();return impl_->incoming.reading();}
void TerrainService::invalidate(const TerrainHost& host) noexcept {
    if(!impl_)return;impl_->host_generation.store(host.generation,std::memory_order_release);impl_->hosts.writing()=host;impl_->hosts.publish();
}
void TerrainService::publish(const TerrainResult& result,const TerrainHost& host) noexcept {
    if(!impl_)return;invalidate(host);impl_->results.writing()=result;impl_->results.publish();
}
void TerrainService::publish_capture(const TerrainCaptureResult& result) noexcept {if(!impl_)return;impl_->capture_results.writing()=result;impl_->capture_results.publish();}
bool TerrainService::poll(TerrainServiceStatus& status) noexcept {if(!impl_||!impl_->statuses.consume())return false;status=impl_->statuses.reading();return true;}
}
