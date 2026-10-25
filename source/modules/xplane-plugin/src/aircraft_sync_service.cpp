#include <psx/sync/worker_io.hpp>
#include "psx_plugin/aircraft_sync_service.hpp"
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
std::wstring aircraft_sync_endpoint(){const auto user=sid();if(user.empty())return {};const std::string prefix=aircraft::plugin_pipe_prefix;return std::wstring(prefix.begin(),prefix.end())+user;}
struct AircraftService::Impl {
    Handle stop{CreateEventW(nullptr,TRUE,FALSE,nullptr)};std::thread worker;std::wstring endpoint;aircraft::Hello hello{};
    std::atomic<bool> connected{},fault{};std::atomic<std::uint64_t> connection{};
    SpscFifo<AircraftCommand,aircraft::ordered_capacity> controls;
    struct Reply {aircraft::Ack ack{};std::uint64_t connection{};};
    SpscFifo<Reply,aircraft::ordered_capacity> replies;
    std::array<TripleBuffer<AircraftSample>,6> samples;
    std::array<TripleBuffer<aircraft::Applied>,6> applied;
    std::array<aircraft::Control,6> claims{};
    std::array<aircraft::Sample,6> previous{};
    std::array<bool,6> have_previous{};
    std::array<std::uint64_t,6> revisions{};
    std::array<std::int64_t,6> claim_floors{};
    std::uint64_t next_revision{},last_operation{},owner{};
    std::uint32_t peer_pid{},in_sequence{},out_sequence{};bool subscribed{};
    aircraft::Framer framer;std::array<aircraft::Applied,6> latest_applied{};std::array<bool,6> applied_pending{};
    unsigned next_group{},output_group{6};std::uint64_t output_revision{};
    ~Impl(){SetEvent(stop.value);if(worker.joinable())worker.join();}
    static bool frame(void* context,const aircraft::Header& header,const std::uint8_t* data,std::size_t length) noexcept {
        auto& self=*static_cast<Impl*>(context);
        if(!header.sequence||header.sequence<=self.in_sequence)return false;self.in_sequence=header.sequence;
        if(header.kind==aircraft::Kind::subscribe){aircraft::Subscribe value;
            if(self.subscribed||!aircraft::decode(data,length,value)||value.instance!=self.hello.instance||value.pid!=self.peer_pid||
                value.qpc_frequency!=self.hello.qpc_frequency||!value.owner)return false;
            self.subscribed=true;self.owner=value.owner;return true;}
        if(!self.subscribed)return false;
        if(header.kind==aircraft::Kind::capture){aircraft::Capture value;return aircraft::decode(data,length,value)&&value.owner==self.owner;}
        if(header.kind==aircraft::Kind::control){aircraft::Control c;
            if(!aircraft::decode(data,length,c)||c.owner!=self.owner||c.operation<=self.last_operation)return false;
            self.last_operation=c.operation;const auto revision=++self.next_revision;
            for(unsigned g=0;g<6;++g)if(c.groups&(1u<<g)) {
                auto& old=self.claims[g];
                if(c.action==aircraft::Action::renew&&(old.owner!=c.owner||old.producer!=c.producer||
                    old.lifecycle!=c.lifecycle||old.configuration!=c.configuration))return false;
                if(c.action==aircraft::Action::release&&old.owner&&
                    (old.producer!=c.producer||old.lifecycle!=c.lifecycle||old.configuration!=c.configuration))return false;
                if(c.action==aircraft::Action::acquire||c.action==aircraft::Action::release){self.revisions[g]=revision;self.have_previous[g]=false;
                    self.claim_floors[g]=observation_qpc();self.applied_pending[g]=false;}
                old=c;
            }
            if(!self.controls.push({c,self.connection.load(std::memory_order_relaxed),revision,observation_qpc()}))return false;
            return true;
        }
        if(header.kind==aircraft::Kind::sample){aircraft::Sample value;
            if(!aircraft::decode(data,length,value)||!aircraft::valid(value))return false;const auto g=static_cast<unsigned>(value.group);
            const auto& c=self.claims[g];if(c.action==aircraft::Action::release||!c.owner||c.producer!=value.producer||c.lifecycle!=value.lifecycle||c.configuration!=value.configuration)return true;
            if(self.have_previous[g]){
                const auto& old=self.previous[g];if(value.sequence<old.sequence||value.received_qpc<old.received_qpc)return false;
                if(value.sequence==old.sequence){aircraft::Frame a,b;
                    if(!aircraft::encode(old,1,a)||!aircraft::encode(value,1,b)||a.size!=b.size||a.bytes!=b.bytes)return false;return true;}
            }
            const auto now=psx::sync::freshness_now(observation_qpc());if(value.received_qpc>now||value.sample_end_qpc>now)return false;
            self.previous[g]=value;self.have_previous[g]=true;
            self.samples[g].writing()={value,self.connection.load(std::memory_order_relaxed),self.revisions[g]};self.samples[g].publish();return true;
        }
        if(header.kind==aircraft::Kind::status){aircraft::Status value;
            if(!aircraft::decode(data,length,value))return false;
            for(unsigned g=0;g<6;++g)if(!(value.groups&(1u<<g))&&self.claims[g].owner&&self.claims[g].action!=aircraft::Action::release) {
                auto c=self.claims[g];if(c.producer!=value.producer||c.lifecycle!=value.lifecycle||c.configuration!=value.configuration)continue;
                c.action=aircraft::Action::release;c.groups=1u<<g;self.claims[g]=c;
                self.revisions[g]=++self.next_revision;self.have_previous[g]=false;
                if(!self.controls.push({c,self.connection.load(std::memory_order_relaxed),self.revisions[g],observation_qpc(),true}))return false;
            }
            return true;
        }
        return false;
    }
    bool next(aircraft::Frame& frame) noexcept {
        output_group=6;
        Reply reply;for(unsigned i=0;i<aircraft::ordered_capacity&&replies.pop(reply);++i)
            if(reply.connection==connection.load(std::memory_order_acquire)&&reply.ack.control.owner==owner)
                return aircraft::encode(reply.ack,++out_sequence,frame);
        for(unsigned g=0;g<6;++g)if(applied[g].consume()){latest_applied[g]=applied[g].reading();applied_pending[g]=true;}
        for(unsigned n=0;n<6;++n){const auto g=(next_group+n)%6;if(applied_pending[g]) {
            applied_pending[g]=false;const auto& a=latest_applied[g];const auto& c=claims[g];
            if(a.owner!=owner||a.producer!=c.producer||a.lifecycle!=c.lifecycle||a.configuration!=c.configuration||a.applied_qpc<claim_floors[g])continue;
            if(c.action==aircraft::Action::release&&a.phase==aircraft::Phase::owned)continue;
            output_group=g;output_revision=revisions[g];next_group=(g+1)%6;
            return aircraft::encode(a,++out_sequence,frame);
        }}
        return false;
    }
    void serve(HANDLE pipe) noexcept {
        Operation read,write;std::array<std::uint8_t,4096> incoming{};aircraft::Frame outgoing;
        if(!read.event.valid()||!write.event.valid())return;
        subscribed=false;owner=0;in_sequence=out_sequence=0;last_operation=0;claims={};claim_floors={};have_previous={};applied_pending={};next_group=0;output_group=6;framer.reset();
        fault.store(false,std::memory_order_release);connection.fetch_add(1,std::memory_order_acq_rel);connected.store(true,std::memory_order_release);
        bool ready=aircraft::encode(hello,++out_sequence,outgoing);std::size_t offset{};
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
                if(!count||!framer.feed(incoming.data(),count,frame,this)){failed=true;break;}
            }
            if(failed)break;
            // Superseded replaceable reports cannot delay a release/acquire boundary.
            // A partially written obsolete frame requires disconnect rather than byte-stream surgery.
            if(ready&&output_group<6&&output_revision!=revisions[output_group]) {
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
    void run() noexcept {while(WaitForSingleObject(stop.value,0)==WAIT_TIMEOUT){if(psx::sync::enabled()){psx::sync::WorkerIo io(psx::sync::Family::aircraft);peer_pid=io.status().peer.pid;serve(nullptr);}else run_local();WaitForSingleObject(stop.value,20);}}
    void run_local() noexcept {
        if(endpoint.empty())return;
        try {
            const auto user=sid();const auto acl=L"D:P(A;;GA;;;SY)(A;;GA;;;"+user+L")";PSECURITY_DESCRIPTOR raw{};
            if(user.empty()||!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(),SDDL_REVISION_1,&raw,nullptr))return;
            struct Descriptor {PSECURITY_DESCRIPTOR p;~Descriptor(){LocalFree(p);}} descriptor{raw};
            SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES),raw,FALSE};
            Handle pipe{CreateNamedPipeW(endpoint.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,&attributes)};
            if(!pipe.valid())return;Operation connect;if(!connect.event.valid())return;
            while(psx::sync::local_io_selected()&&WaitForSingleObject(stop.value,0)==WAIT_TIMEOUT) {
                connect.prepare();bool attached=ConnectNamedPipe(pipe.value,&connect.op)!=FALSE;
                if(!attached){const auto error=GetLastError();if(error==ERROR_PIPE_CONNECTED)attached=true;
                    else if(error==ERROR_IO_PENDING){connect.pending=true;HANDLE waits[]{stop.value,connect.event.value};
                        if(psx::sync::wait_local_connection(stop.value,waits[1])==WAIT_OBJECT_0+1){DWORD bytes{};
                            attached=psx::sync::io_result(pipe.value,&connect.op,&bytes,FALSE)!=FALSE;connect.pending=false;
                        }else{connect.cancel(pipe.value);break;}}else break;}
                ULONG pid{};if(attached&&GetNamedPipeClientProcessId(pipe.value,&pid)&&pid){peer_pid=pid;serve(pipe.value);}
                DisconnectNamedPipe(pipe.value);
            }
        }catch(...){}connected.store(false,std::memory_order_release);
    }
};
AircraftService::AircraftService()=default;AircraftService::~AircraftService(){stop();}
bool AircraftService::start(motion::InstanceId provider,const std::wstring& endpoint) {
    stop();if(!motion::valid(provider))return false;auto next=std::make_unique<Impl>();
    next->endpoint=endpoint.empty()?aircraft_sync_endpoint():endpoint;if((next->endpoint.empty()&&!psx::sync::runtime())||!next->stop.valid())return false;
    next->hello={aircraft::Role::plugin,provider,GetCurrentProcessId(),aircraft::all_groups,creation_time(),observation_qpc_frequency()};
    if(!next->hello.process_creation_filetime)return false;auto* raw=next.get();raw->worker=std::thread([raw]{raw->run();});impl_=std::move(next);return true;
}
void AircraftService::stop() noexcept {impl_.reset();}
bool AircraftService::connected() const noexcept {return impl_&&impl_->connected.load(std::memory_order_acquire)&&!impl_->fault.load(std::memory_order_acquire);}
std::uint64_t AircraftService::connection() const noexcept {return connected()?impl_->connection.load(std::memory_order_acquire):0;}
bool AircraftService::command(AircraftCommand& output) noexcept {
    if(!impl_)return false;for(unsigned i=0;i<aircraft::ordered_capacity;++i)if(impl_->controls.pop(output)){
        if(output.connection==impl_->connection.load(std::memory_order_acquire))return true;
    }else break;return false;
}
bool AircraftService::sample(unsigned group,AircraftSample& output) noexcept {
    if(!impl_||group>=6||!impl_->samples[group].consume())return false;output=impl_->samples[group].reading();
    return output.connection==impl_->connection.load(std::memory_order_acquire);
}
void AircraftService::ack(const aircraft::Ack& ack,std::uint64_t connection) noexcept {if(impl_&&!impl_->replies.push({ack,connection}))impl_->fault.store(true,std::memory_order_release);}
void AircraftService::publish(const std::array<aircraft::Applied,6>& values) noexcept {
    if(!impl_)return;for(unsigned g=0;g<6;++g){impl_->applied[g].writing()=values[g];impl_->applied[g].publish();}
}
}
