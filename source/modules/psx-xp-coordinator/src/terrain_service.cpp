#include <optional>
#include <psx/sync/worker_io.hpp>
#include "coordinator/terrain.hpp"
#include "coordinator/service_io.hpp"
#include "coordinator/status.hpp"
#include "terrain_ingress_batch.hpp"
#include <sddl.h>
#include <psx/diagnostics/journal.hpp>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>
namespace coordinator {
namespace t=psx::terrain;
namespace {
using Operation=ServiceOperation;
void sequence(t::Frame& f,std::uint32_t value){for(std::size_t n=0;n<4;++n)f.bytes[12+n]=static_cast<std::uint8_t>(value>>(8*n));}
}
std::wstring terrain_plugin_pipe_name(){return L"\\\\.\\pipe\\psx-ios.terrain.plugin.v3."+service_user_sid();}
std::wstring terrain_exporter_pipe_name(ProcessIdentity p){return L"\\\\.\\pipe\\psx-ios.terrain.exporter.v3."+service_user_sid()+L"."+std::to_wstring(p.pid)+L"."+std::to_wstring(p.creation_filetime);}
// This resolves the selected plugin endpoint. A remote PID has meaning only on
// its own host; loss/mismatch must never fall back to an unrelated local PID.
ProcessIdentity terrain_process_identity(std::uint32_t pid) noexcept {if(auto* net=psx::sync::runtime()){const auto state=net->status();return state.connected&&state.peer.pid==pid?ProcessIdentity{state.peer.pid,state.peer.process_birth}:ProcessIdentity{};}WinHandle p(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid));return p.valid()&&WaitForSingleObject(p.get(),0)==WAIT_TIMEOUT?ProcessIdentity{pid,service_process_creation(p.get())}:ProcessIdentity{};}
struct TerrainService::Impl {
    struct Outbox {
        ProcessIdentity process{},connection_process{};std::uint64_t generation{},connection_generation{};
        std::array<t::Frame,16> controls{};std::size_t head{},size{};
        t::Frame latest{};bool has_latest{};
        t::Frame capture{};bool has_capture{};
        WinHandle wake{make_publication_event()};
        std::wstring test_name; // private fixture seam, not a product setting
    };
    WinHandle cancel{make_stop_event()},ready{make_stop_event()};
    std::array<std::thread,2> threads;
    mutable std::mutex mutex;std::array<Outbox,2> out;
    std::array<TerrainEvent,256> events{};std::size_t head{},size{},details{};
    std::atomic<std::uint64_t> dropped{},connections{};std::uint64_t frequency{},owner{};
    bool push(const TerrainEvent& event){std::lock_guard<std::mutex> lock(mutex);
        t::Header h;const bool framed=event.kind==TerrainEventKind::frame&&t::read_header(event.frame.bytes.data(),t::header_size,h);
        const bool detail=framed&&(h.kind==t::Kind::probe_detail||h.kind==t::Kind::support_detail||h.kind==t::Kind::render_detail||h.kind==t::Kind::constraint_detail||h.kind==t::Kind::frame_detail);
        const bool replaceable=framed&&
            (h.kind==t::Kind::surface||h.kind==t::Kind::consumed||h.kind==t::Kind::diagnostics||detail);
        if((detail&&details>=64)||(replaceable&&size>=128)){++dropped;return true;}
        if(size==events.size()){dropped+=size+1;head=size=details=0;for(std::size_t i=0;i<2;++i){TerrainEvent lost;lost.kind=TerrainEventKind::lost;lost.role=i?t::Role::exporter:t::Role::plugin;lost.process=out[i].connection_process;lost.connection_generation=out[i].connection_generation;lost.received_qpc=event.received_qpc;lost.error=ERROR_BUFFER_OVERFLOW;events[size++]=lost;}SetEvent(ready.get());return false;}
        events[(head+size)%events.size()]=event;if(detail)++details;if(++size==1)SetEvent(ready.get());return true;
    }
    void lost(std::size_t i,ProcessIdentity process,std::uint32_t error,std::uint64_t connection_generation){TerrainEvent e;e.kind=TerrainEventKind::lost;e.role=i?t::Role::exporter:t::Role::plugin;e.process=process;e.connection_generation=connection_generation;e.received_qpc=qpc_now();e.error=error;push(e);}
    bool changed(std::size_t i,std::uint64_t generation){std::lock_guard<std::mutex> lock(mutex);return out[i].generation!=generation;}
    bool output(std::size_t i,t::Frame& f){std::lock_guard<std::mutex> lock(mutex);auto& o=out[i];if(o.size){f=o.controls[o.head];o.head=(o.head+1)%o.controls.size();--o.size;return true;}if(o.has_capture){f=o.capture;o.has_capture=false;return true;}if(o.has_latest){f=o.latest;o.has_latest=false;return true;}return false;}
    bool capture_enabled{};std::uint64_t capture_generation{1};std::int64_t capture_started_qpc{};
    bool capturing(std::uint64_t generation,std::int64_t started,std::int64_t connected){std::lock_guard<std::mutex> lock(mutex);return capture_enabled&&generation==capture_generation&&started>=capture_started_qpc&&started>=connected;}
    struct Peer {
        Impl* impl{};std::size_t index{};ProcessIdentity process{};bool admitted{};t::Hello hello{};std::uint32_t last_sequence{};std::uint64_t connection_generation{};std::int64_t connected_qpc{};
        static bool receive(void* context,const t::Header& h,const std::uint8_t* bytes,std::size_t count){auto& p=*static_cast<Peer*>(context);auto& self=*p.impl;
            if(!h.sequence||h.sequence<=p.last_sequence)return false;p.last_sequence=h.sequence;
            if(!p.admitted){if(h.kind!=t::Kind::hello||!t::decode(bytes,count,p.hello)||p.hello.role!=(p.index?t::Role::exporter:t::Role::plugin)||p.hello.pid!=p.process.pid||p.hello.process_creation_filetime!=p.process.creation_filetime||p.hello.qpc_frequency!=self.frequency)return false;p.admitted=true;}
            else if(h.kind!=t::Kind::ack&&!(p.index==0&&(h.kind==t::Kind::surface||h.kind==t::Kind::diagnostics||h.kind==t::Kind::probe_detail||h.kind==t::Kind::render_detail))&&!(p.index==1&&(h.kind==t::Kind::consumed||h.kind==t::Kind::support_detail||h.kind==t::Kind::constraint_detail||h.kind==t::Kind::frame_detail)))return false;
            if(h.kind==t::Kind::probe_detail){t::ProbeDetail d;if(!(p.hello.capabilities&t::probe_details)||!t::decode(bytes,count,d)||d.provider!=p.hello.instance)return false;if(!self.capturing(d.capture_generation,d.attempts[0].query_start_qpc,p.connected_qpc))return true;}
            if(h.kind==t::Kind::render_detail){t::RenderDetail d;if(!(p.hello.capabilities&t::render_details)||!t::decode(bytes,count,d)||d.provider!=p.hello.instance)return false;if(!self.capturing(d.capture_generation,d.sample_qpc,p.connected_qpc))return true;}
            if(h.kind==t::Kind::support_detail){t::SupportDetail d;if(!(p.hello.capabilities&t::support_details)||!t::decode(bytes,count,d)||d.producer!=p.hello.instance)return false;if(!self.capturing(d.capture_generation,d.step_start_qpc,p.connected_qpc))return true;}
            if(h.kind==t::Kind::constraint_detail){t::ConstraintDetail d;if(!(p.hello.capabilities&t::constraint_details)||!t::decode(bytes,count,d)||d.producer!=p.hello.instance)return false;if(!self.capturing(d.capture_generation,d.step_start_qpc,p.connected_qpc))return true;}
            if(h.kind==t::Kind::frame_detail){t::FrameDetail d;if(!(p.hello.capabilities&t::frame_details)||!t::decode(bytes,count,d)||d.producer!=p.hello.instance)return false;if(!(d.valid_mask&1u)||!self.capturing(d.capture_generation,d.stage_qpc[0],p.connected_qpc))return true;}
            TerrainEvent e;e.role=p.index?t::Role::exporter:t::Role::plugin;e.process=p.process;e.received_qpc=qpc_now();e.frame.size=t::header_size+count;
            e.connection_generation=p.connection_generation;
            e.frame.bytes[0]='P';e.frame.bytes[1]='X';e.frame.bytes[2]='T';e.frame.bytes[3]=static_cast<std::uint8_t>('0'+t::protocol_version);e.frame.bytes[4]=static_cast<std::uint8_t>(t::protocol_version);e.frame.bytes[5]=0;
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
            const auto connection_generation=++connections;
            {std::lock_guard<std::mutex> lock(mutex);if(out[i].generation!=generation)continue;out[i].connection_generation=connection_generation;out[i].connection_process=expected;}
            WinHandle process(remote?nullptr:OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,expected.pid));
            if(!remote&&(!process.valid()||service_process_creation(process.get())!=expected.creation_filetime)){lost(i,expected,ERROR_INVALID_HANDLE,connection_generation);WaitForSingleObject(cancel.get(),100);continue;}
            if(!remote&&name.empty())name=i?terrain_exporter_pipe_name(expected):terrain_plugin_pipe_name();
            WinHandle pipe;Operation connection,read,write;std::uint32_t failure{};
            if(i){
                const auto sid=service_user_sid();const auto acl=L"D:P(A;;GA;;;SY)(A;;GA;;;"+sid+L")";PSECURITY_DESCRIPTOR raw{};
                if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(),SDDL_REVISION_1,&raw,nullptr)){lost(i,expected,GetLastError(),connection_generation);WaitForSingleObject(cancel.get(),100);continue;}
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
            std::optional<psx::sync::WorkerIo> network; if(remote){const auto state=psx::sync::runtime()->status();if(!state.connected||state.peer.pid!=expected.pid||state.peer.process_birth!=expected.creation_filetime){WaitForSingleObject(cancel.get(),20);continue;}network.emplace(psx::sync::Family::terrain);}
            if(!remote&&!pipe.valid()){failure=GetLastError();if(!changed(i,generation))lost(i,expected,failure,connection_generation);WaitForSingleObject(cancel.get(),100);continue;}
            ULONG pid=remote?expected.pid:0;const bool identified=remote||(i?GetNamedPipeClientProcessId(pipe.get(),&pid):GetNamedPipeServerProcessId(pipe.get(),&pid))!=FALSE;
            if(!identified||pid!=expected.pid){lost(i,expected,ERROR_ACCESS_DENIED,connection_generation);WaitForSingleObject(cancel.get(),100);continue;}
            read.target=write.target=pipe.get();
            {std::lock_guard<std::mutex> lock(mutex);out[i].head=out[i].size=0;out[i].has_latest=out[i].has_capture=false;}
            Peer peer;peer.impl=this;peer.index=i;peer.process=expected;peer.connection_generation=connection_generation;peer.connected_qpc=qpc_now();t::Framer framer;t::Frame frame;std::size_t offset{};std::uint32_t seq{};bool subscribed{};std::int64_t write_started{},started=qpc_now();std::array<std::uint8_t,4096> input{};bool healthy=true;
            while(healthy&&!changed(i,generation)&&WaitForSingleObject(cancel.get(),0)!=WAIT_OBJECT_0&&(remote||WaitForSingleObject(process.get(),0)==WAIT_TIMEOUT)){
                if(!psx::sync::worker_poll(pipe.get()))break;const auto now=qpc_now();if((!peer.admitted&&now-started>static_cast<std::int64_t>(frequency*2))||(write.pending&&now-write_started>static_cast<std::int64_t>(frequency*2))){failure=ERROR_TIMEOUT;break;}
                DWORD count{};
                if(remote){
                    const auto receive=[&](DWORD& received){
                        if(read.pending){
                            if(WaitForSingleObject(read.event.get(),0)!=WAIT_OBJECT_0)return detail::TerrainReadResult::pending;
                            const auto ok=psx::sync::io_result(pipe.get(),&read.value,&received,FALSE);read.pending=false;
                            if(!ok||!received){failure=ok?ERROR_BROKEN_PIPE:GetLastError();return detail::TerrainReadResult::failed;}
                        }else{
                            read.prepare();
                            if(!psx::sync::io_read(pipe.get(),input.data(),static_cast<DWORD>(input.size()),&received,&read.value)){
                                const auto error=GetLastError();if(error==ERROR_IO_PENDING){read.pending=true;return detail::TerrainReadResult::pending;}
                                failure=error;return detail::TerrainReadResult::failed;
                            }
                        }
                        return detail::TerrainReadResult::complete;
                    };
                    if(!detail::drain_remote_terrain_ingress(receive,[&](DWORD received){
                        if(!received||!framer.feed(input.data(),received,Peer::receive,&peer)){failure=ERROR_INVALID_DATA;return false;}return true;
                    }))break;
                }else{
                    if(read.pending&&WaitForSingleObject(read.event.get(),0)==WAIT_OBJECT_0){const auto ok=psx::sync::io_result(pipe.get(),&read.value,&count,FALSE);read.pending=false;if(!ok||!count){failure=ok?ERROR_BROKEN_PIPE:GetLastError();break;}if(!framer.feed(input.data(),count,Peer::receive,&peer)){failure=ERROR_INVALID_DATA;break;}}
                    if(!read.pending){read.prepare();const auto ok=psx::sync::io_read(pipe.get(),input.data(),static_cast<DWORD>(input.size()),&count,&read.value);if(ok){if(!count||!framer.feed(input.data(),count,Peer::receive,&peer)){failure=ERROR_INVALID_DATA;break;}}else if(GetLastError()==ERROR_IO_PENDING)read.pending=true;else {failure=GetLastError();break;}}
                }
                if(write.pending&&WaitForSingleObject(write.event.get(),0)==WAIT_OBJECT_0){const auto ok=psx::sync::io_result(pipe.get(),&write.value,&count,FALSE);write.pending=false;if(!ok||!count){failure=ok?ERROR_BROKEN_PIPE:GetLastError();break;}offset+=count;}
                if(!write.pending&&offset==frame.size){frame={};offset=0;
                    if(peer.admitted&&!subscribed){t::Subscribe s;s.instance=peer.hello.instance;s.owner=owner;s.pid=GetCurrentProcessId();s.qpc_frequency=frequency;healthy=t::encode(s,++seq,frame);subscribed=true;}
                    else if(subscribed&&output(i,frame))sequence(frame,++seq);
                }
                if(healthy&&!write.pending&&offset<frame.size){write.prepare();write_started=qpc_now();const auto ok=psx::sync::io_write(pipe.get(),frame.bytes.data()+offset,static_cast<DWORD>(frame.size-offset),&count,&write.value);if(ok){if(!count){failure=ERROR_BROKEN_PIPE;break;}offset+=count;}else if(GetLastError()==ERROR_IO_PENDING)write.pending=true;else {failure=GetLastError();break;}}
                if(!read.pending)ResetEvent(read.event.get());if(!write.pending)ResetEvent(write.event.get());
                HANDLE waits[]={cancel.get(),read.event.get(),write.event.get(),out[i].wake.get()};const auto wait=WaitForMultipleObjects(4,waits,FALSE,!write.pending&&offset<frame.size?0:(remote?2:20));if(wait==WAIT_FAILED){failure=GetLastError();break;}
            }
            read.cancel(pipe.get());write.cancel(pipe.get());if(i)DisconnectNamedPipe(pipe.get());lost(i,expected,failure?failure:ERROR_OPERATION_ABORTED,connection_generation);WaitForSingleObject(cancel.get(),100);
        }
    }
};
TerrainService::TerrainService():impl_(std::make_unique<Impl>()){}
TerrainService::~TerrainService(){stop();}
void TerrainService::start(std::uint64_t frequency,std::uint64_t owner){if(impl_->threads[0].joinable())return;impl_->frequency=frequency;impl_->owner=owner;ResetEvent(impl_->cancel.get());for(std::size_t i=0;i<2;++i)impl_->threads[i]=std::thread([p=impl_.get(),i]{try{p->run(i);}catch(...){ProcessIdentity process;std::uint64_t scope{};{std::lock_guard<std::mutex> lock(p->mutex);process=p->out[i].connection_process;scope=p->out[i].connection_generation;}p->lost(i,process,ERROR_UNHANDLED_EXCEPTION,scope);}});}
void TerrainService::associate(ProcessIdentity process){std::lock_guard<std::mutex> lock(impl_->mutex);auto& o=impl_->out[1];if(o.process==process)return;o.process=process;++o.generation;o.head=o.size=0;o.has_latest=o.has_capture=false;SetEvent(o.wake.get());}
void TerrainService::plugin(ProcessIdentity process){std::lock_guard<std::mutex> lock(impl_->mutex);auto& o=impl_->out[0];if(o.process==process)return;o.process=process;++o.generation;o.head=o.size=0;o.has_latest=o.has_capture=false;SetEvent(o.wake.get());}
void TerrainService::capture(bool enabled,std::uint64_t generation,std::int64_t now) noexcept {std::lock_guard<std::mutex> lock(impl_->mutex);if(impl_->capture_enabled==enabled&&impl_->capture_generation==generation)return;impl_->capture_enabled=enabled;impl_->capture_generation=generation;impl_->capture_started_qpc=enabled?now:0;}
void TerrainService::stop() noexcept {SetEvent(impl_->cancel.get());for(auto& thread:impl_->threads)if(thread.joinable())thread.join();}
bool TerrainService::pop(TerrainEvent& e){std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->size)return false;e=impl_->events[impl_->head];impl_->head=(impl_->head+1)%impl_->events.size();t::Header h;if(e.kind==TerrainEventKind::frame&&t::read_header(e.frame.bytes.data(),t::header_size,h)&&(h.kind==t::Kind::probe_detail||h.kind==t::Kind::support_detail||h.kind==t::Kind::render_detail||h.kind==t::Kind::constraint_detail||h.kind==t::Kind::frame_detail))--impl_->details;if(!--impl_->size)ResetEvent(impl_->ready.get());return true;}
std::uintptr_t TerrainService::ready_event() const noexcept{return reinterpret_cast<std::uintptr_t>(impl_->ready.get());}
std::uint64_t TerrainService::dropped() const noexcept{return impl_->dropped.load();}
bool TerrainService::send(t::Role role,const t::Frame& f){t::Header h;if((role!=t::Role::plugin&&role!=t::Role::exporter)||f.size<t::header_size||!t::read_header(f.bytes.data(),t::header_size,h)||f.size!=t::header_size+h.payload_size)return false;
    const std::size_t i=role==t::Role::exporter?1:0;std::lock_guard<std::mutex> lock(impl_->mutex);auto& o=impl_->out[i];if(!o.process.pid)return false;
    if(h.kind==t::Kind::control){t::Control c;if(!t::decode(f.bytes.data()+t::header_size,h.payload_size,c))return false;if(c.action==t::Action::release)o.has_latest=false;if(o.size==o.controls.size()){++impl_->dropped;++o.generation;o.head=o.size=0;o.has_latest=o.has_capture=false;SetEvent(o.wake.get());return false;}o.controls[(o.head+o.size)%o.controls.size()]=f;++o.size;}
    else if(h.kind==t::Kind::capture){t::Capture c;if(!t::decode(f.bytes.data()+t::header_size,h.payload_size,c))return false;o.capture=f;o.has_capture=true;}
    else if((i==0&&h.kind==t::Kind::query)||(i==1&&h.kind==t::Kind::surface)){o.latest=f;o.has_latest=true;}
    else return false;SetEvent(o.wake.get());return true;
}
void TerrainService::fault(t::Role role,std::uint32_t error) noexcept {const std::size_t i=role==t::Role::exporter?1:0;ProcessIdentity p;std::uint64_t scope{};{std::lock_guard<std::mutex> lock(impl_->mutex);auto& o=impl_->out[i];p=o.connection_process;scope=o.connection_generation;++o.generation;o.head=o.size=0;o.has_latest=o.has_capture=false;SetEvent(o.wake.get());}impl_->lost(i,p,error,scope);}
void TerrainService::fixture_names(std::wstring plugin,std::wstring exporter){std::lock_guard<std::mutex> lock(impl_->mutex);impl_->out[0].test_name=std::move(plugin);impl_->out[1].test_name=std::move(exporter);}
bool TerrainService::fixture_event(const TerrainEvent& e){return impl_->push(e);}
bool TerrainService::fixture_output(t::Role role,t::Frame& f){return impl_->output(role==t::Role::exporter?1:0,f);}
}
