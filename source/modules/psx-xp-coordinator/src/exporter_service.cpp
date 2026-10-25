#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <sddl.h>
#include "coordinator/exporter.hpp"
#include "coordinator/win_handle.hpp"
#include "coordinator/status.hpp"
#include <psx/diagnostics/journal.hpp>
#include <array>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>
namespace coordinator {
namespace e=psx::exporter;
namespace {
std::uint64_t creation(HANDLE process){FILETIME a{},b{},c{},d{};if(!GetProcessTimes(process,&a,&b,&c,&d))return 0;return std::uint64_t(a.dwLowDateTime)|(std::uint64_t(a.dwHighDateTime)<<32);}
std::wstring sid_string(){HANDLE h{};if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&h))throw_win32_error("Exporter token");WinHandle token(h);
    DWORD size{};GetTokenInformation(token.get(),TokenUser,nullptr,0,&size);std::vector<std::uint8_t> bytes(size);
    if(!GetTokenInformation(token.get(),TokenUser,bytes.data(),size,&size))throw_win32_error("Exporter TokenUser");LPWSTR text{};
    if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&text))throw_win32_error("Exporter SID");std::wstring result(text);LocalFree(text);return result;}
struct Operation {
    WinHandle event{CreateEventW(nullptr,TRUE,FALSE,nullptr)};OVERLAPPED value{};bool pending{};
    Operation(){if(!event.valid())throw_win32_error("Exporter event");value.hEvent=event.get();}
    void prepare(){ResetEvent(event.get());value={};value.hEvent=event.get();}
    void cancel(HANDLE pipe){if(pending){CancelIoEx(pipe,&value);DWORD count{};GetOverlappedResult(pipe,&value,&count,TRUE);pending=false;}}
};
}
bool main_server_process(std::uintptr_t socket,ProcessIdentity& out,std::uint32_t& error) {
    out={};sockaddr_in local{},remote{};int size=sizeof(local);
    if(getsockname(static_cast<SOCKET>(socket),reinterpret_cast<sockaddr*>(&local),&size)||local.sin_family!=AF_INET){error=WSAGetLastError();return false;}
    size=sizeof(remote);if(getpeername(static_cast<SOCKET>(socket),reinterpret_cast<sockaddr*>(&remote),&size)||remote.sin_family!=AF_INET){error=WSAGetLastError();return false;}
    DWORD bytes{};DWORD code=GetExtendedTcpTable(nullptr,&bytes,FALSE,AF_INET,TCP_TABLE_OWNER_PID_CONNECTIONS,0);
    if(code!=ERROR_INSUFFICIENT_BUFFER){error=code;return false;}
    std::vector<std::uint8_t> storage;
    for(unsigned attempt=0;attempt<3;++attempt){storage.resize(bytes);code=GetExtendedTcpTable(storage.data(),&bytes,FALSE,AF_INET,TCP_TABLE_OWNER_PID_CONNECTIONS,0);if(code!=ERROR_INSUFFICIENT_BUFFER)break;}
    if(code!=NO_ERROR){error=code;return false;}
    const auto* table=reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(storage.data());
    for(DWORD i=0;i<table->dwNumEntries;++i){const auto& r=table->table[i];
        if(r.dwState!=MIB_TCP_STATE_ESTAB||r.dwLocalAddr!=remote.sin_addr.s_addr||r.dwRemoteAddr!=local.sin_addr.s_addr||
           static_cast<u_short>(r.dwLocalPort)!=remote.sin_port||static_cast<u_short>(r.dwRemotePort)!=local.sin_port)continue;
        WinHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,r.dwOwningPid));
        if(!process.valid()){error=GetLastError();return false;}const auto born=creation(process.get());
        if(!born){error=GetLastError();return false;}if(WaitForSingleObject(process.get(),0)!=WAIT_TIMEOUT){error=ERROR_PROCESS_ABORTED;return false;}
        out={r.dwOwningPid,born};error=0;return true;
    }
    error=ERROR_NOT_FOUND;return false;
}
std::wstring exporter_pipe_name(ProcessIdentity p){return L"\\\\.\\pipe\\psx-ios.exporter.v1."+sid_string()+L"."+std::to_wstring(p.pid)+L"."+std::to_wstring(p.creation_filetime);}
struct ExporterService::Impl {
    WinHandle cancel{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    WinHandle ready{make_stop_event()};
    WinHandle lease_ready{make_publication_event()};
    std::thread worker;ProcessIdentity process{};std::uint64_t frequency{};std::atomic<bool> running{};
    mutable std::mutex mutex;std::array<ExporterEvent,256> queue{};std::size_t head{},size{};std::atomic<std::uint64_t> lost{},audio_lost{};
    const std::uint64_t coordinator_owner{psx::diagnostics::new_identity()};
    std::array<psx::audio::Lease,8> leases{};std::size_t lease_head{},lease_size{};bool audio_peer{};
    bool push(const ExporterEvent& v){std::lock_guard<std::mutex> lock(mutex);
        // Reserve sixteen slots for ordered identity/lifecycle controls.
        if((v.kind==ExporterEventKind::audio_snapshot||v.kind==ExporterEventKind::audio_ack)&&size>=128){++audio_lost;return true;}
        if(v.kind==ExporterEventKind::snapshot&&size>=queue.size()-16){++lost;return true;}
        if(size==queue.size()){lost+=size+1;head=size=0;ExporterEvent reset;reset.kind=ExporterEventKind::association;reset.process=process;reset.received_qpc=v.received_qpc;queue[size++]=reset;reset.kind=ExporterEventKind::unavailable;reset.error=ERROR_BUFFER_OVERFLOW;queue[size++]=reset;SetEvent(ready.get());return false;}
        queue[(head+size)%queue.size()]=v;if(++size==1)SetEvent(ready.get());return true;
    }
    void state(ExporterEventKind k,std::uint32_t error=0){ExporterEvent v;v.kind=k;v.received_qpc=qpc_now();v.process=process;v.error=error;push(v);}
    struct Peer {
        Impl* owner{};ULONG pid{};bool admitted{},subscribe_pending{};std::uint16_t version{};e::Hello hello{};e::Frame subscribe{};
        static bool receive(void* context,const e::Header& h,const std::uint8_t* data,std::size_t size){
            auto& self=*static_cast<Peer*>(context);auto& owner=*self.owner;
            ExporterEvent event;event.process=owner.process;event.client_pid=self.pid;event.received_qpc=qpc_now();
            if(!self.admitted){
                if(h.kind!=e::Kind::hello||!e::decode(data,size,self.hello)||!exporter_identity_matches(owner.process,self.pid,owner.frequency,self.hello))return false;
                self.version=h.version;e::Subscribe s;s.producer=self.hello.producer;s.coordinator_instance=owner.coordinator_owner;s.qpc_frequency=owner.frequency;
                if(!(h.version==1?e::encode_legacy_subscribe(s,1,self.subscribe):e::encode(s,1,self.subscribe)))return false;
                {std::lock_guard<std::mutex> lock(owner.mutex);owner.audio_peer=h.version==2;owner.lease_head=owner.lease_size=0;}
                self.admitted=true;self.subscribe_pending=true;event.kind=ExporterEventKind::hello;
            }else if(h.kind==e::Kind::snapshot){
                // Keep every structurally valid frame, even a rejected pose.
                // The pure model derives/validates only after CAP6 records it.
                event.kind=ExporterEventKind::snapshot;
            }else if(h.kind==e::Kind::health){e::Health value;if(!e::decode(data,size,value))return false;event.kind=ExporterEventKind::health;}
            else if(h.kind==e::Kind::audio_snapshot&&self.version==2){event.kind=ExporterEventKind::audio_snapshot;}
            else if(h.kind==e::Kind::audio_ack&&self.version==2){event.kind=ExporterEventKind::audio_ack;}
            else return false;
            if(h.version!=self.version)return false;
            event.frame.size=e::header_size+size;auto* p=event.frame.bytes.data();p[0]='P';p[1]='X';p[2]='S';p[3]='E';p[4]=static_cast<std::uint8_t>(h.version);p[5]=0;
            for(std::size_t i=0;i<2;++i)p[6+i]=static_cast<std::uint8_t>(static_cast<unsigned>(h.kind)>>(8*i));
            for(std::size_t i=0;i<4;++i){p[8+i]=static_cast<std::uint8_t>(size>>(8*i));p[12+i]=static_cast<std::uint8_t>(h.sequence>>(8*i));}
            std::copy(data,data+size,p+e::header_size);
            return owner.push(event);
        }
    };
    void run(){
        WinHandle target(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,process.pid));
        if(!target.valid()||creation(target.get())!=process.creation_filetime){state(ExporterEventKind::unavailable,ERROR_INVALID_HANDLE);return;}
        const auto name=exporter_pipe_name(process);const auto acl=L"D:P(A;;GA;;;SY)(A;;GA;;;"+sid_string()+L")";
        PSECURITY_DESCRIPTOR descriptor{};if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(),SDDL_REVISION_1,&descriptor,nullptr)){state(ExporterEventKind::unavailable,GetLastError());return;}
        SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
        WinHandle pipe(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,16384,65536,0,&security));LocalFree(descriptor);
        if(!pipe.valid()){state(ExporterEventKind::unavailable,GetLastError());return;}
        Operation connect,read,write;
        HANDLE lifecycle[]={cancel.get(),target.get()};
        while(WaitForMultipleObjects(2,lifecycle,FALSE,0)==WAIT_TIMEOUT){
            connect.prepare();bool attached=ConnectNamedPipe(pipe.get(),&connect.value)!=FALSE;
            if(!attached){const auto error=GetLastError();if(error==ERROR_PIPE_CONNECTED)attached=true;
                else if(error==ERROR_IO_PENDING){connect.pending=true;HANDLE waits[]={cancel.get(),target.get(),connect.event.get()};
                    if(WaitForMultipleObjects(3,waits,FALSE,INFINITE)==WAIT_OBJECT_0+2){DWORD n{};attached=GetOverlappedResult(pipe.get(),&connect.value,&n,FALSE)!=FALSE;connect.pending=false;}
                    else {connect.cancel(pipe.get());break;}}
                else {state(ExporterEventKind::unavailable,error);break;}}
            if(!attached){DisconnectNamedPipe(pipe.get());continue;}
            Peer peer;peer.owner=this;DWORD failure{};
            if(!GetNamedPipeClientProcessId(pipe.get(),&peer.pid)||peer.pid!=process.pid){state(ExporterEventKind::unavailable,ERROR_ACCESS_DENIED);DisconnectNamedPipe(pipe.get());continue;}
            e::Framer framer;std::array<std::uint8_t,8192> input{};std::size_t sent{};bool healthy=true;const auto attached_at=qpc_now();std::int64_t write_start{};std::uint32_t output_sequence=1;
            while(healthy&&WaitForMultipleObjects(2,lifecycle,FALSE,0)==WAIT_TIMEOUT){
                const auto now=qpc_now();if((!peer.admitted&&now-attached_at>2*qpc_frequency())||(write.pending&&now-write_start>2*qpc_frequency())){failure=ERROR_TIMEOUT;break;}
                DWORD count{};
                if(read.pending&&WaitForSingleObject(read.event.get(),0)==WAIT_OBJECT_0){const auto ok=GetOverlappedResult(pipe.get(),&read.value,&count,FALSE);read.pending=false;
                    if(!ok||!count){failure=ok?ERROR_BROKEN_PIPE:GetLastError();break;}if(!framer.feed(input.data(),count,Peer::receive,&peer)){failure=ERROR_INVALID_DATA;break;}}
                if(!read.pending){read.prepare();const auto ok=ReadFile(pipe.get(),input.data(),static_cast<DWORD>(input.size()),&count,&read.value);
                    if(ok){if(!count){failure=ERROR_BROKEN_PIPE;break;}if(!framer.feed(input.data(),count,Peer::receive,&peer)){failure=ERROR_INVALID_DATA;break;}}
                    else if(GetLastError()==ERROR_IO_PENDING)read.pending=true;else{failure=GetLastError();break;}}
                if(write.pending&&WaitForSingleObject(write.event.get(),0)==WAIT_OBJECT_0){const auto ok=GetOverlappedResult(pipe.get(),&write.value,&count,FALSE);write.pending=false;
                    if(!ok||!count){failure=ok?ERROR_BROKEN_PIPE:GetLastError();break;}sent+=count;}
                if(peer.subscribe_pending&&!write.pending&&sent<peer.subscribe.size){write.prepare();write_start=qpc_now();const auto ok=WriteFile(pipe.get(),peer.subscribe.bytes.data()+sent,static_cast<DWORD>(peer.subscribe.size-sent),&count,&write.value);
                    if(ok){if(!count){failure=ERROR_BROKEN_PIPE;break;}sent+=count;}else if(GetLastError()==ERROR_IO_PENDING)write.pending=true;else{failure=GetLastError();break;}}
                if(sent==peer.subscribe.size&&peer.subscribe.size)peer.subscribe_pending=false;
                if(peer.admitted&&!peer.subscribe_pending&&!write.pending){std::lock_guard<std::mutex> lock(mutex);if(lease_size){const auto lease=leases[lease_head];lease_head=(lease_head+1)%leases.size();--lease_size;
                    if(peer.version==2&&(!lease.groups||lease.expires_qpc>qpc_now())&&e::encode(lease,++output_sequence,peer.subscribe)){sent=0;peer.subscribe_pending=true;}}}
                if(!read.pending)ResetEvent(read.event.get());if(!write.pending)ResetEvent(write.event.get());
                HANDLE waits[]={cancel.get(),target.get(),read.event.get(),write.event.get(),lease_ready.get()};WaitForMultipleObjects(5,waits,FALSE,read.pending?20:0);
            }
            read.cancel(pipe.get());write.cancel(pipe.get());DisconnectNamedPipe(pipe.get());{std::lock_guard<std::mutex> lock(mutex);audio_peer=false;lease_head=lease_size=0;}state(ExporterEventKind::unavailable,failure);
        }
        state(ExporterEventKind::unavailable,WaitForSingleObject(target.get(),0)==WAIT_OBJECT_0?ERROR_PROCESS_ABORTED:ERROR_OPERATION_ABORTED);
    }
};
ExporterService::ExporterService():impl_(std::make_unique<Impl>()){}
ExporterService::~ExporterService(){stop();}
bool ExporterService::associate(ProcessIdentity process,std::uint64_t frequency){
    if(impl_->worker.joinable()&&impl_->running&&impl_->process==process)return true;
    stop();if(!process.pid||!process.creation_filetime||!frequency)return false;
    impl_->process=process;impl_->frequency=frequency;ResetEvent(impl_->cancel.get());impl_->state(ExporterEventKind::association);
    try{impl_->running=true;impl_->worker=std::thread([p=impl_.get()]{
        while(WaitForSingleObject(p->cancel.get(),0)!=WAIT_OBJECT_0){
            try{p->run();}catch(...){p->state(ExporterEventKind::unavailable,ERROR_UNHANDLED_EXCEPTION);}
            WinHandle target(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,p->process.pid));
            if(!target.valid()||creation(target.get())!=p->process.creation_filetime||WaitForSingleObject(target.get(),0)!=WAIT_TIMEOUT)break;
            // Optional listener errors recover without any Main socket event.
            if(WaitForSingleObject(p->cancel.get(),1000)==WAIT_OBJECT_0)break;
        }p->running=false;
    });return true;}catch(...){impl_->running=false;impl_->state(ExporterEventKind::unavailable,ERROR_NOT_ENOUGH_MEMORY);return false;}
}
void ExporterService::stop() noexcept {SetEvent(impl_->cancel.get());if(impl_->worker.joinable())impl_->worker.join();}
bool ExporterService::pop(ExporterEvent& out){std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->size)return false;out=impl_->queue[impl_->head];impl_->head=(impl_->head+1)%impl_->queue.size();if(!--impl_->size)ResetEvent(impl_->ready.get());return true;}
std::uintptr_t ExporterService::ready_event() const noexcept{return reinterpret_cast<std::uintptr_t>(impl_->ready.get());}
std::uint64_t ExporterService::dropped() const noexcept{return impl_->lost.load();}
std::uint64_t ExporterService::audio_dropped() const noexcept{return impl_->audio_lost.load();}
std::uint64_t ExporterService::owner() const noexcept{return impl_->coordinator_owner;}
bool ExporterService::lease(const psx::audio::Lease& value){std::lock_guard<std::mutex> lock(impl_->mutex);if(!psx::audio::valid(value)||value.owner!=impl_->coordinator_owner||!impl_->audio_peer||impl_->lease_size==impl_->leases.size())return false;
    impl_->leases[(impl_->lease_head+impl_->lease_size)%impl_->leases.size()]=value;++impl_->lease_size;SetEvent(impl_->lease_ready.get());return true;}
}
