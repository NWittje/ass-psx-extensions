#include <psx/sync/worker_io.hpp>
#include "psx_plugin/environment_service.hpp"
#include "psx_plugin/observation_client.hpp"
#include "psx_plugin/triple_buffer.hpp"
#include "psx_plugin/motion_support.hpp"
#include <Windows.h>
#include <sddl.h>
#include <thread>

namespace psx::xp {
namespace {
struct Handle {HANDLE value{INVALID_HANDLE_VALUE};Handle()=default;explicit Handle(HANDLE h):value(h){}
    ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}bool valid() const noexcept{return value&&value!=INVALID_HANDLE_VALUE;}};
std::wstring sid() {
    Handle token;token.value=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value))return {};
    alignas(TOKEN_USER) std::array<std::byte,sizeof(TOKEN_USER)+SECURITY_MAX_SID_SIZE> bytes{};DWORD length{};
    if(!GetTokenInformation(token.value,TokenUser,bytes.data(),static_cast<DWORD>(bytes.size()),&length))return {};
    LPWSTR raw{};if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&raw))return {};
    const std::wstring result(raw);LocalFree(raw);return result;
}
std::uint64_t creation_time() noexcept {
    FILETIME c{},e{},k{},u{};if(!GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u))return 0;
    return (static_cast<std::uint64_t>(c.dwHighDateTime)<<32)|c.dwLowDateTime;
}
struct Operation {Handle event{CreateEventW(nullptr,TRUE,FALSE,nullptr)};OVERLAPPED op{};bool pending{};
    void prepare() noexcept {ResetEvent(event.value);op={};op.hEvent=event.value;}
    void cancel(HANDLE pipe) noexcept {if(pending){psx::sync::io_cancel(pipe,&op);DWORD n{};psx::sync::io_result(pipe,&op,&n,TRUE);pending=false;}}
};
}
std::wstring environment_endpoint(){const auto user=sid();if(user.empty())return {};const std::string prefix=envp::plugin_pipe_prefix;return std::wstring(prefix.begin(),prefix.end())+user;}
struct EnvironmentService::Impl {
    Handle stop{CreateEventW(nullptr,TRUE,FALSE,nullptr)};std::thread worker;std::wstring endpoint,legacy_endpoint,modern_endpoint,fourth_endpoint;env::Hello hello{};bool presentation_transport{},third_transport{},fourth_transport{};
    std::atomic<bool> connected{},fault{};std::atomic<std::uint64_t> connection{};
    std::atomic<std::uint64_t> capture_generation{};std::uint64_t last_capture_generation{};
    diagnostics::DiagnosticProducer events;std::array<bool,2> encoding_failed{};std::int64_t last_encoding_trace{};
    SpscFifo<EnvironmentCommand,env::ordered_capacity> controls;
    struct Reply {env::Ack ack{};std::uint64_t connection{};};
    SpscFifo<Reply,env::ordered_capacity> replies;
    TripleBuffer<EnvironmentSample> samples;
    std::array<TripleBuffer<envp::AppliedPresentation>,2> applied;
    std::array<env::Control,2> claims{};
    env::Sample previous{};envp::Context previous_context{};std::uint64_t previous_selection_revision{};bool have_previous{};
    std::array<std::uint64_t,2> revisions{};
    std::array<std::int64_t,2> claim_floors{};
    std::uint64_t next_revision{},last_operation{},owner{};
    std::uint32_t peer_pid{},in_sequence{},out_sequence{};bool subscribed{};
    env::Framer framer;envp::Framer presentation_framer;std::array<envp::AppliedPresentation,2> latest_applied{};std::array<bool,2> applied_pending{};
    unsigned next_group{},output_group{2};std::uint64_t output_revision{};
    ~Impl(){SetEvent(stop.value);if(worker.joinable())worker.join();}
    template<class T>bool decode(const std::uint8_t* data,std::size_t length,T& value) noexcept {
        return presentation_transport?envp::decode({data,length},value):env::decode(data,length,value);
    }
    template<class T>bool encode(const T& value,std::uint32_t sequence,env::Frame& frame) noexcept {
        return presentation_transport?envp::encode(value,sequence,frame):env::encode(value,sequence,frame);
    }
    static bool frame(void* context,const env::Header& header,const std::uint8_t* data,std::size_t length) noexcept {
        return frame_impl(context,static_cast<unsigned>(header.kind),header.sequence,data,length);
    }
    static bool presentation_frame(void* context,const envp::Header& header,const std::uint8_t* data,std::size_t length) noexcept {
        return frame_impl(context,static_cast<unsigned>(header.kind),header.sequence,data,length);
    }
    static bool frame_impl(void* context,unsigned kind,std::uint32_t sequence,const std::uint8_t* data,std::size_t length) noexcept {
        auto& self=*static_cast<Impl*>(context);
        if(!sequence||sequence<=self.in_sequence)return false;self.in_sequence=sequence;
        if(kind==static_cast<unsigned>(env::Kind::subscribe)){env::Subscribe value;
            if(self.subscribed||!self.decode(data,length,value)||value.instance!=self.hello.instance||value.pid!=self.peer_pid||
                value.qpc_frequency!=self.hello.qpc_frequency||!value.owner)return false;
            self.subscribed=true;self.owner=value.owner;return true;}
        if(!self.subscribed)return false;
        if(kind==static_cast<unsigned>(env::Kind::capture)){env::Capture value;if(!self.decode(data,length,value)||value.owner!=self.owner)return false;
            if(value.generation>=self.last_capture_generation){self.last_capture_generation=value.generation;self.capture_generation.store(value.enabled?value.generation:0,std::memory_order_release);}return true;}
        if(kind==static_cast<unsigned>(env::Kind::control)){env::Control c;
            if(!self.decode(data,length,c)||c.owner!=self.owner||c.operation<=self.last_operation)return false;
            self.last_operation=c.operation;const auto revision=++self.next_revision;
            for(unsigned g=0;g<2;++g)if(c.groups&(1u<<g)) {
                auto& old=self.claims[g];
                if(c.action==env::Action::renew&&(old.owner!=c.owner||old.producer!=c.producer||
                    old.lifecycle!=c.lifecycle||c.configuration<old.configuration))return false;
                if(c.action==env::Action::release&&old.owner&&
                    (old.producer!=c.producer||old.lifecycle!=c.lifecycle||c.configuration<old.configuration))return false;
                if(c.action==env::Action::acquire||c.action==env::Action::release){self.revisions[g]=revision;
                    self.claim_floors[g]=observation_qpc();self.applied_pending[g]=false;}
                old=c;
            }
            if(!self.controls.push({c,self.connection.load(std::memory_order_relaxed),revision,observation_qpc()}))return false;
            return true;
        }
        if((!self.presentation_transport&&kind==static_cast<unsigned>(env::Kind::sample))||
           (self.presentation_transport&&kind==static_cast<unsigned>(envp::Kind::presentation))){env::Sample value;
            envp::Presentation presentation;
            if(self.presentation_transport){if(!envp::decode({data,length},presentation)||presentation.visibility.owner!=self.owner)return false;
                value=presentation.source.sample;
            }else if(!env::decode(data,length,value)||!env::valid(value))return false;
            bool wanted=false;for(const auto& c:self.claims)if(c.action!=env::Action::release&&c.owner&&c.producer==value.producer&&c.lifecycle==value.lifecycle)wanted=true;
            if(!wanted)return true;
            if(self.have_previous&&self.previous.producer==value.producer&&self.previous.lifecycle==value.lifecycle){
                if(value.configuration<self.previous.configuration)return true;
                const auto& old=self.previous;if(value.sequence<old.sequence||value.received_qpc<old.received_qpc)return false;
                if(self.presentation_transport&&presentation.visibility.selection_revision<self.previous_selection_revision)return false;
                if(value.sequence==old.sequence){
                    if(!envp::same_sample(old,value))return false;
                    if(!self.presentation_transport)return true;
                    if(presentation.source.context!=self.previous_context)return false;
                }
            }
            const auto now=observation_qpc();if(value.received_qpc>now||value.sample_end_qpc>now||
                (self.presentation_transport&&presentation.source.context.observation_end_qpc>now))return false;
            self.previous=value;self.have_previous=true;
            auto& output=self.samples.writing();output={value,self.connection.load(std::memory_order_relaxed),self.revisions};
            if(self.presentation_transport){output.context=presentation.source.context;output.local_visibility=presentation.source.local_visibility;output.visibility=presentation.visibility;
                output.presentation_revision=sequence;output.presentation_available=true;output.weather=presentation.weather;self.previous_context=output.context;self.previous_selection_revision=output.visibility.selection_revision;}
            self.samples.publish();return true;
        }
        if(kind==static_cast<unsigned>(env::Kind::status)){env::Status value;
            if(!self.decode(data,length,value))return false;
            for(unsigned g=0;g<2;++g)if(!(value.groups&(1u<<g))&&self.claims[g].owner&&self.claims[g].action!=env::Action::release) {
                auto c=self.claims[g];if(c.producer!=value.producer||c.lifecycle!=value.lifecycle||value.configuration<c.configuration)continue;
                c.action=env::Action::release;c.groups=1u<<g;self.claims[g]=c;
                self.revisions[g]=++self.next_revision;
                if(!self.controls.push({c,self.connection.load(std::memory_order_relaxed),self.revisions[g],observation_qpc(),true}))return false;
            }
            return true;
        }
        return false;
    }
    bool next(env::Frame& frame) noexcept {
        output_group=2;
        Reply reply;for(unsigned i=0;i<env::ordered_capacity&&replies.pop(reply);++i)
            if(reply.connection==connection.load(std::memory_order_acquire)&&reply.ack.control.owner==owner){
                if(!++out_sequence){fault.store(true,std::memory_order_release);return false;}
                return encode(reply.ack,out_sequence,frame);
            }
        for(unsigned g=0;g<2;++g)if(applied[g].consume()){latest_applied[g]=applied[g].reading();applied_pending[g]=true;}
        for(unsigned n=0;n<2;++n){const auto g=(next_group+n)%2;if(applied_pending[g]) {
            applied_pending[g]=false;const auto& envelope=latest_applied[g];const auto& a=envelope.applied;const auto& c=claims[g];
            if(a.owner!=owner||a.source.producer!=c.producer||(envelope.weather_target?envelope.current_lifecycle:a.source.lifecycle)!=c.lifecycle||a.applied_qpc<claim_floors[g])continue;
            if(c.action==env::Action::release&&a.phase==env::Phase::owned)continue;
            output_group=g;output_revision=revisions[g];next_group=(g+1)%2;
            if(!++out_sequence){fault.store(true,std::memory_order_release);return false;}
            const auto begin=observation_qpc();const bool encoded=presentation_transport?envp::encode(envelope,out_sequence,frame):env::encode(a,out_sequence,frame);const auto end=observation_qpc();
            if(capture_generation.load(std::memory_order_acquire)&&(!last_encoding_trace||end-last_encoding_trace>=static_cast<std::int64_t>(hello.qpc_frequency/4)||end-begin>static_cast<std::int64_t>(hello.qpc_frequency/10))){
                last_encoding_trace=end;events.emit(diagnostics::Code::message,diagnostics::Severity::info,{},diagnostics::ErrorDomain::none,0,"environment_report_encode_v1",
                    {static_cast<std::uint64_t>(begin),static_cast<std::uint64_t>(end),a.source.sequence,envelope.weather_target,static_cast<std::uint64_t>(a.groups),encoded});}
            if(!encoded&&!encoding_failed[g])events.emit(diagnostics::Code::frame_rejected,diagnostics::Severity::warning,{},diagnostics::ErrorDomain::protocol,ERROR_INVALID_DATA,
                "environment Applied encoding rejected",{g,a.source.sequence,envelope.weather_target,a.accepted_mask,a.actual_mask,a.location_mask});
            if(encoded&&encoding_failed[g])events.emit(diagnostics::Code::synchronized,diagnostics::Severity::info,{},diagnostics::ErrorDomain::none,0,"environment Applied encoding recovered",{g,a.source.sequence,envelope.weather_target,0,0,0});
            encoding_failed[g]=!encoded;return encoded;
        }}
        return false;
    }
    void serve(HANDLE pipe) noexcept {
        Operation read,write;std::array<std::uint8_t,4096> incoming{};env::Frame outgoing;
        if(!read.event.valid()||!write.event.valid())return;
        subscribed=false;owner=0;in_sequence=out_sequence=0;last_operation=0;claims={};claim_floors={};have_previous={};previous_context={};previous_selection_revision=0;applied_pending={};next_group=0;output_group=2;framer.reset();presentation_framer.reset();capture_generation.store(0,std::memory_order_release);last_capture_generation=0;
        fault.store(false,std::memory_order_release);connection.fetch_add(1,std::memory_order_acq_rel);connected.store(true,std::memory_order_release);
        bool ready=presentation_transport?envp::encode(envp::Hello{hello,envp::visibility_presentation|(third_transport?envp::manual_weather:0u)|(fourth_transport?envp::local_visibility_target:0u)},++out_sequence,outgoing):env::encode(hello,++out_sequence,outgoing);std::size_t offset{};
        const auto started=observation_qpc();std::int64_t write_started{};
        while(WaitForSingleObject(stop.value,0)==WAIT_TIMEOUT&&!fault.load(std::memory_order_acquire)) {
            if(!psx::sync::worker_poll(pipe))break;
            if(!subscribed&&observation_qpc()-started>static_cast<std::int64_t>(hello.qpc_frequency*2))break;
            if(write.pending&&observation_qpc()-write_started>static_cast<std::int64_t>(hello.qpc_frequency*2))break;
            bool failed=false;
            for(unsigned batch=0;batch<8;++batch){DWORD count{};
                if(read.pending){if(WaitForSingleObject(read.event.value,0)!=WAIT_OBJECT_0)break;
                    if(!psx::sync::io_result(pipe,&read.op,&count,FALSE)){failed=true;break;}read.pending=false;
                } else {read.prepare();if(!psx::sync::io_read(pipe,incoming.data(),static_cast<DWORD>(incoming.size()),&count,&read.op)){
                    if(GetLastError()!=ERROR_IO_PENDING){failed=true;break;}read.pending=true;break;}}
                if(!count||!(presentation_transport?presentation_framer.feed(incoming.data(),count,presentation_frame,this):framer.feed(incoming.data(),count,frame,this))){failed=true;break;}
            }
            if(failed)break;
            // Superseded replaceable reports cannot delay a release/acquire boundary.
            // A partially written obsolete frame requires disconnect rather than byte-stream surgery.
            if(ready&&output_group<2&&output_revision!=revisions[output_group]) {
                if(offset||write.pending)break;ready=false;
            }
            if(write.pending&&WaitForSingleObject(write.event.value,0)==WAIT_OBJECT_0){DWORD sent{};
                if(!psx::sync::io_result(pipe,&write.op,&sent,FALSE)||!sent)break;write.pending=false;offset+=sent;
                if(offset==outgoing.size){ready=false;offset=0;}}
            for(unsigned batch=0;batch<8&&!write.pending;++batch){
                if(!ready){ready=next(outgoing);offset=0;if(!ready)break;}
                write.prepare();DWORD sent{};write_started=observation_qpc();
                if(!psx::sync::io_write(pipe,outgoing.bytes.data()+offset,static_cast<DWORD>(outgoing.size-offset),&sent,&write.op)){
                    if(GetLastError()!=ERROR_IO_PENDING){failed=true;break;}write.pending=true;break;}
                if(!sent){failed=true;break;}offset+=sent;if(offset==outgoing.size){ready=false;offset=0;}
            }
            if(failed)break;HANDLE waits[]{stop.value,read.event.value,write.event.value};WaitForMultipleObjects(3,waits,FALSE,5);
            if(!read.pending)ResetEvent(read.event.value);if(!write.pending)ResetEvent(write.event.value);
        }
        connected.store(false,std::memory_order_release);read.cancel(pipe);write.cancel(pipe);
    }
    void run() noexcept {while(WaitForSingleObject(stop.value,0)==WAIT_TIMEOUT){if(psx::sync::enabled()){presentation_transport=true;third_transport=true;fourth_transport=true;psx::sync::WorkerIo io(psx::sync::Family::environment);peer_pid=io.status().peer.pid;serve(nullptr);}else run_local();WaitForSingleObject(stop.value,20);}}
    void run_local() noexcept {
        if(endpoint.empty())return;
        try {
            const auto user=sid();const auto acl=L"D:P(A;;GA;;;SY)(A;;GA;;;"+user+L")";PSECURITY_DESCRIPTOR raw{};
            if(user.empty()||!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(),SDDL_REVISION_1,&raw,nullptr))return;
            struct Descriptor {PSECURITY_DESCRIPTOR p;~Descriptor(){LocalFree(p);}} descriptor{raw};
            SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES),raw,FALSE};
            std::array<Handle,4> pipes;std::array<Operation,4> accepts;
            const std::array<const std::wstring*,4> endpoints{&fourth_endpoint,&modern_endpoint,&endpoint,&legacy_endpoint};
            for(unsigned i=0;i<4;++i){pipes[i].value=CreateNamedPipeW(endpoints[i]->c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,&attributes);
                if(!pipes[i].valid()||!accepts[i].event.valid())return;}
            while(psx::sync::local_io_selected()&&WaitForSingleObject(stop.value,0)==WAIT_TIMEOUT) {
                unsigned selected=4;bool failed=false;
                for(unsigned i=0;i<4;++i){auto& a=accepts[i];a.prepare();
                    if(ConnectNamedPipe(pipes[i].value,&a.op)){if(selected==4)selected=i;}
                    else {const auto error=GetLastError();if(error==ERROR_PIPE_CONNECTED){if(selected==4)selected=i;}
                        else if(error==ERROR_IO_PENDING)a.pending=true;else {failed=true;break;}}}
                if(!failed&&selected==4){HANDLE waits[]{stop.value,accepts[0].event.value,accepts[1].event.value,accepts[2].event.value,accepts[3].event.value};
                    DWORD result=WAIT_TIMEOUT;while(psx::sync::local_io_selected()&&(result=WaitForMultipleObjects(5,waits,FALSE,20))==WAIT_TIMEOUT){}
                    if(result>=WAIT_OBJECT_0+1&&result<=WAIT_OBJECT_0+4){selected=result-WAIT_OBJECT_0-1;DWORD bytes{};
                        if(!psx::sync::io_result(pipes[selected].value,&accepts[selected].op,&bytes,FALSE))failed=true;
                        accepts[selected].pending=false;
                    }else failed=true;}
                // One authenticated connection owns both leases. The alternate
                // protocol listener shares this worker and never consumes source.
                for(unsigned i=0;i<4;++i)accepts[i].cancel(pipes[i].value);
                if(failed)break;
                presentation_transport=selected!=3;third_transport=selected<=1;fourth_transport=selected==0;ULONG pid{};
                if(GetNamedPipeClientProcessId(pipes[selected].value,&pid)&&pid){peer_pid=pid;serve(pipes[selected].value);}
                for(auto& pipe:pipes)DisconnectNamedPipe(pipe.value);
            }
        }catch(...){}connected.store(false,std::memory_order_release);
    }
};
EnvironmentService::EnvironmentService()=default;EnvironmentService::~EnvironmentService(){stop();}
bool EnvironmentService::start(motion::InstanceId provider,const std::wstring& endpoint,diagnostics::DiagnosticsHub* journal) {
    stop();if(!motion::valid(provider))return false;auto next=std::make_unique<Impl>();
    next->endpoint=endpoint.empty()?environment_endpoint():endpoint;
    if(journal)next->events=journal->producer(4,diagnostics::Component::sdk);
    if(endpoint.empty()){const auto user=sid();const std::string prefix=env::plugin_pipe_prefix;next->legacy_endpoint=std::wstring(prefix.begin(),prefix.end())+user;}
    else next->legacy_endpoint=endpoint+L".v1";
    next->modern_endpoint=next->endpoint;const auto modern=next->modern_endpoint.find(L".v2.");if(modern!=std::wstring::npos)next->modern_endpoint.replace(modern,4,L".v3.");else next->modern_endpoint+=L".v3";
    next->fourth_endpoint=next->modern_endpoint;const auto fourth=next->fourth_endpoint.find(L".v3.");if(fourth!=std::wstring::npos)next->fourth_endpoint.replace(fourth,4,L".v4.");else next->fourth_endpoint=next->endpoint+L".v4";
    if((next->endpoint.empty()&&!psx::sync::runtime())||!next->stop.valid())return false;
    next->hello={env::Role::plugin,provider,GetCurrentProcessId(),env::all_groups,creation_time(),observation_qpc_frequency()};
    if(!next->hello.process_creation_filetime)return false;auto* raw=next.get();raw->worker=std::thread([raw]{raw->run();});impl_=std::move(next);return true;
}
void EnvironmentService::stop() noexcept {impl_.reset();}
bool EnvironmentService::connected() const noexcept {return impl_&&impl_->connected.load(std::memory_order_acquire)&&!impl_->fault.load(std::memory_order_acquire);}
std::uint64_t EnvironmentService::connection() const noexcept {return connected()?impl_->connection.load(std::memory_order_acquire):0;}
std::uint64_t EnvironmentService::capture_generation() const noexcept {return connected()?impl_->capture_generation.load(std::memory_order_acquire):0;}
bool EnvironmentService::command(EnvironmentCommand& output) noexcept {
    if(!impl_)return false;for(unsigned i=0;i<env::ordered_capacity;++i)if(impl_->controls.pop(output)){
        if(output.connection==impl_->connection.load(std::memory_order_acquire))return true;
    }else break;return false;
}
bool EnvironmentService::sample(EnvironmentSample& output) noexcept {
    if(!impl_||!impl_->samples.consume())return false;output=impl_->samples.reading();
    return output.connection==impl_->connection.load(std::memory_order_acquire);
}
void EnvironmentService::ack(const env::Ack& ack,std::uint64_t connection) noexcept {if(impl_&&!impl_->replies.push({ack,connection}))impl_->fault.store(true,std::memory_order_release);}
void EnvironmentService::publish(const std::array<env::Applied,2>& values,const std::array<envp::VisibilityPolicy,2>& policies,const envp::AppliedPresentation& weather) noexcept {
    if(!impl_)return;for(unsigned g=0;g<2;++g){impl_->applied[g].writing()=g==1&&weather.weather_target?weather:envp::AppliedPresentation{values[g],policies[g]};impl_->applied[g].publish();}
}
}
