#include <psx/sync/worker_io.hpp>
#include "psx_plugin/status_service.hpp"
#include "psx_plugin/triple_buffer.hpp"
#include "psx_plugin/observation_client.hpp"
#include "psx_plugin/motion_support.hpp"
#include <Windows.h>
#include <sddl.h>
#include <thread>

namespace psx::xp {
namespace {
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    Handle()=default; explicit Handle(HANDLE v):value(v){}
    ~Handle(){if(valid())CloseHandle(value);}
    bool valid() const noexcept {return value&&value!=INVALID_HANDLE_VALUE;}
};
bool token_user(HANDLE process,std::array<std::byte,sizeof(TOKEN_USER)+SECURITY_MAX_SID_SIZE>& bytes) noexcept {
    Handle token; token.value=nullptr; DWORD n{};
    return OpenProcessToken(process,TOKEN_QUERY,&token.value)&&
        GetTokenInformation(token.value,TokenUser,bytes.data(),static_cast<DWORD>(bytes.size()),&n);
}
std::uint64_t birth(HANDLE process) noexcept {
    FILETIME c{},e{},k{},u{}; if(!GetProcessTimes(process,&c,&e,&k,&u))return 0;
    return (static_cast<std::uint64_t>(c.dwHighDateTime)<<32)|c.dwLowDateTime;
}
struct Operation {
    Handle event{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    OVERLAPPED ov{}; bool pending{}; DWORD done{}; HANDLE bound_pipe{};
    ~Operation(){if(pending)cancel(bound_pipe);}
    bool begin(HANDLE pipe,bool writing,void* bytes,DWORD n) noexcept {
        bound_pipe=pipe;ResetEvent(event.value);ov={};ov.hEvent=event.value;done=0;
        const auto ok=writing?psx::sync::io_write(pipe,bytes,n,&done,&ov):psx::sync::io_read(pipe,bytes,n,&done,&ov);
        pending=!ok&&GetLastError()==ERROR_IO_PENDING;return ok||pending;
    }
    bool finish(HANDLE pipe) noexcept {
        if(!pending)return true;
        if(!psx::sync::io_result(pipe,&ov,&done,FALSE))return false;
        pending=false;return true;
    }
    void cancel(HANDLE pipe) noexcept {
        if(pending){psx::sync::io_cancel(pipe,&ov);DWORD ignored{};psx::sync::io_result(pipe,&ov,&ignored,TRUE);pending=false;}
    }
};
}
std::wstring status_endpoint() {
    alignas(TOKEN_USER) std::array<std::byte,sizeof(TOKEN_USER)+SECURITY_MAX_SID_SIZE> bytes{};
    if(!token_user(GetCurrentProcess(),bytes))return {};
    LPWSTR raw{}; if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&raw))return {};
    std::wstring name(status::plugin_pipe_prefix,status::plugin_pipe_prefix+sizeof(status::plugin_pipe_prefix)-1);
    name+=raw;LocalFree(raw);return name;
}
struct StatusService::Impl {
    Handle stop{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    std::thread worker;std::wstring endpoint;status::Hello hello{};
    TripleBuffer<status::Snapshot> latest;
    SpscFifo<status::Snapshot,32> transitions;
    TripleBuffer<StatusTransportReport> reports;
    std::atomic<std::uint64_t> dropped{};
    StatusTransportReport report{};
    status::Snapshot cached{};
    status::Framer framer;
    std::uint32_t peer_pid{},out_sequence{};
    std::uint64_t peer_birth{},connection{},sent_sequence{};
    std::int64_t subscribed_qpc{},last_peer{},last_heartbeat{},last_snapshot{};
    bool subscribed{};
    ~Impl(){if(stop.valid())SetEvent(stop.value);if(worker.joinable())worker.join();}
    void report_state() noexcept {
        report.connected=subscribed;report.connection_id=connection;
        report.transitions_dropped=dropped.load(std::memory_order_relaxed);
        reports.writing()=report;reports.publish();
    }
    static bool receive(void* context,const status::Header& h,const std::uint8_t* bytes,std::size_t n) noexcept {
        auto& self=*static_cast<Impl*>(context);const auto now=observation_qpc();
        if(h.kind==status::Kind::subscribe){
            status::Subscribe s;
            if(self.subscribed||!status::decode(bytes,n,s)||s.instance!=self.hello.instance||s.pid!=self.peer_pid||
               s.qpc_frequency!=self.hello.qpc_frequency)return false;
            self.subscribed=true;self.connection=s.connection_id;self.subscribed_qpc=now;self.last_peer=now;
            self.report_state();return true;
        }
        status::Heartbeat beat;
        if(!self.subscribed||h.kind!=status::Kind::heartbeat||!status::decode(bytes,n,beat)||
           beat.producer!=self.hello.instance||beat.connection_id!=self.connection||beat.capabilities!=self.hello.capabilities||
           beat.generated_qpc>now||now-beat.generated_qpc>=static_cast<std::int64_t>(self.hello.qpc_frequency*5))return false;
        self.last_peer=now;return true;
    }
    bool snapshot(status::Snapshot value,status::Frame& out) noexcept {
        if(value.producer!=hello.instance||value.role!=status::Role::plugin||value.observed_qpc<=subscribed_qpc||
           value.sequence<=sent_sequence||value.observed_qpc>observation_qpc())return false;
        value.connection_id=connection; // Transport correlation only; never alter source time/sequence.
        if(!status::encode(value,++out_sequence,out))return false;
        sent_sequence=value.sequence;++report.sent;return true;
    }
    bool next(status::Frame& out,std::int64_t now) noexcept {
        if(!subscribed)return false;
        // Heartbeat has priority even under transition pressure. It says nothing about SDK freshness.
        if(!last_heartbeat||now-last_heartbeat>=static_cast<std::int64_t>(hello.qpc_frequency/4)){
            last_heartbeat=now;
            return status::encode(status::Heartbeat{hello.instance,connection,now,hello.capabilities,status::Reason::none},++out_sequence,out);
        }
        status::Snapshot value;
        for(unsigned i=0;i<32&&transitions.pop(value);++i)if(snapshot(value,out))return true;
        if(latest.consume())cached=latest.reading();
        if(now-last_snapshot>=static_cast<std::int64_t>(hello.qpc_frequency/4)&&snapshot(cached,out)){
            last_snapshot=now;return true;
        }
        return false;
    }
    void service(HANDLE pipe,HANDLE peer) noexcept {
        framer.reset();subscribed=false;connection=sent_sequence=out_sequence=0;
        subscribed_qpc=last_heartbeat=last_snapshot=0;last_peer=observation_qpc();
        status::Frame output;bool writing=true;std::size_t offset{};
        if(!status::encode(hello,++out_sequence,output))return;
        Operation read,write;std::array<std::uint8_t,1024> bytes{};
        if(!read.event.valid()||!write.event.valid()||!read.begin(pipe,false,bytes.data(),static_cast<DWORD>(bytes.size()))||
           !write.begin(pipe,true,output.bytes.data(),static_cast<DWORD>(output.size)))return;
        auto progress=observation_qpc();bool okay=true;
        while(okay&&WaitForSingleObject(stop.value,0)!=WAIT_OBJECT_0){
            if(!psx::sync::worker_poll(pipe))break;
            const auto now=observation_qpc();
            if(out_sequence>=UINT32_MAX-8)break; // Fresh handshake before transport sequence wrap.
            if((peer&&(WaitForSingleObject(peer,0)!=WAIT_TIMEOUT||birth(peer)!=peer_birth))||
               now-last_peer>=static_cast<std::int64_t>(hello.qpc_frequency*5)||
               (writing&&now-progress>=static_cast<std::int64_t>(hello.qpc_frequency*5)))break;
            if(!read.pending||WaitForSingleObject(read.event.value,0)==WAIT_OBJECT_0){
                if(!read.finish(pipe)||!read.done||!framer.feed(bytes.data(),read.done,receive,this)){++report.rejected;break;}
                if(!read.begin(pipe,false,bytes.data(),static_cast<DWORD>(bytes.size())))break;
            }
            for(unsigned batch=0;batch<8;++batch){
                if(writing){
                    if(write.pending&&WaitForSingleObject(write.event.value,0)!=WAIT_OBJECT_0)break;
                    if(!write.finish(pipe)||!write.done){okay=false;break;}
                    offset+=write.done;progress=now;
                    if(offset<output.size){if(!write.begin(pipe,true,output.bytes.data()+offset,static_cast<DWORD>(output.size-offset))){okay=false;break;}continue;}
                    writing=false;
                }
                if(!next(output,now))break;
                offset=0;writing=true;progress=now;
                if(!write.begin(pipe,true,output.bytes.data(),static_cast<DWORD>(output.size))){okay=false;break;}
            }
            report_state();
            HANDLE waits[]={stop.value,read.event.value,write.event.value};
            if(!read.pending)ResetEvent(read.event.value);if(!write.pending)ResetEvent(write.event.value);
            WaitForMultipleObjects(3,waits,FALSE,5);
        }
        read.cancel(pipe);write.cancel(pipe);subscribed=false;connection=0;report_state();
    }
    void run() noexcept {while(WaitForSingleObject(stop.value,0)==WAIT_TIMEOUT){if(psx::sync::enabled()){psx::sync::WorkerIo io(psx::sync::Family::status);const auto p=io.status().peer;peer_pid=p.pid;peer_birth=p.process_birth;++report.reconnects;service(nullptr,nullptr);}else run_local();WaitForSingleObject(stop.value,20);}}
    void run_local() noexcept {
        if(endpoint.empty())return;
        alignas(TOKEN_USER) std::array<std::byte,sizeof(TOKEN_USER)+SECURITY_MAX_SID_SIZE> own{};
        if(!token_user(GetCurrentProcess(),own))return;
        while(psx::sync::local_io_selected()&&WaitForSingleObject(stop.value,0)!=WAIT_OBJECT_0){
            Handle pipe(CreateFileW(endpoint.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr));
            if(pipe.valid()){
                ULONG pid{};
                if(GetNamedPipeServerProcessId(pipe.value,&pid)&&pid){
                    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid));
                    alignas(TOKEN_USER) std::array<std::byte,sizeof(TOKEN_USER)+SECURITY_MAX_SID_SIZE> other{};
                    const auto created=process.valid()?birth(process.value):0;
                    if(created&&token_user(process.value,other)&&EqualSid(reinterpret_cast<TOKEN_USER*>(own.data())->User.Sid,
                       reinterpret_cast<TOKEN_USER*>(other.data())->User.Sid)){
                        peer_pid=pid;peer_birth=created;++report.reconnects;service(pipe.value,process.value);
                    }else ++report.rejected;
                }else ++report.rejected;
            }
            report_state();WaitForSingleObject(stop.value,100);
        }
    }
};
StatusService::StatusService()=default;
StatusService::~StatusService(){stop();}
bool StatusService::start(motion::InstanceId producer,const std::wstring& endpoint) noexcept {
    stop();try {
        auto next=std::make_unique<Impl>();next->endpoint=endpoint.empty()?status_endpoint():endpoint;
        if(!motion::valid(producer)||!next->stop.valid()||(next->endpoint.empty()?!psx::sync::runtime():next->endpoint.rfind(L"\\\\.\\pipe\\",0)!=0))return false;
        next->hello={status::Role::plugin,producer,GetCurrentProcessId(),status::xplane_readiness,birth(GetCurrentProcess()),observation_qpc_frequency()};
        if(!next->hello.process_creation_filetime)return false;
        auto* raw=next.get();raw->worker=std::thread([raw]{raw->run();});impl_=std::move(next);return true;
    }catch(...){return false;}
}
void StatusService::stop() noexcept {impl_.reset();}
void StatusService::publish(const status::Snapshot& snapshot,bool transition) noexcept {
    if(!impl_)return;
    impl_->latest.writing()=snapshot;impl_->latest.publish();
    if(transition&&!impl_->transitions.push(snapshot))impl_->dropped.fetch_add(1,std::memory_order_relaxed);
}
bool StatusService::poll(StatusTransportReport& out) noexcept {
    if(!impl_||!impl_->reports.consume())return false;out=impl_->reports.reading();return true;
}
}
