#include <optional>
#include <psx/sync/worker_io.hpp>
#include "coordinator/aircraft_sync.hpp"
#include "coordinator/service_io.hpp"
#include "coordinator/status.hpp"
#include <sddl.h>
#include <psx/diagnostics/journal.hpp>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>
namespace coordinator {
namespace t=psx::aircraft_sync;
namespace {
using Operation=ServiceOperation;
void sequence(t::Frame& f,std::uint32_t value){for(std::size_t n=0;n<4;++n)f.bytes[12+n]=static_cast<std::uint8_t>(value>>(8*n));}
}
std::wstring aircraft_plugin_pipe_name(){return L"\\\\.\\pipe\\psx-ios.aircraft.plugin.v1."+service_user_sid();}
std::wstring aircraft_exporter_pipe_name(ProcessIdentity p){return L"\\\\.\\pipe\\psx-ios.aircraft.exporter.v1."+service_user_sid()+L"."+std::to_wstring(p.pid)+L"."+std::to_wstring(p.creation_filetime);}
struct AircraftService::Impl {
    struct Outbox {
        ProcessIdentity process{};std::uint64_t generation{};
        std::array<AircraftDispatch,16> controls{};std::size_t head{},size{};
        std::array<AircraftDispatch,6> latest{};std::array<bool,6> has_latest{};std::size_t cursor{};
        AircraftDispatch capture{};bool has_capture{};
        WinHandle wake{make_publication_event()};
        std::wstring test_name; // private fixture seam, not a product setting
    };
    WinHandle cancel{make_stop_event()},ready{make_stop_event()};
    std::array<std::thread,2> threads;
    mutable std::mutex mutex;std::array<Outbox,2> out;
    std::array<AircraftEvent,256> events{};std::size_t head{},size{};
    std::atomic<std::uint64_t> dropped{},coalesced{},connections{};std::uint64_t frequency{},owner{};
    std::array<AircraftEvent,18> latest_events{};std::array<bool,18> has_event{};std::size_t event_cursor{};
    bool push(const AircraftEvent& event){std::lock_guard<std::mutex> lock(mutex);
        t::Header h;const bool framed=event.kind==AircraftEventKind::frame&&t::read_header(event.frame.bytes.data(),t::header_size,h);
        const auto lane=event.role==t::Role::exporter?1u:0u;
        if(framed&&(h.kind==t::Kind::sample||h.kind==t::Kind::applied)){
            std::size_t slot{};
            if(h.kind==t::Kind::sample){t::Sample v;if(!t::decode(event.frame.bytes.data()+t::header_size,h.payload_size,v))return false;slot=lane*6+static_cast<std::size_t>(v.group);}
            else {t::Applied v;if(!t::decode(event.frame.bytes.data()+t::header_size,h.payload_size,v))return false;slot=12+static_cast<std::size_t>(v.group);}
            if(has_event[slot])++coalesced;latest_events[slot]=event;has_event[slot]=true;SetEvent(ready.get());return true;
        }
        if(!framed||h.kind==t::Kind::hello||h.kind==t::Kind::status){for(std::size_t n=0;n<6;n++)has_event[lane*6+n]=false;if(!lane)for(std::size_t n=12;n<18;n++)has_event[n]=false;}
        if(size==events.size()){dropped+=size+1;head=size=0;has_event={};for(std::size_t i=0;i<2;++i){AircraftEvent lost;lost.kind=AircraftEventKind::lost;lost.role=i?t::Role::exporter:t::Role::plugin;lost.received_qpc=event.received_qpc;lost.error=ERROR_BUFFER_OVERFLOW;events[size++]=lost;}SetEvent(ready.get());return false;}
        events[(head+size)%events.size()]=event;if(++size==1)SetEvent(ready.get());return true;
    }
    void lost(std::size_t i,ProcessIdentity process,std::uint32_t error,std::uint64_t generation=0){AircraftEvent e;e.kind=AircraftEventKind::lost;e.role=i?t::Role::exporter:t::Role::plugin;e.process=process;e.received_qpc=qpc_now();e.error=error;e.connection_generation=generation;push(e);}
    bool changed(std::size_t i,std::uint64_t generation){std::lock_guard<std::mutex> lock(mutex);return out[i].generation!=generation;}
    bool output(std::size_t i,t::Frame& f){AircraftDispatch d;{std::lock_guard<std::mutex> lock(mutex);auto& o=out[i];bool found=false;if(o.size){d=o.controls[o.head];o.head=(o.head+1)%o.controls.size();--o.size;found=true;}else if(o.has_capture){d=o.capture;o.has_capture=false;found=true;}else for(std::size_t n=0;n<6;n++){const auto k=o.cursor++%6;if(o.has_latest[k]){d=o.latest[k];o.has_latest[k]=false;found=true;break;}}if(!found)return false;}return d.kind==t::Kind::control?t::encode(d.control,1,f):d.kind==t::Kind::capture?t::encode(d.capture,1,f):t::encode(d.sample,1,f);}
    struct Peer {
        Impl* impl{};std::size_t index{};ProcessIdentity process{};bool admitted{};t::Hello hello{};std::uint32_t last_sequence{};std::uint64_t connection_generation{};std::int64_t connected_qpc{};
        static bool receive(void* context,const t::Header& h,const std::uint8_t* bytes,std::size_t count){auto& p=*static_cast<Peer*>(context);auto& self=*p.impl;
            if(!h.sequence||h.sequence<=p.last_sequence)return false;p.last_sequence=h.sequence;
            if(!p.admitted){if(h.kind!=t::Kind::hello||!t::decode(bytes,count,p.hello)||p.hello.role!=(p.index?t::Role::exporter:t::Role::plugin)||p.hello.pid!=p.process.pid||p.hello.process_creation_filetime!=p.process.creation_filetime||p.hello.qpc_frequency!=self.frequency)return false;p.admitted=true;}
            else if(!(p.index==0&&(h.kind==t::Kind::ack||h.kind==t::Kind::applied||h.kind==t::Kind::status))&&!(p.index==1&&(h.kind==t::Kind::sample||h.kind==t::Kind::status)))return false;
            if(h.kind==t::Kind::sample){t::Sample v;if(!t::decode(bytes,count,v)||v.source!=t::Source::native||v.producer!=p.hello.instance)return false;}
            if(h.kind==t::Kind::applied){t::Applied v;if(!t::decode(bytes,count,v)||v.provider!=p.hello.instance)return false;}
            if(h.kind==t::Kind::ack){t::Ack v;if(!t::decode(bytes,count,v))return false;}
            if(h.kind==t::Kind::status){t::Status v;if(!t::decode(bytes,count,v)||v.producer!=p.hello.instance)return false;}
            AircraftEvent e;e.role=p.index?t::Role::exporter:t::Role::plugin;e.process=p.process;e.received_qpc=qpc_now();e.frame.size=t::header_size+count;
            e.connection_generation=p.connection_generation;
            e.frame.bytes[0]='P';e.frame.bytes[1]='X';e.frame.bytes[2]='A';e.frame.bytes[3]=static_cast<std::uint8_t>('0'+t::protocol_version);e.frame.bytes[4]=static_cast<std::uint8_t>(t::protocol_version);e.frame.bytes[5]=0;
            for(std::size_t n=0;n<2;++n)e.frame.bytes[6+n]=static_cast<std::uint8_t>(static_cast<unsigned>(h.kind)>>(8*n));
            for(std::size_t n=0;n<4;++n)e.frame.bytes[8+n]=static_cast<std::uint8_t>(count>>(8*n));sequence(e.frame,h.sequence);
            std::copy(bytes,bytes+count,e.frame.bytes.begin()+t::header_size);return self.push(e);
        }
    };
    void run(std::size_t i){ const bool remote=i==0&&psx::sync::enabled();
        while(WaitForSingleObject(cancel.get(),0)!=WAIT_OBJECT_0){
            ProcessIdentity expected;std::uint64_t generation{};std::wstring name;
            {std::lock_guard<std::mutex> lock(mutex);expected=out[i].process;generation=out[i].generation;name=out[i].test_name;}
            if(!expected.pid){HANDLE waits[]={cancel.get(),out[i].wake.get()};WaitForMultipleObjects(2,waits,FALSE,100);continue;}
            WinHandle process(remote?nullptr:OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,expected.pid));
            if(!remote&&(!process.valid()||service_process_creation(process.get())!=expected.creation_filetime)){lost(i,expected,ERROR_INVALID_HANDLE);WaitForSingleObject(cancel.get(),100);continue;}
            if(!remote&&name.empty())name=i?aircraft_exporter_pipe_name(expected):aircraft_plugin_pipe_name();
            WinHandle pipe;Operation connection,read,write;std::uint32_t failure{};
            if(i){
                const auto sid=service_user_sid();const auto acl=L"D:P(A;;GA;;;SY)(A;;GA;;;"+sid+L")";PSECURITY_DESCRIPTOR raw{};
                if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(),SDDL_REVISION_1,&raw,nullptr)){lost(i,expected,GetLastError());WaitForSingleObject(cancel.get(),100);continue;}
                SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),raw,FALSE};pipe.reset(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,8192,8192,0,&security));LocalFree(raw);
                if(pipe.valid()){
                    connection.target=pipe.get();
                    connection.prepare();bool attached=ConnectNamedPipe(pipe.get(),&connection.value)!=FALSE;
                    if(!attached){const auto error=GetLastError();if(error==ERROR_PIPE_CONNECTED)attached=true;else if(error==ERROR_IO_PENDING){connection.pending=true;
                        while(!changed(i,generation)&&WaitForSingleObject(cancel.get(),0)!=WAIT_OBJECT_0&&(remote||WaitForSingleObject(process.get(),0)==WAIT_TIMEOUT)){HANDLE waits[]={cancel.get(),connection.event.get(),out[i].wake.get()};const auto wait=WaitForMultipleObjects(3,waits,FALSE,20);if(wait==WAIT_OBJECT_0+1){DWORD n{};attached=psx::sync::io_result(pipe.get(),&connection.value,&n,FALSE)!=FALSE;connection.pending=false;break;}}
                    }}
                    if(!attached){connection.cancel(pipe.get());pipe.reset();}
                }
            }else if(!remote) pipe.reset(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr));
            std::optional<psx::sync::WorkerIo> network; if(remote){const auto state=psx::sync::runtime()->status();if(!state.connected||state.peer.pid!=expected.pid||state.peer.process_birth!=expected.creation_filetime){WaitForSingleObject(cancel.get(),20);continue;}network.emplace(psx::sync::Family::aircraft);}
            if(!remote&&!pipe.valid()){failure=GetLastError();if(!changed(i,generation))lost(i,expected,failure);WaitForSingleObject(cancel.get(),100);continue;}
            ULONG pid=remote?expected.pid:0;const bool identified=remote||(i?GetNamedPipeClientProcessId(pipe.get(),&pid):GetNamedPipeServerProcessId(pipe.get(),&pid))!=FALSE;
            if(!identified||pid!=expected.pid){lost(i,expected,ERROR_ACCESS_DENIED);WaitForSingleObject(cancel.get(),100);continue;}
            read.target=write.target=pipe.get();
            {std::lock_guard<std::mutex> lock(mutex);out[i].head=out[i].size=0;out[i].has_latest={};out[i].has_capture=false;}
            Peer peer;peer.impl=this;peer.index=i;peer.process=expected;peer.connection_generation=++connections;peer.connected_qpc=qpc_now();t::Framer framer;t::Frame frame;std::size_t offset{};std::uint32_t seq{};bool subscribed{};std::int64_t write_started{},started=qpc_now();std::array<std::uint8_t,4096> input{};bool healthy=true;
            while(healthy&&!changed(i,generation)&&WaitForSingleObject(cancel.get(),0)!=WAIT_OBJECT_0&&(remote||WaitForSingleObject(process.get(),0)==WAIT_TIMEOUT)){
                if(!psx::sync::worker_poll(pipe.get()))break;const auto now=qpc_now();if((!peer.admitted&&now-started>static_cast<std::int64_t>(frequency*2))||(write.pending&&now-write_started>static_cast<std::int64_t>(frequency*2))){failure=ERROR_TIMEOUT;break;}
                DWORD count{};
                if(read.pending&&WaitForSingleObject(read.event.get(),0)==WAIT_OBJECT_0){const auto ok=psx::sync::io_result(pipe.get(),&read.value,&count,FALSE);read.pending=false;if(!ok||!count){failure=ok?ERROR_BROKEN_PIPE:GetLastError();break;}if(!framer.feed(input.data(),count,Peer::receive,&peer)){failure=ERROR_INVALID_DATA;break;}}
                if(!read.pending){read.prepare();const auto ok=psx::sync::io_read(pipe.get(),input.data(),static_cast<DWORD>(input.size()),&count,&read.value);if(ok){if(!count||!framer.feed(input.data(),count,Peer::receive,&peer)){failure=ERROR_INVALID_DATA;break;}}else if(GetLastError()==ERROR_IO_PENDING)read.pending=true;else {failure=GetLastError();break;}}
                if(write.pending&&WaitForSingleObject(write.event.get(),0)==WAIT_OBJECT_0){const auto ok=psx::sync::io_result(pipe.get(),&write.value,&count,FALSE);write.pending=false;if(!ok||!count){failure=ok?ERROR_BROKEN_PIPE:GetLastError();break;}offset+=count;}
                if(!write.pending&&offset==frame.size){frame={};offset=0;
                    if(peer.admitted&&!subscribed){t::Subscribe s;s.instance=peer.hello.instance;s.owner=owner;s.pid=GetCurrentProcessId();s.qpc_frequency=frequency;healthy=t::encode(s,++seq,frame);subscribed=true;}
                    else if(subscribed&&output(i,frame))sequence(frame,++seq);
                }
                if(healthy&&!write.pending&&offset<frame.size){write.prepare();write_started=qpc_now();const auto ok=psx::sync::io_write(pipe.get(),frame.bytes.data()+offset,static_cast<DWORD>(frame.size-offset),&count,&write.value);if(ok){if(!count){failure=ERROR_BROKEN_PIPE;break;}offset+=count;}else if(GetLastError()==ERROR_IO_PENDING)write.pending=true;else {failure=GetLastError();break;}}
                if(!read.pending)ResetEvent(read.event.get());if(!write.pending)ResetEvent(write.event.get());
                HANDLE waits[]={cancel.get(),read.event.get(),write.event.get(),out[i].wake.get()};const auto wait=WaitForMultipleObjects(4,waits,FALSE,!write.pending&&offset<frame.size?0:(remote?2:20));if(wait==WAIT_FAILED){failure=GetLastError();break;}
            }
            read.cancel(pipe.get());write.cancel(pipe.get());if(i)DisconnectNamedPipe(pipe.get());lost(i,expected,failure?failure:ERROR_OPERATION_ABORTED,peer.connection_generation);WaitForSingleObject(cancel.get(),100);
        }
    }
};
AircraftService::AircraftService():impl_(std::make_unique<Impl>()){}
AircraftService::~AircraftService(){stop();}
void AircraftService::start(std::uint64_t frequency,std::uint64_t owner){if(impl_->threads[0].joinable())return;{std::lock_guard<std::mutex> lock(impl_->mutex);impl_->head=impl_->size=0;impl_->has_event={};ResetEvent(impl_->ready.get());}impl_->frequency=frequency;impl_->owner=owner;ResetEvent(impl_->cancel.get());for(std::size_t i=0;i<2;++i)impl_->threads[i]=std::thread([p=impl_.get(),i]{try{p->run(i);}catch(...){p->lost(i,{},ERROR_UNHANDLED_EXCEPTION);}});}
void AircraftService::associate(ProcessIdentity process){std::lock_guard<std::mutex> lock(impl_->mutex);auto& o=impl_->out[1];if(o.process==process)return;o.process=process;++o.generation;o.head=o.size=0;o.has_latest={};o.has_capture=false;SetEvent(o.wake.get());}
void AircraftService::plugin(ProcessIdentity process){std::lock_guard<std::mutex> lock(impl_->mutex);auto& o=impl_->out[0];if(o.process==process)return;o.process=process;++o.generation;o.head=o.size=0;o.has_latest={};o.has_capture=false;SetEvent(o.wake.get());}
void AircraftService::stop() noexcept {SetEvent(impl_->cancel.get());for(auto& thread:impl_->threads)if(thread.joinable())thread.join();}
bool AircraftService::pop(AircraftEvent& e){std::lock_guard<std::mutex> lock(impl_->mutex);bool found=false;if(impl_->size){e=impl_->events[impl_->head];impl_->head=(impl_->head+1)%impl_->events.size();--impl_->size;found=true;}else for(std::size_t n=0;n<18;n++){const auto i=impl_->event_cursor++%18;if(impl_->has_event[i]){e=impl_->latest_events[i];impl_->has_event[i]=false;found=true;break;}}if(!impl_->size&&std::none_of(impl_->has_event.begin(),impl_->has_event.end(),[](bool b){return b;}))ResetEvent(impl_->ready.get());return found;}
std::uintptr_t AircraftService::ready_event() const noexcept{return reinterpret_cast<std::uintptr_t>(impl_->ready.get());}
std::uint64_t AircraftService::dropped() const noexcept{return impl_->dropped.load();}
std::uint64_t AircraftService::coalesced() const noexcept{return impl_->coalesced.load();}
bool AircraftService::send(t::Role role,const AircraftDispatch& f){if(role!=t::Role::plugin&&role!=t::Role::exporter)return false;
    const std::size_t i=role==t::Role::exporter?1:0;std::lock_guard<std::mutex> lock(impl_->mutex);auto& o=impl_->out[i];if(!o.process.pid)return false;
    if(f.kind==t::Kind::control){const auto& c=f.control;if(!c.groups||(c.groups&~t::all_groups))return false;if(c.action==t::Action::release){for(std::size_t n=0;n<6;n++)if(c.groups&(1u<<n))o.has_latest[n]=false;std::array<AircraftDispatch,16> retained{};std::size_t count{};for(std::size_t n=0;n<o.size;n++){auto entry=o.controls[(o.head+n)%o.controls.size()];if(entry.control.action!=t::Action::release)entry.control.groups&=~c.groups;if(entry.control.groups)retained[count++]=entry;}o.controls=retained;o.head=0;o.size=count;}if(o.size==o.controls.size()){++impl_->dropped;++o.generation;o.head=o.size=0;o.has_latest={};o.has_capture=false;SetEvent(o.wake.get());return false;}o.controls[(o.head+o.size)%o.controls.size()]=f;++o.size;}
    else if(f.kind==t::Kind::capture){o.capture=f;o.has_capture=true;}
    else if(i==0&&f.kind==t::Kind::sample){if(!t::valid(f.sample))return false;const auto n=static_cast<std::size_t>(f.sample.group);o.latest[n]=f;o.has_latest[n]=true;}
    else return false;SetEvent(o.wake.get());return true;
}
void AircraftService::fault(t::Role role,std::uint32_t error) noexcept {const std::size_t i=role==t::Role::exporter?1:0;ProcessIdentity p;{std::lock_guard<std::mutex> lock(impl_->mutex);auto& o=impl_->out[i];p=o.process;++o.generation;o.head=o.size=0;o.has_latest={};o.has_capture=false;SetEvent(o.wake.get());}impl_->lost(i,p,error);}
void AircraftService::fixture_names(std::wstring plugin,std::wstring exporter){std::lock_guard<std::mutex> lock(impl_->mutex);impl_->out[0].test_name=std::move(plugin);impl_->out[1].test_name=std::move(exporter);}
bool AircraftService::fixture_event(const AircraftEvent& e){return impl_->push(e);}
bool AircraftService::fixture_output(t::Role role,t::Frame& f){return impl_->output(role==t::Role::exporter?1:0,f);}
std::size_t AircraftService::fixture_controls_queued(t::Role role) const {std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->out[role==t::Role::exporter?1:0].size;}
}
