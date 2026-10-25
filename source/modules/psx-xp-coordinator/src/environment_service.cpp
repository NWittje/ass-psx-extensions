#include <optional>
#include <psx/sync/worker_io.hpp>
#include "coordinator/environment.hpp"
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
namespace t=psx::environment;
namespace v2=presentation_wire;
namespace {
using Operation=ServiceOperation;
void sequence(t::Frame& f,std::uint32_t value){for(std::size_t n=0;n<4;++n)f.bytes[12+n]=static_cast<std::uint8_t>(value>>(8*n));}
}
std::wstring environment_plugin_pipe_name(){return L"\\\\.\\pipe\\psx-ios.environment.plugin.v1."+service_user_sid();}
std::wstring environment_exporter_pipe_name(ProcessIdentity p){return L"\\\\.\\pipe\\psx-ios.environment.exporter.v1."+service_user_sid()+L"."+std::to_wstring(p.pid)+L"."+std::to_wstring(p.creation_filetime);}
struct EnvironmentService::Impl {
    struct Outbox {
        ProcessIdentity process{};std::uint64_t generation{};bool version2{};
        std::array<EnvironmentDispatch,16> controls{};std::size_t head{},size{};
        std::array<EnvironmentDispatch,1> latest{};std::array<bool,1> has_latest{};std::size_t cursor{};
        EnvironmentDispatch capture{};bool has_capture{};
        WinHandle wake{make_publication_event()};
        std::wstring test_name; // private fixture seam, not a product setting
    };
    WinHandle cancel{make_stop_event()},ready{make_stop_event()};
    std::array<std::thread,2> threads;
    mutable std::mutex mutex;std::array<Outbox,2> out;
    std::array<EnvironmentEvent,256> events{};std::size_t head{},size{};
    std::atomic<std::uint64_t> dropped{},coalesced{},connections{};std::uint64_t frequency{},owner{};
    std::array<EnvironmentEvent,4> latest_events{};std::array<bool,4> has_event{};std::size_t event_cursor{};
    bool push(const EnvironmentEvent& event){std::lock_guard<std::mutex> lock(mutex);
        t::Header h;v2::Header h2;const bool extended=event.kind==EnvironmentEventKind::frame&&v2::read_header(event.frame.bytes.data(),t::header_size,h2);
        bool framed=extended;if(extended){h.kind=h2.kind==v2::Kind::source?t::Kind::sample:h2.kind==v2::Kind::applied_presentation?t::Kind::applied:static_cast<t::Kind>(h2.kind);h.payload_size=h2.payload_size;h.sequence=h2.sequence;}else framed=event.kind==EnvironmentEventKind::frame&&t::read_header(event.frame.bytes.data(),t::header_size,h);
        const auto lane=event.role==t::Role::exporter?1u:0u;
        if(framed&&(h.kind==t::Kind::sample||h.kind==t::Kind::applied)){
            std::size_t slot{};
            if(h.kind==t::Kind::sample){t::Sample v;v2::Source source;const bool good=extended&&h2.kind==v2::Kind::source?v2::decode({event.frame.bytes.data()+t::header_size,event.frame.size-t::header_size},source):t::decode(event.frame.bytes.data()+t::header_size,h.payload_size,v);if(lane!=1||!good)return false;slot=0;}
            else {t::Applied v;v2::AppliedPresentation report;const bool good=extended&&h2.kind==v2::Kind::applied_presentation?v2::decode({event.frame.bytes.data()+t::header_size,event.frame.size-t::header_size},report):t::decode(event.frame.bytes.data()+t::header_size,h.payload_size,v);if(extended&&h2.kind==v2::Kind::applied_presentation)v=report.applied;if(lane!=0||!good||!v.groups)return false;slot=v.groups;}
            if(has_event[slot])++coalesced;latest_events[slot]=event;has_event[slot]=true;SetEvent(ready.get());return true;
        }
        if(!framed||h.kind==t::Kind::hello||h.kind==t::Kind::status){if(lane)has_event[0]=false;else for(std::size_t n=1;n<4;n++)has_event[n]=false;}
        if(size==events.size()){dropped+=size+1;head=size=0;has_event={};for(std::size_t i=0;i<2;++i){EnvironmentEvent lost;lost.kind=EnvironmentEventKind::lost;lost.role=i?t::Role::exporter:t::Role::plugin;lost.received_qpc=event.received_qpc;lost.error=ERROR_BUFFER_OVERFLOW;events[size++]=lost;}SetEvent(ready.get());return false;}
        events[(head+size)%events.size()]=event;if(++size==1)SetEvent(ready.get());return true;
    }
    void lost(std::size_t i,ProcessIdentity process,std::uint32_t error,std::uint64_t generation=0){EnvironmentEvent e;e.kind=EnvironmentEventKind::lost;e.role=i?t::Role::exporter:t::Role::plugin;e.process=process;e.received_qpc=qpc_now();e.error=error;e.connection_generation=generation;push(e);}
    bool changed(std::size_t i,std::uint64_t generation){std::lock_guard<std::mutex> lock(mutex);return out[i].generation!=generation;}
    bool output(std::size_t i,t::Frame& f){EnvironmentDispatch d;{std::lock_guard<std::mutex> lock(mutex);auto& o=out[i];bool found=false;if(o.size){d=o.controls[o.head];o.head=(o.head+1)%o.controls.size();--o.size;found=true;}else if(o.has_capture){d=o.capture;o.has_capture=false;found=true;}else if(o.has_latest[0]){d=o.latest[0];o.has_latest[0]=false;found=true;}if(!found)return false;d.version2=o.version2;}return encode_environment_dispatch(d,i?t::Role::exporter:t::Role::plugin,1,f);}
    struct Peer {
        Impl* impl{};std::size_t index{};ProcessIdentity process{};bool admitted{},version2{};t::Hello hello{};std::uint32_t capabilities{},last_sequence{};std::uint64_t connection_generation{};std::int64_t connected_qpc{};
        static bool receive(void* context,const t::Header& h,const std::uint8_t* bytes,std::size_t count){return static_cast<Peer*>(context)->input(static_cast<unsigned>(h.kind),h.sequence,bytes,count,1);}
        static bool receive_v2(void* context,const v2::Header& h,const std::uint8_t* bytes,std::size_t count){return static_cast<Peer*>(context)->input(static_cast<unsigned>(h.kind),h.sequence,bytes,count,h.wire_version);}
        bool input(unsigned kind,std::uint32_t sequence_value,const std::uint8_t* bytes,std::size_t count,unsigned wire){
            const bool extended=wire>=2;
            if(extended!=version2||!sequence_value||sequence_value<=last_sequence)return false;last_sequence=sequence_value;
            EnvironmentEvent e;e.role=index?t::Role::exporter:t::Role::plugin;e.process=process;e.received_qpc=qpc_now();e.frame.size=t::header_size+count;e.connection_generation=connection_generation;
            e.frame.bytes[0]='P';e.frame.bytes[1]='X';e.frame.bytes[2]='W';e.frame.bytes[3]=static_cast<std::uint8_t>('0'+wire);e.frame.bytes[4]=static_cast<std::uint8_t>(wire);e.frame.bytes[5]=0;
            for(std::size_t n=0;n<2;++n)e.frame.bytes[6+n]=static_cast<std::uint8_t>(kind>>(8*n));
            for(std::size_t n=0;n<4;++n)e.frame.bytes[8+n]=static_cast<std::uint8_t>(count>>(8*n));sequence(e.frame,sequence_value);
            std::copy(bytes,bytes+count,e.frame.bytes.begin()+t::header_size);const auto whole=std::span<const std::uint8_t>(e.frame.bytes.data()+t::header_size,e.frame.size-t::header_size);
            if(!admitted){
                if(kind!=1)return false;v2::Hello h;const bool good=extended?v2::decode(whole,h):t::decode(bytes,count,hello);if(extended){hello=h.base;capabilities=h.capabilities;}
                if(!good||hello.role!=(index?t::Role::exporter:t::Role::plugin)||hello.pid!=process.pid||hello.process_creation_filetime!=process.creation_filetime||hello.qpc_frequency!=impl->frequency)return false;admitted=true;
            }else{
                if(index==0&&kind!=5&&kind!=7&&kind!=(extended?11u:6u)&&!(extended&&kind==6))return false;
                if(index==1&&kind!=7&&kind!=3&&!(extended&&kind==9))return false;
                if(index==1&&(kind==3||(extended&&kind==9))){t::Sample sample;v2::Source source;const bool good=extended&&kind==9?v2::decode(whole,source):t::decode(bytes,count,sample);if(extended&&kind==9)sample=source.sample;if(!good||sample.producer!=hello.instance)return false;}
                if(index==0&&(kind==6||(extended&&kind==11))){t::Applied applied;v2::AppliedPresentation report;const bool good=extended&&kind==11?v2::decode(whole,report):t::decode(bytes,count,applied);if(extended&&kind==11)applied=report.applied;if(!good||applied.provider!=hello.instance)return false;}
                if(kind==5){t::Ack ack;if(!t::decode(bytes,count,ack))return false;}
                if(kind==7){t::Status status;if(!t::decode(bytes,count,status)||status.producer!=hello.instance)return false;}
            }
            return impl->push(e);
        }
    };
    void run(std::size_t i){ const bool remote=i==0&&psx::sync::enabled();
        while(WaitForSingleObject(cancel.get(),0)!=WAIT_OBJECT_0){
            ProcessIdentity expected;std::uint64_t generation{};std::wstring name;
            {std::lock_guard<std::mutex> lock(mutex);expected=out[i].process;generation=out[i].generation;name=out[i].test_name;}
            if(!expected.pid){HANDLE waits[]={cancel.get(),out[i].wake.get()};WaitForMultipleObjects(2,waits,FALSE,100);continue;}
            WinHandle process(remote?nullptr:OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,expected.pid));
            if(!remote&&(!process.valid()||service_process_creation(process.get())!=expected.creation_filetime)){lost(i,expected,ERROR_INVALID_HANDLE);WaitForSingleObject(cancel.get(),100);continue;}
            if(!remote&&name.empty())name=i?environment_exporter_pipe_name(expected):environment_plugin_pipe_name();
            WinHandle pipe;std::array<WinHandle,4> listeners;HANDLE transport=INVALID_HANDLE_VALUE;Operation read,write;std::uint32_t failure{};bool version2{};
            if(i){
                const auto sid=service_user_sid();const auto acl=L"D:P(A;;GA;;;SY)(A;;GA;;;"+sid+L")";PSECURITY_DESCRIPTOR raw{};
                if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(),SDDL_REVISION_1,&raw,nullptr)){lost(i,expected,GetLastError());WaitForSingleObject(cancel.get(),100);continue;}
                SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),raw,FALSE};
                const auto newer=name==environment_exporter_pipe_name(expected)?L"\\\\.\\pipe\\psx-ios.environment.exporter.v2."+service_user_sid()+L"."+std::to_wstring(expected.pid)+L"."+std::to_wstring(expected.creation_filetime):name+L".v2";
                std::array<Operation,4> connects;std::array<bool,4> attached{};
                auto newest=newer;auto marker=newest.find(L".v2.");if(marker!=std::wstring::npos)newest.replace(marker,4,L".v3.");else newest=name+L".v3";auto fourth=newest;auto fourthMarker=fourth.find(L".v3.");if(fourthMarker!=std::wstring::npos)fourth.replace(fourthMarker,4,L".v4.");else fourth=name+L".v4";const std::array<std::wstring,4> names{fourth,newest,newer,name};
                for(std::size_t lane=0;lane<4;++lane){
                    listeners[lane].reset(CreateNamedPipeW(names[lane].c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,8192,8192,0,&security));
                    if(!listeners[lane].valid())continue;auto& op=connects[lane];op.target=listeners[lane].get();op.prepare();attached[lane]=ConnectNamedPipe(op.target,&op.value)!=FALSE;
                    if(!attached[lane]){const auto error=GetLastError();if(error==ERROR_PIPE_CONNECTED)attached[lane]=true;else if(error==ERROR_IO_PENDING)op.pending=true;else listeners[lane].reset();}
                }
                LocalFree(raw);int selected=-1;
                while(!changed(i,generation)&&WaitForSingleObject(cancel.get(),0)!=WAIT_OBJECT_0&&(remote||WaitForSingleObject(process.get(),0)==WAIT_TIMEOUT)){
                    for(std::size_t lane=0;lane<4;++lane){auto& op=connects[lane];if(op.pending&&WaitForSingleObject(op.event.get(),0)==WAIT_OBJECT_0){DWORD n{};attached[lane]=psx::sync::io_result(op.target,&op.value,&n,FALSE)!=FALSE;op.pending=false;}if(attached[lane]){selected=static_cast<int>(lane);break;}}
                    if(selected>=0||(!listeners[0].valid()&&!listeners[1].valid()&&!listeners[2].valid()&&!listeners[3].valid()))break;
                    HANDLE waits[]={cancel.get(),connects[0].event.get(),connects[1].event.get(),connects[2].event.get(),connects[3].event.get(),out[i].wake.get()};WaitForMultipleObjects(6,waits,FALSE,20);
                }
                for(std::size_t lane=0;lane<4;++lane)if(listeners[lane].valid())connects[lane].cancel(listeners[lane].get());
                if(selected>=0){transport=listeners[static_cast<std::size_t>(selected)].get();version2=selected!=3;for(std::size_t lane=0;lane<4;++lane)if(lane!=static_cast<std::size_t>(selected))listeners[lane].reset();}
            }else if(!remote) {
                const auto newer=name==environment_plugin_pipe_name()?L"\\\\.\\pipe\\psx-ios.environment.plugin.v2."+service_user_sid():name+L".v2";
                auto newest=newer;auto marker=newest.find(L".v2.");if(marker!=std::wstring::npos)newest.replace(marker,4,L".v3.");else newest=name+L".v3";
                auto fourth=newest;auto fourthMarker=fourth.find(L".v3.");if(fourthMarker!=std::wstring::npos)fourth.replace(fourthMarker,4,L".v4.");else fourth=name+L".v4";pipe.reset(CreateFileW(fourth.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr));
                if(!pipe.valid())pipe.reset(CreateFileW(newest.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr));
                if(!pipe.valid())pipe.reset(CreateFileW(newer.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr));
                version2=pipe.valid();if(!pipe.valid())pipe.reset(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr));transport=pipe.get();
            }
            std::optional<psx::sync::WorkerIo> network; if(remote){const auto state=psx::sync::runtime()->status();if(!state.connected||state.peer.pid!=expected.pid||state.peer.process_birth!=expected.creation_filetime){WaitForSingleObject(cancel.get(),20);continue;}network.emplace(psx::sync::Family::environment);transport=nullptr;version2=true;}
            if(!remote&&(!transport||transport==INVALID_HANDLE_VALUE)){failure=GetLastError();if(!changed(i,generation))lost(i,expected,failure);WaitForSingleObject(cancel.get(),100);continue;}
            ULONG pid=remote?expected.pid:0;const bool identified=remote||(i?GetNamedPipeClientProcessId(transport,&pid):GetNamedPipeServerProcessId(transport,&pid))!=FALSE;
            if(!identified||pid!=expected.pid){lost(i,expected,ERROR_ACCESS_DENIED);WaitForSingleObject(cancel.get(),100);continue;}
            read.target=write.target=transport;
            {std::lock_guard<std::mutex> lock(mutex);out[i].head=out[i].size=0;out[i].has_latest={};out[i].has_capture=false;out[i].version2=version2;}
            Peer peer;peer.impl=this;peer.index=i;peer.version2=version2;peer.process=expected;peer.connection_generation=++connections;peer.connected_qpc=qpc_now();t::Framer framer;v2::Framer framer_v2;t::Frame frame;std::size_t offset{};std::uint32_t seq{};bool subscribed{};std::int64_t write_started{},started=qpc_now();std::array<std::uint8_t,4096> input{};bool healthy=true;
            while(healthy&&!changed(i,generation)&&WaitForSingleObject(cancel.get(),0)!=WAIT_OBJECT_0&&(remote||WaitForSingleObject(process.get(),0)==WAIT_TIMEOUT)){
                if(!psx::sync::worker_poll(transport))break;const auto now=qpc_now();if((!peer.admitted&&now-started>static_cast<std::int64_t>(frequency*2))||(write.pending&&now-write_started>static_cast<std::int64_t>(frequency*2))){failure=ERROR_TIMEOUT;break;}
                DWORD count{};
                if(read.pending&&WaitForSingleObject(read.event.get(),0)==WAIT_OBJECT_0){const auto ok=psx::sync::io_result(transport,&read.value,&count,FALSE);read.pending=false;if(!ok||!count){failure=ok?ERROR_BROKEN_PIPE:GetLastError();break;}if(!(version2?framer_v2.feed(input.data(),count,Peer::receive_v2,&peer):framer.feed(input.data(),count,Peer::receive,&peer))){failure=ERROR_INVALID_DATA;break;}}
                if(!read.pending){read.prepare();const auto ok=psx::sync::io_read(transport,input.data(),static_cast<DWORD>(input.size()),&count,&read.value);if(ok){if(!count||!(version2?framer_v2.feed(input.data(),count,Peer::receive_v2,&peer):framer.feed(input.data(),count,Peer::receive,&peer))){failure=ERROR_INVALID_DATA;break;}}else if(GetLastError()==ERROR_IO_PENDING)read.pending=true;else {failure=GetLastError();break;}}
                if(write.pending&&WaitForSingleObject(write.event.get(),0)==WAIT_OBJECT_0){const auto ok=psx::sync::io_result(transport,&write.value,&count,FALSE);write.pending=false;if(!ok||!count){failure=ok?ERROR_BROKEN_PIPE:GetLastError();break;}offset+=count;}
                if(!write.pending&&offset==frame.size){if(seq==UINT32_MAX){failure=ERROR_BUFFER_OVERFLOW;break;}frame={};offset=0;
                    if(peer.admitted&&!subscribed){t::Subscribe s;s.instance=peer.hello.instance;s.owner=owner;s.pid=GetCurrentProcessId();s.qpc_frequency=frequency;healthy=version2?v2::encode(s,++seq,frame):t::encode(s,++seq,frame);subscribed=true;}
                    else if(subscribed&&output(i,frame))sequence(frame,++seq);
                }
                if(healthy&&!write.pending&&offset<frame.size){write.prepare();write_started=qpc_now();const auto ok=psx::sync::io_write(transport,frame.bytes.data()+offset,static_cast<DWORD>(frame.size-offset),&count,&write.value);if(ok){if(!count){failure=ERROR_BROKEN_PIPE;break;}offset+=count;}else if(GetLastError()==ERROR_IO_PENDING)write.pending=true;else {failure=GetLastError();break;}}
                if(!read.pending)ResetEvent(read.event.get());if(!write.pending)ResetEvent(write.event.get());
                HANDLE waits[]={cancel.get(),read.event.get(),write.event.get(),out[i].wake.get()};const auto wait=WaitForMultipleObjects(4,waits,FALSE,!write.pending&&offset<frame.size?0:(remote?2:20));if(wait==WAIT_FAILED){failure=GetLastError();break;}
            }
            read.cancel(transport);write.cancel(transport);if(i)DisconnectNamedPipe(transport);lost(i,expected,failure?failure:ERROR_OPERATION_ABORTED,peer.connection_generation);WaitForSingleObject(cancel.get(),100);
        }
    }
};
EnvironmentService::EnvironmentService():impl_(std::make_unique<Impl>()){}
EnvironmentService::~EnvironmentService(){stop();}
void EnvironmentService::start(std::uint64_t frequency,std::uint64_t owner){if(impl_->threads[0].joinable())return;{std::lock_guard<std::mutex> lock(impl_->mutex);impl_->head=impl_->size=0;impl_->has_event={};ResetEvent(impl_->ready.get());}impl_->frequency=frequency;impl_->owner=owner;ResetEvent(impl_->cancel.get());for(std::size_t i=0;i<2;++i)impl_->threads[i]=std::thread([p=impl_.get(),i]{try{p->run(i);}catch(...){p->lost(i,{},ERROR_UNHANDLED_EXCEPTION);}});}
void EnvironmentService::associate(ProcessIdentity process){std::lock_guard<std::mutex> lock(impl_->mutex);auto& o=impl_->out[1];if(o.process==process)return;o.process=process;++o.generation;o.head=o.size=0;o.has_latest={};o.has_capture=false;SetEvent(o.wake.get());}
void EnvironmentService::plugin(ProcessIdentity process){std::lock_guard<std::mutex> lock(impl_->mutex);auto& o=impl_->out[0];if(o.process==process)return;o.process=process;++o.generation;o.head=o.size=0;o.has_latest={};o.has_capture=false;SetEvent(o.wake.get());}
void EnvironmentService::stop() noexcept {SetEvent(impl_->cancel.get());for(auto& thread:impl_->threads)if(thread.joinable())thread.join();}
bool EnvironmentService::pop(EnvironmentEvent& e){std::lock_guard<std::mutex> lock(impl_->mutex);bool found=false;if(impl_->size){e=impl_->events[impl_->head];impl_->head=(impl_->head+1)%impl_->events.size();--impl_->size;found=true;}else for(std::size_t n=0;n<4;n++){const auto i=impl_->event_cursor++%4;if(impl_->has_event[i]){e=impl_->latest_events[i];impl_->has_event[i]=false;found=true;break;}}if(!impl_->size&&std::none_of(impl_->has_event.begin(),impl_->has_event.end(),[](bool b){return b;}))ResetEvent(impl_->ready.get());return found;}
std::uintptr_t EnvironmentService::ready_event() const noexcept{return reinterpret_cast<std::uintptr_t>(impl_->ready.get());}
std::uint64_t EnvironmentService::dropped() const noexcept{return impl_->dropped.load();}
std::uint64_t EnvironmentService::coalesced() const noexcept{return impl_->coalesced.load();}
bool EnvironmentService::send(t::Role role,const EnvironmentDispatch& f){if(role!=t::Role::plugin&&role!=t::Role::exporter)return false;
    const std::size_t i=role==t::Role::exporter?1:0;std::lock_guard<std::mutex> lock(impl_->mutex);auto& o=impl_->out[i];if(!o.process.pid)return false;
    if(f.kind==t::Kind::control){const auto& c=f.control;if(!c.groups||(c.groups&~t::all_groups))return false;if(c.action==t::Action::release){o.has_latest={};std::array<EnvironmentDispatch,16> retained{};std::size_t count{};for(std::size_t n=0;n<o.size;n++){auto entry=o.controls[(o.head+n)%o.controls.size()];if(entry.control.action!=t::Action::release)entry.control.groups&=~c.groups;if(entry.control.groups)retained[count++]=entry;}o.controls=retained;o.head=0;o.size=count;}if(o.size==o.controls.size()){++impl_->dropped;++o.generation;o.head=o.size=0;o.has_latest={};o.has_capture=false;SetEvent(o.wake.get());return false;}o.controls[(o.head+o.size)%o.controls.size()]=f;++o.size;}
    else if(f.kind==t::Kind::capture){o.capture=f;o.has_capture=true;}
    else if(i==0&&f.kind==t::Kind::sample){if(!t::valid(f.sample))return false;o.latest[0]=f;o.has_latest[0]=true;}
    else return false;SetEvent(o.wake.get());return true;
}
void EnvironmentService::fault(t::Role role,std::uint32_t error) noexcept {const std::size_t i=role==t::Role::exporter?1:0;ProcessIdentity p;{std::lock_guard<std::mutex> lock(impl_->mutex);auto& o=impl_->out[i];p=o.process;++o.generation;o.head=o.size=0;o.has_latest={};o.has_capture=false;SetEvent(o.wake.get());}impl_->lost(i,p,error);}
void EnvironmentService::fixture_names(std::wstring plugin,std::wstring exporter){std::lock_guard<std::mutex> lock(impl_->mutex);impl_->out[0].test_name=std::move(plugin);impl_->out[1].test_name=std::move(exporter);}
bool EnvironmentService::fixture_event(const EnvironmentEvent& e){return impl_->push(e);}
bool EnvironmentService::fixture_output(t::Role role,t::Frame& f){return impl_->output(role==t::Role::exporter?1:0,f);}
}
